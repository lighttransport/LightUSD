// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment, Inc.
//
// lusdrender: CPU preview raytrace renderer for USD scenes.
//
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>  // ProcessRSS()/AvailableSystemMemory() use std::ifstream
#include <iomanip>

#include "preview_gpu_budget.hh"
#include "tydra/scene-preview-compute.hh"
#include "tydra/scene-preview.hh"
#include "tydra/render-converter.hh"
#include "tydra/materialx.hh"
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
#include <map>
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

#include "image-loader.hh"
#include "image-writer.hh"
#include "io-util.hh"
#include "core/mtlx-dom.hh"
#include "tsd/tinysubdiv.hh"
#include "tydra/render-data.hh"
#include "usdVol.hh"  // OpenVDB (.vdb) loader for the `core` volume path

// Experimental `core` lazy loader: fast, low-memory USDC parse used as the
// default backend for the RT preview path. tydra provides bit-exact
// world transforms (see src/tydra/scene-access.cc).
#include "layer/prim-spec.hh"
#include "pcp/prim-index.hh"
#include "prim/path.hh"
#include "reader/usdz-reader.hh"
#include "schema/geom-mesh.hh"
#include "stage/stage.hh"
#include "lightusd-core.hh"
#include "types/value.hh"
#include "../../src/tydra/scene-access.hh"
#include "../../examples/lusdview/lightrt_mtlx_bridge.hh"

extern "C" {
#include "lightrt_c_tri.h"
}

#ifdef LIGHTUSD_WITH_QJS
#include <fstream>
#include <sstream>

#include "external/jsonhpp/nlohmann/json.hpp"
extern "C" {
#include "external/quickjs-ng/quickjs.h"
}
#endif

#include "lusdr_math.hh"
#include "lusdr_types.hh"
#include "lusdr_context.hh"
#include "lusdr_generated.hh"
#include "lusdr_rt_lod.hh"
#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
#include "lusdr_gpu_common.hh"  // GpuInstancedScene, RunVulkanLightRTInstanced
#endif


// The Vulkan backend and main() below live in the global namespace; pull in the
// lusdr names they use (Vec3, Options, RTPreviewStats, qjs::*, ...).
using namespace lusdr;

namespace {

struct GeneratedMtlxScene {
  std::string path;
  ~GeneratedMtlxScene() {
    if (!path.empty()) std::remove(path.c_str());
  }
};

bool BuildMtlxValidationScene(Options &opt, GeneratedMtlxScene *generated,
                              std::string *error) {
  if (!generated || opt.mtlx_file.empty()) return false;
  lightusd::mtlx::MtlxDocument resolved_document;
  if (!resolved_document.ParseFromFile(opt.mtlx_file)) {
    if (error) *error = "MaterialX resolution failed: " +
                        resolved_document.GetError();
    return false;
  }
  std::vector<std::string> graph_order;
  std::string graph_error;
  if (!resolved_document.ResolveNodeGraphs(&graph_order, &graph_error)) {
    if (error) *error = "MaterialX graph validation failed: " + graph_error;
    return false;
  }
  const std::string xml_text = resolved_document.GetResolvedXML();
  if (xml_text.empty()) {
    if (error) *error = "MaterialX resolution produced an empty document";
    return false;
  }
  const size_t slash = opt.mtlx_file.find_last_of("/\\");
  const std::string base_dir = slash == std::string::npos
                                   ? "."
                                   : opt.mtlx_file.substr(0, slash);
  std::string runtime_error;
  const uint64_t texture_budget = opt.texture_budget_mb > 0
      ? uint64_t(opt.texture_budget_mb) * 1024u * 1024u : 0;
  const size_t texture_budget_bytes = static_cast<size_t>((std::min)(
      texture_budget, static_cast<uint64_t>((std::numeric_limits<size_t>::max)())));
  opt.mtlx_runtime = lusdview::CreateMaterialXSurfaceRuntime(
      xml_text.c_str(),
      opt.mtlx_material.empty() ? nullptr : opt.mtlx_material.c_str(),
      base_dir.c_str(), &runtime_error, opt.mtlx_mipmaps, texture_budget_bytes);
  if (!opt.mtlx_runtime) {
    if (error) *error = "LightRT MaterialX runtime creation failed: " +
                        runtime_error;
    return false;
  }
  if (opt.aov == CaptureAov::MaterialXNode) {
    lusdview::MaterialXSurfaceContext context;
    lusdview::MaterialXNodeValue value;
    if (!lusdview::EvaluateMaterialXNodeRuntimeAt(
            opt.mtlx_runtime.get(), context, opt.mtlx_aov_node.c_str(), &value,
            &runtime_error)) {
      if (error) *error = "MaterialX node AOV is invalid: " + runtime_error;
      return false;
    }
  }
  lightusd::tydra::LightRtOpenPBRParams pbr;
  std::string eval_error;
  lusdview::MaterialXSurfaceContext initial_context;
  initial_context.uv[0] = initial_context.uv[1] = 0.5f;
  if (!lusdview::EvaluateMaterialXSurfaceRuntimeAt(
          opt.mtlx_runtime.get(), initial_context, &pbr, &eval_error)) {
    if (error) *error = "LightRT MaterialX graph evaluation failed: " + eval_error;
    return false;
  }
  auto scalar = [](float v) {
    std::ostringstream out;
    out << std::setprecision(9) << (std::isfinite(v) ? v : 0.0f);
    return out.str();
  };
  auto color = [&](const float p[3]) {
    return "(" + scalar(p[0]) + ", " + scalar(p[1]) + ", " +
           scalar(p[2]) + ")";
  };

  static std::atomic<uint64_t> sequence{0};
  const std::string temp_dir = lightusd::io::GetTempDir(opt.temp_directory,
                                                        opt.host_services);
  generated->path = temp_dir + "/lusdr_mtlx_validation_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
      "_" + std::to_string(sequence.fetch_add(1)) + ".usda";
  std::ofstream out(generated->path, std::ios::out | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write temporary MaterialX validation scene: " + generated->path;
    generated->path.clear();
    return false;
  }
  out << "#usda 1.0\n(\n    defaultPrim = \"World\"\n"
         "    metersPerUnit = 1\n    upAxis = \"Y\"\n)\n\n"
         "def Xform \"World\" {\n"
         "    def Sphere \"MaterialSphere\" (\n"
         "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
         "    ) {\n"
         "        double radius = 1\n"
      << (opt.path_trace ? "        uniform bool doubleSided = true\n" : "")
      << "        rel material:binding = </World/Material>\n"
         "    }\n"
         "    def Material \"Material\" {\n"
         "        token outputs:surface.connect = </World/Material/Surface.outputs:surface>\n"
         "        def Shader \"Surface\" {\n"
         "            uniform token info:id = \"ND_open_pbr_surface_surfaceshader\"\n"
      << "            float inputs:base_weight = " << scalar(pbr.baseWeight) << "\n"
      << "            color3f inputs:base_color = " << color(pbr.baseColor) << "\n"
      << "            float inputs:base_roughness = " << scalar(pbr.diffuseRoughness) << "\n"
      << "            float inputs:base_metalness = " << scalar(pbr.metalness) << "\n"
      << "            float inputs:specular_weight = " << scalar(pbr.specularWeight) << "\n"
      << "            color3f inputs:specular_color = " << color(pbr.specularColor) << "\n"
      << "            float inputs:specular_roughness = " << scalar(pbr.specularRoughness) << "\n"
      << "            float inputs:specular_ior = " << scalar(pbr.specularIor) << "\n"
      << "            float inputs:transmission_weight = " << scalar(pbr.transmission) << "\n"
      << "            color3f inputs:transmission_color = " << color(pbr.transmissionColor) << "\n"
      << "            float inputs:transmission_depth = " << scalar(pbr.transmissionDepth) << "\n"
      << "            float inputs:subsurface_weight = " << scalar(pbr.subsurface) << "\n"
      << "            color3f inputs:subsurface_color = " << color(pbr.subsurfaceColor) << "\n"
      << "            color3f inputs:subsurface_radius = " << color(pbr.subsurfaceRadius) << "\n"
      << "            float inputs:subsurface_scale = " << scalar(pbr.subsurfaceScale) << "\n"
      << "            float inputs:coat_weight = " << scalar(pbr.coatWeight) << "\n"
      << "            color3f inputs:coat_color = " << color(pbr.coatColor) << "\n"
      << "            float inputs:coat_roughness = " << scalar(pbr.coatRoughness) << "\n"
      << "            float inputs:coat_ior = " << scalar(pbr.coatIor) << "\n"
      << "            float inputs:coat_darkening = " << scalar(pbr.coatDarkening) << "\n"
      << "            float inputs:sheen_weight = " << scalar(pbr.sheenWeight) << "\n"
      << "            color3f inputs:sheen_color = " << color(pbr.sheenColor) << "\n"
      << "            float inputs:sheen_roughness = " << scalar(pbr.sheenRoughness) << "\n"
      << "            float inputs:thin_film_weight = " << scalar(pbr.thinFilmWeight) << "\n"
      << "            float inputs:thin_film_thickness = " << scalar(pbr.thinFilmThicknessNm) << "\n"
      << "            float inputs:thin_film_ior = " << scalar(pbr.thinFilmIor) << "\n"
      << "            float inputs:emission_luminance = " << scalar(pbr.emission) << "\n"
      << "            color3f inputs:emission_color = " << color(pbr.emissionColor) << "\n"
      << "            float inputs:opacity = " << scalar(pbr.opacity) << "\n"
      << "            bool inputs:thin_walled = " << (pbr.geometryThinWalled ? "true" : "false") << "\n"
         "            token outputs:surface\n"
         "        }\n"
         "    }\n"
         "    def DomeLight \"UniformWhite\" {\n"
         "        color3f inputs:color = (1, 1, 1)\n"
         "        float inputs:intensity = 1\n"
         "    }\n"
         "}\n";
  out.close();
  if (!out) {
    if (error) *error = "failed writing temporary MaterialX validation scene";
    std::remove(generated->path.c_str());
    generated->path.clear();
    return false;
  }
  return true;
}

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
    const lightusd::Stage &stage, const lightusd::UsdPrim &prim,
    const matrix4d &parent_world,
    double time, std::vector<Vec3> *base_colors,
    std::vector<RTPreviewStats::MeshGeometry> *geos, size_t depth,
    const Options &opt) {
  constexpr size_t kMaxGpuPointTraversalDepth = 256;
  if (depth >= kMaxGpuPointTraversalDepth) {
    std::cerr << "WARN: GPU point fallback traversal exceeded "
              << kMaxGpuPointTraversalDepth << " levels at "
              << prim.GetPath().str() << "; subtree skipped.\n";
    return;
  }
  if (!prim.IsActive()) return;
  double dmat[16];
  lightusd::tydra::ComputeLocalTransform(prim, dmat, time);
  const matrix4d local = Mat4FromArray(dmat);
  const matrix4d world = local * parent_world;
  if (lightusd::IsParticleField(prim)) {
    lightusd::ParticleFieldData field;
    std::string field_warning;
    if (!lightusd::GetParticleFieldData(
            stage, prim, &field, time, &field_warning)) {
      std::cerr << "Gaussian fallback: invalid ParticleField data at "
                << prim.GetPath().str() << ".\n";
      return;
    }
    if (!field_warning.empty()) std::cerr << field_warning << "\n";
    lightusd::tydra::ValueArrayRead<float> points;
    lightusd::tydra::ValueArrayRead<float> scales;
    lightusd::tydra::ValueArrayRead<float> orientations;
    lightusd::tydra::ValueArrayRead<float> opacities;
    lightusd::tydra::ValueArrayRead<float> sh;
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
      for (const lightusd::UsdPrim &child : prim.GetChildren())
        CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                            depth + 1, opt);
      return;
    }
    size_t limit = opt.gaussian_max ? std::min(count, opt.gaussian_max) : count;

    const size_t sh_stride = (have_sh && count) ? (sh.size() / 3) / count : 0;
    std::array<RTPreviewStats::MeshGeometry, 64> batches;
    const size_t batch_limit = opt.gpu_triangle_chunk;
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
        lusdr::value::quatf q;
        q.real = orientations[orientation_index + 0];
        q.imag[0] = orientations[orientation_index + 1];
        q.imag[1] = orientations[orientation_index + 2];
        q.imag[2] = orientations[orientation_index + 3];
        const lusdr::value::matrix3d r = lusdr::to_matrix3x3(q);
        const lusdr::value::matrix4d r4 =
            lusdr::to_matrix(r, lusdr::value::double3{0.0, 0.0, 0.0});
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
    lightusd::tydra::ValueArrayRead<float> points;
    lightusd::tydra::ValueArrayRead<float> widths;
    lightusd::tydra::ValueArrayRead<float> normals;
    const bool have_points = ReadFloatArrayViewLazy(prim, "points", time, &points);
    ReadFloatArrayViewLazy(prim, "widths", time, &widths);
    ReadFloatArrayViewLazy(prim, "normals", time, &normals);
    const size_t batchLimit = opt.gpu_triangle_chunk;
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
      for (const lightusd::UsdPrim &child : prim.GetChildren())
        CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                            depth + 1, opt);
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
  for (const lightusd::UsdPrim &child : prim.GetChildren())
    CollectGpuPointsRec(stage, child, world, time, base_colors, geos,
                        depth + 1, opt);
}

// LightRT's GPU triangle APIs currently accept triangle BVHs, while its
// CPU/CUDA curve intersectors use dedicated round-linear primitives. Keep all
// GPU paths on their backend by converting round-linear curves to bounded tube
// triangle streams at the upload boundary.
bool AppendGpuRoundCurves(
    DirectScene &direct, std::vector<Vec3> *base_colors,
    std::vector<RTPreviewStats::MeshGeometry> *geos, const Options &opt) {
  if (!base_colors || !geos) return false;
  constexpr uint32_t kCurveSides = 6;
  const size_t curveChunk = opt.gpu_triangle_chunk;
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

  const uint64_t host_available = MemBudget::AvailableSystemMemory(opt->host_services.get());
  const uint64_t host_capacity =
      host_available
          ? std::min<uint64_t>(host_available,
                               lightusd::tydra::GiB(32))
          : lightusd::tydra::GiB(32);
  const lightusd::tydra::ResourceBudget budget =
      lightusd::tydra::ComputeResourceBudget(
          host_capacity, QueryDeviceLocalVRAMBytes());
  const double host_gib = double(budget.host_limit) /
                          double(lightusd::tydra::GiB(1));
  const double vram_gib = double(budget.vram_limit) /
                          double(lightusd::tydra::GiB(1));

  if (!opt->backend_explicit) {
    opt->vulkan = true;
  }
  if (!opt->rt_lod_explicit) opt->rt_lod = true;
  if (!opt->rt_lod_full_px_explicit) opt->rt_lod_full_px = 64.0f;
  if (!opt->rt_lod_cull_px_explicit) opt->rt_lod_cull_px = 2.0f;

  // Bound texture residency the way geometry already is. -texMaxSize /
  // -texBudgetMb still win; these only fill in the profile defaults.
  {
    const lightusd::tydra::TextureBudget texture_budget =
        lightusd::tydra::DeriveTextureBudget(budget);
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
static CameraFrame ResolveGpuCameraInst(const lightusd::Stage &stage,
                                        const Options &opt, const Bounds &bounds,
                                        lusdr::Axis usdUp, int *out_height) {
  const int cam_width = opt.width > 0 ? opt.width : 960;
  CameraFrame camera;
  Options auto_opt = opt;
  auto_opt.camera.clear();
  auto_opt.width = cam_width;
  if (!opt.camera.empty()) {
    float cam_aspect = 16.0f / 9.0f;
    if (FindCoreCameraFrame(stage, opt.camera, opt.timecode, &camera, &cam_aspect)) {
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
      camera = MakeCameraFrame(auto_opt, bounds, *out_height, usdUp);
    }
  } else if (opt.autoframe) {
    camera = MakeUsdRecordCamera(bounds, usdUp, cam_width, out_height);
  } else {
    if (*out_height <= 0) *out_height = 540;
    camera = MakeCameraFrame(auto_opt, bounds, *out_height, usdUp);
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
  n2w[6] = (b * f - c * e);
  n2w[7] = -(a * f - c * d);
  n2w[8] = (a * e - b * d);
  // Normalization removes determinant magnitude, but never its sign: dropping
  // a negative determinant reverses normals on reflected placements.
  const float determinant = a * n2w[0] + b * n2w[1] + c * n2w[2];
  if (determinant < 0)
    for (size_t k = 0; k < 9; ++k) n2w[k] = -n2w[k];
  if (std::abs(determinant) < 1.0e-20f)
    for (size_t k = 0; k < 9; ++k) n2w[k] = 0;
}

// Tessellate each authored curve prototype once, then share its triangle BLAS
// across placements. Six-sided tubes are a declared GPU approximation; they are
// not an analytic-curve or material-fidelity claim.
static bool ExtractCurveProto(const Options& opt,
                              const lightusd::UsdPrim& prim,
                              const lightusd::EvalOptions& evaluation,
                              GpuInstProto* out) {
  if (prim.GetTypeName() != "BasisCurves") {
    std::cerr << "[vkInstanced] unsupported curve type: "
              << prim.GetPath().str() << "\n";
    return false;
  }
  lightusd::tydra::ConverterConfig config;
  config.time_code = opt.timecode;
  config.curves.retain_control_points = false;
  config.curves.tessellation_segments = opt.curve_segments;
  config.animation.clip_stage_loader = evaluation.clip_stage_loader;
  lightusd::tydra::RenderSceneConverter converter(config);
  lightusd::tydra::RenderCurves curves;
  if (!converter.ConvertCurves(prim, &curves) || curves.has_alloc_failure()) {
    std::cerr << "[vkInstanced] curve conversion: " << prim.GetPath().str()
              << ": " << converter.GetLastError() << "\n";
    return false;
  }
  const size_t points = curves.tessellated_point_count();
  if (!points || points > UINT32_MAX) return false;
  const bool ribbon = !curves.tessellated_normals.empty();
  if(ribbon && curves.tessellated_normals.size()!=points*3)return false;
  const uint32_t sides = ribbon ? 2u : 6u;
  if (points > UINT32_MAX / sides) return false;
  out->verts.reserve(points * sides * 3);
  size_t offset = 0;
  auto point_at = [&](size_t k) {
    return Vec3{curves.tessellated_points[k * 3],
                curves.tessellated_points[k * 3 + 1],
                curves.tessellated_points[k * 3 + 2]};
  };
  auto radius_at = [&](size_t k) {
    const float width = !curves.tessellated_widths.empty()
                            ? curves.tessellated_widths[k]
                        : !curves.widths.empty() ? curves.widths[0]
                                                 : .01f;
    return width * .5f;
  };
  for (uint32_t count : curves.tessellated_vertex_counts) {
    if (count < 2 || size_t(count) > points - offset) return false;
    Vec3 previous_side{};
    bool have_previous_side = false;
    for (size_t j = 0; j < count; ++j) {
      const size_t k = offset + j;
      const Vec3 p = point_at(k);
      const float radius = radius_at(k);
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
          !std::isfinite(radius) || radius < 0)
        return false;
      Vec3 tangent =
          Sub(point_at(j + 1 < count ? k + 1 : k), point_at(j ? k - 1 : k));
      if (Length(tangent) < 1e-12f) tangent = {0, 1, 0};
      tangent = Normalize(tangent);
      const Vec3 up =
          std::abs(tangent.x) < std::abs(tangent.y)
              ? (std::abs(tangent.x) < std::abs(tangent.z) ? Vec3{1, 0, 0}
                                                           : Vec3{0, 0, 1})
              : (std::abs(tangent.y) < std::abs(tangent.z) ? Vec3{0, 1, 0}
                                                           : Vec3{0, 0, 1});
      Vec3 e1 = Normalize(Cross(tangent, up));
      if(ribbon) {
        const Vec3 normal{curves.tessellated_normals[k*3],curves.tessellated_normals[k*3+1],curves.tessellated_normals[k*3+2]};
        const Vec3 side=Cross(tangent,normal);
        if(Length(side)>1e-12f)e1=Normalize(side);
        else if(radius>0)return false;
      }
      if (!ribbon && have_previous_side) {
        // Parallel transport the ring frame instead of choosing a new arbitrary
        // axis at every point, which twists indexed tubes at axis crossings.
        const Vec3 transported=Sub(previous_side,Mul(tangent,Dot(previous_side,tangent)));
        if(Length(transported)>1e-12f)e1=Normalize(transported);
      }
      previous_side=e1;have_previous_side=true;
      const Vec3 e2 = Cross(tangent, e1);
      for (uint32_t side = 0; side < sides; ++side) {
        const float angle = 6.28318530717958648f * float(side) / float(sides);
        const Vec3 v =
            Add(p, Mul(Add(Mul(e1, std::cos(angle)), Mul(e2, std::sin(angle))),
                       radius));
        out->verts.insert(out->verts.end(), {v.x, v.y, v.z});
      }
      if (j && Length(Sub(p, point_at(k - 1))) >= 1e-12f &&
          (radius > 0 || radius_at(k - 1) > 0)) {
        if (out->idx.size() > size_t(UINT32_MAX) - sides * 6) return false;
        const uint32_t previous = uint32_t((k - 1) * sides),
                       current = uint32_t(k * sides);
        if(ribbon) {
          out->idx.insert(out->idx.end(),{previous,previous+1,current+1,previous,current+1,current});
        } else for (uint32_t side = 0; side < sides; ++side) {
          const uint32_t next = (side + 1) % sides;
          out->idx.insert(out->idx.end(),
                          {previous + side, previous + next, current + next,
                           previous + side, current + next, current + side});
        }
      }
    }
    offset += count;
  }
  if (offset != points || out->idx.empty()) return false;
  return true;
}

// Extract one prototype's geometry in PROTOTYPE-LOCAL (object) space from a
// Mesh prim: positions, fan-triangulated indices, per-triangle flat + vertex
// normals, and the constant displayColor. Mirrors the flat GPU extractor minus
// the world transform (the instance transform is applied by the TLAS).
// Displacement IS applied here (once per prototype, in object space — the
// instance transform scales it, like any other prototype-local geometry).
static bool EndsWithPtxPath(const std::string &path) {
  if (path.size() < 4) return false;
  std::string ext = path.substr(path.size() - 4);
  for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".ptx";
}

// Cheap material-subtree scan: does any authored asset property name a .ptx?
// This avoids resolving/loading ordinary image textures for the many
// non-Ptex prototypes in an Island-scale instanced scene.
static bool PrimSubtreeHasPtex(const lightusd::UsdPrim &prim) {
  if (!prim.IsValid()) return false;
  for (const std::string &name : prim.GetPropertyNames()) {
    const lightusd::Value *value = prim.GetPropertyValue(name);
    if (!value) continue;
    std::optional<std::string> asset = value->as_asset_path();
    if (!asset) asset = value->as_string();
    if (asset && EndsWithPtxPath(*asset)) return true;
  }
  for (const lightusd::UsdPrim &child : prim.GetChildren())
    if (PrimSubtreeHasPtex(child)) return true;
  return false;
}

static bool ExtractProtoGeo(
    const lightusd::Stage& stage, const Options& opt, TextureCache& tc,
    std::unordered_map<std::string, ResolvedMat>& material_cache,
    std::unordered_map<std::string, bool>& ptex_material_cache,
    const lightusd::UsdPrim& prim,
    const lightusd::EvalOptions& evaluation, GpuInstProto* out) {
  // Resolve the prototype's base-colour material only when its subtree binds a
  // .ptx. The instanced path previously carried only primvars:displayColor, so
  // a Ptex-bound material rendered with its constant fallback.
  const std::string material_path =
      lightusd::GetInheritedBoundMaterialPath(stage,
                                                    prim.GetPath().str());
  bool has_ptex = false;
  if (!material_path.empty()) {
    auto cached = ptex_material_cache.find(material_path);
    if (cached != ptex_material_cache.end()) {
      has_ptex = cached->second;
    } else {
      has_ptex = PrimSubtreeHasPtex(stage.GetPrimAtPath(material_path));
      ptex_material_cache.emplace(material_path, has_ptex);
    }
  }
  if (has_ptex) {
    MeshJobCore resolved;
    resolved.prim = prim;
    ResolveMeshMaterialCached(stage, prim, tc, material_cache, &resolved);
    if (resolved.tex_id >= 0 && tc.textures &&
        size_t(resolved.tex_id) < tc.textures->size() &&
        (*tc.textures)[size_t(resolved.tex_id)].is_ptex) {
      out->ptex_tex_id = resolved.tex_id;
    }
  }
  const bool want_ptex = out->ptex_tex_id >= 0;
  uint32_t nv = 0;
  std::vector<Vec3> vn;
  bool perVertexN = false;
  std::vector<float> generated_uvs;
  if (IsCurvePrimCore(prim)) {
    if (!ExtractCurveProto(opt, prim, evaluation, out)) return false;
    nv = uint32_t(out->verts.size() / 3);
  } else if (lightusd::tydra::IsAnalyticGeomTypeName(
                 prim.GetTypeName())) {
    GeneratedMeshCore generated;
    if (!GenerateAnalyticMeshCore(stage, prim, opt.timecode, &generated))
      return false;
    std::vector<float> normals;
    ExpandGeneratedMeshCornersCore(generated, &out->verts, &normals,
                                   &generated_uvs, &out->idx);
    nv = uint32_t(out->verts.size() / 3);
    vn.resize(nv);
    for (size_t j = 0; j < size_t(nv); ++j)
      vn[j] = Vec3{normals[j * 3], normals[j * 3 + 1], normals[j * 3 + 2]};
    perVertexN = true;
  } else {
    lightusd::tydra::ValueArrayRead<float> point_values, normal_values;
    if (!lightusd::tydra::ReadFloatArray(prim, "points", evaluation,
                                               &point_values) ||
        point_values.empty() || point_values.size() % 3 != 0 ||
        point_values.size() / 3 > UINT32_MAX)
      return false;
    nv = uint32_t(point_values.size() / 3);
    std::vector<float> skinned_points;
    std::vector<double> skin_normals;
    std::string skin_error;
    if (!lightusd::tydra::EvaluateSkinnedPoints(
            stage, prim, opt.timecode, point_values, &skinned_points,
            &skin_error,
            (prim.GetPropertyValue("normals") ||
             prim.GetPropertyValue("primvars:normals"))
                ? &skin_normals
                : nullptr)) {
      std::cerr << "Prototype skinning failed: " << prim.GetPath().str() << ": "
                << skin_error << "\n";
      return false;
    }
    if (skinned_points.empty())
      out->verts.assign(point_values.begin(), point_values.end());
    else
      out->verts = std::move(skinned_points);
    lightusd::tydra::ValueArrayRead<int32_t> index_values, count_values;
    lightusd::tydra::ReadIntArray(prim, "faceVertexIndices", evaluation,
                                        &index_values);
    lightusd::tydra::ReadIntArray(prim, "faceVertexCounts", evaluation,
                                        &count_values);
    lightusd::tydra::MeshFloatAttribute normals;
    bool have_normals = lightusd::tydra::MeshUsesAuthoredNormals(prim) &&
                              (lightusd::tydra::ReadMeshFloatAttribute(
                                  prim, "primvars:normals", evaluation, 3, nv,
                                  index_values.size(), &normals) ||
                              lightusd::tydra::ReadMeshFloatAttribute(
                                  prim, "normals", evaluation, 3, nv,
                                  index_values.size(), &normals));
    std::vector<float> generated_smooth_normals;
    if (!have_normals && opt.smooth &&
        lightusd::tydra::MeshUsesGeneratedSmoothNormals(prim)) {
      lightusd::tydra::ValueArrayRead<float> posed_points;
      posed_points.view={out->verts.data(),out->verts.size(),false};
      const auto* orientation=prim.GetPropertyValue("orientation");
      const bool left_handed=orientation && orientation->as_token() && *orientation->as_token()=="leftHanded";
      if (lightusd::tydra::ComputeMeshSmoothNormals(posed_points,count_values,index_values,
              left_handed,&generated_smooth_normals)) {
        normals.components=3;normals.interpolation="vertex";
        normals.values.view={generated_smooth_normals.data(),generated_smooth_normals.size(),false};
        have_normals=true;
        skin_normals.clear();
      }
    }
    auto append_triangle = [&](size_t a, size_t b, size_t c, size_t face,
                               int32_t face_count, int32_t fan_index) {
      Vec3 ns[3]{};
      const size_t corners[] = {a, b, c};
      for (size_t k = 0; k < 3; ++k) {
        const int32_t point = index_values[corners[k]];
        if (point < 0 || uint32_t(point) >= nv) return false;
        if (have_normals) {
          size_t tuple = 0;
          if (!normals.TupleIndex(size_t(point), corners[k], face, &tuple))
            return false;
          Vec3 n{normals.values[tuple * 3], normals.values[tuple * 3 + 1],
                 normals.values[tuple * 3 + 2]};
          if (!skin_normals.empty()) {
            const double* m = skin_normals.data() + size_t(point) * 9;
            n = {float(n.x * m[0] + n.y * m[3] + n.z * m[6]),
                 float(n.x * m[1] + n.y * m[4] + n.z * m[7]),
                 float(n.x * m[2] + n.y * m[5] + n.z * m[8])};
          }
          ns[k] = n;
        }
        out->idx.push_back(uint32_t(point));
      }
      if (have_normals) {
        out->vn0.push_back(ns[0]);
        out->vn1.push_back(ns[1]);
        out->vn2.push_back(ns[2]);
      }
      if (want_ptex) {
        // Same intrinsic mapping as the flat/chunked paths: a quad's two fan
        // triangles carry (face, fan_triangle); non-quads are invalid.
        out->ptex_tri.push_back(
            face_count == 4
                ? MakePtexTriRef(uint32_t(face),
                                 int(fan_index) - 1, false)
                : kPtexTriInvalid);
      }
      return true;
    };
    const auto* idx = &index_values;
    const auto* cnt = &count_values;
    if (!idx || idx->empty()) return false;
    if (cnt && !cnt->empty()) {
      lightusd::tydra::ValueArrayRead<int32_t> hole_values;
      lightusd::tydra::ReadIntArray(prim, "holeIndices", evaluation,
                                          &hole_values);
      const std::unordered_set<int32_t> holes(hole_values.begin(),
                                              hole_values.end());
      size_t off = 0;
      size_t face = 0;
      for (int c : *cnt) {
        if (c < 0 || size_t(c) > idx->size() - off) return false;
        if (c >= 3 && !holes.count(static_cast<int32_t>(face))) {
          for (int k = 1; k + 1 < c; ++k)
            if (!append_triangle(off, off + size_t(k), off + size_t(k) + 1,
                                 face, c, k))
              return false;
        }
        off += size_t(c < 0 ? 0 : c);
        ++face;
      }
      if (off != idx->size()) return false;
    } else {
      if (idx->size() % 3 != 0) return false;
      for (size_t off = 0; off < idx->size(); off += 3)
        if (!append_triangle(off, off + 1, off + 2, off / 3, 3, 1))
          return false;
    }
  }
  if (const auto* orientation = prim.GetPropertyValue("orientation")) {
    const auto token = orientation->as_token();
    if (token && *token == "leftHanded")
      for (size_t t = 0; t + 2 < out->idx.size(); t += 3) {
        std::swap(out->idx[t + 1], out->idx[t + 2]);
        if (!out->vn1.empty()) std::swap(out->vn1[t / 3], out->vn2[t / 3]);
        if (t / 3 < out->ptex_tri.size() &&
            out->ptex_tri[t / 3] != kPtexTriInvalid)
          out->ptex_tri[t / 3] ^= 2u;  // winding swapped
      }
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
    std::vector<float> uvs = std::move(generated_uvs);
    if (const lightusd::Value* uv =
            prim.GetPropertyValue("primvars:st")) {
      const auto u = uv->as_float_array();
      if (u && !u->empty()) uvs.assign(u.begin(), u.end());
    }
    float disp_const = 0.0f;
    ScalarTex disp_tex;
    const std::vector<lightusd::Path>* bind =
        prim.GetRelationship("material:binding");
    if (bind && !bind->empty()) {
      lightusd::UsdPrim mat = stage.GetPrimAtPath((*bind)[0]);
      if (mat.IsValid()) {
        lightusd::UsdPrim surf =
            ConnectedPrimCore(stage, mat, "outputs:surface");
        if (!surf.IsValid())
          surf = ConnectedPrimCore(stage, mat, "outputs:mtlx:surface");
        if (surf.IsValid()) {
          if (const lightusd::Value* d =
                  surf.GetPropertyValue("inputs:displacement"))
            if (const float* f = d->as_float()) disp_const = *f;
          ResolveScalarTextureCore(stage, surf, "inputs:displacement", tc,
                                   &disp_tex);
        }
      }
    }
    if (disp_tex.id >= 0 || disp_const != 0.0f) {
      std::vector<Vec3> sn(nv, Vec3{0, 0, 0});  // area-weighted smooth normals
      for (size_t t = 0; t + 2 < out->idx.size(); t += 3) {
        uint32_t a = out->idx[t], b = out->idx[t + 1], c = out->idx[t + 2];
        Vec3 pa{out->verts[a * 3], out->verts[a * 3 + 1],
                out->verts[a * 3 + 2]};
        Vec3 pb{out->verts[b * 3], out->verts[b * 3 + 1],
                out->verts[b * 3 + 2]};
        Vec3 pc{out->verts[c * 3], out->verts[c * 3 + 1],
                out->verts[c * 3 + 2]};
        Vec3 fn = Cross(Sub(pb, pa), Sub(pc, pa));
        sn[a] = Add(sn[a], fn);
        sn[b] = Add(sn[b], fn);
        sn[c] = Add(sn[c], fn);
      }
      const lusdr::Texture* dtex = (disp_tex.id >= 0 && tc.textures &&
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
    uint32_t i0 = out->idx[t * 3], i1 = out->idx[t * 3 + 1],
             i2 = out->idx[t * 3 + 2];

    if (perVertexN) {
      out->vn0.push_back(vn[i0]);
      out->vn1.push_back(vn[i1]);
      out->vn2.push_back(vn[i2]);
    }
  }

  out->base_color = Vec3{0.5f, 0.5f, 0.5f};
  if (const lightusd::Value* dcv =
          prim.GetPropertyValue("primvars:displayColor")) {
    const auto dc = dcv->as_float_array();
    if (dc && dc->size() >= 3)
      out->base_color = Vec3{(*dc)[0], (*dc)[1], (*dc)[2]};
  }
  return true;
}

// Split a prototype whose triangle count exceeds `chunk` into sub-prototypes of
// <= chunk triangles each (vertices remapped/compacted per chunk, shading
// slices carried along). Each sub-proto is just a smaller prototype -- one
// BLAS, its own triangle range -- so the instance encoding (pid =
// instance*stride + localTri) and hit decode are unchanged; a placement of the
// original prototype is simply emitted once per sub-proto. Works around drivers
// (RADV) whose AS build-size query overflows for very large single-BLAS builds
// and then GPU-faults.
static void ChunkProto(const GpuInstProto& src, uint32_t chunk,
                       std::vector<GpuInstProto>* out) {
  const uint32_t nch = (src.ntris + chunk - 1u) / chunk;
  for (uint32_t c = 0; c < nch; ++c) {
    const uint32_t t0 = c * chunk;
    const uint32_t t1 = std::min((c + 1u) * chunk, src.ntris);
    GpuInstProto sub;
    sub.base_color = src.base_color;
    sub.ptex_tex_id = src.ptex_tex_id;
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
      if (!src.normals.empty()) sub.normals.push_back(src.normals[t]);
      if (!src.ptex_tri.empty()) sub.ptex_tri.push_back(src.ptex_tri[t]);
      if (!src.vn0.empty()) {
        sub.vn0.push_back(src.vn0[t]);sub.vn1.push_back(src.vn1[t]);sub.vn2.push_back(src.vn2[t]);
      }
    }
    out->push_back(std::move(sub));
  }
}

// -vkInstanced: decompose the expanded mesh jobs into shared prototypes (grouped
// by source prim path) + per-placement instances, then render with the true
// two-level GPU TLAS. Returns true if it produced an image; false to fall back to
// the flat path. Geometry is stored ONCE per prototype regardless of instance
// count (the memory-sharing win over the flat world-space soup).
static int TryRunInstancedVk(const lightusd::Stage &stage,
                             const Options &opt,
                             const lightusd::ValueClipStageLoader& clip_loader) {
  lightusd::EvalOptions evaluation;
  evaluation.time = lightusd::TimeQuery::Numeric(opt.timecode);
  evaluation.follow_connections = false;
  evaluation.clip_stage_loader = clip_loader;
  evaluation.clip_stage_cache = std::make_shared<lightusd::ValueClipStageCache>();
  bool invalid_prototype = false;
  GpuInstancedScene scene;
  // Each source prim maps to one OR MORE sub-prototype indices (a huge prototype
  // is chunk-split; an empty vector marks a prim that failed extraction).
  std::unordered_map<std::string, std::vector<uint32_t>> proto_id;
  std::vector<Bounds> proto_aabb;
  // Max triangles per prototype BLAS (8M; overridable via -instChunkTris
  // for testing the split path on small meshes). A larger prototype is chunked.
  const uint32_t chunk = opt.inst_chunk_tris;

  // Material textures for prototype extraction (base colour/Ptex and
  // displacement), loaded relative to the input and shared across prototypes.
  std::vector<lusdr::Texture> material_textures;
  std::unordered_map<std::string, ResolvedMat> material_cache;
  std::unordered_map<std::string, bool> ptex_material_cache;
  TextureCache tc;
  tc.textures = &material_textures;
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
  // WITHOUT materializing a MeshJobCore per instance, so a huge instanced scene
  // (Moana island: tens of millions) costs ~one GpuInstPlacement (88 B) of host
  // memory per placement instead of a ~392 B MeshJobCore.
  auto place = [&](const lightusd::UsdPrim &prim, const matrix4d &world,
                   lusdr::Purpose purpose) {
    if (!PurposeVisible(PurposeBit(purpose), opt.purpose_mask)) return;
    const std::string key = prim.GetPath().str();
    auto it = proto_id.find(key);
    const std::vector<uint32_t> *subs = nullptr;
    if (it == proto_id.end()) {
      GpuInstProto pr;
      if (!ExtractProtoGeo(stage, opt, tc, material_cache, ptex_material_cache,
                           prim, evaluation, &pr)) {
        const auto* points = prim.GetPropertyValue("points");
        if (points && points->is_array() && points->array_size() > 0) {
          std::cerr << "[vkInstanced] failed prototype: " << key << " points=" << points->array_size()
                    << " extractedVertices=" << pr.verts.size() / 3
                    << " extractedIndices=" << pr.idx.size() << "\n";
          invalid_prototype = true;
        }
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
  // instances / ~110k prototype BLAS exceed VRAM; raise -instBudget to fan
  // out across multiple TLASes, memory permitting). Explicitly configurable.
  const size_t budget = opt.inst_budget ? opt.inst_budget
      : opt.vulkan_instanced_stream ? 64000000u : 16000000u;

  size_t emitted = 0;
  for (const auto &root : stage.GetRootPrims()) {
    if (emitted > budget) break;
    emitted += CollectRTInstancePlacementsCore(
        stage, root, matrix4d::identity(), lusdr::Purpose::Default,
        opt.timecode, opt.mask, place, budget + 1 - emitted);
  }
  if (invalid_prototype) return -1;
  if (emitted > budget) {
    std::cerr << "[vkInstanced] instance budget " << budget
              << " exceeded; refusing an incomplete capture. Raise -instBudget.\n";
    return -1;
  }
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
  lusdr::Axis usdUp = (up_axis == 2.0)   ? lusdr::Axis::Z
                         : (up_axis == 0.0) ? lusdr::Axis::X
                                            : lusdr::Axis::Y;
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
  // In practice the -instBudget host-memory budget below binds first.
  const uint64_t max_inst = 8ull * 16000000ull;  // 8 TLAS slices (~128M instances)
  if (scene.insts.size() > max_inst) {
    std::cerr << "[vkInstanced] multi-slice instance ceiling exceeded; refusing an incomplete capture\n";
    return -1;
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

  if (opt.stats) {
    size_t ptex_protos = 0, ptex_tris = 0;
    for (const GpuInstProto &pr : scene.protos) {
      if (pr.ptex_tex_id < 0) continue;
      ++ptex_protos;
      for (uint32_t ref : pr.ptex_tri)
        if (ref != kPtexTriInvalid) ++ptex_tris;
    }
    std::cerr << "[vkInstanced] ptex prototypes=" << ptex_protos
              << " triangles_with_face=" << ptex_tris
              << " textures=" << material_textures.size() << "\n";
  }
  // Once GPU construction starts, failure must be reported to the caller.
  // Falling back can hide device loss and expand an already oversized scene.
  return RunVulkanLightRTInstanced(opt, scene, material_textures, camera,
                                   out_height)
             ? 1
             : -1;
}
#endif  // HAVE_VULKAN

int main(int argc, char **argv) {
  lusdview::PreviewGpuBudgetReport previewBudgetReport;
  lightusd::tydra::ScenePreviewRequest scenePreview;
  std::string scenePreviewError;
  if (!lightusd::tydra::ExtractScenePreviewArguments(
          &argc, argv, &scenePreview, &scenePreviewError)) {
    std::cerr << scenePreviewError << "\n";
    return EXIT_FAILURE;
  }
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
  if (scenePreview.enabled && !opt.volume_mtlx_file.empty()) {
    std::cerr << "MaterialX volume looks cannot be combined with scene-preview conversion.\n";
    return EXIT_FAILURE;
  }
  GeneratedMtlxScene generated_mtlx_scene;
  if (!opt.mtlx_file.empty()) {
    if (scenePreview.enabled) {
      std::cerr << "--mtlx cannot be combined with scene-preview options.\n";
      return EXIT_FAILURE;
    }
    std::string mtlx_error;
    if (!BuildMtlxValidationScene(opt, &generated_mtlx_scene, &mtlx_error)) {
      std::cerr << "MaterialX validation scene: " << mtlx_error << "\n";
      return EXIT_FAILURE;
    }
    opt.input = generated_mtlx_scene.path;
    if (!opt.width_explicit) opt.width = 512;
    if (!opt.height_explicit) opt.height = 512;
    opt.rt_preview = true;
    opt.material_shading = Options::MaterialShading::LightRtBsdf;
    opt.ambient = 0.0f;
    std::cerr << "MaterialX validation: sphere, uniform white environment, CPU LightRT BSDF"
              << (opt.path_trace ? " path tracing\n" : "\n");
  }
  if (scenePreview.enabled) {
    scenePreview.options.time_code = std::isfinite(opt.timecode) ? opt.timecode : 0.0;
    scenePreview.options.composition.variant_overrides = opt.variant_overrides;
    if (!opt.mask.empty()) {
      std::cerr << "scene preview does not yet accept -mask; use a USD wrapper layer\n";
      return EXIT_FAILURE;
    }
    if (!lightusd::tydra::PrepareScenePreview(
            &opt.input, scenePreview, 0, &scenePreviewError)) {
      std::cerr << "scene preview: " << scenePreviewError << "\n";
      return EXIT_FAILURE;
    }
    if (scenePreview.camera_lod ||
        (scenePreview.direct_volume &&
         scenePreview.options.kind ==
             lightusd::tydra::ScenePreviewKind::Volume)) {
      std::array<double,3> direction={1,.6,1};
      if(opt.has_view_dir) direction={-opt.view_dir.x,-opt.view_dir.y,-opt.view_dir.z};
      const auto camera=lightusd::tydra::InitialScenePreviewLodCamera(
          scenePreview.result,opt.input,opt.camera,opt.height>0?opt.height:540,opt.fit_scale,direction);
      if (scenePreview.source_refinement) {
        const auto refined = lightusd::tydra::RefineScenePreview(
            scenePreview.source, scenePreview.result, scenePreview.options,
            camera, std::max(1, opt.width), opt.height > 0 ? opt.height : 540,
            scenePreview.lod_pixel_error);
        if (!refined.success) {
          std::cerr << "source refinement: " << refined.error << "\n";
          return EXIT_FAILURE;
        }
        if (refined.usd_path != scenePreview.result.usd_path) {
          scenePreview.result = refined;
          if (scenePreview.options.kind !=
                  lightusd::tydra::ScenePreviewKind::Volume ||
              !scenePreview.direct_volume) {
            scenePreview.lods = lightusd::tydra::BuildScenePreviewLods(
                refined, scenePreview.options);
            if (!scenePreview.lods.success) {
              std::cerr << scenePreview.lods.error << "\n";
              return EXIT_FAILURE;
            }
            opt.input = scenePreview.lods.root_path;
          }
        }
      }
      if (scenePreview.options.kind ==
              lightusd::tydra::ScenePreviewKind::Volume &&
          scenePreview.direct_volume) {
        namespace preview = lightusd::tydra;
        const auto backend =
            opt.cuda ? preview::PreviewComputeBackend::Cuda
            : (opt.vulkan || opt.vulkan_rt || opt.vulkan_instanced)
                ? preview::PreviewComputeBackend::Vulkan
                : preview::PreviewComputeBackend::Cpu;
        preview::PreviewVolumeRenderer renderer;
        if (!renderer.load(scenePreview.result, backend,
                           scenePreview.options.gpu_budget_bytes,
                           &scenePreviewError, scenePreview.options.compute_options)) {
          std::cerr << scenePreviewError << "\n";
          return EXIT_FAILURE;
        }
        preview::PreviewVolumeCamera rays;
        rays.settings=scenePreview.volume;
        rays.exposure=float(camera.exposure);
        rays.eye = {float(camera.eye[0]), float(camera.eye[1]),
                    float(camera.eye[2])};
        rays.forward = {float(camera.forward[0]), float(camera.forward[1]),
                        float(camera.forward[2])};
        const int width = std::max(1, opt.width),
                  height = opt.height > 0 ? opt.height : 540;
        const float fy = float(height) * .5f / float(camera.focal_pixels),
                    fx = fy * float(width) / float(height);
        const Vec3 forward{rays.forward[0], rays.forward[1], rays.forward[2]};
        const Vec3 right = Normalize(
            Cross(forward, Vec3{float(camera.up[0]), float(camera.up[1]),
                                float(camera.up[2])}));
        const Vec3 up = Normalize(Cross(right, forward));
        rays.right = {right.x * fx, right.y * fx, right.z * fx};
        rays.up = {up.x * fy, up.y * fy, up.z * fy};
        rays.orthographic = camera.orthographic;
        rays.near_plane = float(camera.near_plane);
        rays.far_plane = float(camera.far_plane);
        lightusd::Image image;
        image.width = width;
        image.height = height;
        image.channels = 4;
        image.bpp = 8;
        for(uint32_t frame=0;frame<std::min(static_cast<uint32_t>(opt.samples),rays.settings.accumulation_frames);++frame)
          if (!renderer.render(rays, width, height, &image.data,
                               &scenePreviewError)) {
            std::cerr << scenePreviewError << "\n";
            return EXIT_FAILURE;
          }
        lightusd::image::WriteOption write;
        write.format = lightusd::image::WriteImageFormat::Autodetect;
        const auto saved =
            lightusd::image::WriteImageToFile(opt.output, image, write);
        if (!saved) {
          std::cerr << saved.error() << "\n";
          return EXIT_FAILURE;
        }
        std::cerr << "direct UsdVol grids: backend=" << renderer.backend()
                  << " accumulated_frames=" << renderer.accumulatedFrames() << "\n";
        return EXIT_SUCCESS;
      }
      opt.preview_variant_overrides=lightusd::tydra::SelectScenePreviewLods(
          scenePreview.lods,camera,scenePreview.lod_pixel_error);
      if(opt.preview_variant_overrides.empty()) {std::cerr<<"preview LOD selection exceeds budget\n";return EXIT_FAILURE;}
    }
    opt.rt_preview = true;
    opt.scene_preview_albedo=scenePreview.options.kind!=lightusd::tydra::ScenePreviewKind::Volume;
    opt.max_vram_gib = double(scenePreview.options.gpu_budget_bytes) /
                       double(uint64_t(1) << 30);
    opt.texture_budget_mb = 32;
    opt.texture_budget_explicit = true;
    opt.large_scene_profile = Options::LargeSceneProfile::Off;
    opt.lod_stream = false;
    lusdview::ScenePreviewGpuBudget().configure(scenePreview.options.gpu_budget_bytes);
    const uint64_t width=static_cast<uint64_t>(std::max(1,opt.width));
    const uint64_t height=static_cast<uint64_t>(opt.height>0?opt.height:540);
    const uint64_t samples = static_cast<uint64_t>(std::max(1, opt.samples));
    const uint64_t frameLimit = scenePreview.options.gpu_budget_bytes / 2 / 256;
    if (width > frameLimit / height / samples) {
      std::cerr << "preview frame/sampling buffers exceed the GPU budget; reduce resolution or samples\n";
      return EXIT_FAILURE;
    }
  }
  SetIblBackendEnvmap(opt.ibl_envmap);
  SetPreviewSubdivisionLevel(opt.subdivision_level);
  // The application explicitly opts into host memory observations.
  opt.host_services = lightusd::CreateNativeHostServices();
  ApplyLargeSceneProfile(&opt);

  // Configure the process memory budget: -maxMem <GiB>, else auto
  // min(32 GiB, 0.5 * system MemAvailable). Keeps lusdrender from being
  // OOM-killed on huge (instance-expanded) scenes; it aborts with a clear
  // message instead.
  MemBudget::Get().Init(opt.max_mem_gib, opt.host_services);
  // Apply the same bounded texture defaults even without a named large-scene
  // profile. Explicit texture flags remain authoritative, including an
  // explicit zero when a caller intentionally requests source resolution.
  {
    const lightusd::tydra::ResourceBudget budget =
        lightusd::tydra::ComputeResourceBudget(
            MemBudget::Get().Cap(), QueryDeviceLocalVRAMBytes());
    const lightusd::tydra::TextureBudget texture_budget =
        lightusd::tydra::DeriveTextureBudget(budget);
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
    size_t avail = MemBudget::AvailableSystemMemory(opt.host_services.get());
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
  // through to the normal core-loader render. Best-effort: on failure (no
  // districts / compose error) opt.input is left untouched and we render as-is.
  std::string lod_wrapper;
  if (opt.lod_stream) {
    if (!opt.rt_preview && !opt.vulkan && !opt.vulkan_rt && !opt.use_d3d &&
        !opt.hip && !opt.cuda) {
      opt.rt_preview = true;  // default the LOD render to the CPU rtPreview path
    }
    PrepareLodStream(&opt, &lod_wrapper);
  }

  // Every CPU render uses the core streaming path.
  if (!opt.vulkan && !opt.use_d3d && !opt.hip && !opt.cuda) {
    return RunRTPreviewCore(opt);
  }

  // PrimId is intentionally not in this gate: the next CPU path owns the
  // prim-id sidecar map, while the vkr data path writes numeric AOVs. Depth and
  // the two normal channels are the supported vkr diagnostics.
  const bool gpu_data_aov = opt.aov_explicit && opt.vulkan &&
      !opt.path_trace &&
      opt.subdivision_level == 0 && opt.samples == 1 &&
      (opt.aov == CaptureAov::Depth || opt.aov == CaptureAov::GeometricNormal ||
       opt.aov == CaptureAov::ShadingNormal);
  if ((opt.aov_explicit && !gpu_data_aov) || opt.all_render_products) {
    std::cerr << "Capture requires the next CPU path, or flat Vulkan depth/geometricNormal/shadingNormal with one sample\n";
    return EXIT_FAILURE;
  }

  // -frames (per-timecode animation output) is implemented only by the `core`
  // path above. It used to be silently ignored here -- the run produced one
  // image literally named with the `####` token and no animation. Fail loudly
  // instead so the user adds -rtPreview (or drops the flag).
  if (!opt.frames.empty()) {
    std::cerr << "-frames is only supported on the core path (a .usdc input or "
                 "-rtPreview); this run would render a single frame and ignore "
                 "it. Add -rtPreview, or drop -frames.\n";
    return EXIT_FAILURE;
  }

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
  // GPU backends (Vulkan / Direct3D 11 / HIP): load the scene through the `core`
  // lazy loader, build the geometry once, then trace on the selected GPU backend.
  if (opt.vulkan || opt.use_d3d || opt.hip || opt.cuda) {
    // Load through core loader.
    lightusd::Stage stage;
    lightusd::ValueClipStageLoader clip_stage_loader;
    std::string warn, err;
    if (!LoadCoreStageBudgeted(opt, &stage, &warn, &err,
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
    // no per-instance MeshJobCore), then renders. On any failure (no shares, ray
    // query unavailable) it falls through to the flat GPU path below.
    if (opt.vulkan_instanced && opt.vulkan_rt) {
      const int result = TryRunInstancedVk(stage, opt, clip_stage_loader);
      if (result < 0) return EXIT_FAILURE;
      if (result > 0) return EXIT_SUCCESS;
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
      CollectLightsCore(stage, root, matrix4d::identity(), opt.timecode,
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
      // like the CPU -rtPreview path (CollectRTPreviewMeshesCore). The GPU
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
      lightusd::EvalOptions mesh_eval;
      mesh_eval.time = lightusd::TimeQuery::Numeric(opt.timecode);
      mesh_eval.follow_connections = false;
      mesh_eval.clip_stage_loader = clip_stage_loader;
      mesh_eval.clip_stage_cache =
          std::make_shared<lightusd::ValueClipStageCache>();
      for (const auto &root : stage.GetRootPrims()) {
        // Keep only one root's traversal records alive.  A production stage can
        // contain thousands of roots (or large instancer expansions); collecting
        // every MeshJobCore before converting any geometry creates a needless
        // scene-wide transient peak on top of the final GPU chunk stream.
        std::vector<MeshJobCore> mesh_jobs;
        CollectRTPreviewMeshesCore(stage, root, matrix4d::identity(),
                                   lusdr::Purpose::Default, opt.timecode,
                                   opt.mask, &mesh_jobs,
                                   /*expand_instancers=*/true);

        // Stream geometry in WORLD space.
        for (MeshJobCore &job : mesh_jobs) {
        // Purpose visibility: hide guide (and others per -purpose) like the CPU
        // path; the GPU path used to render every purpose unconditionally, so the
        // 26M-triangle guide breadcrumb/endpoint Points engulfed the camera.
        if (!PurposeVisible(PurposeBit(job.purpose), opt.purpose_mask)) continue;
        lightusd::UsdPrim &prim = job.prim;
        ResolveMeshMaterialCached(stage, prim, tc, material_cache, &job);
        if (opt.stats && gpu_materials.size() < 8u) {
          std::cerr << "GPU material: prim=" << prim.GetPath().str()
                    << " binding=" << lightusd::GetInheritedBoundMaterialPath(stage,prim.GetPath().str())
                    << " base=" << job.base_color.x << "," << job.base_color.y << "," << job.base_color.z
                    << " emission=" << job.emission.x << "," << job.emission.y << "," << job.emission.z
                    << " openpbr=" << job.has_openpbr
                    << " weight=" << job.openpbr.emission << " opacity=" << job.openpbr.opacity << "\n";
        }
        RTPreviewStats::MeshGeometry geo;
        uint32_t nv = 0;
        if (lightusd::tydra::IsAnalyticGeomTypeName(
                prim.GetTypeName())) {
          GeneratedMeshCore generated;
          if (!GenerateAnalyticMeshCore(stage, prim, opt.timecode, &generated))
            continue;
          ExpandGeneratedMeshCornersCore(generated, &geo.positions,
                                          &geo.normals, &geo.uvs, &geo.indices);
          nv = uint32_t(geo.positions.size() / 3);
          for (size_t j = 0; j < size_t(nv); ++j) {
            const Vec3 point = TransformPoint(job.world, Vec3{
                geo.positions[j * 3], geo.positions[j * 3 + 1],
                geo.positions[j * 3 + 2]});
            const Vec3 normal = TransformVector(job.world, Vec3{
                geo.normals[j * 3], geo.normals[j * 3 + 1],
                geo.normals[j * 3 + 2]});
            geo.positions[j * 3] = point.x;
            geo.positions[j * 3 + 1] = point.y;
            geo.positions[j * 3 + 2] = point.z;
            geo.normals[j * 3] = normal.x;
            geo.normals[j * 3 + 1] = normal.y;
            geo.normals[j * 3 + 2] = normal.z;
          }
        } else {
        lightusd::tydra::ValueArrayRead<float> point_values;
        lightusd::tydra::ReadFloatArray(
            prim, "points", mesh_eval, &point_values);
        std::vector<float> skinned_points;
        std::vector<double> skin_normals;
        std::string skin_error;
        if (!lightusd::tydra::EvaluateSkinnedPoints(
                stage, prim, opt.timecode, point_values, &skinned_points, &skin_error,
                (prim.GetPropertyValue("normals") || prim.GetPropertyValue("primvars:normals")) ? &skin_normals : nullptr)) {
          std::cerr << "Skinning failed: " << prim.GetPath().str()
                    << ": " << skin_error << "\n";
          return EXIT_FAILURE;
        }
        if (!skinned_points.empty())
          point_values.view = {skinned_points.data(), skinned_points.size(), false};
        const auto *pts = &point_values;
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

        // Resolve UV and normal tuples by authored interpolation and optional
        // indices. Split a shared point only when a corner needs another tuple.
        using lightusd::tydra::MeshFloatAttribute;
        using lightusd::tydra::ReadMeshFloatAttribute;
        lightusd::tydra::ValueArrayRead<int32_t> index_values, count_values;
        lightusd::tydra::ReadIntArray(prim, "faceVertexIndices", mesh_eval, &index_values);
        lightusd::tydra::ReadIntArray(prim, "faceVertexCounts", mesh_eval, &count_values);
        MeshFloatAttribute uvs, uvs1, normals;
        bool have_uvs = false;
        if (!job.uv_primvar.empty()) {
          have_uvs = ReadMeshFloatAttribute(prim, "primvars:" + job.uv_primvar,
              mesh_eval, 2, nv, index_values.size(), &uvs);
        }
        if (!have_uvs) {
          for (const std::string& name : lightusd::tydra::MeshConfig{}.uv_primvar_names) {
            if (ReadMeshFloatAttribute(prim, "primvars:" + name, mesh_eval,
                2, nv, index_values.size(), &uvs)) { have_uvs = true; break; }
          }
        }
        // Secondary UV stream: the second primvar a textured lane references.
        // A graph image node flagged with UV set 1 samples it; every other lane
        // keeps sampling the primary stream.
        bool have_uvs1 = false;
        if (!job.uv_primvar1.empty()) {
          have_uvs1 = ReadMeshFloatAttribute(prim,
              "primvars:" + job.uv_primvar1, mesh_eval, 2, nv,
              index_values.size(), &uvs1);
        }
        bool have_normals = lightusd::tydra::MeshUsesAuthoredNormals(prim) &&
            (ReadMeshFloatAttribute(prim, "primvars:normals", mesh_eval,
            3, nv, index_values.size(), &normals) ||
            ReadMeshFloatAttribute(prim, "normals", mesh_eval,
            3, nv, index_values.size(), &normals));
        std::vector<float> generated_smooth_normals;
        if (!have_normals && opt.smooth &&
        lightusd::tydra::MeshUsesGeneratedSmoothNormals(prim)) {
          const auto* orientation=prim.GetPropertyValue("orientation");
          const bool left_handed=orientation && orientation->as_token() && *orientation->as_token()=="leftHanded";
          if (lightusd::tydra::ComputeMeshSmoothNormals(point_values,count_values,index_values,
                  left_handed,&generated_smooth_normals)) {
            normals.components=3;normals.interpolation="vertex";
            normals.values.view={generated_smooth_normals.data(),generated_smooth_normals.size(),false};
            have_normals=true;
            skin_normals.clear();
          }
        }
        const auto corner_attribute = [](const MeshFloatAttribute& attribute) {
          return attribute.interpolation == "faceVarying" ||
                 attribute.interpolation == "uniform";
        };
        const bool split = (have_uvs && corner_attribute(uvs)) ||
                           (have_uvs1 && corner_attribute(uvs1)) ||
                           (have_normals && corner_attribute(normals));
        const std::vector<float> original_positions = split
            ? std::move(geo.positions) : std::vector<float>();
        geo.normals.assign(split ? 0 : size_t(nv) * 3, 0.0f);
        geo.uvs.assign(split ? 0 : size_t(nv) * 2, 0.0f);
        geo.uvs1.assign(have_uvs1 ? (split ? 0 : size_t(nv) * 2) : 0u,
                        0.0f);
        matrix4d inverse_world;
        const bool invertible = lusdr::inverse(job.world, inverse_world, 1.0e-12);
        std::map<std::array<size_t, 4>, uint32_t> corners;
        auto emit = [&](size_t corner, size_t face) {
          const int32_t point = index_values[corner];
          if (point < 0 || uint32_t(point) >= nv) return false;
          size_t uv_index = SIZE_MAX, normal_index = SIZE_MAX, uv1_index = SIZE_MAX;
          if (have_uvs && !uvs.TupleIndex(size_t(point), corner, face, &uv_index)) return false;
          if (have_uvs1 && !uvs1.TupleIndex(size_t(point), corner, face, &uv1_index)) return false;
          if (have_normals && !normals.TupleIndex(size_t(point), corner, face, &normal_index)) return false;
          uint32_t vertex = uint32_t(point);
          if (split) {
            const std::array<size_t, 4> key{size_t(point), uv_index,
                                            normal_index, uv1_index};
            const auto found = corners.find(key);
            if (found != corners.end()) { geo.indices.push_back(found->second); return true; }
            if (geo.positions.size() / 3 >= UINT32_MAX) return false;
            vertex = uint32_t(geo.positions.size() / 3);
            corners.emplace(key, vertex);
            for (size_t k = 0; k < 3; ++k) geo.positions.push_back(original_positions[size_t(point) * 3 + k]);
            geo.normals.resize(geo.positions.size(), 0.0f);
            geo.uvs.resize(size_t(vertex + 1) * 2, 0.0f);
            if (have_uvs1) geo.uvs1.resize(size_t(vertex + 1) * 2, 0.0f);
          }
          if (have_uvs) for (size_t k = 0; k < 2; ++k)
            geo.uvs[size_t(vertex) * 2 + k] = uvs.values[uv_index * 2 + k];
          if (have_uvs1) for (size_t k = 0; k < 2; ++k)
            geo.uvs1[size_t(vertex) * 2 + k] = uvs1.values[uv1_index * 2 + k];
          if (have_normals && invertible) {
            Vec3 n{normals.values[normal_index * 3], normals.values[normal_index * 3 + 1], normals.values[normal_index * 3 + 2]};
            if (!skin_normals.empty()) {
              const double* m=skin_normals.data()+size_t(point)*9;
              n={float(n.x*m[0]+n.y*m[3]+n.z*m[6]),
                 float(n.x*m[1]+n.y*m[4]+n.z*m[7]),
                 float(n.x*m[2]+n.y*m[5]+n.z*m[8])};
            }
            Vec3 wn{float(n.x * inverse_world.m[0][0] + n.y * inverse_world.m[0][1] + n.z * inverse_world.m[0][2]),
                    float(n.x * inverse_world.m[1][0] + n.y * inverse_world.m[1][1] + n.z * inverse_world.m[1][2]),
                    float(n.x * inverse_world.m[2][0] + n.y * inverse_world.m[2][1] + n.z * inverse_world.m[2][2])};
            if (Length(wn) > 1.0e-12f) wn = Normalize(wn);
            geo.normals[size_t(vertex) * 3] = wn.x;
            geo.normals[size_t(vertex) * 3 + 1] = wn.y;
            geo.normals[size_t(vertex) * 3 + 2] = wn.z;
          }
          geo.indices.push_back(vertex);
          return true;
        };
        bool valid_geometry = true;
        lightusd::tydra::ValueArrayRead<int32_t> hole_values;
        lightusd::tydra::ReadIntArray(prim, "holeIndices", mesh_eval, &hole_values);
        std::unordered_set<int32_t> holes(hole_values.begin(), hole_values.end());
        const auto visible_face = [&](size_t face) {
          return (job.subset_faces.empty() ||
                  (face < job.subset_faces.size() && job.subset_faces[face])) &&
                 !holes.count(static_cast<int32_t>(face));
        };
        // Ptex: when ANY material lane binds a Ptex atlas, record per triangle
        // (face id, three intrinsic corner uv, valid) so the descriptor shader
        // can address the face rectangle. The shader's sampleTexture() routes
        // every Ptex lane through the same hit face/UV, not only base colour.
        // Faces are the authored USD faces; quads use the ordinal uvs lusdview
        // uses (0,1)(1,1)(1,0)(0,0) and sample with y = 1 - v. Non-quad faces
        // get a zeroed (invalid) entry.
        const auto is_ptex = [&](int32_t id) {
          return id >= 0 && size_t(id) < gpu_textures.size() &&
                 gpu_textures[size_t(id)].is_ptex;
        };
        const bool want_ptex =
            is_ptex(job.tex_id) || is_ptex(job.normal_tex_id) ||
            is_ptex(job.coat_normal_tex_id) || is_ptex(job.rough_tex.id) ||
            is_ptex(job.metal_tex.id) || is_ptex(job.emission_tex_id) ||
            is_ptex(job.occ_tex.id) || is_ptex(job.opacity_tex.id) ||
            is_ptex(job.clearcoat_tex.id) ||
            is_ptex(job.clearcoat_rough_tex.id) ||
            is_ptex(job.specular_tex_id) ||
            is_ptex(job.displacement_tex.id);
        const auto push_ptex = [&](size_t face, int32_t count, int32_t k) {
          if (!want_ptex) return;
          geo.ptex_tri.push_back(count == 4 ? MakePtexTriRef(uint32_t(face), k - 1, false)
                                            : kPtexTriInvalid);
        };
        if (count_values.empty()) {
          valid_geometry = index_values.size() % 3 == 0;
          for (size_t corner = 0; valid_geometry && corner < index_values.size(); ++corner)
            if (visible_face(corner / 3)) valid_geometry = emit(corner, corner / 3);
          if (want_ptex) geo.ptex_tri.assign(geo.indices.size() / 3u, kPtexTriInvalid);
        } else {
          size_t offset = 0, face = 0;
          for (int32_t count : count_values) {
            if (count < 0 || size_t(count) > index_values.size() - offset) { valid_geometry = false; break; }
            for (int32_t k = 1; visible_face(face) && valid_geometry && k + 1 < count; ++k) {
              valid_geometry = emit(offset, face) && emit(offset + size_t(k), face) && emit(offset + size_t(k + 1), face);
              if (valid_geometry) push_ptex(face, count, k);
            }
            offset += size_t(count);
            ++face;
          }
          valid_geometry = valid_geometry && offset == index_values.size();
        }
        if (!valid_geometry) {
          std::cerr << "Invalid evaluated mesh topology or attribute: prim="
                    << prim.GetPath().str() << " time=" << opt.timecode << "\n";
          return EXIT_FAILURE;
        }
        if (split) nv = uint32_t(geo.positions.size() / 3);
        const auto* orientation=prim.GetPropertyValue("orientation");
        if (orientation && orientation->as_token() && *orientation->as_token()=="leftHanded")
          for(size_t triangle=0;triangle+2<geo.indices.size();triangle+=3) {
            std::swap(geo.indices[triangle+1],geo.indices[triangle+2]);
            if (geo.ptex_tri.size() > triangle/3 &&
                geo.ptex_tri[triangle/3] != kPtexTriInvalid)
              geo.ptex_tri[triangle/3] ^= 2u;  // winding swapped
          }

        }

        for (uint32_t index : geo.indices) {
          if (index >= nv) {
            std::cerr << "Invalid evaluated mesh topology: prim="
                      << prim.GetPath().str() << " time=" << opt.timecode
                      << " index=" << index << " vertices=" << nv << "\n";
            return EXIT_FAILURE;
          }
        }

        // Coarse displacement: resolve the bound material's inputs:displacement and
        // offset each vertex along its smooth normal (RunVulkanLightRT triangulates
        // indices as a soup and shades with geometric normals computed from these
        // positions, so displaced positions are all that is needed).
        if (opt.displace && opt.displace_scale != 0.0f && !geo.indices.empty()) {
          float disp_const = 0.0f;
          ScalarTex disp_tex;
          const std::vector<lightusd::Path> *bind =
              prim.GetRelationship("material:binding");
          if (bind && !bind->empty()) {
            lightusd::UsdPrim mat = stage.GetPrimAtPath((*bind)[0]);
            if (mat.IsValid()) {
              lightusd::UsdPrim surf =
                  ConnectedPrimCore(stage, mat, "outputs:surface");
              if (!surf.IsValid())
                surf = ConnectedPrimCore(stage, mat, "outputs:mtlx:surface");
              if (surf.IsValid()) {
                if (const lightusd::Value *d =
                        surf.GetPropertyValue("inputs:displacement"))
                  if (const float *f = d->as_float()) disp_const = *f;
                ResolveScalarTextureCore(stage, surf, "inputs:displacement", tc,
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
        if (const lightusd::Value *dcv =
                prim.GetPropertyValue("primvars:displayColor")) {
          const auto dc = dcv->as_float_array();
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
        resolved.opacity_presence = job.opacity_presence;
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
        // UsdPreviewSurface's default opacityMode is "transparent": alpha
        // attenuates the diffuse lobe and the surface stays opaque. Only
        // "presence" is true coverage; ResolveOpacityPresenceCore already
        // distinguishes them for non-PreviewSurface materials as well.
        resolved.openpbr.opacityTransparent = !job.opacity_presence;
        resolved.materialx_graph_json = job.materialx_graph_json;
        resolved.uv_primvar = job.uv_primvar;
        resolved.uv_primvar1 = job.uv_primvar1;
        resolved.uv_set_bits = job.uv_set_bits;
        for (const std::string &api : job.prim.GetMeta().apiSchemas()) {
          if (api != "MeshLightAPI") continue;
          float intensity = 1.0f, exposure = 0.0f;
          Vec3 light_color{1.0f, 1.0f, 1.0f};
          bool normalize = false;
          if (const lightusd::Value *v =
                  job.prim.GetPropertyValue("inputs:intensity"))
            if (const float *f = v->as_float()) intensity = *f;
          if (const lightusd::Value *v =
                  job.prim.GetPropertyValue("inputs:exposure"))
            if (const float *f = v->as_float()) exposure = *f;
          if (const lightusd::Value *v =
                  job.prim.GetPropertyValue("inputs:color"))
            if (const float *f = v->as_float3())
              light_color = Vec3{f[0], f[1], f[2]};
          if (const lightusd::Value *v =
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
    std::vector<CurveJobCore> gpu_curve_jobs;
    stage.Traverse([&](const lightusd::UsdPrim &prim) {
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
        CollectCurvesCoreRec(root, matrix4d::identity(),
                             lusdr::Purpose::Default, opt.timecode,
                             &gpu_curve_jobs);
      }
      if (!BuildCoreCurves(gpu_curve_ctx, gpu_curve_jobs, opt.timecode,
                           /*include_flat=*/false) ||
          !AppendGpuRoundCurves(gpu_curve_ctx.direct, &base_colors, &geos, opt)) {
        return EXIT_FAILURE;
      }
      for (const CurveJobCore &job : gpu_curve_jobs)
        has_flat_curves |= job.prim.GetPropertyValue("normals") != nullptr;
      if (has_flat_curves &&
          !BuildCoreFlatCurveBounds(gpu_curve_jobs, opt.timecode,
                                    clip_stage_loader, &gpu_flat_bounds)) {
        std::cerr << "Failed to read GPU flat/ribbon curve bounds.\n";
        return EXIT_FAILURE;
      }
    }
    // Vulkan uses the native analytic ellipse path for a pure splat scene.
    // Mixed mesh+splat scenes use the same bounded oriented-ellipse mesh
    // fallback as HIP/ROCm and D3D11 so all geometry reaches one flat trace.
    if (gpu_backend && (opt.vulkan || opt.hip || opt.cuda) && geos.empty()) {
      stage.Traverse([&](const lightusd::UsdPrim &prim) {
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
          BuildCoreGaussianEllipses(stage, gaussian_ctx, opt.timecode,
                                    /*defer_gpu_bvh=*/true) &&
          gaussian_ctx.direct.has_ellipses();
    }
    if (gpu_backend && !native_gaussian) {
      for (const auto &root : stage.GetRootPrims()) {
        CollectGpuPointsRec(stage, root, matrix4d::identity(), opt.timecode,
                            &base_colors, &geos, 0, opt);
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
    lusdr::Axis usdUp = (up_axis == 2.0) ? lusdr::Axis::Z
                           : (up_axis == 0.0) ? lusdr::Axis::X
                           : lusdr::Axis::Y;

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
    // (ResolveCameraCore): a named camera, else the USD record camera for
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
      if (FindCoreCameraFrame(stage, opt.camera, opt.timecode, &camera,
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
        camera = MakeCameraFrame(auto_opt, bounds, out_height, usdUp);
      }
    } else if (opt.autoframe) {
      camera = MakeUsdRecordCamera(bounds, usdUp, cam_width, &out_height);
    } else {
      if (out_height <= 0) out_height = 540;
      camera = MakeCameraFrame(auto_opt, bounds, out_height, usdUp);
    }

#if defined(HAVE_VULKAN) || defined(HAVE_D3D11) || defined(HAVE_HIP) || defined(HAVE_CUDA_RT)
    if ((opt.vulkan || opt.use_d3d || opt.hip || opt.cuda) && has_flat_curves) {
      if (!BuildCoreFlatCurveMeshes(gpu_curve_jobs, opt.timecode,
                                    clip_stage_loader, camera, opt, &geos,
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

    const auto make_reference_camera_light = [&]() {
      PreviewLight camera_light;
      camera_light.kind = PreviewLight::Kind::Point;
      camera_light.position = camera.origin;
      camera_light.direction = camera.forward;
      camera_light.radiance.x = 1.0f;
      camera_light.radiance.y = 1.0f;
      camera_light.radiance.z = 1.0f;
      camera_light.constant_attenuation = true;
      camera_light.shadow_enable = false;
      return camera_light;
    };
    if (opt.reference_camera_light) {
      // UsdAppUtilsFrameRecorder replaces the stage lighting with a single
      // camera-attached GlfSimpleLight (diffuse/specular 1, attenuation
      // (1,0,0) = constant) plus SCENE_AMBIENT. Reproduce that light here so
      // the paired usdrecord capture compares one lighting model. Authored
      // lights are deliberately ignored because the SDK capture ignores them.
      gpu_lights = LightCache{};
      gpu_lights.finite.push_back(make_reference_camera_light());
    } else if (opt.vulkan) {
      if (opt.distant_solid_angle) {
        for (PreviewLight &l : gpu_lights.finite) {
          if (l.kind == PreviewLight::Kind::Distant) {
            l.radiance = Mul(l.radiance, l.solid_angle_scale * opt.distant_scale);
          }
        }
      }
      if (opt.no_light_shadows) {
        for (PreviewLight &l : gpu_lights.finite) l.shadow_enable = false;
      }
      if (!opt.gpu_no_dome) {
        CollectDomeLightForGpu(stage, opt.timecode, &gpu_lights, &gpu_textures);
      }
    }
    // -referenceCameraLightAdd: the same camera light, kept IN ADDITION to the
    // authored lights (what usdrecord actually renders: its camera light plus
    // the stage lights). Independent of the replace mode above.
    if (opt.reference_camera_light_add && !opt.reference_camera_light) {
      gpu_lights.finite.push_back(make_reference_camera_light());
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
                           gpu_textures, gpu_lights, camera, out_height,
                           native_gaussian ? &gaussian_ctx.direct.ellipse_chunks : nullptr)) {
        return EXIT_FAILURE;
      }
      return EXIT_SUCCESS;
    }
#endif
    std::cerr << "Requested GPU backend not built in.\n";
    return EXIT_FAILURE;
  }
#endif

  // Every CPU render runs on the core streaming path.
  return RunRTPreviewCore(opt);
}
