// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
#include "urdf-to-usd-internal.hh"
#include <cstdio>
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {

constexpr int32_t kMjcfDefaultCondim = 3;
constexpr double kMjcfDefaultSolmix = 1.0;
constexpr double kMjcfDefaultMargin = 0.0;
constexpr double kMjcfDefaultGap = 0.0;
constexpr int32_t kLegacyUrdfCollisionGroup = 3;
constexpr int32_t kLegacyUrdfVisualGroup = 2;
constexpr int32_t kDefaultNewtonMaxHullVertices = 64;


namespace {
thread_local PhysicsMaterials *active_physics_materials = nullptr;

std::string StringFrom(const Json &src, const char *object_key, const char *key,
                       const char *fallback) {
  const Json *object = JsonObject(src, object_key);
  return object ? JsonString(*object, key, fallback) : std::string(fallback);
}

std::string FrictionKey(const std::vector<double> &mu) {
  std::string key;
  char buffer[32];
  for (double v : mu) {
    std::snprintf(buffer, sizeof(buffer), "%.17g,", v);
    key += buffer;
  }
  return key;
}
}  // namespace

PhysicsMaterials::PhysicsMaterials(tn::Layer *layer)
    : layer_(layer), previous_(active_physics_materials) {
  active_physics_materials = this;
}
PhysicsMaterials::~PhysicsMaterials() { active_physics_materials = previous_; }

std::string PhysicsMaterials::Bind(tn::PrimSpec *collider,
                                   std::vector<double> friction,
                                   const std::vector<double> &solref) {
  // MuJoCo geom friction defaults to (1, 0.005, 0.0001); a partial triplet
  // keeps the remaining defaults.
  static const double kDefault[3] = {1.0, 0.005, 0.0001};
  const size_t authored = friction.size();
  friction.resize(3);
  for (size_t i = 0; i < 3; ++i)
    if (i >= authored || !std::isfinite(friction[i])) friction[i] = kDefault[i];
  // Contact stiffness/damping from solref (default 0.02 1), as Newton's
  // solref_to_stiffness_damping: (timeconst, dampratio) -> (1/(t^2 d^2), 2/t),
  // both negative -> direct (-k, -b). Other values have no ke/kd.
  const double timeconst = solref.size() > 0 ? solref[0] : 0.02;
  const double dampratio = solref.size() > 1 ? solref[1] : 1.0;
  bool stiffness = true;
  double ke = 0.0, kd = 0.0;
  if (timeconst < 0.0 && dampratio < 0.0) {
    ke = -timeconst;
    kd = -dampratio;
  } else if (timeconst > 0.0 && dampratio > 0.0) {
    ke = 1.0 / (timeconst * timeconst * dampratio * dampratio);
    kd = 2.0 / timeconst;
  } else {
    stiffness = false;
  }
  if (!std::isfinite(ke) || !std::isfinite(kd)) stiffness = false;
  std::vector<double> key_values = friction;
  if (stiffness) {
    key_values.push_back(ke);
    key_values.push_back(kd);
  }
  const std::string key = FrictionKey(key_values);
  auto found = by_key_.find(key);
  std::string path;
  if (found != by_key_.end()) {
    path = found->second;
  } else {
    if (!defined_) {
      Define(layer_, "/World/PhysicsMaterials", "Scope");
      defined_ = true;
    }
    path = "/World/PhysicsMaterials/PhysicsMaterial" +
           (by_key_.empty() ? std::string() : "_" + std::to_string(by_key_.size()));
    tn::PrimSpec *material = Define(layer_, path, "Material");
    if (!material) return {};
    AddAPIs(material, {"PhysicsMaterialAPI", "MjcMaterialAPI", "NewtonMaterialAPI"});
    // MuJoCo's single sliding coefficient bounds the friction cone whether a
    // contact sticks or slips: static == dynamic (mujoco-usd-converter 0.4.1).
    Set(material, "physics:staticFriction",
        tn::Value(static_cast<float>(friction[0])), "float");
    Set(material, "physics:dynamicFriction",
        tn::Value(static_cast<float>(friction[0])), "float");
    Set(material, "newton:torsionalFriction",
        tn::Value(static_cast<float>(friction[1])), "float");
    Set(material, "newton:rollingFriction",
        tn::Value(static_cast<float>(friction[2])), "float");
    if (stiffness) {
      Set(material, "newton:contactStiffness", tn::Value(static_cast<float>(ke)), "float");
      Set(material, "newton:contactDamping", tn::Value(static_cast<float>(kd)), "float");
    }
    by_key_.emplace(key, path);
  }
  AddAPI(collider, "MaterialBindingAPI");
  SetRel(collider, "material:binding:physics", {path});
  return path;
}

PhysicsMaterials *ActivePhysicsMaterials() { return active_physics_materials; }

namespace {
thread_local CollisionFilterGroups *active_collision_groups = nullptr;
}  // namespace

CollisionFilterGroups::CollisionFilterGroups() : previous_(active_collision_groups) {
  active_collision_groups = this;
}
CollisionFilterGroups::~CollisionFilterGroups() { active_collision_groups = previous_; }
CollisionFilterGroups *ActiveCollisionFilterGroups() { return active_collision_groups; }

void CollisionFilterGroups::Record(const std::string &collider, int32_t contype,
                                   int32_t conaffinity) {
  members_[{contype, conaffinity}].push_back(collider);
}

size_t CollisionFilterGroups::Write(tn::Layer *layer) const {
  auto collides = [](const std::pair<int32_t, int32_t> &a, const std::pair<int32_t, int32_t> &b) {
    return (a.first & b.second) != 0 || (b.first & a.second) != 0;
  };
  bool filtered = false;
  for (const auto &a : members_)
    for (const auto &b : members_) filtered = filtered || !collides(a.first, b.first);
  if (!filtered) return 0;
  Define(layer, "/World/CollisionGroups", "Scope");
  std::map<std::pair<int32_t, int32_t>, std::string> paths;
  for (const auto &entry : members_)
    paths[entry.first] = "/World/CollisionGroups/contype_" + std::to_string(entry.first.first) +
                         "_conaffinity_" + std::to_string(entry.first.second);
  for (const auto &entry : members_) {
    tn::PrimSpec *group = Define(layer, paths[entry.first], "PhysicsCollisionGroup");
    if (!group) continue;
    SetRel(group, "collection:colliders:includes", entry.second);
    std::vector<std::string> filters;
    for (const auto &other : members_)
      if (!collides(entry.first, other.first)) filters.push_back(paths[other.first]);
    if (!filters.empty()) SetRel(group, "physics:filteredGroups", filters);
  }
  return members_.size();
}

void AddCollisionAPIs(tn::PrimSpec *prim, bool mesh_collision,
                      const Json &src, bool mjcf_source) {
  const Profile &profile = ActiveProfile();
  AddAPIs(prim, {"PhysicsCollisionAPI", "MjcCollisionAPI", "MjcImageableAPI",
                 "NewtonCollisionAPI"});
  // MJCF sdf geoms: Newton builds an SDF from the mesh. NewtonSDFCollisionAPI
  // and NewtonMeshCollisionAPI are alternative representations (Newton warns
  // when both are applied).
  bool sdf = false;
  if (const Json *newton = JsonObject(src, "newton"))
    if (mesh_collision && !profile.legacy) ReadBool(*newton, "sdf", &sdf);
  if (mesh_collision) {
    AddAPIs(prim, {"PhysicsMeshCollisionAPI", "MjcMeshCollisionAPI"});
    AddAPI(prim, sdf ? "NewtonSDFCollisionAPI" : "NewtonMeshCollisionAPI");
  }
  Set(prim, "physics:collisionEnabled", tn::Value(true), "bool");
  if (mesh_collision) {
    // Default approximation is `convexHull`, matching mujoco-usd-converter.
    SetToken(prim, "physics:approximation",
             JsonString(src, "approximation", "convexHull"), true);
    const std::string inertia =
        profile.legacy ? "legacy" : StringFrom(src, "mjc", "inertia", "legacy");
    if (inertia == "shell") {
      AddAPI(prim, "NewtonMassAPI");
      SetToken(prim, "newton:massModel", "shell", true);
    } else if (inertia == "legacy" || inertia == "exact") {
      // MuJoCo's decoder infers convex without it.
      SetToken(prim, "mjc:inertia", inertia, true);
    }
  } else if (!profile.legacy && StringFrom(src, "newton", "massModel", "") == "shell") {
    AddAPI(prim, "NewtonMassAPI");
    SetToken(prim, "newton:massModel", "shell", true);
  }
  if (mjcf_source) {
    int32_t ivalue = 0;
    double dvalue = 0.0;
    if (ReadIntFrom(src, "mjc", "group", &ivalue)) {
      Set(prim, "mjc:group", tn::Value(ivalue), "int", true);
    } else if (src.contains("group")) {
      Set(prim, "mjc:group", tn::Value(JsonInt(src, "group", 0)), "int", true);
    }
    if (ReadIntFrom(src, "mjc", "condim", &ivalue)) {
      Set(prim, "mjc:condim", tn::Value(ivalue), "int", true);
    } else if (src.contains("condim")) {
      Set(prim, "mjc:condim",
          tn::Value(JsonInt(src, "condim", kMjcfDefaultCondim)), "int", true);
    }
    // mjc:geomContype/geomConaffinity are historical aliases of the
    // mjc:contype/conaffinity bits authored below (which Newton reads).
    if (profile.legacy && (ReadIntFrom(src, "mjc", "geomContype", &ivalue) ||
                           ReadIntFrom(src, "mjc", "contype", &ivalue))) {
      Set(prim, "mjc:geomContype", tn::Value(ivalue), "int", true);
    }
    if (profile.legacy && (ReadIntFrom(src, "mjc", "geomConaffinity", &ivalue) ||
                           ReadIntFrom(src, "mjc", "conaffinity", &ivalue))) {
      Set(prim, "mjc:geomConaffinity", tn::Value(ivalue), "int", true);
    }
    if (ReadIntFrom(src, "mjc", "priority", &ivalue)) {
      Set(prim, "mjc:priority", tn::Value(ivalue), "int", true);
    }
    if (ReadNumberFrom(src, "mjc", "solmix", &dvalue)) {
      Set(prim, "mjc:solmix", tn::Value(dvalue), "double", true);
    }
    if (ReadNumberFrom(src, "mjc", "margin", &dvalue)) {
      // mjc:margin/gap are deprecated aliases of newton:contactMargin/Gap.
      if (profile.legacy) Set(prim, "mjc:margin", tn::Value(dvalue), "double", true);
      Set(prim, "newton:contactMargin",
          tn::Value(static_cast<float>(dvalue)), "float");
    } else if (ReadNumberFrom(src, "newton", "contactMargin", &dvalue)) {
      Set(prim, "newton:contactMargin",
          tn::Value(static_cast<float>(dvalue)), "float");
    }
    if (ReadNumberFrom(src, "mjc", "gap", &dvalue)) {
      if (profile.legacy) Set(prim, "mjc:gap", tn::Value(dvalue), "double", true);
      Set(prim, "newton:contactGap", tn::Value(static_cast<float>(dvalue)),
          "float");
    } else if (ReadNumberFrom(src, "newton", "contactGap", &dvalue)) {
      Set(prim, "newton:contactGap", tn::Value(static_cast<float>(dvalue)),
          "float");
    }
    // An unauthored MJCF margin/gap is MuJoCo's 0, which mujoco-usd-converter
    // writes too; left unauthored, Newton would apply its builder default
    // (rigid_gap = 0.1 m).
    if (!profile.legacy) {
      if (!prim->property("newton:contactMargin"))
        Set(prim, "newton:contactMargin", tn::Value(0.0f), "float");
      if (!prim->property("newton:contactGap"))
        Set(prim, "newton:contactGap", tn::Value(0.0f), "float");
    }
    std::vector<double> values = JsonDoublesFrom(src, "mjc", "geomFriction");
    if (values.empty()) values = JsonDoublesFrom(src, "mjc", "friction");
    if (profile.legacy) {
      if (!values.empty()) SetDoubles(prim, "mjc:geomFriction", values, true);
    } else if (PhysicsMaterials *materials = ActivePhysicsMaterials()) {
      // Friction is a bound physics material: the only place UsdPhysics,
      // Newton and MuJoCo's USD decoder read it.
      materials->Bind(prim, values, JsonDoublesFrom(src, "mjc", "solref"));
    }
    values = JsonDoublesFrom(src, "mjc", "solref");
    if (!values.empty()) SetDoubles(prim, "mjc:solref", values, true);
    values = JsonDoublesFrom(src, "mjc", "solimp");
    if (!values.empty()) SetDoubles(prim, "mjc:solimp", values, true);
    values = JsonDoublesFrom(src, "mjc", "geomSize");
    if (!values.empty() && profile.legacy)
      SetDoubles(prim, "mjc:geomSize", values, true);
    if (!profile.legacy) {
      double mass = 0.0, density = 0.0;
      const bool has_mass = ReadNumberFrom(src, "mjc", "mass", &mass);
      const bool has_density = ReadNumberFrom(src, "mjc", "density", &density);
      if (has_mass || has_density) AddAPI(prim, "PhysicsMassAPI");
      if (has_mass) Set(prim, "physics:mass", tn::Value(static_cast<float>(mass)), "float");
      if (has_density)
        Set(prim, "physics:density", tn::Value(static_cast<float>(density)), "float");
    }
  } else {
    Set(prim, "mjc:group",
        tn::Value(JsonInt(src, "group", kLegacyUrdfCollisionGroup)), "int",
        true);
    Set(prim, "mjc:condim",
        tn::Value(JsonInt(src, "condim", kMjcfDefaultCondim)), "int", true);
    Set(prim, "mjc:solmix",
        tn::Value(JsonNumberFrom(src, "mjc", "solmix", kMjcfDefaultSolmix)),
        "double", true);
    const double margin =
        JsonNumberFrom(src, "mjc", "margin", kMjcfDefaultMargin);
    if (profile.legacy) Set(prim, "mjc:margin", tn::Value(margin), "double", true);
    // URDF has no contact margin/gap; an unauthored value keeps Newton's
    // builder defaults (as its URDF importer does) instead of forcing 0.
    double value = 0.0;
    if (profile.legacy || ReadNumberFrom(src, "newton", "contactMargin", &value))
      Set(prim, "newton:contactMargin",
          tn::Value(static_cast<float>(
              JsonNumberFrom(src, "newton", "contactMargin", margin))),
          "float");
    if (profile.legacy || ReadNumberFrom(src, "newton", "contactGap", &value))
      Set(prim, "newton:contactGap",
          tn::Value(static_cast<float>(
              JsonNumberFrom(src, "newton", "contactGap", kMjcfDefaultGap))),
          "float");
  }
  if (mesh_collision) {
    int32_t hull = 0;
    // Non-legacy profiles leave an unlimited MJCF hull unauthored (MuJoCo's
    // decoder would otherwise cap it at the authored value).
    if (profile.legacy || ReadIntFrom(src, "newton", "maxHullVertices", &hull))
      Set(prim, "newton:maxHullVertices",
          tn::Value(JsonIntFrom(src, "newton", "maxHullVertices",
                                kDefaultNewtonMaxHullVertices)),
          "int", true);
  }
  // next-only: MuJoCo contact filter bits under their MJCF attribute names
  // (the legacy converter only authors mjc:geomContype/geomConaffinity).
  const Json *mjc = JsonObject(src, "mjc");
  auto contact_bits = [&](const char *key) {
    double fallback = 1.0;
    ReadNumber(src, key, &fallback);
    double value = fallback;
    if (mjc) ReadNumber(*mjc, key, &value);
    if (!(value >= double(INT32_MIN) && value <= double(INT32_MAX))) return int32_t(1);
    return static_cast<int32_t>(value);
  };
  Set(prim, "mjc:contype", tn::Value(contact_bits("contype")), "int", true);
  Set(prim, "mjc:conaffinity", tn::Value(contact_bits("conaffinity")), "int",
      true);
  if (CollisionFilterGroups *groups = mjcf_source ? ActiveCollisionFilterGroups() : nullptr)
    groups->Record(prim->path().str(), contact_bits("contype"),
                   contact_bits("conaffinity"));
  // `purpose=guide` hides the collider from default renders but keeps it
  // discoverable to schema-aware consumers (mujoco-usd-converter applies
  // the same to non-visual MuJoCo geom groups). A collider that is also the
  // geom's visual stays renderable and binds its look material.
  bool visual = false;
  if (!ReadBool(src, "visual", &visual) || !visual) {
    SetToken(prim, "purpose", "guide", true);
    return;
  }
  const std::string material = JsonString(src, "material");
  if (!material.empty()) {
    AddAPI(prim, "MaterialBindingAPI");
    SetRel(prim, "material:binding", {"/World/Materials/" + Sanitize(material, "material")});
  }
}

// MJCF geom rgba -> constant displayColor/displayOpacity (as
// mujoco-usd-converter writes them).
void SetDisplayColorFromRGBA(tn::PrimSpec *prim, const Json &source) {
  const std::vector<float> rgba = JsonFloats(source, "rgba");
  if (rgba.size() < 4) return;
  Set(prim, "primvars:displayColor",
      tn::Value::MakeFloatCompArray({rgba[0], rgba[1], rgba[2]}, tn::TypeId::Color3f, 3),
      "color3f[]");
  SetInterpolation(prim, "primvars:displayColor", "constant");
  Set(prim, "primvars:displayOpacity", tn::Value::MakeFloatArray({rgba[3]}), "float[]");
  SetInterpolation(prim, "primvars:displayOpacity", "constant");
}

bool AddMeshFromJson(tn::Layer *layer, const std::string &link_path,
                     std::set<std::string> *siblings, const Json &mesh_json,
                     const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
                     const std::string &fallback_name, bool collision,
                     bool mjcf_source, std::string *warn, std::string *err) {
  const Json *geom = JsonObject(mesh_json, "geometry");
  if (!geom) geom = &mesh_json;
  std::vector<float> json_positions;
  std::vector<float> json_normals;
  std::vector<float> json_uvs;
  std::vector<int32_t> json_indices;
  const std::vector<float> *positions = &json_positions;
  const std::vector<float> *normals = &json_normals;
  const std::vector<float> *uvs = &json_uvs;
  const std::vector<int32_t> *source_indices = &json_indices;
  const std::string mesh_ref = JsonString(mesh_json, "meshRef");
  if (!mesh_ref.empty()) {
    auto found = mesh_buffers ? mesh_buffers->find(mesh_ref)
                              : std::map<std::string, URDFMeshBuffer>::const_iterator();
    if (!mesh_buffers || found == mesh_buffers->end()) {
      AppendWarn(warn, "Skipping mesh `" + fallback_name + "`: meshRef `" +
                           mesh_ref + "` was not registered.\n");
      return true;
    }
    positions = &found->second.positions;
    normals = &found->second.normals;
    uvs = &found->second.uvs;
    source_indices = &found->second.indices;
  } else {
    json_positions = JsonFloats(*geom, "positions");
    json_normals = JsonFloats(*geom, "normals");
    json_uvs = JsonFloats(*geom, "uvs");
    json_indices = JsonInts(*geom, "indices");
  }
  if (positions->size() < 9 || positions->size() % 3 != 0) {
    AppendWarn(warn, "Skipping mesh `" + fallback_name +
                         "`: positions must contain at least 3 points.\n");
    return true;
  }
  const size_t point_count = positions->size() / 3;
  std::vector<int32_t> indices;
  if (!source_indices->empty()) {
    indices = *source_indices;
  } else {
    indices.resize(point_count);
    for (size_t i = 0; i < point_count; ++i) indices[i] = static_cast<int32_t>(i);
  }
  std::vector<int32_t> face_counts = JsonInts(*geom, "faceVertexCounts");
  size_t face_total = 0;
  for (int32_t c : face_counts) {
    if (c < 3 || static_cast<size_t>(c) >
                     indices.size() - std::min(face_total, indices.size())) {
      if (err) *err = "Invalid polygon face counts";
      return false;
    }
    face_total += static_cast<size_t>(c);
  }
  if (!face_counts.empty() && face_total != indices.size()) {
    if (err) *err = "Polygon face counts do not match indices";
    return false;
  }
  if (face_counts.empty() && indices.size() % 3 != 0) {
    AppendWarn(warn, "Skipping mesh `" + fallback_name +
                         "`: indices must be triangles.\n");
    return true;
  }
  if (face_counts.empty()) face_counts.assign(indices.size() / 3, 3);
  bool identity_indices = indices.size() == point_count;
  for (size_t i = 0; i < indices.size(); ++i) {
    const int32_t index = indices[i];
    if (index < 0 || static_cast<size_t>(index) >= point_count) {
      AppendWarn(warn, "Skipping mesh `" + fallback_name +
                           "`: index out of range.\n");
      return true;
    }
    if (identity_indices && static_cast<size_t>(index) != i) {
      identity_indices = false;
    }
  }

  const std::string name = ChildName(
      Sanitize(JsonString(mesh_json, "name", fallback_name), fallback_name),
      siblings);
  tn::PrimSpec *prim = Define(layer, link_path + "/" + name, "Mesh");
  if (!prim) {
    if (err) *err = "Failed to add mesh `" + name + "`";
    return false;
  }
  SetSourceDisplayName(prim, JsonString(mesh_json, "name"));
  {
    std::set<std::string> children;
    AddURDFCustom(layer, link_path + "/" + name, prim, JsonObject(mesh_json, "urdfCustom"), &children);
  }
  SetToken(prim, "subdivisionScheme", "none", true);
  Set(prim, "points",
      tn::Value::MakeFloatCompArray(std::vector<float>(*positions),
                                    tn::TypeId::Point3f, 3),
      "point3f[]");
  Set(prim, "faceVertexCounts", tn::Value::MakeIntArray(face_counts), "int[]");
  // Non-identity indexing: per-vertex normals/UVs are expanded to
  // faceVarying so the authored data stays consistent with the topology.
  if (normals->size() == positions->size()) {
    std::vector<float> ns;
    if (identity_indices) {
      ns = *normals;
    } else {
      ns.reserve(indices.size() * 3);
      for (int32_t index : indices) {
        const size_t src = static_cast<size_t>(index) * 3;
        ns.insert(ns.end(), normals->begin() + src, normals->begin() + src + 3);
      }
    }
    Set(prim, "normals",
        tn::Value::MakeFloatCompArray(std::move(ns), tn::TypeId::Normal3f, 3),
        "normal3f[]");
    SetInterpolation(prim, "normals", identity_indices ? "vertex" : "faceVarying");
  }
  if (uvs->size() == point_count * 2) {
    std::vector<float> st;
    if (identity_indices) {
      st = *uvs;
    } else {
      st.reserve(indices.size() * 2);
      for (int32_t index : indices) {
        const size_t src = static_cast<size_t>(index) * 2;
        st.insert(st.end(), uvs->begin() + src, uvs->begin() + src + 2);
      }
    }
    Set(prim, "primvars:st",
        tn::Value::MakeFloatCompArray(std::move(st), tn::TypeId::Texcoord2f, 2),
        "texCoord2f[]");
    SetInterpolation(prim, "primvars:st",
                     identity_indices ? "vertex" : "faceVarying");
  }
  const auto colors = JsonFloats(*geom, "colors");
  if (colors.size() == point_count * 3) {
    Set(prim, "primvars:displayColor",
        tn::Value::MakeFloatCompArray(colors, tn::TypeId::Color3f, 3),
        "color3f[]");
    SetInterpolation(prim, "primvars:displayColor", "vertex");
  } else {
    SetDisplayColorFromRGBA(prim, mesh_json);
  }
  const int32_t influences = JsonInt(*geom, "influences", 0);
  if (influences > 0) {
    const auto joint_indices = JsonInts(*geom, "jointIndices");
    const auto joint_weights = JsonFloats(*geom, "jointWeights");
    if (point_count > SIZE_MAX / static_cast<size_t>(influences) ||
        joint_indices.size() != point_count * static_cast<size_t>(influences) ||
        joint_weights.size() != joint_indices.size()) {
      if (err) *err = "Invalid OBJ skin influence arrays";
      return false;
    }
    AddAPI(prim, "SkelBindingAPI");
    Set(prim, "primvars:skel:jointIndices",
        tn::Value::MakeIntArray(joint_indices), "int[]");
    Set(prim, "primvars:skel:jointWeights",
        tn::Value::MakeFloatArray(joint_weights), "float[]");
    for (const char* attr : {"primvars:skel:jointIndices",
                             "primvars:skel:jointWeights"}) {
      SetInterpolation(prim, attr, "vertex");
      auto& meta = prim->ensure_property_meta(attr);
      meta.elementSize = influences;
      meta.authored |= tn::PropMeta::kElementSize;
    }
  }
  const auto material_ids = JsonInts(*geom, "materialIndices");
  const auto material_names = JsonStrings(*geom, "materialNames");
  if (JsonString(mesh_json, "material").empty() &&
      material_ids.size() == face_counts.size()) {
    std::map<int32_t, std::vector<int32_t>> faces;
    for (size_t f = 0; f < material_ids.size(); f++)
      if (material_ids[f] >= 0 &&
          static_cast<size_t>(material_ids[f]) < material_names.size())
        faces[material_ids[f]].push_back(static_cast<int32_t>(f));
    for (const auto& entry : faces) {
      auto* subset = Define(
          layer,
          link_path + "/" + name + "/material_" + std::to_string(entry.first),
          "GeomSubset");
      SetToken(subset, "elementType", "face", true);
      SetToken(subset, "familyName", "materialBind", true);
      Set(subset, "indices", tn::Value::MakeIntArray(entry.second), "int[]");
      AddAPI(subset, "MaterialBindingAPI");
      SetRel(subset, "material:binding",
             {"/World/Materials/" +
              Sanitize(material_names[static_cast<size_t>(entry.first)],
                       "material")});
    }
    if (!faces.empty())
      SetToken(prim, "subsetFamily:materialBind:familyType", "nonOverlapping",
               true);
  }
  Set(prim, "faceVertexIndices", tn::Value::MakeIntArray(std::move(indices)),
      "int[]");
  SetTransformFromJson(prim, mesh_json);

  if (collision) {
    AddCollisionAPIs(prim, true, mesh_json, mjcf_source);
    return true;
  }
  bool render_only = false;
  ReadBool(mesh_json, "renderOnly", &render_only);
  if (!render_only) AddAPI(prim, "MjcImageableAPI");
  if (!ActiveProfile().legacy && StringFrom(mesh_json, "newton", "massModel", "") == "shell") {
    AddAPI(prim, "NewtonMassAPI");
    SetToken(prim, "newton:massModel", "shell", true);
  }
  if (JsonString(mesh_json, "purpose") == "guide") SetToken(prim, "purpose", "guide", true);
  // Bind a UsdShade material (MJCF <geom material=..> ->
  // /World/Materials/<m>); MJCF material names are unique, so the sanitized
  // path is deterministic and the material prim is emitted later.
  const std::string material = JsonString(mesh_json, "material");
  if (!material.empty()) {
    AddAPI(prim, "MaterialBindingAPI");
    SetRel(prim, "material:binding",
           {"/World/Materials/" + Sanitize(material, "material")});
  }
  if (render_only) return true;
  if (mjcf_source) {
    int32_t group = 0;
    if (ReadIntFrom(mesh_json, "mjc", "group", &group) ||
        mesh_json.contains("group")) {
      Set(prim, "mjc:group", tn::Value(JsonInt(mesh_json, "group", group)),
          "int", true);
    }
  } else {
    Set(prim, "mjc:group",
        tn::Value(JsonInt(mesh_json, "group", kLegacyUrdfVisualGroup)), "int",
        true);
  }
  return true;
}

bool AddNativeCollisionShapeFromJson(tn::Layer *layer,
                                     const std::string &link_path,
                                     std::set<std::string> *siblings,
                                     const Json &shape_json,
                                     const std::string &fallback_name,
                                     bool mjcf_source, std::string *warn,
                                     std::string *err, bool collision,
                                     std::string *out_name) {
  const Json *shape_obj = JsonObject(shape_json, "shape");
  const Json &shape = shape_obj ? *shape_obj : shape_json;
  const std::string type = JsonString(shape, "type");
  if (type.empty()) {
    AppendWarn(warn, "Skipping collision shape `" + fallback_name +
                         "`: missing shape.type.\n");
    return true;
  }
  std::string usd_type;
  if (type == "box" || type == "cube") usd_type = "Cube";
  else if (type == "sphere") usd_type = "Sphere";
  else if (type == "cylinder") usd_type = "Cylinder";
  else if (type == "capsule") usd_type = "Capsule";
  else if (type == "plane") usd_type = "Plane";
  else {
    AppendWarn(warn, "Skipping collision shape `" + fallback_name +
                         "`: unsupported shape type `" + type + "`.\n");
    return true;
  }

  const std::string name = ChildName(
      Sanitize(JsonString(shape_json, "name", fallback_name), fallback_name),
      siblings);
  tn::PrimSpec *prim = Define(layer, link_path + "/" + name, usd_type);
  if (!prim) {
    if (err) *err = "Failed to add collision shape `" + name + "`";
    return false;
  }
  SetSourceDisplayName(prim, JsonString(shape_json, "name"));
  {
    std::set<std::string> children;
    AddURDFCustom(layer, link_path + "/" + name, prim, JsonObject(shape_json, "urdfCustom"), &children);
  }
  auto number = [&](const char *key, double fallback) {
    double value = fallback;
    ReadNumber(shape, key, &value);
    return value;
  };
  if (usd_type == "Cube") {
    Set(prim, "size", tn::Value(2.0), "double");
  } else if (usd_type == "Sphere") {
    Set(prim, "radius", tn::Value(number("radius", 0.5)), "double");
  } else if (usd_type == "Cylinder" || usd_type == "Capsule") {
    Set(prim, "radius", tn::Value(number("radius", 0.5)), "double");
    Set(prim, "height", tn::Value(number("height", 1.0)), "double");
    SetToken(prim, "axis", AxisFromToken(JsonString(shape, "axis", "Z")), true);
  } else {
    Set(prim, "width", tn::Value(number("width", 2.0)), "double");
    Set(prim, "length", tn::Value(number("length", 2.0)), "double");
    SetToken(prim, "axis", AxisFromToken(JsonString(shape, "axis", "Z")), true);
  }
  SetTransformFromJson(prim, shape_json);
  if (collision) {
    AddCollisionAPIs(prim, false, shape_json, mjcf_source);
    bool visual = false;
    if (ReadBool(shape_json, "visual", &visual) && visual) SetDisplayColorFromRGBA(prim, shape_json);
  } else {
    // Native visual-only shape: no CollisionAPI, so MuJoCo decodes a
    // contype=0 geom and Newton a visual shape.
    const std::string purpose = JsonString(shape_json, "purpose");
    if (!purpose.empty()) SetToken(prim, "purpose", purpose, true);
    if (!ActiveProfile().legacy && StringFrom(shape_json, "newton", "massModel", "") == "shell") {
      AddAPI(prim, "NewtonMassAPI");
      SetToken(prim, "newton:massModel", "shell", true);
    }
    if (mjcf_source) {
      AddAPI(prim, "MjcImageableAPI");
      int32_t group = 0;
      if (ReadIntFrom(shape_json, "mjc", "group", &group) || shape_json.contains("group"))
        Set(prim, "mjc:group", tn::Value(JsonInt(shape_json, "group", group)), "int", true);
    }
    const std::string material = JsonString(shape_json, "material");
    if (!material.empty()) {
      AddAPI(prim, "MaterialBindingAPI");
      SetRel(prim, "material:binding", {"/World/Materials/" + Sanitize(material, "material")});
    }
    SetDisplayColorFromRGBA(prim, shape_json);
  }
  if (out_name) *out_name = name;
  return true;
}

}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
