// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
// URDF/MJCF payload validation, authoring order, and stage publication.
#include "urdf-to-usd.hh"
#include "urdf-to-usd-internal.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "next/reader/obj-reader.hh"
#include <cmath>
#include <memory>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
using namespace urdf_detail;

static bool ConvertPayload(
    const ::lightusd::tydra::detail::URDFPayload &payload,
    const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
    ::lightusd::next::Stage *out_stage, std::string *warn, std::string *err) {
  if (!out_stage) {
    if (err) *err = "Output Stage pointer is null";
    return false;
  }
  if (warn) warn->clear();
  if (err) err->clear();

  const Json &root = payload.Root();
  // The JSON bridge keeps the historical output unless a profile is named;
  // native importers always name one (default "newton").
  Profile profile;
  if (!ParseProfile(JsonString(root, "profile", "legacy"), &profile)) {
    if (err) *err = "Unknown output profile `" + JsonString(root, "profile") +
                    "` (expected legacy, newton or openusd)";
    return false;
  }
  ProfileScope profile_scope(profile);
  // The openusd profile has no schema for MuJoCo/Newton-only records; say
  // what was omitted instead of authoring empty engine-typed prims.
  auto engine_section = [&](const char *key, const char *what) {
    if (profile.engine) return true;
    const Json *items = JsonArray(root, key);
    if (items && !items->empty())
      AppendWarn(warn, "openusd profile omits " + std::to_string(items->size()) + " " +
                           what + " (no standard UsdPhysics schema).\n");
    return false;
  };
  if (JsonString(root, "sourceFormat") == "obj") {
#if defined(LIGHTUSD_WITH_USDOBJ)
    tn::Layer obj_layer;
    if (!tn::ConvertOBJPayloadToLayer(root, &obj_layer, err)) return false;
    out_stage->SetRootLayer(std::move(obj_layer));
    return true;
#else
    if (err) *err = "OBJ support is disabled";
    return false;
#endif
  }
  const Json &links = payload.Array("links");
  const Json &joints = payload.Array("joints");
  const bool mjcf_source = payload.mjcf_source;
  bool no_physics = false;
  ReadBool(root, "noPhysics", &no_physics);

  tn::Layer layer;
  // Builders keep PrimSpec pointers while adding shader/subset children.
  // Reserve from structural payload records, never from numeric mesh lanes.
  size_t prim_capacity = 128;
  std::vector<const Json*> pending{&root};
  while (!pending.empty()) {
    const Json* object = pending.back();
    pending.pop_back();
    if (object->is_object()) {
      if (prim_capacity > size_t(UINT32_MAX) - 16) {
        if (err) *err = "Robot prim count limit exceeded";
        return false;
      }
      prim_capacity += 16;
      if (const auto* names = JsonArray(*object, "materialNames")) {
        if (names->size() > size_t(UINT32_MAX) - prim_capacity) {
          if (err) *err = "Robot material count limit exceeded";
          return false;
        }
        prim_capacity += names->size();
      }
      for (const auto& item : *object->object_items())
        if (item.value().is_object() || item.value().is_array())
          pending.push_back(&item.value());
    } else if (object->is_array()) {
      // Heterogeneous arrays may start with a scalar and contain records
      // later. Count those too, without retaining numeric mesh lanes.
      for (const auto& item : *object)
        if (item.is_object() || item.is_array()) pending.push_back(&item);
    }
  }
  layer.reserve(prim_capacity);
  layer.meta().defaultPrim = "World";
  if (root["unsupported"].is_array() && !root["unsupported"].empty()) {
    layer.meta().customLayerData = tn::Value::MakeDictionary();
    layer.meta().customLayerData.as_dictionary()->set("lightusdImportUnsupported",
        tn::Value(root["unsupported"].dump()));
    layer.meta().customLayerData_set = true;
  }
  const std::string up_axis = JsonString(root, "upAxis", "Y");
  const bool y_up = !(up_axis == "Z" || up_axis == "z");
  layer.meta().upAxis = y_up ? "Y" : "Z";
  layer.meta().upAxis_set = true;
  // MuJoCo authors in SI units (1 stage unit = 1 m / 1 kg).
  layer.meta().metersPerUnit = no_physics ? 0.01 : 1.0;
  layer.meta().metersPerUnit_set = !no_physics;
  layer.meta().kilogramsPerUnit = 1.0;
  layer.meta().kilogramsPerUnit_set = !no_physics;

  tn::PrimSpec *world = Define(&layer, "/World", "Xform");
  if (!world) {
    if (err) *err = "Failed to define /World";
    return false;
  }
  world->meta().kind() = "assembly";
  // URDF/MJCF sources are Z-up; a Y-up stage gets a single corrective root
  // rotation Rx(-90deg) so local-frame physics data rides along unchanged
  // (doc/usd-physics-upAxis.md).
  if (y_up && !no_physics) {
    Set(world, "xformOp:rotateX", tn::Value(-90.0), "double");
    Set(world, "xformOpOrder",
        tn::Value::MakeTokenArray(std::vector<std::string>{"xformOp:rotateX"}),
        "token[]", true);
  }

  if (!no_physics) AddPhysicsScene(&layer, root);
  std::unique_ptr<PhysicsMaterials> physics_materials;
  if (!no_physics && !profile.legacy && mjcf_source)
    physics_materials.reset(new PhysicsMaterials(&layer));
  Define(&layer, "/World/Links", "Xform");
  Define(&layer, "/World/Joints", "Xform");

  // --- Links ---------------------------------------------------------------
  std::set<std::string> child_links;
  for (const Json &joint : joints) {
    const std::string child = JsonString(joint, "child");
    if (!child.empty()) child_links.insert(child);
  }
  bool filter_parent = true;
  if (const Json *ms = JsonObject(root, "mjcScene"))
    if (const Json *flags = JsonObject(*ms, "flag")) ReadBool(*flags, "filterparent", &filter_parent);
  bool self_collision_enabled = true;
  if (const Json *newton = JsonObject(root, "newton")) {
    ReadBool(*newton, "selfCollisionEnabled", &self_collision_enabled);
  }
  NameMap link_names;
  NameMap geom_names;  // MJCF tendon wrap geom -> native prim under /World
  std::map<std::string, std::vector<std::string>> link_colliders;  // by source name
  std::set<std::string> static_links;                          // by source name
  std::map<std::string, urdf_detail::Matrix4> link_worlds;                 // by source name
  std::map<std::string, std::set<std::string>> link_children;  // by USD name
  std::set<std::string> used_links;
  // layout="nested": each dynamic link is a child prim of its parent link
  // (the kinematic tree, as the reference converters write it), with a
  // parent-relative transform. Link names then hold paths relative to
  // /World/Links. Links under the world or a static link stay top-level.
  const bool nested = JsonString(root, "layout", "flat") == "nested";
  std::vector<size_t> order(links.size());
  for (size_t i = 0; i < links.size(); ++i) order[i] = i;
  std::vector<std::string> nested_names(links.size());
  std::map<std::string, std::string> nested_parent;  // source -> parent source
  if (nested) {
    std::map<std::string, size_t> index_of;
    std::set<std::string> static_sources;
    for (size_t i = 0; i < links.size(); ++i) {
      const std::string source = JsonString(links[i], "name", "link_" + std::to_string(i));
      index_of[source] = i;
      bool is_static = false;
      ReadBool(links[i], "static", &is_static);
      if (is_static) static_sources.insert(source);
    }
    for (const Json &joint : joints) {
      const std::string child = JsonString(joint, "child"), parent = JsonString(joint, "parent");
      if (!child.empty() && index_of.count(parent) && !static_sources.count(parent) &&
          !static_sources.count(child) && !nested_parent.count(child))
        nested_parent[child] = parent;
    }
    // Parents first; a cycle (invalid input) falls back to source order.
    order.clear();
    std::vector<int> state(links.size(), 0);
    for (size_t i = 0; i < links.size(); ++i) {
      std::vector<size_t> chain;
      for (size_t k = i; state[k] == 0;) {
        state[k] = 1;
        chain.push_back(k);
        const std::string source = JsonString(links[k], "name", "link_" + std::to_string(k));
        auto up = nested_parent.find(source);
        if (up == nested_parent.end()) break;
        k = index_of[up->second];
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        state[*it] = 2;
        order.push_back(*it);
      }
    }
    std::map<std::string, std::string> rel_of;
    for (size_t i : order) {
      const std::string source = JsonString(links[i], "name", "link_" + std::to_string(i));
      const std::string name = Unique(source, "link", &used_links);
      auto up = nested_parent.find(source);
      std::string rel = name;
      if (up != nested_parent.end() && rel_of.count(up->second)) {
        rel = rel_of[up->second] + "/" + name;
        link_children[rel_of[up->second]].insert(name);  // geometry must not take it
      } else {
        nested_parent.erase(source);
      }
      rel_of[source] = rel;
      nested_names[i] = rel;
    }
  }
  for (size_t i : order) {
    const Json &link = links[i];
    const std::string source =
        JsonString(link, "name", "link_" + std::to_string(i));
    const std::string name = nested ? nested_names[i] : Unique(source, "link", &used_links);
    link_names[source] = name;
    const std::string path = "/World/Links/" + name;
    tn::PrimSpec *prim = Define(&layer, path, "Xform");
    if (!prim) {
      if (err) *err = "Failed to define link `" + name + "`";
      return false;
    }
    // A world-fixed link (e.g. the synthetic "world" link holding worldbody
    // floor/ground geoms) is a static collider, not a dynamic body.
    bool is_static = false;
    ReadBool(link, "static", &is_static);
    link_worlds[source] = MatrixFromArray(JsonDoubles(link, "matrix"));
    auto nested_up = nested ? nested_parent.find(source) : nested_parent.end();
    if (nested_up != nested_parent.end())
      SetTransform(prim, MultiplyRowMajor(link_worlds[source],
                                          InverseRigid(link_worlds[nested_up->second])));
    else
      SetTransformFromJson(prim, link);
    if (is_static) static_links.insert(source);
    // Non-legacy profiles: the synthetic world link is a plain static Xform
    // holding static colliders, not a disabled rigid body (Newton and the
    // UsdPhysics parser count every RigidBodyAPI prim as a body).
    if (!no_physics && !(is_static && !profile.legacy)) {
      AddAPIs(prim, {"PhysicsRigidBodyAPI", "PhysicsMassAPI"});
      Set(prim, "physics:rigidBodyEnabled", tn::Value(!is_static), "bool");
      Set(prim, "physics:startsAsleep", tn::Value(false), "bool");
      bool flag = false;
      if (ReadBool(link, "mocap", &flag) && flag) {
        // Mocap bodies are driven externally: a kinematic rigid body.
        if (profile.legacy) Set(prim, "mjc:mocap", tn::Value(true), "bool", true);
        else Set(prim, "physics:kinematicEnabled", tn::Value(true), "bool");
      }
      // MuJoCo <freejoint>: distinguishes a floating-base articulation root
      // from a fixed (anchored) parentless base. UsdPhysics bodies are free
      // unless jointed, so only legacy output records it.
      flag = false;
      ReadBool(link, "floating", &flag);
      if (flag && profile.legacy)
        Set(prim, "mjc:freeJoint", tn::Value(true), "bool", true);
      double gravcomp = 0.0;
      if (!profile.legacy && ReadNumber(link, "gravcomp", &gravcomp))
        Set(prim, "mjc:gravcomp", tn::Value(gravcomp), "double");
      bool articulation_root = false;
      ReadBool(link, "articulationRoot", &articulation_root);
      if (!is_static && (!child_links.count(source) || articulation_root)) {
        AddAPIs(prim,
                {"PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"});
        Set(prim, "newton:selfCollisionEnabled",
            tn::Value(self_collision_enabled), "bool");
        bool adds_mobility = false;
        if (profile.newton && ReadBool(link, "jointsAddMobility", &adds_mobility) &&
            adds_mobility)
          Set(prim, "newton:jointsAddMobility", tn::Value(true), "bool");
      }
      if (const Json* inertial = JsonObject(link, "inertial")) {
        float mass = 0.0f;
        if (ReadNumber(*inertial, "mass", &mass) && mass > 0.0f) {
          Set(prim, "physics:mass", tn::Value(mass), "float");
        }
        const std::vector<float> com = JsonFloats(*inertial, "centerOfMass");
        if (com.size() >= 3)
          SetPoint3(prim, "physics:centerOfMass", com, false);
        // A full inertia tensor is diagonalized into principal moments +
        // principal-axes rotation (the lossless USD representation).
        const std::vector<double> full = JsonDoubles(*inertial, "fullInertia");
        if (!profile.legacy) {
          AuthorInertia(prim, *inertial);
        } else if (full.size() >= 6) {
          // [Ixx, Iyy, Izz, Ixy, Ixz, Iyz] -> symmetric 3x3.
          const double tensor[3][3] = {{full[0], full[3], full[4]},
                                       {full[3], full[1], full[5]},
                                       {full[4], full[5], full[2]}};
          double eval[3];
          double evec[3][3];
          JacobiEigenSymmetric3(tensor, eval, evec);
          Set(prim, "physics:diagonalInertia",
              tn::Value::MakeFloat3(static_cast<float>(eval[0]),
                                    static_cast<float>(eval[1]),
                                    static_cast<float>(eval[2])),
              "float3");
          Set(prim, "physics:principalAxes", RotationMatrixToQuatf(evec),
              "quatf");
        } else {
          const std::vector<float> inertia =
              JsonFloats(*inertial, "diagonalInertia");
          if (inertia.size() >= 3) {
            Set(prim, "physics:diagonalInertia",
                tn::Value::MakeFloat3(inertia[0], inertia[1], inertia[2]),
                "float3");
          }
        }
        const auto principal = JsonFloats(*inertial, "principalAxes");
        if (profile.legacy && full.size() < 6 && principal.size() == 4)
          Set(prim, "physics:principalAxes",
              tn::Value::MakeQuatf(principal[0], principal[1], principal[2],
                                   principal[3]),
              "quatf");
      }

    }  // physics-bearing link

    // Visuals, collisions and (non-legacy) sites share the link's child
    // namespace.
    std::set<std::string> &siblings = link_children[name];
    if (const Json *visuals = JsonArray(link, "visuals")) {
      size_t n = 0;
      for (const Json &visual : *visuals) {
        const std::string fallback = "visual_" + std::to_string(n++);
        std::string prim_name;
        const bool ok =
            JsonObject(visual, "shape")
                ? AddNativeCollisionShapeFromJson(&layer, path, &siblings,
                                                  visual, fallback, mjcf_source,
                                                  warn, err, false, &prim_name)
                : AddMeshFromJson(&layer, path, &siblings, visual, mesh_buffers,
                                  fallback, false, mjcf_source, warn, err);
        if (!ok) return false;
        const std::string geom = JsonString(visual, "mjcGeom");
        if (!geom.empty() && !prim_name.empty())
          geom_names.emplace(geom, "Links/" + name + "/" + prim_name);
      }
    }
    if (const Json *collisions = JsonArray(link, "collisions")) {
      size_t n = 0;
      for (const Json &collision : *collisions) {
        const std::string fallback = "collision_" + std::to_string(n++);
        std::string prim_name;
        const bool ok =
            JsonObject(collision, "shape")
                ? AddNativeCollisionShapeFromJson(&layer, path, &siblings,
                                                  collision, fallback,
                                                  mjcf_source, warn, err, true,
                                                  &prim_name)
                : AddMeshFromJson(&layer, path, &siblings, collision,
                                  mesh_buffers, fallback, true, mjcf_source,
                                  warn, err);
        if (!ok) return false;
        const std::string geom = JsonString(collision, "mjcGeom");
        if (!geom.empty() && !prim_name.empty())
          geom_names[geom] = "Links/" + name + "/" + prim_name;
        if (!prim_name.empty())
          link_colliders[source].push_back(path + "/" + prim_name);
      }
    }
  }

  // MJCF <contact><exclude> -> physics:filteredPairs on the first body.
  if (const Json *pairs = JsonArray(root, "filteredPairs")) {
    std::map<std::string, std::vector<std::string>> filtered;
    for (const Json &pair : *pairs) {
      if (!pair.is_object()) continue;
      const std::string body1 = JsonString(pair, "body1");
      const std::string body2 = JsonString(pair, "body2");
      auto it1 = link_names.find(body1);
      auto it2 = link_names.find(body2);
      if (it1 == link_names.end() || it2 == link_names.end()) {
        AppendWarn(warn, "Skipping filtered pair `" + body1 + "` / `" + body2 +
                             "`: link was not exported.\n");
        continue;
      }
      // A static link has no RigidBodyAPI outside legacy output; filter
      // against its collider prims from the dynamic side instead.
      const bool static1 = !profile.legacy && static_links.count(body1);
      const bool static2 = !profile.legacy && static_links.count(body2);
      if (static1 && static2) continue;
      if (static1 || static2) {
        const std::string &dynamic = static1 ? body2 : body1;
        const auto &shapes = link_colliders[static1 ? body1 : body2];
        auto &out = filtered[dynamic];
        out.insert(out.end(), shapes.begin(), shapes.end());
        continue;
      }
      filtered[body1].push_back("/World/Links/" + it2->second);
    }
    for (const auto &entry : filtered) {
      if (entry.second.empty()) continue;
      tn::PrimSpec *prim = layer.prim_at_path_mutable(
          "/World/Links/" + link_names[entry.first]);
      if (!prim) continue;
      AddAPI(prim, "PhysicsFilteredPairsAPI");
      SetRel(prim, "physics:filteredPairs", entry.second);
    }
  }

  // --- Joints --------------------------------------------------------------
  NameMap joint_names;
  std::vector<std::string> joint_usd_names(joints.size());
  std::set<std::string> used_joints;
  for (size_t i = 0; i < joints.size(); ++i) {
    const std::string source =
        JsonString(joints[i], "name", "joint_" + std::to_string(i));
    joint_usd_names[i] = Unique(source, "joint", &used_joints);
    joint_names[source] = joint_usd_names[i];
  }
  constexpr double kRadToDeg = 57.2957795130823208768;
  for (size_t i = 0; i < joints.size(); ++i) {
    const Json &joint = joints[i];
    const std::string type = JsonString(joint, "type", "fixed");
    const std::string parent = JsonString(joint, "parent");
    const std::string child = JsonString(joint, "child");
    if (!link_names.count(parent) || !link_names.count(child)) {
      AppendWarn(warn, "Skipping joint `" + JsonString(joint, "name", "joint") +
                           "`: parent or child link was not exported.\n");
      continue;
    }
    const std::string &name = joint_usd_names[i];
    const std::string axis = AxisToken(joint);
    // localRot0 is authoritative for the joint frame rotation; originMatrix
    // contributes only its translation, so surface a dropped rotation.
    if (!joint.contains("localRot0")) {
      const std::vector<double> m = JsonDoubles(joint, "originMatrix");
      const size_t idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
      const double expect[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      bool rotated = false;
      for (size_t k = 0; m.size() == 16 && k < 9; k++) {
        rotated = rotated || std::abs(m[idx[k]] - expect[k]) > 1e-6;
      }
      if (rotated) {
        AppendWarn(warn, "Joint `" + name +
                             "` has a rotation in originMatrix but no "
                             "localRot0; the rotation is ignored (supply "
                             "localRot0).\n");
      }
    }
    std::string usd_type = "PhysicsFixedJoint";
    if (type == "revolute" || type == "continuous") {
      usd_type = "PhysicsRevoluteJoint";
    } else if (type == "prismatic") {
      usd_type = "PhysicsPrismaticJoint";
    } else if (type == "spherical") {
      // MuJoCo ball joint; cone limits are not carried in the JSON.
      usd_type = "PhysicsSphericalJoint";
    } else if (type == "planar" && !profile.legacy) {
      // Generic (D6) joint; locks are authored below.
      usd_type = "PhysicsJoint";
    } else if (type != "fixed") {
      AppendWarn(warn, "Joint `" + name + "` type `" + type +
                           "` is exported as PhysicsFixedJoint.\n");
    }
    tn::PrimSpec *prim = Define(&layer, "/World/Joints/" + name, usd_type);
    if (!prim) {
      if (err) *err = "Failed to add joint `" + name + "`";
      return false;
    }
    // An empty body0 means the world frame in UsdPhysics.
    AssignJointBase(prim, joint,
                    !profile.legacy && static_links.count(parent)
                        ? std::string() : link_names[parent],
                    link_names[child], joint_names);
    // <flag filterparent="disable"> lets jointed parent/child bodies collide.
    if (!profile.legacy && !filter_parent)
      Set(prim, "physics:collisionEnabled", tn::Value(true), "bool", true);
    if (usd_type != "PhysicsFixedJoint" && usd_type != "PhysicsJoint") {
      SetToken(prim, "physics:axis", axis, true);
    }
    if (usd_type == "PhysicsJoint") {
      // URDF planar: the joint frame's X is the plane normal (localRot aligns
      // X with the URDF axis). Translate in Y/Z and rotate about X; lock the
      // rest with low > high (UsdPhysics "locked axis").
      for (const char *dof : {"transX", "rotY", "rotZ"}) {
        AddAPI(prim, std::string("PhysicsLimitAPI:") + dof);
        Set(prim, std::string("limit:") + dof + ":physics:low", tn::Value(1.0f), "float");
        Set(prim, std::string("limit:") + dof + ":physics:high", tn::Value(-1.0f), "float");
      }
      // Free axes are unlimited, but Newton creates a D6 DOF only for an
      // axis with a LimitAPI whose low < high.
      const float inf = std::numeric_limits<float>::infinity();
      for (const char *dof : {"transY", "transZ", "rotX"}) {
        AddAPI(prim, std::string("PhysicsLimitAPI:") + dof);
        Set(prim, std::string("limit:") + dof + ":physics:low", tn::Value(-inf), "float");
        Set(prim, std::string("limit:") + dof + ":physics:high", tn::Value(inf), "float");
      }
    }
    double cone = 0.0;
    if (usd_type == "PhysicsSphericalJoint" && ReadNumber(joint, "coneLimit", &cone)) {
      const float degrees = static_cast<float>(cone * kRadToDeg);
      Set(prim, "physics:coneAngle0Limit", tn::Value(degrees), "float");
      Set(prim, "physics:coneAngle1Limit", tn::Value(degrees), "float");
    }
    // Limits: revolute in degrees, prismatic in meters (continuous joints
    // are unlimited).
    const Json *limit = JsonObject(joint, "limit");
    const bool revolute_limit = type == "revolute" && limit;
    const bool prismatic_limit = type == "prismatic" && limit;
    if (revolute_limit || prismatic_limit) {
      const double scale = revolute_limit ? kRadToDeg : 1.0;
      double lower = 0.0;
      double upper = 0.0;
      if (ReadNumber(*limit, "lower", &lower)) {
        Set(prim, "physics:lowerLimit",
            tn::Value(static_cast<float>(lower * scale)), "float", true);
      }
      if (ReadNumber(*limit, "upper", &upper)) {
        Set(prim, "physics:upperLimit",
            tn::Value(static_cast<float>(upper * scale)), "float", true);
      }
      // Revolute/prismatic joints carry their limits in lower/upperLimit.
      // PhysicsLimitAPI is the D6 mechanism; legacy output also applied it
      // with non-standard property names, kept for byte compatibility.
      if (profile.legacy) {
        const std::string dof = JointDofName(axis, revolute_limit);
        AddAPI(prim, "PhysicsLimitAPI:" + dof);
        Set(prim, "physics:limit:" + dof + ":low",
            tn::Value(static_cast<float>(lower * scale)), "float");
        Set(prim, "physics:limit:" + dof + ":high",
            tn::Value(static_cast<float>(upper * scale)), "float");
      }
    }
  }

  // --- MJCF / Newton scopes, in the legacy /World child order ---------------
  Scope actuators_scope(&layer, "Actuators");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("actuators");
    for (size_t i = 0; engine_section("actuators", "Newton actuators") && i < items.size(); ++i) {
      AddNewtonActuatorFromJson(&actuators_scope, &siblings, items[i],
                                joint_names, i, warn);
    }
  }

  // Site names are resolved up front: spatial tendons route through them
  // although the legacy Sites scope follows Tendons/Equalities. Values are
  // paths relative to /World. Non-legacy profiles author each site under its
  // owning link (Newton only loads sites below rigid bodies).
  const Json &sites = payload.Array("sites");
  NameMap site_names;
  std::vector<std::string> site_usd_names(sites.size());
  std::vector<bool> site_on_link(sites.size(), false);
  {
    std::set<std::string> used;
    for (size_t i = 0; i < sites.size(); ++i) {
      const std::string source =
          JsonString(sites[i], "name", "site_" + std::to_string(i));
      const auto owner = link_names.find(JsonString(sites[i], "body"));
      if (!profile.legacy && owner != link_names.end()) {
        const std::string name =
            ChildName(Sanitize(source, "site"), &link_children[owner->second]);
        const std::string path = "/World/Links/" + owner->second + "/" + name;
        tn::PrimSpec *site = Define(&layer, path, "Sphere");
        if (site) {
          const urdf_detail::Matrix4 world = MatrixFromArray(JsonDoubles(sites[i], "matrix"));
          const urdf_detail::Matrix4 local =
              MultiplyRowMajor(world, InverseRigid(link_worlds[owner->first]));
          AuthorMjcSite(site, sites[i], &local);
        }
        site_usd_names[i] = name;
        site_on_link[i] = true;
        site_names[source] = "Links/" + owner->second + "/" + name;
        continue;
      }
      site_usd_names[i] = Unique(source, "site", &used);
      site_names[source] = "Sites/" + site_usd_names[i];
    }
  }

  if (!profile.legacy) AddDeformablesFromJson(&layer, payload.Array("deformables"), link_names, warn);

  NameMap tendon_names;
  Scope tendons_scope(&layer, "Tendons");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("tendons");
    for (size_t i = 0; engine_section("tendons", "MuJoCo tendons") && i < items.size(); ++i) {
      AddMjcTendonFromJson(&tendons_scope, &siblings, items[i], joint_names,
                           site_names, geom_names, &tendon_names, i, warn);
    }
  }

  Scope equalities_scope(&layer, "Equalities");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("equalities");
    for (size_t i = 0; i < items.size(); ++i) {
      if (profile.legacy || no_physics) {
        AddMjcEqualityFromJson(&equalities_scope, &siblings, items[i],
                               link_names, joint_names, i);
        continue;
      }
      AddEqualityConstraint(&layer, &equalities_scope, &siblings, items[i], i,
                            link_names, link_worlds, static_links, joint_names,
                            sites, warn);
    }
  }

  Scope sites_scope(&layer, "Sites");
  for (size_t i = 0; i < sites.size(); ++i) {
    if (!site_on_link[i])
      AddMjcSiteFromJson(&sites_scope, sites[i], site_usd_names[i]);
  }

  Scope mjc_actuators_scope(&layer, "MjcActuators");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("mjcActuators");
    for (size_t i = 0; engine_section("mjcActuators", "MuJoCo actuators") && i < items.size(); ++i) {
      AddMjcActuatorFromJson(&mjc_actuators_scope, &used, items[i],
                             joint_names, tendon_names, site_names, link_names,
                             i, warn);
    }
  }

  Scope keyframes_scope(&layer, "Keyframes");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("keyframes");
    for (size_t i = 0; engine_section("keyframes", "MuJoCo keyframes") && i < items.size(); ++i) {
      AddMjcKeyframeFromJson(&keyframes_scope, &used, items[i], i);
    }
  }

  Scope lights_scope(&layer, "Lights");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("lights");
    for (size_t i = 0; i < items.size(); ++i) {
      AddLightFromJson(&lights_scope, &used, items[i], i);
    }
    if (payload.Root().contains("skybox") && payload.Root().at("skybox").is_object())
      AddSkyboxFromJson(&lights_scope, &used, payload.Root().at("skybox"), !y_up);
  }

  Scope cameras_scope(&layer, "Cameras");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("cameras");
    for (size_t i = 0; i < items.size(); ++i) {
      AddCameraFromJson(&cameras_scope, &used, items[i], i);
    }
  }

  // Geoms bind via /World/Materials/<sanitized-name> (see AddMeshFromJson).
  Scope materials_scope(&layer, "Materials");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("materials");
    for (size_t i = 0; i < items.size(); ++i) {
      AddMaterialFromJson(&materials_scope, &used, items[i], i);
    }
  }

  Scope sensors_scope(&layer, "Sensors");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("sensors");
    for (size_t i = 0; engine_section("sensors", "MuJoCo sensors") && i < items.size(); ++i) {
      AddMjcSensorFromJson(&sensors_scope, &used, items[i], i);
    }
  }

  Scope contacts_scope(&layer, "Contacts");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("contactPairs");
    for (size_t i = 0; engine_section("contactPairs", "MuJoCo contact pairs") && i < items.size(); ++i) {
      AddContactPairFromJson(&contacts_scope, &used, items[i], i);
    }
  }

  // MJCF <custom><numeric|text> -> /World/MjcCustom (model metadata / MJX
  // knobs), authored only when it carries at least one entry.
  if (const Json *custom = profile.engine ? JsonObject(root, "custom") : nullptr) {
    std::vector<std::pair<std::string, const Json *>> numeric, text;
    if (const Json *items = JsonArray(*custom, "numeric")) {
      for (const Json &n : *items) {
        const std::string nm = JsonString(n, "name");
        if (!nm.empty()) numeric.emplace_back(nm, &n);
      }
    }
    if (const Json *items = JsonArray(*custom, "text")) {
      for (const Json &t : *items) {
        const std::string nm = JsonString(t, "name");
        if (!nm.empty()) text.emplace_back(nm, &t);
      }
    }
    if (!numeric.empty() || !text.empty()) {
      tn::PrimSpec *prim = Define(&layer, "/World/MjcCustom", "Xform");
      for (const auto &entry : numeric) {
        SetDoubles(prim, "mjc:custom:" + entry.first,
                   JsonDoubles(*entry.second, "data"));
      }
      for (const auto &entry : text) {
        SetToken(prim, "mjc:customtext:" + entry.first,
                 JsonString(*entry.second, "data"));
      }
    }
  }

  // MJCF <extension><plugin><instance><config> -> /World/MjcPlugins; the
  // instance config (e.g. a PID actuator's kp/ki/kd) is referenced by an
  // MjcActuator's mjc:instance.
  if (const Json *plugins = profile.engine ? JsonArray(root, "plugins") : nullptr) {
    std::vector<std::pair<std::string, std::string>> tokens;
    for (const Json &p : *plugins) {
      const std::string inst = JsonString(p, "instance");
      if (inst.empty()) continue;
      const std::string plugin_id = JsonString(p, "plugin");
      if (!plugin_id.empty()) {
        tokens.emplace_back("mjc:plugin:" + inst + ":plugin", plugin_id);
      }
      const Json *config = JsonObject(p, "config");
      const auto *members = config ? config->object_items() : nullptr;
      if (!members) continue;
      for (const auto &member : *members) {
        if (!member.value().is_string()) continue;
        tokens.emplace_back("mjc:plugin:" + inst + ":config:" + member.key,
                            member.value().get<std::string>());
      }
    }
    if (!tokens.empty()) {
      tn::PrimSpec *prim = Define(&layer, "/World/MjcPlugins", "Xform");
      for (const auto &entry : tokens) SetToken(prim, entry.first, entry.second);
    }
  }

  layer.finalize();
  tn::Stage stage;
  stage.SetRootLayer(std::move(layer));
  *out_stage = std::move(stage);
  return true;
}

bool ConvertURDFJsonToUSDStage(const std::string& robot_json,
    const std::map<std::string, URDFMeshBuffer>* mesh_buffers,
    ::lightusd::next::Stage* out_stage, std::string* warn, std::string* err) {
  ::lightusd::tydra::detail::URDFPayload payload;
  if (!::lightusd::tydra::detail::URDFPayload::Parse(robot_json, &payload, err)) return false;
  return ConvertPayload(payload, mesh_buffers, out_stage, warn, err);
}
bool ConvertURDFPayloadToUSDStage(const ::lightusd::minijson::Value& value,
    ::lightusd::next::Stage* out_stage, std::string* warn, std::string* err) {
  return ConvertURDFPayloadToUSDStage(value, nullptr, out_stage, warn, err);
}

bool ConvertURDFPayloadToUSDStage(const ::lightusd::minijson::Value& value,
    const std::map<std::string, URDFMeshBuffer>* mesh_buffers,
    ::lightusd::next::Stage* out_stage, std::string* warn, std::string* err) {
  ::lightusd::tydra::detail::URDFPayload payload;
  if (!::lightusd::tydra::detail::URDFPayload::Borrow(value, &payload, err)) return false;
  return ConvertPayload(payload, mesh_buffers, out_stage, warn, err);
}

bool ConvertURDFJsonToUSDStage(const std::string &robot_json,
                               ::lightusd::next::Stage *out_stage,
                               std::string *warn, std::string *err) {
  return ConvertURDFJsonToUSDStage(robot_json, nullptr, out_stage, warn, err);
}


} } }  // namespace lightusd::tydra::next
