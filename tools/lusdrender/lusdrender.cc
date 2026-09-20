// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment, Inc.
//
// lusdrender: CPU preview raytrace renderer for USD scenes.
//
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>  // ProcessRSS()/AvailableSystemMemory() use std::ifstream
                    // unconditionally (was only included under LIGHTUSD_WITH_QJS)
#include <iostream>
#include <mutex>
#include <new>
#if !defined(_MSC_VER)
#include <unistd.h>  // POSIX (present under MinGW; absent with MSVC)
#endif
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#if defined(__GLIBC__)
#include <malloc.h>  // mallopt (peak-RSS tuning, glibc)
#endif
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "asset-resolution.hh"
#include "image-loader.hh"
#include "image-writer.hh"
#include "io-util.hh"
#include "mmap-array-ref.hh"
#include "lightusd.hh"
#include "tsd/tinysubdiv.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "usdVol.hh"  // OpenVDB (.vdb) loader for the `next` volume path
#include "usdGeom.hh"
#include "value-types.hh"
#include "xform.hh"

// Experimental `next` lazy loader: fast, low-memory USDC parse used as the
// default backend for the RT preview path. tydra_next provides bit-exact
// world transforms (see src/tydra/next/scene-access.cc).
#include "next/layer/prim-spec.hh"
#include "next/pcp/prim-index.hh"
#include "next/prim/path.hh"
#include "next/reader/usdz-reader.hh"
#include "next/schema/geom-mesh.hh"
#include "next/stage/stage.hh"
#include "next/lightusd-next.hh"
#include "next/types/value.hh"
#include "tydra/next/scene-access.hh"



extern "C" {
#include "lightrt_c_tri.h"
}

#ifdef LIGHTUSD_WITH_QJS
#include <fstream>
#include <sstream>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "tydra/js-script.hh"
extern "C" {
#include "external/quickjs-ng/quickjs.h"
}
#endif

#include "lusdr_math.hh"
#include "lusdr_types.hh"
#include "lusdr_context.hh"
#include "lusdr_rt_lod.hh"
#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
#include "lusdr_gpu_common.hh"  // GpuInstancedScene, RunVulkanLightRTInstanced
#endif


// The Vulkan backend and main() below live in the global namespace; pull in the
// lusdr names they use (Vec3, Options, RTPreviewStats, qjs::*, ...).
using namespace lusdr;

namespace {

uint8_t FloatToByte(float v) {
  if (!std::isfinite(v)) return 0;
  v = std::max(0.0f, std::min(1.0f, v));
  return static_cast<uint8_t>(std::round(v * 255.0f));
}

lightusd::Image MakeBlankImage(const Options &opt, int height) {
  lightusd::Image img;
  img.width = std::max(1, opt.width);
  img.height = std::max(1, height);
  img.channels = 4;
  img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data.resize(size_t(img.width) * size_t(img.height) * 4);
  const uint8_t r = FloatToByte(opt.bg.x);
  const uint8_t g = FloatToByte(opt.bg.y);
  const uint8_t b = FloatToByte(opt.bg.z);
  for (size_t i = 0; i + 3 < img.data.size(); i += 4) {
    img.data[i + 0] = r;
    img.data[i + 1] = g;
    img.data[i + 2] = b;
    img.data[i + 3] = 255;
  }
  return img;
}

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
// GPU triangle backends share the CPU LightRT curve tessellator. This must not
// be Vulkan-only: HIP/ROCm and D3D11 use the same bounded triangle fallback for
// round curves when no analytic curve primitive is exposed by their API.
size_t GpuTriangleChunkLimit() {
  size_t limit = 262144;
  if (const char *env = std::getenv("LUSDR_GPU_TRIANGLE_CHUNK")) {
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end != env && parsed > 0) limit = static_cast<size_t>(parsed);
  }
  return limit;
}

void AppendGpuPointSphere(const Vec3 &center, float radius,
                          RTPreviewStats::MeshGeometry *geo) {
  if (!geo) return;
  constexpr float kPi = 3.14159265358979323846f;
  constexpr int kStacks = 4;
  constexpr int kSlices = 8;
  geo->positions.reserve(geo->positions.size() + kStacks * kSlices * 18);
  geo->normals.reserve(geo->normals.size() + kStacks * kSlices * 18);
  geo->uvs.reserve(geo->uvs.size() + kStacks * kSlices * 12);
  geo->indices.reserve(geo->indices.size() + kStacks * kSlices * 6);
  auto add = [&](const Vec3 &p, const Vec3 &n) {
    geo->positions.insert(geo->positions.end(), {p.x, p.y, p.z});
    geo->normals.insert(geo->normals.end(), {n.x, n.y, n.z});
    geo->uvs.insert(geo->uvs.end(), {0.0f, 0.0f});
  };
  for (int y = 0; y < kStacks; ++y) {
    const float v0 = float(y) / float(kStacks);
    const float v1 = float(y + 1) / float(kStacks);
    const float p0 = (v0 - 0.5f) * kPi;
    const float p1 = (v1 - 0.5f) * kPi;
    for (int x = 0; x < kSlices; ++x) {
      const float a0 = float(x) * 2.0f * kPi / float(kSlices);
      const float a1 = float(x + 1) * 2.0f * kPi / float(kSlices);
      const Vec3 n00{std::cos(p0) * std::cos(a0), std::sin(p0),
                     std::cos(p0) * std::sin(a0)};
      const Vec3 n10{std::cos(p0) * std::cos(a1), std::sin(p0),
                     std::cos(p0) * std::sin(a1)};
      const Vec3 n01{std::cos(p1) * std::cos(a0), std::sin(p1),
                     std::cos(p1) * std::sin(a0)};
      const Vec3 n11{std::cos(p1) * std::cos(a1), std::sin(p1),
                     std::cos(p1) * std::sin(a1)};
      const uint32_t base = uint32_t(geo->positions.size() / 3);
      add(Add(center, Mul(n00, radius)), n00);
      add(Add(center, Mul(n10, radius)), n10);
      add(Add(center, Mul(n11, radius)), n11);
      add(Add(center, Mul(n01, radius)), n01);
      geo->indices.insert(geo->indices.end(), {base, base + 1, base + 2,
                                                base, base + 2, base + 3});
    }
  }
}

void AppendGpuEllipseToGeometry(const Vec3 &center, const Vec3 &normal,
                                float radius_x, float radius_y,
                                const Vec3 *major_axis, int segments,
                                RTPreviewStats::MeshGeometry *geo,
                                bool emit_attributes = true) {
  constexpr float kPi = 3.14159265358979323846f;
  Vec3 n = Normalize(normal);
  if (Length(n) < 1.0e-6f) n = Vec3{0.0f, 1.0f, 0.0f};
  Vec3 u;
  if (major_axis) {
    u = Sub(*major_axis, Mul(n, Dot(*major_axis, n)));
    u = Normalize(u);
    if (Length(u) < 1.0e-6f) {
      Vec3 ref = std::fabs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                       : Vec3{1.0f, 0.0f, 0.0f};
      u = Normalize(Cross(ref, n));
    }
  } else {
    Vec3 ref = std::fabs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                     : Vec3{1.0f, 0.0f, 0.0f};
    u = Normalize(Cross(ref, n));
  }
  Vec3 v = Normalize(Cross(n, u));
  for (int i = 0; i < segments; ++i) {
    const float a0 = float(i) * 2.0f * kPi / float(segments);
    const float a1 = float(i + 1) * 2.0f * kPi / float(segments);
    const Vec3 p0 = center;
    const auto point_at = [&](float a) {
      return Add(center, Add(Mul(u, std::cos(a) * radius_x),
                              Mul(v, std::sin(a) * radius_y)));
    };
    const Vec3 p1 = point_at(a0);
    const Vec3 p2 = point_at(a1);
    const uint32_t base = uint32_t(geo->positions.size() / 3);
    for (const Vec3 &p : {p0, p1, p2}) {
      geo->positions.insert(geo->positions.end(), {p.x, p.y, p.z});
      if (emit_attributes) {
        geo->normals.insert(geo->normals.end(), {n.x, n.y, n.z});
        geo->uvs.insert(geo->uvs.end(), {0.0f, 0.0f});
      }
    }
    geo->indices.insert(geo->indices.end(), {base, base + 1, base + 2});
    const uint32_t back = uint32_t(geo->positions.size() / 3);
    for (const Vec3 &p : {p0, p2, p1}) {
      geo->positions.insert(geo->positions.end(), {p.x, p.y, p.z});
      if (emit_attributes) {
        geo->normals.insert(geo->normals.end(), {-n.x, -n.y, -n.z});
        geo->uvs.insert(geo->uvs.end(), {0.0f, 0.0f});
      }
    }
    geo->indices.insert(geo->indices.end(), {back, back + 1, back + 2});
  }
}

void AppendGpuPointDisc(const Vec3 &center, const Vec3 &normal, float radius,
                        RTPreviewStats::MeshGeometry *geo) {
  AppendGpuEllipseToGeometry(center, normal, radius, radius, nullptr, 16, geo);
}

void CollectGpuPointsRec(
    const lightusd::next::Stage &stage, const lightusd::next::UsdPrim &prim,
    const matrix4d &parent_world,
    double time, std::vector<Vec3> *base_colors,
    std::vector<RTPreviewStats::MeshGeometry> *geos, size_t depth) {
  constexpr size_t kMaxGpuPointTraversalDepth = 256;
  if (depth >= kMaxGpuPointTraversalDepth) {
    std::cerr << "WARN: GPU point fallback traversal exceeded "
              << kMaxGpuPointTraversalDepth << " levels at "
              << prim.GetPath().str() << "; subtree skipped.\n";
    return;
  }
  if (!prim.IsActive()) return;
  double dmat[16];
  lightusd::tydra::next::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const matrix4d world = local * parent_world;
  if (prim.GetTypeName() == "ParticleField3DGaussianSplat") {
    lightusd::next::ParticleFieldData field;
    std::string field_warning;
    if (!lightusd::next::GetParticleFieldData(
            stage, prim, &field, time, &field_warning)) {
      std::cerr << "Gaussian fallback: invalid ParticleField data at "
                << prim.GetPath().str() << ".\n";
      return;
    }
    if (!field_warning.empty()) std::cerr << field_warning << "\n";
    lightusd::tydra::next::ValueArrayRead<float> points;
    lightusd::tydra::next::ValueArrayRead<float> scales;
    lightusd::tydra::next::ValueArrayRead<float> orientations;
    lightusd::tydra::next::ValueArrayRead<float> opacities;
    lightusd::tydra::next::ValueArrayRead<float> sh;
    const bool have_points = !field.positions_property.empty() &&
        ReadFloatArrayViewLazy(prim, field.positions_property.c_str(), time,
                               &points);
    const bool have_scales = !field.scales_property.empty() &&
        ReadFloatArrayViewLazy(prim, field.scales_property.c_str(), time,
                               &scales);
    const bool have_orientations = !field.orientations_property.empty() &&
        ReadFloatArrayViewLazy(prim, field.orientations_property.c_str(), time,
                               &orientations);
    const bool have_opacities = !field.opacities_property.empty() &&
        ReadFloatArrayViewLazy(prim, field.opacities_property.c_str(), time,
                               &opacities);
    const bool allow_sh = AllowGaussianSHDecode(prim);
    const bool have_sh = allow_sh &&
        !field.spherical_harmonics_property.empty() && ReadFloatArrayViewLazy(
        prim, field.spherical_harmonics_property.c_str(), time, &sh);
    if (!allow_sh)
      std::cerr << "Gaussian fallback: skipping oversized compressed SH payload at "
                << prim.GetPath().str() << "; using fallback color.\n";
    const size_t count = have_points ? points.size() / 3 : 0;
    if (!have_points || !have_scales || count == 0 || scales.size() < 3) {
      for (const lightusd::next::UsdPrim &child : prim.GetChildren())
        CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                            depth + 1);
      return;
    }
    size_t limit = count;
    if (const char *limit_text = std::getenv("LUSDR_GAUSSIAN_MAX")) {
      char *end = nullptr;
      const unsigned long long parsed = std::strtoull(limit_text, &end, 10);
      if (end != limit_text && parsed > 0)
        limit = std::min(count, static_cast<size_t>(parsed));
    }
    const size_t sh_stride = (have_sh && count) ? (sh.size() / 3) / count : 0;
    std::array<RTPreviewStats::MeshGeometry, 64> batches;
    const size_t batch_limit = GpuTriangleChunkLimit();
    const size_t gaussian_geo_first = geos->size();
    size_t gaussian_triangles = 0;
    size_t pending_triangles = 0;
    auto flush_splat_batches = [&]() {
      for (size_t bi = 0; bi < batches.size(); ++bi) {
        if (batches[bi].indices.empty()) continue;
        const int br = int((bi >> 4) & 3), bg = int((bi >> 2) & 3),
                  bb = int(bi & 3);
        base_colors->push_back(Vec3{(float(br) + 0.5f) * 0.25f,
                                    (float(bg) + 0.5f) * 0.25f,
                                    (float(bb) + 0.5f) * 0.25f});
        geos->push_back(std::move(batches[bi]));
        batches[bi] = RTPreviewStats::MeshGeometry{};
      }
      pending_triangles = 0;
    };
    const float wx = Length(TransformVector(world, Vec3{1.0f, 0.0f, 0.0f}));
    const float wy = Length(TransformVector(world, Vec3{0.0f, 1.0f, 0.0f}));
    for (size_t i = 0; i < limit; ++i) {
      const size_t scale_index = scales.size() == 3 ? 0 : i * 3;
      if (scale_index + 2 >= scales.size()) break;
      float opacity = 1.0f;
      if (have_opacities && !opacities.empty()) {
        opacity = opacities.size() == 1
                      ? opacities[0]
                      : (i < opacities.size() ? opacities[i] : 1.0f);
      }
      if (!std::isfinite(opacity) || opacity < 0.01f)
        continue;
      const Vec3 p = TransformPoint(world, Vec3{points[i * 3 + 0],
                                                  points[i * 3 + 1],
                                                  points[i * 3 + 2]});
      Vec3 normal{0.0f, 1.0f, 0.0f};
      Vec3 major_axis{1.0f, 0.0f, 0.0f};
      const size_t orientation_index = orientations.size() == 4 ? 0 : i * 4;
      if (have_orientations && orientation_index + 3 < orientations.size()) {
        lightusd::value::quatf q;
        q.real = orientations[orientation_index + 0];
        q.imag[0] = orientations[orientation_index + 1];
        q.imag[1] = orientations[orientation_index + 2];
        q.imag[2] = orientations[orientation_index + 3];
        const lightusd::value::matrix3d r = lightusd::to_matrix3x3(q);
        const lightusd::value::matrix4d r4 =
            lightusd::to_matrix(r, lightusd::value::double3{0.0, 0.0, 0.0});
        normal = TransformVector(world, TransformVector(
            r4, Vec3{0.0f, 0.0f, 1.0f}));
        major_axis = TransformVector(world, TransformVector(
            r4, Vec3{1.0f, 0.0f, 0.0f}));
      }
      const float rx = std::max(
          1.0e-6f, 2.0f * std::fabs(scales[scale_index + 0]) * wx);
      const float ry = std::max(
          1.0e-6f, 2.0f * std::fabs(scales[scale_index + 1]) * wy);
      Vec3 color{0.72f, 0.72f, 0.72f};
      if (have_sh && sh_stride > 0 && i * sh_stride * 3 + 2 < sh.size()) {
        const size_t j = i * sh_stride * 3;
        color = Vec3{0.5f + 0.2820948f * sh[j + 0],
                     0.5f + 0.2820948f * sh[j + 1],
                     0.5f + 0.2820948f * sh[j + 2]};
        color.x = std::max(0.0f, std::min(1.0f, color.x));
        color.y = std::max(0.0f, std::min(1.0f, color.y));
        color.z = std::max(0.0f, std::min(1.0f, color.z));
      }
      color = Mul(color, std::max(0.0f, std::min(1.0f, opacity)));
      const int cr = std::min(3, std::max(0, int(color.x * 4.0f)));
      const int cg = std::min(3, std::max(0, int(color.y * 4.0f)));
      const int cb = std::min(3, std::max(0, int(color.z * 4.0f)));
      const int bucket = (cr << 4) | (cg << 2) | cb;
      // Four segments per side keeps each splat inexpensive; flush the bucket
      // set when the GLOBAL pending budget is full.  A per-bucket cap would
      // allow 64 buckets to retain 64 upload batches simultaneously.
      constexpr size_t kSplatTriangles = 8;
      if (pending_triangles != 0 &&
          pending_triangles + kSplatTriangles > batch_limit)
        flush_splat_batches();
      // A limit below one splat still has to admit the indivisible carrier.
      // The downstream chunker will split the resulting mesh if necessary.
      if (batch_limit == 0) {
        std::cerr << "Invalid zero GPU triangle chunk limit.\n";
        break;
      }
      AppendGpuEllipseToGeometry(p, normal, rx, ry, &major_axis, 4,
                                 &batches[size_t(bucket)],
                                 /*emit_attributes=*/false);
      pending_triangles += kSplatTriangles;
      gaussian_triangles += kSplatTriangles;
    }
    flush_splat_batches();
    std::cerr << "[gpu] gaussian splats: " << limit;
    if (limit != count) std::cerr << " / " << count << " (budgeted)";
    std::cerr << ", tessellated triangles: " << gaussian_triangles
              << ", GPU chunks: " << (geos->size() - gaussian_geo_first)
              << ", limit: " << batch_limit << " tris\n";
  } else if (prim.GetTypeName() == "Points") {
    lightusd::tydra::next::ValueArrayRead<float> points;
    lightusd::tydra::next::ValueArrayRead<float> widths;
    lightusd::tydra::next::ValueArrayRead<float> normals;
    const bool have_points = ReadFloatArrayViewLazy(prim, "points", time, &points);
    ReadFloatArrayViewLazy(prim, "widths", time, &widths);
    ReadFloatArrayViewLazy(prim, "normals", time, &normals);
    const size_t batchLimit = GpuTriangleChunkLimit();
    RTPreviewStats::MeshGeometry batch;
    size_t emitted = 0;
    auto flush = [&]() {
      if (batch.indices.empty()) return;
      base_colors->push_back(Vec3{0.90f, 0.72f, 0.26f});
      geos->push_back(std::move(batch));
      batch = RTPreviewStats::MeshGeometry{};
      emitted = 0;
    };
    if (!have_points) {
      for (const lightusd::next::UsdPrim &child : prim.GetChildren())
        CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                            depth + 1);
      return;
    }
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
      const Vec3 p = TransformPoint(world, Vec3{points[i], points[i + 1],
                                                points[i + 2]});
      const size_t pointIndex = i / 3;
      const float authoredWidth =
          widths.empty()
              ? 0.05f
              : (widths.size() == 1
                     ? widths[0]
                     : (pointIndex < widths.size() ? widths[pointIndex]
                                                    : widths[widths.size() - 1]));
      const float w = std::isfinite(authoredWidth)
                          ? std::max(0.0f, authoredWidth)
                          : 0.0f;
      const float sx = Length(TransformVector(world, Vec3{1.0f, 0.0f, 0.0f}));
      const float sy = Length(TransformVector(world, Vec3{0.0f, 1.0f, 0.0f}));
      const float sz = Length(TransformVector(world, Vec3{0.0f, 0.0f, 1.0f}));
      const float r = std::max(1.0e-5f, 0.5f * w * std::max(sx, std::max(sy, sz)));
      const size_t pointTriangles = normals.size() >= i + 3 ? 32 : 64;
      if (emitted != 0 && emitted + pointTriangles > batchLimit) flush();
      if (normals.size() >= i + 3) {
        AppendGpuPointDisc(p, TransformVector(world,
                                              Vec3{normals[i], normals[i + 1],
                                                   normals[i + 2]}),
                           r, &batch);
      } else {
        AppendGpuPointSphere(p, r, &batch);
      }
      emitted += pointTriangles;
    }
    flush();
  }
  for (const lightusd::next::UsdPrim &child : prim.GetChildren())
    CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                        depth + 1);
}

// LightRT's GPU triangle APIs currently accept triangle BVHs, while its
// CPU/CUDA curve intersectors use dedicated round-linear primitives. Keep all
// GPU paths on their backend by converting round-linear curves to bounded tube
// triangle streams at the upload boundary.
bool AppendGpuRoundCurves(
    DirectScene &direct, std::vector<Vec3> *base_colors,
    std::vector<RTPreviewStats::MeshGeometry> *geos) {
  if (!base_colors || !geos) return false;
  constexpr uint32_t kCurveSides = 6;
  const size_t curveChunk = GpuTriangleChunkLimit();
  auto append = [&](lrt_tri_scene *curve) -> bool {
    if (!curve) return true;
    const size_t total = lrt_tri_curve_tessellate_bound(curve, kCurveSides);
    for (size_t first = 0; first < total; first += curveChunk) {
      RTPreviewStats::MeshGeometry geo;
      geo.positions.resize(curveChunk * 9);
      geo.indices.resize(curveChunk * 3);
      size_t written = 0;
      const lrt_result result = lrt_tri_curve_tessellate_range(
          curve, kCurveSides, first, geo.positions.data(), nullptr,
          curveChunk, &written);
      if (result != LRT_RESULT_OK) {
        std::cerr << "WARN: curve tessellation failed at triangle " << first
                  << " (err=" << int(result) << ").\n";
        return false;
      }
      if (written == 0) continue;
      geo.positions.resize(written * 9);
      geo.indices.resize(written * 3);
      for (size_t i = 0; i < written * 3; ++i) geo.indices[i] = uint32_t(i);
      geos->push_back(std::move(geo));
      base_colors->push_back(kCurveColor);
    }
    return true;
  };
  if (!append(direct.round_curves.get())) return false;
  direct.round_curves.reset();
  for (CurveSceneChunk &chunk : direct.round_curve_chunks) {
    if (!append(chunk.scene.get())) return false;
    // The GPU fallback has consumed this native BVH; retaining it alongside
    // the tessellated source geometry needlessly doubles the curve peak.
    chunk.scene.reset();
    chunk.info.clear();
  }
  direct.round_curve_chunks.clear();
  return true;
}
#endif

bool WriteBlankImage(const Options &opt, int height) {
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(
      opt.output, MakeBlankImage(opt, height), wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return false;
  }
  return true;
}

}  // namespace

static const char *LargeSceneProfileName(Options::LargeSceneProfile p) {
  switch (p) {
    case Options::LargeSceneProfile::Off: return "off";
    case Options::LargeSceneProfile::Auto: return "auto";
    case Options::LargeSceneProfile::Caldera: return "caldera";
    case Options::LargeSceneProfile::Island: return "island";
    case Options::LargeSceneProfile::ALab: return "alab";
  }
  return "off";
}

static std::string LowerAscii(std::string s) {
  for (char &c : s) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

static Options::LargeSceneProfile DetectLargeSceneProfile(const std::string &path) {
  const std::string p = LowerAscii(path);
  if (p.find("caldera") != std::string::npos) return Options::LargeSceneProfile::Caldera;
  if (p.find("island") != std::string::npos ||
      p.find("moana") != std::string::npos) return Options::LargeSceneProfile::Island;
  if (p.find("alab") != std::string::npos ||
      p.find("animal_logic") != std::string::npos ||
      p.find("animal-logic") != std::string::npos) {
    return Options::LargeSceneProfile::ALab;
  }
  return Options::LargeSceneProfile::Off;
}

static void ApplyLargeSceneProfile(Options *opt) {
  if (!opt) return;
  Options::LargeSceneProfile p = opt->large_scene_profile;
  if (p == Options::LargeSceneProfile::Auto) {
    p = DetectLargeSceneProfile(opt->input);
  }
  if (p == Options::LargeSceneProfile::Off) return;

  const uint64_t host_available = MemBudget::AvailableSystemMemory();
  const uint64_t host_capacity =
      host_available
          ? std::min<uint64_t>(host_available,
                               lightusd::tydra::next::GiB(32))
          : lightusd::tydra::next::GiB(32);
  const lightusd::tydra::next::ResourceBudget budget =
      lightusd::tydra::next::ComputeResourceBudget(
          host_capacity, QueryDeviceLocalVRAMBytes());
  const double host_gib = double(budget.host_limit) /
                          double(lightusd::tydra::next::GiB(1));
  const double vram_gib = double(budget.vram_limit) /
                          double(lightusd::tydra::next::GiB(1));

  if (!opt->backend_explicit) {
    opt->vulkan = true;
  }
  if (!opt->rt_lod_explicit) opt->rt_lod = true;
  if (!opt->rt_lod_full_px_explicit) opt->rt_lod_full_px = 64.0f;
  if (!opt->rt_lod_cull_px_explicit) opt->rt_lod_cull_px = 2.0f;

  // Bound texture residency the way geometry already is. -texMaxSize /
  // -texBudgetMb still win; these only fill in the profile defaults.
  {
    const lightusd::tydra::next::TextureBudget texture_budget =
        lightusd::tydra::next::DeriveTextureBudget(budget);
    if (!opt->texture_max_size_explicit && texture_budget.max_edge > 0) {
      opt->texture_max_size = int(texture_budget.max_edge);
    }
    if (!opt->texture_budget_explicit && texture_budget.budget_bytes > 0) {
      opt->texture_budget_mb =
          int(texture_budget.budget_bytes / (1024ull * 1024ull));
    }
  }

  if (p == Options::LargeSceneProfile::Caldera) {
    if (!opt->camera_explicit && opt->camera.empty()) {
      opt->camera = "phospate_mine_overview";
    }
    if (!opt->lod_stream_explicit) opt->lod_stream = true;
    if (!opt->max_mem_explicit) opt->max_mem_gib = host_gib;
    if (!opt->max_vram_explicit) opt->max_vram_gib = vram_gib;
  } else if (p == Options::LargeSceneProfile::Island) {
    if (!opt->max_mem_explicit) opt->max_mem_gib = host_gib;
    if (!opt->max_vram_explicit) opt->max_vram_gib = vram_gib;
  } else if (p == Options::LargeSceneProfile::ALab) {
    if (!opt->max_mem_explicit) opt->max_mem_gib = host_gib;
    if (!opt->max_vram_explicit) opt->max_vram_gib = vram_gib;
  }

  std::cerr << "largeSceneProfile " << LargeSceneProfileName(p)
            << " resolved: backend="
            << (opt->vulkan_instanced ? "vkr+vkInstanced"
                : opt->vulkan_rt ? "vkr"
                : opt->vulkan ? "vk"
                : opt->hip ? "hip"
                : opt->use_d3d ? "d3d"
                : opt->rt_preview ? "rtPreview" : "default")
            << " rtLod=" << (opt->rt_lod ? "on" : "off")
            << " fullPx=" << opt->rt_lod_full_px
            << " cullPx=" << opt->rt_lod_cull_px
            << " lodStream=" << (opt->lod_stream ? "on" : "off")
            << " maxMem=" << opt->max_mem_gib
            << " maxVram=" << opt->max_vram_gib;
  if (!opt->camera.empty()) std::cerr << " camera=" << opt->camera;
  std::cerr << "\n";
}

// ---------------------------------------------------------------------------
// LightRT Vulkan backend: uses the LightRT C API (lightrt_c_vk.h) for GPU
// BVH traversal. Builds the scene with the existing CPU builder, uploads to
// GPU, traces camera rays, then shades hits on the CPU.
// ---------------------------------------------------------------------------

#if defined(HAVE_VULKAN)
// Resolve the GPU render camera exactly like the flat -vk/-vkr path (named
// camera, else USD record camera for -autoframe, else auto-fit). *out_height
// starts at opt.height (<=0 = derive).
static CameraFrame ResolveGpuCameraInst(const lightusd::next::Stage &stage,
                                        const Options &opt, const Bounds &bounds,
                                        lightusd::Axis usdUp, int *out_height) {
  const int cam_width = opt.width > 0 ? opt.width : 960;
  CameraFrame camera;
  Options auto_opt = opt;
  auto_opt.camera.clear();
  auto_opt.width = cam_width;
  if (!opt.camera.empty()) {
    float cam_aspect = 16.0f / 9.0f;
    if (FindNextCameraFrame(stage, opt.camera, opt.timecode, &camera, &cam_aspect)) {
      if (*out_height <= 0) {
        // Clamp: a hostile aperture ratio must not overflow the int conversion
        // (UB) or demand a multi-GB framebuffer.
        double dh = double(cam_width) / double(cam_aspect);
        if (!std::isfinite(dh)) dh = 540.0;
        *out_height = std::max(1, int(std::lround(std::min(32768.0, dh))));
      }
    } else {
      std::cerr << "WARN: camera not found: " << opt.camera
                << ". Using auto-fit.\n";
      if (*out_height <= 0) *out_height = 540;
      camera = MakeCameraFrame({}, auto_opt, bounds, *out_height, usdUp);
    }
  } else if (opt.autoframe) {
    camera = MakeUsdRecordCamera(bounds, usdUp, cam_width, out_height);
  } else {
    if (*out_height <= 0) *out_height = 540;
    camera = MakeCameraFrame({}, auto_opt, bounds, *out_height, usdUp);
  }
  return camera;
}

// Object-space normal matrix (row-major 3x3) = cofactor of o2w's upper 3x3 =
// det * inverse-transpose. The det scale is dropped (the shaded normal is
// renormalized), so this transforms an object-space normal to world correctly
// even under non-uniform scale.
static void NormalMatrixFromO2W(const float o2w[12], float n2w[9]) {
  const float a = o2w[0], b = o2w[1], c = o2w[2];
  const float d = o2w[4], e = o2w[5], f = o2w[6];
  const float g = o2w[8], h = o2w[9], i = o2w[10];
  n2w[0] = (e * i - f * h); n2w[1] = -(d * i - f * g); n2w[2] = (d * h - e * g);
  n2w[3] = -(b * i - c * h); n2w[4] = (a * i - c * g); n2w[5] = -(a * h - b * g);
  n2w[6] = (b * f - c * e); n2w[7] = -(a * f - c * d); n2w[8] = (a * e - b * d);
}

// Extract one prototype's geometry in PROTOTYPE-LOCAL (object) space from a Mesh
// prim: positions, fan-triangulated indices, per-triangle flat + vertex normals,
// and the constant displayColor. Mirrors the flat GPU extractor minus the world
// transform (the instance transform is applied by the TLAS). Displacement IS
// applied here (once per prototype, in object space — the instance transform
// scales it, like any other prototype-local geometry).
static bool ExtractProtoGeo(const lightusd::next::Stage &stage,
                            const Options &opt, TextureCache &tc,
                            const lightusd::next::UsdPrim &prim,
                            GpuInstProto *out) {
  const lightusd::next::Value *val = prim.GetPropertyValue("points");
  if (!val) return false;
  const std::vector<float> *pts = val->as_float_array();
  if (!pts || pts->empty()) return false;
  const uint32_t nv = uint32_t(pts->size() / 3);
  out->verts = *pts;

  std::vector<Vec3> vn(nv, Vec3{0, 0, 0});
  bool perVertexN = false;
  val = prim.GetPropertyValue("normals");
  if (val) {
    const std::vector<float> *nrm = val->as_float_array();
    if (nrm && nrm->size() == size_t(nv) * 3) {
      perVertexN = true;
      for (uint32_t j = 0; j < nv; ++j)
        vn[j] = Vec3{(*nrm)[j * 3], (*nrm)[j * 3 + 1], (*nrm)[j * 3 + 2]};
    }
  }

  val = prim.GetPropertyValue("faceVertexIndices");
  const lightusd::next::Value *cval = prim.GetPropertyValue("faceVertexCounts");
  if (!val) return false;
  const std::vector<int> *idx = val->as_int_array();
  const std::vector<int> *cnt = cval ? cval->as_int_array() : nullptr;
  if (!idx || idx->empty()) return false;
  if (cnt && !cnt->empty()) {
    size_t off = 0;
    for (int c : *cnt) {
      if (c >= 3 && off + size_t(c) <= idx->size()) {
        int v0 = (*idx)[off];
        for (int k = 1; k + 1 < c; ++k) {
          out->idx.push_back(uint32_t(v0));
          out->idx.push_back(uint32_t((*idx)[off + size_t(k)]));
          out->idx.push_back(uint32_t((*idx)[off + size_t(k) + 1]));
        }
      }
      off += size_t(c < 0 ? 0 : c);
    }
  } else {
    for (int v : *idx) out->idx.push_back(uint32_t(v));
  }
  // Drop any triangle that indexes a vertex out of range (the BLAS build sets
  // maxVertex = nv-1; an OOB index would corrupt traversal).
  for (size_t t = 0; t + 2 < out->idx.size(); t += 3)
    if (out->idx[t] >= nv || out->idx[t + 1] >= nv || out->idx[t + 2] >= nv) {
      out->idx.clear();
      break;
    }
  out->ntris = uint32_t(out->idx.size() / 3);
  if (out->ntris == 0) return false;

  // Coarse displacement (object space, applied to out->verts BEFORE the flat
  // normals are recomputed). Mirrors the flat GPU extractor: resolve the bound
  // material's inputs:displacement (constant + scalar texture sampled with
  // primvars:st) and offset each vertex along its area-weighted smooth normal.
  if (opt.displace && opt.displace_scale != 0.0f) {
    std::vector<float> uvs;
    if (const lightusd::next::Value *uv = prim.GetPropertyValue("primvars:st")) {
      const std::vector<float> *u = uv->as_float_array();
      if (u && !u->empty()) uvs = *u;
    }
    float disp_const = 0.0f;
    ScalarTex disp_tex;
    const std::vector<lightusd::next::Path> *bind =
        prim.GetRelationship("material:binding");
    if (bind && !bind->empty()) {
      lightusd::next::UsdPrim mat = stage.GetPrimAtPath((*bind)[0]);
      if (mat.IsValid()) {
        lightusd::next::UsdPrim surf = ConnectedPrimNext(stage, mat, "outputs:surface");
        if (!surf.IsValid())
          surf = ConnectedPrimNext(stage, mat, "outputs:mtlx:surface");
        if (surf.IsValid()) {
          if (const lightusd::next::Value *d =
                  surf.GetPropertyValue("inputs:displacement"))
            if (const float *f = d->as_float()) disp_const = *f;
          ResolveScalarTextureNext(stage, surf, "inputs:displacement", tc, &disp_tex);
        }
      }
    }
    if (disp_tex.id >= 0 || disp_const != 0.0f) {
      std::vector<Vec3> sn(nv, Vec3{0, 0, 0});  // area-weighted smooth normals
      for (size_t t = 0; t + 2 < out->idx.size(); t += 3) {
        uint32_t a = out->idx[t], b = out->idx[t + 1], c = out->idx[t + 2];
        Vec3 pa{out->verts[a * 3], out->verts[a * 3 + 1], out->verts[a * 3 + 2]};
        Vec3 pb{out->verts[b * 3], out->verts[b * 3 + 1], out->verts[b * 3 + 2]};
        Vec3 pc{out->verts[c * 3], out->verts[c * 3 + 1], out->verts[c * 3 + 2]};
        Vec3 fn = Cross(Sub(pb, pa), Sub(pc, pa));
        sn[a] = Add(sn[a], fn); sn[b] = Add(sn[b], fn); sn[c] = Add(sn[c], fn);
      }
      const lusdr::Texture *dtex =
          (disp_tex.id >= 0 && tc.textures &&
           size_t(disp_tex.id) < tc.textures->size())
              ? &(*tc.textures)[size_t(disp_tex.id)]
              : nullptr;
      const bool per_vertex_uv = uvs.size() >= size_t(nv) * 2;
      for (uint32_t v = 0; v < nv; ++v) {
        if (Length(sn[v]) < 1.0e-12f) continue;
        Vec3 n = Normalize(sn[v]);
        float hh = disp_const;
        if (dtex) {
          float u = per_vertex_uv ? uvs[v * 2] : 0.0f;
          float vv = per_vertex_uv ? uvs[v * 2 + 1] : 0.0f;
          hh = dtex->sample_channel(u, vv, 0.0f, disp_tex.ch) * disp_tex.scale +
               disp_tex.bias;
        }
        hh *= opt.displace_scale;
        out->verts[v * 3 + 0] += n.x * hh;
        out->verts[v * 3 + 1] += n.y * hh;
        out->verts[v * 3 + 2] += n.z * hh;
      }
    }
  }

  for (uint32_t t = 0; t < out->ntris; ++t) {
    uint32_t i0 = out->idx[t * 3], i1 = out->idx[t * 3 + 1], i2 = out->idx[t * 3 + 2];
    Vec3 p0{out->verts[i0 * 3], out->verts[i0 * 3 + 1], out->verts[i0 * 3 + 2]};
    Vec3 p1{out->verts[i1 * 3], out->verts[i1 * 3 + 1], out->verts[i1 * 3 + 2]};
    Vec3 p2{out->verts[i2 * 3], out->verts[i2 * 3 + 1], out->verts[i2 * 3 + 2]};
    out->normals.push_back(Normalize(Cross(Sub(p1, p0), Sub(p2, p0))));
    out->vn0.push_back(perVertexN ? vn[i0] : Vec3{0, 0, 0});
    out->vn1.push_back(perVertexN ? vn[i1] : Vec3{0, 0, 0});
    out->vn2.push_back(perVertexN ? vn[i2] : Vec3{0, 0, 0});
  }

  out->base_color = Vec3{0.5f, 0.5f, 0.5f};
  if (const lightusd::next::Value *dcv =
          prim.GetPropertyValue("primvars:displayColor")) {
    const std::vector<float> *dc = dcv->as_float_array();
    if (dc && dc->size() >= 3) out->base_color = Vec3{(*dc)[0], (*dc)[1], (*dc)[2]};
  }
  return true;
}

// Split a prototype whose triangle count exceeds `chunk` into sub-prototypes of
// <= chunk triangles each (vertices remapped/compacted per chunk, shading slices
// carried along). Each sub-proto is just a smaller prototype -- one BLAS, its own
// triangle range -- so the instance encoding (pid = instance*stride + localTri)
// and hit decode are unchanged; a placement of the original prototype is simply
// emitted once per sub-proto. Works around drivers (RADV) whose AS build-size
// query overflows for very large single-BLAS builds and then GPU-faults.
static void ChunkProto(const GpuInstProto &src, uint32_t chunk,
                       std::vector<GpuInstProto> *out) {
  const uint32_t nch = (src.ntris + chunk - 1u) / chunk;
  for (uint32_t c = 0; c < nch; ++c) {
    const uint32_t t0 = c * chunk;
    const uint32_t t1 = std::min((c + 1u) * chunk, src.ntris);
    GpuInstProto sub;
    sub.base_color = src.base_color;
    sub.ntris = t1 - t0;
    sub.idx.reserve(size_t(t1 - t0) * 3);
    std::unordered_map<uint32_t, uint32_t> remap;
    for (uint32_t t = t0; t < t1; ++t) {
      for (int k = 0; k < 3; ++k) {
        const uint32_t ov = src.idx[t * 3 + k];
        auto r = remap.find(ov);
        uint32_t nvid;
        if (r == remap.end()) {
          nvid = uint32_t(sub.verts.size() / 3);
          remap.emplace(ov, nvid);
          sub.verts.push_back(src.verts[ov * 3 + 0]);
          sub.verts.push_back(src.verts[ov * 3 + 1]);
          sub.verts.push_back(src.verts[ov * 3 + 2]);
        } else {
          nvid = r->second;
        }
        sub.idx.push_back(nvid);
      }
      sub.normals.push_back(src.normals[t]);
      sub.vn0.push_back(src.vn0[t]);
      sub.vn1.push_back(src.vn1[t]);
      sub.vn2.push_back(src.vn2[t]);
    }
    out->push_back(std::move(sub));
  }
}

// -vkInstanced: decompose the expanded mesh jobs into shared prototypes (grouped
// by source prim path) + per-placement instances, then render with the true
// two-level GPU TLAS. Returns true if it produced an image; false to fall back to
// the flat path. Geometry is stored ONCE per prototype regardless of instance
// count (the memory-sharing win over the flat world-space soup).
static bool TryRunInstancedVk(const lightusd::next::Stage &stage,
                              const Options &opt) {
  GpuInstancedScene scene;
  // Each source prim maps to one OR MORE sub-prototype indices (a huge prototype
  // is chunk-split; an empty vector marks a prim that failed extraction).
  std::unordered_map<std::string, std::vector<uint32_t>> proto_id;
  std::vector<Bounds> proto_aabb;
  // Max triangles per prototype BLAS (8M; overridable via LUSDR_INST_CHUNK_TRIS
  // for testing the split path on small meshes). A larger prototype is chunked.
  uint32_t chunk = 8u * 1024u * 1024u;
  if (const char *e = std::getenv("LUSDR_INST_CHUNK_TRIS")) {
    long v = std::atol(e);
    if (v > 0) chunk = uint32_t(v);
  }
  // Displacement textures for prototype extraction (loaded relative to the input;
  // shared/cached across prototypes). Same setup as the flat GPU path.
  std::vector<lusdr::Texture> disp_textures;
  TextureCache tc;
  tc.textures = &disp_textures;
  tc.base_dir = DirName(opt.input);
  tc.usdz = nullptr;
  tc.options = &opt;
  // Append a prototype + its object-space AABB; returns its index.
  auto push_proto = [&](GpuInstProto &&pr) -> uint32_t {
    Bounds bb;
    for (size_t j = 0; j < pr.verts.size() / 3; ++j) {
      bb.lo.x = std::min(bb.lo.x, pr.verts[j * 3 + 0]);
      bb.lo.y = std::min(bb.lo.y, pr.verts[j * 3 + 1]);
      bb.lo.z = std::min(bb.lo.z, pr.verts[j * 3 + 2]);
      bb.hi.x = std::max(bb.hi.x, pr.verts[j * 3 + 0]);
      bb.hi.y = std::max(bb.hi.y, pr.verts[j * 3 + 1]);
      bb.hi.z = std::max(bb.hi.z, pr.verts[j * 3 + 2]);
    }
    bb.valid = true;
    const uint32_t idx = uint32_t(scene.protos.size());
    scene.protos.push_back(std::move(pr));
    proto_aabb.push_back(bb);
    return idx;
  };

  // Group each placement into (prototype, per-instance transform) on the fly. The
  // streaming collector calls this sink once per placement (prim, world, purpose)
  // WITHOUT materializing a MeshJobNext per instance, so a huge instanced scene
  // (Moana island: tens of millions) costs ~one GpuInstPlacement (88 B) of host
  // memory per placement instead of a ~392 B MeshJobNext.
  auto place = [&](const lightusd::next::UsdPrim &prim, const matrix4d &world,
                   lightusd::Purpose purpose) {
    if (!PurposeVisible(PurposeBit(purpose), opt.purpose_mask)) return;
    const std::string key = prim.GetPath().str();
    auto it = proto_id.find(key);
    const std::vector<uint32_t> *subs = nullptr;
    if (it == proto_id.end()) {
      GpuInstProto pr;
      if (!ExtractProtoGeo(stage, opt, tc, prim, &pr)) {
        proto_id.emplace(key, std::vector<uint32_t>{});  // empty = bad prim
        return;
      }
      std::vector<uint32_t> indices;
      if (pr.ntris <= chunk) {
        indices.push_back(push_proto(std::move(pr)));
      } else {
        std::vector<GpuInstProto> chunks;
        ChunkProto(pr, chunk, &chunks);
        for (auto &sub : chunks) indices.push_back(push_proto(std::move(sub)));
      }
      subs = &proto_id.emplace(key, std::move(indices)).first->second;
    } else {
      if (it->second.empty()) return;  // known-bad prim
      subs = &it->second;
    }
    GpuInstPlacement pl;
    Mat4ToObj2World(world, pl.o2w);
    NormalMatrixFromO2W(pl.o2w, pl.n2w);
    for (uint32_t sp : *subs) {
      pl.proto = sp;
      scene.insts.push_back(pl);
    }
  };

  // Placement budget: bound host memory on scenes with tens of millions of
  // instances. Default 16M -- one TLAS slice, so the default takes the single-TLAS
  // fast path and stays well within GPU memory (Moana island's full ~42.8M
  // instances / ~110k prototype BLAS exceed VRAM; raise LUSDR_INST_BUDGET to fan
  // out across multiple TLASes, memory permitting). Env-overridable.
  size_t budget = 16000000u;
  if (const char *e = std::getenv("LUSDR_INST_BUDGET")) {
    long v = std::atol(e);
    if (v > 0) budget = size_t(v);
  }
  size_t emitted = 0;
  for (const auto &root : stage.GetRootPrims()) {
    if (emitted >= budget) break;
    emitted += CollectRTInstancePlacementsNext(
        stage, root, matrix4d::identity(), lightusd::Purpose::Default,
        opt.timecode, opt.mask, place, budget - emitted);
  }
  if (emitted >= budget)
    std::cerr << "[vkInstanced] instance budget " << budget
              << " reached; rendering a bounded subset (raise via "
                 "LUSDR_INST_BUDGET).\n";
  if (scene.protos.empty() || scene.insts.empty()) return false;

  // World bounds = union of each instance's prototype AABB under its o2w.
  Bounds bounds;
  for (const GpuInstPlacement &pl : scene.insts) {
    const Bounds &lb = proto_aabb[pl.proto];
    for (int cI = 0; cI < 8; ++cI) {
      Vec3 p{(cI & 1) ? lb.hi.x : lb.lo.x, (cI & 2) ? lb.hi.y : lb.lo.y,
             (cI & 4) ? lb.hi.z : lb.lo.z};
      Vec3 wp = TransformPointO2W(pl.o2w, p);
      bounds.lo.x = std::min(bounds.lo.x, wp.x);
      bounds.lo.y = std::min(bounds.lo.y, wp.y);
      bounds.lo.z = std::min(bounds.lo.z, wp.z);
      bounds.hi.x = std::max(bounds.hi.x, wp.x);
      bounds.hi.y = std::max(bounds.hi.y, wp.y);
      bounds.hi.z = std::max(bounds.hi.z, wp.z);
    }
  }
  bounds.valid = true;

  double up_axis = 1.0;
  {
    std::string up = stage.GetUpAxis();
    if (up == "Z") up_axis = 2.0;
    else if (up == "X") up_axis = 0.0;
  }
  lightusd::Axis usdUp = (up_axis == 2.0)   ? lightusd::Axis::Z
                         : (up_axis == 0.0) ? lightusd::Axis::X
                                            : lightusd::Axis::Y;
  int out_height = opt.height;
  CameraFrame camera = ResolveGpuCameraInst(stage, opt, bounds, usdUp, &out_height);

  // -rtLod on the two-level path: classify each placement from the resolved
  // camera (this is the structure LOD is meant for -- real per-instance TLAS
  // selection, not the flatten-side approximation). Cull -> drop the instance;
  // Proxy -> point the instance at a shared unit-box prototype via a box-fit
  // transform onto the prototype's local AABB (distant prototypes become gray
  // boxes, the box BLAS stored ONCE); Full -> keep. Same knobs/semantics as the
  // CPU -rtPreview -rtLod path.
  if (opt.rt_lod) {
    lusdr::RtLodConfig cfg;
    cfg.enabled = true;
    cfg.proxy = opt.rt_lod_proxy;
    cfg.frustum_cull = opt.rt_lod_frustum_cull;
    cfg.full_px = opt.rt_lod_full_px;
    cfg.cull_px = opt.rt_lod_cull_px;
    const lusdr::RtLodView view = lusdr::MakeRtLodView(camera, out_height);
    static const float kC[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    static const uint32_t kI[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                    0, 1, 5, 0, 5, 4, 3, 6, 2, 3, 7, 6,
                                    0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    uint32_t box_proto = 0xFFFFFFFFu;  // appended lazily on the first Proxy
    lusdr::RtLodStats st;
    std::vector<GpuInstPlacement> kept;
    kept.reserve(scene.insts.size());
    for (const GpuInstPlacement &pl : scene.insts) {
      const lusdr::RtLod lod =
          lusdr::ClassifyInstance(view, cfg, pl.o2w, proto_aabb[pl.proto]);
      if (lod == lusdr::RtLod::Cull) {
        st.culled++;
        continue;
      }
      if (lod == lusdr::RtLod::Proxy) {
        st.proxy++;
        if (box_proto == 0xFFFFFFFFu) {
          GpuInstProto bp;
          bp.verts.assign(&kC[0][0], &kC[0][0] + 24);
          bp.idx.assign(kI, kI + 36);
          bp.ntris = 12;
          for (uint32_t t = 0; t < 12; ++t) {
            uint32_t i0 = kI[t * 3], i1 = kI[t * 3 + 1], i2 = kI[t * 3 + 2];
            Vec3 p0{kC[i0][0], kC[i0][1], kC[i0][2]};
            Vec3 p1{kC[i1][0], kC[i1][1], kC[i1][2]};
            Vec3 p2{kC[i2][0], kC[i2][1], kC[i2][2]};
            bp.normals.push_back(Normalize(Cross(Sub(p1, p0), Sub(p2, p0))));
            bp.vn0.push_back(Vec3{0, 0, 0});
            bp.vn1.push_back(Vec3{0, 0, 0});
            bp.vn2.push_back(Vec3{0, 0, 0});
          }
          bp.base_color = Vec3{0.5f, 0.5f, 0.5f};  // gray box proxy
          box_proto = uint32_t(scene.protos.size());
          scene.protos.push_back(std::move(bp));
          proto_aabb.push_back(Bounds{});  // unused after classify
        }
        // Pad any near-zero AABB axis so a planar/linear prototype still yields a
        // box with volume — a zero-thickness box collapses to mostly-degenerate
        // triangles whose tiny standalone BLAS traverses to no hits (a flat soup
        // hides this, a per-proxy BLAS does not).
        Bounds lb = proto_aabb[pl.proto];
        const float ex = lb.hi.x - lb.lo.x, ey = lb.hi.y - lb.lo.y,
                    ez = lb.hi.z - lb.lo.z;
        const float eps =
            std::max({ex, ey, ez, 0.0f}) * 1.0e-3f + 1.0e-6f;
        if (ex < eps) { lb.lo.x -= 0.5f * eps; lb.hi.x += 0.5f * eps; }
        if (ey < eps) { lb.lo.y -= 0.5f * eps; lb.hi.y += 0.5f * eps; }
        if (ez < eps) { lb.lo.z -= 0.5f * eps; lb.hi.z += 0.5f * eps; }
        GpuInstPlacement bpl;
        lusdr::BoxFitO2W(pl.o2w, lb.lo, lb.hi, bpl.o2w);
        NormalMatrixFromO2W(bpl.o2w, bpl.n2w);
        bpl.proto = box_proto;
        kept.push_back(bpl);
        continue;
      }
      st.full++;
      kept.push_back(pl);
    }
    scene.insts.swap(kept);
    if (opt.stats)
      std::cerr << "[rt-lod] two-level: full=" << st.full << " proxy=" << st.proxy
                << " culled=" << st.culled << " (instances=" << scene.insts.size()
                << ", prototypes=" << scene.protos.size() << ")\n";
    if (scene.insts.empty()) {
      std::cerr << "All instances culled by -rtLod (try a smaller -rtLodCullPx); "
                   "falling back.\n";
      return false;
    }
  }

  // Graceful instance cap. The wide multi-TLAS builder splits the placements into
  // ceil(N / 16M) TLAS slices (sharing one BLAS set), so a scene past the device
  // TLAS maxInstanceCount (2^24) still renders in full -- the whole ~42.8M-instance
  // Moana island fits. Cap only at a generous multi-slice ceiling (to bound VRAM /
  // the K sequential dispatches) and keep the CAMERA-NEAREST placements past it.
  // In practice the LUSDR_INST_BUDGET host-memory budget below binds first.
  const uint64_t max_inst = 8ull * 16000000ull;  // 8 TLAS slices (~128M instances)
  if (scene.insts.size() > max_inst) {
    const Vec3 eye = camera.origin, fwd = camera.forward;
    auto depth = [&](const GpuInstPlacement &p) {  // view-space depth of o2w origin
      return (p.o2w[3] - eye.x) * fwd.x + (p.o2w[7] - eye.y) * fwd.y +
             (p.o2w[11] - eye.z) * fwd.z;
    };
    std::nth_element(scene.insts.begin(), scene.insts.begin() + size_t(max_inst),
                     scene.insts.end(),
                     [&](const GpuInstPlacement &a, const GpuInstPlacement &b) {
                       return depth(a) < depth(b);
                     });
    std::cerr << "[vkInstanced] capping " << scene.insts.size() << " -> " << max_inst
              << " instances to fit the TLAS maxInstanceCount; keeping the "
                 "camera-nearest subset.\n";
    scene.insts.resize(size_t(max_inst));
  }

  // Prune prototypes no instance references (the cap / LOD can orphan many), so we
  // don't build a BLAS per unused prototype. Remap the survivors compactly.
  {
    std::vector<uint32_t> remap(scene.protos.size(), 0xFFFFFFFFu);
    std::vector<GpuInstProto> kept;
    kept.reserve(scene.protos.size());
    for (GpuInstPlacement &in : scene.insts) {
      if (remap[in.proto] == 0xFFFFFFFFu) {
        remap[in.proto] = uint32_t(kept.size());
        kept.push_back(std::move(scene.protos[in.proto]));
      }
      in.proto = remap[in.proto];
    }
    if (kept.size() < scene.protos.size() && opt.stats)
      std::cerr << "[vkInstanced] pruned " << scene.protos.size() << " -> "
                << kept.size() << " referenced prototypes\n";
    scene.protos.swap(kept);
  }

  return RunVulkanLightRTInstanced(opt, scene, camera, out_height);
}
#endif  // HAVE_VULKAN

int main(int argc, char **argv) {
#if defined(__GLIBC__)
  // Triangle streaming allocates and frees large transient per-job buffers
  // (megabytes each) while the final geometry buffers stay live. glibc's default
  // dynamic mmap threshold (which grows up to 32 MB) keeps those big frees in the
  // arena interleaved with live data, so they cannot be returned to the OS and
  // inflate peak RSS by ~1.3 GB on Island-scale scenes (isCoral 6.4 -> 5.0 GB).
  // Pinning the mmap threshold at 1 MB routes the large temporaries through
  // mmap/munmap (returned to the OS on free); only sub-MB allocations stay in the
  // arena, so the per-call syscall cost is negligible (~0.2 s on isCoral).
  mallopt(M_MMAP_THRESHOLD, 1 << 20);
  mallopt(M_TRIM_THRESHOLD, 4 << 20);
  // Grow each (sub-MB) arena in 16 MB chunks. The parallel compose allocates
  // ~140K prims' property storage; with the default minimal top-pad, glibc
  // commits a fresh page almost per prim -- ~139K mprotect() calls on isCoral
  // (97% of all syscall time, serialized under the kernel mmap_lock, which caps
  // the parallel fill near ~4 cores). A 16 MB pad commits arena memory in bulk so
  // those calls collapse to a few hundred; isCoral load 3.85 -> 3.48 s. Sized to
  // hold peak RSS under budget (128 MB pad inflated it ~400 MB; 16 MB is +~50 MB).
  mallopt(M_TOP_PAD, 16 << 20);
#endif
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    return EXIT_FAILURE;
  }
  SetIblBackendEnvmap(opt.ibl_envmap);
  ApplyLargeSceneProfile(&opt);

  // Configure the process memory budget: -maxMem <GiB>, else auto
  // min(32 GiB, 0.5 * system MemAvailable). Keeps lusdrender from being
  // OOM-killed on huge (instance-expanded) scenes; it aborts with a clear
  // message instead.
  MemBudget::Get().Init(opt.max_mem_gib);
  // Apply the same bounded texture defaults even without a named large-scene
  // profile. Explicit texture flags remain authoritative, including an
  // explicit zero when a caller intentionally requests source resolution.
  {
    const lightusd::tydra::next::ResourceBudget budget =
        lightusd::tydra::next::ComputeResourceBudget(
            MemBudget::Get().Cap(), QueryDeviceLocalVRAMBytes());
    const lightusd::tydra::next::TextureBudget texture_budget =
        lightusd::tydra::next::DeriveTextureBudget(budget);
    if (!opt.texture_max_size_explicit && texture_budget.max_edge > 0)
      opt.texture_max_size = static_cast<int>(texture_budget.max_edge);
    if (!opt.texture_budget_explicit && texture_budget.budget_bytes > 0) {
      opt.texture_budget_mb = static_cast<int>(
          texture_budget.budget_bytes / (1024ull * 1024ull));
    }
    if (opt.stats) {
      std::cerr << "texture budget: " << opt.texture_budget_mb
                << " MiB, max edge " << opt.texture_max_size << "\n";
    }
  }
  if (opt.stats) {
    std::cerr << "memory cap: " << MemBudget::GiB(MemBudget::Get().Cap());
    size_t avail = MemBudget::AvailableSystemMemory();
    if (avail) std::cerr << " (system avail: " << MemBudget::GiB(avail) << ")";
    std::cerr << "\n";
  }

  // Interactive / scripted modes (memory-persistent rendering over the next
  // loader): -js runs a JavaScript animation/control script, -mcp runs an MCP
  // stdio control server. Both keep the scene + BVH resident for repeated
  // re-rendering.
  if (opt.mcp || !opt.js_script.empty()) {
#ifdef LIGHTUSD_WITH_QJS
    if (opt.mcp) return RunMCPMode(opt);
    return RunJSScriptMode(opt, opt.js_script);
#else
    std::cerr << "-js/-mcp require building with -DLIGHTUSD_WITH_QJS=ON.\n";
    return EXIT_FAILURE;
#endif
  }

  // -streamHttp: WebSocket browser streaming server. Keeps the scene + BVH
  // resident and re-renders the camera on demand as the browser navigates.
  if (opt.stream_http > 0) {
#ifdef LUSDRENDER_WITH_STREAM
    return RunStreamServer(opt);
#else
    std::cerr << "-streamHttp requires building with LUSDRENDER_WITH_STREAM.\n";
    return EXIT_FAILURE;
#endif
  }

  // View-dependent district LOD (-lodStream): compose the scene in proxy LOD,
  // promote the districts nearest the camera to `full` under the host/VRAM
  // budget, and rewrite opt.input to a generated wrapper layer. Then fall
  // through to the normal next-loader render. Best-effort: on failure (no
  // districts / compose error) opt.input is left untouched and we render as-is.
  std::string lod_wrapper;
  if (opt.lod_stream) {
    if (!opt.rt_preview && !opt.vulkan && !opt.vulkan_rt && !opt.use_d3d &&
        !opt.hip && !opt.cuda) {
      opt.rt_preview = true;  // default the LOD render to the CPU rtPreview path
    }
    PrepareLodStream(&opt, &lod_wrapper);
  }

  // Use the `next` streaming CPU path when requested, and by default for USDC
  // where mmap-backed arrays provide the largest memory win. Ordinary USDA/USDZ
  // stays on the schema-rich converter for direct lights/primitives and uniform
  // subdivision until those paths have full streaming parity.
  const std::string lower_input = LowerAscii(opt.input);
  const bool default_next_usdc =
      lower_input.size() >= 5 &&
      lower_input.compare(lower_input.size() - 5, 5, ".usdc") == 0;
  if (!opt.legacy_load && opt.subdivision_level == 0 &&
      (opt.rt_preview || default_next_usdc) && !opt.vulkan && !opt.use_d3d &&
      !opt.hip && !opt.cuda) {
    return RunRTPreviewNext(opt);
  }

  if (opt.aov_explicit || opt.all_render_products) {
    std::cerr << "Capture AOVs/products require the next CPU path (no GPU, legacy or subdivision override)\n";
    return EXIT_FAILURE;
  }

  // -frames (per-timecode animation output) is implemented only by the `next`
  // path above. It used to be silently ignored here -- the run produced one
  // image literally named with the `####` token and no animation. Fail loudly
  // instead so the user adds -rtPreview (or drops the flag).
  if (!opt.frames.empty()) {
    std::cerr << "-frames is only supported on the next path (a .usdc input or "
                 "-rtPreview); this run would render a single frame and ignore "
                 "it. Add -rtPreview, or drop -frames.\n";
    return EXIT_FAILURE;
  }

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
  // GPU backends (Vulkan / Direct3D 11 / HIP): load the scene through the `next`
  // lazy loader, build the geometry once, then trace on the selected GPU backend.
  if (opt.vulkan || opt.use_d3d || opt.hip || opt.cuda) {
    // Load through next loader.
    lightusd::next::Stage stage;
    lightusd::next::ValueClipStageLoader clip_stage_loader;
    std::string warn, err;
    if (!LoadNextStageBudgeted(opt, &stage, &warn, &err,
                               &clip_stage_loader)) {
      if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";
      std::cerr << "Failed to load USD: " << err << "\n";
      return EXIT_FAILURE;
    }
    if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";

#if defined(HAVE_VULKAN)
    // -vkInstanced: true two-level GPU TLAS (per-prototype BLAS shared across
    // instances). TryRunInstancedVk STREAMS the expanded placements (grouping each
    // by source prim into shared prototypes + per-instance transforms as it goes,
    // no per-instance MeshJobNext), then renders. On any failure (no shares, ray
    // query unavailable) it falls through to the flat GPU path below.
    if (opt.vulkan_instanced && opt.vulkan_rt) {
      if (TryRunInstancedVk(stage, opt)) return EXIT_SUCCESS;
      std::cerr << "[vkInstanced] falling back to the flat GPU path.\n";
    }
#endif

    // Collect meshes and build geometry. Only base_colors + geos feed the GPU
    // backends (RunVulkanLightRT / RunD3D11LightRT).
    std::vector<Vec3> base_colors;
    std::vector<RTPreviewStats::MeshGeometry> geos;
    std::vector<ResolvedMat> gpu_materials;
    std::vector<lusdr::Texture> gpu_textures;
    LightCache gpu_lights;
    for (const auto &root : stage.GetRootPrims()) {
      CollectLightsNext(stage, root, matrix4d::identity(), opt.timecode,
                        &gpu_lights);
    }
    RenderContext gaussian_ctx;
    gaussian_ctx.opt = opt;
    gaussian_ctx.clip_stage_loader = clip_stage_loader;
    // Vulkan and HIP consume the native analytic ellipse scene for a pure
    // Gaussian stage. A flat GPU trace cannot combine that DirectScene with mesh
    // BLASes, however: mixed mesh+splat stages must use the bounded tessellation
    // fallback below or the splats would silently disappear. D3D11 still uses
    // that fallback; do not build native arrays for it. Build the native Gaussian
    // BVHs only after mesh/curve collection proves this is a pure splat stage so
    // mixed flat traces do not duplicate the authored splat arrays.
    bool native_gaussian = false;
    bool has_other_native_carrier = false;

    {
      // Collect meshes WITH their world transforms, purpose, and -mask, exactly
      // like the CPU -rtPreview path (CollectRTPreviewMeshesNext). The GPU
      // backends previously walked every Mesh and emitted its RAW LOCAL points,
      // ignoring world transforms, purpose, and -mask -- correct only for a
      // single mesh at the origin (suzanne), but for any composed/production
      // scene (e.g. Caldera) it collapsed every transformed district to the
      // origin and pulled in the guide breadcrumb/endpoint Points, burying the
      // camera. PointInstancers AND scenegraph (instanceable) native instances are
      // expanded to world-space placements here (expand_instancers=true) so
      // instanced geometry renders on the GPU path -- see doc/lusdrender.md
      // (Instancing on the GPU backends). No per-prototype BLAS sharing yet.
      // Displacement textures are shared across roots; traversal records are
      // intentionally root-local below.
      TextureCache tc;
      tc.textures = &gpu_textures;
      tc.base_dir = DirName(opt.input);
      tc.usdz = nullptr;
      tc.options = &opt;
      std::unordered_map<std::string, ResolvedMat> material_cache;
      for (const auto &root : stage.GetRootPrims()) {
        // Keep only one root's traversal records alive.  A production stage can
        // contain thousands of roots (or large instancer expansions); collecting
        // every MeshJobNext before converting any geometry creates a needless
        // scene-wide transient peak on top of the final GPU chunk stream.
        std::vector<MeshJobNext> mesh_jobs;
        CollectRTPreviewMeshesNext(stage, root, matrix4d::identity(),
                                   lightusd::Purpose::Default, opt.timecode,
                                   opt.mask, &mesh_jobs,
                                   /*expand_instancers=*/true);

        // Stream geometry in WORLD space.
        for (MeshJobNext &job : mesh_jobs) {
        // Purpose visibility: hide guide (and others per -purpose) like the CPU
        // path; the GPU path used to render every purpose unconditionally, so the
        // 26M-triangle guide breadcrumb/endpoint Points engulfed the camera.
        if (!PurposeVisible(PurposeBit(job.purpose), opt.purpose_mask)) continue;
        lightusd::next::UsdPrim &prim = job.prim;
        ResolveMeshMaterialCached(stage, prim, tc, material_cache, &job);
        RTPreviewStats::MeshGeometry geo;
        uint32_t nv = 0;
        const lightusd::next::Value *val = prim.GetPropertyValue("points");
        if (!val) continue;
        const std::vector<float> *pts = val->as_float_array();
        if (!pts || pts->empty()) continue;
        nv = uint32_t(pts->size() / 3);
        // Transform local points into world space by the job's world matrix.
        geo.positions.resize(size_t(nv) * 3);
        for (uint32_t j = 0; j < nv; ++j) {
          Vec3 wp = TransformPoint(
              job.world,
              Vec3{(*pts)[j * 3 + 0], (*pts)[j * 3 + 1], (*pts)[j * 3 + 2]});
          geo.positions[j * 3 + 0] = wp.x;
          geo.positions[j * 3 + 1] = wp.y;
          geo.positions[j * 3 + 2] = wp.z;
        }

        val = prim.GetPropertyValue("normals");
        if (val) {
          const std::vector<float> *nrm = val->as_float_array();
          if (nrm && nrm->size() >= nv * 3) {
            geo.normals.resize(size_t(nv) * 3);
            for (uint32_t j = 0; j < nv; ++j) {
              Vec3 wn = TransformVector(
                  job.world,
                  Vec3{(*nrm)[j * 3 + 0], (*nrm)[j * 3 + 1], (*nrm)[j * 3 + 2]});
              geo.normals[j * 3 + 0] = wn.x;
              geo.normals[j * 3 + 1] = wn.y;
              geo.normals[j * 3 + 2] = wn.z;
            }
          }
        }
        if (geo.normals.empty()) {
          geo.normals.resize(size_t(nv) * 3, 0);
        }

        val = prim.GetPropertyValue("primvars:st");
        if (val) {
          const std::vector<float> *uv = val->as_float_array();
          if (uv && !uv->empty()) {
            geo.uvs = *uv;
          }
        }
        if (geo.uvs.empty()) geo.uvs.resize(nv * 2, 0);

        // Fan-triangulate the polygons into a triangle-soup index list. The GPU
        // backends (RunVulkanLightRT / RunD3D11LightRT) consume geo.indices as
        // groups of three, so quads/n-gons MUST be split here using
        // faceVertexCounts — feeding the raw faceVertexIndices chunked by 3 drops
        // and scrambles triangles (e.g. a 468-quad + 32-tri Suzanne collapses
        // from 968 triangles to 656, rendering with holes).
        val = prim.GetPropertyValue("faceVertexIndices");
        const lightusd::next::Value *cval =
            prim.GetPropertyValue("faceVertexCounts");
        if (val) {
          const std::vector<int> *idx = val->as_int_array();
          const std::vector<int> *cnt = cval ? cval->as_int_array() : nullptr;
          if (idx && !idx->empty()) {
            if (cnt && !cnt->empty()) {
              size_t off = 0;
              for (int c : *cnt) {
                if (c >= 3 && off + size_t(c) <= idx->size()) {
                  int v0 = (*idx)[off];
                  for (int k = 1; k + 1 < c; ++k) {
                    geo.indices.push_back(v0);
                    geo.indices.push_back((*idx)[off + size_t(k)]);
                    geo.indices.push_back((*idx)[off + size_t(k) + 1]);
                  }
                }
                off += size_t(c < 0 ? 0 : c);
              }
            } else {
              // No counts: assume an already-triangulated soup.
              geo.indices.assign(idx->begin(), idx->end());
            }
          }
        }

        // Coarse displacement: resolve the bound material's inputs:displacement and
        // offset each vertex along its smooth normal (RunVulkanLightRT triangulates
        // indices as a soup and shades with geometric normals computed from these
        // positions, so displaced positions are all that is needed).
        if (opt.displace && opt.displace_scale != 0.0f && !geo.indices.empty()) {
          float disp_const = 0.0f;
          ScalarTex disp_tex;
          const std::vector<lightusd::next::Path> *bind =
              prim.GetRelationship("material:binding");
          if (bind && !bind->empty()) {
            lightusd::next::UsdPrim mat = stage.GetPrimAtPath((*bind)[0]);
            if (mat.IsValid()) {
              lightusd::next::UsdPrim surf =
                  ConnectedPrimNext(stage, mat, "outputs:surface");
              if (!surf.IsValid())
                surf = ConnectedPrimNext(stage, mat, "outputs:mtlx:surface");
              if (surf.IsValid()) {
                if (const lightusd::next::Value *d =
                        surf.GetPropertyValue("inputs:displacement"))
                  if (const float *f = d->as_float()) disp_const = *f;
                ResolveScalarTextureNext(stage, surf, "inputs:displacement", tc,
                                         &disp_tex);
              }
            }
          }
          if (disp_tex.id >= 0 || disp_const != 0.0f) {
            // Area-weighted smooth vertex normals from the triangle soup.
            std::vector<Vec3> vn(nv, Vec3{0.0f, 0.0f, 0.0f});
            for (size_t t = 0; t + 2 < geo.indices.size(); t += 3) {
              int a = geo.indices[t], b = geo.indices[t + 1],
                  c = geo.indices[t + 2];
              if (a < 0 || b < 0 || c < 0 || uint32_t(a) >= nv ||
                  uint32_t(b) >= nv || uint32_t(c) >= nv)
                continue;
              Vec3 pa{geo.positions[a * 3], geo.positions[a * 3 + 1],
                      geo.positions[a * 3 + 2]};
              Vec3 pb{geo.positions[b * 3], geo.positions[b * 3 + 1],
                      geo.positions[b * 3 + 2]};
              Vec3 pc{geo.positions[c * 3], geo.positions[c * 3 + 1],
                      geo.positions[c * 3 + 2]};
              Vec3 fn = Cross(Sub(pb, pa), Sub(pc, pa));
              vn[a] = Add(vn[a], fn);
              vn[b] = Add(vn[b], fn);
              vn[c] = Add(vn[c], fn);
            }
            const lusdr::Texture *dtex =
                (disp_tex.id >= 0 &&
                 size_t(disp_tex.id) < gpu_textures.size())
                    ? &gpu_textures[size_t(disp_tex.id)]
                    : nullptr;
            const bool per_vertex_uv = geo.uvs.size() >= size_t(nv) * 2;
            for (uint32_t v = 0; v < nv; ++v) {
              if (Length(vn[v]) < 1.0e-12f) continue;
              Vec3 n = Normalize(vn[v]);
              float h = disp_const;
              if (dtex) {
                float u = per_vertex_uv ? geo.uvs[v * 2] : 0.0f;
                float vv = per_vertex_uv ? geo.uvs[v * 2 + 1] : 0.0f;
                h = dtex->sample_channel(u, vv, 0.0f, disp_tex.ch) * disp_tex.scale +
                    disp_tex.bias;
              }
              h *= opt.displace_scale;
              geo.positions[v * 3 + 0] += n.x * h;
              geo.positions[v * 3 + 1] += n.y * h;
              geo.positions[v * 3 + 2] += n.z * h;
            }
          }
        }

        // Base color from primvars:displayColor (constant); mid-grey default.
        Vec3 display_color{1.0f, 1.0f, 1.0f};
        if (const lightusd::next::Value *dcv =
                prim.GetPropertyValue("primvars:displayColor")) {
          const std::vector<float> *dc = dcv->as_float_array();
          if (dc && dc->size() >= 3)
            display_color = Vec3{(*dc)[0], (*dc)[1], (*dc)[2]};
        }
        const Vec3 bc{job.base_color.x * display_color.x,
                      job.base_color.y * display_color.y,
                      job.base_color.z * display_color.z};
        base_colors.push_back(bc);
        ResolvedMat resolved;
        // Preserve the same material * constant displayColor composition used
        // by the CPU/GPU flat path. Per-vertex displayColor needs a parallel
        // streaming attribute; the constant form can be folded losslessly.
        resolved.base_color = bc;
        resolved.tex_id = job.tex_id;
        resolved.roughness = job.roughness;
        resolved.metallic = job.metallic;
        resolved.normal_tex_id = job.normal_tex_id;
        resolved.coat_normal_tex_id = job.coat_normal_tex_id;
        resolved.uv_xform = job.uv_xform;
        resolved.rough_tex = job.rough_tex;
        resolved.metal_tex = job.metal_tex;
        resolved.emission = job.emission;
        resolved.emission_tex_id = job.emission_tex_id;
        resolved.occlusion = job.occlusion;
        resolved.occ_tex = job.occ_tex;
        resolved.opacity = job.opacity;
        resolved.opacity_tex = job.opacity_tex;
        resolved.opacity_threshold = job.opacity_threshold;
        resolved.clearcoat = job.clearcoat;
        resolved.clearcoat_roughness = job.clearcoat_roughness;
        resolved.clearcoat_tex = job.clearcoat_tex;
        resolved.clearcoat_rough_tex = job.clearcoat_rough_tex;
        resolved.specular_color = job.specular_color;
        resolved.specular_tex_id = job.specular_tex_id;
        resolved.ior = job.ior;
        resolved.use_specular_workflow = job.use_specular_workflow;
        resolved.displacement = job.displacement;
        resolved.displacement_tex = job.displacement_tex;
        resolved.has_openpbr = job.has_openpbr;
        resolved.openpbr = job.openpbr;
        resolved.materialx_graph_json = job.materialx_graph_json;
        for (const std::string &api : job.prim.GetMeta().apiSchemas()) {
          if (api != "MeshLightAPI") continue;
          float intensity = 1.0f, exposure = 0.0f;
          Vec3 light_color{1.0f, 1.0f, 1.0f};
          bool normalize = false;
          if (const lightusd::next::Value *v =
                  job.prim.GetPropertyValue("inputs:intensity"))
            if (const float *f = v->as_float()) intensity = *f;
          if (const lightusd::next::Value *v =
                  job.prim.GetPropertyValue("inputs:exposure"))
            if (const float *f = v->as_float()) exposure = *f;
          if (const lightusd::next::Value *v =
                  job.prim.GetPropertyValue("inputs:color"))
            if (const float *f = v->as_float3())
              light_color = Vec3{f[0], f[1], f[2]};
          if (const lightusd::next::Value *v =
                  job.prim.GetPropertyValue("inputs:normalize"))
            if (const bool *b = v->as_bool()) normalize = *b;
          float area = 0.0f;
          for (size_t ti = 0; ti + 2u < geo.indices.size(); ti += 3u) {
            const uint32_t ia = geo.indices[ti], ib = geo.indices[ti + 1u],
                           ic = geo.indices[ti + 2u];
            if (size_t(std::max({ia, ib, ic})) * 3u + 2u >=
                geo.positions.size()) continue;
            const Vec3 a{geo.positions[ia * 3u], geo.positions[ia * 3u + 1u],
                         geo.positions[ia * 3u + 2u]};
            const Vec3 b{geo.positions[ib * 3u], geo.positions[ib * 3u + 1u],
                         geo.positions[ib * 3u + 2u]};
            const Vec3 c{geo.positions[ic * 3u], geo.positions[ic * 3u + 1u],
                         geo.positions[ic * 3u + 2u]};
            area += 0.5f * Length(Cross(Sub(b, a), Sub(c, a)));
          }
          const Vec3 tint = Luminance(resolved.emission) > 1.0e-6f
                                ? resolved.emission : resolved.base_color;
          float gain = intensity * std::pow(2.0f, exposure);
          if (normalize && area > 1.0e-8f) gain /= area;
          resolved.emission = Vec3{light_color.x * tint.x * gain,
                                   light_color.y * tint.y * gain,
                                   light_color.z * tint.z * gain};
          resolved.area_light = true;
          if (resolved.has_openpbr) {
            resolved.openpbr.emission = 1.0f;
            resolved.openpbr.emissionColor[0] = resolved.emission.x;
            resolved.openpbr.emissionColor[1] = resolved.emission.y;
            resolved.openpbr.emissionColor[2] = resolved.emission.z;
          }
          break;
        }
        gpu_materials.push_back(std::move(resolved));
        geos.push_back(std::move(geo));
      }
    }
    }

    bool has_direct_curves = false;
    bool has_flat_curves = false;
    Bounds gpu_flat_bounds;
    RenderContext gpu_curve_ctx;
    std::vector<CurveJobNext> gpu_curve_jobs;
    stage.Traverse([&](const lightusd::next::UsdPrim &prim) {
      const std::string &type = prim.GetTypeName();
      if (type == "BasisCurves" || type == "HermiteCurves" ||
          type == "NurbsCurves") {
        has_direct_curves = true;
        return false;
      }
      return true;
    });

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
    const bool gpu_backend = opt.vulkan || opt.use_d3d || opt.hip || opt.cuda;
    if (gpu_backend && has_direct_curves) {
      // LightRT's Vulkan API does not yet expose the CUDA analytic curve
      // primitives. Build the same direct round-linear scene used by the next
      // CPU path, then tessellate it once at the Vulkan upload boundary. This
      // keeps curve-only and mixed scenes on the Vulkan renderer while retaining
      // the authored/value-clip curve conversion path.
      gpu_curve_ctx.opt = opt;
      gpu_curve_ctx.clip_stage_loader = clip_stage_loader;
      for (const auto &root : stage.GetRootPrims()) {
        CollectCurvesNextRec(root, matrix4d::identity(),
                             lightusd::Purpose::Default, opt.timecode,
                             &gpu_curve_jobs);
      }
      if (!BuildNextCurves(gpu_curve_ctx, gpu_curve_jobs, opt.timecode,
                           /*include_flat=*/false) ||
          !AppendGpuRoundCurves(gpu_curve_ctx.direct, &base_colors, &geos)) {
        return EXIT_FAILURE;
      }
      for (const CurveJobNext &job : gpu_curve_jobs)
        has_flat_curves |= job.prim.GetPropertyValue("normals") != nullptr;
      if (has_flat_curves &&
          !BuildNextFlatCurveBounds(gpu_curve_jobs, opt.timecode,
                                    clip_stage_loader, &gpu_flat_bounds)) {
        std::cerr << "Failed to read GPU flat/ribbon curve bounds.\n";
        return EXIT_FAILURE;
      }
    }
    // Vulkan uses the native analytic ellipse path for a pure splat scene.
    // Mixed mesh+splat scenes use the same bounded oriented-ellipse mesh
    // fallback as HIP/ROCm and D3D11 so all geometry reaches one flat trace.
    if (gpu_backend && (opt.vulkan || opt.hip || opt.cuda) && geos.empty()) {
      stage.Traverse([&](const lightusd::next::UsdPrim &prim) {
        const std::string &type = prim.GetTypeName();
        if (type == "Points" || type == "BasisCurves" ||
            type == "HermiteCurves" || type == "NurbsCurves") {
          has_other_native_carrier = true;
          return false;
        }
        return true;
      });
    }
    if (gpu_backend && (opt.vulkan || opt.hip || opt.cuda) && geos.empty() &&
        !has_other_native_carrier) {
      native_gaussian =
          BuildNextGaussianEllipses(stage, gaussian_ctx, opt.timecode,
                                    /*defer_gpu_bvh=*/true) &&
          gaussian_ctx.direct.has_ellipses();
    }
    if (gpu_backend && !native_gaussian) {
      for (const auto &root : stage.GetRootPrims()) {
        CollectGpuPointsRec(stage, root, matrix4d::identity(), opt.timecode,
                            &base_colors, &geos, 0);
      }
    }
#endif

    if (geos.empty() && !native_gaussian && !gpu_flat_bounds.valid) {
      std::cerr << "WARN: No renderable geometry found; writing blank image.\n";
      return WriteBlankImage(opt, opt.height > 0 ? opt.height : 540)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }

    // Resolve camera.
    CameraFrame camera;
    double up_axis = 1.0; // Y-up
    {
      std::string up = stage.GetUpAxis();
      if (up == "Z") up_axis = 2.0;
      else if (up == "X") up_axis = 0.0;
    }
    lightusd::Axis usdUp = (up_axis == 2.0) ? lightusd::Axis::Z
                           : (up_axis == 0.0) ? lightusd::Axis::X
                           : lightusd::Axis::Y;

    Bounds bounds = gpu_flat_bounds;
    for (const auto &g : geos) {
      for (size_t j = 0; j < g.positions.size() / 3; ++j) {
        bounds.lo.x = std::min(bounds.lo.x, g.positions[j * 3 + 0]);
        bounds.lo.y = std::min(bounds.lo.y, g.positions[j * 3 + 1]);
        bounds.lo.z = std::min(bounds.lo.z, g.positions[j * 3 + 2]);
        bounds.hi.x = std::max(bounds.hi.x, g.positions[j * 3 + 0]);
        bounds.hi.y = std::max(bounds.hi.y, g.positions[j * 3 + 1]);
        bounds.hi.z = std::max(bounds.hi.z, g.positions[j * 3 + 2]);
      }
    }
    if (native_gaussian && gaussian_ctx.bounds.valid) bounds = gaussian_ctx.bounds;
    bounds.valid = true;
    // Resolve the camera the SAME way the CPU -rtPreview path does
    // (ResolveCameraNext): a named camera, else the USD record camera for
    // -autoframe, else auto-fit. This keeps -vk/-vkr framed identically to the
    // -rtPreview reference (the GPU backends previously used the tilted auto-fit
    // camera here, so the same scene framed differently from the CPU image).
    const int cam_width = opt.width > 0 ? opt.width : 960;
    int out_height = opt.height;
    Options auto_opt = opt;
    auto_opt.camera.clear();
    auto_opt.width = cam_width;
    if (!opt.camera.empty()) {
      float cam_aspect = 16.0f / 9.0f;
      if (FindNextCameraFrame(stage, opt.camera, opt.timecode, &camera,
                              &cam_aspect)) {
        if (out_height <= 0) {
          double dh = double(cam_width) / double(cam_aspect);
          if (!std::isfinite(dh)) dh = 540.0;
          out_height = std::max(1, int(std::lround(std::min(32768.0, dh))));
        }
      } else {
        std::cerr << "WARN: camera not found: " << opt.camera
                  << ". Using auto-fit.\n";
        if (out_height <= 0) out_height = 540;
        camera = MakeCameraFrame({}, auto_opt, bounds, out_height, usdUp);
      }
    } else if (opt.autoframe) {
      camera = MakeUsdRecordCamera(bounds, usdUp, cam_width, &out_height);
    } else {
      if (out_height <= 0) out_height = 540;
      camera = MakeCameraFrame({}, auto_opt, bounds, out_height, usdUp);
    }

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
    if ((opt.vulkan || opt.use_d3d || opt.hip || opt.cuda) && has_flat_curves) {
      if (!BuildNextFlatCurveMeshes(gpu_curve_jobs, opt.timecode,
                                    clip_stage_loader, camera, &geos,
                                    &base_colors)) {
        std::cerr << "Failed to build GPU flat/ribbon curve carriers.\n";
        return EXIT_FAILURE;
      }
      if (opt.stats)
        std::cerr << "GPU flat/ribbon curves: camera-facing triangle carrier\n";
    }
#endif

#if defined(HAVE_VULKAN)
    if (opt.vulkan && native_gaussian && geos.empty()) {
      return RunVulkanGaussianLightRT(opt, &gaussian_ctx.direct, camera,
                                      out_height)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }
#endif
#if defined(HAVE_HIP)
    if (opt.hip && native_gaussian && geos.empty()) {
      return RunHipGaussianLightRT(opt, &gaussian_ctx.direct, camera,
                                   out_height)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }
#endif

    // Flatten-side view-dependent LOD for the GPU backends (-vk/-vkr/-d3d/-hip).
    // LightRT's Vulkan/D3D paths build a single flat world-space BLAS (no GPU
    // TLAS / per-prototype instancing), so there is no two-level structure to do
    // per-instance Full/Proxy/Cull on the GPU. Instead we apply the SAME
    // lusdr_rt_lod classifier here, once, on the already-world-space `geos`:
    //   Cull  -> drop the placement from the flat soup (fewer triangles to trace)
    //   Proxy -> replace its triangles with an axis-aligned box on its world AABB
    //   Full  -> keep the real triangles
    // Each `geos[i]` is one world-space mesh placement, so its world AABB is the
    // classifier input (identity o2w + the world AABB as the "prototype" bounds).
    // Opt-in via -rtLod; byte-identical to before when off. Frustum cull stays
    // OFF by default (a path tracer needs off-screen geo for shadows/GI).
    if (opt.rt_lod && !geos.empty()) {
      lusdr::RtLodConfig cfg;
      cfg.enabled = true;
      cfg.proxy = opt.rt_lod_proxy;
      cfg.frustum_cull = opt.rt_lod_frustum_cull;
      cfg.full_px = opt.rt_lod_full_px;
      cfg.cull_px = opt.rt_lod_cull_px;
      const lusdr::RtLodView view = lusdr::MakeRtLodView(camera, out_height);
      const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      // Unit-cube corners + 12 triangles (36 indices), CCW outward.
      static const float kC[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                     {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
      static const uint32_t kI[36] = {
          0, 2, 1, 0, 3, 2,  // -Z
          4, 5, 6, 4, 6, 7,  // +Z
          0, 1, 5, 0, 5, 4,  // -Y
          3, 6, 2, 3, 7, 6,  // +Y
          0, 4, 7, 0, 7, 3,  // -X
          1, 2, 6, 1, 6, 5}; // +X
      lusdr::RtLodStats lod_stats;
      std::vector<RTPreviewStats::MeshGeometry> kept_geos;
      std::vector<Vec3> kept_colors;
      std::vector<ResolvedMat> kept_materials;
      kept_geos.reserve(geos.size());
      kept_colors.reserve(geos.size());
      kept_materials.reserve(gpu_materials.size());
      for (size_t i = 0; i < geos.size(); ++i) {
        RTPreviewStats::MeshGeometry &g = geos[i];
        const size_t nv = g.positions.size() / 3;
        if (nv == 0) continue;
        // World AABB of this placement (positions are already world-space).
        lusdr::Bounds wb;
        for (size_t j = 0; j < nv; ++j) {
          const float x = g.positions[j * 3 + 0], y = g.positions[j * 3 + 1],
                      z = g.positions[j * 3 + 2];
          wb.lo.x = std::min(wb.lo.x, x); wb.lo.y = std::min(wb.lo.y, y); wb.lo.z = std::min(wb.lo.z, z);
          wb.hi.x = std::max(wb.hi.x, x); wb.hi.y = std::max(wb.hi.y, y); wb.hi.z = std::max(wb.hi.z, z);
        }
        wb.valid = true;
        const lusdr::RtLod lod =
            lusdr::ClassifyInstance(view, cfg, kIdentity, wb);
        if (lod == lusdr::RtLod::Cull) {
          lod_stats.culled++;
          continue;
        }
        if (lod == lusdr::RtLod::Proxy) {
          lod_stats.proxy++;
          // Rebuild this placement as an axis-aligned box on its world AABB.
          float fit[12];
          lusdr::BoxFitO2W(kIdentity, wb.lo, wb.hi, fit);
          RTPreviewStats::MeshGeometry box;
          box.positions.resize(8 * 3);
          box.normals.resize(8 * 3, 0.0f);  // geometric normals recomputed downstream
          box.uvs.resize(8 * 2, 0.0f);
          for (int c = 0; c < 8; ++c) {
            const Vec3 p = lusdr::TransformPointO2W(
                fit, Vec3{kC[c][0], kC[c][1], kC[c][2]});
            box.positions[c * 3 + 0] = p.x;
            box.positions[c * 3 + 1] = p.y;
            box.positions[c * 3 + 2] = p.z;
          }
          box.indices.assign(kI, kI + 36);
          kept_geos.push_back(std::move(box));
          kept_colors.push_back(base_colors[i]);
          if (i < gpu_materials.size())
            kept_materials.push_back(std::move(gpu_materials[i]));
          continue;
        }
        lod_stats.full++;
        kept_geos.push_back(std::move(g));
        kept_colors.push_back(base_colors[i]);
        if (i < gpu_materials.size())
          kept_materials.push_back(std::move(gpu_materials[i]));
      }
      geos.swap(kept_geos);
      base_colors.swap(kept_colors);
      gpu_materials.swap(kept_materials);
      if (opt.stats) {
        std::cerr << "[rt-lod] flatten-side: full=" << lod_stats.full
                  << " proxy=" << lod_stats.proxy
                  << " culled=" << lod_stats.culled
                  << " (placements=" << kept_geos.size() << "->" << geos.size()
                  << ")\n";
      }
      if (geos.empty()) {
        std::cerr << "No renderable geometry after -rtLod culling "
                     "(try a smaller -rtLodCullPx).\n";
        return EXIT_FAILURE;
      }
    }

#ifdef HAVE_D3D11
    if (opt.use_d3d) {
      if (!RunD3D11LightRT(opt, base_colors, geos, camera, out_height)) {
        return EXIT_FAILURE;
      }
      return EXIT_SUCCESS;
    }
#endif
#ifdef HAVE_VULKAN
    if (opt.vulkan) {
      if (!RunVulkanLightRT(opt, base_colors, geos, gpu_materials,
                            gpu_textures, gpu_lights, camera, out_height)) {
        return EXIT_FAILURE;
      }
      return EXIT_SUCCESS;
    }
#endif
#ifdef HAVE_HIP
    if (opt.hip) {
      const bool cpu_shade =
          opt.gpu_shade == Options::GpuShadeMode::Cpu && !opt.path_trace;
      const bool ok = cpu_shade
                          ? RunHipLightRT(opt, base_colors, geos, camera,
                                          out_height)
                          : RunHipSharedRT(opt, base_colors, geos, gpu_materials,
                                           gpu_textures, gpu_lights, camera,
                                           out_height);
      if (!ok) {
        return EXIT_FAILURE;
      }
      return EXIT_SUCCESS;
    }
#endif
#ifdef HAVE_CUDA_RT
    if (opt.cuda) {
      if (!RunCudaSharedRT(opt, base_colors, geos, gpu_materials,
                           gpu_textures, gpu_lights, camera, out_height)) {
        return EXIT_FAILURE;
      }
      return EXIT_SUCCESS;
    }
#endif
    std::cerr << "Requested GPU backend not built in.\n";
    return EXIT_FAILURE;
  }
#endif

  lightusd::Stage stage;
  std::string warn;
  std::string err;
  lightusd::USDLoadOptions load_options;
  load_options.mmap_zero_copy = opt.rt_preview;
  load_options.max_memory_limit_in_mb = opt.rt_preview ? 65536 : 16384;
  load_options.load_assets = !opt.rt_preview;
  if (opt.progress) {
    load_options.progress_callback = LoadProgress;
  }
  const auto load_t0 = std::chrono::steady_clock::now();
  // Composes references / payloads / sublayers / inherits / variants; plain
  // LoadUSDFromFile expands no arcs, so anything they contribute (a Material in a
  // referenced look layer, payload-gated geometry) was missing from the render.
  if (!LoadStageComposedLegacy(opt.input, load_options, &stage, &warn, &err)) {
    if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";
    std::cerr << "Failed to load USD: " << err << "\n";
    return EXIT_FAILURE;
  }
  const auto load_t1 = std::chrono::steady_clock::now();
  const double load_seconds =
      std::chrono::duration<double>(load_t1 - load_t0).count();
  if (!warn.empty()) {
    std::cerr << "WARN: " << warn << "\n";
  }

  if (opt.rt_preview) {
    if (lightusd::IsUSDC(opt.input) && !stage.has_mmap_zero_copy()) {
      std::cerr << "RT preview requires mmap zero-copy metadata for USDC input. "
                << "Write flattened USDC without --compress-float-arrays.\n";
      return EXIT_FAILURE;
    }

    std::vector<float> vertices;
    std::vector<TriInfo> tris;
    Bounds bounds;
    RTPreviewStats rt_stats;
    std::string rt_err;
    if (!BuildRTPreviewScene(stage, opt, &vertices, &tris, &bounds, &rt_stats,
                             &rt_err)) {
      if (opt.stats) {
        std::cerr << "rt preview: 1\n";
        std::cerr << "rt meshes: " << rt_stats.meshes << "\n";
        std::cerr << "rt skipped meshes: " << rt_stats.skipped_meshes << "\n";
        std::cerr << "rt mmap point meshes: "
                  << rt_stats.meshes_with_mmap_points << "\n";
        std::cerr << "rt owned point meshes: "
                  << rt_stats.meshes_with_owned_points << "\n";
        std::cerr << "rt mmap deferred bytes: "
                  << rt_stats.mmap_deferred_bytes << "\n";
        std::cerr << "rt copied point bytes: " << rt_stats.copied_point_bytes
                  << "\n";
        std::cerr << "rt copied topology bytes: "
                  << rt_stats.copied_topology_bytes << "\n";
        std::cerr << "rt purpose default triangles: "
                  << rt_stats.purpose_default_triangles << "\n";
        std::cerr << "rt purpose render triangles: "
                  << rt_stats.purpose_render_triangles << "\n";
        std::cerr << "rt purpose proxy triangles: "
                  << rt_stats.purpose_proxy_triangles << "\n";
        std::cerr << "rt purpose guide triangles: "
                  << rt_stats.purpose_guide_triangles << "\n";
      }
      std::cerr << rt_err << "\n";
      return EXIT_FAILURE;
    }
    if (tris.empty()) {
      std::cerr << "WARN: No renderable geometry found; writing blank image.\n";
      return WriteBlankImage(opt, opt.height > 0 ? opt.height : 540)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }

    lrt_tri_build_options build_opts;
    std::memset(&build_opts, 0, sizeof(build_opts));
    build_opts.quality = opt.quality;
    build_opts.layout = LRT_TRI_LAYOUT_AUTO;
    build_opts.max_leaf_size = 0;
    build_opts.num_threads = WorkerThreadCount(opt.threads);

    const auto bvh_t0 = std::chrono::steady_clock::now();
    lrt_result lrt_err = LRT_RESULT_OK;
    lrt_tri_scene *lrt_scene =
        lrt_tri_scene_build(vertices.data(), tris.size(), &build_opts, &lrt_err);
    const auto bvh_t1 = std::chrono::steady_clock::now();
    if (!lrt_scene) {
      std::cerr << "Failed to build LightRT scene (err=" << int(lrt_err) << ").\n";
      return EXIT_FAILURE;
    }

    int height = opt.height > 0 ? opt.height : 540;
    RenderScene empty_render_scene;
    CameraFrame camera;
    if (!FindStageCameraFrame(stage, opt.camera, opt.timecode, &camera)) {
      if (!opt.camera.empty()) {
        std::cerr << "WARN: Camera not found: " << opt.camera
                  << ". Using auto-fit camera.\n";
      }
      Options auto_opt = opt;
      auto_opt.camera.clear();
      camera = MakeCameraFrame(empty_render_scene, auto_opt, bounds, height,
                               stage.metas().upAxis.get_value());
    }
    DirectScene direct_scene;
    LightCache light_cache;
    IblCache ibl_cache;

    if (opt.stats) {
      lrt_tri_stats st;
      std::memset(&st, 0, sizeof(st));
      lrt_tri_scene_stats(lrt_scene, &st);
      double bvh_seconds =
          std::chrono::duration<double>(bvh_t1 - bvh_t0).count();
      std::cerr << "rt preview: 1\n";
      std::cerr << "rt meshes: " << rt_stats.meshes << "\n";
      std::cerr << "rt skipped meshes: " << rt_stats.skipped_meshes << "\n";
      std::cerr << "rt mmap point meshes: "
                << rt_stats.meshes_with_mmap_points << "\n";
      std::cerr << "rt owned point meshes: "
                << rt_stats.meshes_with_owned_points << "\n";
      std::cerr << "rt mmap deferred bytes: "
                << rt_stats.mmap_deferred_bytes << "\n";
      std::cerr << "rt copied point bytes: " << rt_stats.copied_point_bytes
                << "\n";
      std::cerr << "rt copied topology bytes: "
                << rt_stats.copied_topology_bytes << "\n";
      std::cerr << "rt purpose default triangles: "
                << rt_stats.purpose_default_triangles << "\n";
      std::cerr << "rt purpose render triangles: "
                << rt_stats.purpose_render_triangles << "\n";
      std::cerr << "rt purpose proxy triangles: "
                << rt_stats.purpose_proxy_triangles << "\n";
      std::cerr << "rt purpose guide triangles: "
                << rt_stats.purpose_guide_triangles << "\n";
      std::cerr << "rt packed triangle bytes: "
                << rt_stats.packed_triangle_bytes << "\n";
      std::cerr << "load seconds: " << load_seconds << "\n";
      std::cerr << "rt triangle stream seconds: " << rt_stats.build_seconds
                << "\n";
      std::cerr << "rt bvh build seconds: " << bvh_seconds << "\n";
      std::cerr << "triangles: " << tris.size() << "\n";
      std::cerr << "lightrt: " << lrt_tri_kernel_name(lrt_scene) << "\n";
      std::cerr << "bvh nodes: " << st.node_count << ", leaves: "
                << st.leaf_count << ", memory: " << st.memory_bytes
                << " bytes\n";
    }

    const auto render_t0 = std::chrono::steady_clock::now();
    std::vector<FlatTri> flat_tris;
    std::vector<TriMat> flat_mats;
    SplitTriInfos(tris, &flat_tris, &flat_mats);
    lightusd::Image img =
        RenderImage(lrt_scene, &direct_scene, flat_tris, flat_mats, light_cache,
                    nullptr, camera, opt, height);
    const auto render_t1 = std::chrono::steady_clock::now();
    if (opt.stats) {
      std::cerr << "render seconds: "
                << std::chrono::duration<double>(render_t1 - render_t0).count()
                << "\n";
    }
    lrt_tri_scene_free(lrt_scene);

    lightusd::image::WriteOption wopt;
    wopt.format = lightusd::image::WriteImageFormat::Autodetect;
    auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
    if (!ret) {
      std::cerr << "Failed to write image: " << ret.error() << "\n";
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  lightusd::tydra::RenderScene render_scene;
  lightusd::tydra::RenderSceneConverter converter;
  lightusd::tydra::RenderSceneConverterEnv env(stage);
  env.timecode = opt.timecode;
  env.mesh_config.triangulate = !opt.direct_prims;
  env.mesh_config.subdivision_level = opt.subdivision_level;
  env.mesh_config.build_vertex_indices = !opt.direct_prims;
  env.mesh_config.compute_tangents_and_binormals = false;
  env.scene_config.load_texture_assets = true;
  env.set_search_paths({lightusd::io::GetBaseDir(opt.input)});
  if (opt.no_assetresolver) {
    SetupNullAssetResolution(&env.asset_resolver);
  }
  if (!converter.ConvertToRenderScene(env, &render_scene)) {
    std::cerr << "Failed to convert USD Stage to RenderScene:\n"
              << converter.GetError() << "\n";
    return EXIT_FAILURE;
  }
  std::string converter_warn = converter.GetWarning();
  if (!converter_warn.empty()) {
    std::cerr << "WARN: " << converter_warn << "\n";
  }

  std::vector<float> vertices;
  std::vector<TriInfo> tris;
  Bounds bounds;
  DirectScene direct_scene;
  LightCache light_cache;
  if (opt.direct_prims) {
    std::string direct_err;
    if (!BuildDirectScene(stage, render_scene, opt, &vertices, &tris, &bounds,
                          &direct_scene, &direct_err)) {
      std::cerr << direct_err << "\n";
      return EXIT_FAILURE;
    }
  }
  // Decode the materials' UsdUVTextures (tydra already resolved them) and bind
  // them per triangle. Without this the legacy path flattens every material to a
  // constant base color, so .usda/.usdz — which do not route to the `next` path —
  // render untextured.
  std::vector<lusdr::Texture> legacy_textures;
  const std::vector<LegacyMaterialTex> legacy_mat_tex =
      BuildLegacyTextures(render_scene, &legacy_textures);
  std::vector<float> tri_uvs;
  const bool want_uvs = !legacy_textures.empty();
  if (want_uvs) {
    // tri_uvs must stay parallel to `tris`; any triangles the direct-primitive
    // builder already emitted carry no UVs (and no texture), so pad them.
    tri_uvs.assign(tris.size() * 6, 0.0f);
  }
  // Inherited purpose + visibility from the source Stage (the tydra
  // RenderScene carries neither): guide/proxy filtering (-purpose et al.) and
  // visibility="invisible" now apply on the legacy path like the next path.
  PurposeVisibilityMap purpose_vis;
  BuildLegacyPurposeVisibility(stage, &purpose_vis);
  CollectAllGeometry(render_scene, &vertices, &tris, &bounds,
                     opt.direct_prims ? &direct_scene.direct_paths : nullptr,
                     &light_cache, want_uvs ? &tri_uvs : nullptr,
                     want_uvs ? &legacy_mat_tex : nullptr, &purpose_vis);
    const bool has_direct = direct_scene.spheres ||
                          direct_scene.has_round_curves() ||
                          direct_scene.has_flat_curves() || direct_scene.points ||
                          direct_scene.bez_curves || direct_scene.has_ellipses() ||
                          direct_scene.tets ||
                          !direct_scene.shapes.empty();

  // UsdVol volumes (OpenVDB) -> dense grids for raymarching. Built here so a
  // volume-only scene still renders and contributes to camera-framing bounds.
  std::vector<VolumeData> volumes = BuildVolumes(render_scene);
  ExpandBoundsByVolume(volumes, &bounds);

  if (tris.empty() && !has_direct && volumes.empty()) {
    std::cerr << "WARN: No renderable geometry found; writing blank image.\n";
    return WriteBlankImage(opt, opt.height > 0 ? opt.height : 540)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }

  lrt_tri_build_options build_opts;
  std::memset(&build_opts, 0, sizeof(build_opts));
  build_opts.quality = opt.quality;
  build_opts.layout = LRT_TRI_LAYOUT_AUTO;
  build_opts.max_leaf_size = 0;
  build_opts.num_threads = WorkerThreadCount(opt.threads);

  lrt_result lrt_err = LRT_RESULT_OK;
  lrt_tri_scene *lrt_scene = nullptr;
  if (!tris.empty()) {
    lrt_scene = lrt_tri_scene_build(vertices.data(), tris.size(), &build_opts, &lrt_err);
    if (!lrt_scene) {
      std::cerr << "Failed to build LightRT scene (err=" << int(lrt_err) << ").\n";
      return EXIT_FAILURE;
    }
  }

  int height = opt.height;
  if (height <= 0) {
    height = 540;
    const Node *cam_node = FindCameraNode(render_scene, opt.camera);
    if (cam_node) {
      const RenderCamera &cam = render_scene.cameras[size_t(cam_node->id)];
      if (cam.verticalAspectRatio > 0.0f) {
        double dh = double(opt.width) * double(cam.verticalAspectRatio);
        if (!std::isfinite(dh)) dh = 540.0;
        height = std::max(1, int(std::lround(std::min(32768.0, dh))));
      }
    }
  }

  lightusd::Axis up_axis = GetUpAxis(render_scene.meta.upAxis);
  CameraFrame camera = MakeCameraFrame(render_scene, opt, bounds, height,
                                       up_axis);
  CollectLights(render_scene, &light_cache);
  IblCache ibl_cache;
  BuildIblCache(render_scene, light_cache, &ibl_cache);

  if (opt.stats) {
    lrt_tri_stats st;
    std::memset(&st, 0, sizeof(st));
    if (lrt_scene) lrt_tri_scene_stats(lrt_scene, &st);
    std::cerr << "triangles: " << tris.size() << "\n";
    std::cerr << "direct spheres: " << direct_scene.sphere_info.size() << "\n";
    std::cerr << "direct round curve segments: "
              << direct_scene.round_curve_info.size() << "\n";
    std::cerr << "direct flat curve segments: "
              << direct_scene.flat_curve_info.size() << "\n";
    std::cerr << "direct Hermite/Bezier curve segments: "
              << direct_scene.bez_curve_info.size() << "\n";
    std::cerr << "direct points: " << direct_scene.point_info.size() << "\n";
    std::cerr << "direct tetrahedra: " << direct_scene.tet_prims.size()
              << "\n";
    std::cerr << "direct analytic shapes: " << direct_scene.shapes.size()
              << "\n";
    std::cerr << "subdivision level: " << opt.subdivision_level << "\n";
    std::cerr << "lights: " << light_cache.finite.size() << "\n";
    std::cerr << "mesh light triangles: " << light_cache.mesh.size() << "\n";
    std::cerr << "domelight: " << (light_cache.has_dome ? 1 : 0) << "\n";
    std::cerr << "light sampling finite cdf entries: "
              << light_cache.finite_cdf.size() << "\n";
    std::cerr << "light sampling mesh cdf entries: "
              << light_cache.mesh_cdf.size() << "\n";
    std::cerr << "light sampling env cdf entries: "
              << light_cache.env_cdf.size() << "\n";
    std::cerr << "ibl envmap: " << (ibl_cache.valid ? 1 : 0) << "\n";
    std::cerr << "ibl diffuse size: "
              << (ibl_cache.diffuse.width * ibl_cache.diffuse.height) << "\n";
    std::cerr << "ibl prefilter levels: " << ibl_cache.prefiltered.size()
              << "\n";
    std::cerr << "ibl brdf lut size: "
              << (ibl_cache.brdf_size * ibl_cache.brdf_size) << "\n";
    if (lrt_scene) std::cerr << "lightrt: " << lrt_tri_kernel_name(lrt_scene) << "\n";
    std::cerr << "bvh nodes: " << st.node_count << ", leaves: " << st.leaf_count
              << ", memory: " << st.memory_bytes << " bytes\n";
    std::cerr << "load seconds: " << load_seconds << "\n";
  }

  if (opt.stats) {
    std::cerr << "volumes: " << volumes.size() << "\n";
  }

  const auto render_t0 = std::chrono::steady_clock::now();
  std::vector<FlatTri> flat_tris;
  std::vector<TriMat> flat_mats;
  SplitTriInfos(tris, &flat_tris, &flat_mats);
  lightusd::Image img =
      RenderImage(lrt_scene, &direct_scene, flat_tris, flat_mats, light_cache,
                  ibl_cache.valid ? &ibl_cache : nullptr, camera, opt, height,
                  legacy_textures.empty() ? nullptr : &legacy_textures,
                  tri_uvs.empty() ? nullptr : &tri_uvs, /*tlas*/ nullptr,
                  /*blas*/ nullptr, /*instances*/ nullptr, /*tri_colors*/ nullptr,
                  /*tri_normals*/ nullptr, &volumes);
  const auto render_t1 = std::chrono::steady_clock::now();
  if (opt.stats) {
    std::cerr << "render seconds: "
              << std::chrono::duration<double>(render_t1 - render_t0).count()
              << "\n";
  }
  if (lrt_scene) lrt_tri_scene_free(lrt_scene);

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
