// SPDX-License-Identifier: Apache-2.0
// Joints, actuators, tendons, sites, sensors, lights, equality, contacts, scene.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
void AddJointFromNode(const pugi::xml_node& joint_node, const AttrMap& jcls,
                      const Context& ctx, const std::string& parent_name,
                      const std::string& child_name,
                      const std::vector<double>& origin_matrix,
                      const std::array<double, 3>& origin_pos, Json* joints) {
  Json joint = Json::object();
  joint["name"] =
      joint_node ? Attr(joint_node, "name", parent_name + "_to_" + child_name)
                 : parent_name + "_to_" + child_name + "_fixed";
  const std::string mj_type =
      joint_node ? Eff(joint_node, jcls, "type", "hinge") : "fixed";
  // MuJoCo ball (3-DOF rotation) -> USD PhysicsSphericalJoint. Matches the JS
  // parsers (web/js/cli/urdf-to-usd.js, web/js/urdf.js); the USD converter
  // already handles "spherical". (free/floating base is still emitted as
  // "fixed" here — handled separately as the articulation root.)
  joint["type"] = (mj_type == "hinge")   ? "revolute"
                  : (mj_type == "slide") ? "prismatic"
                  : (mj_type == "ball")  ? "spherical"
                                         : "fixed";
  joint["parent"] = parent_name;
  joint["child"] = child_name;
  const auto axis = ParseDouble3(
      joint_node ? Eff(joint_node, jcls, "axis") : "", {{0.0, 0.0, 1.0}});
  if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-24)
    active_reader->failure = "MJCF joint has zero axis";
  joint["axis"] = Json::array({axis[0], axis[1], axis[2]});
  joint["axisToken"] = AxisToken(axis);
  joint["origin"] = Json::array({origin_pos[0], origin_pos[1], origin_pos[2]});
  joint["originMatrix"] = Array(origin_matrix);
  const auto jp =
      ParseDouble3(joint_node ? Eff(joint_node, jcls, "pos") : "", {{0, 0, 0}});
  std::array<double, 3> body_pos;
  Quat body_q;
  DecomposePosQuat(origin_matrix, &body_pos, &body_q);
  const Quat align = QuatFromTwoVecs({{1, 0, 0}}, axis);
  const Quat q0 = QuatMul(body_q, align);
  joint["axisToken"] = "X";
  joint["localPos0"] =
      Json::array({origin_matrix[0] * jp[0] + origin_matrix[4] * jp[1] +
                       origin_matrix[8] * jp[2] + origin_matrix[12],
                   origin_matrix[1] * jp[0] + origin_matrix[5] * jp[1] +
                       origin_matrix[9] * jp[2] + origin_matrix[13],
                   origin_matrix[2] * jp[0] + origin_matrix[6] * jp[1] +
                       origin_matrix[10] * jp[2] + origin_matrix[14]});
  joint["localPos1"] = Json::array({jp[0], jp[1], jp[2]});
  joint["localRot0"] = Json::array({q0[0], q0[1], q0[2], q0[3]});
  joint["localRot1"] = Json::array({align[0], align[1], align[2], align[3]});

  if (joint_node) {
    const std::vector<double> range =
        ParseDoubles(Eff(joint_node, jcls, "range"));
    // MJCF `limited` is true/false/auto; auto means limited exactly when a
    // range is authored and <compiler autolimits> is enabled (the default).
    const std::string limited_token = ToLower(Eff(joint_node, jcls, "limited", "auto"));
    const bool limited =
        limited_token == "true" ||
        (limited_token == "auto" && ctx.autolimits && range.size() >= 2);
    const bool legacy = active_reader->options.profile == "legacy";
    if (range.size() >= 2 && limited) {
      const double scale = (mj_type == "hinge" || mj_type == "ball") ? ctx.angle_to_rad : 1.0;
      if (mj_type == "ball") {
        // MuJoCo ball joints limit only the swing angle (range[1]); UsdPhysics
        // expresses that as equal spherical cone half-angles.
        if (legacy) {
          if (!UnsupportedXML(joint_node,
                  "MJCF spherical joint cone limits are not converted",
                  "mjcf.ball_limits_unsupported", "mjcf.ball_limits",
                  active_reader->root_source)) return;
        } else {
          joint["coneLimit"] = range[1] * scale;
        }
      } else {
        // The USD converter expects revolute limits in radians (it re-converts
        // to degrees). MJCF hinge ranges are in the compiler's angle unit
        // (degrees by default); slide ranges are meters and pass through.
        joint["limit"] = {{"lower", range[0] * scale},
                          {"upper", range[1] * scale}};
      }
    }
    // MjcJointAPI source values, authored by non-legacy profiles. Angles stay
    // in the compiler unit recorded by mjc:compiler:angle (MuJoCo's decoder
    // interprets ref/springref through it).
    Json mjc = Json::object();
    for (const char* key : {"ref", "springref", "margin", "stiffness"})
      if (HasEff(joint_node, jcls, key)) mjc[key] = EffDouble(joint_node, jcls, key, 0.0);
    for (const char* key : {"solreflimit", "solimplimit", "solreffriction",
                            "solimpfriction", "springdamper"}) {
      const auto values = ParseDoubles(Eff(joint_node, jcls, key));
      if (!values.empty()) mjc[key] = Array(values);
    }
    const auto frc = ParseDoubles(Eff(joint_node, jcls, "actuatorfrcrange"));
    if (frc.size() >= 2) mjc["actuatorfrcrange"] = Json::array({frc[0], frc[1]});
    if (HasEff(joint_node, jcls, "actuatorfrclimited"))
      mjc["actuatorfrclimited"] = ToLower(Eff(joint_node, jcls, "actuatorfrclimited"));
    if (HasEff(joint_node, jcls, "actuatorgravcomp"))
      mjc["actuatorgravcomp"] = MjcBoolAttr(Eff(joint_node, jcls, "actuatorgravcomp"));
    if (HasEff(joint_node, jcls, "group"))
      mjc["group"] = ToInt32(EffDouble(joint_node, jcls, "group", 0.0), "group");
    if (!mjc.empty()) joint["mjc"] = std::move(mjc);
    Json dynamics = Json::object();
    if (HasEff(joint_node, jcls, "damping")) {
      dynamics["damping"] = EffDouble(joint_node, jcls, "damping", 0.0);
    }
    if (HasEff(joint_node, jcls, "frictionloss")) {
      dynamics["friction"] = EffDouble(joint_node, jcls, "frictionloss", 0.0);
    }
    // MJCF <joint stiffness=> and <joint armature=> — propagate so the
    // USD-side converter can author physxLimit:*:stiffness and
    // physxJoint:armature alongside the canonical mjc:* fallbacks.
    if (HasEff(joint_node, jcls, "stiffness")) {
      dynamics["stiffness"] = EffDouble(joint_node, jcls, "stiffness", 0.0);
    }
    if (HasEff(joint_node, jcls, "armature")) {
      dynamics["armature"] = EffDouble(joint_node, jcls, "armature", 0.0);
    }
    joint["dynamics"] = dynamics;
    // MJCF <joint ref="..."> is the rest-angle (degrees for hinge under the
    // default <compiler angle="degree">, meters for slide). Forward it as a
    // generic `initPosition` (radians for hinge) so the C++ converter can
    // emit state:{angular,linear}:physics:position.
    if (HasEff(joint_node, jcls, "ref")) {
      double ref = EffDouble(joint_node, jcls, "ref", 0.0);
      if (mj_type == "hinge") ref *= ctx.angle_to_rad;
      joint["initPosition"] = ref;
    }
  }

  joints->push_back(joint);
}

// MuJoCo's compiler expansion of actuator shortcuts into the generic
// gain/bias/dynamics model (mjs_setToMotor/Position/... in user_api). USD
// MjcActuator prims carry only the expanded form; Newton maps position/velocity
// shaped rows back to joint targets and MuJoCo's USD decoder reads them as is.
// Returns false for actuator kinds the MjcPhysics schema cannot express.
bool ExpandMjcfActuator(const pugi::xml_node& node, const std::string& type,
                        Json* a) {
  auto num = [&](const char* key, double fallback) {
    return HasAttr(node, key) ? ParseDoubleAttr(node, key, fallback) : fallback;
  };
  auto vec = [&](const char* key, std::vector<double> fallback) {
    const auto v = ParseDoubles(Attr(node, key));
    return v.empty() ? fallback : v;
  };
  std::vector<double> gain{1}, bias, dyn{1};
  std::string gaintype = "fixed", biastype = "none", dyntype = "none";
  if (type == "general") {
    gain = vec("gainprm", {1});
    bias = vec("biasprm", {});
    dyn = vec("dynprm", {1});
    gaintype = ToLower(Attr(node, "gaintype", "fixed"));
    biastype = ToLower(Attr(node, "biastype", "none"));
    dyntype = ToLower(Attr(node, "dyntype", "none"));
  } else if (type == "motor") {
  } else if (type == "position" || type == "intvelocity") {
    const double kp = num("kp", 1.0), kv = num("kv", 0.0);
    const double dampratio = num("dampratio", 0.0);
    gain = {kp};
    biastype = "affine";
    // A positive biasprm[2] is MuJoCo's unresolved dampratio placeholder.
    bias = {0.0, -kp, kv > 0.0 ? -kv : dampratio};
    if (type == "intvelocity") {
      dyntype = "integrator";
      (*a)["actLimited"] = "true";
      const auto range = ParseDoubles(Attr(node, "actrange"));
      if (range.size() >= 2) (*a)["actRange"] = Json::array({range[0], range[1]});
    } else {
      const double timeconst = num("timeconst", 0.0);
      if (timeconst > 0.0) {
        dyntype = "filterexact";
        dyn = {timeconst};
      }
    }
    if (HasAttr(node, "inheritrange")) (*a)["inheritRange"] = num("inheritrange", 0.0);
  } else if (type == "velocity") {
    const double kv = num("kv", 1.0);
    gain = {kv};
    biastype = "affine";
    bias = {0.0, 0.0, -kv};
  } else if (type == "damper") {
    const double kv = num("kv", 1.0);
    gaintype = "affine";
    gain = {0.0, 0.0, -kv};
    (*a)["ctrlLimited"] = "true";
  } else if (type == "cylinder") {
    double area = num("area", 1.0);
    if (HasAttr(node, "diameter")) {
      const double d = num("diameter", 1.0);
      area = 0.25 * 3.14159265358979323846 * d * d;
    }
    dyntype = "filter";
    dyn = {num("timeconst", 1.0)};
    gain = {area};
    biastype = "affine";
    bias = vec("bias", {0.0, 0.0, 0.0});
  } else if (type == "muscle") {
    const auto timeconst = vec("timeconst", {0.01, 0.04});
    const auto range = vec("range", {0.75, 1.05});
    dyntype = gaintype = biastype = "muscle";
    dyn = {timeconst.size() > 0 ? timeconst[0] : 0.01,
           timeconst.size() > 1 ? timeconst[1] : 0.04, num("tausmooth", 0.0)};
    gain = {range.size() > 0 ? range[0] : 0.75, range.size() > 1 ? range[1] : 1.05,
            num("force", -1.0), num("scale", 200.0), num("lmin", 0.5),
            num("lmax", 1.6), num("vmax", 1.5), num("fpmax", 1.3),
            num("fvmax", 1.2)};
    bias = gain;
  } else if (type == "adhesion") {
    gain = {num("gain", 1.0)};
    (*a)["ctrlLimited"] = "true";
  } else {
    return false;  // plugin and future shortcuts
  }
  static const std::set<std::string> kGain = {"fixed", "affine", "muscle", "user"};
  static const std::set<std::string> kBias = {"none", "affine", "muscle", "user"};
  static const std::set<std::string> kDyn = {"none", "integrator", "filter",
                                             "filterexact", "muscle", "user"};
  // pid/dcmotor/so3 and similar have no MjcPhysics token; neither MuJoCo's
  // USD decoder nor Newton supports them (mujoco-usd-converter 0.8 skips).
  if (!kGain.count(gaintype) || !kBias.count(biastype) || !kDyn.count(dyntype))
    return false;
  auto pad = [](std::vector<double> v, size_t n) {
    v.resize(std::max(v.size(), n), 0.0);
    return v;
  };
  (*a)["gainType"] = gaintype;
  (*a)["biasType"] = biastype;
  (*a)["dynType"] = dyntype;
  (*a)["gainPrm"] = Array(pad(gain, 10));
  (*a)["biasPrm"] = Array(pad(bias, 10));
  (*a)["dynPrm"] = Array(pad(dyn, 10));
  (*a)["gear"] = Array(pad(vec("gear", {1}), 6));
  for (const char* key : {"ctrlrange", "forcerange", "actrange", "lengthrange"}) {
    const auto range = ParseDoubles(Attr(node, key));
    static const std::map<std::string, std::string> kNames = {
        {"ctrlrange", "ctrlRange"}, {"forcerange", "forceRange"},
        {"actrange", "actRange"}, {"lengthrange", "lengthRange"}};
    if (range.size() >= 2) (*a)[kNames.at(key)] = Json::array({range[0], range[1]});
  }
  for (const auto& entry : {std::make_pair("ctrllimited", "ctrlLimited"),
                            std::make_pair("forcelimited", "forceLimited"),
                            std::make_pair("actlimited", "actLimited")})
    if (HasAttr(node, entry.first)) (*a)[entry.second] = ToLower(Attr(node, entry.first));
  if (HasAttr(node, "group")) (*a)["group"] = ToInt32(num("group", 0.0), "group");
  if (HasAttr(node, "actdim")) (*a)["actDim"] = ToInt32(num("actdim", -1.0), "actdim");
  if (HasAttr(node, "actearly")) (*a)["actEarly"] = MjcBoolAttr(Attr(node, "actearly"));
  if (HasAttr(node, "cranklength")) (*a)["crankLength"] = num("cranklength", 0.0);
  if (HasAttr(node, "refsite")) (*a)["refSite"] = Attr(node, "refsite");
  if (HasAttr(node, "slidersite")) (*a)["sliderSite"] = Attr(node, "slidersite");
  if (HasAttr(node, "jointinparent"))
    (*a)["jointInParent"] = MjcBoolAttr(Attr(node, "jointinparent"));
  return true;
}

// MuJoCo 3.16's shortcut parameter source (xml_base.cc mjXShortcutSource /
// mjXShortcutFromActuator): the class's actuator slot or its nearest ancestor
// that was written with the same shortcut, or with <general> and the
// shortcut's gain type; a general slot's gain/bias/dynamics convert back to
// the shortcut's parameters. Writes them as shortcut attributes (empty when
// the documented defaults apply).
AttrMap MjcfShortcutSource(const Defaults& d, std::string cls, const std::string& kind) {
  for (int guard = 0; guard < 256; ++guard) {
    const bool root = cls.empty() || cls == "main" || !d.actuator.count(cls);
    const AttrMap& slot = root ? d.root_actuator : d.actuator.at(cls);
    const std::string& type = root ? d.root_actuator_type : d.actuator_type.at(cls);
    auto get = [&](const char* key) {
      const auto it = slot.find(key);
      return it == slot.end() ? std::string() : it->second;
    };
    const std::string gaintype = ToLower(get("gaintype").empty() ? "fixed" : get("gaintype"));
    const bool general_match =
        type == "general" &&
        gaintype == (kind == "damper" ? "affine" : kind == "muscle" ? "muscle" : "fixed");
    if (type == kind) {
      AttrMap out;
      for (const auto& entry : slot)
        if (IsMjcfShortcutParam(entry.first)) out[entry.first] = entry.second;
      return out;
    }
    if (general_match) {
      auto prm = [&](const char* key, size_t n) {
        auto v = ParseDoubles(get(key));
        v.resize(std::max(v.size(), n), 0.0);
        return v;
      };
      const auto gp = prm("gainprm", 10), bp = prm("biasprm", 10), dp = prm("dynprm", 10);
      auto num = [](double v) {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return std::string(buffer);
      };
      AttrMap out;
      const double g0 = get("gainprm").empty() ? 1.0 : gp[0];
      if (kind == "position" || kind == "intvelocity") {
        out["kp"] = num(g0);
        if (bp[2] < 0) out["kv"] = num(-bp[2]);
        if (bp[2] > 0) out["dampratio"] = num(bp[2]);
        if (ToLower(get("dyntype")) == "filterexact") out["timeconst"] = num(dp[0]);
      } else if (kind == "velocity") {
        out["kv"] = num(g0);
      } else if (kind == "damper") {
        out["kv"] = num(-gp[2]);
      } else if (kind == "cylinder") {
        out["timeconst"] = num(dp[0]);
        out["area"] = num(g0);
        out["bias"] = num(bp[0]) + " " + num(bp[1]) + " " + num(bp[2]);
      } else if (kind == "muscle") {
        out["timeconst"] = num(dp[0]) + " " + num(dp[1]);
        out["tausmooth"] = num(dp[2]);
        out["range"] = num(gp[0]) + " " + num(gp[1]);
        out["force"] = num(gp[2]);
        out["scale"] = num(gp[3]);
        out["lmin"] = num(gp[4]);
        out["lmax"] = num(gp[5]);
        out["vmax"] = num(gp[6]);
        out["fpmax"] = num(gp[7]);
        out["fvmax"] = num(gp[8]);
      } else if (kind == "adhesion") {
        out["gain"] = num(g0);
      }
      return out;
    }
    if (root) return {};
    const auto parent = d.actuator_parent.find(cls);
    cls = parent == d.actuator_parent.end() ? std::string() : parent->second;
  }
  return {};
}

void AddMujocoActuatorsJson(const pugi::xml_node& root, Json* actuators,
                            Json* mjc_actuators, Stats* stats) {
  if (!actuators) return;
  const auto actuator_root = Child(root, "actuator");
  if (!actuator_root) return;
  const bool legacy = active_reader->options.profile == "legacy";
  const Defaults defaults = legacy ? Defaults{} : ParseDefaults(root);

  for (const auto& source_node : actuator_root.children()) {
    // Non-legacy profiles apply the class's actuator default: its attributes
    // fill in what the element leaves unset, and a <general> inherits the
    // default's shortcut kind (a `class="muscle"` general is a muscle).
    std::string type = source_node.name();
    pugi::xml_document resolved;
    pugi::xml_node act_node = source_node;
    bool inherited_kind = false;
    if (!legacy && IsMjcfActuatorTag(type)) {
      const std::string cls = Attr(source_node, "class");
      const bool named = !cls.empty() && defaults.actuator.count(cls);
      const AttrMap& base = named ? defaults.actuator.at(cls) : defaults.root_actuator;
      const std::string& slot_type = named ? defaults.actuator_type.at(cls)
                                           : defaults.root_actuator_type;
      // A <general> starts from the slot as written: a slot last written by
      // a shortcut makes it that shortcut (a `class="muscle"` general).
      if (type == "general" && !slot_type.empty() && slot_type != "general" &&
          !HasAttr(source_node, "gaintype") && !HasAttr(source_node, "biastype") &&
          !HasAttr(source_node, "dyntype")) {
        type = slot_type;
        inherited_kind = true;
      }
      // Generic attributes (ranges, gear, limits, ...) from the slot; a
      // shortcut's own parameters from MuJoCo's shortcut source.
      AttrMap merged;
      const bool shortcut = type != "general";
      for (const auto& entry : base) {
        static const std::set<std::string> kExpanded = {"gaintype", "biastype", "dyntype",
                                                        "gainprm", "biasprm", "dynprm"};
        if (shortcut && (IsMjcfShortcutParam(entry.first) || kExpanded.count(entry.first)))
          continue;
        merged[entry.first] = entry.second;
      }
      if (shortcut)
        for (const auto& entry : MjcfShortcutSource(defaults, named ? cls : std::string(), type))
          merged[entry.first] = entry.second;
      if (!merged.empty()) {
        act_node = resolved.append_copy(source_node);
        for (const auto& entry : merged)
          if (!act_node.attribute(entry.first.c_str()))
            act_node.append_attribute(entry.first.c_str()).set_value(entry.second.c_str());
      }
    }
    const std::string joint = Attr(act_node, "joint");
    const std::string tendon = Attr(act_node, "tendon");
    const std::string site = Attr(act_node, "site");
    const std::string body = Attr(act_node, "body");
    if (!legacy && mjc_actuators && type != "plugin") {
      Json a = Json::object();
      const std::string fallback =
          !joint.empty() ? joint : (!tendon.empty() ? tendon : (!site.empty() ? site : body));
      a["name"] = Attr(act_node, "name", type + "_" + fallback);
      a["actuatorType"] = type;
      if (!joint.empty()) a["targetJoint"] = joint;
      if (!tendon.empty()) a["targetTendon"] = tendon;
      if (!site.empty()) a["targetSite"] = site;
      if (!body.empty()) a["targetBody"] = body;
      if (!ExpandMjcfActuator(act_node, type, &a)) {
        if (!UnsupportedXML(act_node,
                "MJCF actuator `" + a["name"].get<std::string>() +
                    "` uses a gain/bias/dynamics type without an MjcPhysics token",
                "mjcf.actuator_type_unsupported", "mjcf.actuator_type",
                active_reader->root_source))
          return;
        continue;
      }
      if (inherited_kind) {
        // A general's own gainprm/biasprm/dynprm set only the leading
        // entries of the default kind's parameters (MuJoCo array parsing).
        for (const auto& entry : {std::make_pair("gainprm", "gainPrm"),
                                  std::make_pair("biasprm", "biasPrm"),
                                  std::make_pair("dynprm", "dynPrm")}) {
          const auto own = ParseDoubles(Attr(source_node, entry.first));
          if (own.empty()) continue;
          Json& values = a[entry.second];
          for (size_t i = 0; i < own.size() && i < values.size(); ++i) values[i] = own[i];
        }
      }
      mjc_actuators->push_back(std::move(a));
      if (stats) stats->actuators++;
      continue;
    }

    // Muscle/general/adhesion/cylinder/intvelocity/damper, or any
    // tendon-/site-/ body-targeted actuator -> MjcActuator (preserves the
    // MuJoCo gain/bias/ lengthrange params; e.g. ms_human_700 muscles, flybody
    // adhesion). Plain joint-targeted position/velocity/motor actuators keep
    // the Newton PD path.
    const bool is_mjc =
        (type == "muscle" || type == "general" || type == "adhesion" ||
         type == "cylinder" || type == "intvelocity" || type == "damper" ||
         type == "plugin" || !tendon.empty() || !site.empty() || !body.empty());
    if (is_mjc && mjc_actuators) {
      const std::string fallback_target =
          !joint.empty()
              ? joint
              : (!tendon.empty() ? tendon : (!site.empty() ? site : body));
      Json a = Json::object();
      a["name"] = Attr(act_node, "name", type + "_" + fallback_target);
      a["actuatorType"] = type;
      if (!joint.empty()) a["targetJoint"] = joint;
      if (!tendon.empty()) a["targetTendon"] = tendon;
      if (!site.empty()) a["targetSite"] = site;
      if (!body.empty()) a["targetBody"] = body;
      const auto gainprm = ParseDoubles(Attr(act_node, "gainprm"));
      if (!gainprm.empty())
        a["gainPrm"] = Array(gainprm);
      else if (HasAttr(act_node, "gain"))  // <adhesion gain=..> scalar
        a["gainPrm"] = Json::array({ParseDoubleAttr(act_node, "gain", 0.0)});
      const auto biasprm = ParseDoubles(Attr(act_node, "biasprm"));
      if (!biasprm.empty()) a["biasPrm"] = Array(biasprm);
      const auto lengthrange = ParseDoubles(Attr(act_node, "lengthrange"));
      if (lengthrange.size() >= 2)
        a["lengthRange"] = Json::array({lengthrange[0], lengthrange[1]});
      const auto ctrlrange = ParseDoubles(Attr(act_node, "ctrlrange"));
      if (ctrlrange.size() >= 2)
        a["ctrlRange"] = Json::array({ctrlrange[0], ctrlrange[1]});
      const auto forcerange = ParseDoubles(Attr(act_node, "forcerange"));
      if (forcerange.size() >= 2)
        a["forceRange"] = Json::array({forcerange[0], forcerange[1]});
      const auto gear = ParseDoubles(Attr(act_node, "gear"));
      if (!gear.empty()) a["gear"] = Array(gear);
      if (HasAttr(act_node, "dyntype"))
        a["dynType"] = Attr(act_node, "dyntype");
      if (HasAttr(act_node, "gaintype"))
        a["gainType"] = Attr(act_node, "gaintype");
      if (HasAttr(act_node, "biastype"))
        a["biasType"] = Attr(act_node, "biastype");
      // Engine-plugin actuator (<plugin plugin=".." instance="..">): preserve
      // the plugin id + referenced <extension> instance name.
      if (HasAttr(act_node, "plugin")) a["plugin"] = Attr(act_node, "plugin");
      if (HasAttr(act_node, "instance"))
        a["instance"] = Attr(act_node, "instance");
      mjc_actuators->push_back(std::move(a));
      if (stats) stats->actuators++;
      continue;
    }

    if (joint.empty()) {
      continue;
    }
    Json act = Json::object();
    act["name"] = Attr(act_node, "name", type + "_" + joint);
    act["joint"] = joint;
    act["control"] = "pd";

    if (HasAttr(act_node, "kp")) {
      act["kp"] = ParseDoubleAttr(act_node, "kp", 0.0);
    } else {
      const auto gain = ParseDoubles(Attr(act_node, "gainprm"));
      if (!gain.empty()) {
        act["kp"] = gain[0];
      }
    }
    if (HasAttr(act_node, "kv")) {
      act["kd"] = ParseDoubleAttr(act_node, "kv", 0.0);
    } else {
      const auto bias = ParseDoubles(Attr(act_node, "biasprm"));
      if (bias.size() >= 3) {
        act["kd"] = std::abs(bias[2]);
      }
    }
    const auto force_range = ParseDoubles(Attr(act_node, "forcerange"));
    if (force_range.size() >= 2) {
      act["maxEffort"] =
          std::max(std::abs(force_range[0]), std::abs(force_range[1]));
    }
    if (HasAttr(act_node, "delay")) {
      act["delaySteps"] =
          ToInt32(ParseDoubleAttr(act_node, "delay", 1.0), "delay");
    }
    Json mjc = Json::object();
    mjc["type"] = type;
    for (const auto& attr : act_node.attributes())
      mjc["source:" + std::string(attr.name())] = std::string(attr.value());
    for (const char* key :
         {"gear", "ctrlrange", "forcerange", "gainprm", "biasprm", "dynprm"}) {
      const auto values = ParseDoubles(Attr(act_node, key));
      if (!values.empty()) mjc[key] = Array(values);
    }
    act["mjc"] = std::move(mjc);
    if (type ==
        "motor") {  // A control range is a bound, not a constant command.
      active_reader->Warn(
          "MJCF motor control ranges are retained; no fixed control command is "
          "inferred");
    }
    actuators->push_back(std::move(act));
    if (stats) stats->actuators++;
  }
}

// MJCF <tendon><fixed> -> JSON tendon entries consumed by AddMjcTendonFromJson.
// A fixed tendon constrains a linear combination of joint coordinates; we carry
// the {joint, coef} list plus the spring/damper/range scalars. Spatial tendons
// carry their ordered site/wrap-geom/pulley path.
void AddMujocoTendonsJson(const pugi::xml_node& root, const Context& ctx,
                          Json* tendons, Stats* stats) {
  if (!tendons) return;
  const auto tendon_root = Child(root, "tendon");
  if (!tendon_root) return;
  for (const auto& t_node : tendon_root.children()) {
    const std::string kind = t_node.name();  // "fixed" or "spatial"
    if (kind != "fixed" && kind != "spatial") continue;
    Json tendon = Json::object();
    tendon["name"] = Attr(
        t_node, "name", "tendon_" + std::to_string(stats ? stats->tendons : 0));
    tendon["type"] = kind;
    if (kind == "fixed") {
      // Fixed tendon: linear combination of joint coordinates.
      Json jlist = Json::array();
      for (const auto& j_node : Children(t_node, "joint")) {
        const std::string jn = Attr(j_node, "joint");
        if (jn.empty()) continue;
        Json je = Json::object();
        je["joint"] = jn;
        je["coef"] = ParseDoubleAttr(j_node, "coef", 1.0);
        jlist.push_back(std::move(je));
      }
      if (jlist.empty()) continue;
      tendon["joints"] = std::move(jlist);
    } else {
      // Spatial tendon (muscle path): ordered <site>, <geom sidesite=...> and
      // <pulley divisor=...> elements; the writer encodes MjcTendon's
      // path/segments/divisors/sideSites arrays from this order.
      Json waypoints = Json::array();
      for (const auto& w : t_node.children()) {
        const std::string wn = w.name();
        if (wn == "site" && HasAttr(w, "site")) {
          waypoints.push_back({{"site", Attr(w, "site")}});
        } else if (wn == "geom" && HasAttr(w, "geom")) {
          Json wp = {{"geom", Attr(w, "geom")}};
          if (HasAttr(w, "sidesite")) wp["sidesite"] = Attr(w, "sidesite");
          waypoints.push_back(std::move(wp));
        } else if (wn == "pulley") {
          waypoints.push_back({{"pulley", ParseDoubleAttr(w, "divisor", 1.0)}});
        }
      }
      if (waypoints.size() < 2) continue;
      tendon["path"] = std::move(waypoints);
      if (HasAttr(t_node, "width"))
        tendon["width"] = ParseDoubleAttr(t_node, "width", 0.003);
      const auto rgba = ParseDoubles(Attr(t_node, "rgba"));
      if (rgba.size() >= 4)
        tendon["rgba"] = Json::array({rgba[0], rgba[1], rgba[2], rgba[3]});
    }
    if (HasAttr(t_node, "stiffness"))
      tendon["stiffness"] = ParseDoubleAttr(t_node, "stiffness", 0.0);
    if (HasAttr(t_node, "damping"))
      tendon["damping"] = ParseDoubleAttr(t_node, "damping", 0.0);
    if (HasAttr(t_node, "frictionloss"))
      tendon["frictionloss"] = ParseDoubleAttr(t_node, "frictionloss", 0.0);
    if (HasAttr(t_node, "margin"))
      tendon["margin"] = ParseDoubleAttr(t_node, "margin", 0.0);
    const auto range = ParseDoubles(Attr(t_node, "range"));
    if (range.size() >= 2) tendon["range"] = Json::array({range[0], range[1]});
    {
      const std::string token = ToLower(Attr(t_node, "limited", "auto"));
      const bool limited = token == "true" ||
                           (token == "auto" && ctx.autolimits && range.size() >= 2);
      if (range.size() >= 2 || token != "auto")
        tendon["limited"] = limited ? "true" : "false";
    }
    auto springlen = ParseDoubles(Attr(t_node, "springlength"));
    // One value means a fixed rest length; Newton reads a 2-vector.
    if (springlen.size() == 1) springlen.push_back(springlen[0]);
    if (!springlen.empty()) tendon["springlength"] = Array(springlen);
    if (HasAttr(t_node, "group"))
      tendon["group"] = ToInt32(ParseDoubleAttr(t_node, "group", 0.0), "group");
    if (HasAttr(t_node, "armature"))
      tendon["armature"] = ParseDoubleAttr(t_node, "armature", 0.0);
    for (const char* key : {"solreflimit", "solimplimit", "solreffriction", "solimpfriction"}) {
      const auto values = ParseDoubles(Attr(t_node, key));
      if (!values.empty()) tendon[key] = Array(values);
    }
    const auto frcrange = ParseDoubles(Attr(t_node, "actuatorfrcrange"));
    if (frcrange.size() >= 2)
      tendon["actuatorfrcrange"] = Json::array({frcrange[0], frcrange[1]});
    if (HasAttr(t_node, "actuatorfrclimited")) {
      const std::string token = ToLower(Attr(t_node, "actuatorfrclimited"));
      const bool limited = token == "true" ||
                           (token == "auto" && ctx.autolimits && frcrange.size() >= 2);
      tendon["actuatorfrclimited"] = limited ? "true" : "false";
    } else if (frcrange.size() >= 2 && ctx.autolimits) {
      tendon["actuatorfrclimited"] = "true";
    }
    tendons->push_back(std::move(tendon));
    if (stats) stats->tendons++;
  }
}

// MJCF <site> (in bodies) -> JSON site entries with a baked world transform,
// consumed by the converter to emit MjcSite marker prims. Sites are the routing
// points for spatial (muscle) tendons. Recurses the body tree.
// Sites of a body subtree in preorder (a body's own sites, then its child
// bodies in order), with an explicit stack: chains can be long.
void CollectMujocoSitesJson(const pugi::xml_node& top,
                            const std::vector<double>& top_parent_world,
                            const Context& ctx, Json* sites, Stats* stats) {
  std::vector<std::pair<pugi::xml_node, std::vector<double>>> stack;
  stack.emplace_back(top, top_parent_world);
  while (!stack.empty()) {
    const pugi::xml_node body_node = stack.back().first;
    const std::vector<double> parent_world = std::move(stack.back().second);
    stack.pop_back();
    const std::vector<double> body_world =
        MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
    for (const auto& s_node : Children(body_node, "site")) {
      const std::string name =
          Attr(s_node, "name", "site_" + std::to_string(sites->size()));
      const std::vector<double> site_world =
          MultiplyMatrix(body_world, PoseMatrix(s_node, ctx));
      Json site = {{"name", name}, {"matrix", Array(site_world)},
                   {"bodyMatrix", Array(body_world)}};
      auto owner = active_body_names.find(body_node.internal_object());
      site["body"] = owner != active_body_names.end() ? owner->second : active_world_name;
      if (HasAttr(s_node, "group"))
        site["group"] = ToInt32(ParseDoubleAttr(s_node, "group", 0.0), "group");
      const auto size = ParseDoubles(Attr(s_node, "size"));
      if (!size.empty()) site["size"] = size[0];
      if (size.size() > 1) site["sizes"] = Array(size);
      const std::string type = ToLower(Attr(s_node, "type"));
      if (!type.empty()) site["type"] = type;
      sites->push_back(std::move(site));
      if (stats) stats->sites++;
    }
    const auto children = Children(body_node, "body");
    for (auto it = children.rbegin(); it != children.rend(); ++it) stack.emplace_back(*it, body_world);
  }
}

// MJCF <sensor> children -> JSON sensor entries consumed by the converter's
// AddMjcSensorFromJson -> MjcSensor prim. The sensor kind is the element name;
// the measured object is captured via objtype/objname (frame sensors carry them
// explicitly, others attach via site/joint/actuator/tendon/body/geom).
void AddMujocoSensorsJson(const pugi::xml_node& root, Json* sensors,
                          Stats* stats) {
  if (!sensors) return;
  const auto sensor_root = Child(root, "sensor");
  if (!sensor_root) return;
  for (const auto& s_node : sensor_root.children()) {
    const std::string type = s_node.name();
    if (type.empty()) continue;
    Json s = {{"type", type}};
    s["name"] =
        Attr(s_node, "name", type + "_" + std::to_string(sensors->size()));
    if (HasAttr(s_node, "objtype")) {
      s["objtype"] = Attr(s_node, "objtype");
      s["objname"] = Attr(s_node, "objname");
    } else {
      // Infer objtype from the attachment attribute the sensor type uses.
      for (const char* k :
           {"site", "joint", "actuator", "tendon", "body", "geom"}) {
        if (HasAttr(s_node, k)) {
          s["objtype"] = k;
          s["objname"] = Attr(s_node, k);
          break;
        }
      }
    }
    if (HasAttr(s_node, "reftype")) s["reftype"] = Attr(s_node, "reftype");
    if (HasAttr(s_node, "refname")) s["refname"] = Attr(s_node, "refname");
    if (HasAttr(s_node, "group"))
      s["group"] = ToInt32(ParseDoubleAttr(s_node, "group", 0.0), "group");
    if (HasAttr(s_node, "cutoff"))
      s["cutoff"] = ParseDoubleAttr(s_node, "cutoff", 0.0);
    if (HasAttr(s_node, "noise"))
      s["noise"] = ParseDoubleAttr(s_node, "noise", 0.0);
    const auto user = ParseDoubles(Attr(s_node, "user"));
    if (!user.empty()) s["user"] = Array(user);
    sensors->push_back(std::move(s));
    if (stats) stats->sensors++;
  }
}

// MJCF <custom><numeric|text> -> JSON consumed by the converter to preserve
// model metadata / MJX compile knobs (e.g. max_contact_points) as mjc:custom:*.
void AddMujocoCustomJson(const pugi::xml_node& root, Json* custom) {
  if (!custom) return;
  const auto custom_root = Child(root, "custom");
  if (!custom_root) return;
  Json numerics = Json::array();
  for (const auto& n : Children(custom_root, "numeric")) {
    const std::string name = Attr(n, "name");
    if (name.empty()) continue;
    numerics.push_back(
        {{"name", name}, {"data", Array(ParseDoubles(Attr(n, "data")))}});
  }
  Json texts = Json::array();
  for (const auto& t : Children(custom_root, "text")) {
    const std::string name = Attr(t, "name");
    if (name.empty()) continue;
    texts.push_back({{"name", name}, {"data", Attr(t, "data")}});
  }
  if (!numerics.empty()) (*custom)["numeric"] = std::move(numerics);
  if (!texts.empty()) (*custom)["text"] = std::move(texts);
}

// <extension><plugin plugin="..."><instance name="..."><config key= value=>
// -> JSON plugin-instance declarations. The instance config (e.g. a PID's
// kp/ki/kd) is what an <actuator><plugin instance=".."> references.
void AddMujocoPluginsJson(const pugi::xml_node& root, Json* plugins) {
  if (!plugins) return;
  const auto extension = Child(root, "extension");
  if (!extension) return;
  for (const auto& plugin_node : Children(extension, "plugin")) {
    const std::string plugin_id = Attr(plugin_node, "plugin");
    for (const auto& inst : Children(plugin_node, "instance")) {
      const std::string inst_name = Attr(inst, "name");
      if (inst_name.empty()) continue;
      Json p = Json::object();
      p["instance"] = inst_name;
      if (!plugin_id.empty()) p["plugin"] = plugin_id;
      Json config = Json::object();
      for (const auto& cfg : Children(inst, "config")) {
        const std::string key = Attr(cfg, "key");
        if (key.empty()) continue;
        config[key] = Attr(cfg, "value");
      }
      if (!config.empty()) p["config"] = std::move(config);
      plugins->push_back(std::move(p));
    }
  }
}

// MJCF <light>/<camera> (in worldbody or nested bodies) -> JSON with a baked
// world matrix, consumed by the converter's AddLightFromJson/AddCameraFromJson.
// `frame_world` is the world transform of `frame_node`'s frame.
// Lights and cameras of a body subtree in preorder, with an explicit stack.
void CollectMujocoLightsCamerasJson(const pugi::xml_node& top,
                                    const std::vector<double>& top_world,
                                    const Context& ctx, Json* lights,
                                    Json* cameras) {
  std::vector<std::pair<pugi::xml_node, std::vector<double>>> stack;
  stack.emplace_back(top, top_world);
  while (!stack.empty()) {
    const pugi::xml_node frame_node = stack.back().first;
    const std::vector<double> frame_world = std::move(stack.back().second);
    stack.pop_back();
    for (const auto& l : Children(frame_node, "light")) {
      Json light = Json::object();
      light["name"] = Attr(l, "name", "light");
      std::string type = Attr(l, "type");
      if (type.empty())
        type =
            MjcBoolAttr(Attr(l, "directional", "false")) ? "directional" : "spot";
      light["type"] = type;
      light["matrix"] = Array(MultiplyMatrix(frame_world, PoseMatrix(l, ctx)));
      const auto dir = ParseDouble3(Attr(l, "dir"), {{0.0, 0.0, -1.0}});
      light["dir"] = Json::array({dir[0], dir[1], dir[2]});
      const auto diffuse = ParseDoubles(Attr(l, "diffuse"));
      if (diffuse.size() >= 3)
        light["color"] = Json::array({diffuse[0], diffuse[1], diffuse[2]});
      if (HasAttr(l, "castshadow"))
        light["castshadow"] = MjcBoolAttr(Attr(l, "castshadow"));
      if (HasAttr(l, "cutoff"))
        light["cutoff"] = ParseDoubleAttr(l, "cutoff", 45.0);
      for (const char* key : {"intensity", "exponent"})
        if (HasAttr(l, key)) light[key] = ParseDoubleAttr(l, key, 0.0);
      lights->push_back(std::move(light));
    }
    for (const auto& c : Children(frame_node, "camera")) {
      Json cam = Json::object();
      cam["name"] = Attr(c, "name", "camera");
      cam["matrix"] = Array(MultiplyMatrix(frame_world, PoseMatrix(c, ctx)));
      // MuJoCo's default vertical field of view is 45 degrees.
      cam["fovy"] = ParseDoubleAttr(c, "fovy", 45.0);
      const std::string proj = Attr(c, "projection");
      if (proj == "orthographic" || MjcBoolAttr(Attr(c, "orthographic", "false")))
        cam["orthographic"] = true;
      cameras->push_back(std::move(cam));
    }
    const auto children = Children(frame_node, "body");
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      stack.emplace_back(*it, MultiplyMatrix(frame_world, PoseMatrix(*it, ctx)));
  }
}

// MJCF <equality> -> JSON entries consumed by AddMjcEqualityFromJson.
void AddMujocoEqualityJson(const pugi::xml_node& root, Json* equalities,
                           Stats* stats) {
  if (!equalities) return;
  const auto eq_root = Child(root, "equality");
  if (!eq_root) return;
  for (const auto& e_node : eq_root.children()) {
    const std::string kind = e_node.name();  // connect | weld | joint
    if (kind != "connect" && kind != "weld" && kind != "joint") {
      if (!UnsupportedXML(e_node, "Unsupported MJCF equality: " + kind,
              "mjcf.equality_unsupported", "mjcf.equality", active_reader->root_source)) return;
      continue;
    }
    Json eq = Json::object();
    eq["name"] =
        Attr(e_node, "name",
             kind + "_" + std::to_string(stats ? stats->equalities : 0));
    eq["type"] = kind;
    if (kind == "joint") {
      eq["joint1"] = Attr(e_node, "joint1");
      if (HasAttr(e_node, "joint2")) eq["joint2"] = Attr(e_node, "joint2");
      const auto poly = ParseDoubles(Attr(e_node, "polycoef"));
      if (!poly.empty()) eq["polycoef"] = Array(poly);
    } else {
      eq["body1"] = Attr(e_node, "body1");
      if (HasAttr(e_node, "body2")) eq["body2"] = Attr(e_node, "body2");
      const auto anchor = ParseDoubles(Attr(e_node, "anchor"));
      if (!anchor.empty()) eq["anchor"] = Array(anchor);
      if (kind == "weld" && HasAttr(e_node, "torquescale"))
        eq["torquescale"] = ParseDoubleAttr(e_node, "torquescale", 1.0);
    }
    if (kind == "weld") {
      const auto relpose = ParseDoubles(Attr(e_node, "relpose"));
      if (relpose.size() == 7) eq["relpose"] = Array(relpose);
    }
    // Site form (MuJoCo 3.x): each side's frame is its site. The converter
    // resolves sites to their owning bodies; legacy output cannot express it.
    if (HasAttr(e_node, "site1") || HasAttr(e_node, "site2")) {
      if (active_reader->options.profile == "legacy") {
        if (!UnsupportedXML(e_node, "MJCF site-based equality constraints are not converted",
                            "mjcf.equality_site_unsupported", "mjcf.equality_site",
                            active_reader->root_source))
          return;
        continue;
      }
      eq["site1"] = Attr(e_node, "site1");
      eq["site2"] = Attr(e_node, "site2");
    }
    if (HasAttr(e_node, "active")) eq["active"] = MjcBoolAttr(Attr(e_node, "active"));
    const auto solref = ParseDoubles(Attr(e_node, "solref"));
    if (!solref.empty()) eq["solref"] = Array(solref);
    const auto solimp = ParseDoubles(Attr(e_node, "solimp"));
    if (!solimp.empty()) eq["solimp"] = Array(solimp);
    equalities->push_back(std::move(eq));
    if (stats) stats->equalities++;
  }
}

// MJCF <contact><exclude body1 body2> -> filteredPairs entries (maps to
// PhysicsFilteredPairsAPI in the converter, which already handles this array).
void AddMujocoContactExcludesJson(const pugi::xml_node& root,
                                  Json* filtered_pairs, Stats* stats) {
  if (!filtered_pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto& c_node : Children(contact_root, "exclude")) {
    const std::string b1 = Attr(c_node, "body1");
    const std::string b2 = Attr(c_node, "body2");
    if (b1.empty() || b2.empty()) continue;
    // MJCF's "world" body is exported as the static link holding the
    // <worldbody> geoms.
    auto body = [](const std::string& name) {
      return name == "world" ? active_world_name + "_geoms" : name;
    };
    filtered_pairs->push_back({{"body1", body(b1)}, {"body2", body(b2)}});
    if (stats) stats->contact_excludes++;
  }
}

// MJCF <contact><pair> -> JSON entries consumed by AddContactPairFromJson. An
// explicit collision pair between two geoms with its own friction/solver
// params.
void AddMujocoContactPairsJson(const pugi::xml_node& root, Json* pairs,
                               Stats* stats) {
  if (!pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto& p : Children(contact_root, "pair")) {
    const std::string g1 = Attr(p, "geom1");
    const std::string g2 = Attr(p, "geom2");
    if (g1.empty() || g2.empty()) continue;
    Json pr = {{"geom1", g1}, {"geom2", g2}};
    pr["name"] = Attr(p, "name", "pair_" + std::to_string(pairs->size()));
    if (HasAttr(p, "condim"))
      pr["condim"] = ToInt32(ParseDoubleAttr(p, "condim", 3.0), "condim");
    if (HasAttr(p, "margin")) pr["margin"] = ParseDoubleAttr(p, "margin", 0.0);
    if (HasAttr(p, "gap")) pr["gap"] = ParseDoubleAttr(p, "gap", 0.0);
    const auto friction = ParseDoubles(Attr(p, "friction"));
    if (!friction.empty()) pr["friction"] = Array(friction);
    const auto solref = ParseDoubles(Attr(p, "solref"));
    if (!solref.empty()) pr["solref"] = Array(solref);
    const auto solimp = ParseDoubles(Attr(p, "solimp"));
    if (!solimp.empty()) pr["solimp"] = Array(solimp);
    pairs->push_back(std::move(pr));
    if (stats) stats->contact_pairs++;
  }
}

// Normalize a MuJoCo boolean/flag attribute: <flag x="enable|disable"> and
// <compiler x="true|false"> both occur; map enable/true/1 -> true.
bool MjcBoolAttr(const std::string& v) {
  return v == "true" || v == "enable" || v == "1";
}

// Read the full <option>/<option><flag>/<compiler> attribute set into a
// `mjcScene` JSON block (MJCF attr names) consumed by the converter's
// ApplyMjcSceneOptions -> MjcSceneAPI. (The MjcSceneAPI schema + USDC
// round-trip already support all of these; only timestep was emitted before.)
Json BuildMjcSceneJson(const pugi::xml_node& root) {
  Json ms = Json::object();
  if (const auto option = Child(root, "option")) {
    Json opt = Json::object();
    auto num = [&](const char* k) {
      if (HasAttr(option, k)) opt[k] = ParseDoubleAttr(option, k, 0.0);
    };
    auto inum = [&](const char* k) {
      if (HasAttr(option, k))
        opt[k] = ToInt32(ParseDoubleAttr(option, k, 0.0), k);
    };
    auto tok = [&](const char* k) {
      if (HasAttr(option, k)) {
        const auto authored = Attr(option, k);
        const auto canonical = ToLower(authored);
        opt[k] = canonical;
        if (authored != canonical && active_reader->options.report)
          active_reader->options.report->Diagnostic("info", "mjcf.token_canonicalized",
              std::string(k) + ": " + authored + " -> " + canonical, active_reader->root_source);
      }
    };
    auto vec = [&](const char* k) {
      auto v = ParseDoubles(Attr(option, k));
      if (!v.empty()) opt[k] = Array(v);
    };
    num("timestep");
    num("impratio");
    num("density");
    num("viscosity");
    num("o_margin");
    num("tolerance");
    num("ls_tolerance");
    num("noslip_tolerance");
    num("ccd_tolerance");
    inum("iterations");
    inum("ls_iterations");
    inum("noslip_iterations");
    inum("ccd_iterations");
    inum("sdf_iterations");
    inum("sdf_initpoints");
    tok("integrator");
    tok("cone");
    tok("jacobian");
    tok("solver");
    vec("wind");
    vec("magnetic");
    vec("o_solref");
    vec("o_solimp");
    vec("o_friction");
    if (HasAttr(option, "actuatorgroupdisable")) {
      // Disabled actuator groups, MuJoCo's 0..30 bit range.
      Json groups = Json::array();
      for (double g : ParseDoubles(Attr(option, "actuatorgroupdisable")))
        if (g >= 0 && g <= 30 && g == std::floor(g)) groups.push_back(static_cast<int>(g));
      opt["actuatorgroupdisable"] = std::move(groups);
    }
    if (!opt.empty()) ms["option"] = std::move(opt);
    if (const auto flag = Child(option, "flag")) {
      Json fl = Json::object();
      static const char* kFlags[] = {
          "constraint",   "equality",  "frictionloss", "limit",
          "contact",      "gravity",   "clampctrl",    "warmstart",
          "filterparent", "actuation", "refsafe",      "sensor",
          "midphase",     "nativeccd", "eulerdamp",    "autoreset",
          "island",       "override",  "energy",       "fwdinv",
          "invdiscrete",  "multiccd",     "spring",    "damper"};
      for (const char* k : kFlags)
        if (HasAttr(flag, k)) fl[k] = MjcBoolAttr(Attr(flag, k));
      if (!fl.empty()) ms["flag"] = std::move(fl);
    }
  }
  if (const auto compiler = Child(root, "compiler")) {
    Json comp = Json::object();
    auto num = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = ParseDoubleAttr(compiler, k, 0.0);
    };
    auto tok = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = Attr(compiler, k);
    };
    auto boolean = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = MjcBoolAttr(Attr(compiler, k));
    };
    boolean("autolimits");
    num("boundmass");
    num("boundinertia");
    num("settotalmass");
    boolean("usethread");
    boolean("balanceinertia");
    tok("angle");
    boolean("fitaabb");
    boolean("fusestatic");
    tok("inertiafromgeom");
    boolean("alignfree");
    boolean("saveinertial");
    const auto igr = ParseDoubles(Attr(compiler, "inertiagrouprange"));
    if (igr.size() >= 2) {
      comp["inertiagrouprange_min"] = ToInt32(igr[0], "inertiagrouprange");
      comp["inertiagrouprange_max"] = ToInt32(igr[1], "inertiagrouprange");
    }
    if (!comp.empty()) ms["compiler"] = std::move(comp);
  }
  return ms;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
