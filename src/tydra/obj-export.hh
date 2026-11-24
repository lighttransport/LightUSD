// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>

#include "render-data.hh"

namespace lightusd {
namespace tydra {
// Export mesh-local coordinates (no up-axis conversion). The caller writes
// <mesh_id><sanitized mesh name>.obj/.mtl. Outputs change only on success.
// Triangulated meshes retain source-corner attributes and subset assignments.
bool export_to_obj(const RenderScene& scene, int mesh_id, std::string& obj,
                   std::string& mtl, std::string* warn, std::string* error,
                   size_t max_output_bytes = 512u * 1024u * 1024u);
std::string OBJMeshBasename(const RenderMesh& mesh, int mesh_id);
}  // namespace tydra
}  // namespace lightusd
