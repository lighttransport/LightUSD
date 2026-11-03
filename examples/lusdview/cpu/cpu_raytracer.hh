// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU ray tracing backend.
//
// Reuses BuildHostScene() (rt_scene_build.hh). CUDA/HIP consume its prototype
// BLAS/TLAS representation directly. The CPU backend expands those instances
// into a world-space triangle stream because lightrt_c exposes a single-BVH
// triangle API; this preserves instance transforms and collection masks while
// retaining the deliberately simple CPU fallback.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gpu_scene.hh"
#include "rt_camera.hh"
#include "rt_scene_build.hh"  // HostScene, BuildHostScene, BuildProgress
#include "ptex-sample.hh"     // streamed per-face PTex pages

typedef struct lrt_tri_scene lrt_tri_scene;  // src/external/lightrt/lightrt_c_tri.h

namespace lusdview {

class CpuRayTracer {
 public:
  CpuRayTracer() = default;
  ~CpuRayTracer();

  CpuRayTracer(const CpuRayTracer&) = delete;
  CpuRayTracer& operator=(const CpuRayTracer&) = delete;

  // CPU tracing has no device to detect; always succeeds. Kept for API parity
  // with CudaRayTracer/HipRayTracer so App's dispatch code is uniform.
  bool init(std::string* /*err*/) { return true; }
  void setTextureBudgetBytes(size_t bytes) { textureBudgetBytes_ = bytes; }

  // Flatten `scene` (BuildHostScene) and build a BVH over the result. Replaces
  // any previously built scene. See CudaRayTracer::build for the parameter
  // meanings (identical contract).
  bool build(const DrawScene& scene, size_t maxTris, size_t maxInstances,
             std::string* err, float displacementScale = 0.0f,
             BuildProgress* progress = nullptr,
             uint32_t purposeVisibleMask = 0x3u);
  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err) {
    return UpdateHostMaterialConstants(&hs_, materialId, material, err);
  }
  bool canRefit() const { return false; }  // no vertex-level BVH refit in lightrt_c_tri
  size_t triangleCount() const { return triCount_; }
  bool truncated() const { return truncated_; }

  // Trace one frame on CPU worker threads. `spp` controls deterministic
  // supersampling and `lens` enables depth of field.
  bool trace(const float invViewProj[16], const float viewProj[16],
             const float camPos[3], const float lightDir[3],
             const float clearColor[3], float exposure, int renderMode,
             float depthScale, const float sceneMin[3], const float sceneExtent[3],
             int w, int h, std::vector<uint8_t>* rgba, std::string* err,
             int spp = 1, const RtCameraLens* lens = nullptr);

  const char* deviceName() const { return "CPU"; }

 private:
  bool traceSingle(const float invViewProj[16], const float camPos[3],
                   const float lightDir[3], const float clearColor[3],
                   float exposure, int renderMode, float depthScale, int w,
                   int h, float sampleX, float sampleY, float pixelAngle,
                   const RtCameraLens* lens, std::vector<uint8_t>* rgba,
                   std::string* err);
  void freeScene();

  HostScene hs_;             // retained (unlike CUDA/HIP, which discard it after device upload)
  // lightrt_c has no instance/TLAS traversal API. These are the expanded
  // world-space hit attributes corresponding one-to-one with scene_ triangles.
  std::vector<float> cpuTris_, cpuNrms_, cpuUv_, cpuUv1_;
  std::vector<int> cpuMat_, cpuInstance_, cpuSourceTri_;
  // Per-table-texture streamed PTex sources (nullptr for non-Ptex/atlas-only).
  std::vector<std::unique_ptr<CpuPtexSampler>> ptex_;
  lrt_tri_scene* scene_{nullptr};
  size_t triCount_{0};
  bool truncated_{false};
  size_t textureBudgetBytes_{0};
};

}  // namespace lusdview
