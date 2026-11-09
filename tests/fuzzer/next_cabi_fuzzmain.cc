// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: the next C ABI (src/c-api/lightusd-c.h).
//
// The installed C boundary is what the JavaScript/WASM, Python and engine
// bindings call, but every existing next fuzzer drives the C++ internals. This
// harness fuzzes the FFI surface itself: an arbitrary byte buffer is loaded as
// a stage (format auto-detected), then the prim/property/value accessors are
// walked, including mistargeted calls (empty/oversized paths, NULL out-params)
// that must return errors rather than crash.
//
// Build (clang): cmake -S src/next -B build-fuzz \
//   -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON
// Seed from tests/usda/*.usda and tests/usdc/*.usdc.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "c-api/lightusd-c.h"

namespace {

// Copy a borrowed view into a NUL-terminated buffer for the API calls that
// take `const char*`. Returns nullptr when it does not fit.
const char *ToCString(lightusd_sv sv, char *buf, size_t cap) {
  if (sv.data == nullptr || sv.len + 1 > cap) return nullptr;
  std::memcpy(buf, sv.data, sv.len);
  buf[sv.len] = '\0';
  return buf;
}

void WalkPrim(lightusd_prim p, int depth) {
  if (depth > 8 || !lightusd_prim_is_valid(p)) return;
  (void)lightusd_prim_name(p);
  (void)lightusd_prim_type_name(p);
  (void)lightusd_prim_path(p);
  (void)lightusd_prim_is_active(p);
  (void)lightusd_prim_specifier(p);
  (void)lightusd_prim_has_payload(p);
  (void)lightusd_prim_parent(p);

  const size_t props = lightusd_prim_property_count(p);
  for (size_t i = 0; i < props && i < 64; ++i) {
    (void)lightusd_prim_property_flags_at(p, i);
    char buf[256];
    const char *name = ToCString(lightusd_prim_property_name(p, i), buf, sizeof(buf));
    if (name == nullptr) continue;
    (void)lightusd_prim_has_property(p, name);

    lightusd_value_view view;
    lightusd_sv text;
    if (lightusd_attr_inspect_default(p, name, &view, &text) != LIGHTUSD_OK) {
      (void)text;
    }

    lightusd_value *owned = nullptr;
    if (lightusd_attr_copy_default(p, name, &owned) == LIGHTUSD_OK && owned != nullptr) {
      lightusd_sv s;
      (void)lightusd_value_get_view(owned, &view);
      (void)lightusd_value_get_string(owned, &s);
      lightusd_value_destroy(owned);
    }
    int would_materialize = 0;
    (void)lightusd_attr_would_materialize_array(p, name, &would_materialize);
  }

  lightusd_strlist *names = nullptr;
  if (lightusd_prim_property_names(p, &names) == LIGHTUSD_OK && names != nullptr) {
    lightusd_strlist_destroy(names);
  }

  const size_t kids = lightusd_prim_child_count(p);
  for (size_t i = 0; i < kids && i < 64; ++i) {
    WalkPrim(lightusd_prim_child(p, i), depth + 1);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  opts.max_input_bytes = 8ull << 20;
  opts.max_asset_bytes = 8ull << 20;
  opts.max_resident_bytes = 256ull << 20;
  opts.max_array_elements = 1u << 20;
  opts.max_archive_entries = 1u << 16;
  opts.max_parse_depth = 128;
  opts.max_composition_depth = 32;
  opts.max_namespace_depth = 64;

  lightusd_stage *stage = nullptr;
  if (lightusd_stage_load_from_memory(data, size, &opts, &stage) == LIGHTUSD_OK &&
      stage != nullptr) {
    const size_t roots = lightusd_stage_root_prim_count(stage);
    for (size_t i = 0; i < roots && i < 32; ++i) {
      WalkPrim(lightusd_stage_root_prim(stage, i), 0);
    }
    (void)lightusd_stage_pseudo_root(stage);
    (void)lightusd_stage_default_prim(stage);
    (void)lightusd_stage_prim_count(stage);
    (void)lightusd_stage_prim_at_path(stage, "");
    (void)lightusd_stage_prim_at_path(stage, "not/a/real/path//");
    (void)lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage, "/"));
    lightusd_stage_destroy(stage);
  }

  // Hostile shapes at the boundary must return errors, not crash.
  (void)lightusd_stage_load_from_memory(nullptr, 0, &opts, nullptr);
  (void)lightusd_stage_load_from_memory(data, size, &opts, nullptr);
  return 0;
}
