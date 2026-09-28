// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
//
// next-core port of src/tydra/urdf-to-usd.cc. Every builder mirrors its
// legacy counterpart (same JSON contract, prim paths, prim/child ordering,
// schema attribute names, types, variability and applied API schemas), but
// authors next PrimSpecs directly instead of typed legacy schema objects.

#include "tydra/next/urdf-to-usd.hh"

#include <algorithm>
#include <array>
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

constexpr int32_t kMjcfDefaultCondim = 3;
constexpr double kMjcfDefaultSolmix = 1.0;
constexpr double kMjcfDefaultMargin = 0.0;
constexpr double kMjcfDefaultGap = 0.0;
constexpr int32_t kLegacyUrdfCollisionGroup = 3;
constexpr int32_t kLegacyUrdfVisualGroup = 2;
constexpr int32_t kDefaultNewtonMaxHullVertices = 64;

using NameMap = std::map<std::string, std::string>;

// ------------------------------------------------------------------
// JSON access (same semantics as the legacy converter's helpers)
// ------------------------------------------------------------------

std::string JsonString(const Json &j, const char *key,
                       const std::string &fallback = std::string()) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) {
    return fallback;
  }
  return j.at(key).get<std::string>();
}

bool ReadNumber(const Json &j, const char *key, double *out) {
  if (!out || !j.is_object() || !j.contains(key) || !j.at(key).is_number()) {
    return false;
  }
  *out = j.at(key).get<double>();
  return true;
}

bool ReadNumber(const Json &j, const char *key, float *out) {
  double v = 0.0;
  if (!ReadNumber(j, key, &v)) return false;
  *out = static_cast<float>(v);
  return true;
}

bool ReadBool(const Json &j, const char *key, bool *out) {
  if (!out || !j.is_object() || !j.contains(key)) return false;
  const Json &v = j.at(key);
  if (v.is_boolean()) {
    *out = v.get<bool>();
    return true;
  }
  if (v.is_number_integer()) {
    *out = v.get<int32_t>() != 0;
    return true;
  }
  return false;
}

const Json *JsonObject(const Json &j, const char *key) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_object()) {
    return nullptr;
  }
  return &j.at(key);
}

const Json *JsonArray(const Json &j, const char *key) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
    return nullptr;
  }
  return &j.at(key);
}

int32_t JsonInt(const Json &j, const char *key, int32_t fallback) {
  if (!j.is_object() || !j.contains(key)) return fallback;
  const Json &v = j.at(key);
  if (v.is_number_integer()) return v.get<int32_t>();
  if (v.is_number()) return static_cast<int32_t>(v.get<double>());
  return fallback;
}

// Integer from `src[object_key][key]`, falling back to `src[key]`.
bool ReadIntFrom(const Json &src, const char *object_key, const char *key,
                 int32_t *out) {
  auto read_value = [key, out](const Json &j) -> bool {
    if (!j.is_object() || !j.contains(key)) return false;
    const Json &v = j.at(key);
    if (v.is_number_integer()) {
      *out = v.get<int32_t>();
      return true;
    }
    if (v.is_number()) {
      *out = static_cast<int32_t>(v.get<double>());
      return true;
    }
    return false;
  };
  if (const Json *obj = JsonObject(src, object_key)) {
    if (read_value(*obj)) return true;
  }
  return read_value(src);
}

int32_t JsonIntFrom(const Json &src, const char *object_key, const char *key,
                    int32_t fallback) {
  if (const Json *obj = JsonObject(src, object_key)) {
    if (obj->contains(key)) return JsonInt(*obj, key, fallback);
  }
  return JsonInt(src, key, fallback);
}

bool ReadNumberFrom(const Json &src, const char *object_key, const char *key,
                    double *out) {
  if (const Json *obj = JsonObject(src, object_key)) {
    if (ReadNumber(*obj, key, out)) return true;
  }
  return ReadNumber(src, key, out);
}

double JsonNumberFrom(const Json &src, const char *object_key, const char *key,
                      double fallback) {
  double value = fallback;
  if (ReadNumberFrom(src, object_key, key, &value)) return value;
  return fallback;
}

std::vector<float> JsonFloats(const Json &j, const char *key) {
  std::vector<float> out;
  if (const Json *items = JsonArray(j, key)) {
    for (const Json &v : *items) {
      if (v.is_number()) out.push_back(static_cast<float>(v.get<double>()));
    }
  }
  return out;
}

std::vector<double> JsonDoubles(const Json &j, const char *key) {
  std::vector<double> out;
  if (const Json *items = JsonArray(j, key)) {
    for (const Json &v : *items) {
      if (v.is_number()) out.push_back(v.get<double>());
    }
  }
  return out;
}

std::vector<double> JsonDoublesFrom(const Json &src, const char *object_key,
                                    const char *key) {
  if (const Json *obj = JsonObject(src, object_key)) {
    std::vector<double> out = JsonDoubles(*obj, key);
    if (!out.empty()) return out;
  }
  return JsonDoubles(src, key);
}

std::vector<int32_t> JsonInts(const Json &j, const char *key) {
  std::vector<int32_t> out;
  if (const Json *items = JsonArray(j, key)) {
    for (const Json &v : *items) {
      if (v.is_number_integer()) out.push_back(v.get<int32_t>());
      else if (v.is_number()) out.push_back(static_cast<int32_t>(v.get<double>()));
    }
  }
  return out;
}

std::vector<std::string> JsonStrings(const Json &j, const char *key) {
  std::vector<std::string> out;
  if (const Json *items = JsonArray(j, key)) {
    for (const Json &v : *items) {
      if (v.is_string()) out.push_back(v.get<std::string>());
    }
  }
  return out;
}

// `[min, max]` pair authored as `<key>` (the legacy converter reads both
// elements unconditionally once the array has two entries).
bool ReadRange(const Json &j, const char *key, double *lo, double *hi) {
  const Json *items = JsonArray(j, key);
  if (!items || items->size() < 2) return false;
  *lo = (*items)[0].get<double>();
  *hi = (*items)[1].get<double>();
  return true;
}

// ------------------------------------------------------------------
// Names
// ------------------------------------------------------------------

std::string Sanitize(const std::string &source, const std::string &fallback) {
  std::string out;
  out.reserve(source.empty() ? fallback.size() : source.size());
  for (char c : source) {
    const unsigned char uc = static_cast<unsigned char>(c);
    out.push_back((std::isalnum(uc) || c == '_') ? c : '_');
  }
  if (out.empty()) out = fallback;
  if (!(std::isalpha(static_cast<unsigned char>(out[0])) || out[0] == '_')) {
    out.insert(out.begin(), '_');
  }
  return out;
}

// Legacy UniqueUSDIdentifier: `name`, `name_1`, `name_2`, ...
std::string Unique(const std::string &source, const std::string &fallback,
                   std::set<std::string> *used) {
  const std::string base = Sanitize(source, fallback);
  std::string name = base;
  uint32_t suffix = 1;
  while (used->count(name)) name = base + "_" + std::to_string(suffix++);
  used->insert(name);
  return name;
}

// Sibling-name collision rule of the legacy Prim::add_child(rename=true)
// (makeUniqueName): append "1" until the name is free (geom -> geom1 ->
// geom11). Builders whose names are not already unique go through this so
// the resulting prim paths match the legacy stage.
std::string ChildName(const std::string &name, std::set<std::string> *siblings) {
  std::string out = name;
  while (siblings->count(out)) out += "1";
  siblings->insert(out);
  return out;
}

void AppendWarn(std::string *warn, const std::string &message) {
  if (warn) *warn += message;
}

// ------------------------------------------------------------------
// PrimSpec authoring
// ------------------------------------------------------------------

void AddAPI(tn::PrimSpec *prim, const std::string &schema) {
  if (!prim) return;
  std::vector<std::string> &apis = prim->meta().apiSchemas();
  prim->meta().apiSchemasQualifier() = "prepend";
  if (std::find(apis.begin(), apis.end(), schema) == apis.end()) {
    apis.push_back(schema);
  }
}

void AddAPIs(tn::PrimSpec *prim,
             std::initializer_list<const char *> schemas) {
  for (const char *schema : schemas) AddAPI(prim, schema);
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

void SetDoubles(tn::PrimSpec *prim, const std::string &name,
                std::vector<double> values, bool uniform = false) {
  Set(prim, name, tn::Value::MakeDoubleArray(std::move(values)), "double[]",
      uniform);
}

// Typed attribute declared without a value (e.g. `token outputs:surface`).
void Declare(tn::PrimSpec *prim, const std::string &name, tn::TypeId type_id,
             const std::string &type_name) {
  if (!prim) return;
  const tn::PropNameId id = tn::GetPropNameTable().intern(name);
  if (!prim->property(id)) prim->add_property_slot(id, type_id, 0);
  prim->set_property_type_name(name, type_name);
}

// Attribute connection (`<type> <name>.connect = <target>`).
void Connect(tn::PrimSpec *prim, const std::string &name, tn::TypeId type_id,
             const std::string &type_name, const std::string &target) {
  if (!prim) return;
  const tn::PropNameId id = tn::GetPropNameTable().intern(name);
  if (!prim->property(id)) {
    prim->add_property_slot(id, type_id, tn::PropSlot::kFlagConnection);
  }
  prim->set_property_type_name(name, type_name);
  prim->set_connection_targets(name, {tn::Path(target)});
  tn::ArcEdit &edit = prim->ensure_connection_edit(name);
  edit = tn::ArcEdit();
  edit.authored = true;
  edit.is_explicit = true;
}

void SetRel(tn::PrimSpec *prim, const std::string &name,
            const std::vector<std::string> &targets) {
  if (!prim || targets.empty()) return;
  std::vector<tn::Path> paths;
  paths.reserve(targets.size());
  for (const std::string &target : targets) paths.emplace_back(target);
  prim->set_relationship_targets(name, std::move(paths));
}

void SetInterpolation(tn::PrimSpec *prim, const std::string &name,
                      const char *interpolation) {
  tn::PropMeta &meta = prim->ensure_property_meta(name);
  meta.interpolation = interpolation;
  meta.authored |= tn::PropMeta::kInterpolation;
}

tn::PrimSpec *Define(tn::Layer *layer, const std::string &path,
                     const std::string &type) {
  const uint32_t index = layer->define_prim_at_path(path, type);
  return index == UINT32_MAX ? nullptr : layer->prim_mutable(index);
}

using Matrix4 = std::array<double, 16>;  // row-major, translation in row 3

Matrix4 IdentityMatrix() {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

Matrix4 MatrixFromArray(const std::vector<double> &flat) {
  Matrix4 m = IdentityMatrix();
  if (flat.size() == 16) std::copy(flat.begin(), flat.end(), m.begin());
  return m;
}

void SetTransform(tn::PrimSpec *prim, const Matrix4 &m) {
  Set(prim, "xformOp:transform", tn::Value::MakeMatrix4d(m.data()),
      "matrix4d");
  Set(prim, "xformOpOrder",
      tn::Value::MakeTokenArray(std::vector<std::string>{"xformOp:transform"}),
      "token[]", true);
}

// Authors `matrix` as xformOp:transform when the JSON carries 16 values.
void SetTransformFromJson(tn::PrimSpec *prim, const Json &source) {
  const std::vector<double> matrix = JsonDoubles(source, "matrix");
  if (matrix.size() == 16) SetTransform(prim, MatrixFromArray(matrix));
}

std::string AxisFromToken(const std::string &axis) {
  if (axis == "X" || axis == "x") return "X";
  if (axis == "Y" || axis == "y") return "Y";
  return "Z";
}

// ------------------------------------------------------------------
// Inertia
// ------------------------------------------------------------------

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

// Convert a 3x3 rotation matrix (columns = orthonormal basis) into a quatf
// Value. Forces a right-handed (det +1) frame so the quaternion is
// well-formed.
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

// ------------------------------------------------------------------
// Link geometry
// ------------------------------------------------------------------

// Collider schemas + MuJoCo/Newton contact attributes (legacy
// AddCollisionAPIs).
void AddCollisionAPIs(tn::PrimSpec *prim, bool mesh_collision,
                      const Json &src, bool mjcf_source) {
  AddAPIs(prim, {"PhysicsCollisionAPI", "MjcCollisionAPI", "MjcImageableAPI",
                 "NewtonCollisionAPI"});
  if (mesh_collision) {
    AddAPIs(prim, {"PhysicsMeshCollisionAPI", "MjcMeshCollisionAPI",
                   "NewtonMeshCollisionAPI"});
  }
  Set(prim, "physics:collisionEnabled", tn::Value(true), "bool");
  if (mesh_collision) {
    // Default approximation is `convexHull`, matching mujoco-usd-converter.
    SetToken(prim, "physics:approximation",
             JsonString(src, "approximation", "convexHull"), true);
    SetToken(prim, "mjc:inertia", "legacy", true);
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
    if (ReadIntFrom(src, "mjc", "geomContype", &ivalue) ||
        ReadIntFrom(src, "mjc", "contype", &ivalue)) {
      Set(prim, "mjc:geomContype", tn::Value(ivalue), "int", true);
    }
    if (ReadIntFrom(src, "mjc", "geomConaffinity", &ivalue) ||
        ReadIntFrom(src, "mjc", "conaffinity", &ivalue)) {
      Set(prim, "mjc:geomConaffinity", tn::Value(ivalue), "int", true);
    }
    if (ReadIntFrom(src, "mjc", "priority", &ivalue)) {
      Set(prim, "mjc:priority", tn::Value(ivalue), "int", true);
    }
    if (ReadNumberFrom(src, "mjc", "solmix", &dvalue)) {
      Set(prim, "mjc:solmix", tn::Value(dvalue), "double", true);
    }
    if (ReadNumberFrom(src, "mjc", "margin", &dvalue)) {
      Set(prim, "mjc:margin", tn::Value(dvalue), "double", true);
      Set(prim, "newton:contactMargin",
          tn::Value(static_cast<float>(dvalue)), "float");
    } else if (ReadNumberFrom(src, "newton", "contactMargin", &dvalue)) {
      Set(prim, "newton:contactMargin",
          tn::Value(static_cast<float>(dvalue)), "float");
    }
    if (ReadNumberFrom(src, "mjc", "gap", &dvalue)) {
      Set(prim, "mjc:gap", tn::Value(dvalue), "double", true);
      Set(prim, "newton:contactGap", tn::Value(static_cast<float>(dvalue)),
          "float");
    } else if (ReadNumberFrom(src, "newton", "contactGap", &dvalue)) {
      Set(prim, "newton:contactGap", tn::Value(static_cast<float>(dvalue)),
          "float");
    }
    std::vector<double> values = JsonDoublesFrom(src, "mjc", "geomFriction");
    if (values.empty()) values = JsonDoublesFrom(src, "mjc", "friction");
    if (!values.empty()) SetDoubles(prim, "mjc:geomFriction", values, true);
    values = JsonDoublesFrom(src, "mjc", "solref");
    if (!values.empty()) SetDoubles(prim, "mjc:solref", values, true);
    values = JsonDoublesFrom(src, "mjc", "solimp");
    if (!values.empty()) SetDoubles(prim, "mjc:solimp", values, true);
    values = JsonDoublesFrom(src, "mjc", "geomSize");
    if (!values.empty()) SetDoubles(prim, "mjc:geomSize", values, true);
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
    Set(prim, "mjc:margin", tn::Value(margin), "double", true);
    Set(prim, "newton:contactMargin",
        tn::Value(static_cast<float>(
            JsonNumberFrom(src, "newton", "contactMargin", margin))),
        "float");
    Set(prim, "newton:contactGap",
        tn::Value(static_cast<float>(
            JsonNumberFrom(src, "newton", "contactGap", kMjcfDefaultGap))),
        "float");
  }
  if (mesh_collision) {
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
    return static_cast<int32_t>(value);
  };
  Set(prim, "mjc:contype", tn::Value(contact_bits("contype")), "int", true);
  Set(prim, "mjc:conaffinity", tn::Value(contact_bits("conaffinity")), "int",
      true);
  // `purpose=guide` hides the collider from default renders but keeps it
  // discoverable to schema-aware consumers (mujoco-usd-converter applies
  // the same to non-visual MuJoCo geom groups).
  SetToken(prim, "purpose", "guide", true);
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
  if (indices.size() % 3 != 0) {
    AppendWarn(warn, "Skipping mesh `" + fallback_name +
                         "`: indices must be triangles.\n");
    return true;
  }
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
  SetToken(prim, "subdivisionScheme", "none", true);
  Set(prim, "points",
      tn::Value::MakeFloatCompArray(std::vector<float>(*positions),
                                    tn::TypeId::Point3f, 3),
      "point3f[]");
  Set(prim, "faceVertexCounts",
      tn::Value::MakeIntArray(std::vector<int32_t>(indices.size() / 3, 3)),
      "int[]");
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
  Set(prim, "faceVertexIndices", tn::Value::MakeIntArray(std::move(indices)),
      "int[]");
  SetTransformFromJson(prim, mesh_json);

  if (collision) {
    AddCollisionAPIs(prim, true, mesh_json, mjcf_source);
    return true;
  }
  AddAPI(prim, "MjcImageableAPI");
  // Bind a UsdShade material (MJCF <geom material=..> ->
  // /World/Materials/<m>); MJCF material names are unique, so the sanitized
  // path is deterministic and the material prim is emitted later.
  const std::string material = JsonString(mesh_json, "material");
  if (!material.empty()) {
    AddAPI(prim, "MaterialBindingAPI");
    SetRel(prim, "material:binding",
           {"/World/Materials/" + Sanitize(material, "material")});
  }
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
                                     std::string *err) {
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
  AddCollisionAPIs(prim, false, shape_json, mjcf_source);
  return true;
}

// ------------------------------------------------------------------
// Joints
// ------------------------------------------------------------------

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

// Joint frame, enable flags, MjcJointAPI dynamics and Newton mimic (legacy
// AssignJointBase).
void AssignJointBase(tn::PrimSpec *prim, const Json &joint,
                     const std::string &parent, const std::string &child,
                     const NameMap &joint_names) {
  SetRel(prim, "physics:body0", {"/World/Links/" + parent});
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

  // revolute -> physxLimit:angular:*, prismatic -> physxLimit:linear:*; other
  // joint types keep the mjc:* attributes only (no PhysX analog).
  const std::string subtype = JsonString(joint, "type");
  const bool is_revolute = subtype == "revolute";
  const bool is_prismatic = subtype == "prismatic";
  const std::string limit_ns =
      is_prismatic ? "physxLimit:linear:" : "physxLimit:angular:";
  if (const Json *dyn = JsonObject(joint, "dynamics")) {
    // damping / frictionloss are MjcJointAPI schema attributes (uniform);
    // stiffness / armature are plain mjc:* extension attributes.
    double value = 0.0;
    if (ReadNumber(*dyn, "damping", &value)) {
      Set(prim, "mjc:damping", tn::Value(value), "double", true);
      if (is_revolute || is_prismatic) {
        Set(prim, limit_ns + "damping", tn::Value(value), "double");
      }
    }
    if (ReadNumber(*dyn, "friction", &value)) {
      Set(prim, "mjc:frictionloss", tn::Value(value), "double", true);
      Set(prim, "physxJoint:jointFriction", tn::Value(value), "double");
    }
    if (ReadNumber(*dyn, "stiffness", &value)) {
      Set(prim, "mjc:stiffness", tn::Value(value), "double");
      if (is_revolute || is_prismatic) {
        Set(prim, limit_ns + "stiffness", tn::Value(value), "double");
      }
    }
    if (ReadNumber(*dyn, "armature", &value)) {
      Set(prim, "mjc:armature", tn::Value(value), "double");
      Set(prim, "physxJoint:armature", tn::Value(value), "double");
    }
  }
  // Initial joint configuration as the Newton state:*:physics:position
  // attribute (revolute in degrees, prismatic in meters).
  double init_q = 0.0;
  if (ReadNumber(joint, "initPosition", &init_q) && init_q != 0.0) {
    if (is_revolute) {
      Set(prim, "state:angular:physics:position",
          tn::Value(init_q * 180.0 / 3.14159265358979323846), "double");
    } else if (is_prismatic) {
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
      Set(prim, "newton:mimicEnabled", tn::Value(enabled), "bool", true);
      SetRel(prim, "newton:mimicJoint", {"/World/Joints/" + found->second});
      Set(prim, "newton:mimicCoef0", tn::Value(offset), "float", true);
      Set(prim, "newton:mimicCoef1", tn::Value(multiplier), "float", true);
      AddAPI(prim, "NewtonMimicAPI");
    }
  }
}

// ------------------------------------------------------------------
// Physics scene
// ------------------------------------------------------------------

// Populate the MjcSceneAPI from a payload `mjcScene` block carrying the
// MuJoCo <option>/<option><flag>/<compiler> attributes (MJCF attr names).
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
          "invdiscrete", "multiccd"}) {
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
  Set(scene, "physics:gravityMagnitude", tn::Value(9.80665f), "float", true);
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
  }
  Set(scene, "newton:timeStepsPerSecond", tn::Value(steps_per_second), "int",
      true);
  Set(scene, "newton:maxSolverIterations", tn::Value(max_solver_iterations),
      "int", true);
  Set(scene, "newton:gravityEnabled", tn::Value(gravity_enabled), "bool", true);
}

// ------------------------------------------------------------------
// Scoped MJCF/Newton entities under /World/<Scope>
// ------------------------------------------------------------------

// Lazily defines /World/<name> before its first child, so empty scopes are
// omitted and the scope order follows the authoring order (legacy adds a
// scope only when it received children).
class Scope {
 public:
  Scope(tn::Layer *layer, const std::string &name)
      : layer_(layer), path_("/World/" + name) {}

  // `name` may be a relative path below an already-added child.
  tn::PrimSpec *Add(const std::string &name, const std::string &type) {
    if (!defined_) {
      Define(layer_, path_, "Xform");
      defined_ = true;
    }
    return Define(layer_, path_ + "/" + name, type);
  }

 private:
  tn::Layer *layer_;
  std::string path_;
  bool defined_{false};
};

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

// MJCF <site> -> Sphere marker under /World/Sites carrying MjcSiteAPI and a
// baked world transform (the routing points of spatial/muscle tendons).
// `name` is the unique USD name resolved up front (see site_names).
void AddMjcSiteFromJson(Scope *scope, const Json &site,
                        const std::string &name) {
  tn::PrimSpec *prim = scope->Add(name, "Sphere");
  if (!prim) return;
  double radius = 0.005;
  ReadNumber(site, "size", &radius);
  if (radius <= 0.0) radius = 0.005;
  Set(prim, "radius", tn::Value(radius), "double");
  SetTransformFromJson(prim, site);
  AddAPI(prim, "MjcSiteAPI");
  int32_t group = 0;
  if (ReadIntFrom(site, "", "group", &group)) {
    Set(prim, "mjc:group", tn::Value(group), "int", true);
  }
  // Sites are markers, not colliders / visuals by default (MuJoCo group>=3).
  SetToken(prim, "purpose", "guide", true);
}

// MJCF <tendon> -> MjcTendon. Fixed tendons route through joints
// (`joints:[{joint,coef}]`), spatial tendons through sites
// (`path:[{site|sidesite}]`); the richer physics-to-json shape carries
// absolute/source-name `route` / `sideSites` arrays plus typed route arrays.
void AddMjcTendonFromJson(Scope *scope, std::set<std::string> *siblings,
                          const Json &t, const NameMap &joint_names,
                          const NameMap &site_names, NameMap *tendon_names,
                          size_t index, std::string *warn) {
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
  (*tendon_names)[source] = name;
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
    if (type == "spatial") return resolve_in(site_names, "/World/Sites/", ref, out);
    return resolve_in(joint_names, "/World/Joints/", ref, out);
  };
  auto resolve_site_ref = [&](const std::string &ref, std::string *out) {
    if (ref.empty()) return false;
    if (ref[0] == '/') {
      *out = ref;
      return true;
    }
    return resolve_in(site_names, "/World/Sites/", ref, out);
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
    // Spatial (muscle) tendon: ordered site/sidesite waypoints -> mjc:path
    // (wrap geoms approximated by their sidesite), one coef per site.
    if (path_items) {
      for (const Json &wp : *path_items) {
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

  tn::PrimSpec *prim = scope->Add(ChildName(name, siblings), "MjcTendon");
  if (!prim) return;
  SetToken(prim, "mjc:type", type, true);
  SetRel(prim, "mjc:path", targets);
  std::vector<double> route_coef = JsonDoubles(t, "routeCoef");
  if (!route_coef.empty()) coefs = std::move(route_coef);
  if (!coefs.empty()) SetDoubles(prim, "mjc:path:coef", coefs, true);

  std::vector<std::string> side_sites;
  for (const std::string &ref : JsonStrings(t, "sideSites")) {
    std::string resolved;
    if (resolve_site_ref(ref, &resolved)) {
      side_sites.push_back(resolved);
    } else {
      AppendWarn(warn, "Tendon `" + name + "` sideSite reference `" + ref +
                           "` could not be resolved; skipping it.\n");
    }
  }
  const Json *side_items = JsonArray(t, "sideSites");
  if (side_sites.empty() && side_items) {
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
  SetRel(prim, "mjc:sideSites", side_sites);

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
  std::vector<double> divisors = JsonDoubles(t, "routeDivisors");
  if (!divisors.empty()) SetDoubles(prim, "mjc:path:divisors", divisors, true);

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
    SetToken(prim, "mjc:limited", "true", true);
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

// MJCF <equality> -> Xform host carrying the matching MjcEquality*API plus
// generic mjc:* attributes. connect/weld relate two bodies, joint two joints.
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

// MJCF <general>/<muscle> (and tendon/site/body-targeted) actuators ->
// MjcActuator with the gain/bias/lengthrange parameters; mjc:target resolves
// to the driven tendon, joint, site or body.
void AddMjcActuatorFromJson(Scope *scope, std::set<std::string> *used,
                            const Json &a, const NameMap &joint_names,
                            const NameMap &tendon_names,
                            const NameMap &site_names,
                            const NameMap &link_names, size_t index) {
  const std::string name = Unique(
      JsonString(a, "name", "actuator_" + std::to_string(index)), "actuator",
      used);
  tn::PrimSpec *prim = scope->Add(name, "MjcActuator");
  if (!prim) return;

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
    targets = resolve(site_names, "/World/Sites/", t_site);
  } else if (!t_body.empty()) {  // <adhesion body=..>
    targets = resolve(link_names, "/World/Links/", t_body);
  }
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
                          : resolve(site_names, "/World/Sites/", site));
  }
}

// MJCF <keyframe><key> -> MjcKeyframe (qpos/qvel/act/ctrl/mpos/mquat).
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

// MJCF <sensor> child -> MjcSensor. The kind is `mjc:type`, the measured
// object `mjc:objtype`/`mjc:objname` (+ reftype/refname for frame sensors);
// MuJoCo attribute aliases (`site`, `joint`, `refsite`, ...) are accepted.
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

// MJCF <contact><pair> -> Xform host under /World/Contacts carrying the pair
// geoms + collision params as generic mjc:* attributes.
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

// Row-vector local->world transform for a light whose emission axis (USD
// light -Z) points along the world-space direction of `local_dir`,
// positioned at `world`'s translation. `world` is the baked body*light frame
// (row-major, translation in row 3); `local_dir` is the MJCF <light dir>.
Matrix4 LightTransformMatrix(const Matrix4 &world,
                             const std::array<double, 3> &local_dir) {
  double wd[3];
  for (int j = 0; j < 3; j++) {
    wd[j] = local_dir[0] * world[0 * 4 + j] + local_dir[1] * world[1 * 4 + j] +
            local_dir[2] * world[2 * 4 + j];
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
  Matrix4 m = IdentityMatrix();
  for (int j = 0; j < 3; j++) {
    m[0 * 4 + j] = x[j];
    m[1 * 4 + j] = y[j];
    m[2 * 4 + j] = z[j];
    m[3 * 4 + j] = world[3 * 4 + j];  // world translation
  }
  return m;
}

// MJCF <light> -> UsdLux: directional -> DistantLight, point/spot ->
// SphereLight (spot gets a shaping cone). Color from <light diffuse>.
void AddLightFromJson(Scope *scope, std::set<std::string> *used,
                      const Json &l, size_t index) {
  const std::string name = Unique(
      JsonString(l, "name", "light_" + std::to_string(index)), "light", used);
  const std::string type = JsonString(l, "type", "spot");
  std::array<double, 3> dir{0, 0, -1};
  const std::vector<double> d = JsonDoubles(l, "dir");
  if (d.size() >= 3) dir = {d[0], d[1], d[2]};
  const Matrix4 xform =
      LightTransformMatrix(MatrixFromArray(JsonDoubles(l, "matrix")), dir);
  std::vector<float> color = JsonFloats(l, "color");
  if (color.size() < 3) color = {1.0f, 1.0f, 1.0f};
  bool castshadow = true;
  ReadBool(l, "castshadow", &castshadow);

  tn::PrimSpec *prim =
      scope->Add(name, type == "directional" ? "DistantLight" : "SphereLight");
  if (!prim) return;
  if (type != "directional") {
    Set(prim, "inputs:radius", tn::Value(0.02f), "float");  // near-point
  }
  Set(prim, "inputs:color", tn::Value::MakeColor3f(color[0], color[1], color[2]),
      "color3f");
  Set(prim, "inputs:shadow:enable", tn::Value(castshadow), "bool");
  if (type == "spot") {
    double cutoff = 45.0;
    ReadNumber(l, "cutoff", &cutoff);
    Set(prim, "inputs:shaping:cone:angle",
        tn::Value(static_cast<float>(cutoff)), "float");
  }
  SetTransform(prim, xform);
}

// MJCF <camera> -> UsdGeomCamera. fovy (vertical, degrees) -> apertures for
// the default 50mm focal length; orthographic projection honored.
void AddCameraFromJson(Scope *scope, std::set<std::string> *used,
                       const Json &c, size_t index) {
  const std::string name = Unique(
      JsonString(c, "name", "camera_" + std::to_string(index)), "camera", used);
  tn::PrimSpec *prim = scope->Add(name, "Camera");
  if (!prim) return;
  SetTransform(prim, MatrixFromArray(JsonDoubles(c, "matrix")));
  double fovy = 45.0;
  if (ReadNumber(c, "fovy", &fovy) && fovy > 0.0 && fovy < 180.0) {
    // verticalAperture = 2 * focalLength * tan(fovy/2), square sensor.
    const double f = 50.0;
    const double va =
        2.0 * f * std::tan(fovy * 0.5 * 3.14159265358979323846 / 180.0);
    Set(prim, "focalLength", tn::Value(static_cast<float>(f)), "float");
    Set(prim, "verticalAperture", tn::Value(static_cast<float>(va)), "float");
    Set(prim, "horizontalAperture", tn::Value(static_cast<float>(va)), "float");
  }
  bool ortho = false;
  if (ReadBool(c, "orthographic", &ortho) && ortho) {
    SetToken(prim, "projection", "orthographic");
  }
}

// MJCF <asset><material> -> UsdShade Material + UsdPreviewSurface under
// /World/Materials (rgba/metallic/roughness/emission). A diffuse texture
// feeds diffuseColor through UsdUVTexture <- UsdPrimvarReader_float2(st).
void AddMaterialFromJson(Scope *scope, std::set<std::string> *used,
                         const Json &m, size_t index) {
  const std::string name = Sanitize(
      JsonString(m, "name", "material_" + std::to_string(index)), "material");
  if (!used->insert(name).second) return;  // MJCF names are unique
  const std::string mat_path = "/World/Materials/" + name;
  tn::PrimSpec *mat = scope->Add(name, "Material");
  if (!mat) return;
  Connect(mat, "outputs:surface", tn::TypeId::Token, "token",
          mat_path + "/PreviewSurface.outputs:surface");

  const std::vector<float> rgba = JsonFloats(m, "rgba");
  const std::string tex_file = JsonString(m, "texture");
  tn::PrimSpec *ps = scope->Add(name + "/PreviewSurface", "Shader");
  if (!ps) return;
  SetToken(ps, "info:id", "UsdPreviewSurface", true);
  float base[3] = {0.8f, 0.8f, 0.8f};
  if (rgba.size() >= 3) {
    base[0] = rgba[0];
    base[1] = rgba[1];
    base[2] = rgba[2];
  }
  if (!tex_file.empty()) {
    Connect(ps, "inputs:diffuseColor", tn::TypeId::Color3f, "color3f",
            mat_path + "/DiffuseTexture.outputs:rgb");
  } else if (rgba.size() >= 3) {
    Set(ps, "inputs:diffuseColor",
        tn::Value::MakeColor3f(base[0], base[1], base[2]), "color3f");
  }
  if (rgba.size() >= 4) Set(ps, "inputs:opacity", tn::Value(rgba[3]), "float");
  for (const char *key : {"metallic", "roughness"}) {
    double v = 0.0;
    if (ReadNumber(m, key, &v)) {
      Set(ps, std::string("inputs:") + key, tn::Value(static_cast<float>(v)),
          "float");
    }
  }
  double emission = 0.0;
  if (ReadNumber(m, "emission", &emission) && emission > 0.0) {
    const float e = static_cast<float>(emission);
    Set(ps, "inputs:emissiveColor",
        tn::Value::MakeColor3f(base[0] * e, base[1] * e, base[2] * e),
        "color3f");
  }
  Declare(ps, "outputs:surface", tn::TypeId::Token, "token");
  if (tex_file.empty()) return;

  tn::PrimSpec *reader = scope->Add(name + "/stReader", "Shader");
  if (!reader) return;
  SetToken(reader, "info:id", "UsdPrimvarReader_float2", true);
  Set(reader, "inputs:varname", tn::Value(std::string("st")), "string");
  Declare(reader, "outputs:result", tn::TypeId::Float2, "float2");

  tn::PrimSpec *tex = scope->Add(name + "/DiffuseTexture", "Shader");
  if (!tex) return;
  SetToken(tex, "info:id", "UsdUVTexture", true);
  Set(tex, "inputs:file", tn::Value::MakeAssetPath(tex_file), "asset");
  Connect(tex, "inputs:st", tn::TypeId::Texcoord2f, "texCoord2f",
          mat_path + "/stReader.outputs:result");
  SetToken(tex, "inputs:wrapS", "repeat");
  SetToken(tex, "inputs:wrapT", "repeat");
  SetToken(tex, "inputs:sourceColorSpace", "sRGB");
  Declare(tex, "outputs:rgb", tn::TypeId::Float3, "float3");
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
  const bool mjcf_source = payload.mjcf_source;

  tn::Layer layer;
  layer.meta().defaultPrim = "World";
  const std::string up_axis = JsonString(root, "upAxis", "Y");
  const bool y_up = !(up_axis == "Z" || up_axis == "z");
  layer.meta().upAxis = y_up ? "Y" : "Z";
  layer.meta().upAxis_set = true;
  // MuJoCo authors in SI units (1 stage unit = 1 m / 1 kg).
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
  // URDF/MJCF sources are Z-up; a Y-up stage gets a single corrective root
  // rotation Rx(-90deg) so local-frame physics data rides along unchanged
  // (doc/usd-physics-upAxis.md).
  if (y_up) {
    Set(world, "xformOp:rotateX", tn::Value(-90.0), "double");
    Set(world, "xformOpOrder",
        tn::Value::MakeTokenArray(std::vector<std::string>{"xformOp:rotateX"}),
        "token[]", true);
  }

  AddPhysicsScene(&layer, root);
  Define(&layer, "/World/Links", "Xform");
  Define(&layer, "/World/Joints", "Xform");

  // --- Links ---------------------------------------------------------------
  std::set<std::string> child_links;
  for (const Json &joint : joints) {
    const std::string child = JsonString(joint, "child");
    if (!child.empty()) child_links.insert(child);
  }
  bool self_collision_enabled = true;
  if (const Json *newton = JsonObject(root, "newton")) {
    ReadBool(*newton, "selfCollisionEnabled", &self_collision_enabled);
  }
  NameMap link_names;
  std::set<std::string> used_links;
  for (size_t i = 0; i < links.size(); ++i) {
    const Json &link = links[i];
    const std::string source =
        JsonString(link, "name", "link_" + std::to_string(i));
    const std::string name = Unique(source, "link", &used_links);
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
    AddAPIs(prim, {"PhysicsRigidBodyAPI", "PhysicsMassAPI"});
    Set(prim, "physics:rigidBodyEnabled", tn::Value(!is_static), "bool");
    Set(prim, "physics:startsAsleep", tn::Value(false), "bool");
    bool flag = false;
    if (ReadBool(link, "mocap", &flag) && flag) {
      Set(prim, "mjc:mocap", tn::Value(true), "bool", true);
    }
    // MuJoCo <freejoint>: distinguishes a floating-base articulation root
    // from a fixed (anchored) parentless base.
    flag = false;
    ReadBool(link, "floating", &flag);
    if (flag) Set(prim, "mjc:freeJoint", tn::Value(true), "bool", true);
    if (!is_static && !child_links.count(source)) {
      AddAPIs(prim, {"PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"});
      Set(prim, "newton:selfCollisionEnabled",
          tn::Value(self_collision_enabled), "bool");
    }
    if (const Json *inertial = JsonObject(link, "inertial")) {
      float mass = 0.0f;
      if (ReadNumber(*inertial, "mass", &mass) && mass > 0.0f) {
        Set(prim, "physics:mass", tn::Value(mass), "float");
      }
      const std::vector<float> com = JsonFloats(*inertial, "centerOfMass");
      if (com.size() >= 3) SetPoint3(prim, "physics:centerOfMass", com, false);
      // A full inertia tensor is diagonalized into principal moments +
      // principal-axes rotation (the lossless USD representation).
      const std::vector<double> full = JsonDoubles(*inertial, "fullInertia");
      if (full.size() >= 6) {
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
    }

    // Visuals and collisions share the link's child namespace.
    std::set<std::string> siblings;
    if (const Json *visuals = JsonArray(link, "visuals")) {
      size_t n = 0;
      for (const Json &visual : *visuals) {
        if (!AddMeshFromJson(&layer, path, &siblings, visual, mesh_buffers,
                             "visual_" + std::to_string(n++), false,
                             mjcf_source, warn, err)) {
          return false;
        }
      }
    }
    if (const Json *collisions = JsonArray(link, "collisions")) {
      size_t n = 0;
      for (const Json &collision : *collisions) {
        const std::string fallback = "collision_" + std::to_string(n++);
        const bool ok =
            JsonObject(collision, "shape")
                ? AddNativeCollisionShapeFromJson(&layer, path, &siblings,
                                                  collision, fallback,
                                                  mjcf_source, warn, err)
                : AddMeshFromJson(&layer, path, &siblings, collision,
                                  mesh_buffers, fallback, true, mjcf_source,
                                  warn, err);
        if (!ok) return false;
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
      filtered[body1].push_back("/World/Links/" + it2->second);
    }
    for (const auto &entry : filtered) {
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
    } else if (type != "fixed") {
      AppendWarn(warn, "Joint `" + name + "` type `" + type +
                           "` is exported as PhysicsFixedJoint.\n");
    }
    tn::PrimSpec *prim = Define(&layer, "/World/Joints/" + name, usd_type);
    if (!prim) {
      if (err) *err = "Failed to add joint `" + name + "`";
      return false;
    }
    AssignJointBase(prim, joint, link_names[parent], link_names[child],
                    joint_names);
    if (usd_type != "PhysicsFixedJoint") {
      SetToken(prim, "physics:axis", axis, true);
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
      const std::string dof = JointDofName(axis, revolute_limit);
      AddAPI(prim, "PhysicsLimitAPI:" + dof);
      Set(prim, "physics:limit:" + dof + ":low",
          tn::Value(static_cast<float>(lower * scale)), "float");
      Set(prim, "physics:limit:" + dof + ":high",
          tn::Value(static_cast<float>(upper * scale)), "float");
    }
  }

  // --- MJCF / Newton scopes, in the legacy /World child order ---------------
  Scope actuators_scope(&layer, "Actuators");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("actuators");
    for (size_t i = 0; i < items.size(); ++i) {
      AddNewtonActuatorFromJson(&actuators_scope, &siblings, items[i],
                                joint_names, i, warn);
    }
  }

  // Site names are resolved up front: spatial tendons route through them
  // although the Sites scope follows Tendons/Equalities.
  const Json &sites = payload.Array("sites");
  NameMap site_names;
  std::vector<std::string> site_usd_names(sites.size());
  {
    std::set<std::string> used;
    for (size_t i = 0; i < sites.size(); ++i) {
      const std::string source =
          JsonString(sites[i], "name", "site_" + std::to_string(i));
      site_usd_names[i] = Unique(source, "site", &used);
      site_names[source] = site_usd_names[i];
    }
  }

  NameMap tendon_names;
  Scope tendons_scope(&layer, "Tendons");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("tendons");
    for (size_t i = 0; i < items.size(); ++i) {
      AddMjcTendonFromJson(&tendons_scope, &siblings, items[i], joint_names,
                           site_names, &tendon_names, i, warn);
    }
  }

  Scope equalities_scope(&layer, "Equalities");
  {
    std::set<std::string> siblings;
    const Json &items = payload.Array("equalities");
    for (size_t i = 0; i < items.size(); ++i) {
      AddMjcEqualityFromJson(&equalities_scope, &siblings, items[i],
                             link_names, joint_names, i);
    }
  }

  Scope sites_scope(&layer, "Sites");
  for (size_t i = 0; i < sites.size(); ++i) {
    AddMjcSiteFromJson(&sites_scope, sites[i], site_usd_names[i]);
  }

  Scope mjc_actuators_scope(&layer, "MjcActuators");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("mjcActuators");
    for (size_t i = 0; i < items.size(); ++i) {
      AddMjcActuatorFromJson(&mjc_actuators_scope, &used, items[i],
                             joint_names, tendon_names, site_names, link_names,
                             i);
    }
  }

  Scope keyframes_scope(&layer, "Keyframes");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("keyframes");
    for (size_t i = 0; i < items.size(); ++i) {
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
    for (size_t i = 0; i < items.size(); ++i) {
      AddMjcSensorFromJson(&sensors_scope, &used, items[i], i);
    }
  }

  Scope contacts_scope(&layer, "Contacts");
  {
    std::set<std::string> used;
    const Json &items = payload.Array("contactPairs");
    for (size_t i = 0; i < items.size(); ++i) {
      AddContactPairFromJson(&contacts_scope, &used, items[i], i);
    }
  }

  // MJCF <custom><numeric|text> -> /World/MjcCustom (model metadata / MJX
  // knobs), authored only when it carries at least one entry.
  if (const Json *custom = JsonObject(root, "custom")) {
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
  if (const Json *plugins = JsonArray(root, "plugins")) {
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

bool ConvertURDFJsonToUSDStage(const std::string &robot_json,
                               ::lightusd::next::Stage *out_stage,
                               std::string *warn, std::string *err) {
  return ConvertURDFJsonToUSDStage(robot_json, nullptr, out_stage, warn, err);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
