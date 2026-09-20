// SPDX-License-Identifier: Apache-2.0
#include "hip_raytracer.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#include "hipew.h"
#include "displacement_bake.hh"
#include "log.hh"
#include "lightrt_mtlx_bridge.hh"
#include "rt_scene_build.hh"  // Node, Inst, HostScene, BuildHostScene (shared)

#if defined(_MSC_VER)
#include "kernel_resource.hh"
#endif

namespace lusdview {

namespace {

// Camera/light block passed by value to the kernel (must match `Cam` there).
struct Cam {
  float invVP[16];
  float camPos[4];
  float lightDir[4];   // xyz light, w depthScale
  float clear[4];      // rgb clear, w RenderMode
  float sceneMin[4];   // position AOV bbox
  float sceneExtent[4];
  float vp[16];        // world->clip (wireframe edge projection)
  float lens[4];       // focus distance, aperture radius, enabled, reserved
  float pathLimits[4]; // SSS events, volume events, motion segments, variance
  float context[4];    // MaterialX time, frame, reserved, reserved
};

// Trace kernel source, shared with the CUDA backend (compiled at runtime by
// hiprtc here, NVRTC there).
//
// MSVC caps a single string literal at ~16,380 bytes (C2026), well under this
// kernel's size, so it loads the source from an embedded Win32 resource
// instead (see raytracer_kernel_resource.rc / kernel_resource.cc). GCC/Clang
// have no such limit; they compile the CMake-generated raw-string literal
// (see raytracer_kernel_src.txt in CMakeLists.txt).
#if defined(_MSC_VER)
static const char* kKernelSrc = GetEmbeddedKernelSource().c_str();
#else
#include "raytracer_kernel.inc"
#endif

void NormalizeKernelSource(std::string* source) {
  constexpr const char* close = ")CUDA\"";
  constexpr const char* open = "R\"CUDA(";
  for (size_t at = source->find(close); at != std::string::npos;
       at = source->find(close, at)) {
    const size_t next = source->find(open, at + std::strlen(close));
    if (next == std::string::npos) break;
    source->erase(at, next + std::strlen(open) - at);
  }
}

#define CU_OK(call, what)                                          \
  do {                                                             \
    hipError_t _r = (call);                                        \
    if (_r != hipSuccess) {                                        \
      const char* _s = hipGetErrorString ? hipGetErrorString(_r) : nullptr; \
      if (err) *err = std::string("HIP ") + (what) + ": " + (_s ? _s : "error"); \
      return false;                                                \
    }                                                              \
  } while (0)


}  // namespace

HipRayTracer::~HipRayTracer() {
  freeRuntime();
}

void HipRayTracer::freeRuntime() {
  if (ready_) freeScene();
  if (stream_) hipStreamDestroy(reinterpret_cast<hipStream_t>(stream_));
  if (hostOut_) hipHostFree(hostOut_);
  if (module_) hipModuleUnload(reinterpret_cast<hipModule_t>(module_));
  module_ = nullptr;
  kernel_ = nullptr;
  stream_ = nullptr;
  hostOut_ = nullptr;
  hostOutCap_ = 0;
  ready_ = false;
}

bool HipRayTracer::enumerateDevices(std::vector<std::string>* names,
                                    std::string* err) {
  if (names) names->clear();
  if (hipewInit(HIPEW_INIT_HIP) != HIPEW_SUCCESS) {
    if (err) *err = "hipew: HIP runtime not available";
    return false;
  }
  CU_OK(hipInit(0), "hipInit");
  int count = 0;
  CU_OK(hipGetDeviceCount(&count), "hipGetDeviceCount");
  for (int i = 0; i < count; ++i) {
    hipDevice_t dev = 0;
    if (hipDeviceGet(&dev, i) != hipSuccess) continue;
    char name[256] = {};
    if (hipDeviceGetName(name, sizeof(name), dev) != hipSuccess)
      std::snprintf(name, sizeof(name), "HIP device %d", i);
    if (names) names->push_back(std::to_string(i) + ": " + name);
  }
  if (!names || names->empty()) {
    if (err) *err = "no HIP device";
    return false;
  }
  return true;
}

bool HipRayTracer::selectDevice(int index, std::string* err) {
  std::vector<std::string> names;
  if (!enumerateDevices(&names, err)) return false;
  if (index < 0 || static_cast<size_t>(index) >= names.size()) {
    if (err) *err = "HIP device index is out of range";
    return false;
  }
  if (ready_ && index == device_) return true;
  const int previous = device_;
  const bool hadRuntime = ready_;
  freeRuntime();
  device_ = index;
  if (init(err)) return true;
  freeRuntime();
  device_ = previous;
  if (hadRuntime) {
    std::string restoreError;
    if (!init(&restoreError) && err)
      *err += "; restoring previous HIP device also failed: " + restoreError;
  }
  return false;
}

void HipRayTracer::freeScene() {
  auto F = [](uintptr_t& p) { if (p) { hipFree(reinterpret_cast<void*>(p)); p = 0; } };
  F(dTris_); F(dNrms_); F(dCols_); F(dGeo_); F(dEmask_); F(dMat_); F(dBackMat_);
  F(dMatPbr_); F(dMatBase_);
  F(dMatLightRt_); F(dMatGraph_); F(dGeomPropDesc_); F(dGeomPropValues_); F(dMatTex_);
  F(dMatTexParam_); F(dLightParams_);
  F(dTexels_); F(dTextures_); F(dUV_); F(dUV1_); F(dInfl_); F(dFace_); F(dDomW_); F(dDomJoint_);
  F(dBlasNodes_); F(dTlasNodes_); F(dInstances_); F(dOut_); F(dAccum_);
  F(dVolDens_); F(dVolParams_);
  F(dPointCenters_); F(dPointMajorAxes_); F(dPointNormals_);
  F(dPointRadii_); F(dPointColors_); F(dPointOrder_); F(dPointBvh_);
  F(dPointChunks_);
  F(dPointDepth_);
  numVols_ = 0;
  numMats_ = 0;
  numLights_ = 0;
  numTextures_ = 0;
  pointCount_ = 0;
  pointChunkCount_ = 0;
  outCap_ = 0; accumCap_ = 0; pointDepthCap_ = 0;
  triCount_ = 0; nodeCount_ = 0;
  instCount_ = 0; blasNodeCount_ = 0; tlasNodeCount_ = 0;
  pointPaging_ = false;
  pointHost_.reset();
}

bool HipRayTracer::init(std::string* err) {
  if (ready_) return true;
  if (hipewInit(HIPEW_INIT_HIP) != HIPEW_SUCCESS) {
    if (err) *err = "hipew: HIP runtime not available";
    return false;
  }
  if (hipewInit(HIPEW_INIT_HIPRTC) != HIPEW_SUCCESS) {
    if (err) *err = "hipew: hiprtc not available (needed for runtime kernel compile)";
    return false;
  }
  CU_OK(hipInit(0), "hipInit");
  int count = 0;
  CU_OK(hipGetDeviceCount(&count), "hipGetDeviceCount");
  if (count < 1) { if (err) *err = "no HIP device"; return false; }
  if (device_ < 0 || device_ >= count) device_ = 0;
  CU_OK(hipSetDevice(device_), "hipSetDevice");
  hipDevice_t dev = 0;
  CU_OK(hipDeviceGet(&dev, device_), "hipDeviceGet");
  char name[256] = {0};
  hipDeviceGetName(name, sizeof(name), dev);
  deviceName_ = name;

  // hiprtc compile the kernel. No --offload-arch is passed: hiprtc targets the
  // current device automatically. (Unlike NVRTC there is no PTX step; hiprtc
  // emits a loadable code object directly.)
  hiprtcProgram prog;
  if (hiprtcCreateProgram(&prog, kKernelSrc, "trace.hip", 0, nullptr, nullptr) !=
      HIPRTC_SUCCESS) {
    if (err) *err = "hiprtcCreateProgram failed";
    return false;
  }
  const char* opts[] = {"-ffast-math"};
  hiprtcResult nr = hiprtcCompileProgram(prog, 1, opts);
  if (nr != HIPRTC_SUCCESS) {
    size_t logSize = 0;
    hiprtcGetProgramLogSize(prog, &logSize);
    std::string log(logSize, '\0');
    if (logSize) hiprtcGetProgramLog(prog, &log[0]);
    if (err) *err = "hiprtc compile failed:\n" + log;
    hiprtcDestroyProgram(&prog);
    return false;
  }
  size_t codeSize = 0;
  hiprtcGetCodeSize(prog, &codeSize);
  std::string code(codeSize, '\0');
  hiprtcGetCode(prog, &code[0]);
  hiprtcDestroyProgram(&prog);

  hipModule_t mod;
  CU_OK(hipModuleLoadData(&mod, code.c_str()), "hipModuleLoadData");
  module_ = mod;
  hipFunction_t fn;
  CU_OK(hipModuleGetFunction(&fn, mod, "trace"), "hipModuleGetFunction(trace)");
  kernel_ = fn;
  // Create the stream only after compilation and module loading have
  // succeeded.  This keeps every hiprtc failure path resource-neutral.
  hipStream_t stream = nullptr;
  CU_OK(hipStreamCreate(&stream), "hipStreamCreate");
  stream_ = stream;
  ready_ = true;
  kernelGeneration_ = 1;
  return true;
}

bool HipRayTracer::reloadKernel(const std::string& sourcePath,
                                std::string* err) {
  if (!ready_ || !module_ || !kernel_) {
    if (err) *err = "HIP runtime must be initialized before kernel reload";
    return false;
  }
  std::ifstream input(sourcePath, std::ios::binary);
  if (!input) {
    if (err) *err = "cannot read HIP kernel source: " + sourcePath;
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff sourceSize = input.tellg();
  if (sourceSize <= 0 || sourceSize > 64 * 1024 * 1024) {
    if (err) *err = "invalid HIP kernel source size";
    return false;
  }
  input.seekg(0, std::ios::beg);
  std::string source(static_cast<size_t>(sourceSize), '\0');
  if (!input.read(source.data(), sourceSize)) {
    if (err) *err = "failed reading HIP kernel source";
    return false;
  }
  NormalizeKernelSource(&source);
  CU_OK(hipSetDevice(device_), "hipSetDevice live reload");
  const auto started = std::chrono::steady_clock::now();
  hiprtcProgram program = nullptr;
  if (hiprtcCreateProgram(&program, source.c_str(), sourcePath.c_str(), 0,
                          nullptr, nullptr) != HIPRTC_SUCCESS) {
    if (err) *err = "hiprtcCreateProgram failed for " + sourcePath;
    return false;
  }
  const char* options[] = {"-ffast-math"};
  const hiprtcResult compileResult = hiprtcCompileProgram(program, 1, options);
  if (compileResult != HIPRTC_SUCCESS) {
    size_t logSize = 0;
    hiprtcGetProgramLogSize(program, &logSize);
    std::string log(logSize, '\0');
    if (logSize) hiprtcGetProgramLog(program, log.data());
    if (!log.empty() && log.back() == '\0') log.pop_back();
    hiprtcDestroyProgram(&program);
    if (err) *err = "hiprtc live compile failed:\n" + log;
    return false;
  }
  size_t codeSize = 0;
  if (hiprtcGetCodeSize(program, &codeSize) != HIPRTC_SUCCESS || codeSize == 0) {
    hiprtcDestroyProgram(&program);
    if (err) *err = "hiprtc live compile produced no code object";
    return false;
  }
  std::string code(codeSize, '\0');
  if (hiprtcGetCode(program, code.data()) != HIPRTC_SUCCESS) {
    hiprtcDestroyProgram(&program);
    if (err) *err = "hiprtc live code extraction failed";
    return false;
  }
  hiprtcDestroyProgram(&program);

  hipModule_t replacement = nullptr;
  if (hipModuleLoadData(&replacement, code.data()) != hipSuccess) {
    if (err) *err = "HIP runtime rejected live-compiled code object";
    return false;
  }
  hipFunction_t replacementKernel = nullptr;
  if (hipModuleGetFunction(&replacementKernel, replacement, "trace") !=
      hipSuccess) {
    hipModuleUnload(replacement);
    if (err) *err = "live HIP module has no trace entry point";
    return false;
  }
  if (hipDeviceSynchronize() != hipSuccess) {
    hipModuleUnload(replacement);
    if (err) *err = "HIP synchronization failed before kernel swap";
    return false;
  }
  hipModule_t previous = reinterpret_cast<hipModule_t>(module_);
  module_ = replacement;
  kernel_ = replacementKernel;
  hipModuleUnload(previous);
  ++kernelGeneration_;
  lastKernelCompileMs_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  LOGI("HIP kernel live reload committed: generation %llu, %.1f ms (%s)",
       static_cast<unsigned long long>(kernelGeneration_),
       lastKernelCompileMs_, sourcePath.c_str());
  return true;
}


bool HipRayTracer::build(const DrawScene& scene, size_t maxTris,
                         size_t maxInstances, std::string* err,
                         float displacementScale, BuildProgress* progress,
                         bool retainForRefit, uint32_t purposeVisibleMask) {
  if (!ready_) { if (err) *err = "HIP not initialized"; return false; }
  CU_OK(hipSetDevice(device_), "hipSetDevice");  // also sets the device on a worker thread
  freeScene();
  retained_.reset();
  refitMap_ = RefitMap{};

  // Build the host scene (parallel per-mesh geometry + TLAS) -- shared with CUDA.
  HostScene local;
  if (retainForRefit) retained_.reset(new HostScene());
  HostScene& hs = retained_ ? *retained_ : local;
  if (!BuildHostScene(scene, maxTris, maxInstances, displacementScale, &hs, err,
                      progress, retained_ ? &refitMap_ : nullptr,
                      textureBudgetBytes_, nullptr, purposeVisibleMask)) {
    if (err) *err = "HIP: " + *err;
    retained_.reset();
    return false;
  }
  truncated_ = hs.truncated;
  triCount_ = hs.triCount;
  instCount_ = hs.instCount;
  blasNodeCount_ = hs.blasNodeCount;
  tlasNodeCount_ = hs.tlasNodeCount;
  nodeCount_ = blasNodeCount_ + tlasNodeCount_;
  numMats_ = hs.numMats;
  numLights_ = hs.numLights;
  numTextures_ = hs.numTextures;
  numVols_ = hs.numVols;
  if (hs.pointCount > static_cast<size_t>(std::numeric_limits<int>::max())) {
    if (err) *err = "HIP: Gaussian point count exceeds kernel limit (" +
                    std::to_string(hs.pointCount) + ")";
    // No device arrays have been uploaded yet, but a refit build may already
    // own a large retained HostScene. Do not leave that stale scene behind
    // after rejecting the 32-bit kernel-limit violation.
    freeScene();
    retained_.reset();
    return false;
  }
  const uint64_t gaussianBytes =
      uint64_t(hs.pointCenters.size()) * sizeof(float) +
      uint64_t(hs.pointMajorAxes.size()) * sizeof(float) +
      uint64_t(hs.pointNormals.size()) * sizeof(float) +
      uint64_t(hs.pointRadii.size()) * sizeof(float) +
      uint64_t(hs.pointColors.size()) * sizeof(float) +
      uint64_t(hs.pointOrder.size()) * sizeof(int) +
      uint64_t(hs.pointBvh.size()) * sizeof(Node) +
      uint64_t(hs.pointChunks.size()) * sizeof(PointBvhChunk);
  uint64_t gaussianBudget = 0;
  if (const char* value = std::getenv("LUSDVIEW_GAUSSIAN_GPU_BUDGET_MB")) {
    char* end = nullptr;
    const unsigned long long mb = std::strtoull(value, &end, 10);
    if (value[0] != '\0' && end != value && *end == '\0' && mb > 0 &&
        mb <= (std::numeric_limits<uint64_t>::max)() / (1024ull * 1024ull))
      gaussianBudget = mb * 1024ull * 1024ull;
  }
  if (gaussianBudget == 0 && hipMemGetInfo) {
    size_t freeBytes = 0, totalBytes = 0;
    if (hipMemGetInfo(&freeBytes, &totalBytes) == hipSuccess) {
      constexpr uint64_t kHeadroom = 256ull * 1024ull * 1024ull;
      if (freeBytes > kHeadroom) gaussianBudget = freeBytes - kHeadroom;
    }
  }
  if (gaussianBudget != 0 && gaussianBytes > gaussianBudget) {
    pointPaging_ = true;
    pointHost_.reset(new HostScene());
    pointHost_->pointCenters = std::move(hs.pointCenters);
    pointHost_->pointMajorAxes = std::move(hs.pointMajorAxes);
    pointHost_->pointNormals = std::move(hs.pointNormals);
    pointHost_->pointRadii = std::move(hs.pointRadii);
    pointHost_->pointColors = std::move(hs.pointColors);
    pointHost_->pointOrder = std::move(hs.pointOrder);
    pointHost_->pointBvh = std::move(hs.pointBvh);
    pointHost_->pointChunks = std::move(hs.pointChunks);
    pointHost_->pointCount = hs.pointCount;
    hs.pointCount = 0;
    std::fprintf(stderr,
                 "HIP Gaussian buffers: %.1f MiB exceed %.1f MiB; paging %zu chunk(s)\n",
                 double(gaussianBytes) / (1024.0 * 1024.0),
                 double(gaussianBudget) / (1024.0 * 1024.0),
                 pointHost_->pointChunks.size());
  }
  if (gaussianBytes > 0) {
    std::fprintf(stderr, "HIP Gaussian buffers: %.1f MiB%s\n",
                 double(gaussianBytes) / (1024.0 * 1024.0),
                 pointPaging_ ? " (paged)"
                              : (gaussianBudget ? " (within GPU budget)" : ""));
  }
  pointCount_ = static_cast<int>(pointPaging_ ? pointHost_->pointCount
                                              : hs.pointCount);

  if (progress) { progress->phase = 3; progress->done = 0; progress->total = 0; }  // upload
  // Upload every array to the device.
  auto up = [&](const void* host, size_t bytes, uintptr_t* dptr) -> bool {
    void* p = nullptr;
    const hipError_t alloc = hipMalloc(&p, bytes ? bytes : 1);
    if (alloc != hipSuccess) {
      const char* s = hipGetErrorString ? hipGetErrorString(alloc) : nullptr;
      if (err) {
        *err = "HIP scene upload allocation failed (" +
               std::to_string(bytes) + " bytes): " + (s ? s : "error");
      }
      freeScene();
      retained_.reset();
      return false;
    }
    if (bytes) {
      const hipError_t copy = hipMemcpyHtoD(p, host, bytes);
      if (copy != hipSuccess) {
        const char* s = hipGetErrorString ? hipGetErrorString(copy) : nullptr;
        hipFree(p);
        if (err) {
          *err = "HIP scene upload copy failed (" + std::to_string(bytes) +
                 " bytes): " + (s ? s : "error");
        }
        freeScene();
        retained_.reset();
        return false;
      }
    }
    *dptr = reinterpret_cast<uintptr_t>(p);
    return true;
  };
  if (!up(hs.tris.data(), hs.tris.size() * sizeof(float), &dTris_)) return false;
  if (!up(hs.nrms.data(), hs.nrms.size() * sizeof(float), &dNrms_)) return false;
  if (!up(hs.cols.data(), hs.cols.size() * sizeof(float), &dCols_)) return false;
  if (!up(hs.geo.data(), hs.geo.size(), &dGeo_)) return false;
  if (!up(hs.emask.data(), hs.emask.size(), &dEmask_)) return false;
  if (!up(hs.mat.data(), hs.mat.size() * sizeof(int), &dMat_)) return false;
  if (!hs.backMat.empty() &&
      !up(hs.backMat.data(), hs.backMat.size() * sizeof(int), &dBackMat_))
    return false;
  if (!up(hs.face.data(), hs.face.size() * sizeof(int), &dFace_)) return false;
  if (!up(hs.uv.data(), hs.uv.size() * sizeof(float), &dUV_)) return false;
  if (!up(hs.uv1.data(), hs.uv1.size() * sizeof(float), &dUV1_)) return false;
  if (!up(hs.infl.data(), hs.infl.size() * sizeof(float), &dInfl_)) return false;
  if (!up(hs.domw.data(), hs.domw.size() * sizeof(float), &dDomW_)) return false;
  if (!up(hs.domj.data(), hs.domj.size() * sizeof(int), &dDomJoint_)) return false;
  if (!up(hs.blas.data(), hs.blas.size() * sizeof(Node), &dBlasNodes_)) return false;
  if (!up(hs.tlas.data(), hs.tlas.size() * sizeof(Node), &dTlasNodes_)) return false;
  if (!up(hs.instances.data(), hs.instances.size() * sizeof(Inst), &dInstances_)) return false;
  if (!up(hs.matTex.data(), hs.matTex.size() * sizeof(int), &dMatTex_)) return false;
  if (!up(hs.matTexParam.data(), hs.matTexParam.size() * sizeof(float),
          &dMatTexParam_)) return false;
  if (!up(hs.texels.data(), hs.texels.size(), &dTexels_)) return false;
  if (!up(hs.textures.data(), hs.textures.size() * sizeof(HostTextureDesc), &dTextures_)) return false;
  if (hs.numVols > 0) {
    if (!up(hs.volDens.data(), hs.volDens.size() * sizeof(float), &dVolDens_)) return false;
    if (!up(hs.volParams.data(), hs.volParams.size() * sizeof(HostVolParam), &dVolParams_))
      return false;
  }
  if (!pointPaging_) {
    if (!up(hs.pointCenters.data(), hs.pointCenters.size() * sizeof(float),
            &dPointCenters_)) return false;
    if (!up(hs.pointMajorAxes.data(), hs.pointMajorAxes.size() * sizeof(float),
            &dPointMajorAxes_)) return false;
    if (!up(hs.pointNormals.data(), hs.pointNormals.size() * sizeof(float),
            &dPointNormals_)) return false;
    if (!up(hs.pointRadii.data(), hs.pointRadii.size() * sizeof(float),
            &dPointRadii_)) return false;
    if (!up(hs.pointColors.data(), hs.pointColors.size() * sizeof(float),
            &dPointColors_)) return false;
    if (!up(hs.pointOrder.data(), hs.pointOrder.size() * sizeof(int),
            &dPointOrder_)) return false;
    if (!up(hs.pointBvh.data(), hs.pointBvh.size() * sizeof(Node),
            &dPointBvh_)) return false;
    pointChunkCount_ = static_cast<int>(hs.pointChunks.size());
    if (!up(hs.pointChunks.data(), hs.pointChunks.size() * sizeof(PointBvhChunk),
            &dPointChunks_)) return false;
  }
  if (!up(hs.matPbr.data(), hs.matPbr.size() * sizeof(float), &dMatPbr_)) return false;
  if (!up(hs.matBase.data(), hs.matBase.size() * sizeof(float), &dMatBase_)) return false;
  if (!up(hs.matLightRt.data(), hs.matLightRt.size() * sizeof(float),
          &dMatLightRt_)) return false;
  if (!up(hs.matGraph.data(), hs.matGraph.size() * sizeof(float),
          &dMatGraph_)) return false;
  {
    std::vector<HostGeomPropDesc> desc;
    std::vector<float> values;
    for (const HostGeomProp& prop : hs.geomProps) {
      if (((prop.components < 1 || prop.components > 4) &&
           prop.components != 9 && prop.components != 16) ||
          prop.values.size() != hs.triCount * 3u * prop.components) continue;
      HostGeomPropDesc d;
      d.hash = MaterialXGeomPropHash(prop.name);
      d.components = prop.components;
      d.valueOffset = static_cast<uint32_t>(values.size());
      desc.push_back(d);
      values.insert(values.end(), prop.values.begin(), prop.values.end());
    }
    geomPropCount_ = static_cast<int>(desc.size());
    if (geomPropCount_ > 0) {
      if (!up(desc.data(), desc.size() * sizeof(HostGeomPropDesc), &dGeomPropDesc_) ||
          !up(values.data(), values.size() * sizeof(float), &dGeomPropValues_)) return false;
    }
  }
  if (!up(hs.lightParams.data(), hs.lightParams.size() * sizeof(float),
          &dLightParams_)) return false;
  if (progress) progress->phase = 4;  // done

  // Refit only needs tris/nrms/blas/tlas/instances; drop the pose-invariant
  // arrays (texels alone can be hundreds of MB) now that they are on the device.
  if (retained_) {
    HostScene& r = *retained_;
    auto drop = [](auto& v) { v.clear(); v.shrink_to_fit(); };
    drop(r.cols); drop(r.uv); drop(r.uv1); drop(r.infl); drop(r.domw);
    drop(r.geo); drop(r.emask); drop(r.mat); drop(r.backMat); drop(r.face);
    drop(r.domj);
    drop(r.matPbr); drop(r.matBase); drop(r.matLightRt); drop(r.matGraph); drop(r.matTex);
    drop(r.matTexParam);
    drop(r.texels); drop(r.textures); drop(r.lightParams);
    drop(r.volDens); drop(r.volParams);
    // Gaussian/analytic-point arrays are device-resident after the upload and
    // are not part of triangle refit. Keeping their centers, axes, ordering,
    // and BVH in the retained host scene needlessly duplicates large splat
    // fields on AMD systems.
    drop(r.pointCenters); drop(r.pointMajorAxes); drop(r.pointNormals);
    drop(r.pointRadii); drop(r.pointColors); drop(r.pointOrder);
    drop(r.pointBvh); drop(r.pointChunks);
    r.pointCount = 0;
  }
  return true;
}

bool HipRayTracer::updateMaterialConstants(
    int materialId, const DrawMaterialCPU& material, std::string* err) {
  if (!ready_ || materialId < 0 || materialId >= numMats_ || !dMatPbr_ ||
      !dMatBase_ || !dMatLightRt_) {
    if (err) *err = "HIP material index is out of range or scene is not built";
    return false;
  }
  CU_OK(hipSetDevice(device_), "hipSetDevice(material update)");
  float pbr[6], base[3], lightRt[kLightRtOpenPBRFloats];
  PackRtMaterialConstants(material, pbr, base, lightRt);
  void* pbrDst = reinterpret_cast<void*>(
      dMatPbr_ + static_cast<size_t>(materialId) * sizeof(pbr));
  void* baseDst = reinterpret_cast<void*>(
      dMatBase_ + static_cast<size_t>(materialId) * sizeof(base));
  void* lightRtDst = reinterpret_cast<void*>(
      dMatLightRt_ + static_cast<size_t>(materialId) * sizeof(lightRt));
  CU_OK(hipMemcpyHtoD(pbrDst, pbr, sizeof(pbr)), "material PBR upload");
  CU_OK(hipMemcpyHtoD(baseDst, base, sizeof(base)), "material base upload");
  CU_OK(hipMemcpyHtoD(lightRtDst, lightRt, sizeof(lightRt)),
        "material OpenPBR upload");
  return true;
}

bool HipRayTracer::refit(const DrawScene& scene, std::string* err) {
  if (!ready_ || !dTris_) { if (err) *err = "HIP scene not built"; return false; }
  if (!canRefit()) {
    if (err) *err = "scene was not built with refit retained";
    return false;
  }
  CU_OK(hipSetDevice(device_), "hipSetDevice");
  HostScene& hs = *retained_;
  if (!RefitHostScene(scene, refitMap_, &hs, err)) return false;
  // Only the pose-dependent buffers move; sizes are unchanged, so copy into the
  // existing allocations (no realloc, no touch of materials/textures/uvs).
  CU_OK(hipMemcpyHtoD(reinterpret_cast<void*>(dTris_), hs.tris.data(),
                      hs.tris.size() * sizeof(float)), "hipMemcpyHtoD(tris)");
  CU_OK(hipMemcpyHtoD(reinterpret_cast<void*>(dNrms_), hs.nrms.data(),
                      hs.nrms.size() * sizeof(float)), "hipMemcpyHtoD(nrms)");
  CU_OK(hipMemcpyHtoD(reinterpret_cast<void*>(dBlasNodes_), hs.blas.data(),
                      hs.blas.size() * sizeof(Node)), "hipMemcpyHtoD(blas)");
  CU_OK(hipMemcpyHtoD(reinterpret_cast<void*>(dTlasNodes_), hs.tlas.data(),
                      hs.tlas.size() * sizeof(Node)), "hipMemcpyHtoD(tlas)");
  CU_OK(hipMemcpyHtoD(reinterpret_cast<void*>(dInstances_), hs.instances.data(),
                      hs.instances.size() * sizeof(Inst)),
        "hipMemcpyHtoD(instances)");
  return true;
}

bool HipRayTracer::trace(const float invViewProj[16], const float viewProj[16],
                          const float camPos[3],
                          const float lightDir[3], const float clearColor[3],
                          float exposure,
                          int renderMode, float depthScale, const float sceneMin[3],
                          const float sceneExtent[3], int w, int h,
                          std::vector<uint8_t>* rgba, std::string* err, int spp,
                          const RtCameraLens* lens,
                         const PathTraceSettings* pathTrace,
                         std::vector<float>* linearRgba,
                         uint32_t* renderedSamples,
                         float sceneTime, float sceneFrame,
                         uint32_t accumulationStart) {
  if (!ready_ || !dTris_) { if (err) *err = "HIP scene not built"; return false; }
  CU_OK(hipSetDevice(device_), "hipSetDevice");
  const size_t bytes = size_t(w) * h * 4;
  if (outCap_ < bytes) {
    if (dOut_) hipFree(reinterpret_cast<void*>(dOut_));
    void* p = nullptr;
    CU_OK(hipMalloc(&p, bytes), "hipMalloc(out)");
    dOut_ = reinterpret_cast<uintptr_t>(p);
    outCap_ = bytes;
  }
  // Supersample accumulator (float per channel), only for spp > 1.
  const bool productionPath = pathTrace && pathTrace->enabled;
  const size_t accumBytes = (spp > 1 || productionPath) ? bytes * sizeof(float) : 0;
  if (accumBytes && accumCap_ < accumBytes) {
    if (dAccum_) hipFree(reinterpret_cast<void*>(dAccum_));
    void* p = nullptr;
    CU_OK(hipMalloc(&p, accumBytes), "hipMalloc(accum)");
    dAccum_ = reinterpret_cast<uintptr_t>(p);
    accumCap_ = accumBytes;
  }
  Cam cam{};
  std::memcpy(cam.invVP, invViewProj, 16 * sizeof(float));
  std::memcpy(cam.vp, viewProj, 16 * sizeof(float));  // world->clip (wireframe projection)
  for (int i = 0; i < 3; ++i) {
    cam.camPos[i] = camPos[i];
    cam.lightDir[i] = lightDir[i];
    cam.clear[i] = clearColor[i];
  }
  cam.clear[3] = static_cast<float>(renderMode);
  cam.context[0] = sceneTime;
  cam.context[1] = sceneFrame;
  cam.camPos[3] = exposure;
  cam.lightDir[3] = depthScale;  // depth AOV normalizer
  for (int i = 0; i < 3; ++i) { cam.sceneMin[i] = sceneMin[i]; cam.sceneExtent[i] = sceneExtent[i]; }
  if (lens && lens->enabled()) {
    cam.lens[0] = lens->focusDistance;
    cam.lens[1] = lens->apertureRadius;
    cam.lens[2] = 1.0f;
  }
  if (pathTrace && pathTrace->enabled) {
    cam.lens[3] = static_cast<float>(
        0x800000u | (pathTrace->maxDepth & 255u) |
        ((pathTrace->russianRouletteDepth & 255u) << 8u) |
        ((pathTrace->seed & 127u) << 16u));
    cam.pathLimits[0] = static_cast<float>(pathTrace->maxSubsurfaceEvents);
    cam.pathLimits[1] = static_cast<float>(pathTrace->maxVolumeEvents);
    cam.pathLimits[2] = static_cast<float>(pathTrace->motionSegments);
    cam.pathLimits[3] = pathTrace->varianceThreshold;
  }
  void* dT = reinterpret_cast<void*>(dTris_), *dN = reinterpret_cast<void*>(dNrms_),
        *dC = reinterpret_cast<void*>(dCols_), *dG = reinterpret_cast<void*>(dGeo_),
        *dM = reinterpret_cast<void*>(dMat_), *dBM = reinterpret_cast<void*>(dBackMat_),
        *dMP = reinterpret_cast<void*>(dMatPbr_), *dMB = reinterpret_cast<void*>(dMatBase_),
        *dML = reinterpret_cast<void*>(dMatLightRt_),
        *dMG = reinterpret_cast<void*>(dMatGraph_),
        *dLP = reinterpret_cast<void*>(dLightParams_),
        *dMT = reinterpret_cast<void*>(dMatTex_), *dTx = reinterpret_cast<void*>(dTexels_),
        *dMTP = reinterpret_cast<void*>(dMatTexParam_),
        *dTD = reinterpret_cast<void*>(dTextures_),
        *dU = reinterpret_cast<void*>(dUV_), *dU1 = reinterpret_cast<void*>(dUV1_),
        *dIn = reinterpret_cast<void*>(dInfl_), *dF = reinterpret_cast<void*>(dFace_),
        *dDw = reinterpret_cast<void*>(dDomW_), *dDj = reinterpret_cast<void*>(dDomJoint_),
        *dBl = reinterpret_cast<void*>(dBlasNodes_), *dTl = reinterpret_cast<void*>(dTlasNodes_),
        *dI = reinterpret_cast<void*>(dInstances_), *dO = reinterpret_cast<void*>(dOut_);
  void* dVD = reinterpret_cast<void*>(dVolDens_), *dVP = reinterpret_cast<void*>(dVolParams_);
  void* dEm = reinterpret_cast<void*>(dEmask_);
  void* dPC = reinterpret_cast<void*>(dPointCenters_);
  void* dPA = reinterpret_cast<void*>(dPointMajorAxes_);
  void* dPN = reinterpret_cast<void*>(dPointNormals_);
  void* dPR = reinterpret_cast<void*>(dPointRadii_);
  void* dPCol = reinterpret_cast<void*>(dPointColors_);
  void* dPO = reinterpret_cast<void*>(dPointOrder_);
  void* dPB = reinterpret_cast<void*>(dPointBvh_);
  void* dPChunks = reinterpret_cast<void*>(dPointChunks_);
  void* dDepth = nullptr;
  void* dGeomPropDesc = reinterpret_cast<void*>(dGeomPropDesc_);
  void* dGeomPropValues = reinterpret_cast<void*>(dGeomPropValues_);
  int numMats = numMats_;
  int numLights = numLights_;
  int numTextures = numTextures_;
  int numVols = numVols_;
  const int samples = spp < 1 ? 1 : spp;
  if (renderedSamples) *renderedSamples = static_cast<uint32_t>(samples);
  void* dAcc = (samples > 1 || productionPath)
                   ? reinterpret_cast<void*>(dAccum_) : nullptr;
  int sampleIdx = 0;
  const uint32_t start = productionPath ? accumulationStart : 0u;
  const uint32_t total = start + static_cast<uint32_t>(samples);
  int numSamples = static_cast<int>(std::min<uint32_t>(
      total, static_cast<uint32_t>(std::numeric_limits<int>::max())));
  // ORDER MUST MATCH the kernel signature: tris,nrms,cols,geo,mats,backMats,matPbr,matBase,
  // matLightRt,numMats,lightParams,numLights,matTex,matTexParam,texels,textures,
  // numTextures,uvs,uvs1,infls,faces,domw,domj,blas,tlas,insts,out,W,H,cam,
  // volDens,volParams,numVols,emask,point arrays,pointCount,accum,sampleIdx,numSamples,hitDepth,geomprops.
  void* args[] = {&dT,  &dN,  &dC, &dG, &dM, &dBM, &dMP, &dMB, &dML, &dMG, &numMats, &dLP,
                  &numLights, &dMT, &dMTP, &dTx, &dTD, &numTextures, &dU, &dU1,
                  &dIn, &dF,  &dDw,
                  &dDj, &dBl, &dTl, &dI, &dO, &w, &h, &cam,
                  &dVD, &dVP, &numVols, &dEm, &dPC, &dPA, &dPN, &dPR, &dPCol,
                  &dPO, &dPB, &pointCount_, &dPChunks, &pointChunkCount_,
                  &dAcc, &sampleIdx, &numSamples, &dDepth,
                  &dGeomPropDesc, &dGeomPropValues, &geomPropCount_};
  unsigned gx = (w + 7) / 8, gy = (h + 7) / 8;
  rgba->resize(bytes);
  if (hostOutCap_ < bytes) {
    if (hostOut_) hipHostFree(hostOut_);
    hostOut_ = nullptr;
    CU_OK(hipHostMalloc(&hostOut_, bytes, 0u), "hipHostMalloc(output)");
    hostOutCap_ = bytes;
  }
  if (pointPaging_ && pointHost_ && !pointHost_->pointChunks.empty()) {
    const size_t pixels = size_t(w) * size_t(h);
    if (pointDepthCap_ < pixels) {
      if (dPointDepth_) hipFree(reinterpret_cast<void*>(dPointDepth_));
      void* p = nullptr;
      if (hipMalloc(&p, pixels * sizeof(float)) != hipSuccess) {
        if (err) *err = "HIP Gaussian paging depth allocation failed";
        freeScene();
        return false;
      }
      dPointDepth_ = reinterpret_cast<uintptr_t>(p);
      pointDepthCap_ = pixels;
    }
    dDepth = reinterpret_cast<void*>(dPointDepth_);
    std::vector<uint8_t> best(bytes), chunkRgba(bytes);
    std::vector<uint8_t> bestSet(pixels);
    std::vector<float> bestDepth(pixels), chunkDepth(pixels);
    std::vector<uint32_t> sums(pixels * 3, 0);
    auto allocCopy = [&](const void* src, size_t n, void** dst) {
      *dst = nullptr;
      if (hipMalloc(dst, n ? n : 1) != hipSuccess) return false;
      if (n && hipMemcpyHtoD(*dst, src, n) != hipSuccess) {
        hipFree(*dst); *dst = nullptr; return false;
      }
      return true;
    };
    auto freePointChunk = [&]() {
      if (dPC) hipFree(dPC);
      if (dPA) hipFree(dPA);
      if (dPN) hipFree(dPN);
      if (dPR) hipFree(dPR);
      if (dPCol) hipFree(dPCol);
      if (dPO) hipFree(dPO);
      if (dPB) hipFree(dPB);
      if (dPChunks) hipFree(dPChunks);
      dPC = dPA = dPN = dPR = dPCol = dPO = dPB = dPChunks = nullptr;
    };
    const int totalPointCount = pointCount_;
    const int totalPointChunks = pointChunkCount_;
    dAcc = nullptr;
    numSamples = 1;
    pointChunkCount_ = 1;
    for (int s = 0; s < samples; ++s) {
      std::fill(bestDepth.begin(), bestDepth.end(),
                std::numeric_limits<float>::infinity());
      std::fill(bestSet.begin(), bestSet.end(), uint8_t{0});
      RtPixelJitter(s, samples, &cam.sceneMin[3], &cam.sceneExtent[3]);
      for (const PointBvhChunk& srcChunk : pointHost_->pointChunks) {
        const size_t first = static_cast<size_t>(srcChunk.first);
        const size_t count = static_cast<size_t>(srcChunk.count);
        const size_t orderFirst = static_cast<size_t>(srcChunk.orderFirst);
        const size_t bvhFirst = static_cast<size_t>(srcChunk.bvhFirst);
        const size_t bvhCount = static_cast<size_t>(srcChunk.bvhCount);
        std::vector<int> localOrder(count);
        for (size_t i = 0; i < count; ++i)
          localOrder[i] = pointHost_->pointOrder[orderFirst + i] -
                          static_cast<int>(first);
        std::vector<Node> localBvh(
            pointHost_->pointBvh.begin() + bvhFirst,
            pointHost_->pointBvh.begin() + bvhFirst + bvhCount);
        const PointBvhChunk localChunk{0, static_cast<int>(count), 0, 0,
                                       static_cast<int>(bvhCount)};
        freePointChunk();
        if (!allocCopy(pointHost_->pointCenters.data() + first * 3,
                       count * 3 * sizeof(float), &dPC) ||
            !allocCopy(pointHost_->pointMajorAxes.data() + first * 3,
                       count * 3 * sizeof(float), &dPA) ||
            !allocCopy(pointHost_->pointNormals.data() + first * 3,
                       count * 3 * sizeof(float), &dPN) ||
            !allocCopy(pointHost_->pointRadii.data() + first * 2,
                       count * 2 * sizeof(float), &dPR) ||
            !allocCopy(pointHost_->pointColors.data() + first * 4,
                       count * 4 * sizeof(float), &dPCol) ||
            !allocCopy(localOrder.data(), localOrder.size() * sizeof(int), &dPO) ||
            !allocCopy(localBvh.data(), localBvh.size() * sizeof(Node), &dPB) ||
            !allocCopy(&localChunk, sizeof(localChunk), &dPChunks)) {
          if (err) *err = "HIP Gaussian paging chunk upload failed";
          freePointChunk(); freeScene(); return false;
        }
        pointCount_ = static_cast<int>(count);
        sampleIdx = s;
        CU_OK(hipModuleLaunchKernel(reinterpret_cast<hipFunction_t>(kernel_), gx, gy,
                                    1, 8, 8, 1, 0, nullptr, args, nullptr),
              "hipModuleLaunchKernel Gaussian paging");
        CU_OK(hipDeviceSynchronize(), "hipDeviceSynchronize Gaussian paging");
        CU_OK(hipMemcpyDtoH(chunkRgba.data(), reinterpret_cast<void*>(dOut_), bytes),
              "hipMemcpyDtoH Gaussian paging color");
        CU_OK(hipMemcpyDtoH(chunkDepth.data(), dDepth,
                            pixels * sizeof(float)),
              "hipMemcpyDtoH Gaussian paging depth");
        for (size_t pidx = 0; pidx < pixels; ++pidx) {
          if (!bestSet[pidx] || chunkDepth[pidx] < bestDepth[pidx]) {
            bestDepth[pidx] = chunkDepth[pidx];
            bestSet[pidx] = 1;
            std::memcpy(&best[pidx * 4], &chunkRgba[pidx * 4], 4);
          }
        }
      }
      for (size_t pidx = 0; pidx < pixels; ++pidx) {
        sums[pidx * 3 + 0] += best[pidx * 4 + 0];
        sums[pidx * 3 + 1] += best[pidx * 4 + 1];
        sums[pidx * 3 + 2] += best[pidx * 4 + 2];
      }
    }
    freePointChunk();
    pointCount_ = totalPointCount;
    pointChunkCount_ = totalPointChunks;
    for (size_t pidx = 0; pidx < pixels; ++pidx) {
      (*rgba)[pidx * 4 + 0] = static_cast<uint8_t>(sums[pidx * 3 + 0] / samples);
      (*rgba)[pidx * 4 + 1] = static_cast<uint8_t>(sums[pidx * 3 + 1] / samples);
      (*rgba)[pidx * 4 + 2] = static_cast<uint8_t>(sums[pidx * 3 + 2] / samples);
      (*rgba)[pidx * 4 + 3] = 255;
    }
    return true;
  }
  // Launch one kernel per Halton-jittered sample; the kernel accumulates on the
  // device (float per channel, same math as the old host loop -- byte-identical)
  // and resolves into the RGBA8 output on the last sample. One readback for the
  // whole frame instead of one per sample.
  //
  // LUSDVIEW_RT_TIMING=1: report the isolated per-pass GPU trace time (launch +
  // blocking sync, excluding the DtoH readback) -- the cost an interactive
  // per-frame retrace would pay once the scene is already built/uploaded. The
  // per-pass sync exists only for that timing; the untimed path syncs once.
  const bool rtTiming = std::getenv("LUSDVIEW_RT_TIMING") != nullptr;
  double traceMsTotal = 0.0;
  int completedSamples = samples;
  std::vector<float> convergencePrevious;
  std::vector<float> convergenceCurrent;
  const bool adaptive = start == 0 && productionPath && linearRgba && pathTrace &&
                        pathTrace->varianceThreshold > 0.0f &&
                        samples > static_cast<int>(pathTrace->minSamples);
  const int convergenceInterval =
      std::max(8, static_cast<int>(pathTrace ? pathTrace->minSamples / 4u : 8u));
  for (int s = 0; s < samples; ++s) {
    float jx, jy;
    const int absoluteSample = static_cast<int>(start) + s;
    RtPixelJitter(absoluteSample, numSamples, &jx, &jy);
    cam.sceneMin[3] = jx;
    cam.sceneExtent[3] = jy;
    sampleIdx = absoluteSample;
    auto t0 = std::chrono::steady_clock::now();
    CU_OK(hipModuleLaunchKernel(reinterpret_cast<hipFunction_t>(kernel_), gx, gy, 1,
                                8, 8, 1, 0,
                                reinterpret_cast<hipStream_t>(stream_), args,
                                nullptr),
          "hipModuleLaunchKernel");
    if (rtTiming) {
      CU_OK(hipDeviceSynchronize(), "hipDeviceSynchronize");
      auto t1 = std::chrono::steady_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      traceMsTotal += ms;
      std::fprintf(stderr, "[hip_raytracer] trace pass %d/%d: %.1f ms (%dx%d)\n",
                   s + 1, samples, ms, w, h);
    }
    const int completed = absoluteSample + 1;
    if (adaptive && completed >= static_cast<int>(pathTrace->minSamples) &&
        completed % convergenceInterval == 0) {
      if (!rtTiming)
        CU_OK(hipDeviceSynchronize(), "hipDeviceSynchronize convergence");
      convergenceCurrent.resize(size_t(w) * size_t(h) * 4u);
      CU_OK(hipMemcpyDtoH(convergenceCurrent.data(),
                          reinterpret_cast<void*>(dAccum_),
                          convergenceCurrent.size() * sizeof(float)),
            "hipMemcpyDtoH convergence");
      const float inv = 1.0f / static_cast<float>(completed);
      for (size_t i = 0; i < convergenceCurrent.size(); i += 4) {
        convergenceCurrent[i] *= inv;
        convergenceCurrent[i + 1] *= inv;
        convergenceCurrent[i + 2] *= inv;
        convergenceCurrent[i + 3] = 1.0f;
      }
      const double change =
          PathTraceRelativeChange(convergenceCurrent, convergencePrevious);
      convergencePrevious.swap(convergenceCurrent);
      if (change <= static_cast<double>(pathTrace->varianceThreshold)) {
        completedSamples = completed;
        LOGI("HIP path trace converged at %d samples "
             "(relative RMS %.6f <= %.6f)",
             completed, change, pathTrace->varianceThreshold);
        break;
      }
    }
  }
  CU_OK(hipMemcpyDtoHAsync(hostOut_, reinterpret_cast<void*>(dOut_), bytes,
                           reinterpret_cast<hipStream_t>(stream_)),
        "hipMemcpyDtoHAsync");
  CU_OK(hipStreamSynchronize(reinterpret_cast<hipStream_t>(stream_)),
        "hipStreamSynchronize");
  std::memcpy(rgba->data(), hostOut_, bytes);
  if (linearRgba && pathTrace && pathTrace->enabled) {
    linearRgba->resize(size_t(w) * size_t(h) * 4u);
    CU_OK(hipMemcpyDtoH(linearRgba->data(), reinterpret_cast<void*>(dAccum_),
                        linearRgba->size() * sizeof(float)),
          "hipMemcpyDtoH(linear accumulation)");
    const float inv = 1.0f / static_cast<float>(start + completedSamples);
    for (size_t i = 0; i < linearRgba->size(); i += 4) {
      (*linearRgba)[i] *= inv;
      (*linearRgba)[i + 1] *= inv;
      (*linearRgba)[i + 2] *= inv;
      (*linearRgba)[i + 3] = 1.0f;
    }
  }
  if (rtTiming) {
    std::fprintf(stderr,
                 "[hip_raytracer] %d pass(es): %.1f ms total, %.1f ms/pass (%dx%d)\n",
                 completedSamples, traceMsTotal,
                 traceMsTotal / double(completedSamples), w, h);
  }
  if (renderedSamples)
    *renderedSamples = start + static_cast<uint32_t>(completedSamples);
  return true;
}

}  // namespace lusdview
