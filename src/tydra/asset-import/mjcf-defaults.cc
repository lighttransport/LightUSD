// SPDX-License-Identifier: Apache-2.0
// Default classes and effective attributes.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
Json MeshPayloadToJson(const MeshPayload& payload) {
  if (payload.shape.is_object())
    return {{"name", payload.name},
            {"matrix", Array(payload.matrix)},
            {"shape", payload.shape}};
  Json j = MeshRefJSON(payload.mesh, *active_reader);
  j["name"] = payload.name;
  j["matrix"] = Array(payload.matrix);
  return j;
}

std::string Eff(const pugi::xml_node& node, const AttrMap& cls,
                const std::string& name, const std::string& fallback) {
  if (node && node.attribute(name.c_str())) {
    return node.attribute(name.c_str()).as_string();
  }
  const auto it = cls.find(name);
  return it != cls.end() ? it->second : fallback;
}

bool HasEff(const pugi::xml_node& node, const AttrMap& cls,
            const std::string& name) {
  return (node && node.attribute(name.c_str())) || cls.count(name) > 0;
}

double EffDouble(const pugi::xml_node& node, const AttrMap& cls,
                 const std::string& name, double fallback) {
  if (!HasEff(node, cls, name)) return fallback;
  const std::string s = Eff(node, cls, name);
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || end == s.c_str() || *end || !std::isfinite(v)) {
    active_reader->failure = "Invalid XML number for " + name;
    return fallback;
  }
  return v;
}

bool IsMjcfActuatorTag(const std::string& tag) {
  static const std::set<std::string> kTags = {"general", "motor", "position", "velocity",
                                              "intvelocity", "damper", "cylinder", "muscle",
                                              "adhesion"};
  return kTags.count(tag) > 0;
}

bool IsMjcfShortcutParam(const std::string& key) {
  static const std::set<std::string> kParams = {
      "kp", "kv", "dampratio", "inheritrange", "timeconst", "bias", "area", "diameter",
      "tausmooth", "range", "force", "scale", "lmin", "lmax", "vmax", "fpmax", "fvmax", "gain"};
  return kParams.count(key) > 0;
}

// One <default> tree, preorder (a later class definition wins), with an
// explicit stack: nesting depth comes from the input.
void WalkDefault(const pugi::xml_node& top, Defaults* d) {
  struct State {
    AttrMap geom, joint, actuator;
    std::string type, name;
  };
  std::deque<State> states;  // stable addresses: children read their parent's
  const State none;
  std::vector<std::pair<pugi::xml_node, const State*>> stack{{top, &none}};
  while (!stack.empty()) {
    const pugi::xml_node node = stack.back().first;
    const State& parent = *stack.back().second;
    stack.pop_back();
    states.push_back(parent);
    State& st = states.back();
    st.type = parent.type;
    for (auto c = node.first_child(); c; c = c.next_sibling()) {
      if (c.type() != pugi::node_element || !IsMjcfActuatorTag(c.name())) continue;
      for (const auto& a : c.attributes()) st.actuator[a.name()] = a.value();
      st.type = c.name();
    }
    if (const auto gn = Child(node, "geom")) {
      for (const auto& a : gn.attributes()) st.geom[a.name()] = a.value();
    }
    if (const auto jn = Child(node, "joint")) {
      for (const auto& a : jn.attributes()) st.joint[a.name()] = a.value();
    }
    const std::string cls = Attr(node, "class");
    if (cls.empty() || cls == "main") {
      d->root_geom = st.geom;
      d->root_joint = st.joint;
      d->root_actuator = st.actuator;
      d->root_actuator_type = st.type;
    }
    st.name = cls.empty() ? std::string("main") : cls;
    if (!cls.empty()) {
      d->geom[cls] = st.geom;
      d->joint[cls] = st.joint;
      d->actuator[cls] = st.actuator;
      d->actuator_type[cls] = st.type;
      d->actuator_parent[cls] = parent.name;
    }
    const auto children = Children(node, "default");
    for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back({*it, &st});
  }
}

Defaults ParseDefaults(const pugi::xml_node& root) {
  Defaults d;
  for (const auto& def : Children(root, "default")) {
    WalkDefault(def, &d);
  }
  return d;
}

const AttrMap& ResolveGeomClass(const pugi::xml_node& geom_node,
                                const Defaults& defaults,
                                const std::string& childclass) {
  std::string cls = Attr(geom_node, "class");
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.geom.find(cls);
    if (it != defaults.geom.end()) return it->second;
  }
  return defaults.root_geom;
}

const AttrMap& ResolveJointClass(const pugi::xml_node& joint_node,
                                 const Defaults& defaults,
                                 const std::string& childclass) {
  std::string cls = joint_node ? Attr(joint_node, "class") : std::string();
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.joint.find(cls);
    if (it != defaults.joint.end()) return it->second;
  }
  return defaults.root_joint;
}

void AddGeomPhysicsAttrs(const pugi::xml_node& geom_node, const AttrMap& cls,
                         Json* geom_json) {
  if (!geom_json) return;
  if (HasEff(geom_node, cls, "group")) {
    (*geom_json)["group"] =
        ToInt32(EffDouble(geom_node, cls, "group", 3.0), "group");
  }
  if (HasEff(geom_node, cls, "condim")) {
    (*geom_json)["condim"] =
        ToInt32(EffDouble(geom_node, cls, "condim", 3.0), "condim");
  }
  if (HasEff(geom_node, cls, "margin")) {
    (*geom_json)["mjc"]["margin"] = EffDouble(geom_node, cls, "margin", 0.0);
  }
  if (HasEff(geom_node, cls, "solmix")) {
    (*geom_json)["mjc"]["solmix"] = EffDouble(geom_node, cls, "solmix", 1.0);
  }
  for (const char* key : {"contype", "conaffinity", "priority"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][key] =
          ToInt32(EffDouble(geom_node, cls, key, 0), key);
  if (HasEff(geom_node, cls, "gap"))
    (*geom_json)["mjc"]["gap"] = EffDouble(geom_node, cls, "gap", 0);
  for (const char* key : {"friction", "solref", "solimp", "size"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][std::string(key) == "size" ? "geomSize" : key] =
          Array(ParseDoubles(Eff(geom_node, cls, key)));
  // Geom mass/density drive MuJoCo's inferred body inertia; non-legacy
  // profiles author them as collider PhysicsMassAPI values.
  for (const char* key : {"mass", "density"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][key] = EffDouble(geom_node, cls, key, 0.0);
}

fs::path ResolveMeshPath(const fs::path& base_dir, const fs::path& mesh_dir,
                         const std::string& file) {
  if (fs::path(file).is_absolute()) return fs::path(file);
  if (active_reader->options.read || active_reader->options.open) {
    const auto colon = file.find(':');
    const auto slash = file.find_first_of("/\\");
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
      return fs::path(file);
    return mesh_dir / file;
  }
  std::error_code ec;
  const std::array<fs::path, 3> candidates{
      {mesh_dir / file, base_dir / "assets" / file, base_dir / file}};
  for (const auto& c : candidates) {
    if (fs::exists(c, ec)) return c;
  }
  return mesh_dir / file;  // primary candidate; errors later with a clear msg
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
