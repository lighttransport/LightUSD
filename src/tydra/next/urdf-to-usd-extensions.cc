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

std::vector<std::string> NewtonActuatorSchemas(const Json &act) {
  std::vector<std::string> schemas;
  const std::string control = JsonString(act, "control", "pd");
  if (control == "pid" || act.contains("ki") || act.contains("integralMax")) {
    schemas.push_back("NewtonPIDControlAPI");
  } else {
    schemas.push_back("NewtonPDControlAPI");
  }
  if (act.contains("delaySteps")) schemas.push_back("NewtonActuatorDelayAPI");
  if (act.contains("maxEffort")) schemas.push_back("NewtonMaxEffortClampingAPI");
  if (act.contains("maxMotorEffort") || act.contains("saturationEffort") ||
      act.contains("velocityLimit")) {
    schemas.push_back("NewtonDCMotorClampingAPI");
  }
  if (act.contains("lookupPositions") || act.contains("lookupEfforts")) {
    schemas.push_back("NewtonPositionBasedClampingAPI");
  }
  return schemas;
}

void AddNewtonActuatorFromJson(Scope *scope, std::set<std::string> *siblings,
                               const Json &act, const NameMap &joint_names,
                               size_t index, std::string *warn) {
  const std::string name = Sanitize(
      JsonString(act, "name", "actuator_" + std::to_string(index)), "actuator");
  std::vector<std::string> targets;
  const std::string joint = JsonString(act, "joint");
  if (!joint.empty()) {
    auto found = joint_names.find(joint);
    if (found != joint_names.end()) {
      targets.push_back("/World/Joints/" + found->second);
    }
  }
  if (const Json *items = JsonArray(act, "targets")) {
    for (const Json &target : *items) {
      if (!target.is_string()) continue;
      const std::string name_or_path = target.get<std::string>();
      if (!name_or_path.empty() && name_or_path[0] == '/') {
        targets.push_back(name_or_path);
      } else {
        auto found = joint_names.find(name_or_path);
        if (found != joint_names.end()) {
          targets.push_back("/World/Joints/" + found->second);
        }
      }
    }
  }
  if (targets.empty()) {
    AppendWarn(warn, "Skipping Newton actuator `" + name +
                         "`: no target joint was exported.\n");
    return;
  }
  tn::PrimSpec *prim = scope->Add(ChildName(name, siblings), "NewtonActuator");
  if (!prim) return;
  for (const std::string &schema : NewtonActuatorSchemas(act)) {
    AddAPI(prim, schema);
  }
  SetRel(prim, "newton:targets", targets);
  if (const Json* mjc = JsonObject(act, "mjc")) {
    for (const auto& entry : *mjc->object_items()) {
      const std::string key = "mjc:" + entry.key;
      const Json& v = entry.value();
      if (v.is_string())
        SetToken(prim, key, v.get<std::string>(), true);
      else if (v.is_array())
        SetDoubles(prim, key, JsonDoubles(*mjc, entry.key.c_str()), true);
    }
  }
  double reduction = 1;
  if (ReadNumber(act, "mechanicalReduction", &reduction))
    Set(prim, "urdf:mechanicalReduction", tn::Value(reduction), "double", true);
  if (act.contains("delaySteps")) {
    Set(prim, "newton:delaySteps", tn::Value(JsonInt(act, "delaySteps", 1)),
        "int", true);
  }
  for (const char *key : {"constEffort", "kp", "kd", "ki", "integralMax",
                          "maxEffort", "maxMotorEffort", "saturationEffort",
                          "velocityLimit"}) {
    float v = 0.0f;
    if (ReadNumber(act, key, &v)) {
      Set(prim, std::string("newton:") + key, tn::Value(v), "float", true);
    }
  }
  for (const char *key : {"lookupPositions", "lookupEfforts"}) {
    std::vector<float> values = JsonFloats(act, key);
    if (!values.empty()) {
      Set(prim, std::string("newton:") + key,
          tn::Value::MakeFloatArray(std::move(values)), "float[]", true);
    }
  }
}

void AddMjcSiteFromJson(Scope *scope, const Json &site,
                        const std::string &name) {
  tn::PrimSpec *prim = scope->Add(name, "Sphere");
  if (!prim) return;
  AuthorMjcSite(prim, site, nullptr);
}

void AuthorMjcSite(tn::PrimSpec *prim, const Json &site,
                   const Matrix4 *local) {
  double radius = 0.005;
  ReadNumber(site, "size", &radius);
  if (radius <= 0.0) radius = 0.005;
  Set(prim, "radius", tn::Value(radius), "double");
  if (local) SetTransform(prim, *local);
  else SetTransformFromJson(prim, site);
  AddAPI(prim, "MjcSiteAPI");
  // MuJoCo 3.16's MjcSiteAPI has NewtonSiteAPI built in; Newton only loads
  // sites that are Gprim descendants of a rigid body.
  if (ActiveProfile().newton) AddAPI(prim, "NewtonSiteAPI");
  int32_t group = 0;
  if (ReadIntFrom(site, "", "group", &group)) {
    Set(prim, "mjc:group", tn::Value(group), "int", true);
  }
  // Sites are markers, not colliders / visuals by default (MuJoCo group>=3).
  SetToken(prim, "purpose", "guide", true);
}

void AddMjcTendonFromJson(Scope *scope, std::set<std::string> *siblings,
                          const Json &t, const NameMap &joint_names,
                          const NameMap &site_names, const NameMap &geom_names,
                          NameMap *tendon_names, size_t index,
                          std::string *warn) {
  std::string source = JsonString(t, "name");
  if (source.empty()) {
    const std::string prim_path = JsonString(t, "path");
    const size_t slash = prim_path.find_last_of('/');
    if (!prim_path.empty() && slash != std::string::npos &&
        slash + 1 < prim_path.size()) {
      source = prim_path.substr(slash + 1);
    }
  }
  if (source.empty()) source = "tendon_" + std::to_string(index);
  const std::string name = Sanitize(source, "tendon");
  const std::string type = JsonString(t, "type", "fixed");

  auto resolve_in = [](const NameMap &names, const char *scope_path,
                       const std::string &ref, std::string *out) {
    auto found = names.find(ref);
    if (found == names.end()) return false;
    *out = scope_path + found->second;
    return true;
  };
  auto resolve_route_ref = [&](const std::string &ref, std::string *out) {
    if (ref.empty()) return false;
    if (ref[0] == '/') {
      *out = ref;
      return true;
    }
    if (type == "spatial") return resolve_in(site_names, "/World/", ref, out);
    return resolve_in(joint_names, "/World/Joints/", ref, out);
  };
  auto resolve_site_ref = [&](const std::string &ref, std::string *out) {
    if (ref.empty()) return false;
    if (ref[0] == '/') {
      *out = ref;
      return true;
    }
    return resolve_in(site_names, "/World/", ref, out);
  };
  auto append_route_ref = [&](const std::string &ref,
                              std::vector<std::string> *out) {
    std::string resolved;
    if (resolve_route_ref(ref, &resolved)) {
      out->push_back(resolved);
      return true;
    }
    if (!ref.empty()) {
      AppendWarn(warn, "Tendon `" + name + "` route reference `" + ref +
                           "` could not be resolved; skipping it.\n");
    }
    return false;
  };
  auto append_route_object = [&](const Json &item,
                                 std::vector<std::string> *out) {
    if (!item.is_object()) return false;
    std::string ref = JsonString(item, "target");
    if (ref.empty()) ref = JsonString(item, "path");
    if (ref.empty() && type == "spatial") ref = JsonString(item, "site");
    if (ref.empty() && type == "spatial") ref = JsonString(item, "sidesite");
    if (ref.empty()) ref = JsonString(item, "joint");
    return append_route_ref(ref, out);
  };

  std::vector<std::string> targets;
  // Spatial waypoint encoding: per ordered element segment id and side site,
  // and one divisor per segment.
  bool spatial_waypoints = false;
  std::vector<int32_t> segments;
  std::vector<std::string> ordered_sides;
  std::vector<double> divisors;
  std::vector<double> coefs = JsonDoubles(t, "routeCoef");
  const std::vector<std::string> route = JsonStrings(t, "route");
  const Json *route_items = JsonArray(t, "route");
  const Json *path_items = JsonArray(t, "path");
  if (!route.empty()) {
    for (const std::string &ref : route) append_route_ref(ref, &targets);
  } else if (route_items) {
    for (const Json &item : *route_items) append_route_object(item, &targets);
  } else if (path_items && !JsonStrings(t, "path").empty()) {
    for (const std::string &ref : JsonStrings(t, "path")) {
      append_route_ref(ref, &targets);
    }
  } else if (type == "spatial") {
    // Spatial (muscle) tendon: ordered site / wrap-geom / pulley waypoints.
    // Every pulley, including a leading one, starts a new segment with its
    // divisor; segment 0 has divisor 1 (decoders emit a pulley whenever the
    // segment id increases, as in Newton's tendon import tests).
    spatial_waypoints = true;
    divisors.push_back(1.0);
    bool skip = false;
    static const Json kNoPath = Json::array();
    for (const Json &wp : path_items ? *path_items : kNoPath) {
      if (!wp.is_object()) continue;
      if (wp.contains("pulley")) {
        double divisor = 1.0;
        ReadNumber(wp, "pulley", &divisor);
        divisors.push_back(divisor);
        continue;
      }
      std::string resolved;
      std::string side;
      bool found = false;
      if (wp.contains("site")) {
        found = resolve_site_ref(JsonString(wp, "site"), &resolved);
        if (!found) {
          AppendWarn(warn, "Tendon `" + name + "` site `" + JsonString(wp, "site") +
                               "` could not be resolved; skipping the tendon.\n");
        }
      } else if (wp.contains("geom")) {
        const std::string geom = JsonString(wp, "geom");
        found = resolve_in(geom_names, "/World/", geom, &resolved);
        if (!found) {
          AppendWarn(warn, "Tendon `" + name + "` wrap geom `" + geom +
                               "` was not exported as a native sphere/cylinder; "
                               "skipping the tendon.\n");
        }
        const std::string side_ref = JsonString(wp, "sidesite");
        if (found && !side_ref.empty() && !resolve_site_ref(side_ref, &side)) {
          AppendWarn(warn, "Tendon `" + name + "` sidesite `" + side_ref +
                               "` could not be resolved; wrapping without it.\n");
        }
      } else {
        found = append_route_object(wp, &targets);
        if (found) {
          segments.push_back(static_cast<int32_t>(divisors.size() - 1));
          ordered_sides.emplace_back();
        }
        continue;
      }
      if (!found) {
        skip = true;
        break;
      }
      targets.push_back(resolved);
      segments.push_back(static_cast<int32_t>(divisors.size() - 1));
      ordered_sides.push_back(side);
    }
    if (skip) targets.clear();
  } else if (const Json *joints = JsonArray(t, "joints")) {
    // Fixed tendon: linear combination of joint coordinates.
    for (const Json &je : *joints) {
      if (!je.is_object()) continue;
      const std::string jname = JsonString(je, "joint");
      auto found = joint_names.find(jname);
      if (found == joint_names.end()) {
        AppendWarn(warn, "Tendon `" + name + "` references joint `" + jname +
                             "` that was not exported; skipping it.\n");
        continue;
      }
      targets.push_back("/World/Joints/" + found->second);
      double coef = 1.0;
      ReadNumber(je, "coef", &coef);
      coefs.push_back(coef);
    }
  }

  if (targets.empty()) {
    AppendWarn(warn, type == "spatial"
                         ? "Skipping spatial tendon `" + name +
                               "`: no routing sites were exported.\n"
                         : "Skipping tendon `" + name +
                               "`: no referenced joint was exported.\n");
    return;
  }
  if (type == "spatial" && targets.size() < 2) {
    AppendWarn(warn, "Spatial tendon `" + name +
                         "` has fewer than 2 routing sites; preserving the "
                         "partial route.\n");
  }

  // Record the authored (uniquified) name only once the tendon exists, so
  // actuators never target a skipped tendon or a sibling's renamed prim.
  const std::string usd_name = ChildName(name, siblings);
  tn::PrimSpec *prim = scope->Add(usd_name, "MjcTendon");
  if (!prim) return;
  tendon_names->emplace(source, usd_name);
  SetToken(prim, "mjc:type", type, true);
  // USD relationship targets are unique, while a closed tendon route may
  // visit the same site repeatedly. Index the ordered route into that rel.
  std::vector<std::string> unique_targets;
  std::vector<int32_t> route_indices;
  std::map<std::string, int32_t> target_index;
  for (const auto& target : targets) {
    auto inserted = target_index.emplace(target, static_cast<int32_t>(unique_targets.size()));
    if (inserted.second) unique_targets.push_back(target);
    route_indices.push_back(inserted.first->second);
  }
  SetRel(prim, "mjc:path", unique_targets);
  if (spatial_waypoints || unique_targets.size() != targets.size())
    Set(prim, "mjc:path:indices", tn::Value::MakeIntArray(route_indices), "int[]", true);
  if (spatial_waypoints) {
    Set(prim, "mjc:path:segments", tn::Value::MakeIntArray(segments), "int[]", true);
    SetDoubles(prim, "mjc:path:divisors", divisors, true);
    std::vector<std::string> sides;
    std::vector<int32_t> side_indices;
    bool any_side = false;
    for (const std::string &side : ordered_sides) {
      if (side.empty()) {
        side_indices.push_back(-1);
        continue;
      }
      any_side = true;
      auto at = std::find(sides.begin(), sides.end(), side);
      side_indices.push_back(static_cast<int32_t>(at - sides.begin()));
      if (at == sides.end()) sides.push_back(side);
    }
    if (any_side) {
      SetRel(prim, "mjc:sideSites", sides);
      Set(prim, "mjc:sideSites:indices", tn::Value::MakeIntArray(side_indices), "int[]", true);
    }
  }
  std::vector<double> route_coef = JsonDoubles(t, "routeCoef");
  if (!route_coef.empty()) coefs = std::move(route_coef);
  if (!coefs.empty()) SetDoubles(prim, "mjc:path:coef", coefs, true);

  std::vector<std::string> side_sites;
  for (const std::string &ref : JsonStrings(t, "sideSites")) {
    if (spatial_waypoints) break;
    std::string resolved;
    if (resolve_site_ref(ref, &resolved)) {
      side_sites.push_back(resolved);
    } else {
      AppendWarn(warn, "Tendon `" + name + "` sideSite reference `" + ref +
                           "` could not be resolved; skipping it.\n");
    }
  }
  const Json *side_items = JsonArray(t, "sideSites");
  if (!spatial_waypoints && side_sites.empty() && side_items) {
    for (const Json &item : *side_items) {
      if (!item.is_object()) continue;
      std::string ref = JsonString(item, "target");
      if (ref.empty()) ref = JsonString(item, "path");
      if (ref.empty()) ref = JsonString(item, "site");
      if (ref.empty()) ref = JsonString(item, "sidesite");
      std::string resolved;
      if (resolve_site_ref(ref, &resolved)) {
        side_sites.push_back(resolved);
      } else if (!ref.empty()) {
        AppendWarn(warn, "Tendon `" + name + "` sideSite reference `" + ref +
                             "` could not be resolved; skipping it.\n");
      }
    }
  }
  if (!spatial_waypoints) SetRel(prim, "mjc:sideSites", side_sites);

  const std::pair<const char *, const char *> int_arrays[] = {
      {"routeIndices", "mjc:path:indices"},
      {"sideSiteIndices", "mjc:sideSites:indices"},
      {"routeSegments", "mjc:path:segments"}};
  for (const auto &entry : int_arrays) {
    std::vector<int32_t> values = JsonInts(t, entry.first);
    if (!values.empty()) {
      Set(prim, entry.second, tn::Value::MakeIntArray(std::move(values)),
          "int[]", true);
    }
  }
  std::vector<double> route_divisors = JsonDoubles(t, "routeDivisors");
  if (!spatial_waypoints && !route_divisors.empty())
    SetDoubles(prim, "mjc:path:divisors", route_divisors, true);

  const std::vector<double> rgba = JsonDoubles(t, "rgba");
  if (rgba.size() >= 4) {
    Set(prim, "mjc:rgba",
        tn::Value::MakeColor4f(static_cast<float>(rgba[0]),
                               static_cast<float>(rgba[1]),
                               static_cast<float>(rgba[2]),
                               static_cast<float>(rgba[3])),
        "color4f", true);
  }
  if (t.contains("group")) {
    Set(prim, "mjc:group", tn::Value(JsonInt(t, "group", 0)), "int", true);
  }
  for (const char *key : {"stiffness", "damping", "frictionloss", "margin",
                          "armature", "width"}) {
    double v = 0.0;
    if (ReadNumber(t, key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "double", true);
    }
  }
  double lo = 0.0, hi = 0.0;
  if (ReadRange(t, "range", &lo, &hi)) {
    Set(prim, "mjc:range:min", tn::Value(lo), "double", true);
    Set(prim, "mjc:range:max", tn::Value(hi), "double", true);
    // MJCF `limited` is a true/false/auto token; legacy payloads omit it and
    // imply a limit whenever a range exists.
    SetToken(prim, "mjc:limited", JsonString(t, "limited", "true"), true);
  } else if (t.contains("limited")) {
    SetToken(prim, "mjc:limited", JsonString(t, "limited"), true);
  }
  if (ReadRange(t, "actuatorfrcrange", &lo, &hi)) {
    Set(prim, "mjc:actuatorfrcrange:min", tn::Value(lo), "double", true);
    Set(prim, "mjc:actuatorfrcrange:max", tn::Value(hi), "double", true);
  }
  const std::pair<const char *, const char *> scalars[] = {
      {"range_min", "mjc:range:min"},
      {"range_max", "mjc:range:max"},
      {"actuatorfrcrange_min", "mjc:actuatorfrcrange:min"},
      {"actuatorfrcrange_max", "mjc:actuatorfrcrange:max"}};
  for (const auto &entry : scalars) {
    double v = 0.0;
    if (ReadNumber(t, entry.first, &v)) {
      Set(prim, entry.second, tn::Value(v), "double", true);
    }
  }
  if (t.contains("limited")) {
    SetToken(prim, "mjc:limited", JsonString(t, "limited", "auto"), true);
  }
  if (t.contains("actuatorfrclimited")) {
    SetToken(prim, "mjc:actuatorfrclimited",
             JsonString(t, "actuatorfrclimited", "auto"), true);
  }
  for (const char *key : {"springlength", "solreflimit", "solimplimit",
                          "solreffriction", "solimpfriction"}) {
    std::vector<double> values = JsonDoubles(t, key);
    if (!values.empty()) {
      SetDoubles(prim, std::string("mjc:") + key, std::move(values), true);
    }
  }
}

namespace {
// Row-major USD (row-vector) matrix from position + quaternion (w, x, y, z).
Matrix4 PoseMatrix(const double p[3], double w, double x, double y, double z) {
  const double n = std::sqrt(w * w + x * x + y * y + z * z);
  if (n > 1e-12) { w /= n; x /= n; y /= n; z /= n; } else { w = 1; x = y = z = 0; }
  // Column-vector rotation R; the row-vector matrix stores R^T.
  const double r[3][3] = {
      {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
      {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
      {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  Matrix4 m = IdentityMatrix();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[i * 4 + j] = r[j][i];
  for (int j = 0; j < 3; ++j) m[12 + j] = p[j];
  return m;
}

void SetLocalFrame(tn::PrimSpec *prim, int index, const Matrix4 &local) {
  const std::string suffix = std::to_string(index);
  SetPoint3(prim, "physics:localPos" + suffix,
            {static_cast<float>(local[12]), static_cast<float>(local[13]),
             static_cast<float>(local[14])},
            false);
  double r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = local[j * 4 + i];
  Set(prim, "physics:localRot" + suffix, RotationMatrixToQuatf(r), "quatf");
}
}  // namespace

void AddEqualityConstraint(tn::Layer *layer, Scope *scope,
                           std::set<std::string> *siblings, const Json &e,
                           size_t index, const NameMap &link_names,
                           const std::map<std::string, Matrix4> &link_worlds,
                           const std::set<std::string> &static_links,
                           const NameMap &joint_names, const Json &sites,
                           std::string *warn) {
  const std::string type = JsonString(e, "type", "connect");
  const std::string name = Sanitize(
      JsonString(e, "name", "equality_" + std::to_string(index)), "equality");
  bool active = true;
  ReadBool(e, "active", &active);
  const std::vector<double> solref = JsonDoubles(e, "solref");
  const std::vector<double> solimp = JsonDoubles(e, "solimp");
  auto solver = [&](tn::PrimSpec *prim) {
    if (!solref.empty()) SetDoubles(prim, "mjc:solref", solref);
    if (!solimp.empty()) SetDoubles(prim, "mjc:solimp", solimp);
  };
  constexpr double kPi = 3.14159265358979323846;

  if (type == "joint") {
    // joint1 = polycoef(joint2): joint1 follows joint2.
    auto follower = joint_names.find(JsonString(e, "joint1"));
    if (follower == joint_names.end()) {
      AppendWarn(warn, "Skipping joint equality `" + name + "`: joint1 was not exported.\n");
      return;
    }
    tn::PrimSpec *prim = layer->prim_at_path_mutable("/World/Joints/" + follower->second);
    if (!prim) return;
    std::vector<double> coef = JsonDoubles(e, "polycoef");
    coef.resize(std::max<size_t>(coef.size(), 5), 0.0);
    if (!e.contains("polycoef")) coef[1] = 1.0;  // MuJoCo default 0 1 0 0 0
    const bool angular = prim->type_name() == "PhysicsRevoluteJoint";
    AddAPI(prim, "MjcEqualityJointAPI");
    solver(prim);
    for (int i = 2; i < 5; ++i)
      if (coef[size_t(i)] != 0.0)
        Set(prim, "mjc:coef" + std::to_string(i), tn::Value(coef[size_t(i)]), "double");
    auto leader = joint_names.find(JsonString(e, "joint2"));
    if (leader == joint_names.end()) {
      // A single-joint equality pins joint1 to coef0; keep the MJC form.
      Set(prim, "mjc:coef0", tn::Value(coef[0]), "double");
      if (ActiveProfile().newton)
        AppendWarn(warn, "Joint equality `" + name +
                             "` has no second joint; Newton's USD importer has "
                             "no mimic form for it and skips it.\n");
      return;
    }
    SetRel(prim, "mjc:target", {"/World/Joints/" + leader->second});
    AddAPI(prim, "NewtonMimicAPI");
    SetRel(prim, "newton:mimicJoint", {"/World/Joints/" + leader->second});
    // NewtonMimicAPI coef0 is in the follower's position unit (degrees).
    Set(prim, "newton:mimicCoef0",
        tn::Value(static_cast<float>(angular ? coef[0] * 180.0 / kPi : coef[0])), "float");
    Set(prim, "newton:mimicCoef1", tn::Value(static_cast<float>(coef[1])), "float");
    Set(prim, "newton:mimicEnabled", tn::Value(active), "bool");
    if (coef[2] != 0.0 || coef[3] != 0.0 || coef[4] != 0.0)
      AppendWarn(warn, "Joint equality `" + name +
                           "` has nonlinear coefficients; NewtonMimicAPI keeps only "
                           "the linear terms (mjc:coef2..4 preserved).\n");
    return;
  }

  // connect / weld: a loop-closing joint between body1 and body2 (or world).
  std::string body1 = JsonString(e, "body1");
  std::string body2 = JsonString(e, "body2");
  // Site form: bodies and per-side frames come from the two sites.
  const std::string site1 = JsonString(e, "site1"), site2 = JsonString(e, "site2");
  const Json *site_json[2] = {nullptr, nullptr};
  if (!site1.empty() || !site2.empty()) {
    for (const Json &site : sites) {
      if (JsonString(site, "name") == site1) site_json[0] = &site;
      if (JsonString(site, "name") == site2) site_json[1] = &site;
    }
    if (!site_json[0] || !site_json[1]) {
      AppendWarn(warn, "Skipping equality `" + name + "`: its sites were not exported.\n");
      return;
    }
    body1 = JsonString(*site_json[0], "body");
    body2 = JsonString(*site_json[1], "body");
  }
  auto world_of = [&](const std::string &body, bool *is_world) {
    *is_world = body.empty() || static_links.count(body) > 0 || !link_names.count(body);
    auto found = link_worlds.find(body);
    return *is_world || found == link_worlds.end() ? IdentityMatrix() : found->second;
  };
  bool world1 = false, world2 = false;
  const Matrix4 w1 = world_of(body1, &world1);
  const Matrix4 w2 = world_of(body2, &world2);
  if (!body1.empty() && !link_names.count(body1)) {
    AppendWarn(warn, "Skipping equality `" + name + "`: body1 was not exported.\n");
    return;
  }
  if (world1 && world2) {
    AppendWarn(warn, "Skipping equality `" + name + "`: both bodies are static.\n");
    return;
  }
  std::vector<double> anchor = JsonDoubles(e, "anchor");
  anchor.resize(3, 0.0);
  const double rot_identity[4] = {1, 0, 0, 0};
  Matrix4 frame_world;
  // A weld relpose fixes body2's target pose relative to body1, so body2's
  // frame is the anchor in body2 itself rather than its rest-pose offset.
  bool body2_anchor_frame = false;
  if (type == "weld") {
    const std::vector<double> rel = JsonDoubles(e, "relpose");
    const bool has_rel = rel.size() == 7 &&
                         (rel[3] != 0.0 || rel[4] != 0.0 || rel[5] != 0.0 || rel[6] != 0.0);
    if (has_rel) {
      // body2's pose in body1, then the anchor point in body2.
      const double rp[3] = {rel[0], rel[1], rel[2]};
      const Matrix4 rel_m = PoseMatrix(rp, rel[3], rel[4], rel[5], rel[6]);
      const double ap[3] = {anchor[0], anchor[1], anchor[2]};
      frame_world = MultiplyRowMajor(
          MultiplyRowMajor(PoseMatrix(ap, 1, 0, 0, 0), rel_m), w1);
      body2_anchor_frame = !world2;
    } else {
      const double ap[3] = {anchor[0], anchor[1], anchor[2]};
      frame_world = MultiplyRowMajor(PoseMatrix(ap, 1, 0, 0, 0), w2);
    }
  } else {
    const double ap[3] = {anchor[0], anchor[1], anchor[2]};
    frame_world = MultiplyRowMajor(PoseMatrix(ap, 1, 0, 0, 0), w1);
  }
  (void)rot_identity;
  tn::PrimSpec *prim = scope->Add(ChildName(name, siblings),
                                  type == "weld" ? "PhysicsFixedJoint" : "PhysicsSphericalJoint");
  if (!prim) return;
  // body1 must be a dynamic body; an empty body0 is the world.
  struct Side { std::string body; Matrix4 world; bool is_world; };
  Side a{body1, w1, world1}, b{body2, w2, world2};
  Matrix4 frame_a = frame_world, frame_b = frame_world;
  if (body2_anchor_frame) {
    const double ap[3] = {anchor[0], anchor[1], anchor[2]};
    frame_b = MultiplyRowMajor(PoseMatrix(ap, 1, 0, 0, 0), w2);
  }
  if (site_json[0]) {
    frame_a = MatrixFromArray(JsonDoubles(*site_json[0], "matrix"));
    frame_b = MatrixFromArray(JsonDoubles(*site_json[1], "matrix"));
  }
  if (b.is_world) {
    std::swap(a, b);
    std::swap(frame_a, frame_b);
  }
  if (!a.is_world) SetRel(prim, "physics:body0", {"/World/Links/" + link_names.at(a.body)});
  SetRel(prim, "physics:body1", {"/World/Links/" + link_names.at(b.body)});
  SetLocalFrame(prim, 0, MultiplyRowMajor(frame_a, InverseRigid(a.world)));
  SetLocalFrame(prim, 1, MultiplyRowMajor(frame_b, InverseRigid(b.world)));
  Set(prim, "physics:jointEnabled", tn::Value(active), "bool");
  Set(prim, "physics:collisionEnabled", tn::Value(false), "bool");
  Set(prim, "physics:excludeFromArticulation", tn::Value(true), "bool");
  AddAPI(prim, type == "weld" ? "MjcEqualityWeldAPI" : "MjcEqualityConnectAPI");
  solver(prim);
  double v = 0.0;
  if (type == "weld" && ReadNumber(e, "torquescale", &v))
    Set(prim, "mjc:torqueScale", tn::Value(static_cast<float>(v)), "float");
}

void AddMjcEqualityFromJson(Scope *scope, std::set<std::string> *siblings,
                            const Json &e, const NameMap &link_names,
                            const NameMap &joint_names, size_t index) {
  const std::string type = JsonString(e, "type", "connect");
  const std::string name = Sanitize(
      JsonString(e, "name", "equality_" + std::to_string(index)), "equality");
  tn::PrimSpec *prim = scope->Add(ChildName(name, siblings), "Xform");
  if (!prim) return;
  AddAPI(prim, type == "weld"    ? "MjcEqualityWeldAPI"
               : type == "joint" ? "MjcEqualityJointAPI"
                                 : "MjcEqualityConnectAPI");
  std::vector<std::string> targets;
  auto resolve = [&](const NameMap &names, const char *scope_path,
                     const std::string &n) {
    auto found = names.find(n);
    if (found != names.end()) targets.push_back(scope_path + found->second);
  };
  if (type == "joint") {
    resolve(joint_names, "/World/Joints/", JsonString(e, "joint1"));
    resolve(joint_names, "/World/Joints/", JsonString(e, "joint2"));
  } else {
    resolve(link_names, "/World/Links/", JsonString(e, "body1"));
    resolve(link_names, "/World/Links/", JsonString(e, "body2"));
  }
  SetRel(prim, "mjc:target", targets);

  double v = 0.0;
  if (type == "weld" && ReadNumber(e, "torquescale", &v)) {
    Set(prim, "mjc:torqueScale", tn::Value(static_cast<float>(v)), "float");
  }
  if (type == "joint") {
    if (const Json *pc = JsonArray(e, "polycoef")) {
      for (size_t i = 0; i < pc->size() && i < 5; i++) {
        if ((*pc)[i].is_number()) {
          Set(prim, "mjc:coef" + std::to_string(i),
              tn::Value((*pc)[i].get<double>()), "double");
        }
      }
    }
  }
  for (const char *key : {"solref", "solimp", "anchor"}) {
    std::vector<double> values = JsonDoubles(e, key);
    if (!values.empty()) {
      SetDoubles(prim, std::string("mjc:") + key, std::move(values));
    }
  }
}

void AddMjcActuatorFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &a, const NameMap &joint_names,
                            const NameMap &tendon_names,
                            const NameMap &site_names,
                            const NameMap &link_names, size_t index,
                            std::string *warn) {
  auto resolve = [](const NameMap &names, const char *scope_path,
                    const std::string &n) -> std::vector<std::string> {
    auto found = names.find(n);
    if (found == names.end()) return {};
    return {scope_path + found->second};
  };
  const std::string t_tendon = JsonString(a, "targetTendon");
  const std::string t_joint = JsonString(a, "targetJoint");
  const std::string t_site = JsonString(a, "targetSite");
  const std::string t_body = JsonString(a, "targetBody");
  const std::string t_path = JsonString(a, "target");
  std::vector<std::string> targets;
  if (!t_path.empty() && t_path[0] == '/') {
    targets.push_back(t_path);
  } else if (!t_tendon.empty()) {
    targets = resolve(tendon_names, "/World/Tendons/", t_tendon);
  } else if (!t_joint.empty()) {
    targets = resolve(joint_names, "/World/Joints/", t_joint);
  } else if (!t_site.empty()) {
    targets = resolve(site_names, "/World/", t_site);
  } else if (!t_body.empty()) {  // <adhesion body=..>
    targets = resolve(link_names, "/World/Links/", t_body);
  }
  const std::string source_name =
      JsonString(a, "name", "actuator_" + std::to_string(index));
  if (targets.empty()) {
    // MuJoCo skips and Newton rejects an actuator without exactly one target
    // (e.g. one driving a tendon that could not be exported).
    AppendWarn(warn, "Skipping actuator `" + source_name +
                         "`: its transmission target was not exported.\n");
    return;
  }
  const std::string name = Unique(source_name, "actuator", used);
  tn::PrimSpec *prim = scope->Add(name, "MjcActuator");
  if (!prim) return;
  SetRel(prim, "mjc:target", targets);

  for (const char *key : {"gainPrm", "biasPrm", "dynPrm", "gear"}) {
    std::vector<double> values = JsonDoubles(a, key);
    if (!values.empty()) {
      SetDoubles(prim, std::string("mjc:") + key, std::move(values), true);
    }
  }
  // `<range>: [min, max]` pairs, then the explicit `<range>_min/_max`
  // scalars override them.
  for (const char *key : {"lengthRange", "ctrlRange", "forceRange",
                          "actRange"}) {
    double lo = 0.0, hi = 0.0;
    if (ReadRange(a, key, &lo, &hi)) {
      Set(prim, std::string("mjc:") + key + ":min", tn::Value(lo), "double",
          true);
      Set(prim, std::string("mjc:") + key + ":max", tn::Value(hi), "double",
          true);
    }
  }
  for (const char *key : {"ctrlRange", "forceRange", "actRange",
                          "lengthRange"}) {
    for (const char *bound : {"min", "max"}) {
      double v = 0.0;
      if (ReadNumber(a, (std::string(key) + "_" + bound).c_str(), &v)) {
        Set(prim, std::string("mjc:") + key + ":" + bound, tn::Value(v),
            "double", true);
      }
    }
  }
  for (const char *key : {"crankLength", "inheritRange"}) {
    double v = 0.0;
    if (ReadNumber(a, key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "double", true);
    }
  }
  for (const char *key : {"group", "actDim"}) {
    int32_t v = 0;
    if (ReadIntFrom(a, "", key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "int", true);
    }
  }
  for (const char *key : {"jointInParent", "actEarly"}) {
    bool v = false;
    if (ReadBool(a, key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "bool", true);
    }
  }
  const std::pair<const char *, const char *> tokens[] = {
      {"gainType", "fixed"},     {"biasType", "none"},
      {"dynType", "none"},       {"ctrlLimited", "auto"},
      {"forceLimited", "auto"},  {"actLimited", "auto"},
      {"plugin", ""},            {"instance", ""}};
  for (const auto &entry : tokens) {
    if (a.contains(entry.first)) {
      SetToken(prim, std::string("mjc:") + entry.first,
               JsonString(a, entry.first, entry.second), true);
    }
  }
  for (const char *key : {"refSite", "sliderSite"}) {
    const std::string site = JsonString(a, key);
    if (site.empty()) continue;
    SetRel(prim, std::string("mjc:") + key,
           site[0] == '/' ? std::vector<std::string>{site}
                          : resolve(site_names, "/World/", site));
  }
}

void AddMjcKeyframeFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &k, size_t index) {
  const std::string name =
      Unique(JsonString(k, "name", "key_" + std::to_string(index)), "key", used);
  tn::PrimSpec *prim = scope->Add(name, "MjcKeyframe");
  if (!prim) return;
  for (const char *key : {"qpos", "qvel", "act", "ctrl", "mpos", "mquat"}) {
    std::vector<double> values = JsonDoubles(k, key);
    if (!values.empty()) {
      SetDoubles(prim, std::string("mjc:") + key, std::move(values), true);
    }
  }
}

void AddMjcSensorFromJson(Scope *scope, std::set<std::string> *used,
                          const Json &s, size_t index) {
  const std::string name = Unique(
      JsonString(s, "name", "sensor_" + std::to_string(index)), "sensor", used);
  tn::PrimSpec *prim = scope->Add(name, "MjcSensor");
  if (!prim) return;
  const char *keys[5] = {"type", "objtype", "objname", "reftype", "refname"};
  std::string fields[5];
  for (int i = 0; i < 5; i++) fields[i] = JsonString(s, keys[i]);
  auto alias = [&](const char *key, const char *type, std::string *type_field,
                   std::string *name_field) {
    if (!type_field->empty() || !name_field->empty()) return;
    const std::string target = JsonString(s, key);
    if (!target.empty()) {
      *type_field = type;
      *name_field = target;
    }
  };
  const std::pair<const char *, const char *> target_aliases[] = {
      {"site", "site"},         {"joint", "joint"},
      {"actuator", "actuator"}, {"tendon", "tendon"},
      {"body", "body"},         {"xbody", "xbody"},
      {"geom", "geom"},         {"camera", "camera"},
      {"light", "light"},       {"sensor", "sensor"},
      {"numeric", "numeric"},   {"text", "text"},
      {"tuple", "tuple"},       {"key", "key"},
      {"plugin", "plugin"}};
  for (const auto &entry : target_aliases) {
    alias(entry.first, entry.second, &fields[1], &fields[2]);
  }
  const std::pair<const char *, const char *> ref_aliases[] = {
      {"refsite", "site"},         {"refSite", "site"},
      {"refjoint", "joint"},       {"refJoint", "joint"},
      {"refactuator", "actuator"}, {"refActuator", "actuator"},
      {"reftendon", "tendon"},     {"refTendon", "tendon"},
      {"refbody", "body"},         {"refBody", "body"},
      {"refxbody", "xbody"},       {"refXbody", "xbody"},
      {"refgeom", "geom"},         {"refGeom", "geom"},
      {"refcamera", "camera"},     {"refCamera", "camera"},
      {"reflight", "light"},       {"refLight", "light"}};
  for (const auto &entry : ref_aliases) {
    alias(entry.first, entry.second, &fields[3], &fields[4]);
  }
  for (int i = 0; i < 5; i++) {
    if (!fields[i].empty()) {
      SetToken(prim, std::string("mjc:") + keys[i], fields[i], true);
    }
  }
  int32_t group = 0;
  if (ReadIntFrom(s, "", "group", &group)) {
    Set(prim, "mjc:group", tn::Value(group), "int", true);
  }
  for (const char *key : {"cutoff", "noise"}) {
    double v = 0.0;
    if (ReadNumber(s, key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "double", true);
    }
  }
  std::vector<double> user = JsonDoubles(s, "user");
  if (!user.empty()) SetDoubles(prim, "mjc:user", std::move(user), true);
}

void AddContactPairFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &p, size_t index) {
  const std::string name =
      Unique(JsonString(p, "name", "pair_" + std::to_string(index)), "pair",
             used);
  tn::PrimSpec *prim = scope->Add(name, "Xform");
  if (!prim) return;
  SetToken(prim, "mjc:geom1", JsonString(p, "geom1"), true);
  SetToken(prim, "mjc:geom2", JsonString(p, "geom2"), true);
  int32_t condim = 0;
  if (ReadIntFrom(p, "", "condim", &condim)) {
    Set(prim, "mjc:condim", tn::Value(condim), "int", true);
  }
  for (const char *key : {"margin", "gap"}) {
    double v = 0.0;
    if (ReadNumber(p, key, &v)) {
      Set(prim, std::string("mjc:") + key, tn::Value(v), "double");
    }
  }
  for (const char *key : {"friction", "solref", "solimp"}) {
    std::vector<double> values = JsonDoubles(p, key);
    if (!values.empty()) {
      SetDoubles(prim, std::string("mjc:") + key, std::move(values));
    }
  }
}

}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
