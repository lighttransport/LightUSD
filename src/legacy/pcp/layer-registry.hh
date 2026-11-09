// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// layer-registry.hh - Parse-once, resolved-path-keyed Layer cache.
//
// Part of the lightusd::pcp cached composition engine. A referenced, sublayer,
// or payload asset is resolved and parsed exactly once and then shared across
// every prim that needs it -- the single biggest efficiency win the eager
// CompositionGraph lacks (it re-parses each file per use).
//
#pragma once

#include <memory>
#include <string>

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
#include <mutex>
#endif

#include "asset-resolution.hh"
#include "layer.hh"
#include "tiny-hashmap.hh"

namespace lightusd {
namespace pcp {

/// Caches parsed Layers keyed by their RESOLVED asset path, so the same file
/// referenced from many prims is parsed only once. Borrowed pointers returned
/// by GetOrLoad stay valid until Drop()/Clear() or registry destruction; the
/// registry must therefore outlive any PrimIndex that references its layers.
///
/// Thread-safe: GetOrLoad serializes resolve + open + parse (which also
/// serializes the shared resolver's working-path state) when threads are
/// enabled, so it may be called from worker threads during a parallel build.
class LayerRegistry {
 public:
  LayerRegistry();
  ~LayerRegistry() = default;

  // Move-only (owns parsed layers).
  LayerRegistry(LayerRegistry &&) = default;
  LayerRegistry &operator=(LayerRegistry &&) = default;
  LayerRegistry(const LayerRegistry &) = delete;
  LayerRegistry &operator=(const LayerRegistry &) = delete;

  /// Resolve `asset_path` (honoring `cwp` as the current working path), parse
  /// it once, and return a borrowed pointer to the cached Layer. Returns
  /// nullptr on failure. `asset_path` is expected to be already validated /
  /// normalized by the caller.
  const Layer *GetOrLoad(AssetResolutionResolver &resolver,
                         const std::string &asset_path, const std::string &cwp,
                         std::string *warn, std::string *err);

  /// Drop a single cached layer by its resolved path. Callers MUST first
  /// invalidate every PrimIndex that references it (borrowed pointers dangle).
  void Drop(const std::string &resolved_path);

  /// Drop all cached layers.
  void Clear();

  /// Number of cached (parsed) layers.
  size_t size() const;

  /// Total number of actual parses performed (cache misses). Used by tests to
  /// assert parse-once behavior.
  size_t parse_count() const {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
    std::lock_guard<std::mutex> lock(*_mu);
#endif
    return _parse_count;
  }

 private:
  HashMap<std::string, std::shared_ptr<Layer>> _by_resolved;
  size_t _parse_count{0};

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  // Heap-allocated so LayerRegistry (hence pcp::Cache) stays movable.
  std::unique_ptr<std::mutex> _mu;
#endif
};

}  // namespace pcp
}  // namespace lightusd
