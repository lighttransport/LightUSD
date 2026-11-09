// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment, Inc.
//
// Material serialization utilities for WASM/JS bindings

#pragma once

#include <string>
#include "nonstd/expected.hpp"
#include "render-data.hh"

namespace lightusd {
namespace tydra {

enum class SerializationFormat {
  JSON,
  XML
};

// Serialize a RenderMaterial to JSON or XML format
// Returns serialized string on success, error message on failure
// Pass renderScene to include texture asset identifiers in serialization
nonstd::expected<std::string, std::string> serializeMaterial(
    const RenderMaterial& material,
    SerializationFormat format,
    const RenderScene* renderScene = nullptr);

// Serialize a RenderLight to JSON format
// Returns serialized string on success, error message on failure
// Pass renderScene to include mesh references for geometry lights
nonstd::expected<std::string, std::string> serializeLight(
    const RenderLight& light,
    SerializationFormat format,
    const RenderScene* renderScene = nullptr);

} // namespace tydra
} // namespace lightusd
