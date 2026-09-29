// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Shared internals for the LightUSD C API implementation.

#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "lightusd-c.h"

#include "next/stage/stage.hh"
#include "next/types/value.hh"

// ============================================================
// Handle definitions (opaque in the public header)
// ============================================================

struct lightusd_string {
  std::string s;
};

struct lightusd_strlist {
  std::vector<std::string> items;
};

struct lightusd_value {
  lightusd::next::Value v;
};

struct lightusd_stage {
  std::atomic<size_t> references{1};
  lightusd::next::Stage stage;
  // Non-null only for immutable handles retained from document snapshots.
  std::shared_ptr<const lightusd::next::Stage> snapshot_stage;
  const lightusd::next::Stage& ReadStage() const {
    return snapshot_stage ? *snapshot_stage : stage;
  }

  std::string warnings;
  // Directory of the file this stage was loaded from (empty for in-memory /
  // created stages); used to resolve relative texture asset paths.
  std::string source_dir;
  // Original filename, when file-backed. Property explanations reload this
  // uncomposed layer so variant and arc provenance is not lost in a flattened
  // stage snapshot.
  std::string source_filename;
  // Bumped by structural mutations (define/remove prim) so bindings can
  // detect stale lightusd_prim handles.
  std::atomic<uint64_t> generation{0};
};

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
const lightusd::next::PrimSpec* SpecFromC(lightusd_prim p);
lightusd::next::UsdPrim FromC(lightusd_prim p);
lightusd_prim ToC(const lightusd_stage* owner,
                 const lightusd::next::UsdPrim& prim);

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
lightusd_status MakeView(const lightusd::next::Value& v, lightusd_value_view* out);

// Build a next::Value from raw (type, is_array, data, count) as documented on
// lightusd_attr_set. Returns an empty Value and sets the error on failure.
bool ValueFromRaw(lightusd_type type, uint8_t is_array, const void* data,
                  size_t count, lightusd::next::Value* out);

}  // namespace lightusd_internal
