// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: native OBJ / URDF / MJCF / COLLADA import.
//
// The first byte selects the input format and output profile; the rest is the
// document. OBJ, URDF and MJCF go through the installed C import API
// (lightusd_import_memory: parser, MJCF macro expansion - replicate, cable
// composites, flexcomp soft bodies - and the USD writer), COLLADA through the
// bounded DAE reader. Inputs are in memory with no anchor, so referenced files
// are not opened.
//
// Build (clang): cmake -S src/next -B build-fuzz -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON
// Seed from tests/fuzzer/next_import_seeds.

#include <cstddef>
#include <cstdint>
#include <string>

#include "c-api/lightusd-c.h"
#include "c-api/lightusd-import-c.h"
#include "tydra/asset-import/import.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1 || size > (1u << 20)) return 0;
  const uint8_t selector = data[0];
  ++data;
  --size;
  if ((selector & 3) == 3) {
    namespace ia = lightusd::tydra::import_asset;
    ia::Options options;
    options.max_memory = 64u << 20;
    ia::Reader reader(options);
    ia::OBJ obj;
    std::string err;
    (void)ia::ReadDAE(std::string_view(reinterpret_cast<const char *>(data), size),
                      "fuzz.dae", reader, &obj, &err);
    return 0;
  }
  static const uint32_t kFormats[3] = {LIGHTUSD_IMPORT_OBJ, LIGHTUSD_IMPORT_URDF,
                                       LIGHTUSD_IMPORT_MJCF};
  lightusd_import_options options;
  lightusd_import_options_init(&options);
  options.format = kFormats[selector & 3];
  options.max_memory_bytes = 64u << 20;
  options.settings_json = (selector & 4) ? "{\"profile\":\"legacy\"}" : "{\"profile\":\"newton\"}";
  lightusd_stage *stage = nullptr;
  if (lightusd_import_memory(data, size, "", &options, &stage) == LIGHTUSD_OK && stage) {
    lightusd_string *text = nullptr;
    if (lightusd_stage_export_usda(stage, &text) == LIGHTUSD_OK) lightusd_string_destroy(text);
    lightusd_stage_destroy(stage);
  }
  return 0;
}
