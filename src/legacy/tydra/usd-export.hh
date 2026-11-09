// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file usd-export.hh
/// @brief Export Tydra render data back to USD format
///
/// Provides "round-trip" functionality for exporting LightUSD render scenes
/// back to USD format. This is primarily useful for debugging and validating
/// that RenderScene data structures are correctly constructed from USD input.
///
/// Current support (USDA ASCII only):
/// - ✅ Polygon meshes with vertices, normals, UVs
/// - ✅ Materials and texture references (filenames only)
/// - ❌ Subdivision surfaces
/// - ❌ Skeletal animation and skinning
/// - ❌ Blend shapes
/// - ❌ Time-sampled animations
/// - ❌ Hair/curves
/// - ❌ USDC binary output
///
/// This exporter helps verify the fidelity of USD → RenderScene → USD
/// conversion pipelines and can be used for debugging complex scene issues.
///
/// Usage:
/// ```cpp
/// std::string usda_content;
/// std::string warn, err;
/// bool success = lightusd::tydra::export_to_usda(
///   render_scene, usda_content, &warn, &err);
/// ```
///
#pragma once

#include "render-data.hh"

namespace lightusd {
namespace tydra {

///
/// Export RenderScene to USDA(USD ASCII).
///
/// @param[in] scene RenderScene
/// @param[out] usda_str USDA string
/// @param[out] warn warning message
/// @param[out] err error message
///
/// @return true upon success.
///
bool export_to_usda(const RenderScene &scene,
                   std::string &usda_str,
                   std::string *warn, std::string *err);

}  // namespace tydra
}  // namespace lightusd
