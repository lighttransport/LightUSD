// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
#include "urdf-to-usd-internal.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <set>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {

std::string JsonString(const Json &j, const char *key,
                       const std::string &fallback) {
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

void SetSourceDisplayName(tn::PrimSpec *prim, const std::string &source) {
  if (!prim || source.empty() || ActiveProfile().legacy || source == prim->name()) return;
  prim->meta().displayName() = source;
  prim->meta().setDisplayNameAuthored(true);
}

// URDF elements/attributes without a USD mapping (urdfCustom payload
// members) -> urdf:<attribute> strings, urdf:text and child Scopes, as
// urdf-usd-converter stores "undefined" elements.
void AddURDFCustom(tn::Layer *layer, const std::string &path, tn::PrimSpec *prim,
                          const Json *custom, std::set<std::string> *siblings) {
  if (!prim || !custom || !custom->is_object()) return;
  auto attribute_name = [](const std::string &key) {
    std::string out = "urdf:";
    for (char c : key)
      out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' ? c : '_');
    return out;
  };
  // Depth-first with an explicit stack (the JSON bridge takes caller nesting
  // as is): a child Scope is defined and filled before its next sibling, so
  // prims are defined in the recursive order. A deque keeps frames in place
  // while children are pushed.
  struct Frame {
    std::string path;
    tn::PrimSpec *prim;
    const Json *custom;
    std::set<std::string> own_names;
    std::set<std::string> *names;  // child names in use
    size_t next = 0;
  };
  std::deque<Frame> stack;
  stack.push_back({path, prim, custom, {}, siblings});
  bool fresh = true;
  while (!stack.empty()) {
    Frame &f = stack.back();
    if (fresh) {
      fresh = false;
      if (const Json *attributes = JsonObject(*f.custom, "attributes"))
        if (attributes->object_items())
          for (const auto &member : *attributes->object_items())
            if (member.value().is_string())
              Set(f.prim, attribute_name(member.key), tn::Value(member.value().get<std::string>()),
                  "string");
      const std::string text = JsonString(*f.custom, "text");
      if (!text.empty()) Set(f.prim, "urdf:text", tn::Value(text), "string");
    }
    const Json *elements = JsonArray(*f.custom, "elements");
    if (!elements || f.next >= elements->size()) {
      stack.pop_back();
      continue;
    }
    const Json &element = (*elements)[f.next++];
    const std::string tag = JsonString(element, "tag", "element");
    const std::string name = ChildName(Sanitize(tag, "element"), f.names);
    const std::string child_path = f.path + "/" + name;
    tn::PrimSpec *child = Define(layer, child_path, "Scope");
    if (!child) continue;
    SetSourceDisplayName(child, tag);
    if (!element.is_object()) continue;
    stack.push_back({child_path, child, &element, {}, nullptr});
    stack.back().names = &stack.back().own_names;
    fresh = true;
  }
}


void AddAPI(tn::PrimSpec *prim, const std::string &schema) {
  if (!prim) return;
  if (!ActiveProfile().engine && IsEngineSchema(schema)) return;
  std::vector<std::string> &apis = prim->meta().apiSchemas();
  prim->meta().apiSchemasQualifier() = "prepend";
  prim->meta().setApiSchemasAuthored();
  if (std::find(apis.begin(), apis.end(), schema) == apis.end()) {
    apis.push_back(schema);
  }
}

void AddAPIs(tn::PrimSpec *prim,
             std::initializer_list<const char *> schemas) {
  for (const char *schema : schemas) AddAPI(prim, schema);
}

void Set(tn::PrimSpec *prim, const std::string &name, tn::Value value,
         const std::string &type_name, bool uniform) {
  if (!prim) return;
  const Profile &profile = ActiveProfile();
  if (!profile.engine && IsEngineName(name)) return;
  // Non-legacy profiles take variability from the generated schema
  // descriptors instead of per-call-site flags.
  if (!profile.legacy)
    if (const SchemaProperty *definition = FindSchemaProperty(name))
      uniform = definition->uniform;
  uint16_t flags = uniform ? tn::PropSlot::kFlagUniform : 0;
  prim->upsert_property(name, std::move(value), flags);
  prim->set_property_type_name(name, type_name);
}

void SetToken(tn::PrimSpec *prim, const std::string &name,
              const std::string &value, bool uniform) {
  Set(prim, name, tn::Value::MakeToken(value), "token", uniform);
}

void SetDoubles(tn::PrimSpec *prim, const std::string &name,
                std::vector<double> values, bool uniform) {
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
  tn::ArcListOp &edit = prim->ensure_connection_edit(name);
  edit = tn::ArcListOp();
  edit.authored = true;
  edit.is_explicit = true;
}

void SetRel(tn::PrimSpec *prim, const std::string &name,
            const std::vector<std::string> &targets) {
  if (!prim || targets.empty()) return;
  if (!ActiveProfile().engine && IsEngineName(name)) return;
  std::vector<tn::Path> paths;
  paths.reserve(targets.size());
  for (const std::string &target : targets) paths.emplace_back(target);
  prim->set_relationship_targets(name, std::move(paths));
}

void SetInterpolation(tn::PrimSpec *prim, const std::string &name,
                      const char *interpolation) {
  if (!prim) return;
  tn::PropMeta &meta = prim->ensure_property_meta(name);
  meta.interpolation = interpolation;
  meta.authored |= tn::PropMeta::kInterpolation;
}

namespace {
thread_local bool define_capacity_exceeded = false;
}  // namespace

void ResetDefineCapacity() { define_capacity_exceeded = false; }
bool DefineCapacityExceeded() { return define_capacity_exceeded; }

tn::PrimSpec *Define(tn::Layer *layer, const std::string &path,
                     const std::string &type) {
  // Builders hold PrimSpec pointers across Define calls, so the prim vector
  // must never reallocate: ConvertPayload reserves capacity up front, and a
  // define that would outgrow it fails (the conversion reports it) instead of
  // invalidating those pointers.
  size_t missing = 0;
  for (std::string p = path; p.size() > 1 && layer->index_at_path(p) == UINT32_MAX;) {
    ++missing;
    const size_t slash = p.rfind('/');
    if (slash == std::string::npos) break;
    p.resize(slash);
  }
  if (missing > layer->prims().capacity() - layer->prim_count()) {
    define_capacity_exceeded = true;
    return nullptr;
  }
  const uint32_t index = layer->define_prim_at_path(path, type);
  return index == UINT32_MAX ? nullptr : layer->prim_mutable(index);
}


Matrix4 IdentityMatrix() {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// Row-major product a * b (USD row-vector convention: p' = p * a * b).
Matrix4 MultiplyRowMajor(const Matrix4 &a, const Matrix4 &b) {
  Matrix4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = sum;
    }
  return r;
}

// Inverse of a rigid transform [R 0; t 1]: [R^T 0; -t R^T 1].
Matrix4 InverseRigid(const Matrix4 &m) {
  Matrix4 r = IdentityMatrix();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 4 + j] = m[j * 4 + i];
  for (int j = 0; j < 3; ++j) {
    double sum = 0.0;
    for (int k = 0; k < 3; ++k) sum += m[12 + k] * r[k * 4 + j];
    r[12 + j] = -sum;
  }
  return r;
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
Scope::Scope(tn::Layer* layer, const std::string& name)
    : layer_(layer), path_("/World/" + name) {}

tn::PrimSpec* Scope::Add(const std::string& name, const std::string& type) {
  if (!defined_) {
    Define(layer_, path_, "Xform");
    defined_ = true;
  }
  return Define(layer_, path_ + "/" + name, type);
}

}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
