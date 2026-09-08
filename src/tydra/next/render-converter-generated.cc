// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "render-extract.hh"
#include "safe-arithmetic.hh"
#include "tydra/shape-to-mesh.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {
bool GetBool(const UsdPrim& prim, const char* name, bool* out) {
  const Value* value = prim.GetPropertyValue(name); const bool* result = value ? value->as_bool() : nullptr;
  if (!result || !out) return false; *out = *result; return true;
}
bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  const Value* value = prim.GetPropertyValue(name); if (!value || !out) return false;
  if (const std::string* result = value->as_token()) { *out = *result; return true; }
  if (const std::string* result = value->as_string()) { *out = *result; return true; }
  return false;
}
constexpr size_t kMaxTempAllocBytes = 256u * 1024u * 1024u;
double ReadDoubleProperty(const UsdPrim& prim, const std::string& name,
                          double fallback) {
  double d = fallback;
  if (GetDouble(prim, name, &d)) return d;
  return fallback;
}

void ApplyAxis(std::vector<value::float3>* points,
               std::vector<value::float3>* normals,
               const std::string& axis) {
  if (axis == "Y" || axis.empty()) return;
  // Proper ROTATIONS mapping the generator's +Y symmetry axis onto the
  // authored axis. The previous axis swap was a mirror (determinant -1),
  // which flipped the winding and turned the analytic meshes inside out.
  auto map_point = [&](value::float3& v) {
    const float x = v[0], y = v[1], z = v[2];
    if (axis == "Z") {
      // R_x(+90 deg): +Y -> +Z
      v[0] = x; v[1] = -z; v[2] = y;
    } else if (axis == "X") {
      // R_z(-90 deg): +Y -> +X
      v[0] = y; v[1] = -x; v[2] = z;
    }
  };
  for (value::float3& p : *points) map_point(p);
  if (normals) {
    for (value::float3& n : *normals) map_point(n);
  }
}

void FillGeneratedMesh(const UsdPrim& prim,
                       const std::vector<value::float3>& points,
                       const std::vector<int>& face_counts,
                       const std::vector<int>& face_indices,
                       const std::vector<value::float3>& normals,
                       const std::vector<value::float2>& uvs,
                       RenderMesh* out) {
  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  out->face_vertex_counts.reserve(face_counts.size());
  for (int c : face_counts) {
    out->face_vertex_counts.push_back(c < 0 ? uint32_t{0}
                                            : static_cast<uint32_t>(c));
  }
  out->face_vertex_indices.reserve(face_indices.size());
  for (int idx : face_indices) {
    out->face_vertex_indices.push_back(idx < 0 ? uint32_t{0}
                                               : static_cast<uint32_t>(idx));
  }
  for (const value::float3& p : points) {
    out->points.push_back(p[0]);
    out->points.push_back(p[1]);
    out->points.push_back(p[2]);
  }
  if (!normals.empty()) {
    out->normals_interp = Interpolation::FaceVarying;
    for (const value::float3& n : normals) {
      out->normals.push_back(n[0]);
      out->normals.push_back(n[1]);
      out->normals.push_back(n[2]);
    }
  }
  if (!uvs.empty()) {
    out->texcoords_0_interp = Interpolation::FaceVarying;
    for (const value::float2& uv : uvs) {
      out->texcoords_0.push_back(uv[0]);
      out->texcoords_0.push_back(uv[1]);
    }
  }
  std::string orientation;
  if (GetToken(prim, "orientation", &orientation)) {
    out->left_handed = (orientation == "leftHanded");
  }
  GetBool(prim, "doubleSided", &out->double_sided);
  if (!points.empty()) {
    out->bbox_min = Float3(1e30f, 1e30f, 1e30f);
    out->bbox_max = Float3(-1e30f, -1e30f, -1e30f);
    for (const value::float3& p : points) {
      out->bbox_min.x = std::min(out->bbox_min.x, p[0]);
      out->bbox_min.y = std::min(out->bbox_min.y, p[1]);
      out->bbox_min.z = std::min(out->bbox_min.z, p[2]);
      out->bbox_max.x = std::max(out->bbox_max.x, p[0]);
      out->bbox_max.y = std::max(out->bbox_max.y, p[1]);
      out->bbox_max.z = std::max(out->bbox_max.z, p[2]);
    }
    out->has_bbox = true;
  }
}
}  // namespace

bool RenderSceneConverter::ConvertGeomPrimitive(const UsdPrim& prim,
                                                RenderMesh* out) {
  if (!out || !prim.IsValid() ||
      (!IsAnalyticGeomTypeName(prim.GetTypeName()) &&
       prim.GetTypeName() != "TetMesh")) {
    SetLastError("Invalid generated geom prim");
    return false;
  }

  std::vector<value::float3> points;
  std::vector<int> face_counts;
  std::vector<int> face_indices;
  std::vector<value::float3> normals;
  std::vector<value::float2> uvs;

  const std::string type = prim.GetTypeName();
  if (type == "TetMesh") {
    ValueArrayRead<float> authored_points;
    ValueArrayRead<int32_t> authored_tets;
    if (!ReadFloatArray(prim, "points", config_.time_code, &authored_points) ||
        authored_points.empty() || (authored_points.view.size % 3) != 0 ||
        !ReadIntArray(prim, "tetVertexIndices", config_.time_code,
                      &authored_tets) ||
        authored_tets.empty() || (authored_tets.view.size % 4) != 0) {
      SetLastError("Invalid TetMesh points or tetVertexIndices");
      return false;
    }

    const size_t point_count = authored_points.view.size / 3;
    points.reserve(point_count);
    for (size_t i = 0; i < point_count; ++i) {
      points.push_back(value::float3{authored_points.view.data[i * 3 + 0],
                                     authored_points.view.data[i * 3 + 1],
                                     authored_points.view.data[i * 3 + 2]});
    }

    using FaceKey = std::array<int32_t, 3>;
    struct BoundaryFace {
      FaceKey key{};
      FaceKey oriented{};
    };
    const size_t tet_count = authored_tets.view.size / 4;
    if (tet_count > (kMaxTempAllocBytes / (4 * sizeof(BoundaryFace)))) {
      SetLastError("TetMesh boundary extraction exceeds temporary-memory cap");
      return false;
    }
    std::vector<BoundaryFace> faces;
    faces.reserve(tet_count * 4);
    for (size_t tet = 0; tet < tet_count; ++tet) {
      const int32_t* v = authored_tets.view.data + tet * 4;
      bool valid = true;
      for (size_t corner = 0; corner < 4; ++corner) {
        if (v[corner] < 0 || static_cast<size_t>(v[corner]) >= point_count) {
          valid = false;
        }
      }
      if (!valid || v[0] == v[1] || v[0] == v[2] || v[0] == v[3] ||
          v[1] == v[2] || v[1] == v[3] || v[2] == v[3]) {
        AddWarning("TetMesh '" + prim.GetPath().str() +
                            "': skipped malformed tetrahedron " +
                            std::to_string(tet));
        continue;
      }
      const FaceKey oriented[4] = {{v[0], v[2], v[1]}, {v[0], v[1], v[3]},
                                   {v[0], v[3], v[2]}, {v[1], v[2], v[3]}};
      for (const FaceKey& face : oriented) {
        FaceKey key = face;
        std::sort(key.begin(), key.end());
        faces.push_back(BoundaryFace{key, face});
      }
    }
    std::sort(faces.begin(), faces.end(),
              [](const BoundaryFace& a, const BoundaryFace& b) {
                return a.key < b.key;
              });
    for (size_t begin = 0; begin < faces.size();) {
      size_t end = begin + 1;
      while (end < faces.size() && faces[end].key == faces[begin].key) ++end;
      if (end == begin + 1) {
        face_counts.push_back(3);
        face_indices.insert(face_indices.end(),
                            faces[begin].oriented.begin(),
                            faces[begin].oriented.end());
      }
      begin = end;
    }
    if (face_counts.empty()) {
      SetLastError("TetMesh has no valid boundary faces");
      return false;
    }
  } else if (type == "Cube") {
    ::lightusd::tydra::GenerateCubeMesh(
        ReadDoubleProperty(prim, "size", 2.0), points, face_counts,
        face_indices, normals, uvs);
  } else if (type == "Sphere") {
    ::lightusd::tydra::GenerateIcosphereMesh(
        // USD's Sphere.radius default is 1 (Cube.size is 2). Normally the
        // schema registry supplies it and this fallback never fires.
        ReadDoubleProperty(prim, "radius", 1.0),
        std::max(0, std::min(config_.mesh.sphere_subdivisions, 6)), points,
        face_counts,
        face_indices, normals, uvs);
  } else if (type == "Cylinder" || type == "Cylinder_1") {
    double radius = ReadDoubleProperty(prim, "radius", 1.0);
    if (type == "Cylinder_1") {
      const double rt = ReadDoubleProperty(prim, "radiusTop", 1.0);
      const double rb = ReadDoubleProperty(prim, "radiusBottom", 1.0);
      radius = std::max(rt, rb);
      if (std::fabs(rt - rb) > 1.0e-9) {
        AddWarning("Cylinder_1 '" + prim.GetPath().str() +
                            "': tapered radii are approximated with max radius");
      }
    }
    ::lightusd::tydra::GenerateCylinderMesh(
        radius, ReadDoubleProperty(prim, "height", 2.0), 24, 1, points,
        face_counts, face_indices, normals, uvs);
  } else if (type == "Cone") {
    ::lightusd::tydra::GenerateConeMesh(
        ReadDoubleProperty(prim, "radius", 1.0),
        ReadDoubleProperty(prim, "height", 2.0), 24, points, face_counts,
        face_indices, normals, uvs);
  } else if (type == "Capsule" || type == "Capsule_1") {
    double radius = ReadDoubleProperty(prim, "radius", 0.5);
    double height = ReadDoubleProperty(prim, "height", type == "Capsule_1" ? 1.0 : 2.0);
    if (type == "Capsule_1") {
      const double rt = ReadDoubleProperty(prim, "radiusTop", 0.5);
      const double rb = ReadDoubleProperty(prim, "radiusBottom", 0.5);
      radius = std::max(rt, rb);
      if (std::fabs(rt - rb) > 1.0e-9) {
        AddWarning("Capsule_1 '" + prim.GetPath().str() +
                            "': asymmetric radii are approximated with max radius");
      }
    }
    ::lightusd::tydra::GenerateCapsuleMesh(radius, height, 24, 1, points,
                                           face_counts, face_indices, normals,
                                           uvs);
  } else if (type == "Plane") {
    ::lightusd::tydra::GeneratePlaneMesh(
        ReadDoubleProperty(prim, "width", 2.0),
        ReadDoubleProperty(prim, "length", 2.0), 1, 1, points, face_counts,
        face_indices, normals, uvs);
  } else {
    SetLastError("Unsupported analytic geom prim");
    return false;
  }

  // The shape generators are inconsistent about winding: capsule/cone emit
  // INWARD (negative signed volume) faces while cube/sphere/cylinder are
  // outward. The old axis MIRROR happened to flip capsule/cone right side
  // out at the default Z axis (while turning the cylinder inside out); with
  // the axis applied as a proper rotation below, normalize the winding here
  // so every closed solid is outward. Authored normals already point
  // outward, so only the corner order flips (normals/uvs are per-corner and
  // reverse with it to stay parallel).
  {
    double volume = 0.0;
    size_t off = 0;
    for (int c : face_counts) {
      if (c < 3 || off + size_t(c) > face_indices.size()) break;
      const value::float3& a = points[size_t(face_indices[off])];
      for (int k = 1; k + 1 < c; ++k) {
        const value::float3& b = points[size_t(face_indices[off + size_t(k)])];
        const value::float3& d =
            points[size_t(face_indices[off + size_t(k) + 1])];
        volume += (double(a[0]) * (double(b[1]) * d[2] - double(b[2]) * d[1]) +
                   double(a[1]) * (double(b[2]) * d[0] - double(b[0]) * d[2]) +
                   double(a[2]) * (double(b[0]) * d[1] - double(b[1]) * d[0])) /
                  6.0;
      }
      off += size_t(c);
    }
    if (volume < -1.0e-9) {
      size_t start = 0;
      for (int c : face_counts) {
        if (c <= 0 || start + size_t(c) > face_indices.size()) break;
        std::reverse(face_indices.begin() + start,
                     face_indices.begin() + start + size_t(c));
        if (normals.size() >= start + size_t(c)) {
          std::reverse(normals.begin() + start,
                       normals.begin() + start + size_t(c));
        }
        if (uvs.size() >= start + size_t(c)) {
          std::reverse(uvs.begin() + start,
                       uvs.begin() + start + size_t(c));
        }
        start += size_t(c);
      }
    }
  }

  if (type != "TetMesh") {
    std::string axis = "Z";
    GetToken(prim, "axis", &axis);
    if (type == "Cube" || type == "Sphere") axis = "Y";
    ApplyAxis(&points, &normals, axis);
  }
  FillGeneratedMesh(prim, points, face_counts, face_indices, normals, uvs, out);
  SanitizeMeshTopology(out);
  if (config_.mesh.triangulate && !out->is_triangulated) {
    TriangulateMesh(out);
  }
  if (config_.mesh.compute_normals && out->normals.empty()) {
    ComputeVertexNormals(out);
  }
  if (config_.mesh.compute_tangents && out->tangents.empty()) {
    ComputeVertexTangents(out);
  }
  return true;
}
}}}  // namespace lightusd::tydra::next
