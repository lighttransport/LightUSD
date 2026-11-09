// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.

#pragma once

#include <string>

namespace lightusd {

// forward decl
class RenderScene;

namespace tydra {


///
/// Dump RenderScene to string (for debugging)
///
/// Supported formats:
/// - "yaml" (default) - Human-readable YAML format with metadata header
/// - "json" - Machine-readable JSON format with metadata header
/// - "kdl"  - Original KDL format (https://kdl.dev/)
///
/// Both YAML and JSON formats include:
/// - Metadata section (format_version, generator, source_file, scene settings)
/// - Summary section (counts of nodes, meshes, materials, etc.)
/// - Full scene data (nodes, meshes, skeletons, animations, cameras, materials, textures, images, buffers)
///
std::string DumpRenderScene(const RenderScene &scene,
                            const std::string &format = "yaml");

}  // namespace tydra
}  // namespace lightusd
