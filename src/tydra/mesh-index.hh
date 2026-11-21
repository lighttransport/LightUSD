// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>

#include "render-data.hh"
namespace lightusd {
namespace tydra {
struct MeshIndexOptions {
  bool deduplicate = true;
  float epsilon = 1e-6f;
  size_t max_candidates_per_point = 16;
  size_t memory_budget_bytes = size_t(512) << 20;
};
// Make all vertex attributes share the position index. Deduplication is
// confined to a source point (so skin/morph identity cannot be accidentally
// welded). Triangulated inputs become canonical triangle topology; polygons
// remain polygons. Normal/UV seams, indexed primvars, skin and sparse morphs
// survive. Returns false without changing the mesh on invalid data or budget
// refusal.
bool BuildVertexIndices(RenderMesh* mesh, const MeshIndexOptions& options = {},
                        std::string* error = nullptr);
}  // namespace tydra
}  // namespace lightusd
