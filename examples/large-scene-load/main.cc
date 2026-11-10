// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// large-scene-load: memory-bounded loading benchmark for large USD scenes.
//
// Usage:
//   large-scene-load <scene.usd[a]> [options]
//     --mode=none|all|budget   payload mode (default: none)
//     --budget-mb=N            payload byte budget for --mode=budget
//     --extent-budget=N        optional projected extentsHint-area cap
//     --no-mmap-zero-copy      disable mmap/lazy-array composition
//     --no-dedup               release composition/layer caches between edits
//     --load-some=N            after load, stream in the first N deferred
//     payloads
//     --unload                 after --load-some, unload them again
//
// Reports process RSS (the trustworthy peak gauge), deferred payload count,
// estimated Stage memory, and source-layer count.

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "layer/asset-anchor.hh"
#include "stage/stage-session.hh"
#include "stage/stage.hh"
#include "types/value-view.hh"

namespace {

struct Options {
  enum class PayloadMode { LoadAll, LoadNone, Budget };
  PayloadMode payload_mode = PayloadMode::LoadNone;
  size_t payload_budget_mb = 12288;
  double payload_extent_budget = 0;
  bool mmap_zero_copy = true;
  bool dedup_layers = true;
};

uint64_t MiB(size_t value) {
  constexpr uint64_t unit = 1024 * 1024;
  return value > (std::numeric_limits<uint64_t>::max)() / unit
             ? (std::numeric_limits<uint64_t>::max)()
             : uint64_t(value) * unit;
}

struct PayloadBudget {
  uint64_t used = 0, bytes = 0;
  double extent_used = 0, extent_limit = 0;
  lightusd::AssetResolver* resolver = nullptr;
  std::mutex mutex;
  std::map<std::pair<std::string, std::string>, bool> decisions;
  bool Admit(const lightusd::Path& path, const std::string& asset,
             const lightusd::PrimSpec& owner) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto key = std::make_pair(path.str(), asset);
    const auto previous = decisions.find(key);
    if (previous != decisions.end()) return previous->second;
    const auto decide = [&](bool accept) {
      decisions.emplace(key, accept);
      return accept;
    };
    const std::string directory =
        lightusd::AssetAnchorPath(owner.asset_anchor_id());
    const std::string anchor =
        directory.empty() ? std::string()
        : lightusd::AssetResolver::IsPackagePath(directory)
            ? directory
            : directory + "/__layer__.usd";
    const std::string resolved = resolver->ResolvePath(asset, anchor);
    std::error_code file_error;
    const auto length = std::filesystem::file_size(
        std::filesystem::u8path(resolved), file_error);
    if (file_error || !length || length > bytes - used) return decide(false);
    double coverage = 0;
    if (extent_limit > 0) {
      const auto* value = owner.property_value("extentsHint");
      lightusd::ArrayScratch<float> scratch;
      lightusd::ArrayView<float> points;
      if (value && lightusd::GetFloatArrayView(*value, &scratch, &points) &&
          points.size >= 6 && points.size % 6 == 0) {
        double lo[3] = {points[0], points[1], points[2]};
        double hi[3] = {points[0], points[1], points[2]};
        bool finite = true;
        for (size_t i = 0; i < points.size; ++i) {
          const double v = points[i];
          finite = finite && std::isfinite(v);
          lo[i % 3] = std::min(lo[i % 3], v);
          hi[i % 3] = std::max(hi[i % 3], v);
        }
        if (finite)
          coverage = std::max({(hi[0] - lo[0]) * (hi[1] - lo[1]),
                               (hi[0] - lo[0]) * (hi[2] - lo[2]),
                               (hi[1] - lo[1]) * (hi[2] - lo[2])});
        if (!std::isfinite(coverage)) coverage = 0;
      }
      if (coverage > extent_limit - extent_used) return decide(false);
    }
    used += uint64_t(length);
    extent_used += coverage;
    return decide(true);
  }
};

// StageSession owns the cache, publishes immutable snapshots and retains the
// deferred-payload rules. The example does not use a legacy Stage adapter.
class CoreLoader {
 public:
  bool Load(const std::string& file, const Options& options,
            std::string* warning, std::string* error) {
    release_cache_ = !options.dedup_layers;
    lightusd::StageSessionOptions config;
    // This benchmark intentionally accepts trusted, very large local scenes.
    // Keep an 8 GiB per-file cap while allowing lazy arrays to exceed the
    // ordinary interactive resident/array-count defaults.
    config.load.input_policy = lightusd::InputPolicy::Trusted;
    config.load.limits = lightusd::ResourceLimits::Unlimited();
    const size_t asset_limit = size_t(
        std::min<uint64_t>(MiB(8192), (std::numeric_limits<size_t>::max)()));
    config.load.limits.max_input_bytes = asset_limit;
    config.load.limits.max_asset_bytes = asset_limit;
    config.load.limits.max_composition_depth = 1024;
    config.load.limits.max_namespace_depth = 1024;
    config.composition.usdc_lazy_arrays = options.mmap_zero_copy;
    config.composition.usdc_use_mmap = options.mmap_zero_copy;
    config.load.usdc_options.crate_options.lazy_arrays = options.mmap_zero_copy;
    config.load.usdc_options.crate_options.use_mmap = options.mmap_zero_copy;
    std::error_code path_error;
    const auto input_path =
        std::filesystem::absolute(std::filesystem::u8path(file), path_error);
    if (path_error) {
      if (error) *error = path_error.message();
      return false;
    }
    config.resolver.working_directory = input_path.parent_path().u8string();
    config.resolver.search_paths = {config.resolver.working_directory};
    resolver_.SetConfig(config.resolver);
    config.composition.load_payloads =
        options.payload_mode == Options::PayloadMode::LoadAll;
    if (options.payload_mode == Options::PayloadMode::Budget) {
      auto budget = std::make_shared<PayloadBudget>();
      budget->bytes = MiB(options.payload_budget_mb);
      budget->extent_limit = options.payload_extent_budget;
      budget->resolver = &resolver_;
      config.composition.payload_policy_with_prim =
          [budget](const lightusd::Path& path, const std::string& asset,
                   const lightusd::PrimSpec& owner) {
            return budget->Admit(path, asset, owner);
          };
    }
    return Publish(session_.OpenFile(input_path.u8string(), config), warning,
                   error);
  }
  const lightusd::Stage& stage() const { return *snapshot_; }
  std::vector<lightusd::Path> deferred_payload_paths() const {
    return session_.GetDeferredPayloadPaths();
  }
  size_t deferred_count() const { return deferred_payload_paths().size(); }
  size_t layer_parse_count() const { return layer_count_; }
  size_t estimate_stage_memory_bytes() const {
    return snapshot_ ? snapshot_->GetMemoryUsage() : 0;
  }
  bool load_payload(const lightusd::Path& path, std::string* warning,
                    std::string* error) {
    return Publish(session_.LoadPayload(path), warning, error);
  }
  bool unload_payload(const lightusd::Path& path, std::string* warning,
                      std::string* error) {
    return Publish(session_.UnloadPayload(path), warning, error);
  }
  bool rebuild_stage(std::string* warning, std::string* error) {
    return Publish(session_.Rebuild(), warning, error);
  }

 private:
  bool Publish(lightusd::StageOperationResult result, std::string* warning,
               std::string* error) {
    if (warning) *warning = result.warning;
    if (error) *error = result.error;
    if (!result.success) return false;
    snapshot_ = std::move(result.snapshot);
    layer_count_ = session_.GetLayerDependencies().size();
    if (release_cache_) session_.ReleaseCompositionCache();
    return true;
  }
  lightusd::AssetResolver resolver_;
  lightusd::StageSession session_;
  lightusd::StageSnapshot snapshot_;
  bool release_cache_ = false;
  size_t layer_count_ = 0;
};

bool ParseCount(const std::string& text, size_t* value) {
  if (text.empty() || text[0] == '-' || text[0] == '+') return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long number = std::strtoull(text.c_str(), &end, 10);
  if (errno || end != text.c_str() + text.size() ||
      number > (std::numeric_limits<size_t>::max)())
    return false;
  *value = size_t(number);
  return true;
}

// Resident set size (KB) from /proc/self/status (Linux). 0 if unavailable.
uint64_t VmRSS_kb() {
  std::ifstream st("/proc/self/status");
  std::string line;
  while (std::getline(st, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      uint64_t kb = 0;
      std::sscanf(line.c_str(), "VmRSS: %" SCNu64 " kB", &kb);
      return kb;
    }
  }
  return 0;
}

void print_mem(const char* tag) {
  const uint64_t kb = VmRSS_kb();
  std::printf("  [%s] process RSS = %.1f MiB\n", tag, double(kb) / 1024.0);
}

size_t count_prims(const lightusd::Stage& stage) {
  size_t n = 0;
  stage.Traverse([&](const lightusd::UsdPrim&) {
    ++n;
    return true;
  });
  return n;
}

size_t root_prim_count(const lightusd::Stage& stage) {
  return stage.GetRootPrims().size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::strcmp(argv[1], "--help") == 0 ||
      std::strcmp(argv[1], "-h") == 0) {
    std::printf(
        "Usage: %s <scene.usd[a]> [--mode=none|all|budget] [--budget-mb=N] "
        "[--extent-budget=N] [--no-mmap-zero-copy] [--no-dedup] "
        "[--load-some=N] [--unload]\n",
        argv[0]);
    std::printf(
        "--no-dedup releases parsed-layer/composition caches between edits; "
        "each composition still shares source layers.\n");
    return argc < 2 ? 1 : 0;
  }

  const std::string filename = argv[1];
  Options opts;
  opts.payload_mode = Options::PayloadMode::LoadNone;
  int load_some = 0;
  bool do_unload = false;

  for (int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--mode=none") {
      opts.payload_mode = Options::PayloadMode::LoadNone;
    } else if (a == "--mode=all") {
      opts.payload_mode = Options::PayloadMode::LoadAll;
    } else if (a == "--mode=budget") {
      opts.payload_mode = Options::PayloadMode::Budget;
    } else if (a.rfind("--budget-mb=", 0) == 0) {
      if (!ParseCount(a.substr(strlen("--budget-mb=")),
                      &opts.payload_budget_mb)) {
        std::fprintf(stderr, "Invalid --budget-mb\n");
        return 1;
      }
    } else if (a.rfind("--extent-budget=", 0) == 0) {
      const char* text = a.c_str() + strlen("--extent-budget=");
      char* end = nullptr;
      errno = 0;
      opts.payload_extent_budget = std::strtod(text, &end);
      if (end == text || *end != '\0' || errno == ERANGE ||
          !std::isfinite(opts.payload_extent_budget) ||
          opts.payload_extent_budget < 0.0) {
        std::printf("--extent-budget must be a finite non-negative number\n");
        return 1;
      }
    } else if (a == "--no-dedup") {
      opts.dedup_layers = false;
    } else if (a == "--no-mmap-zero-copy") {
      opts.mmap_zero_copy = false;
    } else if (a.rfind("--load-some=", 0) == 0) {
      size_t value = 0;
      if (!ParseCount(a.substr(strlen("--load-some=")), &value) ||
          value > size_t((std::numeric_limits<int>::max)())) {
        std::fprintf(stderr, "Invalid --load-some\n");
        return 1;
      }
      load_some = int(value);
    } else if (a == "--unload") {
      do_unload = true;
    } else {
      std::printf("Unknown arg: %s\n", a.c_str());
      return 1;
    }
  }

  std::printf("Loading: %s\n", filename.c_str());
  std::printf(
      "  payload_mode = %d, retain_caches = %d, extent_budget = %.3f, "
      "mmap_zero_copy = %d\n",
      int(opts.payload_mode), int(opts.dedup_layers),
      opts.payload_extent_budget, int(opts.mmap_zero_copy));
  print_mem("before");

  CoreLoader loader;
  std::string warn, err;
  if (!loader.Load(filename, opts, &warn, &err)) {
    std::printf("Load FAILED: %s\n", err.c_str());
    if (!warn.empty()) std::printf("warn: %s\n", warn.c_str());
    return 1;
  }
  if (!warn.empty()) std::printf("warn: %s\n", warn.c_str());

  const size_t total_prims = count_prims(loader.stage());
  const size_t n_roots = root_prim_count(loader.stage());
  std::printf("Loaded.\n");
  std::printf("  root prims        = %zu\n", n_roots);
  std::printf("  total prims       = %zu\n", total_prims);
  std::printf("  deferred payloads = %zu\n", loader.deferred_count());
  std::printf("  source layer count = %zu\n", loader.layer_parse_count());
  std::printf("  est. Stage memory = %.1f MiB\n",
              double(loader.estimate_stage_memory_bytes()) / (1024.0 * 1024.0));
  print_mem("after-load");

  if (load_some > 0) {
    const std::vector<lightusd::Path> deferred =
        loader.deferred_payload_paths();
    const int n = std::min<int>(load_some, int(deferred.size()));
    std::printf("Streaming in %d deferred payload(s)...\n", n);
    int ok = 0;
    for (int i = 0; i < n; i++) {
      std::string lwarn, lerr;
      if (loader.load_payload(deferred[size_t(i)], &lwarn, &lerr)) {
        ok++;
      } else {
        std::printf("  load_payload(%s) failed: %s\n",
                    deferred[size_t(i)].str().c_str(), lerr.c_str());
      }
    }
    if (ok != n) return 1;
    std::string rwarn, rerr;
    if (!loader.rebuild_stage(&rwarn, &rerr)) {
      std::printf("rebuild_stage failed: %s\n", rerr.c_str());
      return 1;
    }
    std::printf(
        "  loaded %d/%d, deferred now = %zu, est. Stage memory = %.1f MiB\n",
        ok, n, loader.deferred_count(),
        double(loader.estimate_stage_memory_bytes()) / (1024.0 * 1024.0));
    print_mem("after-stream");

    if (do_unload) {
      int un = 0;
      for (int i = 0; i < n; i++) {
        std::string uwarn, uerr;
        if (loader.unload_payload(deferred[size_t(i)], &uwarn, &uerr)) un++;
      }
      if (un != n) return 1;
      std::string rwarn2, rerr2;
      if (!loader.rebuild_stage(&rwarn2, &rerr2)) {
        std::fprintf(stderr, "rebuild_stage failed: %s\n", rerr2.c_str());
        return 1;
      }
      std::printf("  unloaded %d/%d, deferred now = %zu\n", un, n,
                  loader.deferred_count());
      print_mem("after-unload");
    }
  }

  std::printf("Done.\n");
  return 0;
}
