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
//     --no-mmap-zero-copy      disable next-PCP mmap/lazy-array composition
//     --no-dedup               disable parse-once layer registry
//     --load-some=N            after load, stream in the first N deferred payloads
//     --unload                 after --load-some, unload them again
//
// Reports process RSS (the trustworthy peak gauge), deferred payload count,
// estimated Stage memory, and unique-file parse count.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "large-scene-loader.hh"
#include "stage.hh"

namespace {

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

void print_mem(const char *tag) {
  const uint64_t kb = VmRSS_kb();
  std::printf("  [%s] process RSS = %.1f MiB\n", tag, double(kb) / 1024.0);
}

#if defined(LIGHTUSD_LEGACY_USE_CORE_PCP)
size_t count_prims(const lightusd::Stage &stage) {
  size_t n = 0;
  stage.Traverse([&](const lightusd::UsdPrim &) {
    ++n;
    return true;
  });
  return n;
}

size_t root_prim_count(const lightusd::Stage &stage) {
  return stage.GetRootPrims().size();
}
#else
size_t count_prim_tree(const lightusd::legacy::Prim &p) {
  size_t n = 1;
  for (const auto &c : p.children()) n += count_prim_tree(c);
  return n;
}

size_t count_prims(const lightusd::legacy::Stage &stage) {
  size_t n = 0;
  for (const auto &rp : stage.root_prims()) n += count_prim_tree(rp);
  return n;
}

size_t root_prim_count(const lightusd::legacy::Stage &stage) {
  return stage.root_prims().size();
}
#endif

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf(
        "Usage: %s <scene.usd[a]> [--mode=none|all|budget] [--budget-mb=N] "
        "[--extent-budget=N] [--no-mmap-zero-copy] [--no-dedup] "
        "[--load-some=N] [--unload]\n",
        argv[0]);
    return 1;
  }

  const std::string filename = argv[1];
  lightusd::legacy::LargeSceneLoadOptions opts;
  opts.payload_mode = lightusd::legacy::LargeSceneLoadOptions::PayloadMode::LoadNone;
  int load_some = 0;
  bool do_unload = false;

  for (int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--mode=none") {
      opts.payload_mode = lightusd::legacy::LargeSceneLoadOptions::PayloadMode::LoadNone;
    } else if (a == "--mode=all") {
      opts.payload_mode = lightusd::legacy::LargeSceneLoadOptions::PayloadMode::LoadAll;
    } else if (a == "--mode=budget") {
      opts.payload_mode = lightusd::legacy::LargeSceneLoadOptions::PayloadMode::Budget;
    } else if (a.rfind("--budget-mb=", 0) == 0) {
      opts.payload_budget_mb = std::stoul(a.substr(strlen("--budget-mb=")));
    } else if (a.rfind("--extent-budget=", 0) == 0) {
      const char *text = a.c_str() + strlen("--extent-budget=");
      char *end = nullptr;
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
      load_some = std::stoi(a.substr(strlen("--load-some=")));
    } else if (a == "--unload") {
      do_unload = true;
    } else {
      std::printf("Unknown arg: %s\n", a.c_str());
      return 1;
    }
  }

  std::printf("Loading: %s\n", filename.c_str());
  std::printf("  payload_mode = %d, dedup_layers = %d, extent_budget = %.3f, "
              "mmap_zero_copy = %d\n",
              int(opts.payload_mode), int(opts.dedup_layers),
              opts.payload_extent_budget, int(opts.mmap_zero_copy));
  print_mem("before");

  lightusd::legacy::LargeSceneLoader loader;
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
  std::printf("  unique files parsed = %zu\n", loader.layer_parse_count());
  std::printf("  est. Stage memory = %.1f MiB\n",
              double(loader.estimate_stage_memory_bytes()) / (1024.0 * 1024.0));
  print_mem("after-load");

  if (load_some > 0) {
    const std::vector<lightusd::legacy::Path> deferred = loader.deferred_payload_paths();
    const int n = std::min<int>(load_some, int(deferred.size()));
    std::printf("Streaming in %d deferred payload(s)...\n", n);
    int ok = 0;
    for (int i = 0; i < n; i++) {
      std::string lwarn, lerr;
      if (loader.load_payload(deferred[size_t(i)], &lwarn, &lerr)) {
        ok++;
      } else {
        std::printf("  load_payload(%s) failed: %s\n",
                    deferred[size_t(i)].prim_part().c_str(), lerr.c_str());
      }
    }
    std::string rwarn, rerr;
    if (!loader.rebuild_stage(&rwarn, &rerr)) {
      std::printf("rebuild_stage failed: %s\n", rerr.c_str());
    }
    std::printf("  loaded %d/%d, deferred now = %zu, est. Stage memory = %.1f MiB\n",
                ok, n, loader.deferred_count(),
                double(loader.estimate_stage_memory_bytes()) / (1024.0 * 1024.0));
    print_mem("after-stream");

    if (do_unload) {
      int un = 0;
      for (int i = 0; i < n; i++) {
        std::string uwarn, uerr;
        if (loader.unload_payload(deferred[size_t(i)], &uwarn, &uerr)) un++;
      }
      std::string rwarn2, rerr2;
      loader.rebuild_stage(&rwarn2, &rerr2);
      std::printf("  unloaded %d/%d, deferred now = %zu\n", un, n,
                  loader.deferred_count());
      print_mem("after-unload");
    }
  }

  std::printf("Done.\n");
  return 0;
}
