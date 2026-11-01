// SPDX-License-Identifier: Apache-2.0
// lusdrender — shared LightRT GPU-backend helpers (see lusdr_gpu_common.hh).
#include "lusdr_gpu_common.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

#include "image-writer.hh"

namespace lusdr {

namespace {

// Run fn(y0, y1) over disjoint row slabs on up to `nthreads` threads. Each
// pixel's work is independent and written to a disjoint output range, so the
// result is byte-identical to the serial loop.
template <typename F>
void ParallelRows(int h, unsigned nthreads, F &&fn) {
  if (h <= 0) return;
  nthreads = std::min(nthreads, unsigned(h));
  if (nthreads <= 1) {
    fn(0, h);
    return;
  }
  const int chunk = (h + int(nthreads) - 1) / int(nthreads);
  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    const int y0 = int(t) * chunk;
    const int y1 = std::min(h, y0 + chunk);
    if (y0 >= y1) break;
    workers.emplace_back([y0, y1, &fn] { fn(y0, y1); });
  }
  for (auto &w : workers) w.join();
}

}  // namespace

bool BuildGpuTriScene(const std::vector<Vec3> &base_colors,
                      const std::vector<RTPreviewStats::MeshGeometry> &geos,
                      int threads, bool build_cpu_scene, GpuTriScene *out,
                      lrt_tri_quality quality) {
  if (!out) return false;
  auto discard = [&]() {
    if (out->scene) {
      lrt_tri_scene_free(out->scene);
      out->scene = nullptr;
    }
    std::vector<float>().swap(out->flat_verts);
    std::vector<float>().swap(out->flat_attrs);
    std::vector<uint32_t>().swap(out->flat_idx);
    std::vector<Vec3>().swap(out->base_colors);
    std::vector<uint32_t>().swap(out->material_ids);
    std::vector<float>().swap(out->tri_uvs);
    std::vector<Vec3>().swap(out->normals);
    std::vector<Vec3>().swap(out->vn0);
    std::vector<Vec3>().swap(out->vn1);
    std::vector<Vec3>().swap(out->vn2);
    out->ntris = 0;
    out->has_vertex_normals = false;
  };
  // This function is also used after backend retries. Do not append to a
  // previous failed/consumed result, and release its capacity before rebuilding
  // so a smaller chunk can actually reduce the process peak.
  discard();
  out->has_vertex_normals = false;
  // Flatten all meshes into one indexed vertex/index array LightRT can build:
  // flat_verts = unique positions, flat_idx = 3*ntris vertex ids.
  size_t total_pos = 0;
  size_t total_idx = 0;
  size_t total_vertices = 0;
  const size_t max_u32 = size_t(std::numeric_limits<uint32_t>::max());
  for (const auto &g : geos) {
    if ((g.positions.size() % 3u) != 0u) {
      std::cerr << "Invalid mesh positions: component count is not a multiple of 3.\n";
      discard();
      return false;
    }
    if ((g.indices.size() % 3u) != 0u) {
      std::cerr << "Invalid mesh indices: index count is not a multiple of 3.\n";
      discard();
      return false;
    }
    const size_t nv = g.positions.size() / 3u;
    if (nv > max_u32 || total_vertices > max_u32 - nv) {
      std::cerr << "Too many flattened vertices for 32-bit GPU indices.\n";
      discard();
      return false;
    }
    total_pos += g.positions.size();
    total_idx += g.indices.size();
    total_vertices += nv;
  }
  const size_t total_tris = total_idx / 3;
  if (total_tris > max_u32) {
    std::cerr << "Too many triangles for 32-bit GPU tracing.\n";
    discard();
    return false;
  }
  out->flat_verts.reserve(total_pos);
  out->flat_idx.reserve(total_idx);
  out->flat_attrs.reserve(total_vertices * 8u);
  out->normals.reserve(total_tris);
  for (const auto &g : geos) {
    const size_t nv = g.positions.size() / 3u;
    if (g.normals.size() == nv * 3u) {
      out->has_vertex_normals = true;
      break;
    }
  }
  if (out->has_vertex_normals) {
    out->vn0.reserve(total_tris);
    out->vn1.reserve(total_tris);
    out->vn2.reserve(total_tris);
  }
  out->base_colors.reserve(total_tris);
  uint32_t base_idx = 0;
  for (size_t mesh_idx = 0; mesh_idx < geos.size(); ++mesh_idx) {
    const auto &g = geos[mesh_idx];
    uint32_t nv = uint32_t(g.positions.size() / 3);
    for (uint32_t j = 0; j < nv; ++j) {
      const float px = g.positions[j * 3 + 0];
      const float py = g.positions[j * 3 + 1];
      const float pz = g.positions[j * 3 + 2];
      out->flat_verts.insert(out->flat_verts.end(), {px, py, pz});
      const bool has_n = g.normals.size() == size_t(nv) * 3u;
      const bool has_uv = g.uvs.size() == size_t(nv) * 2u;
      out->flat_attrs.insert(out->flat_attrs.end(),
          {px, py, pz,
           has_n ? g.normals[j * 3u] : 0.0f,
           has_n ? g.normals[j * 3u + 1u] : 0.0f,
           has_n ? g.normals[j * 3u + 2u] : 0.0f,
           has_uv ? g.uvs[j * 2u] : 0.0f,
           has_uv ? g.uvs[j * 2u + 1u] : 0.0f});
    }
    for (uint32_t j = 0; j < uint32_t(g.indices.size()); ++j) {
      if (g.indices[j] >= nv) {
        std::cerr << "Invalid mesh index " << g.indices[j] << " for "
                  << nv << " vertices.\n";
        discard();
        return false;
      }
      out->flat_idx.push_back(base_idx + g.indices[j]);
    }
    // Only trust per-vertex normals when they are exactly one-per-position
    // (USD faceVarying normals have a different layout, so fall back to flat).
    const bool perVertexN = (g.normals.size() == size_t(nv) * 3u);
    auto vnorm = [&](uint32_t vi) -> Vec3 {
      return perVertexN ? Vec3{g.normals[vi * 3 + 0], g.normals[vi * 3 + 1],
                               g.normals[vi * 3 + 2]}
                        : Vec3{0, 0, 0};
    };
    // Per-triangle shading data (flat normal + the three vertex normals).
    for (uint32_t j = 0; j < uint32_t(g.indices.size()) / 3; ++j) {
      uint32_t i0 = g.indices[j * 3 + 0];
      uint32_t i1 = g.indices[j * 3 + 1];
      uint32_t i2 = g.indices[j * 3 + 2];
      Vec3 p0{g.positions[i0 * 3 + 0], g.positions[i0 * 3 + 1], g.positions[i0 * 3 + 2]};
      Vec3 p1{g.positions[i1 * 3 + 0], g.positions[i1 * 3 + 1], g.positions[i1 * 3 + 2]};
      Vec3 p2{g.positions[i2 * 3 + 0], g.positions[i2 * 3 + 1], g.positions[i2 * 3 + 2]};
      Vec3 e1 = Sub(p1, p0), e2 = Sub(p2, p0);
      out->normals.push_back(Normalize(Cross(e1, e2)));
      if (out->has_vertex_normals) {
        out->vn0.push_back(vnorm(i0));
        out->vn1.push_back(vnorm(i1));
        out->vn2.push_back(vnorm(i2));
      }
      out->material_ids.push_back(static_cast<uint32_t>(mesh_idx));
      const bool perVertexUv = g.uvs.size() == size_t(nv) * 2u;
      const uint32_t uv_idx[3] = {i0, i1, i2};
      for (uint32_t corner : uv_idx) {
        out->tri_uvs.push_back(perVertexUv ? g.uvs[corner * 2u] : 0.0f);
        out->tri_uvs.push_back(perVertexUv ? g.uvs[corner * 2u + 1u] : 0.0f);
      }
    }
    size_t nm = (base_colors.size() > geos.size()) ? geos.size() : base_colors.size();
    Vec3 bc = mesh_idx < nm ? base_colors[mesh_idx] : Vec3{0.5f, 0.5f, 0.5f};
    for (uint32_t j = 0; j < uint32_t(g.indices.size()) / 3; ++j) {
      out->base_colors.push_back(bc);
    }
    base_idx += nv;
  }

  out->ntris = uint32_t(out->flat_idx.size() / 3);
  if (out->ntris == 0) {
    std::cerr << "No triangles to render.\n";
    discard();
    return false;
  }
  if (!build_cpu_scene) return true;
  if (!BuildGpuCpuScene(threads, out, quality)) {
    discard();
    return false;
  }
  return true;
}

void AppendGpuSmoothNormals(const GpuTriScene &src, GpuTriScene *dst) {
  if (!dst || (!src.has_vertex_normals && !dst->has_vertex_normals)) return;
  const size_t prior = dst->base_colors.size();
  if (!dst->has_vertex_normals) {
    dst->vn0.assign(prior, Vec3{0.0f, 0.0f, 0.0f});
    dst->vn1.assign(prior, Vec3{0.0f, 0.0f, 0.0f});
    dst->vn2.assign(prior, Vec3{0.0f, 0.0f, 0.0f});
    dst->has_vertex_normals = true;
  }
  if (src.has_vertex_normals) {
    dst->vn0.insert(dst->vn0.end(), src.vn0.begin(), src.vn0.end());
    dst->vn1.insert(dst->vn1.end(), src.vn1.begin(), src.vn1.end());
    dst->vn2.insert(dst->vn2.end(), src.vn2.begin(), src.vn2.end());
  } else {
    dst->vn0.insert(dst->vn0.end(), src.ntris, Vec3{0.0f, 0.0f, 0.0f});
    dst->vn1.insert(dst->vn1.end(), src.ntris, Vec3{0.0f, 0.0f, 0.0f});
    dst->vn2.insert(dst->vn2.end(), src.ntris, Vec3{0.0f, 0.0f, 0.0f});
  }
}

bool ValidateGpuTriangleCount(size_t ntris, const char *backend) {
  if (ntris <= size_t(std::numeric_limits<uint32_t>::max())) return true;
  std::cerr << (backend ? backend : "GPU")
            << " scene has " << ntris
            << " triangles; 32-bit GPU hit IDs cannot represent this scene.\n";
  return false;
}

bool BuildGpuTriChunk(
    const std::vector<Vec3> &base_colors,
    std::vector<RTPreviewStats::MeshGeometry> &geos, size_t limit,
    size_t *mesh_index, size_t *tri_index,
    std::vector<Vec3> *chunk_colors,
    std::vector<RTPreviewStats::MeshGeometry> *chunk_geos,
    size_t *chunk_triangles) {
  if (!mesh_index || !tri_index || !chunk_colors || !chunk_geos ||
      !chunk_triangles || limit == 0) return false;
  chunk_colors->clear();
  chunk_geos->clear();
  *chunk_triangles = 0;
  while (*mesh_index < geos.size() && *chunk_triangles < limit) {
    const size_t source_mesh = *mesh_index;
    const auto &src = geos[source_mesh];
    const size_t nv = src.positions.size() / 3u;
    const size_t ntri = src.indices.size() / 3u;
    if (src.positions.size() % 3u != 0u || src.indices.size() % 3u != 0u) {
      std::cerr << "Invalid GPU mesh chunk source at mesh " << *mesh_index
                << ".\n";
      return false;
    }
    if (*tri_index >= ntri) {
      if (ntri == 0) {
        RTPreviewStats::MeshGeometry &released = geos[source_mesh];
        std::vector<float>().swap(released.positions);
        std::vector<float>().swap(released.normals);
        std::vector<float>().swap(released.uvs);
        std::vector<uint32_t>().swap(released.indices);
      }
      ++*mesh_index;
      *tri_index = 0;
      continue;
    }
    const size_t remaining = limit - *chunk_triangles;
    if (*tri_index == 0 && ntri <= remaining &&
        src.positions.size() % 9u == 0u &&
        src.positions.size() / 9u == ntri &&
        src.indices.size() % 3u == 0u &&
        src.indices.size() / 3u == ntri) {
      // Curve/point fallback tessellation emits a complete bounded mesh with
      // identity indices. Transfer that already-sized carrier directly into
      // the upload batch; copying it again would briefly retain two full
      // position/index streams.
      bool identity = true;
      for (size_t i = 0; i < src.indices.size(); ++i) {
        if (src.indices[i] != i) {
          identity = false;
          break;
        }
      }
      if (identity) {
        chunk_geos->push_back(std::move(geos[source_mesh]));
        chunk_colors->push_back(
            source_mesh < base_colors.size() ? base_colors[source_mesh]
                                             : Vec3{0.5f, 0.5f, 0.5f});
        ++*mesh_index;
        *chunk_triangles += ntri;
        continue;
      }
    }
    const size_t take = std::min(limit - *chunk_triangles, ntri - *tri_index);
    RTPreviewStats::MeshGeometry dst;
    const bool per_vertex_normals = src.normals.size() == nv * 3u;
    const size_t first_index = *tri_index * 3u;
    const size_t index_count = take * 3u;
    dst.positions.reserve(index_count * 3u);
    dst.indices.reserve(index_count);
    if (per_vertex_normals) dst.normals.reserve(index_count * 3u);
    bool sequential = first_index + index_count <= src.indices.size() &&
                      first_index + index_count <= nv;
    for (size_t i = 0; sequential && i < index_count; ++i) {
      if (src.indices[first_index + i] != first_index + i) sequential = false;
    }
    if (sequential) {
      // Curve and point fallback tessellation emits independent triangles
      // with identity indices. Copy that contiguous range directly instead
      // of paying for a hash-map remap on every triangle.
      const size_t vertex_begin = first_index;
      const size_t vertex_count = index_count;
      dst.positions.insert(dst.positions.end(),
                           src.positions.begin() + vertex_begin * 3u,
                           src.positions.begin() + (vertex_begin + vertex_count) * 3u);
      if (per_vertex_normals) {
        dst.normals.insert(dst.normals.end(),
                           src.normals.begin() + vertex_begin * 3u,
                           src.normals.begin() + (vertex_begin + vertex_count) * 3u);
      }
      dst.indices.resize(index_count);
      for (size_t i = 0; i < index_count; ++i)
        dst.indices[i] = static_cast<uint32_t>(i);
    } else {
      dst.normals.reserve(std::min(nv, take * 3u) * 3u);
      // Only vertices touched by this bounded chunk need a remap. A dense
      // vertex-count array made a tiny chunk retain an additional allocation
      // proportional to a potentially enormous source mesh.
      std::unordered_map<uint32_t, uint32_t> remap;
      remap.reserve(std::min(nv, take * size_t(3)));
      for (size_t t = 0; t < take; ++t) {
        const size_t src_tri = *tri_index + t;
        for (size_t k = 0; k < 3; ++k) {
          const uint32_t old_id = src.indices[src_tri * 3u + k];
          if (old_id >= nv) {
            std::cerr << "Invalid GPU mesh index " << old_id << " at mesh "
                      << *mesh_index << ".\n";
            return false;
          }
          auto remap_it = remap.find(old_id);
          uint32_t new_id = 0;
          if (remap_it == remap.end()) {
            new_id = static_cast<uint32_t>(dst.positions.size() / 3u);
            remap.emplace(old_id, new_id);
            dst.positions.insert(dst.positions.end(),
                                 src.positions.begin() + old_id * 3u,
                                 src.positions.begin() + old_id * 3u + 3u);
            if (per_vertex_normals) {
              dst.normals.insert(dst.normals.end(),
                                 src.normals.begin() + old_id * 3u,
                                 src.normals.begin() + old_id * 3u + 3u);
            }
          } else {
            new_id = remap_it->second;
          }
          dst.indices.push_back(new_id);
        }
      }
    }
    chunk_geos->push_back(std::move(dst));
    chunk_colors->push_back(
        source_mesh < base_colors.size() ? base_colors[source_mesh]
                                         : Vec3{0.5f, 0.5f, 0.5f});
    *tri_index += take;
    *chunk_triangles += take;
    if (*tri_index == ntri) {
      RTPreviewStats::MeshGeometry &released = geos[source_mesh];
      std::vector<float>().swap(released.positions);
      std::vector<float>().swap(released.normals);
      std::vector<float>().swap(released.uvs);
      std::vector<uint32_t>().swap(released.indices);
      ++*mesh_index;
      *tri_index = 0;
    }
  }
  return *chunk_triangles != 0;
}

bool BuildGpuCpuScene(int threads, GpuTriScene *out, lrt_tri_quality quality) {
  if (!out || out->ntris == 0 || out->flat_verts.empty() || out->flat_idx.empty()) {
    std::cerr << "No flattened triangles for LightRT scene build.\n";
    return false;
  }
  if (out->scene) return true;

  lrt_tri_build_options bopts;
  std::memset(&bopts, 0, sizeof(bopts));
  bopts.quality = quality;
  bopts.layout = LRT_TRI_LAYOUT_BVH4;
  bopts.num_threads = WorkerThreadCount(threads);

  lrt_result lrterr = LRT_RESULT_OK;
  out->scene = lrt_tri_scene_build_indexed(out->flat_verts.data(),
                                           out->flat_verts.size() / 3,
                                           out->flat_idx.data(), out->ntris,
                                           &bopts, &lrterr);
  if (!out->scene || lrterr != LRT_RESULT_OK) {
    // Some backends can return a handle together with a non-OK diagnostic.
    // Never leave that partial BVH attached to the scene: callers may retry
    // with a smaller chunk and would otherwise retain the failed allocation.
    if (out->scene) {
      lrt_tri_scene_free(out->scene);
      out->scene = nullptr;
    }
    std::cerr << "Failed to build LightRT scene (err=" << int(lrterr)
              << ", vertices=" << (out->flat_verts.size() / 3)
              << ", triangles=" << out->ntris << ").\n";
    return false;
  }
  return true;
}

bool ValidateGpuFrameSize(int w, int h, int spp, const char *backend,
                          size_t *nrays) {
  const char *name = backend ? backend : "GPU";
  if (w <= 0 || h <= 0 || spp <= 0) {
    std::cerr << name << " invalid frame size: " << w << "x" << h
              << " spp=" << spp << "\n";
    return false;
  }
  const size_t sw = size_t(w);
  const size_t sh = size_t(h);
  const size_t ss = size_t(spp);
  const size_t max_rays = size_t(std::numeric_limits<uint32_t>::max());
  if (sw > max_rays / sh) {
    std::cerr << name << " frame is too large for 32-bit GPU ray count: "
              << w << "x" << h << " spp=" << spp << "\n";
    return false;
  }
  const size_t npix = sw * sh;
  if (npix > max_rays / ss) {
    std::cerr << name << " sample count is too large for 32-bit GPU ray count: "
              << w << "x" << h << " spp=" << spp << "\n";
    return false;
  }
  if (nrays) *nrays = npix * ss;
  return true;
}

bool ShadeAndWriteImageInstanced(const Options &opt, const GpuInstancedScene &s,
                                 const std::vector<lrt_ray> &rays,
                                 const std::vector<InstSampleHit> &hits, int w,
                                 int h, int spp) {
  const float ambient = opt.ambient;
  const Vec3 light = Normalize(Vec3{0.5f, 0.8f, 0.6f});

  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.bpp = 8;
  img.data.resize(size_t(w) * size_t(h) * 4, 0);

  // Apply a row-major 3x3 (normal matrix) to an object-space normal.
  auto xform_n = [](const float m[9], const Vec3 &n) -> Vec3 {
    return Vec3{m[0] * n.x + m[1] * n.y + m[2] * n.z,
                m[3] * n.x + m[4] * n.y + m[5] * n.z,
                m[6] * n.x + m[7] * n.y + m[8] * n.z};
  };

  ParallelRows(h, WorkerThreadCount(opt.threads), [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
      for (int x = 0; x < w; ++x) {
        size_t base = (size_t(y) * size_t(w) + size_t(x)) * size_t(spp);
        Vec3 color{0, 0, 0};
        for (int sp = 0; sp < spp; ++sp) {
          const InstSampleHit &hit = hits[base + size_t(sp)];
          if (!hit.valid) continue;
          const uint32_t inst = hit.inst;
          const uint32_t local = hit.local;
          if (inst >= s.insts.size()) continue;
          const GpuInstPlacement &pl = s.insts[inst];
          if (pl.proto >= s.protos.size()) continue;
          const GpuInstProto &pr = s.protos[pl.proto];
          if (local >= pr.ntris) continue;

          Vec3 bc = pr.base_color;
          // Smooth normal in prototype space, then transform to world.
          Vec3 Nobj = pr.normals[local];
          const float w0 = 1.0f - hit.u - hit.v, w1 = hit.u, w2 = hit.v;
          Vec3 sn = Add(Add(Mul(pr.vn0[local], w0), Mul(pr.vn1[local], w1)),
                        Mul(pr.vn2[local], w2));
          if (Length(sn) > 1.0e-8f) Nobj = sn;
          Vec3 N = xform_n(pl.n2w, Nobj);
          if (Length(N) > 1.0e-8f) N = Normalize(N);
          Vec3 V = Vec3{-rays[base + size_t(sp)].dir[0],
                        -rays[base + size_t(sp)].dir[1],
                        -rays[base + size_t(sp)].dir[2]};
          if (Dot(N, V) < 0.0f) N = Mul(N, -1.0f);
          float key = std::max(0.0f, Dot(N, light));
          float head = std::max(0.0f, Dot(N, V));
          float lit = ambient + 0.8f * key + 0.35f * head;
          color = Add(color, Mul(bc, lit));
        }
        color = Mul(color, 1.0f / float(spp));
        size_t pi = (size_t(y) * size_t(w) + size_t(x)) * 4;
        img.data[pi + 0] = uint8_t(std::min(255.0f, color.x * 255.0f));
        img.data[pi + 1] = uint8_t(std::min(255.0f, color.y * 255.0f));
        img.data[pi + 2] = uint8_t(std::min(255.0f, color.z * 255.0f));
        img.data[pi + 3] = 255;
      }
    }
  });

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return false;
  }
  return true;
}

void GenerateCameraRays(const CameraFrame &camera, int w, int h, int spp,
                        std::vector<lrt_ray> *rays) {
  Vec3 eye = camera.origin;
  Vec3 fwd = camera.forward;
  Vec3 up = camera.up;
  Vec3 right = Normalize(Cross(fwd, up));
  Vec3 camUp = Cross(right, fwd);
  float aspect = float(w) / float(h);
  float half_h = std::tan(camera.yfov * 0.5f);
  float half_w = half_h * aspect;

  const size_t nrays = size_t(w) * size_t(h) * size_t(spp);
  rays->resize(nrays);
  ParallelRows(h, std::thread::hardware_concurrency(), [&](int yBegin, int yEnd) {
  for (int y = yBegin; y < yEnd; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t base = (size_t(y) * size_t(w) + size_t(x)) * size_t(spp);
      for (int s = 0; s < spp; ++s) {
        float jx, jy;
        PixelJitter(s, spp, &jx, &jy);
        float fx = (float(x) + jx) / float(w) * 2.0f - 1.0f;
        float fy = (float(y) + jy) / float(h) * 2.0f - 1.0f;
        Vec3 dir = Add(Mul(right, fx * half_w), Add(Mul(camUp, fy * half_h), fwd));
        dir = Normalize(dir);
        lrt_ray &ray = (*rays)[base + size_t(s)];
        ray.org[0] = eye.x; ray.org[1] = eye.y; ray.org[2] = eye.z;
        ray.tmin = 0.001f;
        ray.dir[0] = dir.x; ray.dir[1] = dir.y; ray.dir[2] = dir.z;
        ray.tmax = 1.0e10f;
      }
    }
  }
  });
}

bool ShadeAndWriteImage(const Options &opt, const GpuTriScene &s,
                        const std::vector<lrt_ray> &rays,
                        const std::vector<lrt_hit> &hits, int w, int h, int spp,
                        const std::vector<ResolvedMat> *materials,
                        const std::vector<Texture> *textures) {
  const float ambient = opt.ambient;
  const Vec3 light = Normalize(Vec3{0.5f, 0.8f, 0.6f});

  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.bpp = 8;
  img.data.resize(size_t(w) * size_t(h) * 4, 0);

  ParallelRows(h, WorkerThreadCount(opt.threads), [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
      for (int x = 0; x < w; ++x) {
        size_t base = (size_t(y) * size_t(w) + size_t(x)) * size_t(spp);
        Vec3 color{0, 0, 0};
        for (int sp = 0; sp < spp; ++sp) {
          const lrt_hit &hit = hits[base + size_t(sp)];
          if (hit.prim_id != 0xFFFFFFFFu && hit.prim_id < s.ntris) {
            Vec3 bc = hit.prim_id < s.base_colors.size()
                          ? s.base_colors[hit.prim_id]
                          : Vec3{0.5f, 0.5f, 0.5f};
            const ResolvedMat *mat = nullptr;
            if (materials && hit.prim_id < s.material_ids.size() &&
                s.material_ids[hit.prim_id] < materials->size()) {
              mat = &(*materials)[s.material_ids[hit.prim_id]];
              bc = mat->base_color;
              if (mat->tex_id >= 0 && textures &&
                  static_cast<size_t>(mat->tex_id) < textures->size() &&
                  hit.prim_id * 6u + 5u < s.tri_uvs.size()) {
                const size_t uv = static_cast<size_t>(hit.prim_id) * 6u;
                const float w0 = 1.0f - hit.u - hit.v;
                float u = s.tri_uvs[uv] * w0 + s.tri_uvs[uv + 2u] * hit.u +
                          s.tri_uvs[uv + 4u] * hit.v;
                float v = s.tri_uvs[uv + 1u] * w0 +
                          s.tri_uvs[uv + 3u] * hit.u +
                          s.tri_uvs[uv + 5u] * hit.v;
                mat->uv_xform.apply(&u, &v);
                const Vec3 texel = (*textures)[size_t(mat->tex_id)].sample(u, v);
                bc = Vec3{bc.x * texel.x, bc.y * texel.y, bc.z * texel.z};
              }
            }
            // Smooth normal: barycentric-interpolate the triangle's vertex normals
            // (hit.u, hit.v are the v1/v2 weights). Fall back to the flat face
            // normal when vertex normals are absent/degenerate.
            Vec3 N = hit.prim_id < s.normals.size()
                         ? s.normals[hit.prim_id]
                         : Vec3{0.0f, 1.0f, 0.0f};
            if (s.has_vertex_normals && hit.prim_id < s.vn0.size() &&
                hit.prim_id < s.vn1.size() && hit.prim_id < s.vn2.size()) {
              const float w0 = 1.0f - hit.u - hit.v, w1 = hit.u, w2 = hit.v;
              Vec3 sn = Add(Add(Mul(s.vn0[hit.prim_id], w0),
                                Mul(s.vn1[hit.prim_id], w1)),
                            Mul(s.vn2[hit.prim_id], w2));
              if (Length(sn) > 1.0e-8f) N = Normalize(sn);
            }
            // Orient the normal toward the camera so a surface visible to the eye
            // is never left unlit by a back-facing normal (USD winding/normals are
            // not guaranteed consistent for a quick preview).
            Vec3 V = Vec3{-rays[base + size_t(sp)].dir[0],
                          -rays[base + size_t(sp)].dir[1],
                          -rays[base + size_t(sp)].dir[2]};
            if (Dot(N, V) < 0.0f) N = Mul(N, -1.0f);
            // Fixed key light + a dim camera headlight so the silhouette reads even
            // when the key grazes the surface; ambient lifts the shadow terminator.
            float key = std::max(0.0f, Dot(N, light));
            float head = std::max(0.0f, Dot(N, V));
            const float roughness = mat ? mat->roughness : 0.55f;
            const float metallic = mat ? mat->metallic : 0.0f;
            const float diffuse = (ambient + 0.8f * key + 0.25f * head) *
                                  (1.0f - metallic);
            const float specular = std::pow(head, 2.0f +
                126.0f * (1.0f - std::max(0.0f, std::min(1.0f, roughness))));
            Vec3 shaded = Add(Mul(bc, diffuse),
                              Mul(Vec3{1.0f, 1.0f, 1.0f},
                                  specular * (0.04f + 0.96f * metallic)));
            if (mat) shaded = Add(shaded, mat->emission);
            color = Add(color, shaded);
          }
        }
        color = Mul(color, 1.0f / float(spp));
        size_t pi = (size_t(y) * size_t(w) + size_t(x)) * 4;
        img.data[pi + 0] = uint8_t(std::min(255.0f, color.x * 255.0f));
        img.data[pi + 1] = uint8_t(std::min(255.0f, color.y * 255.0f));
        img.data[pi + 2] = uint8_t(std::min(255.0f, color.z * 255.0f));
        img.data[pi + 3] = 255;
      }
    }
  });

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return false;
  }
  return true;
}

bool ShadeAndWriteGaussianImage(
    const Options &opt, const std::vector<EllipseSceneChunk> &chunks,
    const std::vector<lrt_ray> &rays, const std::vector<lrt_hit> &hits, int w,
    int h, int spp) {
  const float ambient = opt.ambient;
  const Vec3 light = Normalize(Vec3{0.5f, 0.8f, 0.6f});
  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = 4;
  img.bpp = 8;
  img.data.resize(size_t(w) * size_t(h) * 4, 0);

  auto find_info = [&](uint32_t prim_id) -> const TriInfo * {
    size_t lo = 0, hi = chunks.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (chunks[mid].first <= prim_id)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == 0) return nullptr;
    const EllipseSceneChunk &chunk = chunks[lo - 1];
    const size_t local = size_t(prim_id) - chunk.first;
    return local < chunk.info.size() ? &chunk.info[local] : nullptr;
  };

  ParallelRows(h, WorkerThreadCount(opt.threads), [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t base = (size_t(y) * size_t(w) + size_t(x)) * size_t(spp);
        Vec3 color{0, 0, 0};
        for (int sp = 0; sp < spp; ++sp) {
          const lrt_hit &hit = hits[base + size_t(sp)];
          if (hit.prim_id == 0xFFFFFFFFu) {color=Add(color,opt.bg);continue;}
          const TriInfo *ti = find_info(hit.prim_id);
          if (!ti) continue;
          Vec3 normal = Normalize(ti->p1);
          if (Length(normal) < 1.0e-8f) normal = Vec3{0.0f, 1.0f, 0.0f};
          Vec3 view{-rays[base + size_t(sp)].dir[0],
                    -rays[base + size_t(sp)].dir[1],
                    -rays[base + size_t(sp)].dir[2]};
          if (Dot(normal, view) < 0.0f) normal = Mul(normal, -1.0f);
          const float key = std::max(0.0f, Dot(normal, light));
          const float head = std::max(0.0f, Dot(normal, view));
          color = Add(color, Mul(ti->base_color,
                                 ambient + 0.8f * key + 0.35f * head));
        }
        color = Mul(color, 1.0f / float(spp));
        const size_t pi = (size_t(y) * size_t(w) + size_t(x)) * 4;
        img.data[pi + 0] = uint8_t(std::min(255.0f, color.x * 255.0f));
        img.data[pi + 1] = uint8_t(std::min(255.0f, color.y * 255.0f));
        img.data[pi + 2] = uint8_t(std::min(255.0f, color.z * 255.0f));
        img.data[pi + 3] = 255;
      }
    }
  });
  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write image: " << ret.error() << "\n";
    return false;
  }
  return true;
}

}  // namespace lusdr
