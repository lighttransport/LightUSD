// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file physics-to-json.hh
/// @brief Export USD Physics and MuJoCo annotations to JSON
///
#pragma once

#include <string>

namespace lightusd {

class Stage;

namespace tydra {

struct PhysicsJsonExportOptions {
  bool include_mjc{true};        // Include MuJoCo (mjcPhysics) annotations
  bool include_defaults{false};  // Include attributes with default values
  int indent{2};                 // JSON indentation
};

///
/// Export all physics annotations from a Stage to JSON.
///
/// Traverses the stage to find PhysicsScene, PhysicsJoint, MjcActuator,
/// MjcTendon, MjcSensor, and MjcKeyframe prims, then serializes their attributes
/// to a JSON string.
///
/// @param[in] stage The USD stage to export from
/// @param[out] json_str Output JSON string
/// @param[out] err Error message (optional)
/// @param[in] options Export options
/// @return true on success
///
bool ConvertPhysicsToJson(
    const Stage &stage,
    std::string *json_str,
    std::string *err = nullptr,
    const PhysicsJsonExportOptions &options = {});

}  // namespace tydra
}  // namespace lightusd
