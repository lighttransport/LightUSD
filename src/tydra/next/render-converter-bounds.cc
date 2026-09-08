// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Extent and application-supplied bounds proxies are deliberately separate
// from the catalog/orchestration TU: streaming callers use these without
// compiling material, hierarchy, or full scene conversion machinery.

#include "render-converter.hh"
#include "render-extract.hh"
#include "value-types.hh"

#include <algorithm>
#include <string>
#include <vector>

namespace lightusd {
namespace tydra {
namespace next {

using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;

namespace {

bool GetBool(const UsdPrim& prim, const char* name, bool* out) {
  const Value* value = prim.GetPropertyValue(name);
  const bool* result = value ? value->as_bool() : nullptr;
  if (!result || !out) return false;
  *out = *result;
  return true;
}

bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  const Value* value = prim.GetPropertyValue(name);
  if (!value || !out) return false;
  if (const std::string* result = value->as_token()) {
    *out = *result;
    return true;
  }
  if (const std::string* result = value->as_string()) {
    *out = *result;
    return true;
  }
  return false;
}

bool ReadExtent(const UsdPrim& prim, double time, value::float3* minimum,
                value::float3* maximum) {
  if (!minimum || !maximum) return false;
  ValueArrayRead<float> extent;
  if (!ReadFloatArray(prim, "extent", time, &extent) || extent.size() < 6) {
    return false;
  }
  *minimum = value::float3{extent[0], extent[1], extent[2]};
  *maximum = value::float3{extent[3], extent[4], extent[5]};
  return true;
}

void FillProxyMesh(const UsdPrim& prim,
                   const std::vector<value::float3>& points,
                   const std::vector<int>& face_counts,
                   const std::vector<int>& face_indices, RenderMesh* out) {
  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  out->face_vertex_counts.reserve(face_counts.size());
  for (int count : face_counts) {
    out->face_vertex_counts.push_back(
        count < 0 ? uint32_t{0} : static_cast<uint32_t>(count));
  }
  out->face_vertex_indices.reserve(face_indices.size());
  for (int index : face_indices) {
    out->face_vertex_indices.push_back(
        index < 0 ? uint32_t{0} : static_cast<uint32_t>(index));
  }
  for (const value::float3& point : points) {
    out->points.push_back(point[0]);
    out->points.push_back(point[1]);
    out->points.push_back(point[2]);
  }
  std::string orientation;
  if (GetToken(prim, "orientation", &orientation)) {
    out->left_handed = (orientation == "leftHanded");
  }
  GetBool(prim, "doubleSided", &out->double_sided);
  out->bbox_min = Float3(1e30f, 1e30f, 1e30f);
  out->bbox_max = Float3(-1e30f, -1e30f, -1e30f);
  for (const value::float3& point : points) {
    out->bbox_min.x = std::min(out->bbox_min.x, point[0]);
    out->bbox_min.y = std::min(out->bbox_min.y, point[1]);
    out->bbox_min.z = std::min(out->bbox_min.z, point[2]);
    out->bbox_max.x = std::max(out->bbox_max.x, point[0]);
    out->bbox_max.y = std::max(out->bbox_max.y, point[1]);
    out->bbox_max.z = std::max(out->bbox_max.z, point[2]);
  }
  out->has_bbox = true;
}

bool FillBoundsProxyMesh(const UsdPrim& prim, const value::float3& minimum,
                         const value::float3& maximum, RenderMesh* out) {
  if (!out) return false;
  const std::vector<value::float3> points = {
      {minimum[0], minimum[1], minimum[2]},
      {maximum[0], minimum[1], minimum[2]},
      {maximum[0], maximum[1], minimum[2]},
      {minimum[0], maximum[1], minimum[2]},
      {minimum[0], minimum[1], maximum[2]},
      {maximum[0], minimum[1], maximum[2]},
      {maximum[0], maximum[1], maximum[2]},
      {minimum[0], maximum[1], maximum[2]}};
  const std::vector<int> counts(6, 4);
  const std::vector<int> indices = {
      0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
      3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5};
  FillProxyMesh(prim, points, counts, indices, out);
  out->is_proxy = true;
  return true;
}

bool FillExtentProxyMesh(const UsdPrim& prim, double time, RenderMesh* out) {
  value::float3 minimum;
  value::float3 maximum;
  if (!ReadExtent(prim, time, &minimum, &maximum)) return false;
  return FillBoundsProxyMesh(prim, minimum, maximum, out);
}



}  // namespace

bool RenderSceneConverter::ConvertExtentProxy(const UsdPrim& prim,
                                              RenderMesh* out) {
  if (!FillExtentProxyMesh(prim, config_.time_code, out)) return false;
  if (config_.mesh.triangulate && !TriangulateMesh(out)) return false;
  if (config_.mesh.compute_normals && !out->has_normals() &&
      !ComputeVertexNormals(out)) return false;
  out->compact();
  return !out->has_alloc_failure();
}

bool RenderSceneConverter::ConvertBoundsProxy(const UsdPrim& prim,
                                              const Float3& minimum,
                                              const Float3& maximum,
                                              RenderMesh* out) {
  const value::float3 mn = {minimum.x, minimum.y, minimum.z};
  const value::float3 mx = {maximum.x, maximum.y, maximum.z};
  if (!FillBoundsProxyMesh(prim, mn, mx, out)) return false;
  if (config_.mesh.triangulate && !TriangulateMesh(out)) return false;
  if (config_.mesh.compute_normals && !out->has_normals() &&
      !ComputeVertexNormals(out)) return false;
  out->compact();
  return !out->has_alloc_failure();
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
