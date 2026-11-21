// SPDX-License-Identifier: Apache-2.0
// lusdview - HIP/ROCm ray tracing backend.
//
// The AMD counterpart of CudaRayTracer: a self-contained BVH path tracer
// (primary + one shadow ray) that runs on the HIP runtime loaded *at runtime*
// via hipew (no build-time ROCm link) and compiles its trace kernel at runtime
// via hiprtc. If HIP / hiprtc / a device is unavailable the tracer reports
// `initialized() == false` and the caller falls back to another backend.
//
// It shares the exact trace kernel source (raytracer_kernel_src.txt) and the 2-level
// BVH build with the CUDA backend; only the GPU runtime/driver calls differ.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gpu_scene.hh"
#include "rt_camera.hh"
#include "path_trace.hh"
#include "rt_scene_build.hh"  // BuildProgress, HostScene, RefitMap

namespace lusdview {

class HipRayTracer {
 public:
  HipRayTracer() = default;
  ~HipRayTracer();

  HipRayTracer(const HipRayTracer&) = delete;
  HipRayTracer& operator=(const HipRayTracer&) = delete;

  // Runtime detection: load HIP + hiprtc via hipew, select a device, compile the
  // kernel. Returns false (with *err set) if anything is missing. Safe to call
  // once; subsequent calls are no-ops returning the cached result.
  bool init(std::string* err);
  static bool enumerateDevices(std::vector<std::string>* names,
                               std::string* err);
  bool selectDevice(int index, std::string* err);
  int deviceIndex() const { return device_; }
  bool initialized() const { return ready_; }
  // hiprtc counterpart of CudaRayTracer::reloadKernel. The scene stays live and
  // the old module is retained unless compilation and entry-point validation
  // both succeed.
  bool reloadKernel(const std::string& sourcePath, std::string* err);
  uint64_t kernelGeneration() const { return kernelGeneration_; }
  double lastKernelCompileMs() const { return lastKernelCompileMs_; }
  void setTextureBudgetBytes(size_t bytes) { textureBudgetBytes_ = bytes; }

  // Flatten `scene` into world-space triangles, build a BVH, and upload to the
  // device. See CudaRayTracer::build for the full contract. `retainForRefit`
  // keeps the host-side arrays + tri permutation so a later refit() can re-pose
  // without a rebuild (costs host memory; only worth it for deformable scenes).
  bool build(const DrawScene& scene, size_t maxTris, size_t maxInstances,
             std::string* err, float displacementScale = 0.0f,
             BuildProgress* progress = nullptr, bool retainForRefit = false,
             uint32_t purposeVisibleMask = 0x3u);
  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err);
  size_t triangleCount() const { return triCount_; }
  bool truncated() const { return truncated_; }

  // Re-pose the built scene from `scene`'s CURRENT vertices: rewrite tris/nrms
  // in leaf order, refit BLAS/TLAS bounds over the unchanged trees, and upload
  // only those four buffers (materials/textures/uvs stay resident). Requires
  // build(..., retainForRefit=true); returns false (fall back to build) when
  // refit is unavailable or the topology changed.
  bool refit(const DrawScene& scene, std::string* err);
  bool canRefit() const { return retained_ != nullptr && refitMap_.valid; }

  // Trace one frame. See CudaRayTracer::trace for the argument contract.
  bool trace(const float invViewProj[16], const float viewProj[16],
             const float camPos[3],
             const float lightDir[3], const float clearColor[3], float exposure,
             int renderMode,
             float depthScale, const float sceneMin[3], const float sceneExtent[3],
             int w, int h, std::vector<uint8_t>* rgba, std::string* err,
             int spp = 1, const RtCameraLens* lens = nullptr,
             const PathTraceSettings* pathTrace = nullptr,
             std::vector<float>* linearRgba = nullptr,
             uint32_t* renderedSamples = nullptr,
             float sceneTime = 0.0f, float sceneFrame = 0.0f,
             uint32_t accumulationStart = 0);

  const char* deviceName() const { return deviceName_.c_str(); }

 private:
  bool previewAlbedo_{false};
  void freeScene();
  void freeRuntime();

  // Opaque HIP handles (void* to avoid leaking hipew types into the header).
  bool ready_{false};        // device selected + kernel compiled
  void* module_{nullptr};    // hipModule_t
  void* kernel_{nullptr};    // hipFunction_t (trace)
  void* stream_{nullptr};    // non-default hipStream_t for trace + readback
  void* hostOut_{nullptr};   // page-locked RGBA readback buffer
  size_t hostOutCap_{0};
  int device_{0};
  uint64_t kernelGeneration_{0};
  double lastKernelCompileMs_{0.0};

  // Device buffers (hipDeviceptr_t stored as uintptr_t).
  uintptr_t dTris_{0};       // float[9] positions per tri
  uintptr_t dNrms_{0};       // float[9] vertex normals per tri
  uintptr_t dCols_{0};       // float[9] per-vertex color per tri (base*displayColor)
  uintptr_t dGeo_{0};        // uint8 geometricNormal flag per tri
  uintptr_t dEmask_{0};      // uint8 wireframe edge mask per tri (orig-polygon edges)
  uintptr_t dMat_{0};        // material id per tri (GeomSubset shading + AOV)
  uintptr_t dBackMat_{0};    // optional back-face material id per triangle
  uintptr_t dMatPbr_{0};     // float[6] per material: metal,rough,emitRGB,alpha
  uintptr_t dMatBase_{0};    // float[3] per material: base color
  uintptr_t dMatLightRt_{0};  // float[80] per material: LightRT/OpenPBR params
  uintptr_t dMatGraph_{0};    // fixed-size per-material MaterialX graph IR
  uintptr_t dGeomPropDesc_{0};
  uintptr_t dGeomPropValues_{0};
  int geomPropCount_{0};
  uintptr_t dMatTex_{0};     // int[6]: base,metal,rough,normal,emissive,opacity
  uintptr_t dMatTexParam_{0}; // float[56] per material: texture UV/channel params
  int numMats_{0};           // material count (matPbr index bound)
  uintptr_t dLightParams_{0}; // float[32] per light: packed DrawLightCPU params
  int numLights_{0};
  uintptr_t dTexels_{0};     // RGBA8 texture texels
  uintptr_t dTextures_{0};   // HostTextureDesc[]
  int numTextures_{0};
  uintptr_t dUV_{0};         // float[6] per-vertex uv per tri (uv viz)
  uintptr_t dUV1_{0};        // float[6] per-vertex uv set 1 per tri (multi-UV AOV)
  uintptr_t dInfl_{0};       // float[3] per-vertex blendshape influence per tri
  uintptr_t dFace_{0};       // int source USD face id per tri (source-face-id AOV)
  uintptr_t dDomW_{0};       // float[3] per-vertex dominant skin weight per tri
  uintptr_t dDomJoint_{0};   // int provoking-vertex dominant joint per tri (skin-weights)
  uintptr_t dBlasNodes_{0};  // concatenated BLAS nodes (local-space geometry)
  uintptr_t dTlasNodes_{0};  // TLAS nodes over instance world AABBs
  uintptr_t dInstances_{0};  // Inst[] table (w2o,o2w,tint,blasRoot,instId)
  uintptr_t dVolDens_{0};    // UsdVol: concatenated dense float density grids
  uintptr_t dVolParams_{0};  // UsdVol: VolParam[] (one per volume)
  uintptr_t dPointCenters_{0};
  uintptr_t dPointMajorAxes_{0};
  uintptr_t dPointNormals_{0};
  uintptr_t dPointRadii_{0};
  uintptr_t dPointColors_{0};
  uintptr_t dPointOrder_{0};
  uintptr_t dPointBvh_{0};
  uintptr_t dPointChunks_{0};
  uintptr_t dPointDepth_{0};
  int pointCount_{0};
  int pointChunkCount_{0};
  int numVols_{0};           // UsdVol: volume count
  uintptr_t dOut_{0};        // RGBA8 output image
  size_t outCap_{0};         // bytes currently allocated for dOut_
  uintptr_t dAccum_{0};      // float RGBA supersample accumulator (spp > 1)
  size_t accumCap_{0};       // bytes currently allocated for dAccum_
  size_t pointDepthCap_{0};

  size_t triCount_{0};       // unique prototype triangles (geometry stored once)
  size_t instCount_{0};      // total instances (TLAS leaves)
  size_t blasNodeCount_{0};
  size_t tlasNodeCount_{0};
  size_t nodeCount_{0};
  bool truncated_{false};
  std::string deviceName_;

  // Refit state (build(..., retainForRefit=true)): the host scene stays alive
  // so refit() can rewrite tris/nrms + node bounds in place and re-upload them.
  std::unique_ptr<HostScene> retained_;
  RefitMap refitMap_;
  size_t textureBudgetBytes_{0};
  bool pointPaging_{false};
  std::unique_ptr<HostScene> pointHost_;
};

}  // namespace lusdview
