// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include "../stage/stage.hh"

#include <functional>
#include <memory>
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
/// asset state changes. Concurrent mutation requires external synchronization.
struct ValueClipStageCache {
  struct Entry {
    std::shared_ptr<Stage> stage;
    std::string error;
  };
  std::unordered_map<std::string, Entry> entries;
  size_t max_recursion_depth = 32;
  std::vector<std::string> resolution_stack;

  void Clear() {
    entries.clear();
    resolution_stack.clear();
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
                      ValueClipResolutionInfo* resolution_info = nullptr);

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
                              ValueClipResolutionInfo* resolution_info = nullptr);

}  // namespace next
}  // namespace lightusd
