// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "next/resolver/asset-resolver.hh"
#include "scene-preview.hh"
#if defined(__unix__)
#include <dlfcn.h>
#endif

namespace lightusd {
namespace tydra {
namespace next {
bool QueryScenePreviewGpuMemory(uint64_t* capacity, uint64_t* available) {
  if (!capacity || !available) return false;
  *capacity = 0;
  *available = 0;
#if defined(__unix__)
  void* lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return false;
  using Init = int (*)();
  using Shutdown = int (*)();
  using Handle = int (*)(unsigned, void**);
  struct Memory {
    unsigned long long total, free, used;
  };
  using Info = int (*)(void*, Memory*);
  const auto init = reinterpret_cast<Init>(dlsym(lib, "nvmlInit_v2"));
  const auto shutdown = reinterpret_cast<Shutdown>(dlsym(lib, "nvmlShutdown"));
  const auto handle =
      reinterpret_cast<Handle>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
  const auto info =
      reinterpret_cast<Info>(dlsym(lib, "nvmlDeviceGetMemoryInfo"));
  bool ok = false;
  if (init && shutdown && handle && info && init() == 0) {
    void* device = nullptr;
    Memory memory{};
    if (handle(0, &device) == 0 && info(device, &memory) == 0) {
      *capacity = memory.total;
      *available = memory.free;
      ok = true;
    }
    shutdown();
  }
  dlclose(lib);
  return ok;
#else
  return false;
#endif
}
bool ExtractScenePreviewArguments(int* argc, char** argv,
                                  ScenePreviewRequest* request,
                                  std::string* error) {
  if (!argc || !argv || !request) return false;
  int keep = 1;
  for (int i = 1; i < *argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scene-preview") {
      request->enabled = true;
      continue;
    }
    if (a == "--preview-refresh") {
      request->enabled = true;
      request->options.refresh = true;
      continue;
    }
    if (a != "--preview-kind" && a != "--preview-cache-dir" &&
        a != "--preview-budget-mib" && a != "--preview-resolution" &&
        a != "--preview-host-mib") {
      argv[keep++] = argv[i];
      continue;
    }
    request->enabled = true;
    if (i + 1 >= *argc) {
      if (error) *error = "missing value for " + a;
      return false;
    }
    const char* value = argv[++i];
    if (a == "--preview-kind") {
      if (!ParseScenePreviewKind(value, &request->options.kind)) {
        if (error) *error = "invalid preview kind";
        return false;
      }
      continue;
    }
    if (a == "--preview-cache-dir") {
      request->options.cache_directory = value;
      continue;
    }
    char* end = nullptr;
    errno = 0;
    const uint64_t n = std::strtoull(value, &end, 10);
    if (!*value || *value == '-' || errno || !end || *end || !n ||
        n > UINT64_MAX / (uint64_t(1) << 20)) {
      if (error) *error = "invalid value for " + a;
      return false;
    }
    if (a == "--preview-resolution") {
      if (n > 65536) {
        if (error) *error = "preview resolution exceeds 65536";
        return false;
      }
      request->options.resolution = static_cast<uint32_t>(n);
    } else if (a == "--preview-budget-mib") {
      request->options.gpu_budget_bytes = n << 20;
      request->budget_explicit = true;
    } else
      request->options.host_budget_bytes = n << 20;
  }
  *argc = keep;
  argv[keep] = nullptr;
  return true;
}
bool PrepareScenePreview(std::string* source, ScenePreviewRequest& request,
                         uint64_t gpu_capacity, std::string* error) {
  if (!request.enabled) return true;
  if (!source || source->empty()) {
    if (error) *error = "scene preview needs an input USD file";
    return false;
  }
  uint64_t total = 0, available = 0;
  QueryScenePreviewGpuMemory(&total, &available);
  if (!total) total = gpu_capacity;
  if (!request.budget_explicit)
    request.options.gpu_budget_bytes = ScenePreviewGpuBudget(total, available);
  request.options.composition.num_threads = 0;
  request.options.composition.max_layer_memory =
      std::min(request.options.host_budget_bytes, uint64_t(24576) << 20);
  request.options.composition.usdc_limits.max_fields = 300000000;
  request.options.composition.usdc_limits.max_specs = 300000000;
  request.options.composition.usdc_limits.max_paths = 300000000;
  request.options.composition.usdc_limits.max_fieldset_indices = 300000000;
  request.options.progress = [](const std::string& phase, uint64_t n) {
    if (phase != "batch" && phase != "plan" && phase != "write") return true;
    std::fprintf(stderr, "[scene-preview] %s %llu\n", phase.c_str(),
                 static_cast<unsigned long long>(n));
    return true;
  };
  ::lightusd::next::AssetResolver resolver;
  const auto result = ConvertScenePreview(*source, resolver, request.options);
  if (!result.success) {
    if (error) *error = result.error;
    return false;
  }
  if (!result.warning.empty())
    std::fprintf(stderr, "%s", result.warning.c_str());
  std::fprintf(
      stderr,
      "[scene-preview] kind=%s cache_hit=%d samples=%llu planned_gpu_mib=%.2f "
      "elapsed=%.3fs\n",
      ScenePreviewKindName(request.options.kind), result.cache_hit ? 1 : 0,
      static_cast<unsigned long long>(result.samples),
      double(result.estimated_gpu_bytes) / (1 << 20), result.elapsed_seconds);
  *source = result.render_usd_path;
  return true;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
