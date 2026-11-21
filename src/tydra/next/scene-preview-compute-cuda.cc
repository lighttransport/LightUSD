// SPDX-License-Identifier: Apache-2.0
#include <cuda.h>
#include <nvrtc.h>

#include "scene-preview-compute.hh"
#include "scene-preview-kernel-source.hh"
// CUDA 13 headers omit the old context constructor; the driver retains its ABI.
extern "C" CUresult CUDAAPI cuCtxCreate_v2(CUcontext*, unsigned int, CUdevice);

#include <algorithm>
#include <cstdio>
namespace lightusd {
namespace tydra {
namespace next {
namespace {
class CudaCompute final : public PreviewCompute {
  std::shared_ptr<HostLibrary> driver_, compiler_;
  PreviewComputeOptions options_;
#define DRIVER_SYMBOLS(X)                                                \
  X(cuInit)                                                              \
  X(cuDeviceGet)                                                         \
  X(cuDeviceGetAttribute)                                                \
  X(cuCtxCreate_v2)                                                      \
  X(cuCtxDestroy_v2) X(cuMemAlloc_v2) X(cuMemFree_v2) X(cuMemcpyHtoD_v2) \
      X(cuMemcpyDtoH_v2) X(cuModuleLoadDataEx) X(cuModuleGetFunction)    \
          X(cuModuleUnload) X(cuLaunchKernel) X(cuCtxSynchronize)        \
              X(cuCtxSetCurrent)
#define RTC_SYMBOLS(X)   \
  X(nvrtcCreateProgram)  \
  X(nvrtcCompileProgram) \
  X(nvrtcGetPTXSize)     \
  X(nvrtcGetPTX)         \
  X(nvrtcDestroyProgram) X(nvrtcGetProgramLogSize) X(nvrtcGetProgramLog)
#define DECLARE(name) decltype(&name) p_##name = nullptr;
  DRIVER_SYMBOLS(DECLARE)
  RTC_SYMBOLS(DECLARE)
#undef DECLARE
  CUcontext context_{};
  CUmodule module_{};
  CUfunction function_{};
  CUdeviceptr input_ = 0;
  size_t inputSize_ = 0;
  uint64_t budget_;
  bool debug_validate_;

 public:
  CudaCompute(uint64_t budget, const PreviewComputeOptions& options)
      : options_(options), budget_(budget), debug_validate_(options.debug_validate) {}
  ~CudaCompute() override {
    if (context_) {
      p_cuCtxSetCurrent(context_);
      if (input_) p_cuMemFree_v2(input_);
      if (module_) p_cuModuleUnload(module_);
      p_cuCtxDestroy_v2(context_);
    }
  }
  const char* name() const override { return "cuda"; }
  bool init(std::string* error) {
    if (!options_.host_services || !options_.host_services->open_library ||
        options_.cuda_driver_library.empty() || options_.cuda_compiler_library.empty()) {
      *error = "CUDA preview requires explicit host services and driver/compiler filenames";
      return false;
    }
    driver_ = options_.host_services->open_library(options_.cuda_driver_library, error);
    compiler_ = options_.host_services->open_library(options_.cuda_compiler_library, error);
    if (!driver_ || !compiler_) {
      *error = "CUDA driver/NVRTC unavailable";
      return false;
    }
#define LOAD_DRIVER(name)                                                 \
  p_##name = reinterpret_cast<decltype(p_##name)>(driver_->Symbol(#name)); \
  if (!p_##name) {                                                        \
    *error = "CUDA entry point unavailable: " #name;                      \
    return false;                                                         \
  }
#define LOAD_RTC(name)                                                      \
  p_##name = reinterpret_cast<decltype(p_##name)>(compiler_->Symbol(#name)); \
  if (!p_##name) {                                                          \
    *error = "NVRTC entry point unavailable: " #name;                       \
    return false;                                                           \
  }
    DRIVER_SYMBOLS(LOAD_DRIVER)
    RTC_SYMBOLS(LOAD_RTC)
#undef LOAD_DRIVER
#undef LOAD_RTC
    CUdevice device{};
    if (p_cuInit(0) != CUDA_SUCCESS ||
        p_cuDeviceGet(&device, 0) != CUDA_SUCCESS ||
        p_cuCtxCreate_v2(&context_, 0, device) != CUDA_SUCCESS) {
      *error = "CUDA hardware context unavailable";
      return false;
    }
    int major = 0, minor = 0;
    p_cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                           device);
    p_cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                           device);
    std::string source = R"cuda(
typedef unsigned int uint;
#define FN __device__
#define DATA_ARG const uint* inData,
#define DATA_PASS inData,
#define WORK_ARGS ,const uint* inData,float* outData
#define BITS __uint_as_float
#define abs fabsf
)cuda";
    source += kPreviewKernelSource;
    source +=
        "\nextern \"C\" __global__ void preview(const uint* data,float* "
        "result){work(blockIdx.x*blockDim.x+threadIdx.x,data,result);}\n";
    nvrtcProgram program{};
    if (p_nvrtcCreateProgram(&program, source.c_str(), "lightusd-preview.cu", 0,
                             nullptr, nullptr) != NVRTC_SUCCESS)
      return false;
    const std::string architecture = "--gpu-architecture=compute_" +
                                     std::to_string(major) +
                                     std::to_string(minor);
    const char* opts[] = {"--std=c++14", architecture.c_str(), "--fmad=false"};
    if (p_nvrtcCompileProgram(program, 3, opts) != NVRTC_SUCCESS) {
      size_t size = 0;
      p_nvrtcGetProgramLogSize(program, &size);
      std::string log(size, '\0');
      p_nvrtcGetProgramLog(program, log.data());
      *error = "preview CUDA compile failed: " + log;
      p_nvrtcDestroyProgram(&program);
      return false;
    }
    size_t size = 0;
    p_nvrtcGetPTXSize(program, &size);
    std::vector<char> ptx(size);
    p_nvrtcGetPTX(program, ptx.data());
    p_nvrtcDestroyProgram(&program);
    if (p_cuModuleLoadDataEx(&module_, ptx.data(), 0, nullptr, nullptr) !=
            CUDA_SUCCESS ||
        p_cuModuleGetFunction(&function_, module_, "preview") != CUDA_SUCCESS) {
      *error = "preview CUDA module load failed";
      return false;
    }
    return true;
  }
  bool dispatch(const std::vector<uint32_t>& data, size_t count,
                std::vector<float>* output, std::string* error,
                bool camera_only) override {
    if (!ValidatePreviewDispatch(data, count, error, debug_validate_)) return false;
    p_cuCtxSetCurrent(context_);
    const uint64_t inputBytes = data.size() * 4, outputBytes = count * 4,
                   reserve = std::min(budget_ / 2, uint64_t(512) << 20);
    if (inputBytes > budget_ - reserve ||
        outputBytes > budget_ - reserve - inputBytes) {
      *error = "CUDA preview compute exceeds budget";
      return false;
    }
    const bool reuse = camera_only && input_ && inputSize_ == inputBytes;
    if (!reuse) {
      if (input_) p_cuMemFree_v2(input_);
      input_ = 0;
      inputSize_ = 0;
      if (p_cuMemAlloc_v2(&input_, inputBytes) != CUDA_SUCCESS) {
        *error = "CUDA preview input allocation failed";
        return false;
      }
      inputSize_ = inputBytes;
    }
    CUdeviceptr result = 0;
    if (p_cuMemAlloc_v2(&result, outputBytes) != CUDA_SUCCESS) {
      *error = "CUDA preview output allocation failed";
      return false;
    }
    auto status =
        p_cuMemcpyHtoD_v2(input_, data.data(), reuse ? 64 * 4 : inputBytes);
    void* args[] = {&input_, &result};
    if (status == CUDA_SUCCESS)
      status = p_cuLaunchKernel(
          function_,
          static_cast<unsigned>((count / (data[0] == 2 ? 16 : 4) + 63) / 64), 1,
          1, 64, 1, 1, 0, nullptr, args, nullptr);
    if (status == CUDA_SUCCESS) status = p_cuCtxSynchronize();
    if (status == CUDA_SUCCESS) {
      output->resize(count);
      status = p_cuMemcpyDtoH_v2(output->data(), result, outputBytes);
    }
    p_cuMemFree_v2(result);
    if (status != CUDA_SUCCESS) {
      *error = "CUDA preview dispatch failed (" + std::to_string(status) + ")";
      return false;
    }
    return true;
  }
};
#undef DRIVER_SYMBOLS
#undef RTC_SYMBOLS
}  // namespace
std::unique_ptr<PreviewCompute> CreateCudaPreviewCompute(
    uint64_t budget, std::string* error, const PreviewComputeOptions& options) {
  auto engine = std::make_unique<CudaCompute>(budget, options);
  if (!engine->init(error)) return {};
  return engine;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
