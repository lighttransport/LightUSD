// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// cache-parallel.cc - optional multithreaded batch building for pcp::Cache.
//
// The entire worker machinery is gated on LIGHTUSD_ENABLE_THREAD and compiled
// out on wasm. When threads are disabled this translation unit is essentially
// empty and Cache::Impl::BuildParallel is never referenced (PrewarmPrimIndices
// only calls it inside the same #if), so there is no missing-symbol concern.
//
// Design (mirrors the per-index "no shared mutable state in the parallel
// region" approach of src/prim-pprint-parallel.cc): each worker builds an
// independent prim index into its own pre-sized output slot via
// Cache::Impl::BuildEntry (which touches only the internally-locked
// LayerRegistry and read-only inputs). After the join barrier, a
// single-threaded deterministic merge folds the finished entries into the cache
// in input order. This guarantees single-thread and multi-thread runs produce
// identical caches.
//
#include "pcp/cache.hh"

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "pcp/cache-impl.hh"

namespace lightusd {
namespace pcp {

nonstd::expected<bool, std::string> Cache::Impl::BuildParallel(
    const std::vector<Path> &paths, size_t num_threads, std::string *warn,
    std::string *err) {
  (void)err;  // best-effort: per-path failures are surfaced via `warn`, not err

  struct Slot {
    Path path;
    std::shared_ptr<CachedPrimIndex> entry;  // null if skipped/failed
    std::string err;                         // build error when entry is null
  };

  std::vector<Slot> slots(paths.size());
  for (size_t i = 0; i < paths.size(); i++) {
    slots[i].path = paths[i];
  }

  const size_t count = slots.size();
  if (count == 0) {
    return true;  // nothing to build
  }

  // Pull work items by atomically incrementing a shared index. Each worker
  // writes only to its own slot -> no shared mutable state in this region
  // (the LayerRegistry, reached via BuildEntry, locks internally). BuildEntry
  // reports failures via nonstd::expected (no C++ exceptions), so a failed
  // build is recorded in the slot and skipped, never aborting the batch.
  std::atomic<size_t> next_idx{0};

  auto worker = [&]() {
    for (;;) {
      size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
      if (i >= count) break;
      std::string w, e;
      auto built = BuildEntry(slots[i].path, &w, &e);
      if (built) {
        slots[i].entry = built.value();
      } else {
        slots[i].err = built.error();  // record; keep draining the queue
      }
    }
  };

  // Never spawn more threads than work items, and let the calling thread run as
  // one worker (spawn count-1). This bounds std::thread creation -- which can
  // itself fail under resource pressure -- and keeps progress when fewer workers
  // than requested are available. All threads drain the same atomic queue, so
  // the result is independent of how many actually run.
  if (num_threads < 1) num_threads = 1;
  if (num_threads > count) num_threads = count;

  std::vector<std::thread> workers;
  workers.reserve(num_threads - 1);
  for (size_t t = 0; t + 1 < num_threads; t++) {
    workers.emplace_back(worker);
  }
  worker();  // calling thread participates
  for (auto &t : workers) {
    t.join();
  }

  // Deterministic merge (input order): identical to the sequential path.
  size_t failed = 0;
  std::string fail_detail;
  for (auto &s : slots) {
    if (!s.entry) {
      // Null entry == BuildEntry failed for this path (best-effort skip).
      failed++;
      if (fail_detail.size() < 4000) {  // bound the aggregated message
        fail_detail += "  " + s.path.prim_part() + ": " + s.err + "\n";
      }
      continue;
    }
    const std::string key = s.path.prim_part();
    if (index_cache.find(key) != index_cache.end()) {
      continue;  // already cached (keep the existing entry)
    }
    index_cache[key] = s.entry;
    RegisterDependencies(key, *s.entry);
  }

  // Surface best-effort failures as a warning rather than dropping them
  // silently. Non-fatal by design, so the batch still succeeds.
  if (failed && warn) {
    *warn += "pcp::Cache: " + std::to_string(failed) + " of " +
             std::to_string(count) +
             " prim indices failed to build during parallel prewarm:\n" +
             fail_detail;
  }

  return true;
}

}  // namespace pcp
}  // namespace lightusd

#endif  // LIGHTUSD_ENABLE_THREAD && !__EMSCRIPTEN__
