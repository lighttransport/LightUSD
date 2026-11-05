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
typedef struct lrt_tlas lrt_tlas;
struct lrt_ray;

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
  // `viewProj16` is the column-major projection*view used for this frame. When
  // supplied, instances whose world bounds fall entirely outside that frustum
  // are skipped before the single world-space triangle stream is flattened,
  // which is what keeps highly instanced scenes inside the expansion budget.
  // Pass nullptr to flatten everything (interactive/rebuild paths).
  bool build(const DrawScene& scene, size_t maxTris, size_t maxInstances,
             std::string* err, float displacementScale = 0.0f,
             BuildProgress* progress = nullptr,
             uint32_t purposeVisibleMask = 0x3u,
             const float* viewProj16 = nullptr);
  // Resolve a TLAS hit to the source triangle it came from, and trace one
  // world-space ray through the instance TLAS.
  int SrcForInstancePrim(int inst, uint32_t prim) const;
  bool Intersect(const lrt_ray& ray, float* t, float* u, float* v, int* inst,
                 int* src) const;

  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err) {
    return UpdateHostMaterialConstants(&hs_, materialId, material, err);
  }
  bool canRefit() const { return false; }  // no vertex-level BVH refit in lightrt_c_tri
  size_t triangleCount() const { return triCount_; }
  // Instances skipped because their world bounds fall outside the build-time
  // view frustum, and instances actually expanded into the triangle stream.
  size_t culledInstanceCount() const { return culledInstances_; }
  bool tlasActive() const { return tlas_ != nullptr; }
  size_t blasGroupCount() const { return blasScenes_.size(); }
  size_t tlasHitCount() const { return tlasHits_; }
  size_t tlasMismatchCount() const { return tlasMismatches_; }
  size_t tlasMissedFlatCount() const { return tlasMissedFlat_; }
  size_t tlasExtraHitCount() const { return tlasExtraHits_; }
  float tlasMaxTDelta() const { return tlasMaxDelta_; }
  size_t expandedInstanceCount() const { return expandedInstances_; }
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
  std::vector<float> cpuTris_, cpuNrms_, cpuUv_, cpuUv1_, cpuTan_, cpuBin_;
  std::vector<int> cpuMat_, cpuInstance_, cpuSourceTri_;
  // Per-table-texture streamed PTex sources (nullptr for non-Ptex/atlas-only).
  std::vector<std::unique_ptr<CpuPtexSampler>> ptex_;
  // Resolved source path per table texture, for the diagnostic probe dump.
  std::vector<std::string> ptexAsset_;
  lrt_tri_scene* scene_{nullptr};
  // Prototype-space BLASes plus the TLAS over hs_.instances. Built only when
  // LUSDVIEW_CPU_RT_INSTANCED is set, so the flattened reference path stays
  // byte-identical while the instanced traversal is brought up.
  std::vector<lrt_tri_scene*> blasScenes_;
  // Compacted object-space triangles per prototype (must outlive the BLAS) and
  // the BLAS-prim -> global triangle index map used to resolve TLAS hits.
  std::vector<std::vector<float>> tlasBlasTris_;
  std::vector<std::vector<int>> tlasPrimToGlobal_;
  std::vector<int> tlasRootToGroup_;  // hs_.blas node -> BLAS group
  lrt_tlas* tlas_{nullptr};
  bool tlasValid_{false};
  size_t tlasHits_{0};        // both paths hit within tolerance
  size_t tlasMismatches_{0};  // both hit, t disagrees beyond tolerance
  size_t tlasMissedFlat_{0};  // flat hit but TLAS missed: a real defect
  size_t tlasExtraHits_{0};   // TLAS hit, flat missed: expected while the
                              // flattened reference is truncated
  float tlasMaxDelta_{0.0f};
  size_t triCount_{0};
  bool truncated_{false};
  size_t culledInstances_{0};
  size_t expandedInstances_{0};
  size_t textureBudgetBytes_{0};
};

}  // namespace lusdview
