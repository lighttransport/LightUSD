// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Mesh topology and geometry extraction

#include "render-converter.hh"

#include <algorithm>
#include <cstdint>
#include <string>

namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;
namespace {
bool GetBool(const UsdPrim& prim, const char* name, bool* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value || !value->as_bool()) return false;
  *out = *value->as_bool();
  return true;
}

bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const std::string* token = value->as_token()) { *out = *token; return true; }
  if (const std::string* text = value->as_string()) { *out = *text; return true; }
  return false;
}

}  // namespace

bool RenderSceneConverter::ExtractMeshTopology(const UsdPrim& prim, RenderMesh* mesh) {
  // Get face vertex counts
  ValueArrayRead<int32_t> face_counts;
  ReadIntArray(prim, "faceVertexCounts", config_.time_code, &face_counts);
  if (face_counts.empty()) {
    SetLastError("Mesh has no faceVertexCounts");
    return false;
  }

  mesh->face_vertex_counts.reserve(face_counts.size());
  for (int32_t c : face_counts) {
    mesh->face_vertex_counts.push_back(static_cast<uint32_t>(c));
  }

  // Get face vertex indices
  ValueArrayRead<int32_t> indices;
  ReadIntArray(prim, "faceVertexIndices", config_.time_code, &indices);
  if (indices.empty()) {
    SetLastError("Mesh has no faceVertexIndices");
    return false;
  }

  mesh->face_vertex_indices.reserve(indices.size());
  for (int32_t i : indices) {
    mesh->face_vertex_indices.push_back(static_cast<uint32_t>(i));
  }

  std::string orientation;
  if (GetToken(prim, "orientation", &orientation)) {
    mesh->left_handed = (orientation == "leftHanded");
  }

  GetBool(prim, "doubleSided", &mesh->double_sided);

  // holeIndices: face indices excluded from rendering.
  {
    ValueArrayRead<int32_t> holes;
    if (ReadIntArray(prim, "holeIndices", config_.time_code, &holes)) {
      for (int32_t h : holes) {
        if (h >= 0) mesh->hole_faces.push_back(static_cast<uint32_t>(h));
      }
      std::sort(mesh->hole_faces.begin(), mesh->hole_faces.end());
    }
  }

  return true;
}

bool RenderSceneConverter::ExtractMeshGeometry(const UsdPrim& prim, RenderMesh* mesh) {
  ValueArrayRead<float> points;
  ReadFloatArray(prim, "points", config_.time_code, &points);
  if (points.empty()) {
    SetLastError("Invalid points data");
    return false;
  }

  // Copy directly to chunked array
  mesh->points.append(points.view.data, points.view.size);

  // Compute bounding box
  size_t num_points = mesh->point_count();
  if (num_points > 0) {
    mesh->bbox_min = Float3(1e30f, 1e30f, 1e30f);
    mesh->bbox_max = Float3(-1e30f, -1e30f, -1e30f);

    for (size_t i = 0; i < num_points; ++i) {
      float x = mesh->points[i * 3 + 0];
      float y = mesh->points[i * 3 + 1];
      float z = mesh->points[i * 3 + 2];

      mesh->bbox_min.x = std::min(mesh->bbox_min.x, x);
      mesh->bbox_min.y = std::min(mesh->bbox_min.y, y);
      mesh->bbox_min.z = std::min(mesh->bbox_min.z, z);
      mesh->bbox_max.x = std::max(mesh->bbox_max.x, x);
      mesh->bbox_max.y = std::max(mesh->bbox_max.y, y);
      mesh->bbox_max.z = std::max(mesh->bbox_max.z, z);
    }
    mesh->has_bbox = true;
  }

  // Authored normals are handled in ExtractMeshPrimvars (after topology
  // sanitization, where interpolation metadata and element-count validation
  // live).

  return true;
}


} } }  // namespace lightusd::tydra::next
