// SPDX-License-Identifier: Apache-2.0
// lusdrender — LightRT HIP/ROCm backend (GPU BVH traversal via lightrt_c_hip).
// Compiles to nothing unless HAVE_HIP is defined. Geometry flatten, ray
// generation and shading are shared with the Vulkan backend (lusdr_gpu_common):
// build a CPU BVH, upload it, trace every primary ray in one GPU dispatch, then
// shade on the CPU.
#ifdef HAVE_HIP
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

#include "lightrt_c_hip.h"
#include "lusdr_context.hh"
#include "lusdr_gpu_common.hh"

namespace lusdr {

namespace {

double HipSecsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

size_t HipGpuChunkLimit() {
  size_t limit = size_t(262144);
  if (const char *s = std::getenv("LUSDR_GPU_TRIANGLE_CHUNK")) {
    char *end = nullptr;
    const unsigned long long n = std::strtoull(s, &end, 10);
    if (end != s && n > 0) limit = static_cast<size_t>(n);
  }
  return limit;
}

size_t GpuTriangleCount(
    const std::vector<RTPreviewStats::MeshGeometry> &geos) {
  size_t n = 0;
  for (const auto &geo : geos) n += geo.indices.size() / 3u;
  return n;
}

void InitMissHits(std::vector<lrt_hit> *hits) {
  for (lrt_hit &hit : *hits) {
    hit.t = std::numeric_limits<float>::max();
    hit.u = hit.v = 0.0f;
    hit.prim_id = LRT_TRI_NO_HIT;
  }
}

bool RunHipLightRTChunked(
    const Options &opt, const std::vector<Vec3> &base_colors,
    std::vector<RTPreviewStats::MeshGeometry> &geos,
    const CameraFrame &camera, int height, size_t chunk_limit) {
  lrt_hip_engine_options hopts;
  std::memset(&hopts, 0, sizeof(hopts));
  hopts.device_index = -1;
  lrt_result hiperr = LRT_RESULT_OK;
  lrt_hip_engine *hip = lrt_hip_engine_create(&hopts, &hiperr);
  if (!hip) {
    std::cerr << "HIP ray tracing unavailable (rc=" << hiperr
              << "); no ROCm device or kernel compile failed.\n";
    return false;
  }
  std::cerr << "HIP device: " << lrt_hip_engine_device_name(hip)
            << ", mesh chunks=" << chunk_limit << " tris\n";
  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height > 0 ? height : 540;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "HIP", &nrays)) {
    lrt_hip_engine_destroy(hip);
    return false;
  }
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);
  std::vector<lrt_hit> hits(nrays), chunk_hits(nrays);
  InitMissHits(&hits);
  GpuTriScene shade;
  const size_t total_triangles = GpuTriangleCount(geos);
  if (!ValidateGpuTriangleCount(total_triangles, "HIP")) {
    lrt_hip_engine_destroy(hip);
    return false;
  }
  shade.ntris = static_cast<uint32_t>(total_triangles);
  size_t mesh = 0, tri = 0, global_first = 0, chunk_count = 0;
  double flatten_s = 0.0, trace_s = 0.0;
  while (mesh < geos.size()) {
    std::vector<Vec3> chunk_colors;
    std::vector<RTPreviewStats::MeshGeometry> chunk_geos;
    size_t chunk_tris = 0;
    if (!BuildGpuTriChunk(base_colors, geos, chunk_limit, &mesh, &tri,
                          &chunk_colors, &chunk_geos, &chunk_tris)) {
      lrt_hip_engine_destroy(hip);
      return false;
    }
    GpuTriScene scene;
    const auto flatten_t0 = std::chrono::steady_clock::now();
    if (!BuildGpuTriScene(chunk_colors, chunk_geos, opt.threads, true, &scene,
                          opt.quality)) {
      std::cerr << "HIP mesh chunk build failed [" << global_first << ", "
                << (global_first + chunk_tris)
                << "]; reduce LUSDR_GPU_TRIANGLE_CHUNK or inspect source data.\n";
      lrt_hip_engine_destroy(hip);
      return false;
    }
    flatten_s += HipSecsSince(flatten_t0);
    shade.base_colors.insert(shade.base_colors.end(), scene.base_colors.begin(),
                             scene.base_colors.end());
    shade.normals.insert(shade.normals.end(), scene.normals.begin(),
                         scene.normals.end());
    AppendGpuSmoothNormals(scene, &shade, global_first);
    InitMissHits(&chunk_hits);
    lrt_result trace_err = LRT_RESULT_OK;
    const auto trace_t0 = std::chrono::steady_clock::now();
    const int traced = lrt_hip_trace_scene(
        hip, scene.scene, rays.data(), static_cast<uint32_t>(nrays),
        chunk_hits.data(), &trace_err);
    if (traced < 0) {
      std::cerr << "HIP mesh chunk trace failed [" << global_first << ", "
                << (global_first + chunk_tris) << "] (rc=" << trace_err
                << "): " << lrt_hip_engine_last_error(hip) << "\n";
      if (scene.scene) lrt_tri_scene_free(scene.scene);
      lrt_hip_engine_destroy(hip);
      return false;
    }
    trace_s += HipSecsSince(trace_t0);
    for (size_t i = 0; i < nrays; ++i) {
      if (chunk_hits[i].prim_id != LRT_TRI_NO_HIT &&
          chunk_hits[i].t < hits[i].t) {
        hits[i] = chunk_hits[i];
        hits[i].prim_id += static_cast<uint32_t>(global_first);
      }
    }
    if (scene.scene) lrt_tri_scene_free(scene.scene);
    global_first += chunk_tris;
    ++chunk_count;
  }
  const bool ok = ShadeAndWriteImage(opt, shade, rays, hits, w, h, spp);
  if (opt.stats) {
    std::cerr << "[gpu-stats] HIP mesh chunks " << chunk_count
              << ", flatten+bvh " << flatten_s << " s, trace " << trace_s
              << " s\n";
  }
  if (ok) {
    std::cerr << "triangles: " << shade.ntris << " (" << geos.size()
              << " meshes, " << chunk_count << " GPU chunks)\n";
    std::cerr << "backend: LightRT HIP (compute trace)\n";
  }
  lrt_hip_engine_destroy(hip);
  return ok;
}

}  // namespace

bool RunHipLightRT(const Options &opt, const std::vector<Vec3> &base_colors,
                   std::vector<RTPreviewStats::MeshGeometry> &geos,
                   const CameraFrame &camera, int height) {
  if (GpuTriangleCount(geos) > HipGpuChunkLimit()) {
    return RunHipLightRTChunked(opt, base_colors, geos, camera, height,
                                HipGpuChunkLimit());
  }
  GpuTriScene s;
  const auto flatten_t0 = std::chrono::steady_clock::now();
  if (!BuildGpuTriScene(base_colors, geos, opt.threads, true, &s,
                        opt.quality)) {
    return false;
  }
  const double flatten_s = HipSecsSince(flatten_t0);
  for (RTPreviewStats::MeshGeometry &geo : geos) {
    std::vector<float>().swap(geo.positions);
    std::vector<float>().swap(geo.normals);
    std::vector<float>().swap(geo.uvs);
    std::vector<uint32_t>().swap(geo.indices);
  }

  // Create the HIP engine (compiles the trace kernel via hiprtc).
  lrt_hip_engine_options hopts;
  std::memset(&hopts, 0, sizeof(hopts));
  hopts.device_index = -1;
  lrt_result hiperr = LRT_RESULT_OK;
  lrt_hip_engine *hip = lrt_hip_engine_create(&hopts, &hiperr);
  if (!hip) {
    std::cerr << "HIP ray tracing unavailable (rc=" << hiperr
              << "); no ROCm device or kernel compile failed.\n";
    lrt_tri_scene_free(s.scene);
    return false;
  }
  std::cerr << "HIP device: " << lrt_hip_engine_device_name(hip) << "\n";

  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "HIP", &nrays)) {
    lrt_tri_scene_free(s.scene);
    lrt_hip_engine_destroy(hip);
    return false;
  }
  const uint32_t ray_count = uint32_t(nrays);
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);

  std::vector<lrt_hit> hits(nrays);
  lrt_result trerr = LRT_RESULT_OK;
  const auto trace_t0 = std::chrono::steady_clock::now();
  int traced = lrt_hip_trace_scene(hip, s.scene, rays.data(),
                                   ray_count, hits.data(), &trerr);
  if (traced < 0) {
    std::cerr << "HIP trace failed (rc=" << trerr
              << "): " << lrt_hip_engine_last_error(hip) << "\n";
    lrt_tri_scene_free(s.scene);
    lrt_hip_engine_destroy(hip);
    return false;
  }
  const double trace_s = HipSecsSince(trace_t0);

  bool ok = ShadeAndWriteImage(opt, s, rays, hits, w, h, spp);
  if (opt.stats) {
    std::cerr << "[gpu-stats] HIP mesh chunks 1, flatten+bvh " << flatten_s
              << " s, trace " << trace_s << " s\n";
  }
  if (ok) {
    std::cerr << "triangles: " << s.ntris << " (" << geos.size() << " meshes)\n";
    std::cerr << "backend: LightRT HIP (compute trace)\n";
  }
  lrt_tri_scene_free(s.scene);
  lrt_hip_engine_destroy(hip);
  return ok;
}

bool RunHipGaussianLightRT(const Options &opt, DirectScene *direct,
                           const CameraFrame &camera, int height) {
  if (!direct || direct->ellipse_chunks.empty()) return false;
  lrt_hip_engine_options hopts;
  std::memset(&hopts, 0, sizeof(hopts));
  hopts.device_index = -1;
  lrt_result err = LRT_RESULT_OK;
  lrt_hip_engine *hip = lrt_hip_engine_create(&hopts, &err);
  if (!hip) {
    std::cerr << "Failed to create LightRT HIP Gaussian engine (err="
              << int(err) << "): no ROCm device or kernel compile failed.\n";
    return false;
  }
  const int w = opt.width > 0 ? opt.width : 960;
  const int h = height > 0 ? height : 540;
  const int spp = std::max(1, opt.samples);
  size_t nrays = 0;
  if (!ValidateGpuFrameSize(w, h, spp, "HIP Gaussian", &nrays)) {
    lrt_hip_engine_destroy(hip);
    return false;
  }
  std::vector<lrt_ray> rays;
  GenerateCameraRays(camera, w, h, spp, &rays);
  std::vector<lrt_hit> hits(nrays), chunk_hits(nrays);
  InitMissHits(&hits);
  size_t released_chunks = 0;
  for (EllipseSceneChunk &chunk : direct->ellipse_chunks) {
    if (!BuildDeferredGaussianChunk(opt, &chunk)) {
      lrt_hip_engine_destroy(hip);
      return false;
    }
    const int traced = lrt_hip_trace_scene(
        hip, chunk.scene.get(), rays.data(), static_cast<uint32_t>(nrays),
        chunk_hits.data(), &err);
    if (traced < 0) {
      std::cerr << "HIP Gaussian chunk trace failed [" << chunk.first << ", "
                << (chunk.first + chunk.count) << "] (err=" << int(err)
                << "): " << lrt_hip_engine_last_error(hip) << "\n";
      lrt_hip_engine_destroy(hip);
      return false;
    }
    for (size_t i = 0; i < nrays; ++i) {
      if (chunk_hits[i].prim_id != LRT_TRI_NO_HIT &&
          chunk_hits[i].t < hits[i].t) {
        hits[i] = chunk_hits[i];
        hits[i].prim_id += static_cast<uint32_t>(chunk.first);
      }
    }
    chunk.scene.reset();
    ++released_chunks;
  }
  size_t total_ellipses = 0;
  for (const EllipseSceneChunk &chunk : direct->ellipse_chunks)
    total_ellipses += chunk.info.size();
  const bool ok = ShadeAndWriteGaussianImage(
      opt, direct->ellipse_chunks, rays, hits, w, h, spp);
  if (ok) {
    std::cerr << "native Gaussian ellipses: " << total_ellipses
              << " in " << direct->ellipse_chunks.size() << " HIP chunk(s)\n"
              << "released Gaussian BVH chunks: " << released_chunks << "\n"
              << "\nbackend: LightRT HIP (native point/ellipse BVH)\n";
  }
  lrt_hip_engine_destroy(hip);
  return ok;
}

}  // namespace lusdr
#endif  // HAVE_HIP
