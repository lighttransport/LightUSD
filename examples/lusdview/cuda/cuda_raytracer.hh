// SPDX-License-Identifier: Apache-2.0
// lusdview - CUDA ray tracing backend.
//
// A self-contained BVH path tracer (primary + one shadow ray) that runs on the
// CUDA driver API loaded *at runtime* via cuew (no build-time CUDA link) and
// compiles its trace kernel at runtime via NVRTC. If CUDA / NVRTC / a device is
// unavailable the tracer reports `available() == false` and the caller falls back
// to another backend.
//
// Geometry is stored once per prototype in local-space BLAS nodes, with a TLAS
// and affine instance records providing two-level traversal without expanding
// repeated instances into duplicate world-space triangles.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gpu_scene.hh"
#include "renderer.hh"
#include "rt_camera.hh"
#include "path_trace.hh"
#include "rt_scene_build.hh"  // BuildProgress, HostScene, RefitMap
#if defined(LUSDVIEW_HAVE_OPTIX)
#include "cuda/optix_runtime.hh"
#endif

namespace lusdview {

class CudaRayTracer {
 public:
  CudaRayTracer() = default;
  ~CudaRayTracer();

  CudaRayTracer(const CudaRayTracer&) = delete;
  CudaRayTracer& operator=(const CudaRayTracer&) = delete;

  // Runtime detection: load CUDA + NVRTC via cuew, create a context, compile the
  // kernel. Returns false (with *err set) if anything is missing. Safe to call
  // once; subsequent calls are no-ops returning the cached result.
  bool init(std::string* err);
  static bool enumerateDevices(std::vector<std::string>* names,
                               std::string* err);
  bool selectDevice(int index, std::string* err);
  int deviceIndex() const { return device_; }
  bool initialized() const { return ctx_ != nullptr; }
  bool optixAvailable() const {
#if defined(LUSDVIEW_HAVE_OPTIX)
    return optixRuntime_.attached();
#else
    return false;
#endif
  }
  const std::string& optixStatus() const { return optixStatus_; }
  int optixAbiVersion() const {
#if defined(LUSDVIEW_HAVE_OPTIX)
    return optixRuntime_.loaded() ? optixRuntime_.abiVersion() : 0;
#else
    return 0;
#endif
  }
  size_t optixGasBytes() const { return optixGasBytes_; }
  size_t optixIasBytes() const { return optixIasBytes_; }
  size_t optixAccelerationBytes() const {
    return optixGasBytes_ + optixIasBytes_;
  }
  const std::string& optixFallbackReason() const {
    return optixFallbackReason_;
  }
  // Compile `sourcePath` with NVRTC and atomically replace only the module and
  // trace entry point. Device scene/BVH/texture buffers remain resident; a
  // compile or module-validation failure keeps the previous kernel active.
  bool reloadKernel(const std::string& sourcePath, std::string* err);
  uint64_t kernelGeneration() const { return kernelGeneration_; }
  double lastKernelCompileMs() const { return lastKernelCompileMs_; }
  // Override the platform lusdview CUDA-kernel cache directory. An empty path
  // keeps the platform default (for example ~/.cache/lusdview/cuda on Linux).
  void setCacheDirectory(const std::string& path) { cacheDirectory_ = path; }
  void setTextureBudgetBytes(size_t bytes) { textureBudgetBytes_ = bytes; }
  void setRtBackend(CudaRtBackend backend) { rtBackend_ = backend; }
  CudaRtBackend rtBackend() const { return rtBackend_; }
  bool usesOptixTransport() const { return lastUsedOptix_; }

  // Build prototype-local BLASes plus a world-space instance TLAS and upload
  // everything to the device. `maxTris` caps unique prototype triangles; 0 = no
  // cap. Returns false
  // on OOM / build failure. Replaces any previously built scene.
  // displacementScale > 0 bakes coarse UsdPreviewSurface displacement into the
  // traced geometry (ray tracers intersect real triangles, so displacement can't
  // be a shader effect here). 0 = no displacement.
  bool build(const DrawScene& scene, size_t maxTris, size_t maxInstances,
             std::string* err, float displacementScale = 0.0f,
             BuildProgress* progress = nullptr,
             bool retainForRefit = false,
             uint32_t purposeVisibleMask = 0x3u);
  bool refit(const DrawScene& scene, std::string* err);
  bool canRefit() const { return retained_ != nullptr && refitMap_.valid; }
  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err);
  size_t triangleCount() const { return triCount_; }
  bool truncated() const { return truncated_; }
  size_t deviceUsedBytes() const { return deviceUsedBytes_; }
  size_t deviceTotalBytes() const { return deviceTotalBytes_; }
  double lastTraceMs() const { return lastTraceMs_; }

  // Trace one frame. `invViewProj` is the column-major inverse(proj*view) (16
  // floats), `camPos`/`lightDir` are 3-float world vectors (lightDir points toward
  // the light), `clearColor` is the 3-float background. Writes a top-down RGBA8
  // image (row 0 = top) of size w*h*4 into *rgba. Returns false on launch failure.
  // renderMode mirrors RenderMode (0=shaded, 1=wireframe, 2=normals, 3=material-id,
  // 4=geom normal, 5=uv, 6=depth). depthScale normalizes the depth AOV.
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
  void freeScene();
  void freeRuntime();

  // Opaque CUDA handles (void* to avoid leaking cuew/driver types into the header).
  void* ctx_{nullptr};       // CUcontext
  void* module_{nullptr};    // CUmodule
  void* kernel_{nullptr};    // CUfunction (trace)
  void* stream_{nullptr};    // non-default CUstream for trace + readback
  void* hostOut_{nullptr};   // page-locked RGBA readback buffer
  size_t hostOutCap_{0};
  int device_{0};
  int compileArch_{0};
  uint64_t kernelGeneration_{0};
  double lastKernelCompileMs_{0.0};
  double lastTraceMs_{0.0};

  // Device buffers (CUdeviceptr stored as uintptr_t).
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
  // 2-level instancing: per-prototype BLAS nodes (concatenated, global-rebased) +
  // a TLAS over per-instance world AABBs + the instance table.
  uintptr_t dBlasNodes_{0};  // concatenated BLAS nodes (local-space geometry)
  uintptr_t dTlasNodes_{0};  // TLAS nodes over instance world AABBs
  uintptr_t dInstances_{0};  // Inst[] table (w2o,o2w,tint,blasRoot,instId)
  std::vector<uintptr_t> dOptixGases_;
  std::vector<uint64_t> optixGasHandles_;
  std::vector<size_t> optixGasOutputBytes_;
  size_t optixGasBytes_{0};
  bool optixGasUpdateEnabled_{false};
  uintptr_t dOptixInstances_{0};
  uintptr_t dOptixIas_{0};
  uint64_t optixIasHandle_{0};
  size_t optixIasBytes_{0};
  uintptr_t dOptixSbt_{0};
  uint32_t optixSbtMissOffset_{0};
  uint32_t optixSbtHitOffset_{0};
  uint32_t optixSbtHitStride_{0};
  uint32_t optixSbtHitCount_{0};
  bool optixCameraProbeDone_{false};
  bool optixRefitLogged_{false};
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
  bool sceneAllLevelSetVolumes_{true};
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
  std::string optixStatus_{"not built"};
  std::string optixFallbackReason_;
  CudaRtBackend rtBackend_{CudaRtBackend::Auto};
  bool lastUsedOptix_{false};
  bool sceneHasMaterialGraphs_{false};
#if defined(LUSDVIEW_HAVE_OPTIX)
  OptixRuntime optixRuntime_;
#endif
  std::string cacheDirectory_;
  size_t textureBudgetBytes_{0};
  size_t deviceUsedBytes_{0};
  size_t deviceTotalBytes_{0};
  bool pointPaging_{false};
  std::unique_ptr<HostScene> pointHost_;
  std::unique_ptr<HostScene> retained_;
  RefitMap refitMap_;
};

}  // namespace lusdview
