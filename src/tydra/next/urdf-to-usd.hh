// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
//
/// @file urdf-to-usd.hh
/// @brief Build USD Physics + MuJoCo stages from URDF-derived JSON
///        (next-core port of src/tydra/urdf-to-usd.hh).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lightusd {
namespace minijson { class Value; }
namespace next {
class Stage;
}  // namespace next

namespace tydra {
namespace next {

/// Pre-registered mesh geometry referenced from the robot JSON via `meshRef`.
/// Same field layout as the legacy tydra::URDFMeshBuffer.
struct URDFMeshBuffer {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<int32_t> indices;
};

/// Build a USD stage (next-core) from a compact URDF/MJCF export payload.
///
/// The JSON payload is renderer-neutral (same contract as the legacy
/// converter):
/// - links[].visuals/collisions[].geometry contain positions, indices,
///   normals, and UVs (or a `meshRef` into `mesh_buffers`).
/// - links[].collisions[].shape may contain native USD collision shapes
///   (`box`, `sphere`, `cylinder`, `capsule`, or `plane`).
/// - joints[] contain URDF joint metadata, axes, limits, dynamics, and link
///   names.
///
/// The generated stage authors standard UsdPhysics, MuJoCo (mjc:*), and
/// Newton (newton:*) API schema properties, matching the prim paths, types,
/// applied API schemas, attribute names/types, and relationships produced by
/// the legacy converter.
// Native frontends already own validated bounded JSON; avoid text serialization
// and a second full parse (large mesh payloads can exceed JSON text caps).
bool ConvertURDFPayloadToUSDStage(const ::lightusd::minijson::Value& payload,
    ::lightusd::next::Stage* stage, std::string* warn = nullptr, std::string* err = nullptr);
/// Same, with the payload's `meshRef` geometry in `mesh_buffers`.
bool ConvertURDFPayloadToUSDStage(const ::lightusd::minijson::Value& payload,
    const std::map<std::string, URDFMeshBuffer>* mesh_buffers,
    ::lightusd::next::Stage* stage, std::string* warn = nullptr, std::string* err = nullptr);

/// Same, but the buffers are moved into the stage as each mesh is authored
/// (lower peak memory); `mesh_buffers` is left with emptied buffers. Use when
/// the payload is converted once.
bool ConvertURDFPayloadToUSDStageConsuming(const ::lightusd::minijson::Value& payload,
    std::map<std::string, URDFMeshBuffer>* mesh_buffers,
    ::lightusd::next::Stage* out_stage, std::string* warn, std::string* err);

bool ConvertURDFJsonToUSDStage(
    const std::string &robot_json,
    ::lightusd::next::Stage *stage,
    std::string *warn = nullptr,
    std::string *err = nullptr);

bool ConvertURDFJsonToUSDStage(
    const std::string &robot_json,
    const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
    ::lightusd::next::Stage *stage,
    std::string *warn = nullptr,
    std::string *err = nullptr);

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
