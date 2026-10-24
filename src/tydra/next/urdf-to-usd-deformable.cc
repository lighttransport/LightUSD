// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
//
// MJCF flexcomp soft bodies as UsdPhysics deformables (AOUSD deformable
// proposal, as Newton's add_usd reads it): a 1D flex becomes a
// PhysicsCurvesDeformableSimAPI BasisCurves cable, 2D a
// PhysicsSurfaceDeformableSimAPI Mesh, 3D a PhysicsVolumeDeformableSimAPI
// TetMesh. Pinned vertices become a PhysicsAttachment to the owning body.
#include "urdf-to-usd-internal.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include <algorithm>
#include <cmath>
#include <map>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {
namespace {

double Number(const Json &j, const char *key, double fallback) {
  double v = fallback;
  ReadNumber(j, key, &v);
  return v;
}

bool Flag(const Json &j, const char *key, bool fallback) {
  bool v = fallback;
  ReadBool(j, key, &v);
  return v;
}

void SetFloat(tn::PrimSpec *prim, const std::string &name, double v) {
  Set(prim, name, tn::Value(static_cast<float>(v)), "float");
}

// Typed per-element array: float[] physics:<name> + token elementType.
void SetTyped(tn::PrimSpec *prim, const std::string &name,
              std::vector<float> values, const char *element_type) {
  Set(prim, name, tn::Value::MakeFloatArray(std::move(values)), "float[]");
  SetToken(prim, name + ":elementType", element_type, true);
}

}  // namespace

void AddDeformablesFromJson(tn::Layer *layer, const Json &items,
                            const NameMap &link_names, std::string *warn) {
  if (!items.is_array() || items.empty()) return;
  Define(layer, "/World/Deformables", "Scope");
  std::set<std::string> siblings;
  for (const Json &d : items) {
    const int dim = JsonInt(d, "dim", 2);
    const std::string name = ChildName(Sanitize(JsonString(d, "name", "flex"), "flex"), &siblings);
    const std::string path = "/World/Deformables/" + name;
    std::vector<float> points;
    if (const Json *pts = JsonArray(d, "points"))
      for (const Json &p : *pts)
        for (size_t k = 0; k < 3 && p.is_array() && k < p.size(); ++k)
          points.push_back(static_cast<float>(p[k].get<double>()));
    const size_t npnt = points.size() / 3;
    std::vector<int32_t> elements = JsonInts(d, "elements");
    const double radius = Number(d, "radius", 0.005);
    const double mass = Number(d, "mass", 1.0);
    const double young = Number(d, "young", 0.0);
    const double poisson = std::min(Number(d, "poisson", 0.0), 0.499);
    tn::PrimSpec *prim = nullptr;
    const char *sim_api = nullptr;
    const char *material_api = nullptr;
    if (dim == 1) {
      // MuJoCo builds 1D grids and circles as one ordered chain.
      bool chain = elements.size() == 2 * (npnt - (Flag(d, "periodic", false) ? 0 : 1));
      for (size_t e = 0; chain && e + 1 < elements.size(); e += 2)
        chain = elements[e] == static_cast<int32_t>(e / 2) &&
                elements[e + 1] == static_cast<int32_t>((e / 2 + 1) % npnt);
      if (!chain || npnt < 3) {
        AppendWarn(warn, "Skipping flex `" + name +
                             "`: a 1D soft body must be one chain of at least 3 points.\n");
        continue;
      }
      prim = Define(layer, path, "BasisCurves");
      SetToken(prim, "type", "linear", true);
      SetToken(prim, "wrap", Flag(d, "periodic", false) ? "periodic" : "nonperiodic", true);
      Set(prim, "curveVertexCounts", tn::Value::MakeIntArray({static_cast<int32_t>(npnt)}), "int[]");
      Set(prim, "widths", tn::Value::MakeFloatArray({static_cast<float>(2 * radius)}), "float[]");
      SetInterpolation(prim, "widths", "constant");
      sim_api = "PhysicsCurvesDeformableSimAPI";
      material_api = "PhysicsCurvesDeformableMaterialAPI";
    } else if (dim == 2) {
      prim = Define(layer, path, "Mesh");
      Set(prim, "faceVertexCounts",
          tn::Value::MakeIntArray(std::vector<int32_t>(elements.size() / 3, 3)), "int[]");
      Set(prim, "faceVertexIndices", tn::Value::MakeIntArray(elements), "int[]");
      SetToken(prim, "subdivisionScheme", "none", true);
      Set(prim, "doubleSided", tn::Value(true), "bool", true);
      // MuJoCo bends a shell relative to its initial shape, not a flat sheet.
      SetToken(prim, "physics:restBendAnglesDefault", "restShape", true);
      sim_api = "PhysicsSurfaceDeformableSimAPI";
      material_api = "PhysicsSurfaceDeformableMaterialAPI";
    } else {
      // Newton keeps only tets with positive signed volume (right-handed).
      std::map<std::array<int32_t, 3>, std::pair<int, std::array<int32_t, 3>>> faces;
      for (size_t t = 0; t + 3 < elements.size(); t += 4) {
        int32_t *v = &elements[t];
        auto p = [&](int i) { return &points[3 * size_t(v[i])]; };
        double a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) {
          a[k] = p(1)[k] - p(0)[k];
          b[k] = p(2)[k] - p(0)[k];
          c[k] = p(3)[k] - p(0)[k];
        }
        const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                           a[2] * (b[0] * c[1] - b[1] * c[0]);
        if (det < 0) std::swap(v[1], v[2]);
        // Boundary faces (seen once), outward for a positive tet.
        const int32_t tri[4][3] = {{v[0], v[2], v[1]}, {v[0], v[1], v[3]},
                                   {v[1], v[2], v[3]}, {v[0], v[3], v[2]}};
        for (const auto &f : tri) {
          std::array<int32_t, 3> key{{f[0], f[1], f[2]}};
          std::sort(key.begin(), key.end());
          auto &slot = faces[key];
          if (++slot.first == 1) slot.second = {{f[0], f[1], f[2]}};
        }
      }
      std::vector<int32_t> surface;
      for (const auto &f : faces)
        if (f.second.first == 1)
          surface.insert(surface.end(), f.second.second.begin(), f.second.second.end());
      prim = Define(layer, path, "TetMesh");
      Set(prim, "tetVertexIndices", tn::Value::MakeIntCompArray(elements, tn::TypeId::Int4, 4),
          "int4[]");
      Set(prim, "surfaceFaceVertexIndices",
          tn::Value::MakeIntCompArray(surface, tn::TypeId::Int3, 3), "int3[]");
      sim_api = "PhysicsVolumeDeformableSimAPI";
      material_api = "PhysicsVolumeDeformableMaterialAPI";
    }
    if (!prim) continue;
    Set(prim, "points", tn::Value::MakeFloatCompArray(points, tn::TypeId::Point3f, 3), "point3f[]");
    const std::vector<double> rgba = JsonDoubles(d, "rgba");
    if (rgba.size() >= 3) {
      Set(prim, "primvars:displayColor",
          tn::Value::MakeFloatCompArray({static_cast<float>(rgba[0]), static_cast<float>(rgba[1]),
                                         static_cast<float>(rgba[2])},
                                        tn::TypeId::Color3f, 3),
          "color3f[]");
      SetInterpolation(prim, "primvars:displayColor", "constant");
    }
    AddAPI(prim, sim_api);
    AddAPI(prim, "MaterialBindingAPI");
    if (Flag(d, "collides", true)) {
      AddAPI(prim, "PhysicsCollisionAPI");
      Set(prim, "physics:collisionEnabled", tn::Value(true), "bool");
    }
    // A raw flex carries its vertex bodies' masses; flexcomp mass is shared
    // equally by its points.
    std::vector<float> masses;
    for (double m : JsonDoubles(d, "masses")) masses.push_back(static_cast<float>(m));
    if (masses.size() != npnt)
      masses.assign(npnt, static_cast<float>(mass / std::max<size_t>(npnt, 1)));
    SetTyped(prim, "physics:masses", std::move(masses), "point");
    // 1D/2D thickness: the elastic shell thickness, else the contact diameter.
    double thickness = Number(d, "thickness", -1.0);
    if (dim != 2 || thickness <= 0.0) thickness = 2.0 * radius;
    if (dim < 3) SetTyped(prim, "physics:thicknesses", {static_cast<float>(thickness)}, "constant");
    if (dim == 1) {
      const std::string self = JsonString(d, "selfcollide", "auto");
      if (self == "none") Set(prim, "newton:selfCollisionEnabled", tn::Value(false), "bool");
    }

    tn::PrimSpec *material = Define(layer, path + "_material", "Material");
    AddAPIs(material, {"PhysicsMaterialAPI"});
    AddAPI(material, material_api);
    SetFloat(material, "physics:youngsModulus", young);
    SetFloat(material, "physics:poissonsRatio", poisson);
    double friction = 0.0;
    if (ReadNumber(d, "friction", &friction)) {
      SetFloat(material, "physics:staticFriction", friction);
      SetFloat(material, "physics:dynamicFriction", friction);
    }
    if (dim == 1) {
      // A MuJoCo 1D flex is a spring chain: edge stiffness k [N/m] and
      // damping c [N s/m] over rest length L give EA = k L and c L; it has no
      // bending or twisting resistance.
      double length = 0.0;
      for (size_t e = 0; e + 1 < elements.size(); e += 2) {
        const float *a = &points[3 * size_t(elements[e])], *b = &points[3 * size_t(elements[e + 1])];
        length += std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                            (a[2] - b[2]) * (a[2] - b[2]));
      }
      length /= std::max<size_t>(elements.size() / 2, 1);
      SetFloat(material, "physics:curvesStretchStiffness", Number(d, "edgeStiffness", 0.0) * length);
      SetFloat(material, "physics:curvesBendStiffness", 0.0);
      SetFloat(material, "physics:curvesTwistStiffness", 0.0);
      double damping = 0.0;
      if (ReadNumber(d, "edgeDamping", &damping)) {
        AddAPI(material, "NewtonCurvesDeformableMaterialAPI");
        SetFloat(material, "newton:curvesStretchDamping", damping * length);
      }
    } else if (dim == 2) {
      // MuJoCo's 2D elasticity acts only for the elastic2d modes it names.
      const std::string mode = JsonString(d, "elastic2d", "none");
      const double h = thickness, denom = 1.0 - poisson * poisson;
      const bool stretch = mode == "stretch" || mode == "both";
      const bool bend = mode == "bend" || mode == "both";
      SetFloat(material, "physics:surfaceStretchStiffness", stretch ? young * h / denom : 0.0);
      SetFloat(material, "physics:surfaceBendStiffness",
               bend ? young * h * h * h / (12.0 * denom) : 0.0);
    }
    SetRel(prim, "material:binding:physics", {path + "_material"});

    // Pinned vertices: one hard attachment per owning body (or the world),
    // in first-pin order. `pinBodies` (raw flex) names each pin's body;
    // otherwise `pinBody` owns them all.
    const std::vector<int32_t> pins = JsonInts(d, "pins");
    if (!pins.empty()) {
      std::vector<float> all_coords;
      if (const Json *c = JsonArray(d, "pinCoords"))
        for (const Json &p : *c)
          for (size_t k = 0; k < 3 && p.is_array() && k < p.size(); ++k)
            all_coords.push_back(static_cast<float>(p[k].get<double>()));
      std::vector<std::string> pin_bodies(pins.size(), JsonString(d, "pinBody"));
      if (const Json *b = JsonArray(d, "pinBodies"))
        for (size_t i = 0; i < pins.size() && i < b->size(); ++i)
          pin_bodies[i] = (*b)[i].is_string() ? (*b)[i].get<std::string>() : std::string();
      std::vector<std::string> order;
      for (const auto &b : pin_bodies)
        if (std::find(order.begin(), order.end(), b) == order.end()) order.push_back(b);
      for (size_t g = 0; g < order.size(); ++g) {
        const std::string &body = order[g];
        std::vector<int32_t> indices;
        std::vector<float> coords;
        for (size_t i = 0; i < pins.size(); ++i) {
          if (pin_bodies[i] != body) continue;
          indices.push_back(pins[i]);
          for (size_t k = 0; k < 3; ++k)
            coords.push_back(3 * i + k < all_coords.size() ? all_coords[3 * i + k] : 0.0f);
        }
        tn::PrimSpec *attachment = Define(
            layer, path + (g ? "_pins_" + std::to_string(g) : std::string("_pins")),
            "PhysicsAttachment");
        SetRel(attachment, "physics:src0", {path});
        auto link = link_names.find(body);
        if (!body.empty() && link != link_names.end())
          SetRel(attachment, "physics:src1", {"/World/Links/" + link->second});
        SetToken(attachment, "physics:type0", "point");
        SetToken(attachment, "physics:type1", "xform");
        Set(attachment, "physics:indices0", tn::Value::MakeIntArray(indices), "int[]");
        Set(attachment, "physics:coords1",
            tn::Value::MakeFloatCompArray(coords, tn::TypeId::Vector3f, 3), "vector3f[]");
        Set(attachment, "physics:attachmentEnabled", tn::Value(true), "bool");
      }
      if (dim > 1)
        AppendWarn(warn, "Flex `" + name + "` pins are authored as a PhysicsAttachment, which "
                         "Newton imports only for cables.\n");
    }
    if (dim > 1 && (Number(d, "damping", 0.0) > 0.0 || Number(d, "edgeDamping", 0.0) > 0.0))
      AppendWarn(warn, "Flex `" + name + "` damping has no UsdPhysics deformable attribute; "
                       "it is dropped.\n");
    if (!JsonString(d, "edgeEquality").empty() && JsonString(d, "edgeEquality") != "false")
      AppendWarn(warn, "Flex `" + name + "` edge equality constraints are not representable; "
                       "they are dropped.\n");
  }
}

}  // namespace urdf_detail
} } }  // namespace lightusd::tydra::next
