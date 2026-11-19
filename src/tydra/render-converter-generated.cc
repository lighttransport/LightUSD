// SPDX-License-Identifier: Apache-2.0
#include "types/value-view.hh"
#include "render-converter-internal.hh"
#include "render-converter-prop-read.hh"
#include "render-extract.hh"
#include "core/safe-arithmetic.hh"
#include "shape-to-mesh.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <utility>
namespace lightusd { namespace tydra {
using ::lightusd::UsdPrim; using ::lightusd::Value;
namespace {
constexpr size_t kMaxTempAllocBytes = 256u * 1024u * 1024u;
double ReadDoubleProperty(const UsdPrim& prim, const std::string& name,
                          double fallback, double time) {
  const Value* authored = prim.GetPropertyValue(name);
  const Value value = std::isnan(time)
      ? (authored ? *authored : Value())
      : prim.GetInterpolatedValue(name, time);
  if (const double* number = value.as_double()) return *number;
  if (const float* number = value.as_float()) return *number;
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
  out->face_vertex_counts.reserve_exact(face_counts.size());
  for (int c : face_counts) {
    out->face_vertex_counts.push_back(c < 0 ? uint32_t{0}
                                            : static_cast<uint32_t>(c));
  }
  out->face_vertex_indices.reserve_exact(face_indices.size());
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
  if (GetPropToken(prim, "orientation", &orientation)) {
    out->left_handed = (orientation == "leftHanded");
  }
  GetPropBool(prim, "doubleSided", &out->double_sided);
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

namespace {

// float/double array attribute as floats (knots and weights are double[];
// uRange/vRange are double2).
bool ReadPatchFloats(const UsdPrim& prim, const char* name, double time,
                     std::vector<float>* out) {
  ValueArrayRead<float> f;
  if (ReadFloatArray(prim, name, time, &f) && !f.empty()) {
    out->assign(f.begin(), f.end());
    return true;
  }
  const Value* v = prim.GetPropertyValue(name);
  if (!v) return false;
  if (const double* d2 = v->as_double2()) {
    out->assign({float(d2[0]), float(d2[1])});
    return true;
  }
  ::lightusd::ArrayScratch<double> scratch;
  ::lightusd::ArrayView<double> view;
  if (!::lightusd::GetDoubleArrayView(*v, &scratch, &view) || view.empty()) return false;
  out->clear();
  for (size_t i = 0; i < view.size; ++i) out->push_back(float(view[i]));
  return true;
}

// Cox-de Boor basis functions N_{i,order}(t) for all control points.
void NurbsBasis(const std::vector<double>& knots, int order, int count, double t,
                std::vector<double>* out) {
  const int m = static_cast<int>(knots.size()) - 1;
  std::vector<double> n(static_cast<size_t>(m), 0.0);
  // Degree-0 spans; the last non-degenerate span closes at the range end.
  int last = -1;
  for (int i = 0; i < m; ++i)
    if (knots[size_t(i)] < knots[size_t(i) + 1]) last = i;
  for (int i = 0; i < m; ++i) {
    const double a = knots[size_t(i)], b = knots[size_t(i) + 1];
    if (a < b && ((t >= a && t < b) || (i == last && t == b))) n[size_t(i)] = 1.0;
  }
  for (int k = 2; k <= order; ++k) {
    for (int i = 0; i + k <= m; ++i) {
      double v = 0.0;
      const double d1 = knots[size_t(i + k - 1)] - knots[size_t(i)];
      const double d2 = knots[size_t(i + k)] - knots[size_t(i + 1)];
      if (d1 > 0) v += (t - knots[size_t(i)]) / d1 * n[size_t(i)];
      if (d2 > 0) v += (knots[size_t(i + k)] - t) / d2 * n[size_t(i + 1)];
      n[size_t(i)] = v;
    }
  }
  out->assign(n.begin(), n.begin() + count);
}

}  // namespace

// UsdGeomNurbsPatch: rational tensor-product B-spline surface over
// [uRange] x [vRange] (default: knots[order-1] .. knots[count]); points are
// u-fastest (index = v * uVertexCount + u), pointWeights optional. Tessellated
// into a quad grid with (u,v) texcoords; trim curves are not applied.
bool RenderSceneConverter::Impl::TessellateNurbsPatch(
    const UsdPrim& prim, std::vector<value::float3>* points, std::vector<int>* face_counts,
    std::vector<int>* face_indices, std::vector<value::float2>* uvs) {
  auto read_int = [&](const char* name, int fallback) {
    const Value* v = prim.GetPropertyValue(name);
    const int32_t* i = v ? v->as_int() : nullptr;
    return i ? int(*i) : fallback;
  };
  const int nu = read_int("uVertexCount", 0), nv = read_int("vVertexCount", 0);
  const int ou = read_int("uOrder", 0), ov = read_int("vOrder", 0);
  std::vector<float> fu, fv, fw;
  ValueArrayRead<float> cps;
  if (nu < 2 || nv < 2 || ou < 2 || ov < 2 || ou > nu || ov > nv ||
      !ReadPatchFloats(prim, "uKnots", config_.time_code, &fu) ||
      !ReadPatchFloats(prim, "vKnots", config_.time_code, &fv) ||
      fu.size() != size_t(nu + ou) || fv.size() != size_t(nv + ov) ||
      !ReadFloatArray(prim, "points", config_.time_code, &cps) ||
      cps.view.size != size_t(nu) * size_t(nv) * 3) {
    SetLastError("Invalid NurbsPatch '" + prim.GetPath().str() + "'");
    return false;
  }
  const std::vector<double> ku(fu.begin(), fu.end()), kv(fv.begin(), fv.end());
  for (size_t i = 1; i < ku.size(); ++i)
    if (!(ku[i] >= ku[i - 1])) { SetLastError("NurbsPatch uKnots not ascending"); return false; }
  for (size_t i = 1; i < kv.size(); ++i)
    if (!(kv[i] >= kv[i - 1])) { SetLastError("NurbsPatch vKnots not ascending"); return false; }
  const bool weighted = ReadPatchFloats(prim, "pointWeights", config_.time_code, &fw) &&
                        fw.size() == size_t(nu) * size_t(nv);
  double u0 = ku[size_t(ou - 1)], u1 = ku[size_t(nu)];
  double v0 = kv[size_t(ov - 1)], v1 = kv[size_t(nv)];
  std::vector<float> range;
  if (ReadPatchFloats(prim, "uRange", config_.time_code, &range) && range.size() == 2 &&
      range[0] < range[1]) { u0 = range[0]; u1 = range[1]; }
  if (ReadPatchFloats(prim, "vRange", config_.time_code, &range) && range.size() == 2 &&
      range[0] < range[1]) { v0 = range[0]; v1 = range[1]; }
  if (prim.GetPropertyValue("trimCurve:counts"))
    AddWarning("NurbsPatch '" + prim.GetPath().str() + "': trim curves are not applied");
  const int su = std::min(64, 8 * (nu - ou + 1)), sv = std::min(64, 8 * (nv - ov + 1));
  std::vector<double> bu, bv;
  points->clear();
  uvs->clear();
  std::vector<value::float3> grid;
  std::vector<value::float2> grid_uv;
  for (int j = 0; j <= sv; ++j) {
    const double v = v0 + (v1 - v0) * j / sv;
    NurbsBasis(kv, ov, nv, v, &bv);
    for (int i = 0; i <= su; ++i) {
      const double u = u0 + (u1 - u0) * i / su;
      NurbsBasis(ku, ou, nu, u, &bu);
      double acc[3] = {0, 0, 0}, wsum = 0;
      for (int b = 0; b < nv; ++b) {
        if (bv[size_t(b)] == 0) continue;
        for (int a = 0; a < nu; ++a) {
          const size_t idx = size_t(b) * size_t(nu) + size_t(a);
          const double w = bu[size_t(a)] * bv[size_t(b)] * (weighted ? fw[idx] : 1.0);
          if (w == 0) continue;
          for (int c = 0; c < 3; ++c) acc[c] += w * cps.view.data[idx * 3 + size_t(c)];
          wsum += w;
        }
      }
      if (!(std::abs(wsum) > 1e-300)) {
        SetLastError("NurbsPatch '" + prim.GetPath().str() + "': degenerate weights");
        return false;
      }
      grid.push_back(value::float3{float(acc[0] / wsum), float(acc[1] / wsum),
                                   float(acc[2] / wsum)});
      grid_uv.push_back(value::float2{float(double(i) / su), float(double(j) / sv)});
    }
  }
  *points = grid;
  face_counts->clear();
  face_indices->clear();
  for (int j = 0; j < sv; ++j)
    for (int i = 0; i < su; ++i) {
      const int a = j * (su + 1) + i;
      const int quad[4] = {a, a + 1, a + su + 2, a + su + 1};
      face_counts->push_back(4);
      for (int k : quad) {
        face_indices->push_back(k);
        uvs->push_back(grid_uv[size_t(k)]);
      }
    }
  return true;
}

bool RenderSceneConverter::Impl::ConvertGeomPrimitive(const UsdPrim& prim,
                                                RenderMesh* out) {
  if (!out || !prim.IsValid() ||
      (!IsAnalyticGeomTypeName(prim.GetTypeName()) &&
       prim.GetTypeName() != "TetMesh" && prim.GetTypeName() != "NurbsPatch")) {
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
        ReadDoubleProperty(prim, "size", 2.0, config_.time_code), points, face_counts,
        face_indices, normals, uvs);
  } else if (type == "Sphere") {
    ::lightusd::tydra::GenerateIcosphereMesh(
        // USD's Sphere.radius default is 1 (Cube.size is 2). Normally the
        // schema registry supplies it and this fallback never fires.
        ReadDoubleProperty(prim, "radius", 1.0, config_.time_code),
        std::max(0, std::min(config_.mesh.sphere_subdivisions, 6)), points,
        face_counts,
        face_indices, normals, uvs);
  } else if (type == "Cylinder" || type == "Cylinder_1") {
    double radius = ReadDoubleProperty(prim, "radius", 1.0, config_.time_code);
    if (type == "Cylinder_1") {
      const double rt = ReadDoubleProperty(prim, "radiusTop", 1.0, config_.time_code);
      const double rb = ReadDoubleProperty(prim, "radiusBottom", 1.0, config_.time_code);
      radius = std::max(rt, rb);
      if (std::fabs(rt - rb) > 1.0e-9) {
        AddWarning("Cylinder_1 '" + prim.GetPath().str() +
                            "': tapered radii are approximated with max radius");
      }
    }
    ::lightusd::tydra::GenerateCylinderMesh(
        radius, ReadDoubleProperty(prim, "height", 2.0, config_.time_code), 24, 1, points,
        face_counts, face_indices, normals, uvs);
  } else if (type == "Cone") {
    ::lightusd::tydra::GenerateConeMesh(
        ReadDoubleProperty(prim, "radius", 1.0, config_.time_code),
        ReadDoubleProperty(prim, "height", 2.0, config_.time_code), 24, points, face_counts,
        face_indices, normals, uvs);
  } else if (type == "Capsule" || type == "Capsule_1") {
    double radius = ReadDoubleProperty(prim, "radius", 0.5, config_.time_code);
    double height = ReadDoubleProperty(prim, "height", type == "Capsule_1" ? 1.0 : 2.0, config_.time_code);
    if (type == "Capsule_1") {
      const double rt = ReadDoubleProperty(prim, "radiusTop", 0.5, config_.time_code);
      const double rb = ReadDoubleProperty(prim, "radiusBottom", 0.5, config_.time_code);
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
        ReadDoubleProperty(prim, "width", 2.0, config_.time_code),
        ReadDoubleProperty(prim, "length", 2.0, config_.time_code), 1, 1, points, face_counts,
        face_indices, normals, uvs);
    std::string axis = "Z";
    GetPropToken(prim, "axis", &axis);
    if (axis == "Z") {
      // UsdGeomPlane's canonical quad starts at (+width,+length); the
      // generator's face starts at (-width,+length). Remap authored tuples
      // rather than indexing them with the generator's private vertex order.
      ::lightusd::EvalOptions evaluation;
      evaluation.time = std::isnan(config_.time_code)
          ? ::lightusd::TimeQuery::Default()
          : ::lightusd::TimeQuery::Numeric(config_.time_code);
      evaluation.follow_connections = false;
      MeshFloatAttribute authored;
      if (ReadMeshFloatAttribute(prim, "primvars:st", evaluation, 2, 4, 4,
                                 &authored)) {
        const size_t canonical[4] = {1, 2, 3, 0};
        std::vector<value::float2> remapped;
        for (size_t i : canonical) {
          size_t tuple = 0;
          if (!authored.TupleIndex(i, i, 0, &tuple)) break;
          remapped.push_back({authored.values[tuple * 2],
                              authored.values[tuple * 2 + 1]});
        }
        if (remapped.size() == 4) uvs = std::move(remapped);
      }
    }
  } else if (type == "NurbsPatch") {
    if (!TessellateNurbsPatch(prim, &points, &face_counts, &face_indices, &uvs)) return false;
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
  if (type != "NurbsPatch") {  // open surfaces have no signed volume
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

  if (type != "TetMesh" && type != "NurbsPatch") {
    std::string axis = "Z";
    GetPropToken(prim, "axis", &axis);
    if (type == "Cube" || type == "Sphere") axis = "Y";
    ApplyAxis(&points, &normals, axis);
  }
  FillGeneratedMesh(prim, points, face_counts, face_indices, normals, uvs, out);
  // Extraction appends streams into an empty mesh. Keep generated defaults
  // when no usable authored stream exists, without doubling UV/normal arrays.
  auto generated_normals = std::move(out->normals);
  auto generated_uvs = std::move(out->texcoords_0);
  const auto generated_normals_interp = out->normals_interp;
  const auto generated_uv_interp = out->texcoords_0_interp;
  out->normals.clear();
  out->texcoords_0.clear();
  ExtractMeshPrimvars(prim, out);
  if (out->normals.empty()) {
    out->normals = std::move(generated_normals);
    out->normals_interp = generated_normals_interp;
  }
  // Plane generation already maps the authored texture domain to its canonical
  // quad corners. Preserve that mapping instead of reinterpreting it as Mesh
  // vertex order.
  if (out->texcoords_0.empty() || type == "Plane") {
    out->texcoords_0 = std::move(generated_uvs);
    out->texcoords_0_interp = generated_uv_interp;
  }
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
}}  // namespace lightusd::tydra
