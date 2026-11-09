// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// cache-impl.hh - private implementation detail of pcp::Cache (pimpl).
//
// Shared between cache.cc and cache-parallel.cc. NOT a public header.
//
#pragma once

#include <set>
#include <string>
#include <vector>

#include "pcp/cache.hh"
#include "tiny-hashmap.hh"

namespace lightusd {
namespace pcp {

// An opinion site: (resolved layer identifier, prim path within that layer).
struct Site {
  std::string layer_id;
  std::string prim_path;
  bool operator==(const Site &rhs) const {
    return layer_id == rhs.layer_id && prim_path == rhs.prim_path;
  }
};

struct SiteHasher {
  size_t operator()(const Site &s) const {
    return std::hash<std::string>()(s.layer_id) ^
           (std::hash<std::string>()(s.prim_path) << 1);
  }
};

// One cached prim's composition graph plus the (per-index) tables it borrows.
// Stored via shared_ptr and never moved after creation, so the index's borrowed
// table pointers stay valid. Referenced/payload Layers are NOT owned here --
// they live in the shared LayerRegistry (parse-once).
struct CachedPrimIndex {
  cg::CompositionContext ctx;  // owns path/layer-stack/map tables
  cg::PrimIndex index;         // borrows pointers into ctx's tables
};

// All Cache state lives here, heap-pinned so the cached PrimIndices' internal
// pointers (and the load-layer thunk's userdata) remain stable across Cache
// moves.
struct Cache::Impl {
  CacheOptions opts;
  AssetResolutionResolver *resolver{nullptr};

  std::shared_ptr<Layer> composited_root;  // owns the L-phase result
  const Layer *root_layer{nullptr};        // borrowed (== composited_root)

  LayerRegistry layers;  // parse-once referenced/payload layers

  HashMap<std::string, std::shared_ptr<CachedPrimIndex>> index_cache;
  HashMap<Site, std::set<std::string>, SiteHasher> site_to_indices;
  HashMap<std::string, std::vector<Site>> index_to_sites;

  // -- operations (the real logic) --
  const cg::PrimIndex *ComputePrimIndex(const Path &prim_path,
                                        std::string *warn, std::string *err);
  nonstd::expected<std::shared_ptr<CachedPrimIndex>, std::string> BuildEntry(
      const Path &prim_path, std::string *warn, std::string *err);
  nonstd::expected<bool, std::string> PrewarmPrimIndices(
      const std::vector<Path> &paths, std::string *warn, std::string *err);
  nonstd::expected<bool, std::string> BuildParallel(
      const std::vector<Path> &paths, size_t num_threads, std::string *warn,
      std::string *err);
  bool BuildStage(Stage *stage, std::string *warn, std::string *err);

  nonstd::expected<bool, std::string> LoadPayload(const Path &prim_path,
                                                  std::string *warn,
                                                  std::string *err);
  nonstd::expected<bool, std::string> UnloadPayload(const Path &prim_path);
  bool HasDeferredPayload(const Path &prim_path) const;
  std::vector<Path> GetDeferredPayloadPaths() const;

  void Invalidate(const Path &prim_path);
  void InvalidateLayer(const std::string &resolved_layer_id);

  const PrimSpec *FindLocalPrimSpec(const Path &prim_path) const;
  void RegisterDependencies(const std::string &prim_path,
                            const CachedPrimIndex &entry);
  void UnregisterDependencies(const std::string &prim_path);
  void DropIndex(const std::string &prim_path);
  void GatherAllPrimPaths(std::vector<Path> *out) const;

  // CompositionContext::load_layer_fn thunk -> routes loads through `layers`.
  // userdata is an Impl*.
  static const Layer *LoadLayerThunk(void *userdata,
                                     const std::string &asset_path,
                                     const std::string &cwp, std::string *warn,
                                     std::string *err);
};

}  // namespace pcp
}  // namespace lightusd
