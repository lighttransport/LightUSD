// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Mesh topology sanitization

#include "render-converter.hh"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

void RenderSceneConverter::SanitizeMeshTopology(RenderMesh* mesh) {
  const uint32_t point_count = static_cast<uint32_t>(mesh->point_count());
  const size_t index_count = mesh->face_vertex_indices.size();

  // Fast path: everything consistent.
  bool ok = true;
  size_t need = 0;
  for (uint32_t c : mesh->face_vertex_counts) {
    need += c;
    if (need > index_count) { ok = false; break; }
  }
  if (ok && need <= index_count) {
    for (uint32_t idx : mesh->face_vertex_indices) {
      if (idx >= point_count) { ok = false; break; }
    }
    if (ok && need == index_count) return;
  }

  std::vector<uint32_t> counts;
  std::vector<uint32_t> indices;
  // Authored face index -> post-sanitize face index (-1 = dropped), so
  // consumers of authored face numbering (holeIndices, GeomSubset indices)
  // can be remapped instead of discarded.
  std::vector<int32_t> face_remap(mesh->face_vertex_counts.size(), -1);
  counts.reserve(mesh->face_vertex_counts.size());
  indices.reserve(index_count);
  size_t offset = 0;
  size_t dropped = 0;
  size_t authored_face = 0;
  for (uint32_t c : mesh->face_vertex_counts) {
    if (offset + c > index_count) {
      // counts overrun the index buffer: drop this and all later faces.
      dropped += 1;
      break;
    }
    bool face_ok = true;
    for (uint32_t i = 0; i < c; ++i) {
      // face_vertex_indices is uint32; a negative authored index arrived as a
      // huge value and fails this check too.
      if (mesh->face_vertex_indices[offset + i] >= point_count) {
        face_ok = false;
        break;
      }
    }
    if (face_ok) {
      face_remap[authored_face] = static_cast<int32_t>(counts.size());
      counts.push_back(c);
      for (uint32_t i = 0; i < c; ++i) {
        indices.push_back(mesh->face_vertex_indices[offset + i]);
      }
    } else {
      ++dropped;
    }
    offset += c;
    ++authored_face;
  }
  if (dropped > 0 || indices.size() != index_count ||
      counts.size() != mesh->face_vertex_counts.size()) {
    mesh->sanitize_dropped_faces = static_cast<uint32_t>(
        mesh->face_vertex_counts.size() - counts.size());
    if (mesh->sanitize_dropped_faces > 0) {
      mesh->sanitize_face_remap = std::move(face_remap);
    }
    AddWarning("Mesh '" + mesh->prim_path +
                        "': dropped invalid faces (out-of-range or negative "
                        "faceVertexIndices, or counts overrunning the index "
                        "buffer)");
    mesh->face_vertex_counts.clear();
    mesh->face_vertex_counts.append(counts.data(), counts.size());
    mesh->face_vertex_indices.clear();
    mesh->face_vertex_indices.append(indices.data(), indices.size());

    // holeIndices were read in authored face numbering; remap them so hole
    // faces keep pointing at the same topological faces after the drop.
    if (mesh->sanitize_dropped_faces > 0 && !mesh->hole_faces.empty()) {
      std::vector<uint32_t> remapped_holes;
      remapped_holes.reserve(mesh->hole_faces.size());
      for (uint32_t h : mesh->hole_faces) {
        if (h < mesh->sanitize_face_remap.size() &&
            mesh->sanitize_face_remap[h] >= 0) {
          remapped_holes.push_back(
              static_cast<uint32_t>(mesh->sanitize_face_remap[h]));
        }
      }
      std::sort(remapped_holes.begin(), remapped_holes.end());
      mesh->hole_faces = std::move(remapped_holes);
    }
  }
}


} } }  // namespace lightusd::tydra::next
