// SPDX-License-Identifier: Apache-2.0
// lusdrender — LightRT Direct3D 11 backend (GPU BVH traversal via
// lightrt_c_d3d11). Compiles to nothing unless HAVE_D3D11 is defined.
//
// Unlike lusdr_vulkan.cc (one GPU trace call per pixel), this batches every
// primary ray into one lrt_d3d11_trace_scene() dispatch per bounded geometry
// chunk, keeping device and BVH residency bounded for large scenes.
#ifdef HAVE_D3D11
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include "image-writer.hh"
#include "lightrt_c_d3d11.h"
#include "lusdr_context.hh"
#include "lusdr_gpu_common.hh"

namespace lusdr {

bool RunD3D11LightRT(const Options &opt, const std::vector<Vec3> &base_colors,
                     std::vector<RTPreviewStats::MeshGeometry> &geos,
                     const CameraFrame &camera, int height) {
  lrt_result d3derr = LRT_RESULT_OK;
  lrt_d3d11_engine *dev = lrt_d3d11_engine_create(/*prefer_discrete=*/1, &d3derr);
  if (!dev) {
    std::cerr << "Failed to create LightRT Direct3D 11 engine.\n";
    return false;
  }
  std::cerr << "Direct3D 11 device: " << lrt_d3d11_engine_device_name(dev) << "\n";

  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "Direct3D 11", &nrays)) {
    lrt_d3d11_engine_destroy(dev);
    return false;
  }
  const uint32_t ray_count = uint32_t(nrays);

  // Camera basis.
  Vec3 eye = camera.origin, fwd = camera.forward, up = camera.up;
  Vec3 right = Normalize(Cross(fwd, up));
  Vec3 camUp = Cross(right, fwd);
  float aspect = float(w) / float(h);
  float half_h = std::tan(camera.yfov * 0.5f);
  float half_w = half_h * aspect;

  // Build every primary ray for the frame (spp samples per pixel), then trace
  // them all in one GPU dispatch.
  const size_t npix = size_t(w) * size_t(h);
  std::vector<lrt_ray> rays(nrays);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int s = 0; s < spp; ++s) {
        float fx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f;
        float fy = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
        Vec3 dir = Normalize(
            Add(Mul(right, fx * half_w), Add(Mul(camUp, fy * half_h), fwd)));
        lrt_ray &r = rays[(size_t(y) * w + x) * spp + s];
        r.org[0] = eye.x; r.org[1] = eye.y; r.org[2] = eye.z; r.tmin = 0.001f;
        r.dir[0] = dir.x; r.dir[1] = dir.y; r.dir[2] = dir.z; r.tmax = 1.0e10f;
      }
    }
  }

  std::vector<lrt_hit> hits(nrays);
  for (lrt_hit &hit : hits) {
    hit.prim_id = LRT_TRI_NO_HIT;
    hit.t = std::numeric_limits<float>::infinity();
  }

  const size_t chunk_limit = opt.gpu_triangle_chunk;

  size_t mesh_index = 0, tri_index = 0, global_first = 0, chunk_count = 0;
  std::vector<Vec3> chunk_colors;
  std::vector<RTPreviewStats::MeshGeometry> chunk_geos;
  std::vector<lrt_hit> chunk_hits(nrays);
  std::vector<Vec3> tri_base_colors;
  std::vector<Vec3> tri_normals;
  size_t total_tris = 0;
  size_t source_tris = 0;
  for (const RTPreviewStats::MeshGeometry &geo : geos) {
    const size_t n = geo.indices.size() / 3u;
    if (source_tris > (std::numeric_limits<size_t>::max)() - n) {
      std::cerr << "Direct3D 11 triangle count overflow while preparing chunks.\n";
      lrt_d3d11_engine_destroy(dev);
      return false;
    }
    source_tris += n;
  }
  if (!ValidateGpuTriangleCount(source_tris, "Direct3D 11")) {
    lrt_d3d11_engine_destroy(dev);
    return false;
  }
  while (true) {
    size_t chunk_tris = 0;
    if (!BuildGpuTriChunk(base_colors, geos, chunk_limit, &mesh_index,
                          &tri_index, &chunk_colors, &chunk_geos,
                          &chunk_tris))
      break;
    GpuTriScene chunk;
    if (!BuildGpuTriScene(chunk_colors, chunk_geos, opt.threads,
                          /*build_cpu_scene=*/false, &chunk)) {
      lrt_d3d11_engine_destroy(dev);
      return false;
    }
    if (!BuildGpuCpuScene(opt.threads, &chunk)) {
      if (chunk.scene) lrt_tri_scene_free(chunk.scene);
      lrt_d3d11_engine_destroy(dev);
      return false;
    }
    chunk_hits.assign(nrays, lrt_hit{});
    lrt_result trerr = LRT_RESULT_OK;
    int traced = lrt_d3d11_trace_scene(dev, chunk.scene, rays.data(),
                                       ray_count, chunk_hits.data(), &trerr);
    if (traced < 0) {
      std::cerr << "Direct3D 11 trace failed for chunk [" << global_first
                << ", " << (global_first + chunk_tris) << "]: "
                << lrt_d3d11_engine_last_error(dev) << "\n";
      lrt_tri_scene_free(chunk.scene);
      lrt_d3d11_engine_destroy(dev);
      return false;
    }
    for (size_t i = 0; i < nrays; ++i) {
      if (chunk_hits[i].prim_id != LRT_TRI_NO_HIT &&
          chunk_hits[i].t < hits[i].t) {
        hits[i] = chunk_hits[i];
        hits[i].prim_id += static_cast<uint32_t>(global_first);
      }
    }
    tri_base_colors.insert(tri_base_colors.end(), chunk.base_colors.begin(),
                           chunk.base_colors.end());
    tri_normals.insert(tri_normals.end(), chunk.normals.begin(),
                       chunk.normals.end());
    lrt_tri_scene_free(chunk.scene);
    global_first += chunk_tris;
    total_tris += chunk_tris;
    ++chunk_count;
  }
  if (total_tris == 0 ||
      total_tris > size_t(std::numeric_limits<uint32_t>::max())) {
    std::cerr << "No triangles to render.\n";
    lrt_d3d11_engine_destroy(dev);
    return false;
  }

  // Shade.
  lightusd::Image img;
  img.width = w; img.height = h; img.channels = 4; img.bpp = 8;
  img.data.resize(npix * 4, 0);
  const float ambient = opt.ambient;
  const Vec3 L = Normalize(Vec3{0.5f, 0.8f, 0.6f});
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Vec3 color{0, 0, 0};
      for (int s = 0; s < spp; ++s) {
        const lrt_hit &hit = hits[(size_t(y) * w + x) * spp + s];
        if (hit.prim_id != LRT_TRI_NO_HIT && hit.prim_id < total_tris) {
          Vec3 bc = hit.prim_id < tri_base_colors.size()
                        ? tri_base_colors[hit.prim_id] : Vec3{0.5f, 0.5f, 0.5f};
          Vec3 N = hit.prim_id < tri_normals.size()
                       ? tri_normals[hit.prim_id] : Vec3{0, 1, 0};
          float diff = std::max(0.0f, Dot(N, L));
          color = Add(color, Add(Mul(bc, diff), Mul(bc, ambient)));
        }
      }
      color = Mul(color, 1.0f / float(spp));
      size_t pi = (size_t(y) * w + x) * 4;
      img.data[pi + 0] = uint8_t(std::min(255.0f, color.x * 255.0f));
      img.data[pi + 1] = uint8_t(std::min(255.0f, color.y * 255.0f));
      img.data[pi + 2] = uint8_t(std::min(255.0f, color.z * 255.0f));
      img.data[pi + 3] = 255;
    }
  }

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::Autodetect;
  auto ret = lightusd::image::WriteImageToFile(opt.output, img, wopt);
  if (!ret) {
    std::cerr << "Failed to write PNG: " << ret.error() << "\n";
    lrt_d3d11_engine_destroy(dev);
    return false;
  }

  std::cerr << "triangles: " << total_tris << " (" << geos.size()
            << " source meshes, " << chunk_count << " GPU chunks)\n";
  std::cerr << "backend: LightRT D3D11 (compute trace, " << nrays
            << " rays per chunk)\n";
  lrt_d3d11_engine_destroy(dev);
  return true;
}

}  // namespace lusdr
#endif  // HAVE_D3D11
