// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include "stage/stage.hh"
#include "types/interpolation.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {

struct ValueClipSet {
  std::string name;
  std::vector<std::string> asset_paths;
  std::vector<std::pair<double, double>> times;
  std::vector<std::pair<double, int>> active;
  std::string prim_path;
  std::string manifest_asset_path;
  bool interpolate_missing = false;
};

using ValueClipStageLoader = std::function<bool(
    const std::string& asset_path, Stage* stage, std::string* warn,
    std::string* err)>;

/// Optional caller-owned cache shared by multiple value-clip queries. Cache
/// lifetime defines freshness: clear or replace it when the loader's backing
/// asset state changes.
///
/// A stage-lifetime cache is shared by concurrent queries, so the entry map is
/// internally synchronized. By default a cached entry is trusted for the cache
/// lifetime: the caller owns freshness and clears or replaces the cache when
/// its backing assets change. Set `revalidate_file_stamps` to instead record
/// each asset's file stamp and re-check it on every hit, so a rewritten clip is
/// observed by the next query (the cache then elides re-parsing, not
/// freshness). Entries without a stamp are always trusted as-is; pre-seeded
/// caches (tydra's bake) use that to hand over stages they loaded themselves.
///
/// Retention is LRU-bounded by `max_entries` (default 16). A clip asset is
/// large (ALab's per-frame payloads are 11 MB each), so a frame sweep must not
/// keep every stage it touches: the point of the cache is to collapse a burst
/// of queries at one time, not to retain the whole clip timeline. Eviction is
/// safe for concurrent queries because hits return `shared_ptr`; a stage stays
/// alive until its query finishes.
struct ValueClipStageCache {
  struct Entry {
    std::shared_ptr<Stage> stage;
    std::string error;
    bool stamped = false;
    int64_t mtime_ticks = 0;
    uint64_t size_bytes = 0;
    uint64_t use_tick = 0;
  };
  mutable std::mutex mutex;
  std::unordered_map<std::string, Entry> entries;
  size_t max_recursion_depth = 32;
  size_t max_entries = 16;
  uint64_t clock = 0;
  /// Re-stat cached assets on every hit instead of trusting them. Required
  /// when the asset files may be rewritten while the cache is alive.
  bool revalidate_file_stamps = false;

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }
};

/// Provenance for the clip choice made by one value query. Stage times name
/// entries in `active`; clip_time is the time-function result sampled inside
/// the active clip. Missing-value interpolation records both neighboring
/// assets and their active stage times.
struct ValueClipResolutionInfo {
  int active_index = -1;
  std::string active_asset;
  double clip_time = 0.0;
  bool interpolated_missing = false;
  std::string lower_asset;
  std::string upper_asset;
  double lower_stage_time = 0.0;
  double upper_stage_time = 0.0;
};

namespace value_clip_detail {
/// Index (assetIndex value) of the clip active at `stage_time`.
int ActiveIndex(const ValueClipSet& set, double stage_time);
/// Clip time mapped from `stage_time` through `times` (identity when empty).
double ClipTime(const ValueClipSet& set, double stage_time);
}  // namespace value_clip_detail

bool ParseValueClipSets(const UsdPrim& prim, std::vector<ValueClipSet>* out,
                        std::string* error = nullptr);

/// Resolve one numeric-time query through the prim's composed clip metadata.
/// Ordinary timeSamples/default precedence is handled by AttributeEval before
/// this function is called.
bool ResolveValueClip(const UsdPrim& prim, const std::string& property,
                      double stage_time, const ValueClipStageLoader& loader,
                      Value* out, std::string* source_asset = nullptr,
                      std::string* error = nullptr,
                      std::string* source_clip_set = nullptr,
                      ValueClipStageCache* stage_cache = nullptr,
                      ValueClipResolutionInfo* resolution_info = nullptr,
                      TimeInterpolation interpolation = TimeInterpolation::Linear);

/// Same resolution over PRE-PARSED clip sets (from ParseValueClipSets):
/// callers issuing many queries against one prim (e.g. Tydra's animation
/// bake) parse the metadata once instead of per query. `prim` is still
/// needed for the fallback clip prim path and nested-clip recursion.
bool ResolveValueClipFromSets(const std::vector<ValueClipSet>& sets,
                              const UsdPrim& prim, const std::string& property,
                              double stage_time,
                              const ValueClipStageLoader& loader, Value* out,
                              std::string* source_asset = nullptr,
                              std::string* error = nullptr,
                              std::string* source_clip_set = nullptr,
                              ValueClipStageCache* stage_cache = nullptr,
                              ValueClipResolutionInfo* resolution_info = nullptr,
                      TimeInterpolation interpolation = TimeInterpolation::Linear);

}  // namespace next
}  // namespace lightusd
