// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Shared internals for the LightUSD C API implementation.

#pragma once

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lightusd-c.h"
#include "stage/stage.hh"
#include "types/value.hh"

namespace lightusd {
class AssetResolver;
struct LoadUSDOptions;
struct ImportedAssets;
struct ValueClipStageCache;
}  // namespace lightusd

namespace lightusd_internal {
// Borrowed native resolver behind a public C handle (null-safe).
lightusd::AssetResolver* AssetResolverForLoad(
    struct lightusd_asset_resolver* resolver);
}  // namespace lightusd_internal

// ============================================================
// Handle definitions (opaque in the public header)
// ============================================================

struct lightusd_string {
  std::string s;
};

struct lightusd_strlist {
  std::vector<std::string> items;
};

struct lightusd_buffer {
  lu_value value;
  lu_buffer* copy;
  const void* external;
  void (*release_external)(const void*);
  lightusd_value_view view;
};

struct lightusd_value {
  lightusd::Value v;
};

struct lightusd_stage {
  std::atomic<size_t> references{1};
  lightusd::Stage stage;
  // Non-null only for immutable handles retained from document snapshots.
  std::shared_ptr<const lightusd::Stage> snapshot_stage;
  const lightusd::Stage& ReadStage() const {
    return snapshot_stage ? *snapshot_stage : stage;
  }

  std::string warnings;
  std::string import_report = "{}";  // Runtime telemetry; not USD metadata.
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> import_assets;
  std::map<std::string, std::string> import_asset_paths;
  // Importer-generated files (MJCF builtin textures) keyed by the relative
  // path the stage authors; USDA/USDC saves write them beside the layer.
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> generated_assets;
  // Directory of the file this stage was loaded from (empty for in-memory /
  // created stages); used to resolve relative texture asset paths.
  std::string source_dir;
  // Original filename, when file-backed. Property explanations reload this
  // uncomposed layer so variant and arc provenance is not lost in a flattened
  // stage snapshot.
  std::string source_filename;
  // File-backed clip evaluation must retain the caller's finite load limits.
  // Custom resolvers remain borrowed only during the original load call.
  std::shared_ptr<const lightusd::LoadUSDOptions> file_eval_options;
  // Value-clip stages are parsed on demand. Keep one stage-lifetime cache so
  // repeated time queries reuse them instead of reloading per attribute.
  mutable std::mutex clip_cache_mutex;
  mutable std::shared_ptr<lightusd::ValueClipStageCache> clip_stage_cache;
  // Bumped by structural mutations (define/remove prim) so bindings can
  // detect stale lightusd_prim handles.
  std::atomic<uint64_t> generation{0};
  // Instance proxies share physical specs but retain logical path/parent and
  // relationship remapping. Intern only queried proxies; _pad is a 1-based ID.
  // std::map nodes keep borrowed path views stable until structural mutation.
  mutable std::mutex proxy_mutex;
  mutable uint64_t proxy_generation = 0;
  mutable std::map<std::string, lightusd::UsdPrim> proxy_prims;
  mutable std::vector<const lightusd::UsdPrim*> proxy_ids;
  mutable std::map<std::string, uint32_t> proxy_path_ids;

};

namespace lightusd_internal {
void RetainImportedAssets(lightusd_stage* stage,
                         const lightusd::ImportedAssets& assets);
}

namespace lightusd_internal {

// Thread-local error message backing lightusd_last_error().
void SetError(const std::string& msg);
void SetError(const char* msg);

// Convenience: set the error and return the status in one expression.
inline lightusd_status Fail(lightusd_status st, const std::string& msg) {
  SetError(msg);
  return st;
}

// Compiled handle validation keeps storage details out of binding headers.
const lightusd::PrimSpec* SpecFromC(lightusd_prim p);
lightusd::UsdPrim FromC(lightusd_prim p);
lightusd_prim ToC(const lightusd_stage* owner,
                 const lightusd::UsdPrim& prim);

inline lightusd_sv SV(const std::string& s) {
  lightusd_sv v;
  v.data = s.c_str();
  v.len = s.size();
  return v;
}

inline lightusd_sv SV(std::string_view s) {
  lightusd_sv v;
  v.data = s.data();
  v.len = s.size();
  return v;
}

inline lightusd_sv EmptySV() {
  lightusd_sv v;
  v.data = "";
  v.len = 0;
  return v;
}

// Build a borrowed zero-copy view from a Value. Materializes lazy arrays
// (serialized by an internal mutex). String-family / dictionary / token-array
// values yield data == NULL with storage == LIGHTUSD_COMP_NONE.
lightusd_status MakeView(lightusd::ValueView v, lightusd_value_view* out);

// Build a ::lightusd::Value from raw (type, is_array, data, count) as documented on
// lightusd_attr_set. Returns an empty Value and sets the error on failure.
bool ValueFromRaw(lightusd_type type, uint8_t is_array, const void* data,
                  size_t count, lightusd::Value* out);

}  // namespace lightusd_internal
