// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "next/resolver/asset-resolver.hh"
#include "scene-preview-compute.hh"
#include "scene-preview.hh"

namespace lightusd {
namespace tydra {
namespace next {
bool QueryScenePreviewGpuMemory(
    uint64_t* capacity, uint64_t* available,
    const std::shared_ptr<const HostServices>& host_services,
    const std::string& library_path) {
  if (!capacity || !available) return false;
  *capacity = 0;
  *available = 0;
  if (!host_services || !host_services->open_library || library_path.empty()) return false;
  std::string error;
  auto lib = host_services->open_library(library_path, &error);
  if (!lib) return false;
  using Init = int (*)();
  using Shutdown = int (*)();
  using Handle = int (*)(unsigned, void**);
  struct Memory {
    unsigned long long total, free, used;
  };
  using Info = int (*)(void*, Memory*);
  const auto init = reinterpret_cast<Init>(lib->Symbol("nvmlInit_v2"));
  const auto shutdown = reinterpret_cast<Shutdown>(lib->Symbol("nvmlShutdown"));
  const auto handle =
      reinterpret_cast<Handle>(lib->Symbol("nvmlDeviceGetHandleByIndex_v2"));
  const auto info =
      reinterpret_cast<Info>(lib->Symbol("nvmlDeviceGetMemoryInfo"));
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
  return ok;
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
    if (a == "--preview-volume-carrier") {
      request->enabled = true;
      request->direct_volume = false;
      continue;
    }
    if (a == "--preview-software" || a == "--preview-debug-validate" ||
        a == "--preview-trace-pipeline") {
      if (a == "--preview-software") request->options.compute_options.software_only = true;
      if (a == "--preview-debug-validate") request->options.compute_options.debug_validate = true;
      if (a == "--preview-trace-pipeline") request->options.compute_options.trace_pipeline = true;
      request->enabled = true;
      continue;
    }
    if (a == "--preview-native-host") {
      auto services = CreateNativeHostServices({}, error);
      if (!services) return false;
      request->options.host_services = services;
      request->options.compute_options.host_services = services;
      request->enabled = true;
      continue;
    }
    if (a == "--preview-vulkan-library" || a == "--preview-cuda-driver-library" ||
        a == "--preview-cuda-compiler-library") {
      if (i + 1 >= *argc || !argv[i + 1][0]) {
        if (error) *error = "missing preview library filename";
        return false;
      }
      const std::string path = argv[++i];
      if (a == "--preview-vulkan-library") request->options.compute_options.vulkan_library = path;
      if (a == "--preview-cuda-driver-library") request->options.compute_options.cuda_driver_library = path;
      if (a == "--preview-cuda-compiler-library") request->options.compute_options.cuda_compiler_library = path;
      request->enabled = true;
      continue;
    }
    if (a == "--preview-compute") {
      if (i + 1 >= *argc) {
        if (error) *error = "missing preview compute backend";
        return false;
      }
      const std::string backend = argv[++i];
      if (backend == "auto")
        request->options.compute_backend = PreviewComputeBackend::Auto;
      else if (backend == "cpu")
        request->options.compute_backend = PreviewComputeBackend::Cpu;
      else if (backend == "vulkan")
        request->options.compute_backend = PreviewComputeBackend::Vulkan;
      else if (backend == "cuda")
        request->options.compute_backend = PreviewComputeBackend::Cuda;
      else {
        if (error) *error = "invalid preview compute backend";
        return false;
      }
      request->enabled = true;
      continue;
    }
    if (a == "--preview-no-source-refine") {
      request->enabled = true;
      request->source_refinement = false;
      continue;
    }
    if (a == "--preview-lod-pixels") {
      if (i + 1 >= *argc) {
        if (error) *error = "missing preview LOD pixel error";
        return false;
      }
      char* end = nullptr;
      const double pixels = std::strtod(argv[++i], &end);
      if (!end || end == argv[i] || *end || !std::isfinite(pixels) ||
          pixels <= 0) {
        if (error) *error = "invalid preview LOD pixel error";
        return false;
      }
      request->enabled = true;
      request->lod_pixel_error = pixels;
      continue;
    }
    if (a == "--preview-volume-headlight" ||
        a == "--preview-volume-no-shadows") {
      request->enabled = true;
      if (a == "--preview-volume-headlight")
        request->volume.scene_lights = false;
      else
        request->volume.shadows = false;
      continue;
    }
    if (a == "--preview-volume-bounces" || a == "--preview-volume-samples" ||
        a == "--preview-light-samples" ||
        a == "--preview-accumulation-frames" || a == "--preview-volume-steps" ||
        a == "--preview-shadow-steps") {
      if (i + 1 >= *argc) {
        if (error) *error = "missing value for " + a;
        return false;
      }
      char* end = nullptr;
      const char* text = argv[++i];
      const unsigned long n = std::strtoul(text, &end, 10);
      const unsigned long cap =
          a == "--preview-volume-steps" || a == "--preview-shadow-steps" ? 65536
          : a == "--preview-volume-bounces"                              ? 256
          : a == "--preview-accumulation-frames"                         ? 4096
                                                                         : 64;
      if (end == text || *end || !n || n > cap) {
        if (error) *error = "invalid value for " + a;
        return false;
      }
      request->enabled = true;
      if (a == "--preview-volume-bounces")
        request->volume.scatter_bounces = static_cast<uint32_t>(n);
      else if (a == "--preview-volume-steps")
        request->volume.max_steps = static_cast<uint32_t>(n);
      else if (a == "--preview-shadow-steps")
        request->volume.shadow_steps = static_cast<uint32_t>(n);
      else if (a == "--preview-accumulation-frames")
        request->volume.accumulation_frames = static_cast<uint32_t>(n);
      else if (a == "--preview-light-samples")
        request->volume.light_samples = static_cast<uint32_t>(n);
      else
        request->volume.scatter_samples = static_cast<uint32_t>(n);
      continue;
    }
    if (a == "--preview-volume-mode" || a == "--preview-volume-density" ||
        a == "--preview-volume-scattering" ||
        a == "--preview-volume-emission" ||
        a == "--preview-volume-anisotropy" ||
        a == "--preview-volume-step-size" || a == "--preview-volume-error" ||
        a == "--preview-volume-cutoff") {
      if (i + 1 >= *argc) {
        if (error) *error = "missing value for " + a;
        return false;
      }
      request->enabled = true;
      const std::string text = argv[++i];
      if (a == "--preview-volume-mode") {
        if (text != "surface" && text != "medium") {
          if (error) *error = "invalid preview volume mode";
          return false;
        }
        request->volume.medium = text == "medium";
      } else {
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        const bool phase = a == "--preview-volume-anisotropy";
        if (end == text.c_str() || *end || !std::isfinite(value) ||
            (phase ? (value < -.95f || value > .95f)
                   : (value < 0 || value > 1e6f)) ||
            (a == "--preview-volume-scattering" && value > 1) ||
            (a == "--preview-volume-step-size" && (value <= 0 || value > 2)) ||
            ((a == "--preview-volume-error" ||
              a == "--preview-volume-cutoff") &&
             value > 1)) {
          if (error) *error = "invalid value for " + a;
          return false;
        }
        if (a == "--preview-volume-density")
          request->volume.density = value;
        else if (a == "--preview-volume-scattering")
          request->volume.scattering = value;
        else if (a == "--preview-volume-emission")
          request->volume.emission = value;
        else if (a == "--preview-volume-step-size")
          request->volume.step_size = value;
        else if (a == "--preview-volume-cutoff")
          request->volume.transmittance_cutoff = value;
        else if (a == "--preview-volume-error")
          request->volume.integration_error = value;
        else
          request->volume.anisotropy = value;
      }
      continue;
    }
    if (a == "--preview-volume-grids") {
      request->enabled = true;
      request->native_volume = true;
      request->source_refinement = false;
      request->options.kind = ScenePreviewKind::Volume;
      request->direct_volume = true;
      continue;
    }
    if (a == "--preview-no-lod") {
      request->enabled = true;
      request->camera_lod = false;
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
  if (request.native_volume &&
      (request.options.kind != ScenePreviewKind::Volume ||
       !request.direct_volume)) {
    if (error)
      *error = "--preview-volume-grids requires direct volume rendering";
    return false;
  }
  if (!source || source->empty()) {
    if (error) *error = "scene preview needs an input USD file";
    return false;
  }
  uint64_t total = 0, available = 0;
  // GPU capacity is an explicit caller observation; do not load NVML here.
  if (!total && !request.options.compute_options.software_only) total = gpu_capacity;
  if (!request.budget_explicit)
    request.options.gpu_budget_bytes = ScenePreviewGpuBudget(total, available);
  request.options.composition.num_threads = 0;
  request.options.composition.max_layer_memory =
      std::min(request.options.host_budget_bytes, uint64_t(24576) << 20);
  request.options.composition.usdc_limits.max_fields = 300000000;
  request.options.composition.usdc_limits.max_specs = 300000000;
  request.options.composition.usdc_limits.max_paths = 300000000;
  request.options.composition.usdc_limits.max_fieldset_indices = 300000000;
  request.options.composition.max_array_elements = static_cast<size_t>(
      std::min(uint64_t(1) << 30, request.options.host_budget_bytes / 16));
  request.options.progress = [](const std::string& phase, uint64_t n) {
    if (phase != "batch" && phase != "plan" && phase != "write") return true;
    std::fprintf(stderr, "[scene-preview] %s %llu\n", phase.c_str(),
                 static_cast<unsigned long long>(n));
    return true;
  };
  ::lightusd::next::AssetResolver resolver;
  if (request.options.kind == ScenePreviewKind::Volume && request.direct_volume)
    request.options.volume_surface_carrier = false;
  request.source = *source;
  const auto result =
      request.native_volume
          ? OpenScenePreviewVolume(*source, request.options)
          : ConvertScenePreview(*source, resolver, request.options);
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
  request.result = result;
  *source = result.render_usd_path;
  if (request.options.kind == ScenePreviewKind::Volume &&
      request.direct_volume) {
    request.lods.success = true;
    request.lods.root_path = *source;
    request.lods.cell_size = result.voxel_size;
    request.lods.gpu_budget_bytes = request.options.gpu_budget_bytes;
  } else if (request.camera_lod) {
    request.lods = BuildScenePreviewLods(result, request.options);
    if (!request.lods.success) {
      if (error) *error = request.lods.error;
      return false;
    }
    *source = request.lods.root_path;
  }
  return true;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
