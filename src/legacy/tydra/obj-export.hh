// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file obj-export.hh
/// @brief Export Tydra render data to Wavefront OBJ format
///
/// Provides utilities for exporting LightUSD render scene data to
/// industry-standard Wavefront OBJ/MTL format for use in 3D modeling
/// and rendering applications.
///
/// Features:
/// - Exports mesh geometry (vertices, normals, texture coordinates)
/// - Generates corresponding MTL material files
/// - Preserves material assignments and texture references
/// - Direct export from RenderScene data structures
///
/// Limitations:
/// - No up-axis conversion (exports coordinates as-is)
/// - Z-up USD scenes export as Z-up OBJ files
/// - Single mesh export (use multiple calls for multiple meshes)
///
/// Usage:
/// ```cpp
/// std::string obj_content, mtl_content;
/// std::string warn, err;
/// bool success = lightusd::tydra::export_to_obj(
///   render_scene, mesh_id, obj_content, mtl_content, &warn, &err);
/// ```
///
#pragma once

#include "render-data.hh"

namespace lightusd {
namespace tydra {

///
/// Export RenderMesh/RenderMaterial to .obj.
/// Requires RenderScene instance to export Material/Texture correctly.
///
/// NOTE: No consideration of up-Axis. 3D coordinate is exported as-as.
/// Thus, if your USD scene is Z-up, 3D coordinate in exported .obj is Z-up.
///
/// (fortunately, you can import .obj by specifying Z-up axis in Blender)
///
/// @param[in] scene RenderScene
/// @param[in] mesh_id Mesh id in RenderScene
/// @param[out] obj_str .obj string
/// @param[out] warn warning message
/// @param[out] err error message
///
/// @return true upon success.
///
bool export_to_obj(const RenderScene &scene, const int mesh_id,
                   std::string &obj_str, std::string &mtl_str,
                   std::string *warn, std::string *err);

}  // namespace tydra
}  // namespace lightusd
