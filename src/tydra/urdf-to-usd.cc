// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.

#include "tydra/urdf-to-usd.hh"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/attribute.hh"
#include "core/composition-types.hh"
#include "core/path.hh"
#include "core/prim-enums.hh"
#include "core/prim.hh"
#include "core/property.hh"
#include "core/relationship.hh"
#include "core/xform-op.hh"
#include "mjcPhysics.hh"
#include "minijson.hh"
#include "nonstd/optional.hpp"
#include "stage.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdPhysics.hh"
#include "usdShade.hh"
#include "value-types.hh"
#include "xform.hh"

namespace lightusd {
namespace tydra {
namespace {

// Keep the established local spelling to minimize churn in this large
// converter; this translation unit intentionally binds it to the bundled
// minijson implementation and does not include nlohmann/json.hpp.
namespace nlohmann {
using json = ::lightusd::minijson::Value;
}

struct MiniURDFPayload {
  nlohmann::json root;
  nlohmann::json empty_array = minijson::Value::array();
  bool mjcf_source{false};

  const nlohmann::json &Array(const char *name) const {
    if (root.contains(name) && root.at(name).is_array()) return root.at(name);
    return empty_array;
  }

  static bool Parse(const std::string &text, MiniURDFPayload *out,
                    std::string *err) {
    if (!out) {
      if (err) *err = "URDF payload output is null";
      return false;
    }
    minijson::Error parse_err;
    minijson::ParseOptions options;
    if (!minijson::Parse(text, &out->root, &parse_err, options) ||
        !out->root.is_object()) {
      if (err) *err = "URDF export JSON parse failed: " + parse_err.message;
      return false;
    }
    auto string_value = [&](const char *key) -> std::string {
      if (!out->root.contains(key) || !out->root.at(key).is_string()) {
        return std::string();
      }
      return out->root.at(key).get<std::string>();
    };
    const std::string source_format = string_value("sourceFormat");
    const std::string input_format = string_value("inputFormat");
    out->mjcf_source = source_format == "mjcf" || source_format == "MJCF" ||
                       input_format == "mjcf" || input_format == "MJCF";
    if (out->Array("links").empty()) {
      if (err) *err = "URDF export JSON has no links";
      return false;
    }
    return true;
  }
};

constexpr int32_t kMjcfDefaultGroup = 0;
constexpr int32_t kMjcfDefaultCondim = 3;
constexpr double kMjcfDefaultSolmix = 1.0;
constexpr double kMjcfDefaultMargin = 0.0;
constexpr double kMjcfDefaultGap = 0.0;
constexpr int32_t kLegacyUrdfCollisionGroup = 3;
constexpr int32_t kLegacyUrdfVisualGroup = 2;
constexpr int32_t kDefaultNewtonMaxHullVertices = 64;

std::string SanitizeUSDIdentifier(const std::string &name,
                                  const std::string &fallback) {
  std::string out;
  out.reserve(name.empty() ? fallback.size() : name.size());
  for (char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '_') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) {
    out = fallback;
  }
  if (!(std::isalpha(static_cast<unsigned char>(out[0])) || out[0] == '_')) {
    out.insert(out.begin(), '_');
  }
  return out;
}

std::string UniqueUSDIdentifier(const std::string &name,
                                std::set<std::string> &used,
                                const std::string &fallback) {
  const std::string base = SanitizeUSDIdentifier(name, fallback);
  std::string candidate = base;
  int suffix = 1;
  while (used.count(candidate)) {
    candidate = base + "_" + std::to_string(suffix++);
  }
  used.insert(candidate);
  return candidate;
}

bool JsonNumber(const nlohmann::json &j, const char *key, double *out) {
  if (!out || !j.is_object() || !j.contains(key) || !j.at(key).is_number()) {
    return false;
  }
  *out = j.at(key).get<double>();
  return true;
}

bool JsonNumber(const nlohmann::json &j, const char *key, float *out) {
  double v = 0.0;
  if (!JsonNumber(j, key, &v)) {
    return false;
  }
  *out = static_cast<float>(v);
  return true;
}

bool JsonBool(const nlohmann::json &j, const char *key, bool *out) {
  if (!out || !j.is_object() || !j.contains(key)) {
    return false;
  }
  const auto &item = j.at(key);
  if (item.is_boolean()) {
    *out = item.get<bool>();
    return true;
  }
  if (item.is_number_integer()) {
    *out = item.get<int32_t>() != 0;
    return true;
  }
  return false;
}

std::string JsonString(const nlohmann::json &j, const char *key,
                       const std::string &fallback = std::string()) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) {
    return fallback;
  }
  return j.at(key).get<std::string>();
}

const nlohmann::json *JsonObjectOrNull(const nlohmann::json &j,
                                       const char *key) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_object()) {
    return nullptr;
  }
  return &j.at(key);
}

std::vector<double> JsonDoubleArray(const nlohmann::json &j, const char *key) {
  std::vector<double> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
    return out;
  }
  for (const auto &item : j.at(key)) {
    if (item.is_number()) {
      out.push_back(item.get<double>());
    }
  }
  return out;
}

std::vector<double> JsonDoubleArrayFromObjectOrParent(
    const nlohmann::json &src, const char *object_key, const char *key) {
  if (const nlohmann::json *obj = JsonObjectOrNull(src, object_key)) {
    std::vector<double> out = JsonDoubleArray(*obj, key);
    if (!out.empty()) {
      return out;
    }
  }
  return JsonDoubleArray(src, key);
}

std::vector<float> JsonFloatArray(const nlohmann::json &j, const char *key) {
  std::vector<float> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
    return out;
  }
  for (const auto &item : j.at(key)) {
    if (item.is_number()) {
      out.push_back(static_cast<float>(item.get<double>()));
    }
  }
  return out;
}

int32_t JsonInt(const nlohmann::json &j, const char *key,
                int32_t fallback) {
  if (!j.is_object() || !j.contains(key)) {
    return fallback;
  }
  const auto &item = j.at(key);
  if (item.is_number_integer()) {
    return item.get<int32_t>();
  }
  if (item.is_number()) {
    return static_cast<int32_t>(item.get<double>());
  }
  return fallback;
}

std::vector<int32_t> JsonIntArray(const nlohmann::json &j, const char *key) {
  std::vector<int32_t> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
    return out;
  }
  for (const auto &item : j.at(key)) {
    if (item.is_number_integer()) {
      out.push_back(item.get<int32_t>());
    } else if (item.is_number()) {
      out.push_back(static_cast<int32_t>(item.get<double>()));
    }
  }
  return out;
}

std::vector<std::string> JsonStringArray(const nlohmann::json &j,
                                         const char *key) {
  std::vector<std::string> out;
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
    return out;
  }
  for (const auto &item : j.at(key)) {
    if (item.is_string()) {
      out.push_back(item.get<std::string>());
    }
  }
  return out;
}

value::matrix4d MatrixFromUSDArray(const std::vector<double> &flat) {
  value::matrix4d m;
  Identity(&m);
  if (flat.size() == 16) {
    size_t idx = 0;
    for (size_t r = 0; r < 4; r++) {
      for (size_t c = 0; c < 4; c++) {
        m.m[r][c] = flat[idx++];
      }
    }
  }
  return m;
}

Axis AxisFromToken(const std::string &axis) {
  if (axis == "X" || axis == "x") {
    return Axis::X;
  }
  if (axis == "Y" || axis == "y") {
    return Axis::Y;
  }
  return Axis::Z;
}

void AddAPISchemas(
    PrimMeta &meta,
    const std::vector<std::pair<APISchemas::APIName, std::string>> &schemas) {
  APISchemas api;
  api.listOpQual = ListEditQual::Prepend;
  api.names = schemas;
  meta.set_apiSchemas(api);
}

void AppendAPISchema(PrimMeta &meta, APISchemas::APIName name,
                     const std::string &instance_name = std::string()) {
  APISchemas api;
  if (meta.has_apiSchemas()) {
    api = meta.get_apiSchemas();
  } else {
    api.listOpQual = ListEditQual::Prepend;
  }
  for (const auto &schema : api.names) {
    if (schema.first == name && schema.second == instance_name) {
      meta.set_apiSchemas(api);
      return;
    }
  }
  api.names.push_back({name, instance_name});
  meta.set_apiSchemas(api);
}

template <typename T>
void AddAttr(std::map<std::string, Property> &props, const std::string &name,
             const T &value, bool uniform = false) {
  Attribute attr;
  attr.set_value(value);
  if (uniform) {
    attr.variability() = Variability::Uniform;
  }
  props[name] = Property(std::move(attr), false);
}

double JsonNumberFromObjectOrParent(const nlohmann::json &src,
                                    const char *object_key,
                                    const char *key,
                                    double fallback) {
  double value = fallback;
  if (const nlohmann::json *obj = JsonObjectOrNull(src, object_key)) {
    if (JsonNumber(*obj, key, &value)) {
      return value;
    }
  }
  JsonNumber(src, key, &value);
  return value;
}

int32_t JsonIntFromObjectOrParent(const nlohmann::json &src,
                                  const char *object_key,
                                  const char *key,
                                  int32_t fallback) {
  if (const nlohmann::json *obj = JsonObjectOrNull(src, object_key)) {
    if (obj->contains(key)) {
      return JsonInt(*obj, key, fallback);
    }
  }
  return JsonInt(src, key, fallback);
}

bool JsonIntFromObjectOrParent(const nlohmann::json &src,
                               const char *object_key,
                               const char *key,
                               int32_t *out) {
  if (!out) {
    return false;
  }
  auto read_value = [key, out](const nlohmann::json &j) -> bool {
    if (!j.is_object() || !j.contains(key)) {
      return false;
    }
    const auto &item = j.at(key);
    if (item.is_number_integer()) {
      *out = item.get<int32_t>();
      return true;
    }
    if (item.is_number()) {
      *out = static_cast<int32_t>(item.get<double>());
      return true;
    }
    return false;
  };
  if (const nlohmann::json *obj = JsonObjectOrNull(src, object_key)) {
    if (read_value(*obj)) {
      return true;
    }
  }
  return read_value(src);
}

bool JsonNumberFromObjectOrParent(const nlohmann::json &src,
                                  const char *object_key,
                                  const char *key,
                                  double *out) {
  if (!out) {
    return false;
  }
  if (const nlohmann::json *obj = JsonObjectOrNull(src, object_key)) {
    if (JsonNumber(*obj, key, out)) {
      return true;
    }
  }
  return JsonNumber(src, key, out);
}

void AddTransformOp(Xformable &xformable, const value::matrix4d &matrix) {
  XformOp op;
  op.op_type = XformOp::OpType::Transform;
  op.set_value(matrix);
  xformable.xformOps.push_back(op);
}

// Jacobi eigenvalue decomposition of a symmetric 3x3 matrix. Outputs the
// eigenvalues `eval[i]` and their eigenvectors as the columns of `evec`
// (orthonormal). Used to diagonalize a full inertia tensor into principal
// moments (eigenvalues) + a principal-axes rotation (eigenvectors).
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
        // Apply the Givens rotation a := Jᵀ a J for indices (p, q).
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

// Convert a 3x3 rotation matrix (columns = orthonormal basis) into a quatf.
// Forces a right-handed (det +1) frame so the quaternion is well-formed.
value::quatf RotationMatrixToQuatf(double r[3][3]) {
  // Ensure proper rotation: if det < 0, flip the third column.
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
  value::quatf q;
  q.real = static_cast<float>(w);
  q.imag = value::float3{static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(z)};
  return q;
}

template <typename GeomT>
void AddCollisionAPIs(GeomT &geom, bool mesh_collision,
                      const nlohmann::json &src, bool mjcf_source) {
  // Schemas applied to every collider geom: UsdPhysics core, plus the
  // codeless MjcPhysics mirror (read by `prim-reconstruct-physics.cc`
  // and consumed by the lightgeom + mujoco-usd-converter pipelines).
  // `MjcImageableAPI` carries `mjc:group` for round-trip into MuJoCo.
  std::vector<std::pair<APISchemas::APIName, std::string>> apis{
      {APISchemas::APIName::PhysicsCollisionAPI, ""},
      {APISchemas::APIName::MjcCollisionAPI, ""},
      {APISchemas::APIName::MjcImageableAPI, ""},
      {APISchemas::APIName::NewtonCollisionAPI, ""},
  };
  if (mesh_collision) {
    apis.push_back({APISchemas::APIName::PhysicsMeshCollisionAPI, ""});
    apis.push_back({APISchemas::APIName::MjcMeshCollisionAPI, ""});
    apis.push_back({APISchemas::APIName::NewtonMeshCollisionAPI, ""});
  }
  AddAPISchemas(geom.metas(), apis);
  AddAttr(geom.props, "physics:collisionEnabled", true);
  if (mesh_collision) {
    // Default approximation is `convexHull` — matches NVIDIA / Newton's
    // mujoco-usd-converter (`_impl/geom.py:321`) and the convention
    // documented in lightgeom's `doc/usd.md`. The previous default
    // `none` left readers with arbitrary triangle-soup collision,
    // which neither MuJoCo nor PhysX/Newton handle natively without
    // an explicit decomposition pass.
    AddAttr(geom.props, "physics:approximation",
            value::token(JsonString(src, "approximation", "convexHull")), true);
    AddAttr(geom.props, "mjc:inertia", value::token("legacy"), true);
  }
  if (mjcf_source) {
    int32_t ivalue = 0;
    double dvalue = 0.0;
    if (JsonIntFromObjectOrParent(src, "mjc", "group", &ivalue)) {
      AddAttr(geom.props, "mjc:group", ivalue, true);
    } else if (src.contains("group")) {
      AddAttr(geom.props, "mjc:group",
              JsonInt(src, "group", kMjcfDefaultGroup), true);
    }
    if (JsonIntFromObjectOrParent(src, "mjc", "condim", &ivalue)) {
      AddAttr(geom.props, "mjc:condim", ivalue, true);
    } else if (src.contains("condim")) {
      AddAttr(geom.props, "mjc:condim",
              JsonInt(src, "condim", kMjcfDefaultCondim), true);
    }
    if (JsonIntFromObjectOrParent(src, "mjc", "geomContype", &ivalue) ||
        JsonIntFromObjectOrParent(src, "mjc", "contype", &ivalue)) {
      AddAttr(geom.props, "mjc:geomContype", ivalue, true);
    }
    if (JsonIntFromObjectOrParent(src, "mjc", "geomConaffinity", &ivalue) ||
        JsonIntFromObjectOrParent(src, "mjc", "conaffinity", &ivalue)) {
      AddAttr(geom.props, "mjc:geomConaffinity", ivalue, true);
    }
    if (JsonIntFromObjectOrParent(src, "mjc", "priority", &ivalue)) {
      AddAttr(geom.props, "mjc:priority", ivalue, true);
    }
    if (JsonNumberFromObjectOrParent(src, "mjc", "solmix", &dvalue)) {
      AddAttr(geom.props, "mjc:solmix", dvalue, true);
    }
    if (JsonNumberFromObjectOrParent(src, "mjc", "margin", &dvalue)) {
      AddAttr(geom.props, "mjc:margin", dvalue, true);
      AddAttr(geom.props, "newton:contactMargin", static_cast<float>(dvalue));
    } else if (JsonNumberFromObjectOrParent(src, "newton", "contactMargin",
                                            &dvalue)) {
      AddAttr(geom.props, "newton:contactMargin", static_cast<float>(dvalue));
    }
    if (JsonNumberFromObjectOrParent(src, "mjc", "gap", &dvalue)) {
      AddAttr(geom.props, "mjc:gap", dvalue, true);
      AddAttr(geom.props, "newton:contactGap", static_cast<float>(dvalue));
    } else if (JsonNumberFromObjectOrParent(src, "newton", "contactGap",
                                            &dvalue)) {
      AddAttr(geom.props, "newton:contactGap", static_cast<float>(dvalue));
    }
    std::vector<double> values =
        JsonDoubleArrayFromObjectOrParent(src, "mjc", "geomFriction");
    if (values.empty()) {
      values = JsonDoubleArrayFromObjectOrParent(src, "mjc", "friction");
    }
    if (!values.empty()) {
      AddAttr(geom.props, "mjc:geomFriction", values, true);
    }
    values = JsonDoubleArrayFromObjectOrParent(src, "mjc", "solref");
    if (!values.empty()) {
      AddAttr(geom.props, "mjc:solref", values, true);
    }
    values = JsonDoubleArrayFromObjectOrParent(src, "mjc", "solimp");
    if (!values.empty()) {
      AddAttr(geom.props, "mjc:solimp", values, true);
    }
    values = JsonDoubleArrayFromObjectOrParent(src, "mjc", "geomSize");
    if (!values.empty()) {
      AddAttr(geom.props, "mjc:geomSize", values, true);
    }
  } else {
    AddAttr(geom.props, "mjc:group",
            JsonInt(src, "group", kLegacyUrdfCollisionGroup), true);
    AddAttr(geom.props, "mjc:condim",
            JsonInt(src, "condim", kMjcfDefaultCondim), true);
    AddAttr(geom.props, "mjc:solmix",
            JsonNumberFromObjectOrParent(src, "mjc", "solmix",
                                         kMjcfDefaultSolmix), true);
    const double margin =
        JsonNumberFromObjectOrParent(src, "mjc", "margin",
                                     kMjcfDefaultMargin);
    AddAttr(geom.props, "mjc:margin", margin, true);
    AddAttr(geom.props, "newton:contactMargin",
            static_cast<float>(JsonNumberFromObjectOrParent(
                src, "newton", "contactMargin", margin)));
    AddAttr(geom.props, "newton:contactGap",
            static_cast<float>(JsonNumberFromObjectOrParent(
                src, "newton", "contactGap", kMjcfDefaultGap)));
  }
  if (mesh_collision) {
    AddAttr(geom.props, "newton:maxHullVertices",
            JsonIntFromObjectOrParent(src, "newton", "maxHullVertices",
                                      kDefaultNewtonMaxHullVertices),
            true);
  }

  // `purpose=guide` hides the collider from default Hydra renders but
  // keeps it discoverable to schema-aware consumers (lightgeom's
  // web/sim viewer, usdview's purpose toggle, etc.). Mirrors
  // mujoco-usd-converter/_impl/utils.py:20 which applies this when
  // MuJoCo's `geom.group ∉ {0,1,2}`; URDF <collision> elements are
  // conceptually equivalent to that group and get the same treatment.
  geom.purpose.set_value(Purpose::Guide);
}

template <typename GeomT>
bool AddGeomChild(Prim &link_prim, GeomT &&geom, const std::string &name,
                  std::string *err) {
  std::string add_err;
  if (!link_prim.add_child(Prim(std::forward<GeomT>(geom)), true, &add_err)) {
    if (err) {
      *err = "Failed to add collision shape `" + name + "`: " + add_err;
    }
    return false;
  }
  return true;
}

void AppendWarn(std::string *warn, const std::string &msg) {
  if (warn) {
    (*warn) += msg;
  }
}

void SetErr(std::string *err, const std::string &msg) {
  if (err) {
    (*err) = msg;
  }
}

std::string AxisToken(const nlohmann::json &joint_json) {
  const std::string authored = JsonString(joint_json, "axisToken");
  if (!authored.empty()) {
    return {authored};
  }
  const std::vector<float> axis = JsonFloatArray(joint_json, "axis");
  if (axis.size() < 3) {
    return "X";
  }
  const float ax = std::fabs(axis[0]);
  const float ay = std::fabs(axis[1]);
  const float az = std::fabs(axis[2]);
  if (ay >= ax && ay >= az) {
    return "Y";
  }
  if (az >= ax && az >= ay) {
    return "Z";
  }
  return "X";
}

std::string JointDofName(const std::string &axis, bool rotational) {
  const char c = axis.empty() ? 'X' : axis[0];
  return std::string(rotational ? "rot" : "trans") + c;
}

value::point3f LocalPos0FromJoint(const nlohmann::json &joint_json) {
  const std::vector<double> m = JsonDoubleArray(joint_json, "originMatrix");
  if (m.size() == 16) {
    return value::point3f{
        static_cast<float>(m[12]), static_cast<float>(m[13]),
        static_cast<float>(m[14])};
  }
  const std::vector<float> xyz = JsonFloatArray(joint_json, "origin");
  if (xyz.size() >= 3) {
    return value::point3f{xyz[0], xyz[1], xyz[2]};
  }
  return value::point3f{0.0f, 0.0f, 0.0f};
}

value::point3f Point3FromJoint(const nlohmann::json &joint_json,
                               const char *key,
                               const value::point3f &fallback) {
  const std::vector<float> values = JsonFloatArray(joint_json, key);
  if (values.size() >= 3) {
    return value::point3f{values[0], values[1], values[2]};
  }
  return fallback;
}

value::quatf QuatFromJoint(const nlohmann::json &joint_json, const char *key) {
  const std::vector<float> q = JsonFloatArray(joint_json, key);
  if (q.size() >= 4) {
    // JSON uses USD text order: real, imaginary x, y, z.
    return value::quatf{{q[1], q[2], q[3]}, q[0]};
  }
  return value::quatf{{0.0f, 0.0f, 0.0f}, 1.0f};
}

// The joint frame rotation is authored via the JSON `localRot0`/`localRot1`
// quaternions (see QuatFromJoint), which every producer in this repo supplies.
// `originMatrix` is consumed only for its translation (LocalPos0FromJoint); its
// rotation is intentionally NOT decoded into localRot0. Detect the case where a
// producer encoded a rotation only in `originMatrix` so we can warn instead of
// silently dropping it. Returns true if originMatrix has 16 elems and a
// non-identity upper-left 3x3.
bool JointOriginMatrixHasRotation(const nlohmann::json &joint_json) {
  const std::vector<double> m = JsonDoubleArray(joint_json, "originMatrix");
  if (m.size() != 16) return false;
  // Column-major (THREE/USD bridge): rotation basis is indices 0,1,2 / 4,5,6 /
  // 8,9,10. Compare against identity with a loose tolerance.
  const double expect[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  const size_t idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
  for (size_t i = 0; i < 9; i++) {
    if (std::abs(m[idx[i]] - expect[i]) > 1e-6) return true;
  }
  return false;
}

template <typename JointT>
void AssignJointBase(JointT &joint, const nlohmann::json &joint_json,
                     const std::string &parent_name,
                     const std::string &child_name,
                     const std::map<std::string, std::string> &joint_name_to_usd) {
  joint.body0.set(Path("/World/Links/" + parent_name, ""));
  joint.body1.set(Path("/World/Links/" + child_name, ""));
  const value::point3f local_pos0 = Point3FromJoint(
      joint_json, "localPos0", LocalPos0FromJoint(joint_json));
  joint.localPos0.set_value(local_pos0);
  joint.localPos1.set_value(Point3FromJoint(
      joint_json, "localPos1", value::point3f{0.0f, 0.0f, 0.0f}));
  joint.localRot0.set_value(QuatFromJoint(joint_json, "localRot0"));
  joint.localRot1.set_value(QuatFromJoint(joint_json, "localRot1"));
  joint.jointEnabled.set_value(true);
  joint.collisionEnabled.set_value(false);

  // Dispatch the per-axis (angular vs linear) PhysX limit namespace
  // based on the joint subtype the JSON declares. revolute -> angular,
  // prismatic -> linear; any other type still gets the canonical mjc:*
  // attributes but skips the physxLimit:* mirror because PhysX has no
  // direct analog. Matches the SchemaResolverPhysx mapping in
  // ref/newton/newton/_src/usd/schemas.py.
  const std::string joint_subtype = JsonString(joint_json, "type");
  const bool is_revolute = (joint_subtype == "revolute");
  const bool is_prismatic = (joint_subtype == "prismatic");
  const char *limit_ns = is_prismatic ? "physxLimit:linear:"
                                       : "physxLimit:angular:";

  MjcJointAPI mjc;
  if (joint_json.contains("dynamics") && joint_json["dynamics"].is_object()) {
    const auto &dyn = joint_json["dynamics"];
    double damping = 0.0;
    if (JsonNumber(dyn, "damping", &damping)) {
      mjc.damping.set_value(damping);
      AddAttr(joint.props, "mjc:damping", damping);
      if (is_revolute || is_prismatic) {
        AddAttr(joint.props, std::string(limit_ns) + "damping", damping);
      }
    }
    double friction = 0.0;
    if (JsonNumber(dyn, "friction", &friction)) {
      mjc.frictionloss.set_value(friction);
      AddAttr(joint.props, "mjc:frictionloss", friction);
      AddAttr(joint.props, "physxJoint:jointFriction", friction);
    }
    double stiffness = 0.0;
    if (JsonNumber(dyn, "stiffness", &stiffness)) {
      // mjcJoint's stiffness lives on the MjcJointAPI extension —
      // expose both forms so MuJoCo + PhysX consumers see it.
      AddAttr(joint.props, "mjc:stiffness", stiffness);
      if (is_revolute || is_prismatic) {
        AddAttr(joint.props, std::string(limit_ns) + "stiffness", stiffness);
      }
    }
    double armature = 0.0;
    if (JsonNumber(dyn, "armature", &armature)) {
      AddAttr(joint.props, "mjc:armature", armature);
      AddAttr(joint.props, "physxJoint:armature", armature);
    }
  }
  // Initial joint configuration. MJCF and URDF both surface this via
  // <joint range>/<joint pos> respectively; we promote it into the
  // Newton state:*:physics:position schema, which Genesis/Isaac/Newton
  // all read for joint qpos initialization. Revolute joints carry the
  // value in *degrees* per the PhysX/Newton convention; prismatic in
  // *meters*.
  double init_q = 0.0;
  if (JsonNumber(joint_json, "initPosition", &init_q) && init_q != 0.0) {
    if (is_revolute) {
      AddAttr(joint.props, "state:angular:physics:position",
              init_q * 180.0 / 3.14159265358979323846);
    } else if (is_prismatic) {
      AddAttr(joint.props, "state:linear:physics:position", init_q);
    }
  }
  joint.mjcJoint = mjc;
  std::vector<std::pair<APISchemas::APIName, std::string>> schemas{
      {APISchemas::APIName::MjcJointAPI, ""}};
  if (const nlohmann::json *mimic =
          JsonObjectOrNull(joint_json, "mimic")) {
    const std::string mimic_joint = JsonString(*mimic, "joint");
    auto mimic_it = joint_name_to_usd.find(mimic_joint);
    if (mimic_it != joint_name_to_usd.end()) {
      NewtonMimicAPI newton_mimic;
      bool enabled = true;
      JsonBool(*mimic, "enabled", &enabled);
      newton_mimic.mimicEnabled.set_value(enabled);
      newton_mimic.mimicJoint.set(Path("/World/Joints/" + mimic_it->second, ""));
      float offset = 0.0f;
      float multiplier = 1.0f;
      JsonNumber(*mimic, "offset", &offset);
      JsonNumber(*mimic, "multiplier", &multiplier);
      newton_mimic.mimicCoef0.set_value(offset);
      newton_mimic.mimicCoef1.set_value(multiplier);
      joint.newtonMimic = newton_mimic;
      schemas.push_back({APISchemas::APIName::NewtonMimicAPI, ""});
    }
  }
  AddAPISchemas(joint.metas(), schemas);
}

void AddNewtonSceneOptions(const nlohmann::json &root, PhysicsScene &scene) {
  NewtonSceneAPI newton_scene;

  double timestep = 0.002;
  JsonNumber(root, "timestep", &timestep);
  int32_t steps_per_second =
      (timestep > 0.0) ? static_cast<int32_t>(std::lround(1.0 / timestep))
                       : 500;
  bool gravity_enabled = true;
  int32_t max_solver_iterations = -1;
  if (const nlohmann::json *newton = JsonObjectOrNull(root, "newton")) {
    steps_per_second =
        JsonInt(*newton, "timeStepsPerSecond", steps_per_second);
    max_solver_iterations =
        JsonInt(*newton, "maxSolverIterations", max_solver_iterations);
    JsonBool(*newton, "gravityEnabled", &gravity_enabled);
  }
  newton_scene.timeStepsPerSecond.set_value(steps_per_second);
  newton_scene.maxSolverIterations.set_value(max_solver_iterations);
  newton_scene.gravityEnabled.set_value(gravity_enabled);
  scene.newtonScene = newton_scene;
}

// Populate the full MjcSceneAPI from a payload `mjcScene` block carrying the
// MuJoCo <option>/<option><flag>/<compiler> attributes (MJCF attr names). The
// schema (src/mjcPhysics.hh) and its USDA/USDC round-trip already support all
// of these; the parsers previously emitted only timestep.
void ApplyMjcSceneOptions(const nlohmann::json &root, MjcSceneAPI &mjc) {
  const nlohmann::json *ms = JsonObjectOrNull(root, "mjcScene");
  if (!ms) return;

  auto setNum = [](const nlohmann::json &o, const char *k, auto &field) {
    double v;
    if (JsonNumber(o, k, &v)) field.set_value(v);
  };
  auto setInt = [](const nlohmann::json &o, const char *k, auto &field) {
    int v;
    if (JsonIntFromObjectOrParent(o, "", k, &v)) field.set_value(v);
  };
  auto setTok = [](const nlohmann::json &o, const char *k, auto &field) {
    const std::string v = JsonString(o, k);
    if (!v.empty()) field.set_value(value::token(v));
  };
  auto setBool = [](const nlohmann::json &o, const char *k, auto &field) {
    bool v;
    if (JsonBool(o, k, &v)) field.set_value(v);
  };
  auto setVec3 = [](const nlohmann::json &o, const char *k, auto &field) {
    const auto a = JsonDoubleArray(o, k);
    if (a.size() >= 3) field.set_value(value::double3{a[0], a[1], a[2]});
  };

  if (const nlohmann::json *o = JsonObjectOrNull(*ms, "option")) {
    setNum(*o, "timestep", mjc.timestep);
    setNum(*o, "impratio", mjc.impratio);
    setNum(*o, "density", mjc.density);
    setNum(*o, "viscosity", mjc.viscosity);
    setNum(*o, "o_margin", mjc.o_margin);
    setNum(*o, "tolerance", mjc.tolerance);
    setNum(*o, "ls_tolerance", mjc.ls_tolerance);
    setNum(*o, "noslip_tolerance", mjc.noslip_tolerance);
    setNum(*o, "ccd_tolerance", mjc.ccd_tolerance);
    setInt(*o, "iterations", mjc.iterations);
    setInt(*o, "ls_iterations", mjc.ls_iterations);
    setInt(*o, "noslip_iterations", mjc.noslip_iterations);
    setInt(*o, "ccd_iterations", mjc.ccd_iterations);
    setInt(*o, "sdf_iterations", mjc.sdf_iterations);
    setInt(*o, "sdf_initpoints", mjc.sdf_initpoints);
    setTok(*o, "integrator", mjc.integrator);
    setTok(*o, "cone", mjc.cone);
    setTok(*o, "jacobian", mjc.jacobian);
    setTok(*o, "solver", mjc.solver);
    setVec3(*o, "wind", mjc.wind);
    setVec3(*o, "magnetic", mjc.magnetic);
    const auto sr = JsonDoubleArray(*o, "o_solref");
    if (!sr.empty()) mjc.o_solref.set_value(sr);
    const auto si = JsonDoubleArray(*o, "o_solimp");
    if (!si.empty()) mjc.o_solimp.set_value(si);
    const auto fr = JsonDoubleArray(*o, "o_friction");
    if (!fr.empty()) mjc.o_friction.set_value(fr);
  }

  if (const nlohmann::json *f = JsonObjectOrNull(*ms, "flag")) {
    setBool(*f, "constraint", mjc.flag_constraint);
    setBool(*f, "equality", mjc.flag_equality);
    setBool(*f, "frictionloss", mjc.flag_frictionloss);
    setBool(*f, "limit", mjc.flag_limit);
    setBool(*f, "contact", mjc.flag_contact);
    setBool(*f, "gravity", mjc.flag_gravity);
    setBool(*f, "clampctrl", mjc.flag_clampctrl);
    setBool(*f, "warmstart", mjc.flag_warmstart);
    setBool(*f, "filterparent", mjc.flag_filterparent);
    setBool(*f, "actuation", mjc.flag_actuation);
    setBool(*f, "refsafe", mjc.flag_refsafe);
    setBool(*f, "sensor", mjc.flag_sensor);
    setBool(*f, "midphase", mjc.flag_midphase);
    setBool(*f, "nativeccd", mjc.flag_nativeccd);
    setBool(*f, "eulerdamp", mjc.flag_eulerdamp);
    setBool(*f, "autoreset", mjc.flag_autoreset);
    setBool(*f, "island", mjc.flag_island);
    setBool(*f, "override", mjc.flag_override);
    setBool(*f, "energy", mjc.flag_energy);
    setBool(*f, "fwdinv", mjc.flag_fwdinv);
    setBool(*f, "invdiscrete", mjc.flag_invdiscrete);
    setBool(*f, "multiccd", mjc.flag_multiccd);
  }

  if (const nlohmann::json *c = JsonObjectOrNull(*ms, "compiler")) {
    setBool(*c, "autolimits", mjc.compiler_autoLimits);
    setNum(*c, "boundmass", mjc.compiler_boundMass);
    setNum(*c, "boundinertia", mjc.compiler_boundInertia);
    setNum(*c, "settotalmass", mjc.compiler_setTotalMass);
    setBool(*c, "usethread", mjc.compiler_useThread);
    setBool(*c, "balanceinertia", mjc.compiler_balanceInertia);
    setTok(*c, "angle", mjc.compiler_angle);
    setBool(*c, "fitaabb", mjc.compiler_fitAABB);
    setBool(*c, "fusestatic", mjc.compiler_fuseStatic);
    setTok(*c, "inertiafromgeom", mjc.compiler_inertiaFromGeom);
    setBool(*c, "alignfree", mjc.compiler_alignFree);
    setInt(*c, "inertiagrouprange_min", mjc.compiler_inertiaGroupRangeMin);
    setInt(*c, "inertiagrouprange_max", mjc.compiler_inertiaGroupRangeMax);
    setBool(*c, "saveinertial", mjc.compiler_saveInertial);
  }
}

std::vector<std::pair<APISchemas::APIName, std::string>>
NewtonActuatorSchemas(const nlohmann::json &act_json) {
  std::vector<std::pair<APISchemas::APIName, std::string>> schemas;
  const std::string control = JsonString(act_json, "control", "pd");
  if (control == "pid" || act_json.contains("ki") ||
      act_json.contains("integralMax")) {
    schemas.push_back({APISchemas::APIName::NewtonPIDControlAPI, ""});
  } else {
    schemas.push_back({APISchemas::APIName::NewtonPDControlAPI, ""});
  }
  if (act_json.contains("delaySteps")) {
    schemas.push_back({APISchemas::APIName::NewtonActuatorDelayAPI, ""});
  }
  if (act_json.contains("maxEffort")) {
    schemas.push_back({APISchemas::APIName::NewtonMaxEffortClampingAPI, ""});
  }
  if (act_json.contains("maxMotorEffort") ||
      act_json.contains("saturationEffort") ||
      act_json.contains("velocityLimit")) {
    schemas.push_back({APISchemas::APIName::NewtonDCMotorClampingAPI, ""});
  }
  if (act_json.contains("lookupPositions") || act_json.contains("lookupEfforts")) {
    schemas.push_back({APISchemas::APIName::NewtonPositionBasedClampingAPI, ""});
  }
  return schemas;
}

bool AddNewtonActuatorFromJson(
    Prim &actuators_prim, const nlohmann::json &act_json,
    const std::map<std::string, std::string> &joint_name_to_usd,
    size_t index, std::string *warn, std::string *err) {
  NewtonActuator act;
  act.name = SanitizeUSDIdentifier(
      JsonString(act_json, "name", "actuator_" + std::to_string(index)),
      "actuator");
  AddAPISchemas(act.metas(), NewtonActuatorSchemas(act_json));

  std::vector<Path> targets;
  const std::string joint_name = JsonString(act_json, "joint");
  if (!joint_name.empty()) {
    auto it = joint_name_to_usd.find(joint_name);
    if (it != joint_name_to_usd.end()) {
      targets.emplace_back("/World/Joints/" + it->second, "");
    }
  }
  if (act_json.contains("targets") && act_json["targets"].is_array()) {
    for (const auto &target : act_json["targets"]) {
      if (!target.is_string()) {
        continue;
      }
      const std::string name_or_path = target.get<std::string>();
      if (!name_or_path.empty() && name_or_path[0] == '/') {
        targets.emplace_back(name_or_path, "");
      } else {
        auto it = joint_name_to_usd.find(name_or_path);
        if (it != joint_name_to_usd.end()) {
          targets.emplace_back("/World/Joints/" + it->second, "");
        }
      }
    }
  }
  if (targets.empty()) {
    AppendWarn(warn, "Skipping Newton actuator `" + act.name +
                         "`: no target joint was exported.\n");
    return true;
  }
  act.targets.set(std::move(targets));

  if (act_json.contains("delaySteps")) {
    act.delaySteps.set_value(JsonInt(act_json, "delaySteps", 1));
  }
  float v = 0.0f;
  if (JsonNumber(act_json, "constEffort", &v)) act.constEffort.set_value(v);
  if (JsonNumber(act_json, "kp", &v)) act.kp.set_value(v);
  if (JsonNumber(act_json, "kd", &v)) act.kd.set_value(v);
  if (JsonNumber(act_json, "ki", &v)) act.ki.set_value(v);
  if (JsonNumber(act_json, "integralMax", &v)) act.integralMax.set_value(v);
  if (JsonNumber(act_json, "maxEffort", &v)) act.maxEffort.set_value(v);
  if (JsonNumber(act_json, "maxMotorEffort", &v)) {
    act.maxMotorEffort.set_value(v);
  }
  if (JsonNumber(act_json, "saturationEffort", &v)) {
    act.saturationEffort.set_value(v);
  }
  if (JsonNumber(act_json, "velocityLimit", &v)) {
    act.velocityLimit.set_value(v);
  }
  std::vector<float> lookup_positions =
      JsonFloatArray(act_json, "lookupPositions");
  if (!lookup_positions.empty()) {
    act.lookupPositions.set_value(std::move(lookup_positions));
  }
  std::vector<float> lookup_efforts = JsonFloatArray(act_json, "lookupEfforts");
  if (!lookup_efforts.empty()) {
    act.lookupEfforts.set_value(std::move(lookup_efforts));
  }

  std::string add_err;
  if (!actuators_prim.add_child(Prim(act), true, &add_err)) {
    SetErr(err, "Failed to add Newton actuator `" + act.name +
                    "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <tendon> -> MjcTendon prim. The legacy MJCF JSON shape uses
// `joints:[{joint,coef}]` for fixed tendons and `path:[{site|sidesite}]` for
// spatial tendons. The richer tydra JSON shape emitted by physics-to-json uses
// absolute or source-name string arrays (`route`, `sideSites`) plus the typed
// route metadata arrays.
bool AddMjcTendonFromJson(
    Prim &tendons_prim, const nlohmann::json &t_json,
    const std::map<std::string, std::string> &joint_name_to_usd,
    const std::map<std::string, std::string> &site_name_to_usd,
    std::map<std::string, std::string> &tendon_name_to_usd,
    size_t index, std::string *warn, std::string *err) {
  MjcTendon tendon;
  std::string source_name = JsonString(t_json, "name");
  if (source_name.empty()) {
    const std::string prim_path = JsonString(t_json, "path");
    const size_t slash = prim_path.find_last_of('/');
    if (!prim_path.empty() && slash != std::string::npos &&
        slash + 1 < prim_path.size()) {
      source_name = prim_path.substr(slash + 1);
    }
  }
  if (source_name.empty()) {
    source_name = "tendon_" + std::to_string(index);
  }
  tendon.name = SanitizeUSDIdentifier(source_name, "tendon");
  tendon_name_to_usd[source_name] = tendon.name;
  const std::string type = JsonString(t_json, "type", "fixed");
  tendon.type.set_value(value::token(type));

  auto resolve_route_ref = [&](const std::string &ref) -> nonstd::optional<Path> {
    if (ref.empty()) {
      return nonstd::nullopt;
    }
    if (ref[0] == '/') {
      return Path(ref, "");
    }
    if (type == "spatial") {
      auto it = site_name_to_usd.find(ref);
      if (it != site_name_to_usd.end()) {
        return Path("/World/Sites/" + it->second, "");
      }
    } else {
      auto it = joint_name_to_usd.find(ref);
      if (it != joint_name_to_usd.end()) {
        return Path("/World/Joints/" + it->second, "");
      }
    }
    return nonstd::nullopt;
  };

  auto resolve_site_ref = [&](const std::string &ref) -> nonstd::optional<Path> {
    if (ref.empty()) {
      return nonstd::nullopt;
    }
    if (ref[0] == '/') {
      return Path(ref, "");
    }
    auto it = site_name_to_usd.find(ref);
    if (it != site_name_to_usd.end()) {
      return Path("/World/Sites/" + it->second, "");
    }
    return nonstd::nullopt;
  };

  auto append_route_ref = [&](const std::string &ref,
                              std::vector<Path> *out) -> bool {
    auto resolved = resolve_route_ref(ref);
    if (resolved.has_value()) {
      out->push_back(resolved.value());
      return true;
    }
    if (!ref.empty()) {
      AppendWarn(warn, "Tendon `" + tendon.name +
                           "` route reference `" + ref +
                           "` could not be resolved; skipping it.\n");
    }
    return false;
  };

  auto append_route_object = [&](const nlohmann::json &item,
                                 std::vector<Path> *out) -> bool {
    if (!item.is_object()) {
      return false;
    }
    std::string ref = JsonString(item, "target");
    if (ref.empty()) ref = JsonString(item, "path");
    if (ref.empty() && type == "spatial") ref = JsonString(item, "site");
    if (ref.empty() && type == "spatial") ref = JsonString(item, "sidesite");
    if (ref.empty()) ref = JsonString(item, "joint");
    return append_route_ref(ref, out);
  };

  std::vector<Path> targets;
  std::vector<double> coefs = JsonDoubleArray(t_json, "routeCoef");
  const std::vector<std::string> route = JsonStringArray(t_json, "route");
  if (!route.empty()) {
    for (const std::string &ref : route) {
      append_route_ref(ref, &targets);
    }
  } else if (t_json.contains("route") && t_json["route"].is_array()) {
    for (const auto &item : t_json["route"]) {
      append_route_object(item, &targets);
    }
  } else if (t_json.contains("path") && t_json["path"].is_array() &&
             !JsonStringArray(t_json, "path").empty()) {
    for (const std::string &ref : JsonStringArray(t_json, "path")) {
      append_route_ref(ref, &targets);
    }
  } else if (type == "spatial") {
    // Spatial (muscle) tendon: ordered <site>/<geom sidesite=..> waypoints ->
    // mjc:path rel to the routing site prims (wrap geoms approximated by their
    // sidesite via point). mjc:path:coef carries per-waypoint site coefficients;
    // pulley divisors are preserved separately as mjc:path:divisors.
    if (t_json.contains("path") && t_json["path"].is_array()) {
      for (const auto &wp : t_json["path"]) {
        if (!wp.is_object()) continue;
        std::string site_ref;
        if (wp.contains("site")) site_ref = JsonString(wp, "site");
        else if (wp.contains("sidesite")) site_ref = JsonString(wp, "sidesite");
        if (site_ref.empty()) {
          append_route_object(wp, &targets);
          continue;  // pulley / unresolvable geom wrap
        }
        append_route_ref(site_ref, &targets);
        coefs.push_back(1.0);
      }
    }
  } else {
    // Fixed tendon: linear combination of joint coordinates.
    if (t_json.contains("joints") && t_json["joints"].is_array()) {
      for (const auto &je : t_json["joints"]) {
        if (!je.is_object()) continue;
        const std::string jname = JsonString(je, "joint");
        auto it = joint_name_to_usd.find(jname);
        if (it == joint_name_to_usd.end()) {
          AppendWarn(warn, "Tendon `" + tendon.name + "` references joint `" +
                               jname + "` that was not exported; skipping it.\n");
          continue;
        }
        targets.emplace_back("/World/Joints/" + it->second, "");
        double coef = 1.0;
        JsonNumber(je, "coef", &coef);
        coefs.push_back(coef);
      }
    }
  }

  if (type == "spatial") {
    if (targets.empty()) {
      AppendWarn(warn, "Skipping spatial tendon `" + tendon.name +
                           "`: no routing sites were exported.\n");
      return true;
    } else if (targets.size() < 2) {
      AppendWarn(warn, "Spatial tendon `" + tendon.name +
                           "` has fewer than 2 routing sites; preserving the "
                           "partial route.\n");
    }
  } else {
    if (targets.empty()) {
      AppendWarn(warn, "Skipping tendon `" + tendon.name +
                           "`: no referenced joint was exported.\n");
      return true;
    }
  }

  tendon.path.set(std::move(targets));
  if (!coefs.empty()) {
    tendon.path_coef.set_value(coefs);
  }

  std::vector<Path> side_sites;
  for (const std::string &ref : JsonStringArray(t_json, "sideSites")) {
    auto resolved = resolve_site_ref(ref);
    if (resolved.has_value()) {
      side_sites.push_back(resolved.value());
    } else {
      AppendWarn(warn, "Tendon `" + tendon.name +
                           "` sideSite reference `" + ref +
                           "` could not be resolved; skipping it.\n");
    }
  }
  if (side_sites.empty() && t_json.contains("sideSites") &&
      t_json["sideSites"].is_array()) {
    for (const auto &item : t_json["sideSites"]) {
      if (!item.is_object()) continue;
      std::string ref = JsonString(item, "target");
      if (ref.empty()) ref = JsonString(item, "path");
      if (ref.empty()) ref = JsonString(item, "site");
      if (ref.empty()) ref = JsonString(item, "sidesite");
      auto resolved = resolve_site_ref(ref);
      if (resolved.has_value()) {
        side_sites.push_back(resolved.value());
      } else if (!ref.empty()) {
        AppendWarn(warn, "Tendon `" + tendon.name +
                             "` sideSite reference `" + ref +
                             "` could not be resolved; skipping it.\n");
      }
    }
  }
  if (!side_sites.empty()) {
    tendon.sideSites.set(std::move(side_sites));
  }

  std::vector<int32_t> int_values = JsonIntArray(t_json, "routeIndices");
  if (!int_values.empty()) tendon.path_indices.set_value(int_values);
  int_values = JsonIntArray(t_json, "sideSiteIndices");
  if (!int_values.empty()) tendon.sideSites_indices.set_value(int_values);
  int_values = JsonIntArray(t_json, "routeSegments");
  if (!int_values.empty()) tendon.path_segments.set_value(int_values);

  std::vector<double> values = JsonDoubleArray(t_json, "routeDivisors");
  if (!values.empty()) tendon.path_divisors.set_value(values);
  values = JsonDoubleArray(t_json, "routeCoef");
  if (!values.empty()) tendon.path_coef.set_value(values);

  const auto rgba = JsonDoubleArray(t_json, "rgba");
  if (rgba.size() >= 4) {
    tendon.rgba.set_value(value::color4f{
        static_cast<float>(rgba[0]), static_cast<float>(rgba[1]),
        static_cast<float>(rgba[2]), static_cast<float>(rgba[3])});
  }

  double v = 0.0;
  int32_t group = 0;
  if (t_json.contains("group")) {
    group = JsonInt(t_json, "group", 0);
    tendon.group.set_value(group);
  }
  if (JsonNumber(t_json, "stiffness", &v)) tendon.stiffness.set_value(v);
  if (JsonNumber(t_json, "damping", &v)) tendon.damping.set_value(v);
  if (JsonNumber(t_json, "frictionloss", &v)) tendon.frictionloss.set_value(v);
  if (JsonNumber(t_json, "margin", &v)) tendon.margin.set_value(v);
  if (JsonNumber(t_json, "armature", &v)) tendon.armature.set_value(v);
  if (JsonNumber(t_json, "width", &v)) tendon.width.set_value(v);
  if (t_json.contains("range") && t_json["range"].is_array() &&
      t_json["range"].size() >= 2) {
    tendon.range_min.set_value(t_json["range"][0].get<double>());
    tendon.range_max.set_value(t_json["range"][1].get<double>());
    tendon.limited.set_value(value::token("true"));
  }
  if (JsonNumber(t_json, "range_min", &v)) tendon.range_min.set_value(v);
  if (JsonNumber(t_json, "range_max", &v)) tendon.range_max.set_value(v);
  if (t_json.contains("actuatorfrcrange") &&
      t_json["actuatorfrcrange"].is_array() &&
      t_json["actuatorfrcrange"].size() >= 2) {
    tendon.actuatorfrcrange_min.set_value(
        t_json["actuatorfrcrange"][0].get<double>());
    tendon.actuatorfrcrange_max.set_value(
        t_json["actuatorfrcrange"][1].get<double>());
  }
  if (JsonNumber(t_json, "actuatorfrcrange_min", &v)) {
    tendon.actuatorfrcrange_min.set_value(v);
  }
  if (JsonNumber(t_json, "actuatorfrcrange_max", &v)) {
    tendon.actuatorfrcrange_max.set_value(v);
  }
  if (t_json.contains("limited")) {
    tendon.limited.set_value(value::token(JsonString(t_json, "limited", "auto")));
  }
  if (t_json.contains("actuatorfrclimited")) {
    tendon.actuatorfrclimited.set_value(
        value::token(JsonString(t_json, "actuatorfrclimited", "auto")));
  }
  values = JsonDoubleArray(t_json, "springlength");
  if (!values.empty()) tendon.springlength.set_value(values);
  values = JsonDoubleArray(t_json, "solreflimit");
  if (!values.empty()) tendon.solreflimit.set_value(values);
  values = JsonDoubleArray(t_json, "solimplimit");
  if (!values.empty()) tendon.solimplimit.set_value(values);
  values = JsonDoubleArray(t_json, "solreffriction");
  if (!values.empty()) tendon.solreffriction.set_value(values);
  values = JsonDoubleArray(t_json, "solimpfriction");
  if (!values.empty()) tendon.solimpfriction.set_value(values);

  std::string add_err;
  if (!tendons_prim.add_child(Prim(tendon), true, &add_err)) {
    SetErr(err, "Failed to add tendon `" + tendon.name + "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <site> -> a small GeomSphere marker under /World/Sites carrying
// MjcSiteAPI and a baked world transform (sites are the routing points for
// spatial/muscle tendons). Records source-name -> USD-name in site_name_to_usd
// so tendons can resolve their path. JSON: { name, matrix:[16], group, size }.
bool AddMjcSiteFromJson(Prim &sites_prim, const nlohmann::json &s_json,
                        std::set<std::string> &used_names,
                        std::map<std::string, std::string> &site_name_to_usd,
                        size_t index, std::string *err) {
  const std::string source_name =
      JsonString(s_json, "name", "site_" + std::to_string(index));
  const std::string usd_name =
      UniqueUSDIdentifier(source_name, used_names, "site");
  site_name_to_usd[source_name] = usd_name;

  GeomSphere site;
  site.name = usd_name;
  double radius = 0.005;
  JsonNumber(s_json, "size", &radius);
  if (radius <= 0.0) radius = 0.005;
  site.radius.set_value(radius);
  const std::vector<double> matrix = JsonDoubleArray(s_json, "matrix");
  if (matrix.size() == 16) {
    AddTransformOp(site, MatrixFromUSDArray(matrix));
  }
  AppendAPISchema(site.metas(), APISchemas::APIName::MjcSiteAPI);
  int group = 0;
  if (JsonIntFromObjectOrParent(s_json, "", "group", &group)) {
    AddAttr(site.props, "mjc:group", group, /*uniform=*/true);
  }
  // Sites are markers, not colliders / visuals by default (MuJoCo group>=3).
  site.purpose.set_value(Purpose::Guide);

  std::string add_err;
  if (!sites_prim.add_child(Prim(site), true, &add_err)) {
    SetErr(err, "Failed to add site `" + usd_name + "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <equality> -> an Xform host prim carrying the matching MjcEquality*API
// applied schema plus mjc:* attrs in the generic props map (these round-trip
// through the generic props pass, mirroring the physx:* preservation contract).
// connect/weld relate two bodies; joint relates two joints. JSON shape:
//   { "name","type":"connect|weld|joint",
//     "body1","body2","anchor":[x,y,z],            (connect/weld)
//     "joint1","joint2","polycoef":[c0..c4],        (joint)
//     "torquescale",                                 (weld)
//     "solref":[...],"solimp":[...] }
bool AddMjcEqualityFromJson(
    Prim &equalities_prim, const nlohmann::json &e_json,
    const std::map<std::string, std::string> &link_name_to_usd,
    const std::map<std::string, std::string> &joint_name_to_usd,
    size_t index, std::string *warn, std::string *err) {
  const std::string type = JsonString(e_json, "type", "connect");
  Xform host;
  host.name = SanitizeUSDIdentifier(
      JsonString(e_json, "name", "equality_" + std::to_string(index)),
      "equality");

  APISchemas::APIName api_name = APISchemas::APIName::MjcEqualityConnectAPI;
  if (type == "weld") api_name = APISchemas::APIName::MjcEqualityWeldAPI;
  else if (type == "joint") api_name = APISchemas::APIName::MjcEqualityJointAPI;
  AppendAPISchema(host.metas(), api_name);

  // Resolve the two related entities into rel targets so the constraint topology
  // survives. connect/weld -> bodies (links); joint -> joints.
  auto resolve_link = [&](const std::string &n, std::vector<Path> *out) {
    auto it = link_name_to_usd.find(n);
    if (it != link_name_to_usd.end())
      out->emplace_back("/World/Links/" + it->second, "");
  };
  auto resolve_joint = [&](const std::string &n, std::vector<Path> *out) {
    auto it = joint_name_to_usd.find(n);
    if (it != joint_name_to_usd.end())
      out->emplace_back("/World/Joints/" + it->second, "");
  };

  std::vector<Path> targets;
  if (type == "joint") {
    resolve_joint(JsonString(e_json, "joint1"), &targets);
    resolve_joint(JsonString(e_json, "joint2"), &targets);
  } else {
    resolve_link(JsonString(e_json, "body1"), &targets);
    resolve_link(JsonString(e_json, "body2"), &targets);
  }
  if (!targets.empty()) {
    Relationship rel;
    rel.set(targets);
    host.props["mjc:target"] = Property(std::move(rel), false);
  }

  // Type-specific scalar/array attrs as generic mjc:* props.
  double v = 0.0;
  if (type == "weld" && JsonNumber(e_json, "torquescale", &v)) {
    AddAttr(host.props, "mjc:torqueScale", static_cast<float>(v));
  }
  if (type == "joint" && e_json.contains("polycoef") &&
      e_json["polycoef"].is_array()) {
    const auto &pc = e_json["polycoef"];
    const char *coef_names[5] = {"mjc:coef0", "mjc:coef1", "mjc:coef2",
                                 "mjc:coef3", "mjc:coef4"};
    for (size_t i = 0; i < pc.size() && i < 5; i++) {
      if (pc[i].is_number()) AddAttr(host.props, coef_names[i], pc[i].get<double>());
    }
  }
  std::vector<double> solref = JsonDoubleArray(e_json, "solref");
  if (!solref.empty()) AddAttr(host.props, "mjc:solref", solref);
  std::vector<double> solimp = JsonDoubleArray(e_json, "solimp");
  if (!solimp.empty()) AddAttr(host.props, "mjc:solimp", solimp);
  std::vector<double> anchor = JsonDoubleArray(e_json, "anchor");
  if (!anchor.empty()) AddAttr(host.props, "mjc:anchor", anchor);

  std::string add_err;
  if (!equalities_prim.add_child(Prim(host), true, &add_err)) {
    SetErr(err, "Failed to add equality `" + host.name + "`: " + add_err);
    return false;
  }
  (void)warn;
  return true;
}

// MJCF <general>/<muscle> (and tendon/site-targeted) actuators -> MjcActuator
// prim, preserving the MuJoCo gain/bias/lengthrange parameters. The mjc:target
// rel resolves to the tendon (muscles), joint, or site the actuator drives.
// JSON: { name, actuatorType, targetTendon|targetJoint|targetSite,
//         gainPrm, biasPrm, lengthRange, ctrlRange, forceRange, gear }.
bool AddMjcActuatorFromJson(
    Prim &actuators_prim, const nlohmann::json &a_json,
    const std::map<std::string, std::string> &joint_name_to_usd,
    const std::map<std::string, std::string> &tendon_name_to_usd,
    const std::map<std::string, std::string> &site_name_to_usd,
    const std::map<std::string, std::string> &link_name_to_usd,
    std::set<std::string> &used_names, size_t index, std::string *warn,
    std::string *err) {
  MjcActuator act;
  act.name = UniqueUSDIdentifier(
      JsonString(a_json, "name", "actuator_" + std::to_string(index)),
      used_names, "actuator");

  // Resolve the driven entity into mjc:target.
  std::vector<Path> targets;
  const std::string t_tendon = JsonString(a_json, "targetTendon");
  const std::string t_joint = JsonString(a_json, "targetJoint");
  const std::string t_site = JsonString(a_json, "targetSite");
  const std::string t_body = JsonString(a_json, "targetBody");
  const std::string t_path = JsonString(a_json, "target");
  if (!t_path.empty() && t_path[0] == '/') {
    targets.emplace_back(t_path, "");
  } else if (!t_tendon.empty()) {
    auto it = tendon_name_to_usd.find(t_tendon);
    if (it != tendon_name_to_usd.end())
      targets.emplace_back("/World/Tendons/" + it->second, "");
  } else if (!t_joint.empty()) {
    auto it = joint_name_to_usd.find(t_joint);
    if (it != joint_name_to_usd.end())
      targets.emplace_back("/World/Joints/" + it->second, "");
  } else if (!t_site.empty()) {
    auto it = site_name_to_usd.find(t_site);
    if (it != site_name_to_usd.end())
      targets.emplace_back("/World/Sites/" + it->second, "");
  } else if (!t_body.empty()) {  // <adhesion body=..>
    auto it = link_name_to_usd.find(t_body);
    if (it != link_name_to_usd.end())
      targets.emplace_back("/World/Links/" + it->second, "");
  }
  if (!targets.empty()) act.target.set(std::move(targets));

  std::vector<double> gainPrm = JsonDoubleArray(a_json, "gainPrm");
  if (!gainPrm.empty()) act.gainPrm.set_value(gainPrm);
  std::vector<double> biasPrm = JsonDoubleArray(a_json, "biasPrm");
  if (!biasPrm.empty()) act.biasPrm.set_value(biasPrm);
  std::vector<double> dynPrm = JsonDoubleArray(a_json, "dynPrm");
  if (!dynPrm.empty()) act.dynPrm.set_value(dynPrm);
  std::vector<double> gear = JsonDoubleArray(a_json, "gear");
  if (!gear.empty()) act.gear.set_value(gear);
  if (a_json.contains("lengthRange") && a_json["lengthRange"].is_array() &&
      a_json["lengthRange"].size() >= 2) {
    act.lengthRange_min.set_value(a_json["lengthRange"][0].get<double>());
    act.lengthRange_max.set_value(a_json["lengthRange"][1].get<double>());
  }
  if (a_json.contains("ctrlRange") && a_json["ctrlRange"].is_array() &&
      a_json["ctrlRange"].size() >= 2) {
    act.ctrlRange_min.set_value(a_json["ctrlRange"][0].get<double>());
    act.ctrlRange_max.set_value(a_json["ctrlRange"][1].get<double>());
  }
  if (a_json.contains("forceRange") && a_json["forceRange"].is_array() &&
      a_json["forceRange"].size() >= 2) {
    act.forceRange_min.set_value(a_json["forceRange"][0].get<double>());
    act.forceRange_max.set_value(a_json["forceRange"][1].get<double>());
  }
  if (a_json.contains("actRange") && a_json["actRange"].is_array() &&
      a_json["actRange"].size() >= 2) {
    act.actRange_min.set_value(a_json["actRange"][0].get<double>());
    act.actRange_max.set_value(a_json["actRange"][1].get<double>());
  }
  double v = 0.0;
  if (JsonNumber(a_json, "ctrlRange_min", &v)) act.ctrlRange_min.set_value(v);
  if (JsonNumber(a_json, "ctrlRange_max", &v)) act.ctrlRange_max.set_value(v);
  if (JsonNumber(a_json, "forceRange_min", &v)) act.forceRange_min.set_value(v);
  if (JsonNumber(a_json, "forceRange_max", &v)) act.forceRange_max.set_value(v);
  if (JsonNumber(a_json, "actRange_min", &v)) act.actRange_min.set_value(v);
  if (JsonNumber(a_json, "actRange_max", &v)) act.actRange_max.set_value(v);
  if (JsonNumber(a_json, "lengthRange_min", &v)) act.lengthRange_min.set_value(v);
  if (JsonNumber(a_json, "lengthRange_max", &v)) act.lengthRange_max.set_value(v);
  if (JsonNumber(a_json, "crankLength", &v)) act.crankLength.set_value(v);
  if (JsonNumber(a_json, "inheritRange", &v)) act.inheritRange.set_value(v);
  int int_value = 0;
  if (JsonIntFromObjectOrParent(a_json, "", "group", &int_value)) {
    act.group.set_value(int_value);
  }
  if (JsonIntFromObjectOrParent(a_json, "", "actDim", &int_value)) {
    act.actDim.set_value(int_value);
  }
  bool bool_value = false;
  if (JsonBool(a_json, "jointInParent", &bool_value)) {
    act.jointInParent.set_value(bool_value);
  }
  if (JsonBool(a_json, "actEarly", &bool_value)) {
    act.actEarly.set_value(bool_value);
  }
  if (a_json.contains("gainType"))
    act.gainType.set_value(value::token(JsonString(a_json, "gainType", "fixed")));
  if (a_json.contains("biasType"))
    act.biasType.set_value(value::token(JsonString(a_json, "biasType", "none")));
  if (a_json.contains("dynType"))
    act.dynType.set_value(value::token(JsonString(a_json, "dynType", "none")));
  if (a_json.contains("ctrlLimited"))
    act.ctrlLimited.set_value(value::token(JsonString(a_json, "ctrlLimited", "auto")));
  if (a_json.contains("forceLimited"))
    act.forceLimited.set_value(value::token(JsonString(a_json, "forceLimited", "auto")));
  if (a_json.contains("actLimited"))
    act.actLimited.set_value(value::token(JsonString(a_json, "actLimited", "auto")));
  if (a_json.contains("plugin"))
    act.plugin.set_value(value::token(JsonString(a_json, "plugin", "")));
  if (a_json.contains("instance"))
    act.instance.set_value(value::token(JsonString(a_json, "instance", "")));
  auto resolve_site_rel = [&](const std::string &site,
                              RelationshipProperty *rel) {
    if (!rel || site.empty()) return;
    if (site[0] == '/') {
      rel->set(Path(site, ""));
      return;
    }
    auto it = site_name_to_usd.find(site);
    if (it != site_name_to_usd.end()) {
      rel->set(Path("/World/Sites/" + it->second, ""));
    }
  };
  resolve_site_rel(JsonString(a_json, "refSite"), &act.refSite);
  resolve_site_rel(JsonString(a_json, "sliderSite"), &act.sliderSite);

  std::string add_err;
  if (!actuators_prim.add_child(Prim(act), true, &add_err)) {
    SetErr(err, "Failed to add MjcActuator `" + act.name + "`: " + add_err);
    return false;
  }
  (void)warn;
  return true;
}

// MJCF <keyframe><key> -> MjcKeyframe prim (qpos/qvel/act/ctrl/mpos/mquat).
bool AddMjcKeyframeFromJson(Prim &keyframes_prim, const nlohmann::json &k_json,
                           std::set<std::string> &used_names, size_t index,
                           std::string *err) {
  MjcKeyframe kf;
  kf.name = UniqueUSDIdentifier(
      JsonString(k_json, "name", "key_" + std::to_string(index)), used_names,
      "key");
  auto setArr = [&](const char *key, auto &field) {
    auto v = JsonDoubleArray(k_json, key);
    if (!v.empty()) field.set_value(v);
  };
  setArr("qpos", kf.qpos);
  setArr("qvel", kf.qvel);
  setArr("act", kf.act);
  setArr("ctrl", kf.ctrl);
  setArr("mpos", kf.mpos);
  setArr("mquat", kf.mquat);

  std::string add_err;
  if (!keyframes_prim.add_child(Prim(kf), true, &add_err)) {
    SetErr(err, "Failed to add MjcKeyframe `" + kf.name + "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <sensor> child -> MjcSensor prim. A single typed prim covers all sensor
// kinds; the kind is `mjc:type` (the element name) and the measured object is
// `mjc:objtype`/`mjc:objname` (+ reftype/refname for frame sensors). JSON may
// be pre-normalized or use MuJoCo attribute aliases such as `site`, `joint`,
// `tendon`, `body`, and `refsite`.
bool AddMjcSensorFromJson(Prim &sensors_prim, const nlohmann::json &s_json,
                          std::set<std::string> &used_names, size_t index,
                          std::string *err) {
  MjcSensor sensor;
  sensor.name = UniqueUSDIdentifier(
      JsonString(s_json, "name", "sensor_" + std::to_string(index)), used_names,
      "sensor");
  auto tok = [&](const char *k, auto &field) {
    const std::string v = JsonString(s_json, k);
    if (!v.empty()) field.set_value(value::token(v));
  };
  tok("type", sensor.type);
  tok("objtype", sensor.objType);
  tok("objname", sensor.objName);
  tok("reftype", sensor.refType);
  tok("refname", sensor.refName);
  auto set_sensor_target_alias = [&](const char *key, const char *obj_type,
                                     auto &type_field, auto &name_field) {
    if (type_field.authored() || name_field.authored()) {
      return;
    }
    const std::string name = JsonString(s_json, key);
    if (!name.empty()) {
      type_field.set_value(value::token(obj_type));
      name_field.set_value(value::token(name));
    }
  };
  const std::pair<const char *, const char *> target_aliases[] = {
      {"site", "site"},       {"joint", "joint"},
      {"actuator", "actuator"}, {"tendon", "tendon"},
      {"body", "body"},       {"xbody", "xbody"},
      {"geom", "geom"},       {"camera", "camera"},
      {"light", "light"},     {"sensor", "sensor"},
      {"numeric", "numeric"}, {"text", "text"},
      {"tuple", "tuple"},     {"key", "key"},
      {"plugin", "plugin"}};
  for (const auto &alias : target_aliases) {
    set_sensor_target_alias(alias.first, alias.second, sensor.objType,
                            sensor.objName);
  }
  const std::pair<const char *, const char *> ref_aliases[] = {
      {"refsite", "site"},       {"refSite", "site"},
      {"refjoint", "joint"},     {"refJoint", "joint"},
      {"refactuator", "actuator"}, {"refActuator", "actuator"},
      {"reftendon", "tendon"},   {"refTendon", "tendon"},
      {"refbody", "body"},       {"refBody", "body"},
      {"refxbody", "xbody"},     {"refXbody", "xbody"},
      {"refgeom", "geom"},       {"refGeom", "geom"},
      {"refcamera", "camera"},   {"refCamera", "camera"},
      {"reflight", "light"},     {"refLight", "light"}};
  for (const auto &alias : ref_aliases) {
    set_sensor_target_alias(alias.first, alias.second, sensor.refType,
                            sensor.refName);
  }
  int g = 0;
  if (JsonIntFromObjectOrParent(s_json, "", "group", &g)) sensor.group.set_value(g);
  double v = 0.0;
  if (JsonNumber(s_json, "cutoff", &v)) sensor.cutoff.set_value(v);
  if (JsonNumber(s_json, "noise", &v)) sensor.noise.set_value(v);
  const auto user = JsonDoubleArray(s_json, "user");
  if (!user.empty()) sensor.user.set_value(user);

  std::string add_err;
  if (!sensors_prim.add_child(Prim(sensor), true, &add_err)) {
    SetErr(err, "Failed to add MjcSensor `" + sensor.name + "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <contact><pair> -> an Xform host prim under /World/Contacts carrying the
// pair geoms + collision params as generic mjc:* props (round-trips like the
// equality host prims; no dedicated schema). JSON: { name, geom1, geom2,
// condim, friction, solref, solimp, margin, gap }.
bool AddContactPairFromJson(Prim &contacts_prim, const nlohmann::json &p_json,
                            std::set<std::string> &used_names, size_t index,
                            std::string *err) {
  Xform host;
  host.name = UniqueUSDIdentifier(
      JsonString(p_json, "name", "pair_" + std::to_string(index)), used_names,
      "pair");
  AddAttr(host.props, "mjc:geom1", value::token(JsonString(p_json, "geom1")), true);
  AddAttr(host.props, "mjc:geom2", value::token(JsonString(p_json, "geom2")), true);
  int condim = 0;
  if (JsonIntFromObjectOrParent(p_json, "", "condim", &condim))
    AddAttr(host.props, "mjc:condim", condim, true);
  double v = 0.0;
  if (JsonNumber(p_json, "margin", &v)) AddAttr(host.props, "mjc:margin", v);
  if (JsonNumber(p_json, "gap", &v)) AddAttr(host.props, "mjc:gap", v);
  const auto friction = JsonDoubleArray(p_json, "friction");
  if (!friction.empty()) AddAttr(host.props, "mjc:friction", friction);
  const auto solref = JsonDoubleArray(p_json, "solref");
  if (!solref.empty()) AddAttr(host.props, "mjc:solref", solref);
  const auto solimp = JsonDoubleArray(p_json, "solimp");
  if (!solimp.empty()) AddAttr(host.props, "mjc:solimp", solimp);

  std::string add_err;
  if (!contacts_prim.add_child(Prim(host), true, &add_err)) {
    SetErr(err, "Failed to add contact pair `" + host.name + "`: " + add_err);
    return false;
  }
  return true;
}

// Build a row-vector local->world transform for a light whose emission axis
// (USD light -Z) points along the world-space `dir`, positioned at the world
// matrix's translation. `world` is the baked body*light frame (row-major,
// translation in row 3); `local_dir` is the MJCF <light dir> in that frame.
value::matrix4d LightTransformMatrix(const value::matrix4d &world,
                                     const std::array<double, 3> &local_dir) {
  // Rotate local_dir by the world frame's upper-3x3 (row-vector: v' = v * R).
  double wd[3];
  for (int j = 0; j < 3; j++) {
    wd[j] = local_dir[0] * world.m[0][j] + local_dir[1] * world.m[1][j] +
            local_dir[2] * world.m[2][j];
  }
  auto norm = [](double v[3]) {
    double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-12) { v[0] /= n; v[1] /= n; v[2] /= n; }
  };
  norm(wd);
  // USD light emits along -Z, so the local Z axis (world) = -dir.
  double z[3] = {-wd[0], -wd[1], -wd[2]};
  // Pick an up reference not parallel to z.
  double up[3] = {0, 0, 1};
  if (std::fabs(z[2]) > 0.95) { up[0] = 0; up[1] = 1; up[2] = 0; }
  double x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2],
                 up[0] * z[1] - up[1] * z[0]};
  norm(x);
  double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
                 z[0] * x[1] - z[1] * x[0]};
  value::matrix4d m;
  Identity(&m);
  for (int j = 0; j < 3; j++) {
    m.m[0][j] = x[j];
    m.m[1][j] = y[j];
    m.m[2][j] = z[j];
    m.m[3][j] = world.m[3][j];  // world translation
  }
  return m;
}

// MJCF <light> -> UsdLux: directional -> DistantLight, point/spot -> SphereLight
// (spot gets a shaping cone). Color from <light diffuse>. JSON:
//   { name, type, matrix:[16 world], dir:[3 local], color:[3], castshadow, cutoff }.
bool AddLightFromJson(Prim &lights_prim, const nlohmann::json &l_json,
                      std::set<std::string> &used_names, size_t index,
                      std::string *err) {
  const std::string usd_name = UniqueUSDIdentifier(
      JsonString(l_json, "name", "light_" + std::to_string(index)), used_names,
      "light");
  const std::string type = JsonString(l_json, "type", "spot");
  const value::matrix4d world = MatrixFromUSDArray(JsonDoubleArray(l_json, "matrix"));
  std::array<double, 3> dir{0, 0, -1};
  const auto d = JsonDoubleArray(l_json, "dir");
  if (d.size() >= 3) dir = {d[0], d[1], d[2]};
  const value::matrix4d xform = LightTransformMatrix(world, dir);

  value::color3f color{1.0f, 1.0f, 1.0f};
  const auto c = JsonFloatArray(l_json, "color");
  if (c.size() >= 3) color = {c[0], c[1], c[2]};
  bool castshadow = true;
  JsonBool(l_json, "castshadow", &castshadow);

  std::string add_err;
  if (type == "directional") {
    DistantLight light;
    light.name = usd_name;
    light.color.set_value(Animatable<value::color3f>(color));
    light.shadowEnable.set_value(Animatable<bool>(castshadow));
    AddTransformOp(light, xform);
    if (!lights_prim.add_child(Prim(usd_name, light), true, &add_err)) {
      SetErr(err, "Failed to add light `" + usd_name + "`: " + add_err);
      return false;
    }
  } else {
    SphereLight light;
    light.name = usd_name;
    light.radius.set_value(Animatable<float>(0.02f));  // near-point
    light.color.set_value(Animatable<value::color3f>(color));
    light.shadowEnable.set_value(Animatable<bool>(castshadow));
    if (type == "spot") {
      double cutoff = 45.0;
      JsonNumber(l_json, "cutoff", &cutoff);
      light.shapingConeAngle.set_value(Animatable<float>(static_cast<float>(cutoff)));
    }
    AddTransformOp(light, xform);
    if (!lights_prim.add_child(Prim(usd_name, light), true, &add_err)) {
      SetErr(err, "Failed to add light `" + usd_name + "`: " + add_err);
      return false;
    }
  }
  return true;
}

// MJCF <camera> -> UsdGeomCamera. fovy (vertical, degrees) -> verticalAperture
// for the default focalLength; orthographic projection honored. JSON:
//   { name, matrix:[16 world], fovy, orthographic }.
bool AddCameraFromJson(Prim &cameras_prim, const nlohmann::json &c_json,
                       std::set<std::string> &used_names, size_t index,
                       std::string *err) {
  GeomCamera cam;
  cam.name = UniqueUSDIdentifier(
      JsonString(c_json, "name", "camera_" + std::to_string(index)), used_names,
      "camera");
  AddTransformOp(cam, MatrixFromUSDArray(JsonDoubleArray(c_json, "matrix")));
  double fovy = 45.0;
  if (JsonNumber(c_json, "fovy", &fovy) && fovy > 0.0 && fovy < 180.0) {
    // verticalAperture = 2 * focalLength * tan(fovy/2). Keep the default
    // focalLength (50mm) and derive a matching vertical aperture (and a square
    // horizontal aperture as a sensor-agnostic default).
    const double f = 50.0;
    const double va = 2.0 * f * std::tan(fovy * 0.5 * 3.14159265358979323846 / 180.0);
    cam.focalLength.set_value(Animatable<float>(static_cast<float>(f)));
    cam.verticalAperture.set_value(Animatable<float>(static_cast<float>(va)));
    cam.horizontalAperture.set_value(Animatable<float>(static_cast<float>(va)));
  }
  bool ortho = false;
  if (JsonBool(c_json, "orthographic", &ortho) && ortho) {
    cam.projection.set_value(Animatable<GeomCamera::Projection>(
        GeomCamera::Projection::Orthographic));
  }
  std::string add_err;
  if (!cameras_prim.add_child(Prim(cam), true, &add_err)) {
    SetErr(err, "Failed to add camera `" + cam.name + "`: " + add_err);
    return false;
  }
  return true;
}

// MJCF <asset><material> -> UsdShade Material + child UsdPreviewSurface shader
// under /World/Materials. Color/PBR-scalar only (rgba/metallic/roughness/
// emission); texture maps are a documented follow-on. Geoms bind via a
// deterministic /World/Materials/<sanitized-name> path (see AddMeshFromJson).
bool AddMaterialFromJson(Prim &materials_prim, const nlohmann::json &m_json,
                         std::set<std::string> &used_names, size_t index,
                         std::string *err) {
  const std::string src =
      JsonString(m_json, "name", "material_" + std::to_string(index));
  const std::string usd = SanitizeUSDIdentifier(src, "material");
  if (!used_names.insert(usd).second) return true;  // MJCF names are unique

  Material mat;
  mat.name = usd;
  Shader shader;
  shader.name = "PreviewSurface";
  shader.info_id = kUsdPreviewSurface;

  UsdPreviewSurface ps;
  ps.outputsSurface.set_authored(true);
  const auto rgba = JsonFloatArray(m_json, "rgba");
  value::color3f base{0.8f, 0.8f, 0.8f};
  if (rgba.size() >= 3) {
    base = value::color3f{rgba[0], rgba[1], rgba[2]};
    ps.diffuseColor.set_value(Animatable<value::color3f>(base));
  }
  if (rgba.size() >= 4) ps.opacity.set_value(Animatable<float>(rgba[3]));
  double v = 0.0;
  if (JsonNumber(m_json, "metallic", &v))
    ps.metallic.set_value(Animatable<float>(static_cast<float>(v)));
  if (JsonNumber(m_json, "roughness", &v))
    ps.roughness.set_value(Animatable<float>(static_cast<float>(v)));
  double emission = 0.0;
  if (JsonNumber(m_json, "emission", &emission) && emission > 0.0) {
    ps.emissiveColor.set_value(Animatable<value::color3f>(value::color3f{
        base[0] * static_cast<float>(emission),
        base[1] * static_cast<float>(emission),
        base[2] * static_cast<float>(emission)}));
  }
  // A texture map (<asset><texture file=...>) connects diffuseColor to a
  // UsdUVTexture fed by a UsdPrimvarReader_float2 reading the mesh `st` primvar:
  //   PreviewSurface.diffuseColor <- DiffuseTexture.outputs:rgb
  //   DiffuseTexture.st           <- stReader.outputs:result
  const std::string mat_path = "/World/Materials/" + usd;
  const std::string tex_file = JsonString(m_json, "texture");
  const bool has_texture = !tex_file.empty();
  if (has_texture) {
    ps.diffuseColor.set_connection(Path(mat_path + "/DiffuseTexture", "outputs:rgb"));
    ps.diffuseColor.set_value_empty();
  }
  shader.value = std::move(ps);
  mat.surface.set(Path(mat_path + "/PreviewSurface", "outputs:surface"));

  Prim mat_prim(mat);
  std::string add_err;
  if (!mat_prim.add_child(Prim(shader), true, &add_err)) {
    SetErr(err, "Failed to add shader for material `" + usd + "`: " + add_err);
    return false;
  }
  if (has_texture) {
    UsdPrimvarReader_float2 reader;
    reader.varname.set_value(Animatable<std::string>("st"));
    reader.result.set_authored(true);
    Shader reader_shader;
    reader_shader.name = "stReader";
    reader_shader.info_id = kUsdPrimvarReader_float2;
    reader_shader.value = std::move(reader);

    UsdUVTexture tex;
    tex.file.set_value(Animatable<value::AssetPath>(value::AssetPath(tex_file)));
    tex.st.set_connection(Path(mat_path + "/stReader", "outputs:result"));
    tex.st.set_value_empty();
    tex.wrapS.set_value(Animatable<UsdUVTexture::Wrap>(UsdUVTexture::Wrap::Repeat));
    tex.wrapT.set_value(Animatable<UsdUVTexture::Wrap>(UsdUVTexture::Wrap::Repeat));
    tex.sourceColorSpace.set_value(
        Animatable<UsdUVTexture::SourceColorSpace>(UsdUVTexture::SourceColorSpace::SRGB));
    tex.outputsRGB.set_authored(true);
    Shader tex_shader;
    tex_shader.name = "DiffuseTexture";
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = std::move(tex);

    if (!mat_prim.add_child(Prim(reader_shader), true, &add_err) ||
        !mat_prim.add_child(Prim(tex_shader), true, &add_err)) {
      SetErr(err, "Failed to add texture shaders for material `" + usd + "`: " + add_err);
      return false;
    }
  }
  if (!materials_prim.add_child(std::move(mat_prim), true, &add_err)) {
    SetErr(err, "Failed to add material `" + usd + "`: " + add_err);
    return false;
  }
  return true;
}

bool AddMeshFromJson(Prim &link_prim, const nlohmann::json &mesh_json,
                     const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
                     const std::string &fallback_name, bool collision,
                     bool mjcf_source, std::string *warn, std::string *err) {
  const nlohmann::json *geom = &mesh_json;
  if (mesh_json.contains("geometry") && mesh_json["geometry"].is_object()) {
    geom = &mesh_json["geometry"];
  }
  std::vector<float> json_positions;
  std::vector<float> json_normals;
  std::vector<float> json_uvs;
  std::vector<int32_t> json_indices;
  const std::vector<float> *positions = nullptr;
  const std::vector<float> *normals = nullptr;
  const std::vector<float> *uvs = nullptr;
  const std::vector<int32_t> *source_indices = nullptr;
  const std::string mesh_ref = JsonString(mesh_json, "meshRef");
  if (!mesh_ref.empty()) {
    if (!mesh_buffers) {
      AppendWarn(warn, "Skipping mesh `" + fallback_name +
                           "`: meshRef `" + mesh_ref + "` was not registered.\n");
      return true;
    }
    auto buffer_it = mesh_buffers->find(mesh_ref);
    if (buffer_it == mesh_buffers->end()) {
      AppendWarn(warn, "Skipping mesh `" + fallback_name +
                           "`: meshRef `" + mesh_ref + "` was not registered.\n");
      return true;
    }
    const URDFMeshBuffer &buffer = buffer_it->second;
    positions = &buffer.positions;
    normals = &buffer.normals;
    uvs = &buffer.uvs;
    source_indices = &buffer.indices;
  } else {
    json_positions = JsonFloatArray(*geom, "positions");
    json_normals = JsonFloatArray(*geom, "normals");
    json_uvs = JsonFloatArray(*geom, "uvs");
    json_indices = JsonIntArray(*geom, "indices");
    positions = &json_positions;
    normals = &json_normals;
    uvs = &json_uvs;
    source_indices = &json_indices;
  }
  if (!positions || positions->size() < 9 || (positions->size() % 3) != 0) {
    AppendWarn(warn, "Skipping mesh `" + fallback_name +
                         "`: positions must contain at least 3 points.\n");
    return true;
  }

  std::vector<int32_t> indices;
  if (source_indices && !source_indices->empty()) {
    indices = *source_indices;
  } else {
    indices.resize(positions->size() / 3);
    for (size_t i = 0; i < indices.size(); i++) {
      indices[i] = static_cast<int32_t>(i);
    }
  }
  if ((indices.size() % 3) != 0) {
    AppendWarn(warn,
               "Skipping mesh `" + fallback_name + "`: indices must be triangles.\n");
    return true;
  }
  const size_t point_count = positions->size() / 3;
  bool identity_indices = indices.size() == point_count;
  for (size_t i = 0; i < indices.size(); i++) {
    const int32_t idx = indices[i];
    if (idx < 0 || static_cast<size_t>(idx) >= point_count) {
      AppendWarn(warn, "Skipping mesh `" + fallback_name +
                           "`: index out of range.\n");
      return true;
    }
    if (identity_indices && static_cast<size_t>(idx) != i) {
      identity_indices = false;
    }
  }

  const std::string mesh_name = SanitizeUSDIdentifier(
      JsonString(mesh_json, "name", fallback_name), fallback_name);
  GeomMesh mesh;
  mesh.name = mesh_name;
  mesh.subdivisionScheme.set_value(
      GeomMesh::SubdivisionScheme::SubdivisionSchemeNone);

  std::vector<value::point3f> points;
  points.reserve(positions->size() / 3);
  for (size_t i = 0; i + 2 < positions->size(); i += 3) {
    points.push_back(
        {(*positions)[i + 0], (*positions)[i + 1], (*positions)[i + 2]});
  }
  mesh.points.set_value(std::move(points));

  std::vector<int32_t> counts(indices.size() / 3, 3);
  mesh.faceVertexCounts.set_value(std::move(counts));
  mesh.faceVertexIndices.set_value(std::move(indices));

  if (normals && normals->size() == positions->size()) {
    std::vector<value::normal3f> ns;
    if (identity_indices) {
      ns.reserve(normals->size() / 3);
      for (size_t i = 0; i + 2 < normals->size(); i += 3) {
        ns.push_back({(*normals)[i + 0], (*normals)[i + 1], (*normals)[i + 2]});
      }
      mesh.normals.metas().set_interpolation_enum(Interpolation::Vertex);
    } else {
      ns.reserve(indices.size());
      for (int32_t idx : indices) {
        const size_t src = static_cast<size_t>(idx) * 3;
        ns.push_back({(*normals)[src + 0], (*normals)[src + 1],
                      (*normals)[src + 2]});
      }
      mesh.normals.metas().set_interpolation_enum(Interpolation::FaceVarying);
    }
    mesh.normals.set_value(std::move(ns));
  }

  if (uvs && uvs->size() == point_count * 2) {
    Attribute uv_attr;
    std::vector<value::texcoord2f> st;
    if (identity_indices) {
      st.reserve(uvs->size() / 2);
      for (size_t i = 0; i + 1 < uvs->size(); i += 2) {
        st.push_back({(*uvs)[i + 0], (*uvs)[i + 1]});
      }
      uv_attr.metas().set_interpolation_enum(Interpolation::Vertex);
    } else {
      st.reserve(indices.size());
      for (int32_t idx : indices) {
        const size_t src = static_cast<size_t>(idx) * 2;
        st.push_back({(*uvs)[src + 0], (*uvs)[src + 1]});
      }
      uv_attr.metas().set_interpolation_enum(Interpolation::FaceVarying);
    }
    uv_attr.set_value(std::move(st));
    mesh.props.emplace("primvars:st", Property(std::move(uv_attr), false));
  }

  const std::vector<double> matrix = JsonDoubleArray(mesh_json, "matrix");
  if (matrix.size() == 16) {
    AddTransformOp(mesh, MatrixFromUSDArray(matrix));
  }

  if (collision) {
    AddCollisionAPIs(mesh, true, mesh_json, mjcf_source);
  } else {
    AddAPISchemas(mesh.metas(), {{APISchemas::APIName::MjcImageableAPI, ""}});
    // Bind a UsdShade material (MJCF <geom material=..> -> /World/Materials/<m>).
    // Material names are unique in MJCF, so a deterministic sanitized path lets
    // us bind without threading a name map; the material prim is emitted later.
    const std::string mat_name = JsonString(mesh_json, "material");
    if (!mat_name.empty()) {
      Relationship mat_rel;
      mat_rel.set(Path("/World/Materials/" +
                       SanitizeUSDIdentifier(mat_name, "material"), ""));
      mesh.set_materialBinding(mat_rel);
      AppendAPISchema(mesh.metas(), APISchemas::APIName::MaterialBindingAPI);
    }
    if (mjcf_source) {
      int32_t group = 0;
      if (JsonIntFromObjectOrParent(mesh_json, "mjc", "group", &group) ||
          mesh_json.contains("group")) {
        AddAttr(mesh.props, "mjc:group", JsonInt(mesh_json, "group", group),
                true);
      }
    } else {
      AddAttr(mesh.props, "mjc:group",
              JsonInt(mesh_json, "group", kLegacyUrdfVisualGroup), true);
    }
  }

  std::string add_err;
  if (!link_prim.add_child(Prim(mesh), true, &add_err)) {
    SetErr(err, "Failed to add mesh `" + mesh_name + "`: " + add_err);
    return false;
  }
  return true;
}

bool AddNativeCollisionShapeFromJson(Prim &link_prim,
                                     const nlohmann::json &shape_json,
                                     const std::string &fallback_name,
                                     bool mjcf_source, std::string *warn,
                                     std::string *err) {
  const nlohmann::json shape =
      (shape_json.contains("shape") && shape_json["shape"].is_object())
          ? shape_json["shape"]
          : shape_json;
  const std::string type = JsonString(shape, "type");
  if (type.empty()) {
    AppendWarn(warn, "Skipping collision shape `" + fallback_name +
                         "`: missing shape.type.\n");
    return true;
  }

  const std::string name = SanitizeUSDIdentifier(
      JsonString(shape_json, "name", fallback_name), fallback_name);
  const std::vector<double> matrix = JsonDoubleArray(shape_json, "matrix");

  if (type == "box" || type == "cube") {
    GeomCube cube;
    cube.name = name;
    cube.size.set_value(2.0);
    if (matrix.size() == 16) {
      AddTransformOp(cube, MatrixFromUSDArray(matrix));
    }
    AddCollisionAPIs(cube, false, shape_json, mjcf_source);
    return AddGeomChild(link_prim, std::move(cube), name, err);
  }

  if (type == "sphere") {
    GeomSphere sphere;
    sphere.name = name;
    double radius = 0.5;
    JsonNumber(shape, "radius", &radius);
    sphere.radius.set_value(radius);
    if (matrix.size() == 16) {
      AddTransformOp(sphere, MatrixFromUSDArray(matrix));
    }
    AddCollisionAPIs(sphere, false, shape_json, mjcf_source);
    return AddGeomChild(link_prim, std::move(sphere), name, err);
  }

  if (type == "cylinder") {
    GeomCylinder cylinder;
    cylinder.name = name;
    double radius = 0.5;
    double height = 1.0;
    JsonNumber(shape, "radius", &radius);
    JsonNumber(shape, "height", &height);
    cylinder.radius.set_value(radius);
    cylinder.height.set_value(height);
    cylinder.axis.set_value(AxisFromToken(JsonString(shape, "axis", "Z")));
    if (matrix.size() == 16) {
      AddTransformOp(cylinder, MatrixFromUSDArray(matrix));
    }
    AddCollisionAPIs(cylinder, false, shape_json, mjcf_source);
    return AddGeomChild(link_prim, std::move(cylinder), name, err);
  }

  if (type == "capsule") {
    GeomCapsule capsule;
    capsule.name = name;
    double radius = 0.5;
    double height = 1.0;
    JsonNumber(shape, "radius", &radius);
    JsonNumber(shape, "height", &height);
    capsule.radius.set_value(radius);
    capsule.height.set_value(height);
    capsule.axis.set_value(AxisFromToken(JsonString(shape, "axis", "Z")));
    if (matrix.size() == 16) {
      AddTransformOp(capsule, MatrixFromUSDArray(matrix));
    }
    AddCollisionAPIs(capsule, false, shape_json, mjcf_source);
    return AddGeomChild(link_prim, std::move(capsule), name, err);
  }

  if (type == "plane") {
    GeomPlane plane;
    plane.name = name;
    double width = 2.0;
    double length = 2.0;
    JsonNumber(shape, "width", &width);
    JsonNumber(shape, "length", &length);
    plane.width.set_value(width);
    plane.length.set_value(length);
    plane.axis.set_value(AxisFromToken(JsonString(shape, "axis", "Z")));
    if (matrix.size() == 16) {
      AddTransformOp(plane, MatrixFromUSDArray(matrix));
    }
    AddCollisionAPIs(plane, false, shape_json, mjcf_source);
    return AddGeomChild(link_prim, std::move(plane), name, err);
  }

  AppendWarn(warn, "Skipping collision shape `" + fallback_name +
                       "`: unsupported shape type `" + type + "`.\n");
  return true;
}

}  // namespace

bool ConvertURDFJsonToUSDStage(
    const std::string &robot_json,
    const std::map<std::string, URDFMeshBuffer> *mesh_buffers,
    Stage *out_stage,
    std::string *warn, std::string *err) {
  if (!out_stage) {
    SetErr(err, "Output Stage pointer is null");
    return false;
  }
  if (warn) {
    warn->clear();
  }
  if (err) {
    err->clear();
  }

  MiniURDFPayload payload;
  if (!MiniURDFPayload::Parse(robot_json, &payload, err)) {
    return false;
  }
  const nlohmann::json &root = payload.root;
  const nlohmann::json &empty_array = payload.empty_array;
  const nlohmann::json &links_json = payload.Array("links");
  const nlohmann::json &joints_json = payload.Array("joints");
  const nlohmann::json &actuators_json = payload.Array("actuators");
  const nlohmann::json &tendons_json = payload.Array("tendons");
  const nlohmann::json &equalities_json = payload.Array("equalities");
  const nlohmann::json &sites_json = payload.Array("sites");
  const bool mjcf_source = payload.mjcf_source;

  Stage stage;
  stage.metas().defaultPrim = value::token("World");
  const std::string up_axis = JsonString(root, "upAxis", "Y");
  const bool y_up = !(up_axis == "Z" || up_axis == "z");
  stage.metas().upAxis = y_up ? Axis::Y : Axis::Z;
  // MuJoCo authors in SI units (meters, kilograms). Declare the stage scale so
  // usdchecker's StageMetadataChecker passes and DCCs interpret distances and
  // masses correctly (1 stage unit = 1 m / 1 kg).
  stage.metas().metersPerUnit = 1.0;
  stage.metas().kilogramsPerUnit = 1.0;

  Xform world;
  world.name = "World";
  world.metas().set_kind(Kind::Assembly);

  // URDF/MJCF source data is authored Z-up (and all current frontends emit
  // Z-up world transforms). When the target stage is Y-up, reconcile the two
  // with a single corrective root rotation Rx(-90deg) (+Z -> +Y) on `World`.
  //
  // Per doc/usd-physics-upAxis.md this is the only correct way to convert the
  // up axis: a single root rotation propagates through the transform hierarchy
  // to every descendant body/collider world transform, while local-frame data
  // (physics:axis, joint localPos0/1 + localRot0/1, collision shape axis) and
  // the world-space `physics:gravityDirection` attribute correctly ride along
  // unchanged. Per-property/per-axis-token conversion would double-rotate.
  if (y_up) {
    XformOp rot_x;
    rot_x.op_type = XformOp::OpType::RotateX;
    rot_x.set_value(-90.0);  // degrees; row-vector RotateX(-90) maps +Z -> +Y
    world.xformOps.push_back(rot_x);
  }

  PhysicsScene scene;
  scene.name = "PhysicsScene";
  {
    value::vector3f gravity_dir{0.0f, -1.0f, 0.0f};
    std::vector<float> g = JsonFloatArray(root, "gravity");
    if (g.size() >= 3) {
      gravity_dir = {g[0], g[1], g[2]};
    }
    scene.gravityDirection.set_value(gravity_dir);
    scene.gravityMagnitude.set_value(9.80665f);
    MjcSceneAPI mjc_scene;
    double timestep = 0.002;
    if (JsonNumber(root, "timestep", &timestep)) {
      mjc_scene.timestep.set_value(timestep);
    }
    // Full <option>/<flag>/<compiler> set (overrides/augments timestep).
    ApplyMjcSceneOptions(root, mjc_scene);
    scene.mjcScene = mjc_scene;
    AddNewtonSceneOptions(root, scene);
    AddAPISchemas(scene.metas(), {
                                     {APISchemas::APIName::MjcSceneAPI, ""},
                                     {APISchemas::APIName::NewtonSceneAPI, ""},
                                 });
  }

  Xform links_scope;
  links_scope.name = "Links";
  Xform joints_scope;
  joints_scope.name = "Joints";

  std::map<std::string, std::string> link_name_to_usd;
  std::map<std::string, size_t> link_name_to_index;
  std::set<std::string> used_link_names;
  std::vector<Prim> link_prims;
  std::set<std::string> child_links;
  for (const auto &joint_json : joints_json) {
    const std::string child = JsonString(joint_json, "child");
    if (!child.empty()) {
      child_links.insert(child);
    }
  }
  bool self_collision_enabled = true;
  if (const nlohmann::json *newton = JsonObjectOrNull(root, "newton")) {
    JsonBool(*newton, "selfCollisionEnabled", &self_collision_enabled);
  }

  for (size_t link_index = 0; link_index < links_json.size(); link_index++) {
    const nlohmann::json &link_json = links_json[link_index];
    const std::string link_name =
        JsonString(link_json, "name", "link_" + std::to_string(link_index));
    const std::string usd_link_name =
        UniqueUSDIdentifier(link_name, used_link_names, "link");
    link_name_to_usd[link_name] = usd_link_name;
    link_name_to_index[link_name] = link_prims.size();

    // A world-fixed link (e.g. the synthetic "world" link holding worldbody-
    // level floor/ground/hfield geoms) is a static collider, not a dynamic
    // body: rigidBodyEnabled=false and no articulation root.
    bool is_static = false;
    JsonBool(link_json, "static", &is_static);

    Xform link_xform;
    link_xform.name = usd_link_name;
    AddAPISchemas(link_xform.metas(), {
                                         {APISchemas::APIName::PhysicsRigidBodyAPI,
                                          ""},
                                         {APISchemas::APIName::PhysicsMassAPI, ""},
                                     });
    AddAttr(link_xform.props, "physics:rigidBodyEnabled", !is_static);
    AddAttr(link_xform.props, "physics:startsAsleep", false);
    // MuJoCo mocap body (externally driven, not simulated).
    bool is_mocap = false;
    if (JsonBool(link_json, "mocap", &is_mocap) && is_mocap) {
      AddAttr(link_xform.props, "mjc:mocap", true, /*uniform=*/true);
    }
    // MuJoCo <freejoint>: a 6-DOF floating base. Mark it so a floating-base
    // articulation root is distinguishable from a fixed (anchored) base, which
    // is a parentless link WITHOUT this flag.
    bool is_floating = false;
    JsonBool(link_json, "floating", &is_floating);
    if (is_floating) {
      AddAttr(link_xform.props, "mjc:freeJoint", true, /*uniform=*/true);
    }
    if (!is_static && !child_links.count(link_name)) {
      AppendAPISchema(link_xform.metas(),
                      APISchemas::APIName::PhysicsArticulationRootAPI);
      AppendAPISchema(link_xform.metas(),
                      APISchemas::APIName::NewtonArticulationRootAPI);
      AddAttr(link_xform.props, "newton:selfCollisionEnabled",
              self_collision_enabled);
    }

    if (link_json.contains("inertial") && link_json["inertial"].is_object()) {
      const nlohmann::json &inertial = link_json["inertial"];
      float mass = 0.0f;
      if (JsonNumber(inertial, "mass", &mass) && mass > 0.0f) {
        AddAttr(link_xform.props, "physics:mass", mass);
      }
      std::vector<float> com = JsonFloatArray(inertial, "centerOfMass");
      if (com.size() >= 3) {
        AddAttr(link_xform.props, "physics:centerOfMass",
                value::point3f{com[0], com[1], com[2]});
      }
      // Full (non-diagonal) inertia tensor: diagonalize into principal moments
      // (physics:diagonalInertia) + a principal-axes rotation
      // (physics:principalAxes), the lossless USD representation. Falls back to
      // the diagonal-only path when only diaginertia was authored.
      std::vector<double> fullI = JsonDoubleArray(inertial, "fullInertia");
      if (fullI.size() >= 6) {
        // [Ixx, Iyy, Izz, Ixy, Ixz, Iyz] -> symmetric 3x3.
        const double m[3][3] = {{fullI[0], fullI[3], fullI[4]},
                                {fullI[3], fullI[1], fullI[5]},
                                {fullI[4], fullI[5], fullI[2]}};
        double eval[3];
        double evec[3][3];
        JacobiEigenSymmetric3(m, eval, evec);
        AddAttr(link_xform.props, "physics:diagonalInertia",
                value::float3{static_cast<float>(eval[0]),
                              static_cast<float>(eval[1]),
                              static_cast<float>(eval[2])});
        AddAttr(link_xform.props, "physics:principalAxes",
                RotationMatrixToQuatf(evec));
      } else {
        std::vector<float> inertia =
            JsonFloatArray(inertial, "diagonalInertia");
        if (inertia.size() >= 3) {
          AddAttr(link_xform.props, "physics:diagonalInertia",
                  value::float3{inertia[0], inertia[1], inertia[2]});
        }
      }
    }

    Prim link_prim(link_xform);
    if (link_json.contains("visuals") && link_json["visuals"].is_array()) {
      size_t i = 0;
      for (const auto &visual : link_json["visuals"]) {
        if (!AddMeshFromJson(link_prim, visual, mesh_buffers,
                             "visual_" + std::to_string(i++), false,
                             mjcf_source, warn, err)) {
          return false;
        }
      }
    }
    if (link_json.contains("collisions") &&
        link_json["collisions"].is_array()) {
      size_t i = 0;
      for (const auto &collision : link_json["collisions"]) {
        const std::string fallback = "collision_" + std::to_string(i++);
        if (collision.contains("shape") && collision["shape"].is_object()) {
          if (!AddNativeCollisionShapeFromJson(link_prim, collision, fallback,
                                               mjcf_source, warn, err)) {
            return false;
          }
          continue;
        }
        if (!AddMeshFromJson(link_prim, collision, mesh_buffers, fallback, true,
                             mjcf_source, warn, err)) {
          return false;
        }
      }
    }

    link_prims.push_back(std::move(link_prim));
  }

  if (root.contains("filteredPairs") && root["filteredPairs"].is_array()) {
    std::map<std::string, std::vector<Path>> filtered_targets;
    for (const auto &pair_json : root["filteredPairs"]) {
      if (!pair_json.is_object()) {
        continue;
      }
      const std::string body1 = JsonString(pair_json, "body1");
      const std::string body2 = JsonString(pair_json, "body2");
      auto it1 = link_name_to_usd.find(body1);
      auto it2 = link_name_to_usd.find(body2);
      if (it1 == link_name_to_usd.end() || it2 == link_name_to_usd.end()) {
        AppendWarn(warn, "Skipping filtered pair `" + body1 + "` / `" +
                             body2 + "`: link was not exported.\n");
        continue;
      }
      filtered_targets[body1].push_back(
          Path("/World/Links/" + it2->second, ""));
    }
    for (const auto &kv : filtered_targets) {
      auto index_it = link_name_to_index.find(kv.first);
      if (index_it == link_name_to_index.end() ||
          index_it->second >= link_prims.size() || kv.second.empty()) {
        continue;
      }
      Prim &prim = link_prims[index_it->second];
      AppendAPISchema(prim.metas(), APISchemas::APIName::PhysicsFilteredPairsAPI);
      Relationship rel;
      rel.set(kv.second);
      if (auto *xform = prim.get_data().as<Xform>()) {
        xform->props["physics:filteredPairs"] = Property(std::move(rel), false);
      }
    }
  }

  Prim joints_prim(joints_scope);
  std::set<std::string> used_joint_names;
  std::vector<std::string> joint_usd_names(joints_json.size());
  std::map<std::string, std::string> joint_name_to_usd;
  for (size_t joint_index = 0; joint_index < joints_json.size(); joint_index++) {
    const nlohmann::json &joint_json = joints_json[joint_index];
    const std::string source_name =
        JsonString(joint_json, "name", "joint_" + std::to_string(joint_index));
    const std::string joint_name =
        UniqueUSDIdentifier(source_name, used_joint_names, "joint");
    joint_usd_names[joint_index] = joint_name;
    joint_name_to_usd[source_name] = joint_name;
  }
  for (size_t joint_index = 0; joint_index < joints_json.size(); joint_index++) {
    const nlohmann::json &joint_json = joints_json[joint_index];
    const std::string type = JsonString(joint_json, "type", "fixed");
    const std::string parent = JsonString(joint_json, "parent");
    const std::string child = JsonString(joint_json, "child");
    if (!link_name_to_usd.count(parent) || !link_name_to_usd.count(child)) {
      AppendWarn(warn, "Skipping joint `" + JsonString(joint_json, "name", "joint") +
                           "`: parent or child link was not exported.\n");
      continue;
    }

    const std::string joint_name = joint_usd_names[joint_index];
    const std::string parent_usd = link_name_to_usd[parent];
    const std::string child_usd = link_name_to_usd[child];
    const std::string axis = AxisToken(joint_json);
    const bool rotational = (type == "revolute" || type == "continuous");

    // localRot0 is authoritative for the joint frame rotation. If a producer
    // omitted it but baked a rotation into originMatrix, that rotation is lost
    // (we only read originMatrix's translation) — surface it rather than drop
    // it silently.
    if (!joint_json.contains("localRot0") &&
        JointOriginMatrixHasRotation(joint_json)) {
      AppendWarn(warn, "Joint `" + joint_name +
                           "` has a rotation in originMatrix but no localRot0; "
                           "the rotation is ignored (supply localRot0).\n");
    }

    std::string add_err;
    if (type == "revolute" || type == "continuous") {
      PhysicsRevoluteJoint joint;
      joint.name = joint_name;
      AssignJointBase(joint, joint_json, parent_usd, child_usd,
                      joint_name_to_usd);
      joint.axis.set_value(value::token(axis));

      if (type == "revolute" && joint_json.contains("limit") &&
          joint_json["limit"].is_object()) {
        constexpr double kRadToDeg = 57.2957795130823208768;
        double lower = 0.0;
        double upper = 0.0;
        if (JsonNumber(joint_json["limit"], "lower", &lower)) {
          joint.lowerLimit.set_value(static_cast<float>(lower * kRadToDeg));
        }
        if (JsonNumber(joint_json["limit"], "upper", &upper)) {
          joint.upperLimit.set_value(static_cast<float>(upper * kRadToDeg));
        }
        const std::string dof = JointDofName(axis, rotational);
        APISchemas api = joint.metas().get_apiSchemas();
        api.names.push_back({APISchemas::APIName::PhysicsLimitAPI, dof});
        joint.metas().set_apiSchemas(api);
        AddAttr(joint.props, "physics:limit:" + dof + ":low",
                static_cast<float>(lower * kRadToDeg));
        AddAttr(joint.props, "physics:limit:" + dof + ":high",
                static_cast<float>(upper * kRadToDeg));
      }
      if (!joints_prim.add_child(Prim(joint), true, &add_err)) {
        SetErr(err, "Failed to add revolute joint `" + joint_name +
                        "`: " + add_err);
        return false;
      }
    } else if (type == "prismatic") {
      PhysicsPrismaticJoint joint;
      joint.name = joint_name;
      AssignJointBase(joint, joint_json, parent_usd, child_usd,
                      joint_name_to_usd);
      joint.axis.set_value(value::token(axis));

      if (joint_json.contains("limit") && joint_json["limit"].is_object()) {
        double lower = 0.0;
        double upper = 0.0;
        if (JsonNumber(joint_json["limit"], "lower", &lower)) {
          joint.lowerLimit.set_value(static_cast<float>(lower));
        }
        if (JsonNumber(joint_json["limit"], "upper", &upper)) {
          joint.upperLimit.set_value(static_cast<float>(upper));
        }
        const std::string dof = JointDofName(axis, false);
        APISchemas api = joint.metas().get_apiSchemas();
        api.names.push_back({APISchemas::APIName::PhysicsLimitAPI, dof});
        joint.metas().set_apiSchemas(api);
        AddAttr(joint.props, "physics:limit:" + dof + ":low",
                static_cast<float>(lower));
        AddAttr(joint.props, "physics:limit:" + dof + ":high",
                static_cast<float>(upper));
      }
      if (!joints_prim.add_child(Prim(joint), true, &add_err)) {
        SetErr(err, "Failed to add prismatic joint `" + joint_name +
                        "`: " + add_err);
        return false;
      }
    } else if (type == "spherical") {
      // MuJoCo ball joint (3-DOF rotation) -> PhysicsSphericalJoint. Cone-angle
      // limits aren't carried in the JSON, so they're left unauthored (free
      // rotation), which round-trips structurally for the preview/articulation.
      PhysicsSphericalJoint joint;
      joint.name = joint_name;
      AssignJointBase(joint, joint_json, parent_usd, child_usd,
                      joint_name_to_usd);
      joint.axis.set_value(value::token(axis));
      if (!joints_prim.add_child(Prim(joint), true, &add_err)) {
        SetErr(err, "Failed to add spherical joint `" + joint_name +
                        "`: " + add_err);
        return false;
      }
    } else {
      if (type != "fixed") {
        AppendWarn(warn, "Joint `" + joint_name + "` type `" + type +
                             "` is exported as PhysicsFixedJoint.\n");
      }
      PhysicsFixedJoint joint;
      joint.name = joint_name;
      AssignJointBase(joint, joint_json, parent_usd, child_usd,
                      joint_name_to_usd);
      if (!joints_prim.add_child(Prim(joint), true, &add_err)) {
        SetErr(err,
               "Failed to add fixed joint `" + joint_name + "`: " + add_err);
        return false;
      }
    }
  }

  Prim links_prim(links_scope);
  for (auto &link_prim : link_prims) {
    std::string add_err;
    if (!links_prim.add_child(std::move(link_prim), true, &add_err)) {
      SetErr(err, "Failed to add link: " + add_err);
      return false;
    }
  }

  Prim world_prim(world);
  Prim actuators_prim;
  bool has_actuators = false;
  if (!actuators_json.empty()) {
    Xform actuators_scope;
    actuators_scope.name = "Actuators";
    actuators_prim = Prim(actuators_scope);
    for (size_t i = 0; i < actuators_json.size(); i++) {
      if (!AddNewtonActuatorFromJson(actuators_prim, actuators_json[i],
                                     joint_name_to_usd, i, warn, err)) {
        return false;
      }
    }
    has_actuators = !actuators_prim.children().empty();
  }

  // MJCF <site> -> /World/Sites/<name> (GeomSphere markers + MjcSiteAPI). Built
  // before tendons so spatial (muscle) tendons can resolve their routing sites.
  Prim sites_prim;
  bool has_sites = false;
  std::map<std::string, std::string> site_name_to_usd;
  if (!sites_json.empty()) {
    Xform sites_scope;
    sites_scope.name = "Sites";
    sites_prim = Prim(sites_scope);
    std::set<std::string> used_site_names;
    for (size_t i = 0; i < sites_json.size(); i++) {
      if (!AddMjcSiteFromJson(sites_prim, sites_json[i], used_site_names,
                              site_name_to_usd, i, err)) {
        return false;
      }
    }
    has_sites = !sites_prim.children().empty();
  }

  // MJCF <tendon> -> /World/Tendons/<name> (MjcTendon prims).
  Prim tendons_prim;
  bool has_tendons = false;
  std::map<std::string, std::string> tendon_name_to_usd;
  if (!tendons_json.empty()) {
    Xform tendons_scope;
    tendons_scope.name = "Tendons";
    tendons_prim = Prim(tendons_scope);
    for (size_t i = 0; i < tendons_json.size(); i++) {
      if (!AddMjcTendonFromJson(tendons_prim, tendons_json[i], joint_name_to_usd,
                                site_name_to_usd, tendon_name_to_usd, i, warn,
                                err)) {
        return false;
      }
    }
    has_tendons = !tendons_prim.children().empty();
  }

  // MJCF <general>/<muscle> actuators -> /World/MjcActuators (MjcActuator prims).
  const nlohmann::json &mjc_actuators_json =
      (root.contains("mjcActuators") && root["mjcActuators"].is_array())
          ? root["mjcActuators"]
          : empty_array;
  Prim mjc_actuators_prim;
  bool has_mjc_actuators = false;
  if (!mjc_actuators_json.empty()) {
    Xform scope;
    scope.name = "MjcActuators";
    mjc_actuators_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < mjc_actuators_json.size(); i++) {
      if (!AddMjcActuatorFromJson(mjc_actuators_prim, mjc_actuators_json[i],
                                  joint_name_to_usd, tendon_name_to_usd,
                                  site_name_to_usd, link_name_to_usd, used, i,
                                  warn, err)) {
        return false;
      }
    }
    has_mjc_actuators = !mjc_actuators_prim.children().empty();
  }

  // MJCF <keyframe> -> /World/Keyframes/<name> (MjcKeyframe prims).
  const nlohmann::json &keyframes_json =
      (root.contains("keyframes") && root["keyframes"].is_array())
          ? root["keyframes"]
          : empty_array;
  Prim keyframes_prim;
  bool has_keyframes = false;
  if (!keyframes_json.empty()) {
    Xform scope;
    scope.name = "Keyframes";
    keyframes_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < keyframes_json.size(); i++) {
      if (!AddMjcKeyframeFromJson(keyframes_prim, keyframes_json[i], used, i,
                                  err)) {
        return false;
      }
    }
    has_keyframes = !keyframes_prim.children().empty();
  }

  // MJCF <sensor> -> /World/Sensors (MjcSensor prims).
  const nlohmann::json &sensors_json =
      (root.contains("sensors") && root["sensors"].is_array()) ? root["sensors"]
                                                               : empty_array;
  Prim sensors_prim;
  bool has_sensors = false;
  if (!sensors_json.empty()) {
    Xform scope;
    scope.name = "Sensors";
    sensors_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < sensors_json.size(); i++) {
      if (!AddMjcSensorFromJson(sensors_prim, sensors_json[i], used, i, err)) {
        return false;
      }
    }
    has_sensors = !sensors_prim.children().empty();
  }

  // MJCF <contact><pair> -> /World/Contacts (host Xforms with mjc:* props).
  const nlohmann::json &contact_pairs_json =
      (root.contains("contactPairs") && root["contactPairs"].is_array())
          ? root["contactPairs"]
          : empty_array;
  Prim contacts_prim;
  bool has_contacts = false;
  if (!contact_pairs_json.empty()) {
    Xform scope;
    scope.name = "Contacts";
    contacts_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < contact_pairs_json.size(); i++) {
      if (!AddContactPairFromJson(contacts_prim, contact_pairs_json[i], used, i,
                                  err)) {
        return false;
      }
    }
    has_contacts = !contacts_prim.children().empty();
  }

  // MJCF <custom><numeric|text> -> /World/MjcCustom (model metadata / MJX knobs).
  Prim custom_prim;
  bool has_custom = false;
  if (const nlohmann::json *custom = JsonObjectOrNull(root, "custom")) {
    Xform scope;
    scope.name = "MjcCustom";
    if (custom->contains("numeric") && (*custom)["numeric"].is_array()) {
      for (const auto &n : (*custom)["numeric"]) {
        const std::string nm = JsonString(n, "name");
        if (nm.empty()) continue;
        AddAttr(scope.props, "mjc:custom:" + nm, JsonDoubleArray(n, "data"));
      }
    }
    if (custom->contains("text") && (*custom)["text"].is_array()) {
      for (const auto &t : (*custom)["text"]) {
        const std::string nm = JsonString(t, "name");
        if (nm.empty()) continue;
        AddAttr(scope.props, "mjc:customtext:" + nm,
                value::token(JsonString(t, "data")));
      }
    }
    if (!scope.props.empty()) {
      custom_prim = Prim(scope);
      has_custom = true;
    }
  }

  // MJCF <extension><plugin><instance><config> -> /World/MjcPlugins. The
  // instance config (e.g. a PID actuator's kp/ki/kd) is referenced by an
  // MjcActuator's mjc:instance; preserve it so the engine-plugin setup survives.
  Prim plugins_prim;
  bool has_plugins = false;
  if (root.contains("plugins") && root["plugins"].is_array()) {
    Xform scope;
    scope.name = "MjcPlugins";
    for (const auto &p : root["plugins"]) {
      const std::string inst = JsonString(p, "instance");
      if (inst.empty()) continue;
      const std::string plugin_id = JsonString(p, "plugin");
      if (!plugin_id.empty()) {
        AddAttr(scope.props, "mjc:plugin:" + inst + ":plugin",
                value::token(plugin_id));
      }
      if (p.contains("config") && p["config"].is_object()) {
        const auto *config = p["config"].object_items();
        for (const auto &it : *config) {
          if (!it.value().is_string()) continue;
          AddAttr(scope.props,
                  "mjc:plugin:" + inst + ":config:" + it.key,
                  value::token(it.value().get<std::string>()));
        }
      }
    }
    if (!scope.props.empty()) {
      plugins_prim = Prim(scope);
      has_plugins = true;
    }
  }

  // MJCF <light> -> /World/Lights (UsdLux); <camera> -> /World/Cameras.
  const nlohmann::json &lights_json =
      (root.contains("lights") && root["lights"].is_array()) ? root["lights"]
                                                             : empty_array;
  const nlohmann::json &cameras_json =
      (root.contains("cameras") && root["cameras"].is_array()) ? root["cameras"]
                                                               : empty_array;
  Prim lights_prim;
  bool has_lights = false;
  if (!lights_json.empty()) {
    Xform scope;
    scope.name = "Lights";
    lights_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < lights_json.size(); i++) {
      if (!AddLightFromJson(lights_prim, lights_json[i], used, i, err)) {
        return false;
      }
    }
    has_lights = !lights_prim.children().empty();
  }
  Prim cameras_prim;
  bool has_cameras = false;
  if (!cameras_json.empty()) {
    Xform scope;
    scope.name = "Cameras";
    cameras_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < cameras_json.size(); i++) {
      if (!AddCameraFromJson(cameras_prim, cameras_json[i], used, i, err)) {
        return false;
      }
    }
    has_cameras = !cameras_prim.children().empty();
  }

  // MJCF <asset><material> -> /World/Materials (UsdShade Material). Geoms bind
  // via /World/Materials/<sanitized-name> (set in AddMeshFromJson).
  const nlohmann::json &materials_json =
      (root.contains("materials") && root["materials"].is_array())
          ? root["materials"]
          : empty_array;
  Prim materials_prim;
  bool has_materials = false;
  if (!materials_json.empty()) {
    Xform scope;
    scope.name = "Materials";
    materials_prim = Prim(scope);
    std::set<std::string> used;
    for (size_t i = 0; i < materials_json.size(); i++) {
      if (!AddMaterialFromJson(materials_prim, materials_json[i], used, i, err)) {
        return false;
      }
    }
    has_materials = !materials_prim.children().empty();
  }

  // MJCF <equality> -> /World/Equalities/<name> (Xform + MjcEquality*API).
  Prim equalities_prim;
  bool has_equalities = false;
  if (!equalities_json.empty()) {
    Xform equalities_scope;
    equalities_scope.name = "Equalities";
    equalities_prim = Prim(equalities_scope);
    for (size_t i = 0; i < equalities_json.size(); i++) {
      if (!AddMjcEqualityFromJson(equalities_prim, equalities_json[i],
                                  link_name_to_usd, joint_name_to_usd, i, warn,
                                  err)) {
        return false;
      }
    }
    has_equalities = !equalities_prim.children().empty();
  }
  {
    std::string add_err;
    if (!world_prim.add_child(Prim(scene), true, &add_err) ||
        !world_prim.add_child(std::move(links_prim), true, &add_err) ||
        !world_prim.add_child(std::move(joints_prim), true, &add_err)) {
      SetErr(err, "Failed to assemble URDF USD stage: " + add_err);
      return false;
    }
    if (has_actuators &&
        !world_prim.add_child(std::move(actuators_prim), true, &add_err)) {
      SetErr(err, "Failed to add Newton actuator scope: " + add_err);
      return false;
    }
    if (has_tendons &&
        !world_prim.add_child(std::move(tendons_prim), true, &add_err)) {
      SetErr(err, "Failed to add tendon scope: " + add_err);
      return false;
    }
    if (has_equalities &&
        !world_prim.add_child(std::move(equalities_prim), true, &add_err)) {
      SetErr(err, "Failed to add equality scope: " + add_err);
      return false;
    }
    if (has_sites &&
        !world_prim.add_child(std::move(sites_prim), true, &add_err)) {
      SetErr(err, "Failed to add site scope: " + add_err);
      return false;
    }
    if (has_mjc_actuators &&
        !world_prim.add_child(std::move(mjc_actuators_prim), true, &add_err)) {
      SetErr(err, "Failed to add MjcActuator scope: " + add_err);
      return false;
    }
    if (has_keyframes &&
        !world_prim.add_child(std::move(keyframes_prim), true, &add_err)) {
      SetErr(err, "Failed to add Keyframes scope: " + add_err);
      return false;
    }
    if (has_lights &&
        !world_prim.add_child(std::move(lights_prim), true, &add_err)) {
      SetErr(err, "Failed to add Lights scope: " + add_err);
      return false;
    }
    if (has_cameras &&
        !world_prim.add_child(std::move(cameras_prim), true, &add_err)) {
      SetErr(err, "Failed to add Cameras scope: " + add_err);
      return false;
    }
    if (has_materials &&
        !world_prim.add_child(std::move(materials_prim), true, &add_err)) {
      SetErr(err, "Failed to add Materials scope: " + add_err);
      return false;
    }
    if (has_sensors &&
        !world_prim.add_child(std::move(sensors_prim), true, &add_err)) {
      SetErr(err, "Failed to add Sensors scope: " + add_err);
      return false;
    }
    if (has_contacts &&
        !world_prim.add_child(std::move(contacts_prim), true, &add_err)) {
      SetErr(err, "Failed to add Contacts scope: " + add_err);
      return false;
    }
    if (has_custom &&
        !world_prim.add_child(std::move(custom_prim), true, &add_err)) {
      SetErr(err, "Failed to add MjcCustom scope: " + add_err);
      return false;
    }
    if (has_plugins &&
        !world_prim.add_child(std::move(plugins_prim), true, &add_err)) {
      SetErr(err, "Failed to add MjcPlugins scope: " + add_err);
      return false;
    }
  }

  if (!stage.add_root_prim(std::move(world_prim))) {
    SetErr(err, "Failed to add World root prim: " + stage.get_error());
    return false;
  }

  *out_stage = std::move(stage);
  return true;
}

bool ConvertURDFJsonToUSDStage(const std::string &robot_json, Stage *out_stage,
                               std::string *warn, std::string *err) {
  return ConvertURDFJsonToUSDStage(robot_json, nullptr, out_stage, warn, err);
}

}  // namespace tydra
}  // namespace lightusd
