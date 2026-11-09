// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// cache.hh - Cached / lazy (partial-composition) USD composition engine.
//
// lightusd::pcp::Cache is the lightusd counterpart to OpenUSD's PcpCache. It
// reuses the composition_graph engine (PrimIndex / CompNode / PrimIndexBuilder)
// and adds the layer the eager CompositionGraph lacks:
//
//   * Lazy per-prim composition: ComputePrimIndex(path) builds and caches just
//     the requested prim's PrimIndex (partial composition).
//   * A parse-once layer-asset cache (LayerRegistry): a referenced/payload file
//     is parsed once and shared across every prim that uses it.
//   * Sublayer (L phase) composited once at Open() time.
//   * Dependency tracking + Invalidate(path): drop only the cached indices that
//     actually read an opinion at/under a changed prim.
//   * Incremental payload load/unload.
//   * Optional multithreaded batch building (PrewarmPrimIndices), single
//     threaded by default and the only path on wasm.
//
// This is the "Lazy + caches + invalidate" first cut: it does NOT implement a
// PcpChanges-style change processor that diffs arbitrary scene edits.
//
// State lives behind a heap-pinned Impl (pimpl), so a Cache is cheaply movable
// without disturbing the cached PrimIndices' internal pointers.
//
// Threading model
// ---------------
//   * A single Cache instance is NOT safe for concurrent use by multiple
//     threads. The methods that mutate cache state -- ComputePrimIndex,
//     Invalidate, InvalidateLayer, LoadPayload, UnloadPayload (and Open) --
//     must be called from one thread at a time, and never concurrently with
//     PrewarmPrimIndices()/BuildStage() on the same Cache.
//   * The ONLY parallelism is INTERNAL to PrewarmPrimIndices()/BuildStage():
//     when CacheOptions::num_threads != 1 (and threads are compiled in, never
//     on wasm) they build independent prim indices on worker threads, then
//     merge single-threaded. That internal fan-out is self-synchronized (each
//     worker builds into a private CompositionContext; the shared root layer is
//     read-only and the LayerRegistry is internally locked).
//   * Because that fan-out runs user callbacks on worker threads, any state
//     captured by CacheOptions::composition.payload_policy / the `fileformats`
//     handlers must be thread-safe; see the contract on CacheOptions below.
//   * Distinct Cache instances are independent and may be used from different
//     threads concurrently.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"

#include "asset-resolution.hh"
#include "composition-graph.hh"
#include "core/path.hh"
#include "layer.hh"
#include "pcp/layer-registry.hh"

namespace lightusd {

class Stage;  // forward decl; stage.hh is only included in cache.cc

namespace pcp {

namespace cg = lightusd::composition_graph;

// ---------------------------------------------------------------------------
// CacheOptions
// ---------------------------------------------------------------------------

struct CacheOptions {
  /// Options forwarded to the underlying composition engine (payload policy,
  /// max depth, file formats, ...).
  ///
  /// THREAD-SAFETY CONTRACT: when num_threads != 1, any user callbacks reachable
  /// through these options -- notably composition.payload_policy and the
  /// `fileformats` handlers -- are invoked concurrently from worker threads
  /// during PrewarmPrimIndices()/BuildStage(). Each worker uses its own copy of
  /// the std::function, but any state the callback *captures* (or any global it
  /// touches) is shared, so such callbacks MUST be thread-safe (e.g. guard
  /// captured mutable state, avoid non-reentrant globals). Callbacks that only
  /// read immutable captured data, or pure functions, are fine. With the default
  /// num_threads == 1 (and on wasm) callbacks are only ever called on the
  /// calling thread and need no synchronization.
  cg::CompositionGraphOptions composition;

  /// Worker thread count for PrewarmPrimIndices()/BuildStage():
  ///   1  = single-threaded (DEFAULT, and the only path on wasm)
  ///  -1  = std::thread::hardware_concurrency()
  ///  >1  = exactly that many workers
  /// Ignored (treated as 1) when threads are not compiled in.
  /// NOTE: values != 1 require thread-safe `composition` callbacks; see the
  /// thread-safety contract on `composition` above.
  int num_threads{1};

  /// Below this many requested paths, batch building stays single-threaded.
  size_t min_paths_for_parallel{8};
};

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

class Cache {
 public:
  Cache();
  ~Cache();

  // Move-only (cheap: moves a single Impl pointer).
  Cache(Cache &&) noexcept;
  Cache &operator=(Cache &&) noexcept;
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;

  /// Open a cache over `root_layer`: composites its sublayers (L phase) once
  /// and prepares for lazy per-prim composition. The resolver is borrowed and
  /// must outlive the cache.
  static nonstd::expected<Cache, std::string> Open(
      AssetResolutionResolver &resolver, const Layer &root_layer,
      const CacheOptions &options = {});

  /// Lazily build (if needed) and cache the PrimIndex for `prim_path`, then
  /// return a borrowed pointer (owned by the cache; valid until Invalidate or
  /// destruction). Returns nullptr if the prim does not exist. NOT thread-safe.
  const cg::PrimIndex *ComputePrimIndex(const Path &prim_path,
                                        std::string *warn, std::string *err);

  /// Build the PrimIndices for `paths` as a batch. Parallel iff num_threads !=
  /// 1 and threads are compiled in; otherwise sequential. Best-effort: a prim
  /// that fails to build is skipped.
  /// In the parallel path, user callbacks in CacheOptions::composition run on
  /// worker threads and must be thread-safe (see CacheOptions).
  nonstd::expected<bool, std::string> PrewarmPrimIndices(
      const std::vector<Path> &paths, std::string *warn, std::string *err);

  /// Materialize a full Stage: computes every prim's PrimIndex (honoring
  /// num_threads) and lowers them via the shared reconstruct pipeline. The
  /// result is structurally identical to CompositionGraph::BuildStage().
  /// With num_threads != 1, user callbacks in CacheOptions::composition run on
  /// worker threads and must be thread-safe (see CacheOptions).
  bool BuildStage(Stage *stage, std::string *warn, std::string *err);

  // -- Payloads --
  nonstd::expected<bool, std::string> LoadPayload(const Path &prim_path,
                                                  std::string *warn,
                                                  std::string *err);
  nonstd::expected<bool, std::string> UnloadPayload(const Path &prim_path);
  bool HasDeferredPayload(const Path &prim_path) const;
  std::vector<Path> GetDeferredPayloadPaths() const;

  // -- Invalidation --
  /// Drop the cached index for `prim_path`, its namespace descendants, and any
  /// cached index that read an opinion at/under `prim_path`.
  void Invalidate(const Path &prim_path);
  /// Drop every cached index that read from `resolved_layer_id`, then drop the
  /// layer from the registry.
  void InvalidateLayer(const std::string &resolved_layer_id);

  // -- Queries / diagnostics --
  bool HasComputedPrimIndex(const Path &prim_path) const;
  std::vector<Path> GetComputedPrimPaths() const;
  size_t ComputedPrimIndexCount() const;
  const LayerRegistry &layer_registry() const;
  const Layer *composited_root_layer() const;

  struct Impl;  // defined in pcp/cache-impl.hh

 private:
  std::unique_ptr<Impl> _impl;
};

}  // namespace pcp
}  // namespace lightusd
