// SPDX-License-Identifier: Apache-2.0
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "next/resolver/asset-resolver.hh"
#include "tydra/next/scene-preview.hh"

int main(int argc, char** argv) {
  using namespace lightusd::tydra::next;
  ScenePreviewOptions options;
  options.composition.num_threads = 0;
  options.composition.max_layer_memory = size_t(24576) << 20;
  options.composition.usdc_limits.max_fields = 300000000;
  options.composition.usdc_limits.max_paths = 300000000;
  options.composition.usdc_limits.max_specs = 300000000;
  options.composition.usdc_limits.max_fieldset_indices = 300000000;
  std::string source, output;
  bool budget_explicit = false;
  bool lod = false;
  auto number = [](const char* arg, uint64_t* n) {
    char* end = nullptr;
    errno = 0;
    if (!arg || !*arg || *arg == '-') return false;
    const auto value = std::strtoull(arg, &end, 10);
    if (errno || !end || *end) return false;
    *n = value;
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::puts(
          "lusdpreview input.usd [export-directory]\n  --kind "
          "gaussian|points|volume (default gaussian)\n  --preview-cache-dir "
          "DIR\n  --preview-budget-mib N (total renderer cap; default 1/4 "
          "VRAM)\n  "
          "--host-budget-mib N (default 16384)\n  --preview-resolution N (auto "
          "by default)\n  --texture-max-edge N (default 512)\n  --batch-prims "
          "N --batch-roots N\n  --time-code T --variant SET=VALUE --refresh "
          "--progress --lod");
      return 0;
    }
    if (a == "--lod") {
      lod = true;
      continue;
    }
    if (a == "--refresh") {
      options.refresh = true;
      continue;
    }
    if (a == "--progress") {
      options.progress = [last = std::chrono::steady_clock::time_point{}](
                             const std::string& phase, uint64_t n) mutable {
        const auto now = std::chrono::steady_clock::now();
        if (phase == "prototype-samples" &&
            now - last < std::chrono::seconds(2))
          return true;
        last = now;
        std::fprintf(stderr, "[scene-preview] %s %llu\n", phase.c_str(),
                     static_cast<unsigned long long>(n));
        return true;
      };
      continue;
    }
    if (a.rfind("--", 0) == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing option value: %s\n", a.c_str());
        return 2;
      }
      const std::string value = argv[++i];
      uint64_t n = 0;
      if (a == "--kind") {
        if (!ParseScenePreviewKind(value, &options.kind)) return 2;
      } else if (a == "--preview-cache-dir")
        options.cache_directory = value;
      else if (a == "--time-code") {
        char* end = nullptr;
        options.time_code = std::strtod(value.c_str(), &end);
        if (!end || *end || !std::isfinite(options.time_code)) return 2;
      } else if (a == "--variant") {
        const auto p = value.find('=');
        if (p == std::string::npos) return 2;
        options.composition.variant_overrides[value.substr(0, p)] =
            value.substr(p + 1);
      } else if (!number(value.c_str(), &n) || !n) {
        std::fprintf(stderr, "invalid numeric option: %s\n", a.c_str());
        return 2;
      } else if (a == "--preview-budget-mib" && n <= UINT64_MAX / (1 << 20)) {
        options.gpu_budget_bytes = n << 20;
        budget_explicit = true;
      } else if (a == "--host-budget-mib" && n <= UINT64_MAX / (1 << 20))
        options.host_budget_bytes = n << 20;
      else if (a == "--preview-resolution" && n <= 65536)
        options.resolution = static_cast<uint32_t>(n);
      else if (a == "--texture-max-edge" && n <= 16384)
        options.texture_max_edge = static_cast<uint32_t>(n);
      else if (a == "--batch-prims")
        options.batch_prims = static_cast<size_t>(n);
      else if (a == "--batch-roots")
        options.batch_roots = static_cast<size_t>(n);
      else {
        std::fprintf(stderr, "unknown or invalid option: %s\n", a.c_str());
        return 2;
      }
    } else if (source.empty())
      source = a;
    else if (output.empty())
      output = a;
    else
      return 2;
  }
  if (source.empty()) {
    std::fprintf(stderr,
                 "usage: lusdpreview input.usd [export-directory] --help\n");
    return 2;
  }
  lightusd::next::AssetResolver resolver;
  if (!budget_explicit) {
    uint64_t capacity = 0, available = 0;
    QueryScenePreviewGpuMemory(&capacity, &available);
    options.gpu_budget_bytes = ScenePreviewGpuBudget(capacity, available);
  }
  const auto result = ConvertScenePreview(source, resolver, options);
  if (!result.warning.empty())
    std::fprintf(stderr, "%s", result.warning.c_str());
  if (!result.success) {
    std::fprintf(stderr, "preview failed: %s\n", result.error.c_str());
    return 1;
  }
  if (lod) {
    const auto levels = BuildScenePreviewLods(result, options);
    if (!levels.success) {
      std::fprintf(stderr, "LOD build failed: %s\n", levels.error.c_str());
      return 1;
    }
    std::printf("[scene-preview] lod_root=%s\n", levels.root_path.c_str());
  }
  if (!output.empty()) {
    std::string error;
    if (!ExportScenePreview(result, output, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }
  std::printf(
      "[scene-preview] kind=%s cache_hit=%d prims=%llu meshes=%llu "
      "instances=%llu samples=%llu cell=%.9g planned_gpu_mib=%.2f "
      "elapsed=%.3fs\n%s\n",
      ScenePreviewKindName(options.kind), result.cache_hit ? 1 : 0,
      static_cast<unsigned long long>(result.source_prims),
      static_cast<unsigned long long>(result.source_meshes),
      static_cast<unsigned long long>(result.source_instances),
      static_cast<unsigned long long>(result.samples), result.voxel_size,
      double(result.estimated_gpu_bytes) / (1 << 20), result.elapsed_seconds,
      result.usd_path.c_str());
  return 0;
}
