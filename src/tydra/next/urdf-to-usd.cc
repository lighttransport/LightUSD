// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.

#include "tydra/next/urdf-to-usd.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <utility>

#include "next/layer/layer.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "tydra/urdf-payload.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace {

using Json = ::lightusd::tydra::detail::JSONValue;
namespace tn = ::lightusd::next;

std::string JsonString(const Json &j, const char *key,
                       const std::string &fallback = std::string()) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) {
    return fallback;
  }
  return j.at(key).get<std::string>();
}

double JsonNumber(const Json &j, const char *key, double fallback) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_number()) {
    return fallback;
  }
  return j.at(key).get<double>();
}

bool JsonBool(const Json &j, const char *key, bool fallback) {
  if (!j.is_object() || !j.contains(key)) return fallback;
  const Json &v = j.at(key);
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer()) return v.get<int32_t>() != 0;
  return fallback;
}

std::vector<float> JsonFloats(const Json &j, const char *key) {
  std::vector<float> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) return out;
  for (const Json &v : j.at(key)) {
    if (v.is_number()) out.push_back(static_cast<float>(v.get<double>()));
  }
  return out;
}

std::vector<double> JsonDoubles(const Json &j, const char *key) {
  std::vector<double> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) return out;
  for (const Json &v : j.at(key)) {
    if (v.is_number()) out.push_back(v.get<double>());
  }
  return out;
}

std::vector<int32_t> JsonInts(const Json &j, const char *key) {
  std::vector<int32_t> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) return out;
  for (const Json &v : j.at(key)) {
    if (v.is_number_integer()) out.push_back(v.get<int32_t>());
    else if (v.is_number()) out.push_back(static_cast<int32_t>(v.get<double>()));
  }
  return out;
}

std::string Sanitize(const std::string &source, const std::string &fallback) {
  const std::string &input = source.empty() ? fallback : source;
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    out.push_back((std::isalnum(uc) || c == '_') ? c : '_');
  }
  if (out.empty()) out = fallback;
  if (!(std::isalpha(static_cast<unsigned char>(out[0])) || out[0] == '_')) {
    out.insert(out.begin(), '_');
  }
  return out;
}

std::string Unique(const std::string &source, const std::string &fallback,
                   std::set<std::string> *used) {
  const std::string base = Sanitize(source, fallback);
  std::string name = base;
  uint32_t suffix = 1;
  while (used->count(name)) name = base + "_" + std::to_string(suffix++);
  used->insert(name);
  return name;
}

void AppendWarn(std::string *warn, const std::string &message) {
  if (warn) *warn += message;
}

void AddAPIs(tn::PrimSpec *prim,
             std::initializer_list<const char *> schemas) {
  if (!prim) return;
  std::vector<std::string> &apis = prim->meta().apiSchemas();
  prim->meta().apiSchemasQualifier() = "prepend";
  for (const char *schema : schemas) {
    if (std::find(apis.begin(), apis.end(), schema) == apis.end()) {
      apis.emplace_back(schema);
    }
  }
}

void AddAPI(tn::PrimSpec *prim, const std::string &schema) {
  if (!prim) return;
  std::vector<std::string> &apis = prim->meta().apiSchemas();
  prim->meta().apiSchemasQualifier() = "prepend";
  if (std::find(apis.begin(), apis.end(), schema) == apis.end()) {
    apis.push_back(schema);
  }
}

void Set(tn::PrimSpec *prim, const std::string &name, tn::Value value,
         const std::string &type_name, bool uniform = false) {
  if (!prim) return;
  uint16_t flags = uniform ? tn::PropSlot::kFlagUniform : 0;
  prim->upsert_property(name, std::move(value), flags);
  prim->set_property_type_name(name, type_name);
}

void SetToken(tn::PrimSpec *prim, const std::string &name,
              const std::string &value, bool uniform = false) {
  Set(prim, name, tn::Value::MakeToken(value), "token", uniform);
}

void SetFloat3(tn::PrimSpec *prim, const std::string &name,
               const std::vector<float> &v, const std::string &type_name) {
  if (v.size() >= 3) {
    Set(prim, name, tn::Value::MakeFloat3(v[0], v[1], v[2]), type_name);
  }
}

tn::PrimSpec *Define(tn::Layer *layer, const std::string &path,
                     const std::string &type) {
  const uint32_t index = layer->define_prim_at_path(path, type);
  return index == UINT32_MAX ? nullptr : layer->prim_mutable(index);
}

void SetTransform(tn::PrimSpec *prim, const Json &source) {
  std::vector<double> matrix = JsonDoubles(source, "matrix");
  if (matrix.size() != 16) matrix = JsonDoubles(source, "originMatrix");
  if (matrix.size() != 16) return;
  Set(prim, "xformOp:transform", tn::Value::MakeMatrix4d(matrix.data()),
      "matrix4d");
  Set(prim, "xformOpOrder",
      tn::Value::MakeTokenArray(std::vector<std::string>{"xformOp:transform"}),
      "token[]", true);
}

bool JsonValue(const Json &source, tn::Value *value, std::string *type_name) {
  if (source.is_boolean()) {
    *value = tn::Value(source.get<bool>());
    *type_name = "bool";
    return true;
  }
  if (source.is_number_integer()) {
    *value = tn::Value(source.get<int32_t>());
    *type_name = "int";
    return true;
  }
  if (source.is_number()) {
    *value = tn::Value(source.get<double>());
    *type_name = "double";
    return true;
  }
  if (source.is_string()) {
    *value = tn::Value::MakeToken(source.get<std::string>());
    *type_name = "token";
    return true;
  }
  if (!source.is_array() || source.empty()) return false;
  bool strings = true;
  bool integers = true;
  bool numbers = true;
  for (const Json &item : source) {
    strings = strings && item.is_string();
    integers = integers && item.is_number_integer();
    numbers = numbers && item.is_number();
  }
  if (strings) {
    std::vector<std::string> values;
    for (const Json &item : source) values.push_back(item.get<std::string>());
    *value = tn::Value::MakeTokenArray(std::move(values));
    *type_name = "token[]";
    return true;
  }
  if (integers) {
    std::vector<int32_t> values;
    for (const Json &item : source) values.push_back(item.get<int32_t>());
    *value = tn::Value::MakeIntArray(std::move(values));
    *type_name = "int[]";
    return true;
  }
  if (numbers) {
    std::vector<double> values;
    for (const Json &item : source) values.push_back(item.get<double>());
    *value = tn::Value::MakeDoubleArray(std::move(values));
    *type_name = "double[]";
    return true;
  }
  return false;
}

void AuthorObject(tn::PrimSpec *prim, const Json &object,
                  const std::string &prefix) {
  if (!prim || !object.is_object()) return;
  const auto *members = object.object_items();
  if (!members) return;
  for (const auto &member : *members) {
    const std::string name = prefix + member.key;
    const Json &member_value = member.value();
    if (member_value.is_object()) {
      AuthorObject(prim, member_value, name + ":");
      continue;
    }
    tn::Value value;
    std::string type_name;
    if (JsonValue(member_value, &value, &type_name)) {
      Set(prim, name, std::move(value), type_name, true);
    }
  }
}

void AuthorExtensions(tn::PrimSpec *prim, const Json &source) {
  if (!source.is_object()) return;
  for (const char *name : {"mjc", "newton", "physx", "state"}) {
    if (source.contains(name) && source.at(name).is_object()) {
      AuthorObject(prim, source.at(name), std::string(name) + ":");
    }
  }
}

std::string AxisToken(const Json &joint) {
  const std::vector<float> axis = JsonFloats(joint, "axis");
  if (axis.size() < 3) return "X";
  const float ax = std::fabs(axis[0]);
  const float ay = std::fabs(axis[1]);
  const float az = std::fabs(axis[2]);
  if (ay >= ax && ay >= az) return "Y";
  if (az >= ax && az >= ay) return "Z";
  return "X";
}

void AddCollisionData(tn::PrimSpec *prim, const Json &source,
                      bool mesh, bool mjcf_source) {
  AddAPIs(prim, {"PhysicsCollisionAPI", "MjcCollisionAPI",
                 "MjcImageableAPI", "NewtonCollisionAPI"});
  if (mesh) AddAPI(prim, "PhysicsMeshCollisionAPI");
  Set(prim, "physics:collisionEnabled", tn::Value(true), "bool");
  if (mesh) {
    SetToken(prim, "physics:approximation",
             JsonString(source, "approximation", "convexHull"));
  }
  const Json *mjc = source.contains("mjc") && source.at("mjc").is_object()
                        ? &source.at("mjc")
                        : nullptr;
  const int32_t default_group = mjcf_source ? 0 : 3;
  const int32_t group = static_cast<int32_t>(
      JsonNumber(mjc ? *mjc : source, "group",
                 JsonNumber(source, "group", default_group)));
  Set(prim, "mjc:group", tn::Value(group), "int", true);
  Set(prim, "mjc:contype",
      tn::Value(static_cast<int32_t>(JsonNumber(
          mjc ? *mjc : source, "contype", JsonNumber(source, "contype", 1)))),
      "int", true);
  Set(prim, "mjc:conaffinity",
      tn::Value(static_cast<int32_t>(JsonNumber(
          mjc ? *mjc : source, "conaffinity",
          JsonNumber(source, "conaffinity", 1)))),
      "int", true);
  AuthorExtensions(prim, source);
}

bool AddMesh(tn::Layer *layer, const std::string &path, const Json &item,
             const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
             bool collision, bool mjcf_source, std::string *warn) {
  const Json &geom = item.contains("geometry") && item.at("geometry").is_object()
                         ? item.at("geometry")
                         : item;
  std::vector<float> positions = JsonFloats(geom, "positions");
  std::vector<float> normals = JsonFloats(geom, "normals");
  std::vector<float> uvs = JsonFloats(geom, "uvs");
  std::vector<int32_t> indices = JsonInts(geom, "indices");
  const std::string mesh_ref = JsonString(item, "meshRef");
  if (!mesh_ref.empty()) {
    const auto found = mesh_buffers ? mesh_buffers->find(mesh_ref)
                                    : std::map<std::string, URDFMeshBuffer>::const_iterator();
    if (!mesh_buffers || found == mesh_buffers->end()) {
      AppendWarn(warn, "Skipping meshRef `" + mesh_ref + "`: not registered.\n");
      return true;
    }
    positions = found->second.positions;
    normals = found->second.normals;
    uvs = found->second.uvs;
    indices = found->second.indices;
  }
  if (positions.size() < 9 || positions.size() % 3 != 0) {
    AppendWarn(warn, "Skipping mesh `" + path + "`: invalid positions.\n");
    return true;
  }
  const size_t point_count = positions.size() / 3;
  if (indices.empty()) {
    indices.resize(point_count);
    for (size_t i = 0; i < point_count; ++i) indices[i] = static_cast<int32_t>(i);
  }
  if (indices.size() % 3 != 0) {
    AppendWarn(warn, "Skipping mesh `" + path + "`: indices are not triangles.\n");
    return true;
  }
  for (int32_t index : indices) {
    if (index < 0 || static_cast<size_t>(index) >= point_count) {
      AppendWarn(warn, "Skipping mesh `" + path + "`: index out of range.\n");
      return true;
    }
  }

  tn::PrimSpec *prim = Define(layer, path, "Mesh");
  if (!prim) return false;
  Set(prim, "points",
      tn::Value::MakeFloatCompArray(std::move(positions), tn::TypeId::Point3f, 3),
      "point3f[]");
  Set(prim, "faceVertexIndices", tn::Value::MakeIntArray(indices), "int[]");
  Set(prim, "faceVertexCounts",
      tn::Value::MakeIntArray(std::vector<int32_t>(indices.size() / 3, 3)),
      "int[]");
  SetToken(prim, "subdivisionScheme", "none", true);
  if (normals.size() == point_count * 3) {
    Set(prim, "normals",
        tn::Value::MakeFloatCompArray(std::move(normals), tn::TypeId::Normal3f, 3),
        "normal3f[]");
    prim->ensure_property_meta("normals").interpolation = "vertex";
    prim->ensure_property_meta("normals").authored |= tn::PropMeta::kInterpolation;
  }
  if (uvs.size() == point_count * 2) {
    Set(prim, "primvars:st",
        tn::Value::MakeFloatCompArray(std::move(uvs), tn::TypeId::Texcoord2f, 2),
        "texCoord2f[]");
    prim->ensure_property_meta("primvars:st").interpolation = "vertex";
    prim->ensure_property_meta("primvars:st").authored |= tn::PropMeta::kInterpolation;
  }
  SetTransform(prim, item);
  if (collision) {
    AddCollisionData(prim, item, true, mjcf_source);
  } else {
    AddAPI(prim, "MjcImageableAPI");
    Set(prim, "mjc:group",
        tn::Value(static_cast<int32_t>(JsonNumber(item, "group", mjcf_source ? 0 : 2))),
        "int", true);
    const std::string material = JsonString(item, "material");
    if (!material.empty()) {
      AddAPI(prim, "MaterialBindingAPI");
      prim->add_relationship("material:binding",
                             tn::Path("/World/Materials/" +
                                      Sanitize(material, "material")));
    }
    AuthorExtensions(prim, item);
  }
  return true;
}

bool AddShape(tn::Layer *layer, const std::string &path, const Json &item,
              bool mjcf_source, std::string *warn) {
  const Json &shape = item.contains("shape") && item.at("shape").is_object()
                          ? item.at("shape")
                          : item;
  const std::string type = JsonString(shape, "type");
  std::string usd_type;
  if (type == "box" || type == "cube") usd_type = "Cube";
  else if (type == "sphere") usd_type = "Sphere";
  else if (type == "cylinder") usd_type = "Cylinder";
  else if (type == "capsule") usd_type = "Capsule";
  else if (type == "plane") usd_type = "Plane";
  else {
    AppendWarn(warn, "Skipping shape `" + path + "`: unsupported type `" +
                         type + "`.\n");
    return true;
  }
  tn::PrimSpec *prim = Define(layer, path, usd_type);
  if (!prim) return false;
  if (usd_type == "Cube") Set(prim, "size", tn::Value(2.0), "double");
  if (usd_type == "Sphere") {
    Set(prim, "radius", tn::Value(JsonNumber(shape, "radius", 0.5)), "double");
  }
  if (usd_type == "Cylinder" || usd_type == "Capsule") {
    Set(prim, "radius", tn::Value(JsonNumber(shape, "radius", 0.5)), "double");
    Set(prim, "height",
        tn::Value(JsonNumber(shape, "height", JsonNumber(shape, "length", 1.0))),
        "double");
    SetToken(prim, "axis", JsonString(shape, "axis", "Z"));
  }
  if (usd_type == "Plane") {
    Set(prim, "width", tn::Value(JsonNumber(shape, "width", 2.0)), "double");
    Set(prim, "length", tn::Value(JsonNumber(shape, "length", 2.0)), "double");
    SetToken(prim, "axis", JsonString(shape, "axis", "Z"));
  }
  SetTransform(prim, item);
  AddCollisionData(prim, item, false, mjcf_source);
  return true;
}

void AddJointDynamics(tn::PrimSpec *prim, const Json &joint,
                      const std::string &dof) {
  if (!joint.contains("dynamics") || !joint.at("dynamics").is_object()) return;
  const Json &dynamics = joint.at("dynamics");
  const bool angular = dof.rfind("angular", 0) == 0;
  const std::string physx = angular ? "physxLimit:angular:" : "physxLimit:linear:";
  for (const char *key : {"damping", "stiffness", "armature", "frictionloss", "ref"}) {
    if (!dynamics.contains(key) || !dynamics.at(key).is_number()) continue;
    const double value = dynamics.at(key).get<double>();
    Set(prim, std::string("mjc:") + key, tn::Value(value), "double");
    if (std::string(key) == "damping" || std::string(key) == "stiffness") {
      Set(prim, physx + key, tn::Value(static_cast<float>(value)), "float");
    } else if (std::string(key) == "armature") {
      Set(prim, "physxJoint:armature", tn::Value(static_cast<float>(value)), "float");
    } else if (std::string(key) == "frictionloss") {
      Set(prim, "physxJoint:jointFriction", tn::Value(static_cast<float>(value)), "float");
    }
  }
  AuthorExtensions(prim, dynamics);
}

void AuthorGenericScope(tn::Layer *layer, const Json &items,
                        const std::string &scope_name,
                        const std::string &prim_type,
                        const std::string &prefix) {
  if (!items.is_array() || items.empty()) return;
  Define(layer, "/World/" + scope_name, "Xform");
  std::set<std::string> used;
  for (size_t i = 0; i < items.size(); ++i) {
    const Json &item = items[i];
    const std::string name = Unique(JsonString(item, "name"),
                                    "item_" + std::to_string(i), &used);
    tn::PrimSpec *prim = Define(layer, "/World/" + scope_name + "/" + name,
                                prim_type);
    if (!prim) continue;
    AuthorObject(prim, item, prefix);
  }
}

}  // namespace

bool ConvertURDFJsonToUSDStage(
    const std::string &robot_json,
    const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
    ::lightusd::next::Stage *out_stage, std::string *warn, std::string *err) {
  if (!out_stage) {
    if (err) *err = "Output Stage pointer is null";
    return false;
  }
  if (warn) warn->clear();
  if (err) err->clear();

  ::lightusd::tydra::detail::URDFPayload payload;
  if (!::lightusd::tydra::detail::URDFPayload::Parse(robot_json, &payload,
                                                      err)) {
    return false;
  }
  const Json &root = payload.root;
  const Json &links = payload.Array("links");
  const Json &joints = payload.Array("joints");

  tn::Layer layer;
  layer.meta().defaultPrim = "World";
  layer.meta().upAxis = JsonString(root, "upAxis", "Y");
  layer.meta().upAxis_set = true;
  layer.meta().metersPerUnit = 1.0;
  layer.meta().metersPerUnit_set = true;
  layer.meta().kilogramsPerUnit = 1.0;
  layer.meta().kilogramsPerUnit_set = true;

  tn::PrimSpec *world = Define(&layer, "/World", "Xform");
  if (!world) {
    if (err) *err = "Failed to define /World";
    return false;
  }
  world->meta().kind() = "assembly";
  if (layer.meta().upAxis != "Z" && layer.meta().upAxis != "z") {
    Set(world, "xformOp:rotateX", tn::Value(-90.0), "double");
    Set(world, "xformOpOrder",
        tn::Value::MakeTokenArray(std::vector<std::string>{"xformOp:rotateX"}),
        "token[]", true);
  }

  tn::PrimSpec *scene = Define(&layer, "/World/PhysicsScene", "PhysicsScene");
  AddAPIs(scene, {"MjcSceneAPI", "NewtonSceneAPI"});
  std::vector<float> gravity = JsonFloats(root, "gravity");
  if (gravity.size() < 3) gravity = {0.0f, -1.0f, 0.0f};
  SetFloat3(scene, "physics:gravityDirection", gravity, "vector3f");
  Set(scene, "physics:gravityMagnitude", tn::Value(9.80665f), "float");
  Set(scene, "mjc:timestep", tn::Value(JsonNumber(root, "timestep", 0.002)),
      "double");
  AuthorExtensions(scene, root);

  Define(&layer, "/World/Links", "Xform");
  Define(&layer, "/World/Joints", "Xform");

  std::set<std::string> child_links;
  for (const Json &joint : joints) {
    const std::string child = JsonString(joint, "child");
    if (!child.empty()) child_links.insert(child);
  }
  std::set<std::string> used_links;
  std::map<std::string, std::string> link_names;
  for (size_t i = 0; i < links.size(); ++i) {
    const Json &link = links[i];
    const std::string source_name =
        JsonString(link, "name", "link_" + std::to_string(i));
    const std::string name = Unique(source_name, "link", &used_links);
    link_names[source_name] = name;
  }

  for (size_t i = 0; i < links.size(); ++i) {
    const Json &link = links[i];
    const std::string source_name =
        JsonString(link, "name", "link_" + std::to_string(i));
    const std::string name = link_names[source_name];
    const std::string path = "/World/Links/" + name;
    tn::PrimSpec *prim = Define(&layer, path, "Xform");
    AddAPIs(prim, {"PhysicsRigidBodyAPI", "PhysicsMassAPI"});
    const bool is_static = JsonBool(link, "static", false);
    Set(prim, "physics:rigidBodyEnabled", tn::Value(!is_static), "bool");
    Set(prim, "physics:startsAsleep", tn::Value(false), "bool");
    if (!is_static && child_links.count(source_name) == 0) {
      AddAPIs(prim, {"PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"});
      Set(prim, "newton:selfCollisionEnabled",
          tn::Value(JsonBool(root.contains("newton") ? root.at("newton") : root,
                             "selfCollisionEnabled", true)),
          "bool");
    }
    if (JsonBool(link, "mocap", false)) {
      Set(prim, "mjc:mocap", tn::Value(true), "bool", true);
    }
    if (JsonBool(link, "floating", false)) {
      Set(prim, "mjc:freeJoint", tn::Value(true), "bool", true);
    }
    SetTransform(prim, link);
    AuthorExtensions(prim, link);

    if (link.contains("inertial") && link.at("inertial").is_object()) {
      const Json &inertial = link.at("inertial");
      const double mass = JsonNumber(inertial, "mass", 0.0);
      if (mass > 0.0) Set(prim, "physics:mass", tn::Value(static_cast<float>(mass)), "float");
      SetFloat3(prim, "physics:centerOfMass",
                JsonFloats(inertial, "centerOfMass"), "point3f");
      SetFloat3(prim, "physics:diagonalInertia",
                JsonFloats(inertial, "diagonalInertia"), "float3");
      const std::vector<float> axes = JsonFloats(inertial, "principalAxes");
      if (axes.size() >= 4) {
        Set(prim, "physics:principalAxes",
            tn::Value::MakeQuatf(axes[1], axes[2], axes[3], axes[0]), "quatf");
      }
    }

    auto add_items = [&](const char *key, bool collision) -> bool {
      if (!link.contains(key) || !link.at(key).is_array()) return true;
      std::set<std::string> used;
      const Json &items = link.at(key);
      for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        const Json &item = items[item_index];
        const std::string fallback = std::string(collision ? "collision_" : "visual_") +
                                     std::to_string(item_index);
        const std::string item_name =
            Unique(JsonString(item, "name"), fallback, &used);
        const std::string item_path = path + "/" + item_name;
        if (collision && item.contains("shape") && item.at("shape").is_object()) {
          if (!AddShape(&layer, item_path, item, payload.mjcf_source, warn)) return false;
        } else if (!AddMesh(&layer, item_path, item, mesh_buffers, collision,
                            payload.mjcf_source, warn)) {
          return false;
        }
      }
      return true;
    };
    if (!add_items("visuals", false) || !add_items("collisions", true)) {
      if (err) *err = "Failed to author link geometry";
      return false;
    }
  }

  std::set<std::string> used_joints;
  std::map<std::string, std::string> joint_names;
  for (size_t i = 0; i < joints.size(); ++i) {
    const std::string source = JsonString(joints[i], "name",
                                          "joint_" + std::to_string(i));
    joint_names[source] = Unique(source, "joint", &used_joints);
  }
  constexpr double kRadToDeg = 57.2957795130823208768;
  for (size_t i = 0; i < joints.size(); ++i) {
    const Json &joint = joints[i];
    const std::string source = JsonString(joint, "name",
                                          "joint_" + std::to_string(i));
    const std::string parent = JsonString(joint, "parent");
    const std::string child = JsonString(joint, "child");
    if (!link_names.count(parent) || !link_names.count(child)) {
      AppendWarn(warn, "Skipping joint `" + source + "`: unknown link.\n");
      continue;
    }
    const std::string type = JsonString(joint, "type", "fixed");
    std::string usd_type = "PhysicsFixedJoint";
    if (type == "revolute" || type == "continuous") usd_type = "PhysicsRevoluteJoint";
    else if (type == "prismatic") usd_type = "PhysicsPrismaticJoint";
    else if (type == "spherical" || type == "ball") usd_type = "PhysicsSphericalJoint";
    tn::PrimSpec *prim = Define(&layer, "/World/Joints/" + joint_names[source],
                                usd_type);
    prim->add_relationship("physics:body0",
                           tn::Path("/World/Links/" + link_names[parent]));
    prim->add_relationship("physics:body1",
                           tn::Path("/World/Links/" + link_names[child]));
    const std::string axis = AxisToken(joint);
    if (usd_type != "PhysicsFixedJoint") SetToken(prim, "physics:axis", axis);
    SetFloat3(prim, "physics:localPos0", JsonFloats(joint, "localPos0"), "point3f");
    SetFloat3(prim, "physics:localPos1", JsonFloats(joint, "localPos1"), "point3f");
    const std::vector<float> local_rot0 = JsonFloats(joint, "localRot0");
    const std::vector<float> local_rot1 = JsonFloats(joint, "localRot1");
    if (local_rot0.size() >= 4) {
      Set(prim, "physics:localRot0",
          tn::Value::MakeQuatf(local_rot0[1], local_rot0[2], local_rot0[3], local_rot0[0]),
          "quatf");
    }
    if (local_rot1.size() >= 4) {
      Set(prim, "physics:localRot1",
          tn::Value::MakeQuatf(local_rot1[1], local_rot1[2], local_rot1[3], local_rot1[0]),
          "quatf");
    }
    const bool rotational = usd_type == "PhysicsRevoluteJoint";
    const std::string dof = std::string(rotational ? "angular" : "linear") + axis;
    if (joint.contains("limit") && joint.at("limit").is_object()) {
      const Json &limit = joint.at("limit");
      const double scale = rotational ? kRadToDeg : 1.0;
      const double low = JsonNumber(limit, "lower", 0.0) * scale;
      const double high = JsonNumber(limit, "upper", 0.0) * scale;
      Set(prim, "physics:lowerLimit", tn::Value(static_cast<float>(low)), "float");
      Set(prim, "physics:upperLimit", tn::Value(static_cast<float>(high)), "float");
      AddAPI(prim, "PhysicsLimitAPI:" + dof);
      Set(prim, "physics:limit:" + dof + ":low",
          tn::Value(static_cast<float>(low)), "float");
      Set(prim, "physics:limit:" + dof + ":high",
          tn::Value(static_cast<float>(high)), "float");
    }
    AddJointDynamics(prim, joint, dof);
    AuthorExtensions(prim, joint);
  }

  AuthorGenericScope(&layer, payload.Array("actuators"), "Actuators",
                     "NewtonActuator", "newton:");
  AuthorGenericScope(&layer, payload.Array("sites"), "Sites", "Sphere", "mjc:");
  AuthorGenericScope(&layer, payload.Array("tendons"), "Tendons", "MjcTendon", "mjc:");
  AuthorGenericScope(&layer, payload.Array("mjcActuators"), "MjcActuators",
                     "MjcActuator", "mjc:");
  AuthorGenericScope(&layer, payload.Array("equalities"), "Equalities", "Xform", "mjc:");
  AuthorGenericScope(&layer, payload.Array("keyframes"), "Keyframes", "MjcKeyframe", "mjc:");
  AuthorGenericScope(&layer, payload.Array("sensors"), "Sensors", "MjcSensor", "mjc:");
  AuthorGenericScope(&layer, payload.Array("contactPairs"), "Contacts", "Xform", "mjc:");
  AuthorGenericScope(&layer, payload.Array("lights"), "Lights", "SphereLight", "inputs:");
  AuthorGenericScope(&layer, payload.Array("cameras"), "Cameras", "Camera", "");
  AuthorGenericScope(&layer, payload.Array("materials"), "Materials", "Material", "mjc:");

  layer.finalize();
  tn::Stage stage;
  stage.SetRootLayer(std::move(layer));
  *out_stage = std::move(stage);
  return true;
}

bool ConvertURDFJsonToUSDStage(const std::string &robot_json,
                               ::lightusd::next::Stage *out_stage,
                               std::string *warn, std::string *err) {
  return ConvertURDFJsonToUSDStage(robot_json, nullptr, out_stage, warn, err);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
