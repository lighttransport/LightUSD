// SPDX-License-Identifier: Apache-2.0
// Geom payloads: mesh fitting, fromto frames, inertials.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
// Symmetric 3x3 eigen-decomposition (cyclic Jacobi), eigenvalues sorted in
// decreasing order like mjuu_eig3; eigenvector columns form a right-handed
// rotation.
void EigenDescending(const double in[3][3], double eval[3], double evec[3][3]) {
  double a[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      a[i][j] = in[i][j];
      evec[i][j] = i == j ? 1.0 : 0.0;
    }
  // Converge relative to the tensor scale: round-off off-diagonals of an
  // (almost) isotropic tensor must not pick an arbitrary principal frame.
  const double scale = std::abs(a[0][0]) + std::abs(a[1][1]) + std::abs(a[2][2]);
  for (int sweep = 0; sweep < 64; ++sweep) {
    const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
    if (off <= 1e-14 * scale || off < 1e-300) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
        for (int k = 0; k < 3; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - sn * akq;
          a[k][q] = sn * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - sn * aqk;
          a[q][k] = sn * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = evec[k][p], vkq = evec[k][q];
          evec[k][p] = c * vkp - sn * vkq;
          evec[k][q] = sn * vkp + c * vkq;
        }
      }
  }
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int x, int y) { return a[x][x] > a[y][y]; });
  double v[3][3];
  for (int j = 0; j < 3; ++j) {
    eval[j] = a[order[j]][order[j]];
    for (int i = 0; i < 3; ++i) v[i][j] = evec[i][order[j]];
  }
  // Right-handed: third column = first x second.
  v[0][2] = v[1][0] * v[2][1] - v[2][0] * v[1][1];
  v[1][2] = v[2][0] * v[0][1] - v[0][0] * v[2][1];
  v[2][2] = v[0][0] * v[1][1] - v[1][0] * v[0][1];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) evec[i][j] = v[i][j];
}

// Triangle list of a loaded mesh (polygons triangulated as ClipPolygon).
bool MeshTriangles(const Mesh& mesh, std::vector<int32_t>* tri) {
  tri->clear();
  if (mesh.counts.empty()) {
    if (mesh.indices.size() % 3) return false;
    *tri = mesh.indices;
    return true;
  }
  size_t offset = 0;
  for (int32_t count : mesh.counts) {
    if (count < 3 || offset + size_t(count) > mesh.indices.size()) return false;
    std::vector<int32_t> corners;
    if (count == 3) {
      corners = {0, 1, 2};
    } else if (!ClipPolygon(mesh.positions, mesh.indices.data() + offset, size_t(count), &corners)) {
      return false;
    }
    for (int32_t c : corners) tri->push_back(mesh.indices[offset + size_t(c)]);
    offset += size_t(count);
  }
  return offset == mesh.indices.size();
}

bool FitMeshGeom(const pugi::xml_node& geom_node, const AttrMap& cls,
                 const std::map<std::string, MeshAsset>& assets,
                 const std::string& type, MeshFit* fit, std::string* err) {
  const std::string mesh_name = Eff(geom_node, cls, "mesh");
  const auto it = assets.find(mesh_name);
  if (it == assets.end()) {
    if (err) *err = "Missing MJCF mesh asset: " + mesh_name;
    return false;
  }
  const std::string key = mesh_name + "|" + type;
  if (active_mesh_fits) {
    auto cached = active_mesh_fits->find(key);
    if (cached != active_mesh_fits->end()) {
      *fit = cached->second;
      goto scale;
    }
  }
  {
    Mesh mesh;
    if (!LoadMeshFile(it->second.path, it->second.smoothnormal, &mesh, err)) return false;
    const std::vector<double> m =
        MultiplyMatrix(MeshRefMatrix(it->second), ScaleMatrix(it->second.scale));
    std::vector<std::array<double, 3>> v(mesh.positions.size() / 3);
    for (size_t i = 0; i < v.size(); ++i) {
      const double x = mesh.positions[3 * i], y = mesh.positions[3 * i + 1],
                   z = mesh.positions[3 * i + 2];
      v[i] = {{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
               m[2] * x + m[6] * y + m[10] * z + m[14]}};
    }
    std::vector<int32_t> tri;
    double volume = 0, unit[3][3];
    std::array<double, 3> com{{0, 0, 0}};
    if (!MeshTriangles(mesh, &tri) ||
        !MeshMassProperties(v, tri, it->second.inertia, &volume, &com, unit)) {
      if (err) *err = "Cannot fit MJCF geom to degenerate mesh: " + mesh_name;
      return false;
    }
    double eval[3], evec[3][3];
    EigenDescending(unit, eval, evec);
    std::array<double, 3> box;
    for (int i = 0; i < 3; ++i) {
      const double e = eval[(i + 1) % 3] + eval[(i + 2) % 3] - eval[i];
      box[i] = 0.5 * std::sqrt(std::max(0.0, 6.0 * e / volume));
    }
    std::array<double, 3> center{{0, 0, 0}};
    if (active_fitaabb) {
      // AABB of the mesh in its principal frame (mjCMesh aamm_).
      std::array<double, 3> lo{{1e300, 1e300, 1e300}}, hi{{-1e300, -1e300, -1e300}};
      for (const auto& p : v)
        for (int j = 0; j < 3; ++j) {
          double d = 0;
          for (int i = 0; i < 3; ++i) d += evec[i][j] * (p[i] - com[i]);
          lo[j] = std::min(lo[j], d);
          hi[j] = std::max(hi[j], d);
        }
      for (int j = 0; j < 3; ++j) {
        center[j] = 0.5 * (lo[j] + hi[j]);
        box[j] = hi[j] - center[j];
      }
    }
    std::array<double, 3>& size = fit->size;
    if (type == "sphere") {
      size[0] = active_fitaabb ? std::max({box[0], box[1], box[2]})
                               : (box[0] + box[1] + box[2]) / 3;
    } else if (type == "capsule" || type == "cylinder") {
      size[0] = active_fitaabb ? std::max(box[0], box[1]) : (box[0] + box[1]) / 2;
      size[1] = active_fitaabb ? box[2] - (type == "capsule" ? size[0] : 0.0)
                               : (type == "capsule" ? std::max(0.0, box[2] - size[0] / 2) : box[2]);
    } else if (type == "box" || type == "ellipsoid") {
      size = box;
    } else {
      if (err) *err = "MJCF mesh fitting does not support geom type " + type;
      return false;
    }
    fit->frame = IdentityMatrix();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) fit->frame[j * 4 + i] = evec[i][j];
      double c = com[i];
      for (int j = 0; j < 3; ++j) c += evec[i][j] * center[j];
      fit->frame[12 + i] = c;
    }
    if (active_mesh_fits) active_mesh_fits->emplace(key, *fit);
  }
scale:
  const double fitscale = EffDouble(geom_node, cls, "fitscale", 1.0);
  for (double& x : fit->size) x *= fitscale;
  return true;
}

// MJCF fromto for box/ellipsoid: the segment sets the frame (midpoint, Z along
// the segment) and the half-size along Z (size[axis]).
void ApplyFromtoFrame(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::vector<double>& body_world, size_t axis,
                      std::array<double, 3>* size, std::vector<double>* matrix) {
  const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
  if (fromto.size() < 6) return;
  const std::array<double, 3> dir{{fromto[3] - fromto[0], fromto[4] - fromto[1],
                                   fromto[5] - fromto[2]}};
  (*size)[axis] = 0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  *matrix = MultiplyMatrix(
      body_world, MatrixFromPosQuat({{0.5 * (fromto[0] + fromto[3]), 0.5 * (fromto[1] + fromto[4]),
                                      0.5 * (fromto[2] + fromto[5])}},
                                    QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir)));
}

bool BuildGeomPayload(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::string& cls_name,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world,
                      MeshPayload* payload, bool* is_visual, std::string* err,
                      bool force_collision, bool native_shape) {
  const std::string type = HasEff(geom_node, cls, "type")
                               ? Eff(geom_node, cls, "type")
                           : HasEff(geom_node, cls, "mesh") ? "mesh"
                                                            : "sphere";
  payload->name = Attr(geom_node, "name");
  if (payload->name.empty())
    payload->name = Eff(geom_node, cls, "mesh", "geom");
  // Bake the accumulated body-chain world transform into the geom matrix: the
  // body_world is the requested geometry anchor (identity for body-local
  // meshes, a world frame for static world geoms). The geom pose may
  // be inherited from a <default> class (e.g. allegro fingertip geoms get
  // pos="0 0 0.0267" from class="fingertip_visual"), so resolve via Eff.
  const std::array<double, 3> geom_pos =
      ParseDouble3(Eff(geom_node, cls, "pos"), {{0.0, 0.0, 0.0}});
  const Quat geom_quat = OrientationQuat(
      Eff(geom_node, cls, "quat"), Eff(geom_node, cls, "axisangle"),
      Eff(geom_node, cls, "euler"), Eff(geom_node, cls, "xyaxes"),
      Eff(geom_node, cls, "zaxis"), ctx);
  payload->matrix =
      MultiplyMatrix(body_world, MatrixFromPosQuat(geom_pos, geom_quat));
  // Mesh-fit primitive (mesh= on a non-mesh type): fitted size and frame.
  std::vector<double> fitted_size;
  if (type != "mesh" && type != "hfield" && type != "plane" && HasEff(geom_node, cls, "mesh")) {
    MeshFit fit;
    if (!FitMeshGeom(geom_node, cls, assets, type, &fit, err)) return false;
    fitted_size.assign(fit.size.begin(), fit.size.end());
    payload->matrix = MultiplyMatrix(payload->matrix, fit.frame);
  }
  auto geom_sizes = [&]() {
    return fitted_size.empty() ? ParseDoubles(Eff(geom_node, cls, "size")) : fitted_size;
  };
  auto geom_size3 = [&](std::array<double, 3> fallback) {
    const auto v = geom_sizes();
    for (size_t i = 0; i < 3 && i < v.size(); ++i) fallback[i] = v[i];
    return fallback;
  };

  // Classification mirrors the JS converter: a resolved geom group (own or
  // inherited from <default>) wins -- groups 0-2 are visible, 3-5 are
  // collision. With no group anywhere, fall back to the class name and then
  // MuJoCo's contype/conaffinity==0 (visual-only) convention.
  if (HasEff(geom_node, cls, "group")) {
    (*is_visual) =
        ToInt32(EffDouble(geom_node, cls, "group", 3.0), "group") < 3;
  } else {
    const std::string lname = ToLower(cls_name);
    if (lname.find("collision") != std::string::npos ||
        lname.find("collider") != std::string::npos) {
      (*is_visual) = false;
    } else if (Eff(geom_node, cls, "contype") == "0" &&
               Eff(geom_node, cls, "conaffinity") == "0") {
      (*is_visual) = true;
    } else {
      (*is_visual) = true;  // default group 0 is visible
    }
  }

  if (force_collision) *is_visual = false;
  // Native gprim (collider, or a primitive visual in non-legacy profiles)
  // instead of a tessellated mesh.
  const bool native = native_shape || (!(*is_visual) && !opts.tessellate_collision_shapes);

  if (type == "mesh") {
    const std::string mesh_name = Eff(geom_node, cls, "mesh");
    const auto it = assets.find(mesh_name);
    if (it == assets.end()) {
      if (err) *err = "Missing MJCF mesh asset: " + mesh_name;
      return false;
    }
    if (!LoadMeshFile(it->second.path, it->second.smoothnormal, &payload->mesh,
                      err))
      return false;

    const std::array<double, 3> scale =
        ParseDouble3(Eff(geom_node, cls, "scale"), it->second.scale);
    // G * refframe(refpos/refquat) * scale, matching the JS mesh handling.
    payload->matrix =
        MultiplyMatrix(payload->matrix, MeshRefMatrix(it->second));
    payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(scale));
  } else if (type == "hfield") {
    // <geom type="hfield" hfield="name"> -> tessellated GeomMesh (top surface).
    const std::string hf_name = Eff(geom_node, cls, "hfield");
    const auto it = hfields.find(hf_name);
    if (it == hfields.end()) {
      if (err) *err = "Missing MJCF hfield asset: " + hf_name;
      return false;
    }
    payload->mesh = TessellateHField(it->second);
    if (payload->mesh.positions.empty()) {
      if (err)
        *err = active_reader->failure.empty() ? "Empty/invalid hfield asset: " + hf_name
                                              : active_reader->failure;
      return false;
    }
  } else if (type == "box") {
    auto half = geom_size3({{0.05, 0.05, 0.05}});
    ApplyFromtoFrame(geom_node, cls, body_world, 2, &half, &payload->matrix);
    if (native) {
      payload->shape = {{"type", "box"}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(half));
    } else {
      payload->mesh = MakeBoxMesh(half);
    }
  } else if (type == "sphere") {
    const auto size = geom_sizes();
    const double radius = size.empty() ? 0.05 : size[0];
    if (native) {
      payload->shape = {{"type", "sphere"}, {"radius", radius}};
    } else {
      payload->mesh = MakeSphereMesh(radius);
    }
  } else if (type == "ellipsoid") {
    auto radii = geom_size3({{0.05, 0.05, 0.05}});
    ApplyFromtoFrame(geom_node, cls, body_world, 2, &radii, &payload->matrix);
    const bool uniform = radii[0] == radii[1] && radii[1] == radii[2];
    if (!uniform && opts.profile != "legacy") {
      // UsdGeom has no ellipsoid and consumers ignore a nonuniformly scaled
      // sphere's scale (Newton keeps the unit radius; OpenUSD rejects such
      // colliders): bake the radii into a mesh, a convex hull as a collider.
      payload->mesh = MakeSphereMesh(1.0, 24, 12);
      for (size_t i = 0; i < payload->mesh.positions.size(); ++i)
        payload->mesh.positions[i] = static_cast<float>(payload->mesh.positions[i] * radii[i % 3]);
    } else if (native) {
      if (!uniform && !(*is_visual) &&
          !UnsupportedXML(geom_node,
              "Ellipsoid collider is represented by a nonuniformly scaled sphere; enable tessellate_collision_shapes for a mesh collider",
              "mjcf.ellipsoid_collision_approximated", "mjcf.ellipsoid_collision",
              active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
      payload->shape = {{"type", "sphere"}, {"radius", 1.0}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    } else {
      payload->mesh = MakeSphereMesh(1.0);
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    }
  } else if (type == "cylinder" || type == "capsule") {
    const auto size = geom_sizes();
    const double radius = size.empty() ? 0.05 : size[0];
    double half_length = size.size() >= 2 ? size[1] : radius;
    const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
    if (fromto.size() >= 6) {
      const std::array<double, 3> dir{{fromto[3] - fromto[0],
                                       fromto[4] - fromto[1],
                                       fromto[5] - fromto[2]}};
      half_length =
          0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      // fromto fully specifies the geom frame (midpoint + Z aligned to dir),
      // overriding the geom's own pos/quat; still placed in body world space.
      const std::array<double, 3> center{{0.5 * (fromto[0] + fromto[3]),
                                          0.5 * (fromto[1] + fromto[4]),
                                          0.5 * (fromto[2] + fromto[5])}};
      payload->matrix = MultiplyMatrix(
          body_world,
          MatrixFromPosQuat(center, QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir)));
    }
    if (native) {
      payload->shape = {{"type", type},
                        {"radius", radius},
                        {"height", half_length * 2.0},
                        {"axis", "Z"}};
    } else {
      payload->mesh = type == "capsule" ? MakeCapsuleMesh(radius, half_length)
                                        : MakeCylinderMesh(radius, half_length);
    }
  } else if (type == "plane") {
    const auto size = ParseDoubles(Eff(geom_node, cls, "size"));
    const double sx = size.size() >= 1 && size[0] > 0.0 ? size[0] : 1.0;
    const double sy = size.size() >= 2 && size[1] > 0.0 ? size[1] : 1.0;
    const double sz = size.size() >= 3 && size[2] > 0.0 ? size[2] : 0.001;
    if (native) {
      payload->shape = {{"type", "plane"},
                        {"width", sx * 2.0},
                        {"length", sy * 2.0},
                        {"axis", "Z"}};
    } else {
      payload->mesh = MakeBoxMesh({{sx, sy, sz}});
    }
  } else {
    if (err) *err = "Unsupported native MJCF geom type: " + type;
    return false;
  }
  return true;
}

std::string AxisToken(const std::array<double, 3>& axis) {
  const double ax = std::abs(axis[0]);
  const double ay = std::abs(axis[1]);
  const double az = std::abs(axis[2]);
  if (ay >= ax && ay >= az) return "Y";
  if (az >= ax && az >= ay) return "Z";
  return "X";
}

Json InertialToJson(const pugi::xml_node& body_node, const Context& ictx) {
  Json inertial = Json::object();
  const auto inertial_node = Child(body_node, "inertial");
  if (!inertial_node) return inertial;
  inertial["mass"] = ParseDoubleAttr(inertial_node, "mass", 0.0);
  const auto com = ParseDouble3(Attr(inertial_node, "pos"), {{0.0, 0.0, 0.0}});
  inertial["centerOfMass"] = Json::array({com[0], com[1], com[2]});
  // MJCF inertia: <inertial fullinertia="Ixx Iyy Izz Ixy Ixz Iyz"> (full
  // symmetric tensor) or the more common <inertial diaginertia="Ixx Iyy Izz">.
  const std::vector<double> full =
      ParseDoubles(Attr(inertial_node, "fullinertia"));
  if (full.size() >= 6) {
    // Carry all 6 components so the converter can diagonalize them into
    // diagonalInertia (eigenvalues) + principalAxes (eigenvector quaternion).
    // Keep a diagonal fallback for consumers that ignore fullInertia.
    inertial["fullInertia"] =
        Json::array({full[0], full[1], full[2], full[3], full[4], full[5]});
    inertial["diagonalInertia"] = Json::array({full[0], full[1], full[2]});
  } else if (full.size() >= 3) {
    inertial["diagonalInertia"] = Json::array({full[0], full[1], full[2]});
  } else {
    const std::vector<double> diag =
        ParseDoubles(Attr(inertial_node, "diaginertia"));
    if (diag.size() >= 3) {
      inertial["diagonalInertia"] = Json::array({diag[0], diag[1], diag[2]});
    }
  }
  const auto iq = OrientationQuat(
      Attr(inertial_node, "quat"), Attr(inertial_node, "axisangle"),
      Attr(inertial_node, "euler"), Attr(inertial_node, "xyaxes"),
      Attr(inertial_node, "zaxis"), ictx);
  inertial["principalAxes"] = Json::array({iq[0], iq[1], iq[2], iq[3]});
  // An explicit zero tensor with a positive mass (MuJoCo accepts it) stays
  // zero instead of being re-derived from colliders.
  bool zero = HasAttr(inertial_node, "fullinertia") || HasAttr(inertial_node, "diaginertia");
  for (double v : full.size() >= 3 ? full : ParseDoubles(Attr(inertial_node, "diaginertia")))
    zero = zero && v == 0.0;
  if (zero && inertial["mass"].get<double>() > 0.0) inertial["zeroInertia"] = true;
  if (inertial["mass"].get<double>() == 0.0) inertial["zeroMass"] = true;
  return inertial;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
