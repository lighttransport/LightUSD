// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file json-export.hh
/// @brief Export Tydra render data to JSON format
///
/// Provides JSON export functionality for LightUSD render scenes, with a
/// schema designed for web applications and Three.js compatibility. Supports
/// both embedded and binary asset formats similar to glTF.
///
/// Features:
/// - Three.js-compatible JSON scene format
/// - Embedded or binary asset storage (like glTF/GLB)
/// - Complete scene export (geometry, materials, textures, transforms)
/// - Optimized for web delivery and streaming
///
/// Output formats:
/// - JSON with embedded BASE64 assets (smaller scenes)
/// - JSON + binary buffer (recommended for large scenes with textures)
///
/// Reference: https://github.com/mrdoob/three.js/wiki/JSON-Object-Scene-format-4
///
#pragma once

#include "render-data.hh"

namespace lightusd {
namespace tydra {

///
/// Export RenderScene to JSON.
/// JSON schema is LightUSD specific, but Three.js' JSON Object scene format in mind.
///
/// https://github.com/mrdoob/three.js/wiki/JSON-Object-Scene-format-4
///
/// When `asset_as_binary` is set to true, Export asset data(Mesh vertex attributes, Texture images, etc) as separated binary data(like glTF's GLB format).
/// Otherwise all asserts are embedded to JSON(Use BASE64 string for texture image data)
///
/// If you want to export a USD scene with lots of geometry(e.g. 1M+ tris) + textures, binary output is recommended.
///
/// @param[in] scene RenderScene
/// @param[in] asset_as_binary Export asset data(Mesh, Texture, etc) as binary data?
/// @param[out] json_str exported JSON string
/// @param[out] binary_str Binary data string
/// @param[out] warn warning message
/// @param[out] err error message
///
/// @return true upon success.
///
bool export_to_json(const RenderScene &scene, bool asset_as_binary,
                   std::string &json_str, std::string &binary_str,
                   std::string *warn, std::string *err);

}  // namespace tydra
}  // namespace lightusd
