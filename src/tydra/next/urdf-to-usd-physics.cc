// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
#include "urdf-to-usd-internal.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {

void JacobiEigenSymmetric3(const double in[3][3], double eval[3],
                           double evec[3][3]) {
  double a[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) a[i][j] = in[i][j];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) evec[i][j] = (i == j) ? 1.0 : 0.0;

  for (int sweep = 0; sweep < 100; sweep++) {
    double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
    if (off < 1e-18) break;
    for (int p = 0; p < 2; p++) {
      for (int q = p + 1; q < 3; q++) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        // Apply the Givens rotation a := J^T a J for indices (p, q).
        const double app = a[p][p], aqq = a[q][q], apq = a[p][q];
        a[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a[p][q] = a[q][p] = 0.0;
        const int r = 3 - p - q;  // the third index
        const double arp = a[r][p], arq = a[r][q];
        a[r][p] = a[p][r] = c * arp - s * arq;
        a[r][q] = a[q][r] = s * arp + c * arq;
        // Accumulate the eigenvectors.
        for (int k = 0; k < 3; k++) {
          const double ekp = evec[k][p], ekq = evec[k][q];
          evec[k][p] = c * ekp - s * ekq;
          evec[k][q] = s * ekp + c * ekq;
        }
      }
    }
  }
  for (int i = 0; i < 3; i++) eval[i] = a[i][i];
}

tn::Value RotationMatrixToQuatf(double r[3][3]) {
  const double det =
      r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
      r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
      r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  if (det < 0.0) {
    r[0][2] = -r[0][2];
    r[1][2] = -r[1][2];
    r[2][2] = -r[2][2];
  }
  const double tr = r[0][0] + r[1][1] + r[2][2];
  double w, x, y, z;
  if (tr > 0.0) {
    double S = std::sqrt(tr + 1.0) * 2.0;
    w = 0.25 * S;
    x = (r[2][1] - r[1][2]) / S;
    y = (r[0][2] - r[2][0]) / S;
    z = (r[1][0] - r[0][1]) / S;
  } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
    double S = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
    w = (r[2][1] - r[1][2]) / S;
    x = 0.25 * S;
    y = (r[0][1] + r[1][0]) / S;
    z = (r[0][2] + r[2][0]) / S;
  } else if (r[1][1] > r[2][2]) {
    double S = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
    w = (r[0][2] - r[2][0]) / S;
    x = (r[0][1] + r[1][0]) / S;
    y = 0.25 * S;
    z = (r[1][2] + r[2][1]) / S;
  } else {
    double S = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
    w = (r[1][0] - r[0][1]) / S;
    x = (r[0][2] + r[2][0]) / S;
    y = (r[1][2] + r[2][1]) / S;
    z = 0.25 * S;
  }
  // next quat Values store USD text order: (real, i, j, k).
  return tn::Value::MakeQuatf(static_cast<float>(w), static_cast<float>(x),
                              static_cast<float>(y), static_cast<float>(z));
}

// Body-frame inertia for non-legacy profiles. UsdPhysics stores principal
// moments + principal-axes rotation; NewtonMassAPI additionally carries the
// full [Ixx, Iyy, Izz, Ixy, Ixz, Iyz] tensor that Newton and MuJoCo's USD
// decoder prefer (mujoco-usd-converter / urdf-usd-converter parity). An
// all-zero tensor is left unauthored so consumers infer it from colliders.
void AuthorInertia(tn::PrimSpec *prim, const Json &inertial) {
  double tensor[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  const std::vector<double> full = JsonDoubles(inertial, "fullInertia");
  const std::vector<double> diag = JsonDoubles(inertial, "diagonalInertia");
  std::vector<double> axes = JsonDoubles(inertial, "principalAxes");
  if (full.size() >= 6) {
    const double t[3][3] = {{full[0], full[3], full[4]},
                            {full[3], full[1], full[5]},
                            {full[4], full[5], full[2]}};
    std::copy(&t[0][0], &t[0][0] + 9, &tensor[0][0]);
  } else if (diag.size() >= 3) {
    double w = 1, x = 0, y = 0, z = 0;
    if (axes.size() == 4) {
      const double n = std::sqrt(axes[0] * axes[0] + axes[1] * axes[1] +
                                 axes[2] * axes[2] + axes[3] * axes[3]);
      if (n > 1e-12 && std::isfinite(n)) {
        w = axes[0] / n; x = axes[1] / n; y = axes[2] / n; z = axes[3] / n;
      }
    }
    const double r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) tensor[i][j] += r[i][k] * diag[k] * r[j][k];
    axes = {w, x, y, z};
  } else {
    return;
  }
  double magnitude = 0.0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      if (!std::isfinite(tensor[i][j])) return;
      magnitude = std::max(magnitude, std::fabs(tensor[i][j]));
    }
  if (magnitude == 0.0) return;
  if (full.size() >= 6) {
    double eval[3];
    double evec[3][3];
    JacobiEigenSymmetric3(tensor, eval, evec);
    Set(prim, "physics:diagonalInertia",
        tn::Value::MakeFloat3(static_cast<float>(eval[0]),
                              static_cast<float>(eval[1]),
                              static_cast<float>(eval[2])),
        "float3");
    Set(prim, "physics:principalAxes", RotationMatrixToQuatf(evec), "quatf");
  } else {
    Set(prim, "physics:diagonalInertia",
        tn::Value::MakeFloat3(static_cast<float>(diag[0]),
                              static_cast<float>(diag[1]),
                              static_cast<float>(diag[2])),
        "float3");
    Set(prim, "physics:principalAxes",
        tn::Value::MakeQuatf(static_cast<float>(axes[0]), static_cast<float>(axes[1]),
                             static_cast<float>(axes[2]), static_cast<float>(axes[3])),
        "quatf");
  }
  if (ActiveProfile().newton) {
    AddAPI(prim, "NewtonMassAPI");
    SetDoubles(prim, "newton:inertia",
               {tensor[0][0], tensor[1][1], tensor[2][2], tensor[0][1],
                tensor[0][2], tensor[1][2]});
  }
}

std::string AxisToken(const Json &joint) {
  const std::string authored = JsonString(joint, "axisToken");
  if (!authored.empty()) return authored;
  const std::vector<float> axis = JsonFloats(joint, "axis");
  if (axis.size() < 3) return "X";
  const float ax = std::fabs(axis[0]);
  const float ay = std::fabs(axis[1]);
  const float az = std::fabs(axis[2]);
  if (ay >= ax && ay >= az) return "Y";
  if (az >= ax && az >= ay) return "Z";
  return "X";
}

std::string JointDofName(const std::string &axis, bool rotational) {
  const char c = axis.empty() ? 'X' : axis[0];
  return std::string(rotational ? "rot" : "trans") + c;
}

void SetPoint3(tn::PrimSpec *prim, const std::string &name,
               const std::vector<float> &v, bool uniform) {
  Set(prim, name, tn::Value::MakePoint3f(v[0], v[1], v[2]), "point3f",
      uniform);
}

void AssignJointBase(tn::PrimSpec *prim, const Json &joint,
                     const std::string &parent, const std::string &child,
                     const NameMap &joint_names) {
  if (!parent.empty()) SetRel(prim, "physics:body0", {"/World/Links/" + parent});
  SetRel(prim, "physics:body1", {"/World/Links/" + child});
  std::vector<float> pos0 = JsonFloats(joint, "localPos0");
  if (pos0.size() < 3) {
    const std::vector<double> m = JsonDoubles(joint, "originMatrix");
    if (m.size() == 16) {
      pos0 = {static_cast<float>(m[12]), static_cast<float>(m[13]),
              static_cast<float>(m[14])};
    } else {
      pos0 = JsonFloats(joint, "origin");
      if (pos0.size() < 3) pos0 = {0.0f, 0.0f, 0.0f};
    }
  }
  SetPoint3(prim, "physics:localPos0", pos0, true);
  std::vector<float> pos1 = JsonFloats(joint, "localPos1");
  if (pos1.size() < 3) pos1 = {0.0f, 0.0f, 0.0f};
  SetPoint3(prim, "physics:localPos1", pos1, true);
  for (const char *key : {"localRot0", "localRot1"}) {
    // JSON and next quat Values both use USD text order: real, i, j, k.
    const std::vector<float> q = JsonFloats(joint, key);
    tn::Value rot = q.size() >= 4 ? tn::Value::MakeQuatf(q[0], q[1], q[2], q[3])
                                  : tn::Value::MakeQuatf(1.0f, 0.0f, 0.0f, 0.0f);
    Set(prim, std::string("physics:") + key, std::move(rot), "quatf", true);
  }
  Set(prim, "physics:jointEnabled", tn::Value(true), "bool", true);
  Set(prim, "physics:collisionEnabled", tn::Value(false), "bool", true);

  const std::string subtype = JsonString(joint, "type");
  const bool is_revolute = subtype == "revolute" || subtype == "continuous";
  const bool is_prismatic = subtype == "prismatic";
  const bool angular = is_revolute || subtype == "spherical";
  constexpr double kPi = 3.14159265358979323846;
  const Profile &profile = ActiveProfile();
  if (profile.legacy) {
    // revolute -> physxLimit:angular:*, prismatic -> physxLimit:linear:*;
    // other joint types keep the mjc:* attributes only (no PhysX analog).
    // Historical mapping, retained byte for byte.
    const bool legacy_revolute = subtype == "revolute";
    const std::string limit_ns =
        is_prismatic ? "physxLimit:linear:" : "physxLimit:angular:";
    if (const Json *dyn = JsonObject(joint, "dynamics")) {
      double value = 0.0;
      if (ReadNumber(*dyn, "damping", &value)) {
        Set(prim, "mjc:damping", tn::Value(value), "double", true);
        if (legacy_revolute || is_prismatic) {
          Set(prim, limit_ns + "damping", tn::Value(value), "double");
        }
      }
      if (ReadNumber(*dyn, "friction", &value)) {
        Set(prim, "mjc:frictionloss", tn::Value(value), "double", true);
        Set(prim, "physxJoint:jointFriction", tn::Value(value), "double");
      }
      if (ReadNumber(*dyn, "stiffness", &value)) {
        Set(prim, "mjc:stiffness", tn::Value(value), "double");
        if (legacy_revolute || is_prismatic) {
          Set(prim, limit_ns + "stiffness", tn::Value(value), "double");
        }
      }
      if (ReadNumber(*dyn, "armature", &value)) {
        Set(prim, "mjc:armature", tn::Value(value), "double");
        Set(prim, "physxJoint:armature", tn::Value(value), "double");
      }
    }
  } else {
    // NewtonJointAPI carries passive joint dynamics for Newton 1.8 and
    // MuJoCo 3.16's USD decoder; angular gains are per degree. The deprecated
    // mjc:armature/frictionloss/damping aliases would take precedence in
    // MuJoCo's decoder, so they are not authored. newton:limitStiffness/
    // limitDamping are left unauthored: MJCF limits are solref-based
    // (mjc:solreflimit), matching mujoco-usd-converter 0.8.
    const Json *dyn = JsonObject(joint, "dynamics");
    double value = 0.0;
    bool newton_joint = false;
    auto newton = [&](const char *name, double v) {
      Set(prim, name, tn::Value(static_cast<float>(v)), "float");
      newton_joint = true;
    };
    if (dyn && ReadNumber(*dyn, "armature", &value)) newton("newton:armature", value);
    if (dyn && ReadNumber(*dyn, "friction", &value)) newton("newton:friction", value);
    if (dyn && ReadNumber(*dyn, "damping", &value))
      newton("newton:damping", angular ? value * kPi / 180.0 : value);
    if (const Json *limit = JsonObject(joint, "limit")) {
      // URDF <limit velocity> is rad/s (or m/s); NewtonJointAPI uses deg/s.
      if (ReadNumber(*limit, "velocity", &value) && value > 0.0)
        newton("newton:velocityLimit", angular ? value * 180.0 / kPi : value);
      if (ReadNumber(*limit, "effort", &value)) {
        Set(prim, "urdf:limit:effort", tn::Value(value), "double");
        // The joint force limit Newton (Mjc resolver) and MuJoCo read; URDF
        // effort 0 conventionally means "unspecified".
        const Json *mjc = JsonObject(joint, "mjc");
        if (value > 0.0 && !(mjc && mjc->contains("actuatorfrcrange"))) {
          AddAPI(prim, "MjcJointAPI");
          SetToken(prim, "mjc:actuatorfrclimited", "true", true);
          Set(prim, "mjc:actuatorfrcrange:min", tn::Value(-value), "double", true);
          Set(prim, "mjc:actuatorfrcrange:max", tn::Value(value), "double", true);
        }
      }
    }
    if (newton_joint && profile.newton) AddAPI(prim, "NewtonJointAPI");
    // URDF-only metadata without a schema home (urdf-usd-converter parity).
    for (const char *section : {"safety_controller", "calibration"}) {
      const Json *object = JsonObject(joint, section);
      if (!object || !object->object_items()) continue;
      for (const auto &member : *object->object_items()) {
        if (!member.value().is_number()) continue;
        Set(prim, std::string("urdf:") + section + ":" + member.key,
            tn::Value(member.value().get<double>()), "double");
      }
    }
    if (const Json *mjc = JsonObject(joint, "mjc")) {
      for (const char *key : {"ref", "springref", "margin", "stiffness"})
        if (ReadNumber(*mjc, key, &value))
          Set(prim, std::string("mjc:") + key, tn::Value(value), "double", true);
      for (const char *key : {"solreflimit", "solimplimit", "solreffriction",
                              "solimpfriction", "springdamper"}) {
        std::vector<double> values = JsonDoubles(*mjc, key);
        if (!values.empty())
          SetDoubles(prim, std::string("mjc:") + key, std::move(values), true);
      }
      double lo = 0.0, hi = 0.0;
      if (ReadRange(*mjc, "actuatorfrcrange", &lo, &hi)) {
        Set(prim, "mjc:actuatorfrcrange:min", tn::Value(lo), "double", true);
        Set(prim, "mjc:actuatorfrcrange:max", tn::Value(hi), "double", true);
      }
      const std::string limited = JsonString(*mjc, "actuatorfrclimited");
      if (!limited.empty()) SetToken(prim, "mjc:actuatorfrclimited", limited, true);
      bool flag = false;
      if (ReadBool(*mjc, "actuatorgravcomp", &flag))
        Set(prim, "mjc:actuatorgravcomp", tn::Value(flag), "bool", true);
      if (mjc->contains("group"))
        Set(prim, "mjc:group", tn::Value(JsonInt(*mjc, "group", 0)), "int", true);
    } else if (dyn && ReadNumber(*dyn, "stiffness", &value)) {
      Set(prim, "mjc:stiffness", tn::Value(value), "double", true);
    }
  }
  // Initial joint configuration as the PhysX JointStateAPI
  // state:*:physics:position attribute (revolute in degrees, prismatic in
  // meters), which Newton reads through its PhysX resolver.
  double init_q = 0.0;
  // In the newton profile the MJCF ref is carried by mjc:ref alone: Newton
  // measures joint coordinates from ref, so a mirrored state would apply it
  // twice (MuJoCo qpos = 2 * ref) under the PhysX resolver.
  const Json *joint_mjc = JsonObject(joint, "mjc");
  const bool ref_only = profile.newton && joint_mjc && joint_mjc->contains("ref");
  if (profile.engine && !ref_only && ReadNumber(joint, "initPosition", &init_q) && init_q != 0.0) {
    if (is_revolute && (profile.legacy ? subtype == "revolute" : true)) {
      if (!profile.legacy) AddAPI(prim, "PhysicsJointStateAPI:angular");
      Set(prim, "state:angular:physics:position",
          tn::Value(init_q * 180.0 / kPi), "double");
    } else if (is_prismatic) {
      if (!profile.legacy) AddAPI(prim, "PhysicsJointStateAPI:linear");
      Set(prim, "state:linear:physics:position", tn::Value(init_q), "double");
    }
  }
  AddAPI(prim, "MjcJointAPI");
  if (const Json *mimic = JsonObject(joint, "mimic")) {
    auto found = joint_names.find(JsonString(*mimic, "joint"));
    if (found != joint_names.end()) {
      bool enabled = true;
      ReadBool(*mimic, "enabled", &enabled);
      float offset = 0.0f;
      float multiplier = 1.0f;
      ReadNumber(*mimic, "offset", &offset);
      ReadNumber(*mimic, "multiplier", &multiplier);
      // NewtonMimicAPI documents coef0 in the follower's position units:
      // degrees for revolute joints (Newton >= 1.5, MuJoCo 3.16 decoder);
      // Newton converts no other follower type.
      if (!profile.legacy && is_revolute)
        offset = static_cast<float>(offset * 180.0 / kPi);
      Set(prim, "newton:mimicEnabled", tn::Value(enabled), "bool", true);
      SetRel(prim, "newton:mimicJoint", {"/World/Joints/" + found->second});
      Set(prim, "newton:mimicCoef0", tn::Value(offset), "float", true);
      Set(prim, "newton:mimicCoef1", tn::Value(multiplier), "float", true);
      AddAPI(prim, "NewtonMimicAPI");
    }
  }
}

void ApplyMjcSceneOptions(tn::PrimSpec *scene, const Json &root) {
  const Json *ms = JsonObject(root, "mjcScene");
  if (!ms) return;
  auto set_num = [&](const Json &o, const char *key, const std::string &name) {
    double v = 0.0;
    if (ReadNumber(o, key, &v)) Set(scene, name, tn::Value(v), "double", true);
  };
  auto set_int = [&](const Json &o, const char *key, const std::string &name) {
    int32_t v = 0;
    if (ReadIntFrom(o, "", key, &v)) Set(scene, name, tn::Value(v), "int", true);
  };
  auto set_tok = [&](const Json &o, const char *key, const std::string &name) {
    const std::string v = JsonString(o, key);
    if (!v.empty()) SetToken(scene, name, v, true);
  };
  auto set_bool = [&](const Json &o, const char *key, const std::string &name) {
    bool v = false;
    if (ReadBool(o, key, &v)) Set(scene, name, tn::Value(v), "bool", true);
  };
  auto set_vec3 = [&](const Json &o, const char *key, const std::string &name) {
    const std::vector<double> a = JsonDoubles(o, key);
    if (a.size() >= 3) {
      Set(scene, name, tn::Value::MakeDouble3(a[0], a[1], a[2]), "double3",
          true);
    }
  };
  auto set_array = [&](const Json &o, const char *key, const std::string &name) {
    std::vector<double> a = JsonDoubles(o, key);
    if (!a.empty()) SetDoubles(scene, name, std::move(a), true);
  };

  if (const Json *o = JsonObject(*ms, "option")) {
    const std::string p = "mjc:option:";
    for (const char *key : {"timestep", "impratio", "density", "viscosity",
                            "o_margin", "tolerance", "ls_tolerance",
                            "noslip_tolerance", "ccd_tolerance"}) {
      set_num(*o, key, p + key);
    }
    for (const char *key : {"iterations", "ls_iterations", "noslip_iterations",
                            "ccd_iterations", "sdf_iterations",
                            "sdf_initpoints"}) {
      set_int(*o, key, p + key);
    }
    for (const char *key : {"integrator", "cone", "jacobian", "solver"}) {
      set_tok(*o, key, p + key);
    }
    set_vec3(*o, "wind", p + "wind");
    set_vec3(*o, "magnetic", p + "magnetic");
    for (const char *key : {"o_solref", "o_solimp", "o_friction"}) {
      set_array(*o, key, p + key);
    }
  }
  if (const Json *f = JsonObject(*ms, "flag")) {
    for (const char *key :
         {"constraint", "equality", "frictionloss", "limit", "contact",
          "gravity", "clampctrl", "warmstart", "filterparent", "actuation",
          "refsafe", "sensor", "midphase", "nativeccd", "eulerdamp",
          "autoreset", "island", "override", "energy", "fwdinv",
          "invdiscrete", "multiccd", "spring", "damper"}) {
      set_bool(*f, key, std::string("mjc:flag:") + key);
    }
  }
  if (const Json *c = JsonObject(*ms, "compiler")) {
    const std::string p = "mjc:compiler:";
    set_bool(*c, "autolimits", p + "autoLimits");
    set_num(*c, "boundmass", p + "boundMass");
    set_num(*c, "boundinertia", p + "boundInertia");
    set_num(*c, "settotalmass", p + "setTotalMass");
    set_bool(*c, "usethread", p + "useThread");
    set_bool(*c, "balanceinertia", p + "balanceInertia");
    set_tok(*c, "angle", p + "angle");
    set_bool(*c, "fitaabb", p + "fitAABB");
    set_bool(*c, "fusestatic", p + "fuseStatic");
    set_tok(*c, "inertiafromgeom", p + "inertiaFromGeom");
    set_bool(*c, "alignfree", p + "alignFree");
    set_int(*c, "inertiagrouprange_min", p + "inertiaGroupRange:min");
    set_int(*c, "inertiagrouprange_max", p + "inertiaGroupRange:max");
    set_bool(*c, "saveinertial", p + "saveInertial");
  }
}

void AddPhysicsScene(tn::Layer *layer, const Json &root) {
  tn::PrimSpec *scene = Define(layer, "/World/PhysicsScene", "PhysicsScene");
  AddAPIs(scene, {"MjcSceneAPI", "NewtonSceneAPI"});
  std::vector<float> gravity = JsonFloats(root, "gravity");
  if (gravity.size() < 3) gravity = {0.0f, -1.0f, 0.0f};
  Set(scene, "physics:gravityDirection",
      tn::Value::MakeVector3f(gravity[0], gravity[1], gravity[2]), "vector3f",
      true);
  float gravity_magnitude = 9.80665f;
  ReadNumber(root, "gravityMagnitude", &gravity_magnitude);
  Set(scene, "physics:gravityMagnitude", tn::Value(gravity_magnitude), "float",
      true);
  double timestep = 0.002;
  if (ReadNumber(root, "timestep", &timestep)) {
    Set(scene, "mjc:option:timestep", tn::Value(timestep), "double", true);
  }
  // next-only: the solver timestep also under the flat mjc:timestep name.
  Set(scene, "mjc:timestep", tn::Value(timestep), "double");
  // Full <option>/<flag>/<compiler> set (overrides/augments timestep).
  ApplyMjcSceneOptions(scene, root);

  int32_t steps_per_second =
      timestep > 0.0 ? static_cast<int32_t>(std::lround(1.0 / timestep)) : 500;
  int32_t max_solver_iterations = -1;
  bool gravity_enabled = true;
  if (const Json *newton = JsonObject(root, "newton")) {
    steps_per_second = JsonInt(*newton, "timeStepsPerSecond", steps_per_second);
    max_solver_iterations =
        JsonInt(*newton, "maxSolverIterations", max_solver_iterations);
    ReadBool(*newton, "gravityEnabled", &gravity_enabled);
  } else if (const Json *ms = JsonObject(root, "mjcScene")) {
    // Newton's own gravity switch wins over mjc:flag:gravity.
    if (const Json *flags = JsonObject(*ms, "flag")) ReadBool(*flags, "gravity", &gravity_enabled);
  }
  Set(scene, "newton:timeStepsPerSecond", tn::Value(steps_per_second), "int",
      true);
  Set(scene, "newton:maxSolverIterations", tn::Value(max_solver_iterations),
      "int", true);
  Set(scene, "newton:gravityEnabled", tn::Value(gravity_enabled), "bool", true);
}

}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
