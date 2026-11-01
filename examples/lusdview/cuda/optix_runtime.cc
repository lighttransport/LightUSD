#include "optix_launch_abi.hh"
// SPDX-License-Identifier: Apache-2.0
#include "cuda/optix_runtime.hh"

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <optix_function_table.h>

namespace lusdview {

struct OptixRuntime::Impl {
#if defined(_WIN32)
  HMODULE library{nullptr};
#else
  void* library{nullptr};
#endif
  OptixFunctionTable table{};
  OptixDeviceContext context{nullptr};
  OptixModule module{nullptr};
  OptixProgramGroup raygen{nullptr};
  OptixProgramGroup miss{nullptr};
  OptixProgramGroup hit{nullptr};
  OptixPipeline pipeline{nullptr};
  bool ready{false};
};

OptixRuntime::OptixRuntime() : impl_(new Impl) {}

OptixRuntime::~OptixRuntime() { unload(); }

bool OptixRuntime::load(std::string* err) {
  if (impl_->ready) return true;
  unload();

#if defined(_WIN32)
  impl_->library = LoadLibraryA("nvoptix.dll");
  if (!impl_->library) {
    if (err) *err = "OptiX driver library nvoptix.dll is unavailable";
    return false;
  }
  FARPROC symbol = GetProcAddress(impl_->library, "optixQueryFunctionTable");
  OptixQueryFunctionTable_t* query =
      reinterpret_cast<OptixQueryFunctionTable_t*>(symbol);
#else
  impl_->library = dlopen("libnvoptix.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!impl_->library) {
    if (err) {
      const char* detail = dlerror();
      *err = std::string("OptiX driver library libnvoptix.so.1 is unavailable") +
             (detail ? std::string(": ") + detail : std::string());
    }
    return false;
  }
  void* symbol = dlsym(impl_->library, "optixQueryFunctionTable");
  OptixQueryFunctionTable_t* query = nullptr;
  static_assert(sizeof(query) == sizeof(symbol),
                "function and data pointers must have equal size");
  std::memcpy(&query, &symbol, sizeof(query));
#endif

  if (!query) {
    if (err) *err = "OptiX driver does not export optixQueryFunctionTable";
    unload();
    return false;
  }

  const OptixResult result = query(OPTIX_ABI_VERSION, 0, nullptr, nullptr,
                                   &impl_->table, sizeof(impl_->table));
  if (result != OPTIX_SUCCESS) {
    if (err) {
      std::ostringstream ss;
      ss << "optixQueryFunctionTable failed for ABI " << OPTIX_ABI_VERSION
         << " (OptixResult " << static_cast<int>(result) << ")";
      *err = ss.str();
    }
    unload();
    return false;
  }
  impl_->ready = true;
  if (err) err->clear();
  return true;
}

bool OptixRuntime::attachCudaContext(void* cudaContext, std::string* err) {
  if (impl_->context) return true;
  if (!cudaContext) {
    if (err) *err = "cannot attach OptiX to a null CUDA context";
    return false;
  }
  if (!load(err)) return false;
  if (!impl_->table.optixDeviceContextCreate) {
    if (err) *err = "OptiX function table has no device-context entry point";
    return false;
  }
  OptixDeviceContextOptions options{};
  const OptixResult result = impl_->table.optixDeviceContextCreate(
      reinterpret_cast<CUcontext>(cudaContext), &options, &impl_->context);
  if (result != OPTIX_SUCCESS) {
    if (err) {
      const char* detail = impl_->table.optixGetErrorString
                               ? impl_->table.optixGetErrorString(result)
                               : nullptr;
      std::ostringstream ss;
      ss << "optixDeviceContextCreate failed (OptixResult "
         << static_cast<int>(result) << ")";
      if (detail) ss << ": " << detail;
      *err = ss.str();
    }
    impl_->context = nullptr;
    return false;
  }
  if (err) err->clear();
  return true;
}

void OptixRuntime::detachCudaContext() {
  if (!impl_ || !impl_->context) return;
  destroyPipeline();
  if (impl_->ready && impl_->table.optixDeviceContextDestroy) {
    impl_->table.optixDeviceContextDestroy(impl_->context);
  }
  impl_->context = nullptr;
}

void OptixRuntime::unload() {
  if (!impl_) return;
  detachCudaContext();
  impl_->ready = false;
  impl_->table = OptixFunctionTable{};
  if (impl_->library) {
#if defined(_WIN32)
    FreeLibrary(impl_->library);
#else
    dlclose(impl_->library);
#endif
    impl_->library = nullptr;
  }
}

bool OptixRuntime::loaded() const { return impl_ && impl_->ready; }

bool OptixRuntime::attached() const { return impl_ && impl_->context; }

int OptixRuntime::abiVersion() const { return OPTIX_ABI_VERSION; }

const void* OptixRuntime::functionTable() const {
  return loaded() ? static_cast<const void*>(&impl_->table) : nullptr;
}

void* OptixRuntime::deviceContext() const {
  return attached() ? reinterpret_cast<void*>(impl_->context) : nullptr;
}

void OptixRuntime::destroyPipeline() {
  if (!impl_ || !impl_->ready) return;
  if (impl_->pipeline && impl_->table.optixPipelineDestroy)
    impl_->table.optixPipelineDestroy(impl_->pipeline);
  impl_->pipeline = nullptr;
  OptixProgramGroup* groups[] = {&impl_->hit, &impl_->miss, &impl_->raygen};
  for (OptixProgramGroup* group : groups) {
    if (*group && impl_->table.optixProgramGroupDestroy)
      impl_->table.optixProgramGroupDestroy(*group);
    *group = nullptr;
  }
  if (impl_->module && impl_->table.optixModuleDestroy)
    impl_->table.optixModuleDestroy(impl_->module);
  impl_->module = nullptr;
}

bool OptixRuntime::pipelineReady() const {
  return attached() && impl_->pipeline;
}

bool OptixRuntime::createPreviewPipeline(const void* optixIr,
                                         size_t optixIrBytes,
                                         std::string* err) {
  if (!attached() || !optixIr || optixIrBytes == 0) {
    if (err) *err = "invalid OptiX pipeline input";
    return false;
  }
  destroyPipeline();
  OptixModuleCompileOptions moduleOptions{};
  moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_LEVEL_3;
  moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
  OptixPipelineCompileOptions pipelineOptions{};
  pipelineOptions.traversableGraphFlags =
      OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
  pipelineOptions.numPayloadValues = 12;
  pipelineOptions.numAttributeValues = 2;
  pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
  pipelineOptions.pipelineLaunchParamsVariableName =
      "lusdviewOptixLaunchParams";
  pipelineOptions.pipelineLaunchParamsSizeInBytes = kOptixLaunchParamsBytes;
  pipelineOptions.usesPrimitiveTypeFlags =
      OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
  char log[4096]{};
  size_t logBytes = sizeof(log);
  OptixResult result = impl_->table.optixModuleCreate(
      impl_->context, &moduleOptions, &pipelineOptions,
      static_cast<const char*>(optixIr), optixIrBytes, log, &logBytes,
      &impl_->module);
  auto fail = [&](const char* operation, OptixResult failure) {
    if (err) {
      *err = std::string(operation) + " failed: " +
             std::to_string(static_cast<int>(failure));
      if (logBytes > 1) *err += std::string("; ") + log;
    }
    destroyPipeline();
    return false;
  };
  if (result != OPTIX_SUCCESS) return fail("optixModuleCreate", result);

  OptixProgramGroupOptions groupOptions{};
  auto makeGroup = [&](OptixProgramGroupKind kind, const char* entry,
                       OptixProgramGroup* group) {
    OptixProgramGroupDesc desc{};
    desc.kind = kind;
    if (kind == OPTIX_PROGRAM_GROUP_KIND_RAYGEN) {
      desc.raygen.module = impl_->module;
      desc.raygen.entryFunctionName = entry;
    } else if (kind == OPTIX_PROGRAM_GROUP_KIND_MISS) {
      desc.miss.module = impl_->module;
      desc.miss.entryFunctionName = entry;
    } else {
      desc.hitgroup.moduleCH = impl_->module;
      desc.hitgroup.entryFunctionNameCH = entry;
    }
    logBytes = sizeof(log);
    log[0] = '\0';
    return impl_->table.optixProgramGroupCreate(
        impl_->context, &desc, 1, &groupOptions, log, &logBytes, group);
  };
  result = makeGroup(OPTIX_PROGRAM_GROUP_KIND_RAYGEN,
                     "__raygen__lusdview", &impl_->raygen);
  if (result != OPTIX_SUCCESS) return fail("OptiX raygen program", result);
  result = makeGroup(OPTIX_PROGRAM_GROUP_KIND_MISS,
                     "__miss__lusdview", &impl_->miss);
  if (result != OPTIX_SUCCESS) return fail("OptiX miss program", result);
  result = makeGroup(OPTIX_PROGRAM_GROUP_KIND_HITGROUP,
                     "__closesthit__lusdview", &impl_->hit);
  if (result != OPTIX_SUCCESS) return fail("OptiX hit program", result);
  const OptixProgramGroup groups[] = {impl_->raygen, impl_->miss, impl_->hit};
  OptixPipelineLinkOptions linkOptions{};
  linkOptions.maxTraceDepth = 1;
  linkOptions.maxTraversableGraphDepth = 2;
  logBytes = sizeof(log);
  log[0] = '\0';
  result = impl_->table.optixPipelineCreate(
      impl_->context, &pipelineOptions, &linkOptions, groups, 3, log,
      &logBytes, &impl_->pipeline);
  if (result != OPTIX_SUCCESS) return fail("optixPipelineCreate", result);
  result = impl_->table.optixPipelineSetStackSizeFromCallDepths(
      impl_->pipeline, 1, 0, 0, 0, 2);
  if (result != OPTIX_SUCCESS)
    return fail("optixPipelineSetStackSizeFromCallDepths", result);
  if (err) err->clear();
  return true;
}

bool OptixRuntime::packPreviewSbt(
    const std::vector<uint64_t>& triangleOffsets, std::vector<uint8_t>* packed,
    uint32_t* missOffset, uint32_t* hitOffset, uint32_t* hitStride,
    std::string* err) const {
  if (!pipelineReady() || !packed || !missOffset || !hitOffset || !hitStride ||
      triangleOffsets.empty()) {
    if (err) *err = "OptiX pipeline is not ready for SBT packing";
    return false;
  }
  constexpr uint32_t kHeader = OPTIX_SBT_RECORD_HEADER_SIZE;
  constexpr uint32_t kHitStride =
      (kHeader + sizeof(uint64_t) + OPTIX_SBT_RECORD_ALIGNMENT - 1u) &
      ~(OPTIX_SBT_RECORD_ALIGNMENT - 1u);
  *missOffset = kHeader;
  *hitOffset = 2u * kHeader;
  *hitStride = kHitStride;
  packed->assign(*hitOffset + triangleOffsets.size() * kHitStride, uint8_t{0});
  const OptixProgramGroup fixedGroups[2] = {impl_->raygen, impl_->miss};
  for (size_t i = 0; i < 2; ++i) {
    const OptixResult result = impl_->table.optixSbtRecordPackHeader(
        fixedGroups[i], packed->data() + i * kHeader);
    if (result != OPTIX_SUCCESS) {
      if (err) *err = "OptiX SBT header packing failed: " +
                      std::to_string(static_cast<int>(result));
      packed->clear();
      return false;
    }
  }
  for (size_t i = 0; i < triangleOffsets.size(); ++i) {
    uint8_t* record = packed->data() + *hitOffset + i * kHitStride;
    const OptixResult result =
        impl_->table.optixSbtRecordPackHeader(impl_->hit, record);
    if (result != OPTIX_SUCCESS) {
      if (err) *err = "OptiX hit SBT header packing failed: " +
                      std::to_string(static_cast<int>(result));
      packed->clear();
      return false;
    }
    std::memcpy(record + kHeader, &triangleOffsets[i], sizeof(uint64_t));
  }
  return true;
}

bool OptixRuntime::launchPreview(uintptr_t cudaStream, uintptr_t launchParams,
                                 size_t launchParamsBytes,
                                 uintptr_t sbtRecords, uint32_t missOffset,
                                 uint32_t hitOffset, uint32_t hitStride,
                                 uint32_t hitCount, unsigned int width,
                                 unsigned int height, std::string* err) const {
  if (!pipelineReady() || !launchParams || !sbtRecords || width == 0 ||
      height == 0) {
    if (err) *err = "invalid OptiX preview launch request";
    return false;
  }
  OptixShaderBindingTable sbt{};
  sbt.raygenRecord = static_cast<CUdeviceptr>(sbtRecords);
  sbt.missRecordBase = static_cast<CUdeviceptr>(sbtRecords + missOffset);
  sbt.missRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
  sbt.missRecordCount = 1;
  sbt.hitgroupRecordBase = static_cast<CUdeviceptr>(sbtRecords + hitOffset);
  sbt.hitgroupRecordStrideInBytes = hitStride;
  sbt.hitgroupRecordCount = hitCount;
  const OptixResult result = impl_->table.optixLaunch(
      impl_->pipeline, reinterpret_cast<CUstream>(cudaStream),
      static_cast<CUdeviceptr>(launchParams), launchParamsBytes, &sbt, width,
      height, 1);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "optixLaunch failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  return true;
}

namespace {

void FillTriangleInput(uintptr_t vertices, size_t triangleCount,
                       CUdeviceptr* vertexBuffer, unsigned int* flags,
                       OptixBuildInput* input) {
  *vertexBuffer = static_cast<CUdeviceptr>(vertices);
  *flags = OPTIX_GEOMETRY_FLAG_NONE;
  *input = OptixBuildInput{};
  input->type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
  input->triangleArray.vertexBuffers = vertexBuffer;
  input->triangleArray.numVertices =
      static_cast<unsigned int>(triangleCount * 3u);
  input->triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
  input->triangleArray.vertexStrideInBytes = 3u * sizeof(float);
  input->triangleArray.flags = flags;
  input->triangleArray.numSbtRecords = 1;
}

OptixAccelBuildOptions GasBuildOptions(bool allowUpdate,
                                       bool update = false) {
  OptixAccelBuildOptions options{};
  options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE |
      (allowUpdate ? OPTIX_BUILD_FLAG_ALLOW_UPDATE
                   : OPTIX_BUILD_FLAG_ALLOW_COMPACTION);
  options.operation = update ? OPTIX_BUILD_OPERATION_UPDATE
                             : OPTIX_BUILD_OPERATION_BUILD;
  return options;
}

}  // namespace

bool OptixRuntime::triangleGasSizes(uintptr_t vertices, size_t triangleCount,
                                    OptixAccelSizes* sizes,
                                    std::string* err,
                                    bool allowUpdate) const {
  if (!attached() || !sizes || !vertices || triangleCount == 0 ||
      triangleCount > static_cast<size_t>(UINT_MAX) / 3u) {
    if (err) *err = "invalid OptiX triangle GAS size request";
    return false;
  }
  CUdeviceptr vertexBuffer = 0;
  unsigned int flags = 0;
  OptixBuildInput input{};
  FillTriangleInput(vertices, triangleCount, &vertexBuffer, &flags, &input);
  const OptixAccelBuildOptions options = GasBuildOptions(allowUpdate);
  OptixAccelBufferSizes nativeSizes{};
  const OptixResult result = impl_->table.optixAccelComputeMemoryUsage(
      impl_->context, &options, &input, 1, &nativeSizes);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "optixAccelComputeMemoryUsage failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  sizes->outputBytes = nativeSizes.outputSizeInBytes;
  sizes->temporaryBytes = allowUpdate
                              ? std::max(nativeSizes.tempSizeInBytes,
                                         nativeSizes.tempUpdateSizeInBytes)
                              : nativeSizes.tempSizeInBytes;
  return true;
}

bool OptixRuntime::buildTriangleGas(
    uintptr_t cudaStream, uintptr_t vertices, size_t triangleCount,
    uintptr_t temporary, size_t temporaryBytes, uintptr_t output,
    size_t outputBytes, uintptr_t compactedSizeOutput, uint64_t* traversable,
    std::string* err, bool allowUpdate) const {
  if (!attached() || !temporary || !output || !traversable) {
    if (err) *err = "invalid OptiX triangle GAS build request";
    return false;
  }
  CUdeviceptr vertexBuffer = 0;
  unsigned int flags = 0;
  OptixBuildInput input{};
  FillTriangleInput(vertices, triangleCount, &vertexBuffer, &flags, &input);
  const OptixAccelBuildOptions options = GasBuildOptions(allowUpdate);
  OptixTraversableHandle handle = 0;
  OptixAccelEmitDesc compactedSize{};
  compactedSize.result = static_cast<CUdeviceptr>(compactedSizeOutput);
  compactedSize.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
  const OptixAccelEmitDesc* emitted = compactedSizeOutput ? &compactedSize
                                                          : nullptr;
  const OptixResult result = impl_->table.optixAccelBuild(
      impl_->context, reinterpret_cast<CUstream>(cudaStream), &options, &input,
      1, static_cast<CUdeviceptr>(temporary), temporaryBytes,
      static_cast<CUdeviceptr>(output), outputBytes, &handle, emitted,
      emitted ? 1u : 0u);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "optixAccelBuild failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  *traversable = static_cast<uint64_t>(handle);
  return true;
}

bool OptixRuntime::updateTriangleGas(
    uintptr_t cudaStream, uintptr_t vertices, size_t triangleCount,
    uintptr_t temporary, size_t temporaryBytes, uintptr_t output,
    size_t outputBytes, uint64_t* traversable, std::string* err) const {
  if (!attached() || !temporary || !output || !traversable) {
    if (err) *err = "invalid OptiX triangle GAS update request";
    return false;
  }
  CUdeviceptr vertexBuffer = 0;
  unsigned int flags = 0;
  OptixBuildInput input{};
  FillTriangleInput(vertices, triangleCount, &vertexBuffer, &flags, &input);
  const OptixAccelBuildOptions options = GasBuildOptions(true, true);
  OptixTraversableHandle handle = 0;
  const OptixResult result = impl_->table.optixAccelBuild(
      impl_->context, reinterpret_cast<CUstream>(cudaStream), &options, &input,
      1, static_cast<CUdeviceptr>(temporary), temporaryBytes,
      static_cast<CUdeviceptr>(output), outputBytes, &handle, nullptr, 0);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "OptiX GAS update failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  *traversable = static_cast<uint64_t>(handle);
  return true;
}

bool OptixRuntime::compactGas(uintptr_t cudaStream, uint64_t inputTraversable,
                              uintptr_t output, size_t outputBytes,
                              uint64_t* outputTraversable,
                              std::string* err) const {
  if (!attached() || !inputTraversable || !output || outputBytes == 0 ||
      !outputTraversable || !impl_->table.optixAccelCompact) {
    if (err) *err = "invalid OptiX GAS compaction request";
    return false;
  }
  OptixTraversableHandle handle = 0;
  const OptixResult result = impl_->table.optixAccelCompact(
      impl_->context, reinterpret_cast<CUstream>(cudaStream),
      static_cast<OptixTraversableHandle>(inputTraversable),
      static_cast<CUdeviceptr>(output), outputBytes, &handle);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "optixAccelCompact failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  *outputTraversable = static_cast<uint64_t>(handle);
  return true;
}

bool OptixRuntime::packInstances(
    const std::vector<OptixInstanceInput>& instances,
    std::vector<uint8_t>* packed, std::string* err) const {
  if (!packed || instances.size() > static_cast<size_t>(UINT_MAX)) {
    if (err) *err = "invalid OptiX instance pack request";
    return false;
  }
  std::vector<OptixInstance> native(instances.size());
  for (size_t i = 0; i < instances.size(); ++i) {
    if (!instances[i].traversable || instances[i].instanceId > 0x00ffffffu) {
      if (err) *err = "invalid OptiX instance handle or 24-bit instance id";
      return false;
    }
    std::memcpy(native[i].transform, instances[i].transform,
                sizeof(native[i].transform));
    native[i].instanceId = instances[i].instanceId;
    native[i].sbtOffset = instances[i].sbtOffset;
    native[i].visibilityMask = 0xffu;
    native[i].flags = OPTIX_INSTANCE_FLAG_NONE;
    native[i].traversableHandle =
        static_cast<OptixTraversableHandle>(instances[i].traversable);
  }
  packed->resize(native.size() * sizeof(OptixInstance));
  if (!native.empty())
    std::memcpy(packed->data(), native.data(), packed->size());
  return true;
}

bool OptixRuntime::instanceAccelSizes(uintptr_t packedInstances,
                                      size_t instanceCount,
                                      OptixAccelSizes* sizes,
                                      std::string* err) const {
  if (!attached() || !packedInstances || instanceCount == 0 || !sizes ||
      instanceCount > static_cast<size_t>(UINT_MAX)) {
    if (err) *err = "invalid OptiX IAS size request";
    return false;
  }
  OptixBuildInput input{};
  input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
  input.instanceArray.instances = static_cast<CUdeviceptr>(packedInstances);
  input.instanceArray.numInstances = static_cast<unsigned int>(instanceCount);
  OptixAccelBuildOptions options{};
  options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
  options.operation = OPTIX_BUILD_OPERATION_BUILD;
  OptixAccelBufferSizes nativeSizes{};
  const OptixResult result = impl_->table.optixAccelComputeMemoryUsage(
      impl_->context, &options, &input, 1, &nativeSizes);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "OptiX IAS memory query failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  sizes->outputBytes = nativeSizes.outputSizeInBytes;
  sizes->temporaryBytes = nativeSizes.tempSizeInBytes;
  return true;
}

bool OptixRuntime::buildInstanceAccel(
    uintptr_t cudaStream, uintptr_t packedInstances, size_t instanceCount,
    uintptr_t temporary, size_t temporaryBytes, uintptr_t output,
    size_t outputBytes, uint64_t* traversable, std::string* err) const {
  if (!attached() || !packedInstances || instanceCount == 0 || !temporary ||
      !output || !traversable || instanceCount > static_cast<size_t>(UINT_MAX)) {
    if (err) *err = "invalid OptiX IAS build request";
    return false;
  }
  OptixBuildInput input{};
  input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
  input.instanceArray.instances = static_cast<CUdeviceptr>(packedInstances);
  input.instanceArray.numInstances = static_cast<unsigned int>(instanceCount);
  OptixAccelBuildOptions options{};
  options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
  options.operation = OPTIX_BUILD_OPERATION_BUILD;
  OptixTraversableHandle handle = 0;
  const OptixResult result = impl_->table.optixAccelBuild(
      impl_->context, reinterpret_cast<CUstream>(cudaStream), &options, &input,
      1, static_cast<CUdeviceptr>(temporary), temporaryBytes,
      static_cast<CUdeviceptr>(output), outputBytes, &handle, nullptr, 0);
  if (result != OPTIX_SUCCESS) {
    if (err) *err = "OptiX IAS build failed: " +
                    std::to_string(static_cast<int>(result));
    return false;
  }
  *traversable = static_cast<uint64_t>(handle);
  return true;
}

}  // namespace lusdview
