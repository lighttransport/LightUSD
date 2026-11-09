// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Physics annotations to JSON Converter Implementation
//

#include "physics-to-json.hh"
#include "materialx-to-json.hh"  // for EscapeJsonString

#include <sstream>
#include <functional>
#include <vector>

#include "core/prim.hh"
#include "stage.hh"
#include "usdPhysics.hh"
#include "mjcPhysics.hh"

namespace lightusd {
namespace tydra {

namespace {

std::string Indent(int level, int spaces) {
  return std::string(static_cast<size_t>(level * spaces), ' ');
}

// JSON value helpers
std::string JsonStr(const std::string &s) {
  return "\"" + EscapeJsonString(s) + "\"";
}

std::string JsonBool(bool b) {
  return b ? "true" : "false";
}

template <typename T>
std::string JsonNum(T v) {
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

std::string JsonVec3f(const value::vector3f &v) {
  std::ostringstream oss;
  oss << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
  return oss.str();
}

std::string JsonDouble3(const value::double3 &v) {
  std::ostringstream oss;
  oss << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
  return oss.str();
}

std::string JsonDoubleArray(const std::vector<double> &arr) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << arr[i];
  }
  oss << "]";
  return oss.str();
}

std::string JsonIntArray(const std::vector<int> &arr) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << arr[i];
  }
  oss << "]";
  return oss.str();
}

std::string JsonStringArray(const std::vector<std::string> &arr) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << JsonStr(arr[i]);
  }
  oss << "]";
  return oss.str();
}

std::string JsonColor4f(const value::color4f &v) {
  std::ostringstream oss;
  oss << "[" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << "]";
  return oss.str();
}

std::string JsonValue(const bool v) { return JsonBool(v); }
std::string JsonValue(const int v) { return JsonNum(v); }
std::string JsonValue(const double v) { return JsonNum(v); }
std::string JsonValue(const value::token &v) { return JsonStr(v.str()); }
std::string JsonValue(const value::color4f &v) { return JsonColor4f(v); }

// Helper to emit a key-value pair
void EmitKV(std::ostringstream &ss, int ind, int sp, const std::string &key,
            const std::string &val, bool comma = true) {
  ss << Indent(ind, sp) << JsonStr(key) << ": " << val;
  if (comma) ss << ",";
  ss << "\n";
}

template <typename T>
void EmitFallbackAttr(std::ostringstream &ss, int ind, int sp,
                      const std::string &key,
                      const TypedAttributeWithFallback<T> &attr,
                      bool include_defaults,
                      bool comma = true) {
  if (include_defaults || attr.authored()) {
    EmitKV(ss, ind, sp, key, JsonValue(attr.get_value()), comma);
  }
}

// Helper to emit optional TypedAttribute<std::vector<T>> as JSON array
template <typename T>
void EmitOptionalArray(std::ostringstream &ss, int ind, int sp,
                       const std::string &key,
                       const TypedAttribute<std::vector<T>> &attr,
                       bool comma = true) {
  auto opt = attr.get_value();
  if (opt.has_value()) {
    if constexpr (std::is_same_v<T, double>) {
      EmitKV(ss, ind, sp, key, JsonDoubleArray(opt.value()), comma);
    } else {
      EmitKV(ss, ind, sp, key, JsonIntArray(opt.value()), comma);
    }
  }
}

void EmitOptionalRelTargets(std::ostringstream &ss, int ind, int sp,
                            const std::string &key,
                            const RelationshipProperty &rel,
                            bool comma = true) {
  if (!rel.authored() || rel.is_blocked()) return;
  std::vector<std::string> targets;
  for (const auto &path : rel.get_targetPaths()) {
    targets.push_back(path.full_path_name());
  }
  if (!targets.empty()) {
    EmitKV(ss, ind, sp, key, JsonStringArray(targets), comma);
  }
}

// Helper to get relationship target path as string
std::string RelTargetStr(const RelationshipProperty &rp) {
  if (!rp.authored()) return "";
  const auto &rel = rp.relationship();
  if (rel.is_path()) {
    return rel.targetPath.full_path_name();
  }
  return "";
}

// Remove emitter-created trailing commas without touching comma-like text
// inside JSON strings.
std::string RemoveTrailingJsonCommas(const std::string &src) {
  std::string out;
  out.reserve(src.size());

  bool in_string = false;
  bool escaped = false;
  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];

    if (in_string) {
      out.push_back(c);
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    if (c == '"') {
      in_string = true;
      out.push_back(c);
      continue;
    }

    if (c == ',') {
      size_t j = i + 1;
      while (j < src.size() &&
             (src[j] == ' ' || src[j] == '\n' || src[j] == '\r' ||
              src[j] == '\t')) {
        ++j;
      }
      if (j < src.size() && (src[j] == '}' || src[j] == ']')) {
        continue;
      }
    }

    out.push_back(c);
  }

  return out;
}

// Emit MjcSceneAPI to JSON
void EmitMjcSceneAPI(std::ostringstream &ss, const MjcSceneAPI &api,
                     int ind, int sp) {
  ss << Indent(ind, sp) << "\"mjc\": {\n";
  int i = ind + 1;

  // Option
  ss << Indent(i, sp) << "\"option\": {\n";
  int i2 = i + 1;
  EmitKV(ss, i2, sp, "timestep", JsonNum(api.timestep.get_value()));
  EmitKV(ss, i2, sp, "impratio", JsonNum(api.impratio.get_value()));
  EmitKV(ss, i2, sp, "wind", JsonDouble3(api.wind.get_value()));
  EmitKV(ss, i2, sp, "magnetic", JsonDouble3(api.magnetic.get_value()));
  EmitKV(ss, i2, sp, "density", JsonNum(api.density.get_value()));
  EmitKV(ss, i2, sp, "viscosity", JsonNum(api.viscosity.get_value()));
  EmitKV(ss, i2, sp, "o_margin", JsonNum(api.o_margin.get_value()));
  EmitOptionalArray(ss, i2, sp, "o_solref", api.o_solref);
  EmitOptionalArray(ss, i2, sp, "o_solimp", api.o_solimp);
  EmitOptionalArray(ss, i2, sp, "o_friction", api.o_friction);
  EmitKV(ss, i2, sp, "integrator", JsonStr(api.integrator.get_value().str()));
  EmitKV(ss, i2, sp, "cone", JsonStr(api.cone.get_value().str()));
  EmitKV(ss, i2, sp, "jacobian", JsonStr(api.jacobian.get_value().str()));
  EmitKV(ss, i2, sp, "solver", JsonStr(api.solver.get_value().str()));
  EmitKV(ss, i2, sp, "iterations", JsonNum(api.iterations.get_value()));
  EmitKV(ss, i2, sp, "tolerance", JsonNum(api.tolerance.get_value()));
  EmitKV(ss, i2, sp, "ls_iterations", JsonNum(api.ls_iterations.get_value()));
  EmitKV(ss, i2, sp, "ls_tolerance", JsonNum(api.ls_tolerance.get_value()));
  EmitKV(ss, i2, sp, "noslip_iterations", JsonNum(api.noslip_iterations.get_value()));
  EmitKV(ss, i2, sp, "noslip_tolerance", JsonNum(api.noslip_tolerance.get_value()));
  EmitKV(ss, i2, sp, "ccd_iterations", JsonNum(api.ccd_iterations.get_value()));
  EmitKV(ss, i2, sp, "ccd_tolerance", JsonNum(api.ccd_tolerance.get_value()));
  EmitKV(ss, i2, sp, "sdf_iterations", JsonNum(api.sdf_iterations.get_value()));
  EmitKV(ss, i2, sp, "sdf_initpoints", JsonNum(api.sdf_initpoints.get_value()));
  EmitOptionalArray(ss, i2, sp, "actuatorgroupdisable", api.actuatorgroupdisable, false);
  ss << Indent(i, sp) << "},\n";

  // Flags (all 24)
  ss << Indent(i, sp) << "\"flag\": {\n";
  EmitKV(ss, i2, sp, "constraint", JsonBool(api.flag_constraint.get_value()));
  EmitKV(ss, i2, sp, "equality", JsonBool(api.flag_equality.get_value()));
  EmitKV(ss, i2, sp, "frictionloss", JsonBool(api.flag_frictionloss.get_value()));
  EmitKV(ss, i2, sp, "limit", JsonBool(api.flag_limit.get_value()));
  EmitKV(ss, i2, sp, "contact", JsonBool(api.flag_contact.get_value()));
  EmitKV(ss, i2, sp, "spring", JsonBool(api.flag_spring.get_value()));
  EmitKV(ss, i2, sp, "damper", JsonBool(api.flag_damper.get_value()));
  EmitKV(ss, i2, sp, "gravity", JsonBool(api.flag_gravity.get_value()));
  EmitKV(ss, i2, sp, "clampctrl", JsonBool(api.flag_clampctrl.get_value()));
  EmitKV(ss, i2, sp, "warmstart", JsonBool(api.flag_warmstart.get_value()));
  EmitKV(ss, i2, sp, "filterparent", JsonBool(api.flag_filterparent.get_value()));
  EmitKV(ss, i2, sp, "actuation", JsonBool(api.flag_actuation.get_value()));
  EmitKV(ss, i2, sp, "refsafe", JsonBool(api.flag_refsafe.get_value()));
  EmitKV(ss, i2, sp, "sensor", JsonBool(api.flag_sensor.get_value()));
  EmitKV(ss, i2, sp, "midphase", JsonBool(api.flag_midphase.get_value()));
  EmitKV(ss, i2, sp, "nativeccd", JsonBool(api.flag_nativeccd.get_value()));
  EmitKV(ss, i2, sp, "eulerdamp", JsonBool(api.flag_eulerdamp.get_value()));
  EmitKV(ss, i2, sp, "autoreset", JsonBool(api.flag_autoreset.get_value()));
  EmitKV(ss, i2, sp, "island", JsonBool(api.flag_island.get_value()));
  EmitKV(ss, i2, sp, "override", JsonBool(api.flag_override.get_value()));
  EmitKV(ss, i2, sp, "energy", JsonBool(api.flag_energy.get_value()));
  EmitKV(ss, i2, sp, "fwdinv", JsonBool(api.flag_fwdinv.get_value()));
  EmitKV(ss, i2, sp, "invdiscrete", JsonBool(api.flag_invdiscrete.get_value()));
  EmitKV(ss, i2, sp, "multiccd", JsonBool(api.flag_multiccd.get_value()), false);
  ss << Indent(i, sp) << "},\n";

  // Compiler (all 14)
  ss << Indent(i, sp) << "\"compiler\": {\n";
  EmitKV(ss, i2, sp, "autoLimits", JsonBool(api.compiler_autoLimits.get_value()));
  EmitKV(ss, i2, sp, "boundMass", JsonNum(api.compiler_boundMass.get_value()));
  EmitKV(ss, i2, sp, "boundInertia", JsonNum(api.compiler_boundInertia.get_value()));
  EmitKV(ss, i2, sp, "setTotalMass", JsonNum(api.compiler_setTotalMass.get_value()));
  EmitKV(ss, i2, sp, "useThread", JsonBool(api.compiler_useThread.get_value()));
  EmitKV(ss, i2, sp, "balanceInertia", JsonBool(api.compiler_balanceInertia.get_value()));
  EmitKV(ss, i2, sp, "angle", JsonStr(api.compiler_angle.get_value().str()));
  EmitKV(ss, i2, sp, "fitAABB", JsonBool(api.compiler_fitAABB.get_value()));
  EmitKV(ss, i2, sp, "fuseStatic", JsonBool(api.compiler_fuseStatic.get_value()));
  EmitKV(ss, i2, sp, "inertiaFromGeom", JsonStr(api.compiler_inertiaFromGeom.get_value().str()));
  EmitKV(ss, i2, sp, "alignFree", JsonBool(api.compiler_alignFree.get_value()));
  EmitKV(ss, i2, sp, "inertiaGroupRangeMin", JsonNum(api.compiler_inertiaGroupRangeMin.get_value()));
  EmitKV(ss, i2, sp, "inertiaGroupRangeMax", JsonNum(api.compiler_inertiaGroupRangeMax.get_value()));
  EmitKV(ss, i2, sp, "saveInertial", JsonBool(api.compiler_saveInertial.get_value()), false);
  ss << Indent(i, sp) << "}\n";

  ss << Indent(ind, sp) << "}";
}

// Emit MjcJointAPI to JSON
void EmitMjcJointAPI(std::ostringstream &ss, const MjcJointAPI &api,
                     int ind, int sp, bool include_defaults) {
  ss << Indent(ind, sp) << "\"mjc\": {\n";
  int i = ind + 1;
  EmitFallbackAttr(ss, i, sp, "group", api.group, include_defaults);
  EmitFallbackAttr(ss, i, sp, "stiffness", api.stiffness, include_defaults);
  EmitFallbackAttr(ss, i, sp, "damping", api.damping, include_defaults);
  EmitFallbackAttr(ss, i, sp, "armature", api.armature, include_defaults);
  EmitFallbackAttr(ss, i, sp, "frictionloss", api.frictionloss,
                   include_defaults);
  EmitOptionalArray(ss, i, sp, "springdamper", api.springdamper);
  EmitFallbackAttr(ss, i, sp, "springref", api.springref, include_defaults);
  EmitFallbackAttr(ss, i, sp, "ref", api.ref, include_defaults);
  EmitFallbackAttr(ss, i, sp, "margin", api.margin, include_defaults);
  EmitOptionalArray(ss, i, sp, "solreflimit", api.solreflimit);
  EmitOptionalArray(ss, i, sp, "solimplimit", api.solimplimit);
  EmitOptionalArray(ss, i, sp, "solreffriction", api.solreffriction);
  EmitOptionalArray(ss, i, sp, "solimpfriction", api.solimpfriction);
  EmitFallbackAttr(ss, i, sp, "actuatorfrcrange_min",
                   api.actuatorfrcrange_min, include_defaults);
  EmitFallbackAttr(ss, i, sp, "actuatorfrcrange_max",
                   api.actuatorfrcrange_max, include_defaults);
  EmitFallbackAttr(ss, i, sp, "actuatorfrclimited",
                   api.actuatorfrclimited, include_defaults);
  EmitFallbackAttr(ss, i, sp, "actuatorgravcomp", api.actuatorgravcomp,
                   include_defaults, false);
  ss << Indent(ind, sp) << "}";
}

// Recursive prim traversal
using PrimVisitor = std::function<void(const Prim &, const std::string &)>;
void TraversePrims(const std::vector<Prim> &prims, const std::string &parent_path,
                   PrimVisitor visitor) {
  for (const auto &prim : prims) {
    std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrims(prim.children(), path, visitor);
  }
}

}  // anonymous namespace

bool ConvertPhysicsToJson(
    const Stage &stage,
    std::string *json_str,
    std::string *err,
    const PhysicsJsonExportOptions &options) {

  if (!json_str) {
    if (err) *err = "json_str is null";
    return false;
  }

  int sp = options.indent;

  // Collect physics prims by traversal
  struct PhysicsData {
    std::vector<std::pair<std::string, const PhysicsScene*>> scenes;
    std::vector<std::pair<std::string, const PhysicsRevoluteJoint*>> revoluteJoints;
    std::vector<std::pair<std::string, const PhysicsPrismaticJoint*>> prismaticJoints;
    std::vector<std::pair<std::string, const PhysicsSphericalJoint*>> sphericalJoints;
    std::vector<std::pair<std::string, const PhysicsFixedJoint*>> fixedJoints;
    std::vector<std::pair<std::string, const PhysicsDistanceJoint*>> distanceJoints;
    std::vector<std::pair<std::string, const MjcActuator*>> actuators;
    std::vector<std::pair<std::string, const MjcTendon*>> tendons;
    std::vector<std::pair<std::string, const MjcSensor*>> sensors;
    std::vector<std::pair<std::string, const MjcKeyframe*>> keyframes;
  } data;

  TraversePrims(stage.root_prims(), "", [&](const Prim &prim, const std::string &path) {
    if (auto *scene = prim.as<PhysicsScene>()) data.scenes.emplace_back(path, scene);
    else if (auto *revolute = prim.as<PhysicsRevoluteJoint>()) data.revoluteJoints.emplace_back(path, revolute);
    else if (auto *prismatic = prim.as<PhysicsPrismaticJoint>()) data.prismaticJoints.emplace_back(path, prismatic);
    else if (auto *spherical = prim.as<PhysicsSphericalJoint>()) data.sphericalJoints.emplace_back(path, spherical);
    else if (auto *fixed = prim.as<PhysicsFixedJoint>()) data.fixedJoints.emplace_back(path, fixed);
    else if (auto *distance = prim.as<PhysicsDistanceJoint>()) data.distanceJoints.emplace_back(path, distance);
    else if (auto *actuator = prim.as<MjcActuator>()) data.actuators.emplace_back(path, actuator);
    else if (auto *tendon = prim.as<MjcTendon>()) data.tendons.emplace_back(path, tendon);
    else if (auto *sensor = prim.as<MjcSensor>()) data.sensors.emplace_back(path, sensor);
    else if (auto *keyframe = prim.as<MjcKeyframe>()) data.keyframes.emplace_back(path, keyframe);
  });

  std::ostringstream ss;
  ss << "{\n";

  // Scenes
  if (!data.scenes.empty()) {
    const auto &[path, scene] = data.scenes[0];
    ss << Indent(1, sp) << "\"physicsScene\": {\n";
    EmitKV(ss, 2, sp, "path", JsonStr(path));
    auto gd_opt = scene->gravityDirection.get_value();
    if (gd_opt.has_value()) {
      EmitKV(ss, 2, sp, "gravityDirection", JsonVec3f(gd_opt.value()));
    }
    auto gm_opt = scene->gravityMagnitude.get_value();
    if (gm_opt.has_value()) {
      EmitKV(ss, 2, sp, "gravityMagnitude", JsonNum(gm_opt.value()),
             scene->mjcScene.has_value() && options.include_mjc);
    }

    if (scene->mjcScene.has_value() && options.include_mjc) {
      EmitMjcSceneAPI(ss, scene->mjcScene.value(), 2, sp);
      ss << "\n";
    }

    ss << Indent(1, sp) << "},\n";
  }

  // Joints (all types combined)
  {
    bool has_joints = !data.revoluteJoints.empty() || !data.prismaticJoints.empty() ||
                      !data.sphericalJoints.empty() || !data.fixedJoints.empty() ||
                      !data.distanceJoints.empty();

    ss << Indent(1, sp) << "\"joints\": [";
    if (has_joints) {
      ss << "\n";
      bool first = true;

      // Pull a numeric (float/double) value from a generic Property map by
      // key. Used to surface PhysX (`physxJoint:* / physxLimit:*`) and
      // Newton (`state:*:physics:*`) attrs that aren't consumed into any
      // typed schema struct. Returns true on success.
      auto getNum = [](const std::map<std::string, lightusd::Property> &props,
                       const std::string &key, double *out) -> bool {
        auto it = props.find(key);
        if (it == props.end()) return false;
        if (!it->second.is_attribute()) return false;
        const auto &attr = it->second.get_attribute();
        if (auto v = attr.get_value<float>())  { *out = static_cast<double>(*v); return true; }
        if (auto v = attr.get_value<double>()) { *out = *v; return true; }
        return false;
      };

      auto emitJoint = [&](const std::string &path, const std::string &type,
                           const PhysicsJointBase &base,
                           const std::map<std::string, lightusd::Property> &props,
                           auto emitTypedFields) {
        if (!first) ss << ",\n";
        first = false;
        ss << Indent(2, sp) << "{\n";
        EmitKV(ss, 3, sp, "path", JsonStr(path));
        EmitKV(ss, 3, sp, "type", JsonStr(type));
        auto b0 = RelTargetStr(base.body0);
        auto b1 = RelTargetStr(base.body1);
        if (!b0.empty()) EmitKV(ss, 3, sp, "body0", JsonStr(b0));
        if (!b1.empty()) EmitKV(ss, 3, sp, "body1", JsonStr(b1));
        emitTypedFields();
        bool has_mjc = base.mjcJoint.has_value() && options.include_mjc;
        {
          if (base.jointEnabled.authored()) {
            EmitKV(ss, 3, sp, "jointEnabled",
                   JsonBool(base.jointEnabled.get_value()));
          }
          if (base.breakForce.authored()) {
            EmitKV(ss, 3, sp, "breakForce",
                   JsonNum(base.breakForce.get_value()));
          }
          if (base.breakTorque.authored()) {
            EmitKV(ss, 3, sp, "breakTorque",
                   JsonNum(base.breakTorque.get_value()));
          }
        }
        // PhysX / Newton mirror block. Authored under physxJoint:* /
        // physxLimit:{angular,linear}:* / state:{angular,linear}:physics:*.
        // Emit a structured `physx` object when any key is present so JSON
        // consumers can iterate it without re-deriving the namespace
        // priority chain. See doc/usd.md "Cross-engine attribute mirror".
        const bool emit_revolute = (type == "PhysicsRevoluteJoint");
        const std::string limitNs = emit_revolute ? "physxLimit:angular" : "physxLimit:linear";
        const std::string stateNs = emit_revolute ? "state:angular:physics" : "state:linear:physics";
        double v = 0.0;
        std::vector<std::pair<std::string, double>> physx_block;
        std::vector<std::pair<std::string, double>> state_block;
        if (getNum(props, "physxJoint:armature", &v)) physx_block.push_back({"armature", v});
        if (getNum(props, "physxJoint:jointFriction", &v)) physx_block.push_back({"jointFriction", v});
        if (getNum(props, "physxJoint:maxJointVelocity", &v)) physx_block.push_back({"maxJointVelocity", v});
        if (getNum(props, limitNs + ":damping", &v)) physx_block.push_back({"damping", v});
        if (getNum(props, limitNs + ":stiffness", &v)) physx_block.push_back({"stiffness", v});
        if (getNum(props, stateNs + ":position", &v)) state_block.push_back({"position", v});
        if (getNum(props, stateNs + ":velocity", &v)) state_block.push_back({"velocity", v});
        const bool has_physx = !physx_block.empty();
        const bool has_state = !state_block.empty();
        if (has_physx) {
          ss << Indent(3, sp) << "\"physx\": {\n";
          for (size_t i = 0; i < physx_block.size(); ++i) {
            EmitKV(ss, 4, sp, physx_block[i].first, JsonNum(physx_block[i].second),
                   i + 1 < physx_block.size());
          }
          ss << Indent(3, sp) << "}";
          ss << ((has_state || has_mjc) ? ",\n" : "\n");
        }
        if (has_state) {
          ss << Indent(3, sp) << "\"state\": {\n";
          for (size_t i = 0; i < state_block.size(); ++i) {
            EmitKV(ss, 4, sp, state_block[i].first, JsonNum(state_block[i].second),
                   i + 1 < state_block.size());
          }
          ss << Indent(3, sp) << "}";
          ss << (has_mjc ? ",\n" : "\n");
        }
        if (has_mjc) {
          EmitMjcJointAPI(ss, base.mjcJoint.value(), 3, sp,
                          options.include_defaults);
          ss << "\n";
        }
        ss << Indent(2, sp) << "}";
      };

      for (const auto &item : data.revoluteJoints) {
        const auto &p = item.first;
        const auto &j = item.second;
        emitJoint(p, "PhysicsRevoluteJoint", *j, j->props, [&]() {
          auto axis = j->axis.get_value();
          if (axis.has_value()) EmitKV(ss, 3, sp, "axis", JsonStr(axis.value().str()));
          auto lo = j->lowerLimit.get_value();
          if (lo.has_value()) EmitKV(ss, 3, sp, "lowerLimit", JsonNum(lo.value()));
          auto hi = j->upperLimit.get_value();
          if (hi.has_value()) EmitKV(ss, 3, sp, "upperLimit", JsonNum(hi.value()));
        });
      }
      for (const auto &item : data.prismaticJoints) {
        const auto &p = item.first;
        const auto &j = item.second;
        emitJoint(p, "PhysicsPrismaticJoint", *j, j->props, [&]() {
          auto axis = j->axis.get_value();
          if (axis.has_value()) EmitKV(ss, 3, sp, "axis", JsonStr(axis.value().str()));
          auto lo = j->lowerLimit.get_value();
          if (lo.has_value()) EmitKV(ss, 3, sp, "lowerLimit", JsonNum(lo.value()));
          auto hi = j->upperLimit.get_value();
          if (hi.has_value()) EmitKV(ss, 3, sp, "upperLimit", JsonNum(hi.value()));
        });
      }
      for (const auto &item : data.sphericalJoints) {
        const auto &p = item.first;
        const auto &j = item.second;
        emitJoint(p, "PhysicsSphericalJoint", *j, j->props, [&]() {
          auto axis = j->axis.get_value();
          if (axis.has_value()) EmitKV(ss, 3, sp, "axis", JsonStr(axis.value().str()));
          auto c0 = j->coneAngle0Limit.get_value();
          if (c0.has_value()) EmitKV(ss, 3, sp, "coneAngle0Limit", JsonNum(c0.value()));
          auto c1 = j->coneAngle1Limit.get_value();
          if (c1.has_value()) EmitKV(ss, 3, sp, "coneAngle1Limit", JsonNum(c1.value()));
        });
      }
      for (const auto &item : data.fixedJoints) {
        const auto &p = item.first;
        const auto &j = item.second;
        emitJoint(p, "PhysicsFixedJoint", *j, j->props, []() {});
      }
      for (const auto &item : data.distanceJoints) {
        const auto &p = item.first;
        const auto &j = item.second;
        emitJoint(p, "PhysicsDistanceJoint", *j, j->props, [&]() {
          auto lo = j->minDistance.get_value();
          if (lo.has_value()) EmitKV(ss, 3, sp, "minDistance", JsonNum(lo.value()));
          auto hi = j->maxDistance.get_value();
          if (hi.has_value()) EmitKV(ss, 3, sp, "maxDistance", JsonNum(hi.value()));
        });
      }

      ss << "\n" << Indent(1, sp);
    }
    ss << "],\n";
  }

  // Actuators
  ss << Indent(1, sp) << "\"actuators\": [";
  if (!data.actuators.empty()) {
    ss << "\n";
    for (size_t i = 0; i < data.actuators.size(); ++i) {
      const auto &[path, act] = data.actuators[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      auto tgt = RelTargetStr(act->target);
      if (!tgt.empty()) EmitKV(ss, 3, sp, "target", JsonStr(tgt));
      EmitFallbackAttr(ss, 3, sp, "group", act->group,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "ctrlLimited", act->ctrlLimited,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "forceLimited", act->forceLimited,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actLimited", act->actLimited,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "ctrlRange_min", act->ctrlRange_min,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "ctrlRange_max", act->ctrlRange_max,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "forceRange_min", act->forceRange_min,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "forceRange_max", act->forceRange_max,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actRange_min", act->actRange_min,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actRange_max", act->actRange_max,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "lengthRange_min", act->lengthRange_min,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "lengthRange_max", act->lengthRange_max,
                       options.include_defaults);
      EmitOptionalArray(ss, 3, sp, "gear", act->gear);
      EmitFallbackAttr(ss, 3, sp, "crankLength", act->crankLength,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "jointInParent", act->jointInParent,
                       options.include_defaults);
      {
        auto ref = RelTargetStr(act->refSite);
        if (!ref.empty()) EmitKV(ss, 3, sp, "refSite", JsonStr(ref));
      }
      {
        auto slider = RelTargetStr(act->sliderSite);
        if (!slider.empty()) EmitKV(ss, 3, sp, "sliderSite", JsonStr(slider));
      }
      EmitFallbackAttr(ss, 3, sp, "actDim", act->actDim,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "dynType", act->dynType,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "gainType", act->gainType,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "biasType", act->biasType,
                       options.include_defaults);
      EmitOptionalArray(ss, 3, sp, "dynPrm", act->dynPrm);
      EmitOptionalArray(ss, 3, sp, "gainPrm", act->gainPrm);
      EmitOptionalArray(ss, 3, sp, "biasPrm", act->biasPrm);
      EmitFallbackAttr(ss, 3, sp, "actEarly", act->actEarly,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "inheritRange", act->inheritRange,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "plugin", act->plugin,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "instance", act->instance,
                       options.include_defaults, false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.actuators.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp);
  }
  ss << "],\n";

  // Tendons
  ss << Indent(1, sp) << "\"tendons\": [";
  if (!data.tendons.empty()) {
    ss << "\n";
    for (size_t i = 0; i < data.tendons.size(); ++i) {
      const auto &[path, t] = data.tendons[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitFallbackAttr(ss, 3, sp, "type", t->type,
                       options.include_defaults);
      EmitOptionalRelTargets(ss, 3, sp, "route", t->path);
      EmitOptionalRelTargets(ss, 3, sp, "sideSites", t->sideSites);
      EmitOptionalArray(ss, 3, sp, "routeIndices", t->path_indices);
      EmitOptionalArray(ss, 3, sp, "sideSiteIndices", t->sideSites_indices);
      EmitOptionalArray(ss, 3, sp, "routeSegments", t->path_segments);
      EmitOptionalArray(ss, 3, sp, "routeDivisors", t->path_divisors);
      EmitOptionalArray(ss, 3, sp, "routeCoef", t->path_coef);
      EmitFallbackAttr(ss, 3, sp, "group", t->group,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "limited", t->limited,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actuatorfrclimited",
                       t->actuatorfrclimited, options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "range_min", t->range_min,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "range_max", t->range_max,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actuatorfrcrange_min",
                       t->actuatorfrcrange_min, options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "actuatorfrcrange_max",
                       t->actuatorfrcrange_max, options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "stiffness", t->stiffness,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "damping", t->damping,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "armature", t->armature,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "frictionloss", t->frictionloss,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "margin", t->margin,
                       options.include_defaults);
      EmitOptionalArray(ss, 3, sp, "springlength", t->springlength);
      EmitOptionalArray(ss, 3, sp, "solreflimit", t->solreflimit);
      EmitOptionalArray(ss, 3, sp, "solimplimit", t->solimplimit);
      EmitOptionalArray(ss, 3, sp, "solreffriction", t->solreffriction);
      EmitOptionalArray(ss, 3, sp, "solimpfriction", t->solimpfriction);
      EmitFallbackAttr(ss, 3, sp, "width", t->width,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "rgba", t->rgba,
                       options.include_defaults, false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.tendons.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp);
  }
  ss << "],\n";

  // Sensors
  ss << Indent(1, sp) << "\"sensors\": [";
  if (!data.sensors.empty()) {
    ss << "\n";
    for (size_t i = 0; i < data.sensors.size(); ++i) {
      const auto &[path, sensor] = data.sensors[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "name", JsonStr(sensor->name));
      EmitFallbackAttr(ss, 3, sp, "type", sensor->type,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "objtype", sensor->objType,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "objname", sensor->objName,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "reftype", sensor->refType,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "refname", sensor->refName,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "group", sensor->group,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "cutoff", sensor->cutoff,
                       options.include_defaults);
      EmitFallbackAttr(ss, 3, sp, "noise", sensor->noise,
                       options.include_defaults);
      EmitOptionalArray(ss, 3, sp, "user", sensor->user, false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.sensors.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp);
  }
  ss << "],\n";

  // Keyframes
  ss << Indent(1, sp) << "\"keyframes\": [";
  if (!data.keyframes.empty()) {
    ss << "\n";
    for (size_t i = 0; i < data.keyframes.size(); ++i) {
      const auto &[path, kf] = data.keyframes[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "name", JsonStr(kf->name));
      EmitOptionalArray(ss, 3, sp, "qpos", kf->qpos);
      EmitOptionalArray(ss, 3, sp, "qvel", kf->qvel);
      EmitOptionalArray(ss, 3, sp, "act", kf->act);
      EmitOptionalArray(ss, 3, sp, "ctrl", kf->ctrl);
      EmitOptionalArray(ss, 3, sp, "mpos", kf->mpos);
      EmitOptionalArray(ss, 3, sp, "mquat", kf->mquat, false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.keyframes.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp);
  }
  ss << "]\n";

  ss << "}\n";

  // Emitters may leave trailing commas when optional fields are absent.
  std::string result = RemoveTrailingJsonCommas(ss.str());
  *json_str = std::move(result);
  return true;
}

}  // namespace tydra
}  // namespace lightusd
