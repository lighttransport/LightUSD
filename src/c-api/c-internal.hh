// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Shared internals for the LightUSD C API implementation.

#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
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
  lightusd::next::Stage stage;
  std::string warnings;
  // Directory of the file this stage was loaded from (empty for in-memory /
  // created stages); used to resolve relative texture asset paths.
  std::string source_dir;
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

// lightusd_prim <-> next::UsdPrim (both are {spec, layer, index} triples).
inline lightusd::next::UsdPrim FromC(lightusd_prim p) {
  return lightusd::next::UsdPrim(
      static_cast<const lightusd::next::PrimSpec*>(p._spec),
      static_cast<const lightusd::next::Layer*>(p._layer), p._index);
}

inline lightusd_prim ToC(const lightusd::next::UsdPrim& up) {
  lightusd_prim p;
  p._spec = up.GetPrimSpec();
  p._layer = up.GetLayer();
  p._index = up.GetIndex();
  p._pad = 0;
  return p;
}

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
