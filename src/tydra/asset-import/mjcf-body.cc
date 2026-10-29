// SPDX-License-Identifier: Apache-2.0
// Mass properties and the body/geom tree.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
// Bake one <geom> into a link's "visuals"/"collisions" arrays. Shared by the
// per-body traversal and the worldbody-level (static) geom collection.
// --- MuJoCo inertia inference (compiler inertiafromgeom) -------------------
// Ports mjCGeom::GetVolume/SetInertia and mjCMesh::ComputeVolume/Inertia so a
// body without <inertial> gets the mass properties MuJoCo's compiler derives
// from its geoms (inertiagrouprange, density default 1000, explicit mass).

// Rotation R (column vectors) and translation of a MatrixFromPosQuat matrix:
// p' = R p + t with R[i][j] = m[j * 4 + i].
void FrameRotation(const std::vector<double>& m, double r[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = m[j * 4 + i];
}

void RotateInertia(const double r[3][3], const double local[3], double out[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) sum += r[i][k] * local[k] * r[j][k];
      out[i][j] = sum;
    }
}

// Analytic volume/area and principal inertia per unit mass for primitives.
bool PrimitiveMassProperties(const std::string& type, const std::array<double, 3>& size,
                             bool shell, double* volume, double unit[3]) {
  constexpr double pi = 3.14159265358979323846;
  const double r = size[0];
  if (type == "sphere") {
    *volume = shell ? 4 * pi * r * r : 4 * pi * r * r * r / 3;
    unit[0] = unit[1] = unit[2] = (shell ? 2.0 / 3.0 : 2.0 / 5.0) * r * r;
  } else if (type == "capsule") {
    const double h = 2 * size[1];
    if (!shell) {
      *volume = pi * (r * r * h + 4 * r * r * r / 3);
      const double sphere = 4 * r / (4 * r + 3 * h), cylinder = 1 - sphere;
      unit[0] = unit[1] = cylinder * (3 * r * r + h * h) / 12 + 2 * sphere * r * r / 5 +
                          sphere * h * (3 * r + 2 * h) / 8;
      unit[2] = cylinder * r * r / 2 + 2 * sphere * r * r / 5;
    } else {
      const double as = 4 * pi * r * r, ac = 2 * pi * r * h;
      *volume = as + ac;
      const double sphere = as / (as + ac), cylinder = 1 - sphere;
      const double hs_com = r / 2, hs_pos = h / 2 + hs_com;
      unit[0] = unit[1] = cylinder * (6 * r * r + h * h) / 12 + 2 * sphere * r * r / 3 +
                          sphere * (hs_pos * hs_pos - hs_com * hs_com);
      unit[2] = cylinder * r * r + 2 * sphere * r * r / 3;
    }
  } else if (type == "cylinder") {
    const double h = 2 * size[1];
    if (!shell) {
      *volume = pi * r * r * h;
      unit[0] = unit[1] = (3 * r * r + h * h) / 12;
      unit[2] = r * r / 2;
    } else {
      const double ad = pi * r * r, ac = 2 * pi * r * h;
      *volume = 2 * ad + ac;
      const double disk = ad / *volume, cylinder = 1 - 2 * disk;
      unit[0] = unit[1] = cylinder * (6 * r * r + h * h) / 12 +
                          2 * (disk * r * r / 4 + disk * h * h / 4);
      unit[2] = cylinder * r * r + 2 * disk * r * r / 2;
    }
  } else if (type == "box" && !shell) {
    *volume = 8 * size[0] * size[1] * size[2];
    unit[0] = (size[1] * size[1] + size[2] * size[2]) / 3;
    unit[1] = (size[0] * size[0] + size[2] * size[2]) / 3;
    unit[2] = (size[0] * size[0] + size[1] * size[1]) / 3;
  } else if (type == "ellipsoid" && !shell) {
    *volume = 4 * pi * size[0] * size[1] * size[2] / 3;
    unit[0] = (size[1] * size[1] + size[2] * size[2]) / 5;
    unit[1] = (size[0] * size[0] + size[2] * size[2]) / 5;
    unit[2] = (size[0] * size[0] + size[1] * size[1]) / 5;
  } else {
    return false;  // shell boxes/ellipsoids, planes, hfields, sdf
  }
  return true;
}

// Volume (or area for shell), COM and inertia per unit density of a triangle
// mesh already transformed into the body frame (mjCMesh::Process).
bool MeshMassProperties(const std::vector<std::array<double, 3>>& v,
                        const std::vector<int32_t>& tri, const std::string& mode,
                        double* volume, std::array<double, 3>* com, double inert[3][3]) {
  const bool legacy = mode == "legacy", shell = mode == "shell";
  auto triangle = [&](size_t f, double normal[3], double center[3]) {
    const auto &a = v[tri[3 * f]], &b = v[tri[3 * f + 1]], &c = v[tri[3 * f + 2]];
    const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                   e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) {
      normal[i] = len > 0 ? n[i] / len : 0;
      center[i] = (a[i] + b[i] + c[i]) / 3;
    }
    return len / 2;
  };
  const size_t faces = tri.size() / 3;
  double facecen[3] = {0, 0, 0}, area = 0;
  for (size_t f = 0; f < faces; ++f) {
    double n[3], c[3];
    const double a = triangle(f, n, c);
    for (int i = 0; i < 3; ++i) facecen[i] += a * c[i];
    area += a;
  }
  if (!(area > 1e-15)) return false;
  for (double& x : facecen) x /= area;
  double total = 0, cm[3] = {0, 0, 0};
  for (size_t f = 0; f < faces; ++f) {
    double n[3], c[3];
    const double a = triangle(f, n, c);
    double w = a;
    if (!shell) {
      const double d[3] = {c[0] - facecen[0], c[1] - facecen[1], c[2] - facecen[2]};
      w = (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) * a / 3;
      if (legacy) w = std::abs(w);
    }
    total += w;
    for (int i = 0; i < 3; ++i) cm[i] += w * (c[i] * 0.75 + facecen[i] * 0.25);
  }
  if (!(total > 1e-15)) return false;
  for (int i = 0; i < 3; ++i) (*com)[i] = cm[i] / total;
  // Products of inertia about the COM (mjCMesh::ComputeInertia).
  const int k[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
  double P[6] = {0, 0, 0, 0, 0, 0}, measure = 0;
  for (size_t f = 0; f < faces; ++f) {
    std::array<double, 3> q[3];
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) q[j][i] = v[tri[3 * f + j]][i] - (*com)[i];
    double n[3], c[3];
    const double e1[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]};
    const double e2[3] = {q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2]};
    double nn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    const double a = len / 2;
    for (int i = 0; i < 3; ++i) {
      n[i] = len > 0 ? nn[i] / len : 0;
      c[i] = (q[0][i] + q[1][i] + q[2][i]) / 3;
    }
    double w = shell ? a : (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) * a / 3;
    if (legacy) w = std::abs(w);
    measure += w;
    const double C = shell ? 12 : 20;
    for (int j = 0; j < 6; ++j) {
      const int x = k[j][0], y = k[j][1];
      P[j] += w / C *
              (2 * (q[0][x] * q[0][y] + q[1][x] * q[1][y] + q[2][x] * q[2][y]) +
               q[0][x] * q[1][y] + q[0][y] * q[1][x] + q[0][x] * q[2][y] +
               q[0][y] * q[2][x] + q[1][x] * q[2][y] + q[1][y] * q[2][x]);
    }
  }
  *volume = measure;
  inert[0][0] = P[1] + P[2];
  inert[1][1] = P[0] + P[2];
  inert[2][2] = P[0] + P[1];
  inert[0][1] = inert[1][0] = -P[3];
  inert[0][2] = inert[2][0] = -P[4];
  inert[1][2] = inert[2][1] = -P[5];
  return measure > 1e-15;
}

// Mass properties of one geom in the body frame, or false when it contributes
// nothing (zero density/mass, unsupported shape).
bool GeomMassProperties(const pugi::xml_node& geom_node, const AttrMap& cls,
                        const Context& ctx, const std::map<std::string, MeshAsset>& assets,
                        const MeshPayload* mesh_payload, GeomMass* out) {
  const std::string type = HasEff(geom_node, cls, "type") ? Eff(geom_node, cls, "type")
                           : HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere";
  const bool has_mass = HasEff(geom_node, cls, "mass");
  const double mass_attr = EffDouble(geom_node, cls, "mass", 0.0);
  const double density = EffDouble(geom_node, cls, "density", 1000.0);
  const bool shell = MjcBoolAttr(Eff(geom_node, cls, "shellinertia", "false"));
  if ((has_mass && mass_attr <= 0) || (!has_mass && density <= 0)) return false;
  double volume = 0;
  if (type == "mesh") {
    if (!mesh_payload || mesh_payload->mesh.positions.empty()) return false;
    const auto it = assets.find(Eff(geom_node, cls, "mesh"));
    const std::string mode = it != assets.end() ? it->second.inertia : "legacy";
    const auto& m = mesh_payload->matrix;
    const auto& pos = mesh_payload->mesh.positions;
    std::vector<std::array<double, 3>> v(pos.size() / 3);
    for (size_t i = 0; i < v.size(); ++i) {
      const double x = pos[3 * i], y = pos[3 * i + 1], z = pos[3 * i + 2];
      v[i] = {{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
               m[2] * x + m[6] * y + m[10] * z + m[14]}};
    }
    std::vector<int32_t> tri;
    if (!MeshTriangles(mesh_payload->mesh, &tri)) return false;
    for (int32_t i : tri)
      if (i < 0 || size_t(i) >= v.size()) return false;
    // Left-handed scaling reverses the triangle orientation (MuJoCo flips
    // faces); keep exact volumes positive.
    const double det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                       m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (det < 0)
      for (size_t f = 0; f + 2 < tri.size(); f += 3) std::swap(tri[f + 1], tri[f + 2]);
    double unit[3][3];
    if (!MeshMassProperties(v, tri, mode == "shell" || shell ? "shell" : mode, &volume,
                            &out->com, unit))
      return false;
    if (mode == "convex" && active_reader)
      active_reader->Warn("MJCF convex mesh inertia approximated with the exact mesh volume",
                          "mjcf.inertia_convex_approximated");
    out->mass = has_mass ? mass_attr : density * volume;
    const double scale = out->mass / volume;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) out->inertia[i][j] = unit[i][j] * scale;
    return out->mass > 1e-15;
  }
  // Primitive frame: geom pos/orientation, or the fromto segment.
  std::array<double, 3> size{{0.05, 0.05, 0.05}};
  const auto sizes = ParseDoubles(Eff(geom_node, cls, "size"));
  for (size_t i = 0; i < 3 && i < sizes.size(); ++i) size[i] = sizes[i];
  if (type == "capsule" || type == "cylinder") {
    if (sizes.size() < 2) size[1] = size[0];
  } else if (type == "sphere") {
  } else if (type != "box" && type != "ellipsoid") {
    return false;
  }
  std::vector<double> frame = MatrixFromPosQuat(
      ParseDouble3(Eff(geom_node, cls, "pos"), {{0, 0, 0}}),
      OrientationQuat(Eff(geom_node, cls, "quat"), Eff(geom_node, cls, "axisangle"),
                      Eff(geom_node, cls, "euler"), Eff(geom_node, cls, "xyaxes"),
                      Eff(geom_node, cls, "zaxis"), ctx));
  const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
  if (HasEff(geom_node, cls, "mesh")) {
    MeshFit fit;
    std::string ignored;
    if (!FitMeshGeom(geom_node, cls, assets, type, &fit, &ignored)) return false;
    size = fit.size;
    frame = MultiplyMatrix(frame, fit.frame);
  } else if (fromto.size() >= 6 && type != "sphere") {
    const std::array<double, 3> dir{{fromto[3] - fromto[0], fromto[4] - fromto[1],
                                     fromto[5] - fromto[2]}};
    const double half = 0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    size[type == "capsule" || type == "cylinder" ? 1 : 2] = half;
    frame = MatrixFromPosQuat({{0.5 * (fromto[0] + fromto[3]), 0.5 * (fromto[1] + fromto[4]),
                                0.5 * (fromto[2] + fromto[5])}},
                              QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir));
  }
  double unit[3];
  if (!PrimitiveMassProperties(type, size, shell, &volume, unit) || !(volume > 1e-15))
    return false;
  out->mass = has_mass ? mass_attr : density * volume;
  const double local[3] = {unit[0] * out->mass, unit[1] * out->mass, unit[2] * out->mass};
  double r[3][3];
  FrameRotation(frame, r);
  RotateInertia(r, local, out->inertia);
  out->com = {{frame[12], frame[13], frame[14]}};
  return true;
}

// Combine geom mass properties about the body COM (parallel axis theorem).
Json CombineGeomMasses(const std::vector<GeomMass>& geoms) {
  double mass = 0, com[3] = {0, 0, 0};
  for (const auto& g : geoms) {
    mass += g.mass;
    for (int i = 0; i < 3; ++i) com[i] += g.mass * g.com[i];
  }
  if (!(mass > 1e-15)) return Json::object();
  for (double& c : com) c /= mass;
  double total[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (const auto& g : geoms) {
    const double d[3] = {g.com[0] - com[0], g.com[1] - com[1], g.com[2] - com[2]};
    const double d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        total[i][j] += g.inertia[i][j] + g.mass * ((i == j ? d2 : 0.0) - d[i] * d[j]);
  }
  return Json{{"mass", mass},
              {"centerOfMass", Json::array({com[0], com[1], com[2]})},
              {"fullInertia", Json::array({total[0][0], total[1][1], total[2][2], total[0][1],
                                           total[0][2], total[1][2]})},
              {"provenance", "inertiafromgeom"}};
}

bool AppendGeomToLink(const pugi::xml_node& geom_node, const std::string& cc,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world, Json* link,
                      Stats* stats, std::string* err,
                      bool dual_collider) {
  const AttrMap& cls = ResolveGeomClass(geom_node, ctx.defaults, cc);
  std::string cls_name = Attr(geom_node, "class");
  if (cls_name.empty()) cls_name = cc;
  if (opts.profile != "legacy") {
    // MuJoCo semantics: a geom collides iff contype|conaffinity is nonzero;
    // its group only selects visibility (0-2 visible by default). A
    // non-colliding geom outside the visible groups is kept as guide
    // geometry (it still contributes inertia in MuJoCo).
    const int contype = ToInt32(EffDouble(geom_node, cls, "contype", 1.0), "contype");
    const int conaffinity =
        ToInt32(EffDouble(geom_node, cls, "conaffinity", 1.0), "conaffinity");
    const int group = ToInt32(EffDouble(geom_node, cls, "group", 0.0), "group");
    const bool collides = contype != 0 || conaffinity != 0;
    const bool visible = group >= 0 && group < 3;
    const bool weighs = active_geom_masses && group >= ctx.inertia_group_min &&
                        group <= ctx.inertia_group_max;
    if (weighs) {
      // Mesh mass properties need the loaded triangles in the body frame;
      // primitives are analytic.
      MeshPayload mesh_payload;
      bool ignored = true;
      const bool mesh = Eff(geom_node, cls, "type", HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere") == "mesh";
      if (mesh && !BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                                    body_world, &mesh_payload, &ignored, err))
        return false;
      GeomMass mass;
      if (GeomMassProperties(geom_node, cls, ctx, assets, mesh ? &mesh_payload : nullptr, &mass))
        active_geom_masses->push_back(mass);
    }
    const std::string geom_name = Attr(geom_node, "name");
    const bool wrap = !geom_name.empty() && ctx.wrap_geoms.count(geom_name);
    const std::string geom_type =
        Eff(geom_node, cls, "type", HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere");
    // MuJoCo and Newton wrap tendons only around native spheres/cylinders. A
    // colliding wrap geom is referenced through its native collider.
    const bool native_wrap = wrap && !collides && (geom_type == "sphere" || geom_type == "cylinder");
    if (wrap && geom_type != "sphere" && geom_type != "cylinder" &&
        !UnsupportedXML(geom_node, "Tendon wrap geom `" + geom_name + "` must be a sphere or cylinder",
                        "mjcf.tendon_wrap_type", "mjcf.tendon_wrap", active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    // A visible collider is one prim carrying both roles (as
    // mujoco-usd-converter writes it, and MuJoCo's decoder reads one geom),
    // unless its collider and visual geometry differ (tessellated collision
    // shapes keep a native visual).
    const bool primitive_geom = geom_type != "mesh" && geom_type != "hfield";
    const bool merged = visible && collides && !(primitive_geom && opts.tessellate_collision_shapes);
    if (!merged && (visible || !collides)) {
      MeshPayload payload;
      bool ignored = true;
      // Primitive visuals stay native gprims (as mujoco-usd-converter writes
      // them); meshes and heightfields are triangles.
      const bool primitive = geom_type != "mesh" && geom_type != "hfield";
      if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                            body_world, &payload, &ignored, err, native_wrap, primitive))
        return false;
      Json visual_json = MeshPayloadToJson(payload);
      // MuJoCo's geom color (default 0.5 0.5 0.5 1); a material, when bound,
      // takes precedence in renderers.
      const auto rgba = ParseDoubles(Eff(geom_node, cls, "rgba", "0.5 0.5 0.5 1"));
      if (rgba.size() >= 4) visual_json["rgba"] = Array(rgba);
      if (native_wrap) visual_json["mjcGeom"] = geom_name;
      AddGeomPhysicsAttrs(geom_node, cls, &visual_json);
      // MuJoCo computes the mass of visual geoms too: keep a shell mass model
      // (a thin shell mesh has no usable volume).
      if (opts.profile != "legacy") {
        bool shell = MjcBoolAttr(Eff(geom_node, cls, "shellinertia", "false"));
        if (geom_type == "mesh") {
          const auto mesh_it = assets.find(Eff(geom_node, cls, "mesh"));
          shell = mesh_it != assets.end() && mesh_it->second.inertia == "shell";
        }
        if (shell) visual_json["newton"]["massModel"] = "shell";
      }
      const std::string mat = Eff(geom_node, cls, "material");
      if (!mat.empty()) visual_json["material"] = mat;
      if (!visible) visual_json["purpose"] = "guide";
      (*link)["visuals"].push_back(std::move(visual_json));
      if (stats) stats->visuals++;
    }
    if (collides) {
      MeshPayload collider;
      bool ignored = false;
      if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                            body_world, &collider, &ignored, err, true))
        return false;
      Json col = MeshPayloadToJson(collider);
      AddGeomPhysicsAttrs(geom_node, cls, &col);
      // MuJoCo collides every mesh through its convex hull; only a
      // tessellated heightfield keeps exact triangles.
      col["approximation"] = dual_collider && geom_type == "hfield" ? "none" : "convexHull";
      if (wrap) col["mjcGeom"] = geom_name;
      if (merged) {
        col["visual"] = true;
        const auto rgba = ParseDoubles(Eff(geom_node, cls, "rgba", "0.5 0.5 0.5 1"));
        if (rgba.size() >= 4) col["rgba"] = Array(rgba);
        const std::string mat = Eff(geom_node, cls, "material");
        if (!mat.empty()) col["material"] = mat;
        if (stats) stats->visuals++;
      }
      if (geom_type == "mesh" && opts.profile != "legacy" && HasAttr(geom_node, kSdfGeomMarker)) {
        // MuJoCo collides an sdf geom with the exact surface, not a hull.
        col["approximation"] = "none";
        col["newton"]["sdf"] = true;
        if (opts.sdf == "auto" && opts.profile == "newton") {
          Json sdf;
          std::string why;
          const std::string mesh_name = Eff(geom_node, cls, "mesh");
          if (MjcfSdfLevelSet(mesh_name, collider.mesh, &sdf, &why)) {
            // Newton's narrow band is in the shape's units: author it only
            // when the collider transform has no scale.
            bool unit = collider.matrix.size() == 16;
            for (int c = 0; unit && c < 3; ++c) {
              const double len = std::sqrt(collider.matrix[4 * c] * collider.matrix[4 * c] +
                                           collider.matrix[4 * c + 1] * collider.matrix[4 * c + 1] +
                                           collider.matrix[4 * c + 2] * collider.matrix[4 * c + 2]);
              unit = std::fabs(len - 1) < 1e-6;
            }
            sdf["unitScale"] = unit;
            col["sdf"] = std::move(sdf);
          } else {
            active_reader->Warn("MJCF sdf geom `" + geom_name + "`: no level set (" + why + ")",
                                "mjcf.sdf_level_set_failed");
          }
        }
      }
      if (geom_type == "mesh") {
        const auto mesh_it = assets.find(Eff(geom_node, cls, "mesh"));
        if (mesh_it != assets.end() && mesh_it->second.maxhullvert > 0) {
          col["newton"]["maxHullVertices"] = mesh_it->second.maxhullvert;
          col["mjc"]["maxhullvert"] = mesh_it->second.maxhullvert;
        }
        // <mesh inertia>: shell is newton:massModel, legacy/exact mjc:inertia,
        // convex the implied default (mujoco-usd-converter 0.8.0).
        if (opts.profile != "legacy")
          col["mjc"]["inertia"] = mesh_it != assets.end() ? mesh_it->second.inertia : "legacy";
      } else if (opts.profile != "legacy" &&
                 MjcBoolAttr(Eff(geom_node, cls, "shellinertia", "false"))) {
        col["newton"]["massModel"] = "shell";
      }
      (*link)["collisions"].push_back(std::move(col));
      if (stats) stats->collisions++;
    }
    return true;
  }
  MeshPayload payload;
  bool visual = true;
  if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                        body_world, &payload, &visual, err)) {
    return false;
  }
  if (visual) {
    Json visual_json = MeshPayloadToJson(payload);
    AddGeomPhysicsAttrs(geom_node, cls, &visual_json);
    const std::string mat = Eff(geom_node, cls, "material");
    if (!mat.empty()) visual_json["material"] = mat;
    (*link)["visuals"].push_back(std::move(visual_json));
    if (stats) stats->visuals++;
    // A world-fixed geom that is visible AND a collider (MuJoCo default
    // contype=conaffinity=1; non-collider only when BOTH are 0) is ALSO emitted
    // as a USD collider so floors/ground/hfield actually collide. The owning
    // link is static, so use an exact triangle-mesh collider (`none`) rather
    // than a convex hull, which would flatten terrain. Robot bodies keep the
    // group-based visual/collision split (they ship dedicated collision geoms).
    {
      const int contype =
          ToInt32(EffDouble(geom_node, cls, "contype", 1.0), "contype");
      const int conaffinity =
          ToInt32(EffDouble(geom_node, cls, "conaffinity", 1.0), "conaffinity");
      if (contype != 0 || conaffinity != 0) {
        MeshPayload collider = payload;
        const std::string geom_type = Eff(geom_node, cls, "type");
        if (geom_type != "mesh" && geom_type != "hfield" &&
            !HasEff(geom_node, cls, "mesh")) {
          bool ignored = false;
          if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts,
                                ctx, body_world, &collider, &ignored, err,
                                true))
            return false;
        }
        Json col = MeshPayloadToJson(collider);
        AddGeomPhysicsAttrs(geom_node, cls, &col);
        col["approximation"] = dual_collider ? "none" : "convexHull";
        (*link)["collisions"].push_back(std::move(col));
        if (stats) stats->collisions++;
      }
    }
  } else {
    Json col = MeshPayloadToJson(payload);
    AddGeomPhysicsAttrs(geom_node, cls, &col);
    // Default approximation `convexHull` matches
    // `src/tydra/urdf-to-usd.cc::AddCollisionAPIs` and the
    // mujoco-usd-converter convention (one Mesh per geom +
    // `UsdPhysicsMeshCollisionAPI` with hull approximation).
    col["approximation"] = "convexHull";
    (*link)["collisions"].push_back(std::move(col));
    if (stats) stats->collisions++;
  }
  return true;
}

// Geoms that are direct children of <worldbody> (e.g. a ground/floor plane or
// heightfield) are world-fixed and belong to no body. Collect them onto a
// single static root link named "world" so they still convert. Accumulates
// across every <worldbody> block (post-<include> merge). World-fixed colliders
// are emitted as both a render mesh and a (triangle) collider via
// dual_collider.
bool AddWorldbodyGeomsLink(const std::vector<pugi::xml_node>& worldbodies,
                           const std::map<std::string, MeshAsset>& assets,
                           const std::map<std::string, HFieldAsset>& hfields,
                           const Options& opts, const Context& ctx, Json* links,
                           Stats* stats, std::string* err) {
  Json link = {{"name", active_world_name + "_geoms"},
               {"static", true},
               {"inertial", {{"mass", 0.0}}},
               {"visuals", Json::array()},
               {"collisions", Json::array()}};
  for (const auto& worldbody : worldbodies) {
    for (const auto& geom_node : Children(worldbody, "geom")) {
      std::string gerr;
      if (!AppendGeomToLink(geom_node, "", assets, hfields, opts, ctx,
                            IdentityMatrix(), &link, stats, &gerr,
                            /*dual_collider=*/true)) {
        // Match the per-body geom path: a build failure is fatal unless the
        // caller opted into --allow-missing.
        if (!opts.allow_missing) {
          if (err) *err = gerr.empty() ? "worldbody geom failed" : gerr;
          return false;
        }
        active_reader->Warn(gerr.empty() ? "worldbody geom skipped" : gerr);
      }
    }
  }
  if (link["visuals"].empty() && link["collisions"].empty()) return true;
  links->push_back(std::move(link));
  if (stats) stats->links++;
  return true;
}

// One body: its link, geoms and joints. Appends its child bodies (in order)
// to *children instead of recursing.
bool VisitMujocoBodyOne(const pugi::xml_node& body_node,
                        const std::string& parent_name,
                        const std::string& childclass,
                        const std::vector<double>& parent_world,
                        const std::map<std::string, MeshAsset>& assets,
                        const std::map<std::string, HFieldAsset>& hfields,
                        const Options& opts, const Context& ctx, Json* links,
                        Json* joints, Stats* stats, std::vector<MujocoBodyJob>* children,
                        std::string* err) {
  // A body's own `childclass` becomes the default class for its descendant
  // geoms/joints/bodies, unless overridden deeper.
  const std::string cc = HasAttr(body_node, "childclass")
                             ? Attr(body_node, "childclass")
                             : childclass;
  // Accumulate the body-chain world transform: this body's frame relative to
  // its parent, composed onto the parent's world frame.
  if (active_flex_bodies && active_flex_bodies->count(body_node.internal_object())) return true;
  const std::vector<double> body_world =
      MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
  const std::string body_name =
      Attr(body_node, "name", "body_" + std::to_string(stats->links));
  active_body_names[body_node.internal_object()] = body_name;
  // A <freejoint> (or <joint type="free">) makes this body a 6-DOF floating
  // base: it is NOT joined to its parent. Mark it so the converter emits a free
  // articulation root (vs a fixed base, which has no such joint).
  bool has_free = static_cast<bool>(Child(body_node, "freejoint"));
  if (!has_free) {
    for (const auto& jn : Children(body_node, "joint")) {
      if (ToLower(Attr(jn, "type")) == "free") {
        has_free = true;
        break;
      }
    }
  }
  Json link = {{"name", body_name},
               {"inertial", InertialToJson(body_node, ctx)},
               {"matrix", Array(body_world)},
               {"visuals", Json::array()},
               {"collisions", Json::array()}};
  if (has_free) link["floating"] = true;
  if (parent_name == active_world_name || has_free)
    link["articulationRoot"] = true;
  // MuJoCo mocap body (driven externally; not simulated). Flag it so the USD
  // link records mjc:mocap.
  if (MjcBoolAttr(Attr(body_node, "mocap", "false"))) link["mocap"] = true;
  if (HasAttr(body_node, "gravcomp"))
    link["gravcomp"] = ParseDoubleAttr(body_node, "gravcomp", 0.0);

  // MuJoCo infers the inertia of a body without <inertial> (or of every body
  // with inertiafromgeom="true") from its geoms. Non-legacy profiles author
  // that compiled result so engines need not recompute it.
  std::vector<GeomMass> geom_masses;
  const bool explicit_inertial = static_cast<bool>(Child(body_node, "inertial"));
  const bool infer = opts.profile != "legacy" && ctx.inertiafromgeom != "false" &&
                     (!explicit_inertial || ctx.inertiafromgeom == "true");
  struct MassScope {
    std::vector<GeomMass>* previous = active_geom_masses;
    explicit MassScope(std::vector<GeomMass>* m) { active_geom_masses = m; }
    ~MassScope() { active_geom_masses = previous; }
  } mass_scope(infer ? &geom_masses : nullptr);
  for (const auto& geom_node : Children(body_node, "geom")) {
    if (!AppendGeomToLink(geom_node, cc, assets, hfields, opts, ctx,
                          IdentityMatrix(), &link, stats, err)) {
      if (opts.allow_missing) {
        active_reader->Warn(err ? *err : "mesh skipped");
        if (err) err->clear();
        continue;
      }
      return false;
    }
  }

  if (infer) {
    Json inferred = CombineGeomMasses(geom_masses);
    if (!inferred.empty()) {
      link["inertial"] = std::move(inferred);
      if (active_reader->options.report) {
        auto& report = *active_reader->options.report;
        auto& count = report.counts["inferred_inertia_bodies"];
        count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + 1;
      }
    }
  }
  links->push_back(std::move(link));
  stats->links++;
  if (!parent_name.empty() && !has_free) {
    const std::vector<double> body_matrix = PoseMatrix(body_node, ctx);
    const std::array<double, 3> body_pos =
        ParseDouble3(Attr(body_node, "pos"), {{0.0, 0.0, 0.0}});
    const std::vector<double> identity = IdentityMatrix();
    const std::array<double, 3> zero_pos = {{0.0, 0.0, 0.0}};
    const std::vector<pugi::xml_node> joint_nodes =
        Children(body_node, "joint");

    // Newton merges a stack into one D6 and accepts only hinges and slides
    // there; a stack with a ball keeps the intermediate-link chain.
    bool mergeable = true;
    for (const auto& jn : joint_nodes) {
      const std::string jtype = Eff(jn, ResolveJointClass(jn, ctx.defaults, cc), "type", "hinge");
      mergeable &= jtype == "hinge" || jtype == "slide";
    }
    if (joint_nodes.size() > 1 && active_reader->options.profile != "legacy" &&
        (mergeable || active_reader->options.profile != "newton")) {
      // Stacked joints: every DOF connects parent and body directly, each in
      // the body frame at its own pos, composed in authored order. This is
      // MuJoCo's own description (mujoco-usd-converter, MuJoCo's USD decoder)
      // and Newton merges joints sharing a body pair into one D6 joint. The
      // root records it with newton:jointsAddMobility.
      for (const auto& jn : joint_nodes) {
        const AttrMap& jcls = ResolveJointClass(jn, ctx.defaults, cc);
        AddJointFromNode(jn, jcls, ctx, parent_name, body_name, body_matrix,
                         body_pos, joints);
        stats->joints++;
      }
      if (stacked_joint_bodies) stacked_joint_bodies->insert(body_name);
    } else if (joint_nodes.size() <= 1) {
      // 0 joints -> fixed connection; 1 joint -> single DOF (the common case).
      const pugi::xml_node jn =
          joint_nodes.empty() ? pugi::xml_node() : joint_nodes[0];
      const AttrMap& jcls = ResolveJointClass(jn, ctx.defaults, cc);
      AddJointFromNode(jn, jcls, ctx, parent_name, body_name, body_matrix,
                       body_pos, joints);
      stats->joints++;
    } else {
      // MuJoCo allows multiple <joint> per body (e.g. ball+slide, or a stack of
      // hinges) — they compose in sequence. Represent the N DOFs as a chain of
      // (N-1) massless intermediate link Xforms joined by single-DOF joints:
      //   parent --j0--> dof_1 --j1--> ... --j(N-1)--> body
      // Only the first joint carries the body offset; intermediates sit at the
      // body frame (identity). Geometry is world-baked on `body`, so the empty
      // intermediate links add no visible geometry.
      std::string prev = parent_name;
      for (size_t k = 0; k < joint_nodes.size(); k++) {
        const bool last = (k + 1 == joint_nodes.size());
        const std::string child =
            last ? body_name
                 : (body_name + "__mjcdof_" + std::to_string(k + 1));
        if (!last) {
          // Emit a massless intermediate link (no geometry, no mass).
          Json dof_link = {{"name", child},
                           {"inertial", {{"mass", 0.0}}},
                           {"matrix", Array(body_world)},
                           {"visuals", Json::array()},
                           {"collisions", Json::array()}};
          links->push_back(std::move(dof_link));
          stats->links++;
        }
        const AttrMap& jcls =
            ResolveJointClass(joint_nodes[k], ctx.defaults, cc);
        AddJointFromNode(joint_nodes[k], jcls, ctx, prev, child,
                         (k == 0) ? body_matrix : identity,
                         (k == 0) ? body_pos : zero_pos, joints);
        stats->joints++;
        prev = child;
      }
    }
  }

  for (const auto& child_body : Children(body_node, "body"))
    children->push_back({child_body, body_name, cc, body_world});
  return true;
}

// A body subtree in preorder (links and joints in document order), with an
// explicit stack: body chains can be as deep as the input allows.
bool VisitMujocoBody(const pugi::xml_node& body_node,
                     const std::string& parent_name,
                     const std::string& childclass,
                     const std::vector<double>& parent_world,
                     const std::map<std::string, MeshAsset>& assets,
                     const std::map<std::string, HFieldAsset>& hfields,
                     const Options& opts, const Context& ctx, Json* links,
                     Json* joints, Stats* stats, std::string* err) {
  std::vector<MujocoBodyJob> stack{{body_node, parent_name, childclass, parent_world}};
  std::vector<MujocoBodyJob> children;
  while (!stack.empty()) {
    const MujocoBodyJob job = std::move(stack.back());
    stack.pop_back();
    children.clear();
    if (!VisitMujocoBodyOne(job.node, job.parent_name, job.childclass, job.parent_world, assets,
                            hfields, opts, ctx, links, joints, stats, &children, err))
      return false;
    for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(std::move(*it));
  }
  return true;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
