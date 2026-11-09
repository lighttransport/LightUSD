// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file ar-to-json.hh
/// @brief Export AR/Interactive and Media annotations to JSON
///
#pragma once

#include <string>

namespace lightusd {

class Stage;

namespace tydra {

struct ARJsonExportOptions {
  bool include_defaults{false};  // Include attributes with default values
  int indent{2};                 // JSON indentation
};

///
/// Export all AR/Interactive annotations from a Stage to JSON.
///
/// Traverses the stage to find Preliminary_* and SpatialAudio prims,
/// then serializes their attributes to a JSON string.
///
/// @param[in] stage The USD stage to export from
/// @param[out] json_str Output JSON string
/// @param[out] err Error message (optional)
/// @param[in] options Export options
/// @return true on success
///
bool ConvertARToJson(
    const Stage &stage,
    std::string *json_str,
    std::string *err = nullptr,
    const ARJsonExportOptions &options = {});

}  // namespace tydra
}  // namespace lightusd
