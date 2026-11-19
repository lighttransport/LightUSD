// SPDX-License-Identifier: Apache-2.0
#include "validation-context.hh"
#include "usd-validation-internal.hh"
#include "schema/schema-registry.hh"
#include "types/spline.hh"
#include "prim/identifier.hh"
#include "core/minijson.hh"
#include "c/lightusd-core-c.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace lightusd {
namespace {
using Json = lightusd::minijson::Value;
using namespace validation_detail;
bool Starts(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
void Incomplete(USDValidationResult* r, const std::string& rule,
                const std::string& path, const std::string& message) {
  r->complete = false;
  AddError(r, rule, path, message);
}
std::string Text(const PrimSpec& p, const std::string& name) {
  std::string value;
  GetStringLikeProperty(p, name, &value);
  return value;
}
std::string ParentPath(const std::string& p) {
  const size_t slash = p.rfind('/');
  return slash == 0 || slash == std::string::npos ? "/" : p.substr(0, slash);
}
const ValidationRegistry& Registry(const ValidationOptions& o) {
  return o.registry ? *o.registry : GetBuiltinValidationRegistry();
}
bool SameProperty(const ValidationPropertyDefinition& a,
                  const ValidationPropertyDefinition& b) {
  return a.type == b.type && a.relationship == b.relationship &&
         a.variability == b.variability && a.allowed_tokens == b.allowed_tokens;
}
bool SameSchema(const ValidationSchemaDefinition& a,
                const ValidationSchemaDefinition& b) {
  if (a.parent != b.parent || a.kind != b.kind ||
      a.property_namespace != b.property_namespace ||
      a.properties.size() != b.properties.size()) return false;
  for (const auto& p : a.properties) {
    auto i = b.properties.find(p.first);
    if (i == b.properties.end() || !SameProperty(p.second, i->second)) return false;
  }
  return true;
}
bool StringArray(const Json& j, std::vector<std::string>* values) {
  if (!j.is_array()) return false;
  for (const auto& v : j) {
    if (!v.is_string()) return false;
    values->push_back(v.get_string());
  }
  return true;
}
std::string DeclaredType(const PrimSpec& prim, const std::string& name) {
  std::string type = AttrTypeNameOf(prim, name);
  const auto* slot = prim.property(name);
  if (slot && slot->is_array() && !EndsWith(type, "[]")) type += "[]";
  return type;
}
bool ValidType(const std::string& type) {
  const std::string base = EndsWith(type, "[]") ? type.substr(0, type.size() - 2) : type;
  return base == "opaque" || GetTypeIdFromName(base.c_str()) != TypeId::Invalid;
}
bool PortMap(const Json& j, std::map<std::string, std::string>* out) {
  if (!j.is_object()) return false;
  for (const auto& pair : *j.object_items()) {
    if (!IsValidNamespacedIdentifier(pair.key) || !pair.value().is_string() ||
        !ValidType(pair.value().get_string())) return false;
    out->emplace(pair.key, pair.value().get_string());
  }
  return true;
}
}  // namespace

// Built-in data is read directly from the immutable C11 tables.
ValidationRegistry::ValidationRegistry() = default;
namespace {
std::string_view View(lu_view v) { return {v.data ? v.data : "", v.len}; }
lu_view View(const std::string& s) { return {s.data(), s.size()}; }
size_t BuiltinSchemaCount() { size_t n = 0; lu_validation_schema_count(&n); return n; }
std::optional<ValidationSchemaView> BuiltinSchema(const std::string& name) {
  size_t i; lu_validation_schema_view v;
  if (lu_validation_schema_find(View(name), &i) != LU_OK ||
      lu_validation_schema_at(i, &v) != LU_OK) return {};
  return ValidationSchemaView{View(v.name), View(v.parent), View(v.kind),
    View(v.property_namespace), View(v.source), i, nullptr};
}
ValidationShaderView BuiltinShader(size_t i) {
  lu_validation_shader_view v{}; lu_validation_shader_at(i, &v);
  return {View(v.identifier), View(v.source_type), View(v.source), i, nullptr};
}
ValidationShaderView CustomShader(const ValidationShaderDefinition& s) {
  return {s.identifier, s.source_type, s.source, 0, &s};
}
bool SameBuiltinSchema(const ValidationSchemaView& v, const ValidationSchemaDefinition& s) {
  lu_validation_schema_view row{}; lu_validation_schema_at(v.index, &row);
  if (v.parent != s.parent || v.kind != s.kind || v.property_namespace != s.property_namespace ||
      row.property_count != s.properties.size()) return false;
  for (const auto& p : s.properties) {
    size_t index; lu_validation_property_view pv{};
    if (lu_validation_property_find(v.index, View(p.first), &index) != LU_OK ||
        lu_validation_property_at(v.index, index, &pv) != LU_OK) return false;
    const auto& d = p.second;
    if (View(pv.type) != d.type || View(pv.variability) != d.variability ||
        (pv.is_relationship != 0) != d.relationship || pv.token_count != d.allowed_tokens.size()) return false;
    for (size_t t = 0; t < pv.token_count; ++t) {
      lu_view token{}; lu_validation_property_token(v.index, index, t, &token);
      if (View(token) != d.allowed_tokens[t]) return false;
    }
  }
  return true;
}
bool SameShader(const ValidationShaderView& v, const ValidationShaderDefinition& s) {
  for (bool output : {false, true}) {
    const auto& ports = output ? s.outputs : s.inputs;
    if (v.port_count(output) != ports.size()) return false;
    for (size_t i = 0; i < v.port_count(output); ++i) {
      const auto p = v.port(output, i);
      const auto it = ports.find(std::string(p.first));
      if (it == ports.end() || it->second != p.second) return false;
    }
  }
  return true;
}
} // namespace
std::optional<ValidationPropertyView> ValidationSchemaView::property(const std::string& name) const {
  if (custom) {
    const auto p = custom->properties.find(name);
    if (p == custom->properties.end()) return {};
    return ValidationPropertyView{p->second.type, p->second.variability, p->second.relationship};
  }
  size_t p; lu_validation_property_view v{};
  if (lu_validation_property_find(index, View(name), &p) != LU_OK ||
      lu_validation_property_at(index, p, &v) != LU_OK) return {};
  return ValidationPropertyView{View(v.type), View(v.variability), v.is_relationship != 0};
}
size_t ValidationShaderView::port_count(bool output) const {
  if (custom) return (output ? custom->outputs : custom->inputs).size();
  lu_validation_shader_view v{}; lu_validation_shader_at(index, &v);
  return output ? v.output_count : v.input_count;
}
std::pair<std::string_view, std::string_view> ValidationShaderView::port(bool output, size_t p) const {
  if (p >= port_count(output)) return {};
  if (custom) {
    const auto& ports = output ? custom->outputs : custom->inputs;
    auto it = ports.begin(); std::advance(it, p); return {it->first, it->second};
  }
  lu_view name{}, type{}; lu_validation_shader_port(index, output ? 1 : 0, p, &name, &type);
  return {View(name), View(type)};
}
std::optional<std::string_view> ValidationShaderView::input_type(std::string_view name) const {
  for (size_t i = 0; i < port_count(false); ++i) {
    const auto p = port(false, i); if (p.first == name) return p.second;
  }
  return {};
}
std::optional<ValidationSchemaView> ValidationRegistry::FindSchema(const std::string& name) const {
  const auto i = schemas_.find(name);
  if (i == schemas_.end()) return BuiltinSchema(name);
  const auto& s = i->second;
  return ValidationSchemaView{s.name, s.parent, s.kind, s.property_namespace, s.source, 0, &s};
}
std::optional<ValidationShaderView> ValidationRegistry::FindShader(
    const std::string& id, const std::string& source_type) const {
  const auto exact = shaders_.find({id, source_type});
  if (exact != shaders_.end()) return CustomShader(exact->second);
  size_t index;
  if (lu_validation_shader_find(View(id), View(source_type), &index) == LU_OK) return BuiltinShader(index);
  if (!source_type.empty()) return {};
  std::optional<ValidationShaderView> candidate;
  for (const auto& s : shaders_) {
    if (s.first.first != id) continue;
    if (candidate) return {};
    candidate = CustomShader(s.second);
  }
  size_t count = 0; lu_validation_shader_count(&count);
  for (size_t i = 0; i < count; ++i) {
    const auto s = BuiltinShader(i);
    if (s.identifier != id || shaders_.count({std::string(s.identifier), std::string(s.source_type)})) continue;
    if (candidate) return {};
    candidate = s;
  }
  return candidate;
}
const ValidationRegistry& GetBuiltinValidationRegistry() {
  static const ValidationRegistry registry;
  return registry;
}

expected<bool, std::string> ValidationRegistry::LoadSchemaDefinitions(
    const std::string& text) {
  Json root;
  if (!lightusd::minijson::Parse(text, &root) || !root["formatVersion"].is_number_integer() || root["formatVersion"].get_int() != 1 ||
      !root["schemas"].is_array())
    return make_unexpected(std::string("expected schema manifest formatVersion 1"));
  auto candidate = schemas_;
  std::set<std::string> names;
  for (const auto& item : root["schemas"]) {
    ValidationSchemaDefinition s;
    s.name = item["name"].get_string(); s.parent = item["inherits"].get_string();
    s.kind = item["kind"].get_string();
    s.property_namespace = item["propertyNamespacePrefix"].get_string();
    s.source = item["source"].get_string();
    if (!IsValidIdentifier(s.name) || !names.insert(s.name).second ||
        (item.contains("inherits") && !item["inherits"].is_string()) ||
        (item.contains("source") && !item["source"].is_string()) ||
        (!s.property_namespace.empty() && !IsValidNamespacedIdentifier(s.property_namespace)) ||
        (s.kind == "multipleApply" && s.property_namespace.empty()) ||
        (!s.parent.empty() && !IsValidIdentifier(s.parent)) ||
        (s.kind != "concrete" && s.kind != "abstract" && s.kind != "singleApply" &&
         s.kind != "multipleApply" && s.kind != "nonApplied") ||
        !item["properties"].is_array())
      return make_unexpected(std::string("invalid or duplicate schema definition: ") + s.name);
    for (const auto& p : item["properties"]) {
      ValidationPropertyDefinition d;
      const std::string name = p["name"].get_string();
      d.type = p["type"].get_string();
      const std::string kind = p["kind"].get_string();
      d.relationship = kind == "relationship";
      d.variability = p["variability"].get_string();
      if (!IsValidNamespacedIdentifier(name) || (kind != "attribute" && !d.relationship) ||
          (d.relationship ? d.type != "rel" : !ValidType(d.type)) ||
          (!d.variability.empty() && d.variability != "uniform" && d.variability != "varying") ||
          (p.contains("allowedTokens") && !StringArray(p["allowedTokens"], &d.allowed_tokens)) ||
          !s.properties.emplace(name, d).second)
        return make_unexpected(std::string("invalid property definition: ") + s.name + "." + name);
    }
    const auto found = candidate.find(s.name);
    if (found != candidate.end() && !SameSchema(found->second, s))
      return make_unexpected(std::string("conflicting schema definition: ") + s.name);
    const auto builtin = BuiltinSchema(s.name);
    if (builtin && !SameBuiltinSchema(*builtin, s))
      return make_unexpected(std::string("conflicting schema definition: ") + s.name);
    if (!builtin) candidate[s.name] = std::move(s);
  }
  for (const auto& pair : candidate) {
    std::set<std::string> seen;
    std::string name = pair.first;
    // AOUSD Core 1.1: a VERSIONED schema must not inherit from any schema in
    // its own family (`Cylinder_2` may not inherit `Cylinder_1`/`Cylinder`).
    std::string own_family;
    uint32_t own_version = 0;
    bool versioned = false;
    ParseSchemaFamilyAndVersion(pair.first, &own_family, &own_version, &versioned);
    while (!name.empty()) {
      if (!seen.insert(name).second)
        return make_unexpected(std::string("cyclic schema inheritance: ") + name);
      if (versioned && name != pair.first) {
        std::string family;
        uint32_t version = 0;
        bool has_version = false;
        ParseSchemaFamilyAndVersion(name, &family, &version, &has_version);
        if (family == own_family)
          return make_unexpected(std::string("versioned schema ") + pair.first +
                                 " inherits from its own schema family (" +
                                 name + ")");
      }
      auto i = candidate.find(name);
      if (i == candidate.end()) {
        if (const auto builtin = BuiltinSchema(name)) { name = builtin->parent; continue; }
        if (GetSchemaRegistry().IsKnownSchema(name)) break;
        return make_unexpected(std::string("undefined schema ancestor: ") + name);
      }
      name = i->second.parent;
    }
  }
  schemas_ = std::move(candidate);
  return true;
}

expected<bool, std::string> ValidationRegistry::LoadShaderDefinitions(
    const std::string& text) {
  Json root;
  if (!lightusd::minijson::Parse(text, &root) || !root["formatVersion"].is_number_integer() || root["formatVersion"].get_int() != 1 ||
      !root["shaders"].is_array())
    return make_unexpected(std::string("expected shader manifest formatVersion 1"));
  auto candidate = shaders_;
  std::set<std::pair<std::string, std::string>> names;
  for (const auto& item : root["shaders"]) {
    ValidationShaderDefinition s;
    s.identifier = item["identifier"].get_string();
    s.source_type = item["sourceType"].get_string(); s.source = item["source"].get_string();
    const auto key = std::make_pair(s.identifier, s.source_type);
    if (s.identifier.empty() || !names.insert(key).second ||
        (item.contains("sourceType") && !item["sourceType"].is_string()) ||
        (item.contains("source") && !item["source"].is_string()) ||
        !PortMap(item["inputs"], &s.inputs) || !PortMap(item["outputs"], &s.outputs))
      return make_unexpected(std::string("invalid or duplicate shader definition: ") + s.identifier);
    const auto old = candidate.find(key);
    if (old != candidate.end() &&
        (old->second.inputs != s.inputs || old->second.outputs != s.outputs))
      return make_unexpected(std::string("conflicting shader definition: ") + s.identifier);
    size_t index;
    const bool builtin = lu_validation_shader_find(View(s.identifier), View(s.source_type), &index) == LU_OK;
    if (builtin && !SameShader(BuiltinShader(index), s))
      return make_unexpected(std::string("conflicting shader definition: ") + s.identifier);
    if (!builtin) candidate[key] = std::move(s);
  }
  shaders_ = std::move(candidate);
  return true;
}

expected<bool, std::string> ValidationRegistry::RegisterValidator(
    ValidatorDefinition rule) {
  if (rule.id.empty() || !rule.callback)
    return make_unexpected(std::string("validator needs an id and callback"));
  size_t count = 0;
  const auto* builtins = GetValidationRuleTable(&count);
  for (size_t i = 0; i < count; ++i)
    if (rule.id == builtins[i].id)
      return make_unexpected(std::string("reserved validator id: ") + rule.id);
  for (const auto& existing : validators_)
    if (existing.id == rule.id)
      return make_unexpected(std::string("duplicate validator id: ") + rule.id);
  validators_.push_back(std::move(rule));
  return true;
}
bool ValidationRegistry::InheritsFrom(const std::string& name, const std::string& ancestor) const {
  std::string current = name;
  // Loaded and built-in schemas can both form the chain, so the walk bound is
  // the total number of distinct schema names.
  const size_t limit = schemas_.size() + BuiltinSchemaCount();
  for (size_t i = 0; i <= limit; ++i) {
    if (current == ancestor) return true;
    const auto s = FindSchema(current);
    if (!s) return GetSchemaRegistry().InheritsFrom(current, ancestor);
    if (s->parent.empty()) break;
    current = s->parent;
  }
  return false;
}
std::optional<ValidationPropertyView> ValidationRegistry::FindProperty(
    const PrimSpec& prim, const std::string& name) const {
  std::vector<std::pair<std::string, std::string>> schemas;
  // Core 1.1 "Typed Schemas": a typeName naming an abstract typed schema or a
  // non-typed (API) schema leaves the composed prim typeless.
  const auto typed = FindSchema(prim.type_name());
  if (!typed || typed->kind == "concrete") schemas.emplace_back(prim.type_name(), "");
  for (const auto& api : prim.meta().apiSchemas()) {
    const size_t colon = api.find(':');
    schemas.emplace_back(api.substr(0, colon), colon == std::string::npos ? "" : api.substr(colon + 1));
  }
  const size_t limit = schemas_.size() + BuiltinSchemaCount();
  for (const auto& applied : schemas) {
    std::string current = applied.first;
    for (size_t hops = 0; hops <= limit; ++hops) {
      const auto s = FindSchema(current);
      if (!s) break;
      std::string local = name;
      if (s->kind == "multipleApply") {
        if (applied.second.empty()) break;
        const std::string prefix = std::string(s->property_namespace) + ":" + applied.second;
        if (local == prefix) local = "__INSTANCE_NAME__";
        else if (Starts(local, prefix + ":")) local = local.substr(prefix.size() + 1);
        else break;
      }
      auto property = s->property(local);
      if (property) return property;
      current = s->parent;
    }
  }
  return {};
}
void ValidationRegistry::RunCallbacks(ValidationScope scope, const ValidationContext& context,
    const PrimSpec* prim, USDValidationResult* result) const {
  for (const auto& rule : validators_) {
    if (rule.scope != scope) continue;
    if (!context.options.validator_keywords.empty()) {
      bool matches = false;
      for (const auto& keyword : context.options.validator_keywords)
        matches |= std::find(rule.keywords.begin(), rule.keywords.end(), keyword) != rule.keywords.end();
      if (!matches) continue;
    }
    if (context.options.normative_only && rule.metadata.category != "normative") continue;
    if (!rule.schema_types.empty()) {
      if (!prim) continue;
      bool applies = false;
      for (const auto& type : rule.schema_types) {
        applies |= InheritsFrom(prim->type_name(), type);
        for (const auto& api : prim->meta().apiSchemas())
          applies |= InheritsFrom(api.substr(0, api.find(':')), type);
      }
      if (!applies) continue;
    }
    auto callback_result = rule.callback(context, prim);
    if (!callback_result) {
      Incomplete(result, "checker.coverage.callback", prim ? prim->path().str() : "<layer>",
                 rule.id + ": " + callback_result.error());
      continue;
    }
    for (auto& issue : callback_result->issues) {
      if (issue.rule_id.empty()) issue.rule_id = rule.id;
      issue.source_asset = context.source_asset;
      issue.variants = context.variants;
      issue.has_time = context.time.is_numeric();
      issue.time = context.time.numeric_time();
    }
    MergeValidationResults(result, *callback_result);
  }
}

void ValidateRegisteredProperties(const Layer& layer, const ValidationOptions& options,
                                  USDValidationResult* result) {
  const auto& registry = Registry(options);
  for (const auto& prim : layer.prims()) {
    if (options.require_complete && !prim.type_name().empty() &&
        !registry.FindSchema(prim.type_name()) && !GetSchemaRegistry().IsKnownSchema(prim.type_name()))
      Incomplete(result, "checker.coverage.schema", prim.path().str(),
                 "No schema definition for " + prim.type_name());
    for (const auto& api : prim.meta().apiSchemas()) {
      const std::string name = api.substr(0, api.find(':'));
      if (options.require_complete && !registry.FindSchema(name) && !GetSchemaRegistry().IsKnownSchema(name))
        Incomplete(result, "checker.coverage.schema", prim.path().str(), "No API schema definition for " + name);
    }
    if (!options.core) continue;
    for (const auto& name : prim.relationship_names()) {
      const auto definition = registry.FindProperty(prim, name);
      if (definition && !definition->relationship)
        AddWarning(result, "core.schema.propertyKind", prim.path().str() + "." + name,
                   "Schema requires an attribute, authored a relationship");
    }
    for (const auto& slot : prim.properties().slots()) {
      const std::string name(GetPropNameTable().get(slot.name_id));
      const auto d = registry.FindProperty(prim, name);
      // Supplemental implemented schema definitions not in the pinned manifest.
      const auto* builtin = d ? nullptr : GetSchemaRegistry().FindProperty(prim, name);
      if (!d && !builtin) continue;
      const std::string expected = d ? std::string(d->type) : builtin->type_name;
      const bool rel = d ? d->relationship : expected == "rel";
      if (rel != slot.is_relationship()) {
        AddWarning(result, "core.schema.propertyKind", prim.path().str() + "." + name,
                   "Schema requires " + std::string(rel ? "a relationship" : "an attribute"));
      } else if (!rel) {
        const std::string actual = DeclaredType(prim, name);
        if (!actual.empty() && actual != expected)
          AddWarning(result, "core.schema.attributeType", prim.path().str() + "." + name,
                     "Schema requires '" + expected + "', authored '" + actual + "'");
        // Generated descriptors carry variability; supplemental definitions
        // do not, so only check when the descriptor states it.
        const std::string variability = d ? std::string(d->variability) : std::string();
        if ((variability == "uniform") != slot.is_uniform() && !variability.empty())
          AddWarning(result, "core.schema.variability", prim.path().str() + "." + name,
                     "Schema declares '" + variability + "', authored '" +
                         (slot.is_uniform() ? "uniform" : "varying") + "'");
      }
    }
  }
}

void ValidateComposedGeometryTargets(const Stage& stage, const ValidationOptions& options,
                                     USDValidationResult* result) {
  const Layer* layer = stage.GetRootLayer();
  if (!options.geom || !layer) return;
  const auto& registry = Registry(options);
  for (const auto& prim : layer->prims()) {
    if (registry.InheritsFrom(prim.type_name(), "Volume")) {
      const UsdPrim volume = stage.GetPrimAtPath(prim.path());
      for (const std::string& name : prim.relationship_names()) {
        if (!Starts(name, "field:")) continue;
        std::vector<Path> targets;
        if (!volume.GetForwardedRelationshipTargets(name, &targets) || targets.size() != 1)
          continue;
        const Path& target = targets.front();
        if (target.is_root() || target.has_property() || !target.is_valid()) continue;
        const UsdPrim field = stage.GetPrimAtPath(target);
        if (!field.IsValid() || !field.IsActive()) {
          AddWarning(result, "vol.volume.fieldTargetMissing", prim.path().str() + "." + name,
                     "volume field target `" + target.str() + "` does not resolve to an active prim on the composed stage");
        } else if (field.GetTypeName().empty()) {
          AddWarning(result, "vol.volume.fieldTargetType", prim.path().str() + "." + name,
                     "volume field target `" + target.str() + "` has no field schema type");
        }
      }
    }
    if (!registry.InheritsFrom(prim.type_name(), "PointInstancer")) continue;
    const auto require_array = [&](const char* name, TypeId type) {
      const auto valid = [type](const Value* value) {
        if (!value || !value->is_array()) return false;
        const TypeId value_type = value->type_id();
        // USDC can store point3f lanes under the unroled float3 value type.
        return value_type == type ||
               (type == TypeId::Point3f && value_type == TypeId::Float3);
      };
      if (valid(GetAttrValue(prim, name))) return;
      if (const auto* samples = GetTimeSamplesOf(prim, name)) {
        for (const auto& sample : *samples) {
          if (valid(prim.time_sample_value(sample.second))) return;
        }
      }
      // A clip declared here or on an ancestor can supply a value.
      for (Path path = prim.path(); !path.empty() && !path.is_root(); path = path.parent()) {
        const PrimSpec* ancestor = layer->prim_at_path(path.str());
        if (ancestor && ancestor->meta().clips().as_dictionary()) return;
      }
      AddError(result, "geom.pointInstancer.requiredArray", prim.path().str() + "." + name,
               std::string("PointInstancer requires an authored ") + name + " array");
    };
    require_array("protoIndices", TypeId::Int);
    require_array("positions", TypeId::Point3f);
    for (const Path& target : RelTargets(prim, "prototypes")) {
      if (target.is_root() || target.has_property() ||
          !IsValidScenePathString(target.str(), nullptr)) continue;
      if (!stage.GetPrimAtPath(target).IsValid()) {
        AddError(result, "geom.pointInstancer.prototypes", prim.path().str() + ".prototypes",
                 "prototype target `" + target.str() + "` does not resolve on the composed stage");
      }
    }
  }
}

void ValidateRegisteredStage(const Layer& layer, const ValidationOptions& options,
                            USDValidationResult* result) {
  const auto& registry = Registry(options);
  std::map<std::string, const PrimSpec*> prims;
  for (const auto& p : layer.prims()) prims[p.path().str()] = &p;
  if (options.geom) {
    // Subset-family restrictions relate siblings, not individual subsets.
    std::map<std::pair<std::string, std::string>, std::vector<const PrimSpec*>> families;
    for (const auto& p : layer.prims())
      if (registry.InheritsFrom(p.type_name(), "GeomSubset"))
        families[{ParentPath(p.path().str()), Text(p, "familyName")}].push_back(&p);
    for (const auto& f : families) {
      const auto parent = prims.find(f.first.first);
      if (parent == prims.end()) continue;
      const PrimSpec& p = *parent->second;
      std::string family_type = Text(p, "subsetFamily:" + f.first.second + ":familyType");
      if (family_type.empty()) family_type = "unrestricted";
      const bool restricted = family_type == "partition" || family_type == "nonOverlapping";
      if (!restricted && family_type != "unrestricted")
        AddWarning(result, "geom.subset.familyType", p.path().str(), "Invalid subset family type: " + family_type);
      if (options.shade && f.first.second == "materialBind" && !restricted)
        AddWarning(result, "shade.subset.materialBindFamily", p.path().str(),
                   "materialBind subset families must be nonOverlapping or partition");
      std::set<int32_t> seen, covered;
      std::set<std::pair<int32_t, int32_t>> seen_pairs, possible_pairs, covered_pairs;
      std::string element_type;
      size_t count = 0;
      bool have_count = false;
      for (const auto* subset : f.second) {
        std::string element = Text(*subset, "elementType");
        if (element.empty()) element = "face";
        if (!element_type.empty() && element != element_type)
          AddWarning(result, "geom.subset.familyType", p.path().str(), "Subset family mixes element types");
        element_type = element;
        const auto* indices = subset->property_value("indices");
        auto values = indices ? indices->as_int_array() : nullptr;
        if (element == "edge" || element == "segment") {
          possible_pairs.clear();
          if (element == "edge" && registry.InheritsFrom(p.type_name(), "Mesh")) {
            const auto* cv = p.property_value("faceVertexCounts");
            const auto* iv = p.property_value("faceVertexIndices");
            auto counts = cv ? cv->as_int_array() : nullptr;
            auto indices = iv ? iv->as_int_array() : nullptr;
            if (counts && indices) {
              have_count = true;
              size_t offset = 0;
              for (int32_t n : *counts) {
                if (n < 0 || static_cast<size_t>(n) > indices->size() - offset) { have_count = false; break; }
                for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
                  int32_t a = (*indices)[offset+i], b = (*indices)[offset+(i+1)%static_cast<size_t>(n)];
                  possible_pairs.emplace(std::min(a,b), std::max(a,b));
                }
                offset += static_cast<size_t>(n);
              }
            }
          } else if (element == "segment" && registry.InheritsFrom(p.type_name(), "BasisCurves")) {
            const auto* cv = p.property_value("curveVertexCounts");
            auto counts = cv ? cv->as_int_array() : nullptr;
            std::string type = Text(p, "type"), basis = Text(p, "basis"), wrap = Text(p, "wrap");
            if (type.empty()) type = "cubic";
            if (basis.empty()) basis = "bezier";
            if (wrap.empty()) wrap = "nonperiodic";
            if (counts) {
              have_count = true;
              for (size_t curve = 0; curve < counts->size(); ++curve) {
                const int32_t n = (*counts)[curve];
                int32_t segments = 0;
                if (type == "linear") segments = wrap == "periodic" ? n : n-1;
                else if (basis == "bezier") segments = wrap == "periodic" ? n/3 : (n-4)/3+1;
                else segments = wrap == "periodic" ? n : wrap == "pinned" ? n-1 : n-3;
                // Invalid huge topology should not cause unbounded allocation.
                if (segments > 0 && static_cast<size_t>(segments) > (size_t(1)<<24) - possible_pairs.size()) {
                  Incomplete(result, "checker.coverage.subset", p.path().str(), "Subset domain exceeds element limit");
                  have_count = false; break;
                }
                for (int32_t segment = 0; segment < segments; ++segment)
                  possible_pairs.emplace(static_cast<int32_t>(curve), segment);
              }
            }
          }
          count = possible_pairs.size();
          if (!values) continue;
          if (values->size() % 2)
            AddWarning(result, "geom.subset.indices", subset->path().str() + ".indices", "Edge and segment indices require pairs");
          for (size_t i = 1; i < values->size(); i += 2) {
            int32_t a = (*values)[i-1], b = (*values)[i];
            if (element == "edge" && b < a) std::swap(a,b);
            const auto pair = std::make_pair(a,b);
            if (!seen_pairs.insert(pair).second && restricted)
              AddWarning(result, "geom.subset.familyOverlap", subset->path().str() + ".indices", "Restricted subset family repeats an element pair");
            if (a < 0 || b < 0 || (have_count && !possible_pairs.count(pair))) {
              AddWarning(result, "geom.subset.indices", subset->path().str() + ".indices", "Subset pair is outside parent elements");
            } else if (family_type == "partition" && have_count) {
              covered_pairs.insert(pair);
            }
          }
          continue;
        } else if (element == "face") {
          if (registry.InheritsFrom(p.type_name(), "TetMesh"))
            have_count = GetArrayLengthProperty(p, "surfaceFaceVertexIndices", &count);
          else {
            const auto* counts = p.property_value("faceVertexCounts");
            if (counts && counts->as_int_array()) { count = counts->as_int_array()->size(); have_count = true; }
          }
        } else if (element == "tetrahedron") {
          have_count = GetArrayLengthProperty(p, "tetVertexIndices", &count);
        } else if (element == "point") {
          have_count = GetPoint3ArrayLen(p, "points", &count);
        }
        if (!values) continue;
        for (int32_t index : *values) {
          if (restricted && !seen.insert(index).second)
            AddWarning(result, "geom.subset.familyOverlap", subset->path().str() + ".indices",
                       "Restricted subset family '" + f.first.second + "' repeats index " + std::to_string(index));
          else if (!restricted) seen.insert(index);
          if (index < 0 || (have_count && static_cast<size_t>(index) >= count)) {
            AddWarning(result, "geom.subset.indices", subset->path().str() + ".indices", "Subset index is outside parent elements");
          } else if (family_type == "partition" && have_count) {
            covered.insert(index);
          }
        }
      }
      if (family_type == "partition" && have_count &&
          (element_type == "edge" || element_type == "segment" ? covered_pairs.size() : covered.size()) != count)
        AddWarning(result, "geom.subset.familyPartition", p.path().str(), "Partition does not cover every parent element");
      if (options.require_complete && restricted && !have_count &&
          element_type != "face" && element_type != "point" && element_type != "tetrahedron")
        Incomplete(result, "checker.coverage.subset", p.path().str(), "Unsupported subset element type: " + element_type);
    }
  }
  ValidateRegisteredPhysics(layer, options, result);
  if (!options.shade) return;
  for (const auto& prim : layer.prims()) {
    bool binding_api = false;
    for (const auto& api : prim.meta().apiSchemas()) binding_api |= api == "MaterialBindingAPI";
    if (binding_api) for (const auto& name : prim.relationship_names()) {
      if (!Starts(name, "material:binding")) continue;
      const auto& targets = RelTargets(prim, name);
      if (targets.empty()) continue;  // explicitly unbound
      const bool collection = Starts(name, "material:binding:collection:");
      if (targets.size() != (collection ? 2u : 1u)) {
        AddWarning(result, "shade.collection.binding", prim.path().str() + "." + name,
                   "Material binding has an invalid number of targets"); continue;
      }
      const auto material = prims.find(targets.back().str());
      if (material == prims.end() || !registry.InheritsFrom(material->second->type_name(), "Material"))
        AddWarning(result, "shade.material.binding", prim.path().str() + "." + name,
                   "Material binding target does not resolve to a Material");
      if (collection) {
        const std::string target = targets.front().str();
        const size_t dot = target.find('.');
        const auto owner = prims.find(target.substr(0, dot));
        bool found = false;
        if (dot != std::string::npos && owner != prims.end()) {
          const std::string property = target.substr(dot + 1);
          if (Starts(property, "collection:"))
            for (const auto& api : owner->second->meta().apiSchemas())
              found |= api == "CollectionAPI:" + property.substr(11);
        }
        if (!found) AddWarning(result, "shade.collection.binding", prim.path().str() + "." + name,
                               "Collection binding target does not identify an applied CollectionAPI instance");
      }
    }
    if (!registry.InheritsFrom(prim.type_name(), "Shader")) continue;
    std::string implementation = Text(prim, "info:implementationSource");
    if (implementation.empty()) implementation = "id";
    if (implementation != "id" && implementation != "sourceAsset" && implementation != "sourceCode") {
      AddWarning(result, "shade.shader.implementationSource", prim.path().str() + ".info:implementationSource",
                 "Invalid shader implementation source: " + implementation); continue;
    }
    std::vector<ValidationShaderView> definitions;
    const std::string id = Text(prim, "info:id");
    if (implementation == "id" && IsMaterialXShaderId(id)) {
      for (const auto& slot : prim.properties().slots()) {
        const std::string name(GetPropNameTable().get(slot.name_id));
        if (slot.is_relationship() || !Starts(name, "inputs:")) continue;
        const auto* connections = prim.connection(slot.name_id);
        if (!connections) continue;
        const std::string input_type = DeclaredType(prim, name);
        if (!ValidType(input_type)) continue;
        for (const auto& target : *connections) {
          std::string source_path, source_port;
          SplitScenePathString(target.str(), &source_path, &source_port);
          const auto source = prims.find(source_path);
          if (source == prims.end() ||
              (!Starts(source_port, "outputs:") && !Starts(source_port, "inputs:"))) {
            continue;
          }
          const auto* source_slot = source->second->property(source_port);
          if (!source_slot || source_slot->is_relationship()) continue;
          const std::string source_type = DeclaredType(*source->second, source_port);
          if (ValidType(source_type) && source_type != input_type) {
            AddWarning(result, "shade.materialX.connectionType",
                       prim.path().str() + "." + name,
                       "MaterialX connection from " + target.str() + " has type '" +
                           source_type + "', but the input declares '" + input_type + "'");
          }
        }
      }
    }
    if (implementation == "id") {
      const auto d = registry.FindShader(id);
      if (d) definitions.push_back(*d);
      else if (options.require_complete)
        Incomplete(result, "checker.coverage.shader", prim.path().str(), "No shader definition for '" + id + "'");
    } else {
      std::set<std::string> source_types;
      for (const auto& slot : prim.properties().slots()) {
        const std::string name(GetPropNameTable().get(slot.name_id));
        const std::string suffix = ":" + implementation;
        if (Starts(name, "info:") && EndsWith(name, suffix) && name.size() > 5 + suffix.size()) {
          const std::string source_type = name.substr(5, name.size() - 5 - suffix.size());
          source_types.insert(source_type);
          const auto d = registry.FindShader(id, source_type);
          if (d) definitions.push_back(*d);
          else if (options.require_complete)
            Incomplete(result, "checker.coverage.shader", prim.path().str() + "." + name,
                       "No exported shader definition for source type '" + source_type + "'");
        }
      }
      if (source_types.empty())
        AddWarning(result, "shade.shader.sourceType", prim.path().str(), "Source-based shader has no source type");
    }
    std::map<std::string, std::string> expected;
    for (const auto& d : definitions) {
      for (bool output : {false, true}) {
        if (output && !IsMaterialXShaderId(id)) continue;
        for (size_t i = 0; i < d.port_count(output); ++i) {
          const auto port = d.port(output, i);
          const std::string name = std::string(output ? "outputs:" : "inputs:") +
                                   std::string(port.first);
          const auto old = expected.find(name);
          if (old != expected.end() && old->second != port.second)
            AddWarning(result, "shade.shader.sourceTypeConflict",
                       prim.path().str() + "." + name,
                       "Shader definitions disagree on port type");
          expected[name] = port.second;
        }
      }
    }
    for (const auto& [name, type] : expected) {
      if (prim.property(name) && DeclaredType(prim, name) != type)
        AddWarning(result, "shade.shader.typeMismatch", prim.path().str() + "." + name,
                   "Shader definition requires '" + type + "', authored '" + DeclaredType(prim, name) + "'");
    }
    if (implementation == "id" && IsMaterialXShaderId(id) &&
        std::any_of(definitions.begin(), definitions.end(), [](const auto& definition) {
          return definition.source_type == "mtlx";
        })) {
      for (const auto& slot : prim.properties().slots()) {
        const std::string name(GetPropNameTable().get(slot.name_id));
        if (!slot.is_relationship() && (Starts(name, "inputs:") || Starts(name, "outputs:")) &&
            expected.count(name) == 0) {
          AddWarning(result, "shade.materialX.unknownPort",
                     prim.path().str() + "." + name,
                     "MaterialX node definition '" + id + "' has no port '" + name + "'");
        }
      }
    }
  }
}

USDValidationResult ValidateStageSamples(const Stage& stage,
    const ValidationContext& context, const EvalOptions& eval_options) {
  USDValidationResult result;
  result.checked_groups.core = false;
  result.checked_groups.max_issues = context.options.max_issues;
  result.checked_groups.max_report_bytes = context.options.max_report_bytes;
  const Layer* layer = stage.GetRootLayer();
  if (!layer) { Incomplete(&result, "checker.coverage.stage", "<stage>", "Stage has no root layer"); return result; }
  std::set<double> times;
  const size_t cap = context.limits.max_value_clip_samples;
  auto add_time = [&](double time) {
    if (std::isfinite(time) && times.size() <= cap) times.insert(time);
  };
  for (const auto& prim : layer->prims()) {
    for (const auto& slot : prim.properties().slots()) {
      if (const auto* samples = prim.time_samples(slot.name_id))
        for (const auto& s : *samples) add_time(s.first);
      if (const auto* text = prim.spline_source(slot.name_id)) {
        SplineData spline; std::string error;
        if (!ParseSplineText(*text, &spline, &error))
          Incomplete(&result, "checker.coverage.spline", prim.path().str(), error);
        else for (const auto& knot : spline.knots) add_time(knot.time);
      }
    }
    const UsdPrim p = stage.GetPrimAtPath(prim.path());
    std::vector<ValueClipSet> sets; std::string error;
    if (!ParseValueClipSets(p, &sets, &error)) {
      if (!error.empty()) Incomplete(&result, "checker.coverage.clips", prim.path().str(), error);
      continue;
    }
    for (const auto& set : sets) {
      for (const auto& t : set.times) add_time(t.first);
      for (const auto& t : set.active) add_time(t.first);
      if (!eval_options.clip_stage_loader) {
        Incomplete(&result, "checker.coverage.clips", prim.path().str(), "Clip sampling requires a layer loader"); continue;
      }
      for (const auto& asset : set.asset_paths) {
        Stage clip; std::string warn, err;
        if (!eval_options.clip_stage_loader(asset, &clip, &warn, &err)) {
          Incomplete(&result, "checker.coverage.clips", prim.path().str(), err); continue;
        }
        const Layer* clip_layer = clip.GetRootLayer();
        if (!clip_layer) continue;
        for (const auto& cp : clip_layer->prims()) for (const auto& slot : cp.properties().slots()) {
          if (const auto* samples = cp.time_samples(slot.name_id)) for (const auto& sample : *samples) {
            if (set.times.empty()) add_time(sample.first);
            for (size_t i = 1; i < set.times.size(); ++i) {
              const auto a = set.times[i-1], b = set.times[i];
              if (a.second == b.second) continue;
              const double u = (sample.first - a.second) / (b.second - a.second);
              if (u >= 0 && u <= 1) add_time(a.first + u * (b.first - a.first));
            }
          }
        }
      }
    }
  }
  if (times.size() > cap) {
    Incomplete(&result, "checker.coverage.samples", "<stage>", "Validation sample limit exceeded");
    return result;
  }
  AttributeEval evaluator(&stage);
  EvalOptions eval = eval_options;
  eval.strict_aousd_conformance = true;
  eval.follow_connections = false;
  // One evaluated snapshot per time keeps topology/primvar comparisons coherent.
  for (double time : times) {
    Layer snapshot = layer->Clone();
    std::vector<std::string> blocked_instancer_arrays;
    eval.time = TimeQuery::Numeric(time);
    evaluator.SetOptions(eval);
    for (size_t prim_index = 0; prim_index < snapshot.prims().size(); ++prim_index) {
      PrimSpec& prim = *snapshot.prim(static_cast<uint32_t>(prim_index));
      const UsdPrim p = stage.GetPrimAtPath(prim.path());
      std::vector<PropNameId> attributes;
      for (const auto& slot : prim.properties().slots())
        if (!slot.is_relationship()) attributes.push_back(slot.name_id);
      for (const auto id : attributes) {
        const std::string name(GetPropNameTable().get(id));
        const auto value = evaluator.Eval(p, name);
        if (value.success) prim.upsert_property(id, value.value);
        else {
          if (value.blocked && context.options.geom &&
              (name == "positions" || name == "protoIndices") &&
              Registry(context.options).InheritsFrom(prim.type_name(), "PointInstancer")) {
            blocked_instancer_arrays.push_back(prim.path().str() + "." + name);
          }
          // Do not accidentally validate the authored default at a blocked time.
          prim.remove_property(id);
          if (!value.error.empty())
            Incomplete(&result, "checker.coverage.evaluation", prim.path().str() + "." + name, value.error);
        }
      }
    }
    ValidationOptions options = context.options;
    options.stage_presence_checks = false;
    options.run_callbacks = false;
    USDValidationResult sample = ValidateLayerAgainstAOUSDCore(snapshot, options);
    for (const auto& location : blocked_instancer_arrays) {
      AddError(&sample, "geom.pointInstancer.requiredArray", location,
               "required PointInstancer array is blocked at this time");
    }
    USDValidationResult tagged;
    tagged.checked_groups = sample.checked_groups;
    tagged.complete = sample.complete;
    tagged.omitted_issues = sample.omitted_issues;
    for (auto& issue : sample.issues) {
      issue.has_time = true; issue.time = time;
      issue.source_asset = context.source_asset; issue.variants = context.variants;
      tagged.AddIssue(std::move(issue));
    }
    sample = std::move(tagged);
    Stage sampled_stage;
    sampled_stage.SetRootLayer(std::move(snapshot));
    ValidationContext sampled_context = context;
    sampled_context.layer = sampled_stage.GetRootLayer();
    sampled_context.stage = &sampled_stage;
    sampled_context.time = TimeQuery::Numeric(time);
    Registry(options).RunCallbacks(ValidationScope::Stage, sampled_context, nullptr, &sample);
    for (const auto& prim : sampled_context.layer->prims())
      Registry(options).RunCallbacks(ValidationScope::Prim, sampled_context, &prim, &sample);
    MergeValidationResults(&result, sample);
  }
  return result;
}

// Rule id -> Lean declaration (usdspec-conformance verif/lean/UsdSpec/**). Kept
// sorted by rule id; that repository's scripts/check-lean-reflection.py
// (--lightusd <this checkout>) parses this table and verifies every declaration
// exists in the Lean sources. Keep the BEGIN/END markers intact.
// BEGIN LEAN_REFLECTION_TABLE
static const std::pair<const char*, const char*> kLeanReflectionTable[] = {
    {"core.dict.duplicateKey", "UsdSpec.Dict.WellFormed"},
    {"core.layer.layerRelocates.ancestor", "UsdSpec.Relocates.OkAncestor"},
    {"core.layer.layerRelocates.duplicateSource", "UsdSpec.Relocates.OkSourcesUnique"},
    {"core.layer.layerRelocates.duplicateTarget", "UsdSpec.Relocates.OkTargetsUnique"},
    {"core.layer.layerRelocates.identity", "UsdSpec.Relocates.OkSame"},
    {"core.layer.layerRelocates.nestedSource", "UsdSpec.Relocates.OkAncestral"},
    {"core.layer.layerRelocates.root", "UsdSpec.Relocates.OkRootOpenUsd"},
    {"core.layer.layerRelocates.targetIsSource", "UsdSpec.Relocates.OkOtherSourceOpenUsd"},
    {"core.layer.layerRelocates.targetUnderOtherSource", "UsdSpec.Relocates.OkTargetNamespaceOpenUsd"},
    {"core.layer.layerRelocates.targetUnderSource", "UsdSpec.Relocates.OkBelow"},
    {"core.layer.subLayerOffset", "UsdSpec.LayerOffset.valid"},
    {"core.listop.deprecatedAdd", "UsdSpec.ListOp.applyLegacy"},
    {"core.listop.deprecatedReorder", "UsdSpec.ListOp.applyLegacy"},
    {"core.listop.duplicateItems", "UsdSpec.Composable"},
    {"core.schema.familyVersion", "UsdSpec.Schema.parseFamilyVersion"},
    {"core.schema.selfFamilyInherit", "UsdSpec.Schema.versionedMayInherit"},
    {"geom.mesh.topology.count", "UsdSpec.Domains.meshFaceCountsValid"},
    {"geom.mesh.topology.index", "UsdSpec.Domains.meshInvalidIndexCount"},
    {"geom.mesh.topology.size", "UsdSpec.Domains.meshIndexCountMatches"},
    {"geom.pointInstancer.indices", "UsdSpec.Domains.pointInstancerIndexErrors"},
    {"geom.pointInstancer.prototypes", "UsdSpec.Domains.prototypeErrors"},
    {"geom.pointInstancer.requiredArray", "UsdSpec.Domains.pointInstancerRequiredArrayErrors"},
    {"geom.pointInstancer.orientation", "UsdSpec.Domains.pointInstancerQuaternionValid"},
    {"geom.points.size", "UsdSpec.Domains.pointsArraySizeValid"},
    {"geom.points.widths", "UsdSpec.Domains.invalidPointWidths"},
    {"geom.skel.animation.blendShapes", "UsdSpec.Domains.SkeletonValidation.animationBlendShapes"},
    {"geom.skel.animation.joints", "UsdSpec.Domains.SkeletonValidation.animationJoints"},
    {"geom.skel.animation.transforms", "UsdSpec.Domains.SkeletonValidation.arrayCardinality"},
    {"geom.skel.blendShape.inbetween", "UsdSpec.Domains.SkeletonValidation.blendShapeInbetween"},
    {"geom.skel.blendShape.normalOffsets", "UsdSpec.Domains.SkeletonValidation.arrayCardinality"},
    {"geom.skel.blendShape.offsets", "UsdSpec.Domains.SkeletonValidation.blendShapeOffsets"},
    {"geom.skel.blendShape.pointIndices", "UsdSpec.Domains.SkeletonValidation.blendShapeIndices"},
    {"geom.skel.skeleton.jointNames", "UsdSpec.Domains.skeletonJointNameCount"},
    {"geom.skel.skeleton.joints", "UsdSpec.Domains.skeletonJoints"},
    {"geom.skel.skeleton.topology", "UsdSpec.Domains.skeletonTopology"},
    {"geom.skel.skeleton.transforms", "UsdSpec.Domains.SkeletonValidation.skeletonTransforms"},
    {"geom.subset.familyPartition", "UsdSpec.Domains.subsetCoversDomain"},
    {"physics.drive.value", "UsdSpec.Domains.driveValueValid"},
    {"physics.joint.axis", "UsdSpec.Domains.physicsJointAxis"},
    {"physics.joint.distance", "UsdSpec.Domains.distanceLimitErrors"},
    {"physics.joint.limit", "UsdSpec.Domains.jointLimitErrors"},
    {"physics.scene.gravity", "UsdSpec.Domains.sceneGravityErrors"},
    {"physics.value.range", "UsdSpec.Domains.physicalScalarValid"},
    {"shade.materialX.configAPI", "UsdSpec.Domains.materialXConfig"},
    {"shade.materialX.referencePrimPath", "UsdSpec.Domains.materialXReferencePrimPathValid"},
    {"shade.materialX.version", "UsdSpec.Domains.materialXVersion"},
    {"vol.fieldAsset.dataType", "UsdSpec.Domains.VolumeValidation.fieldAssetDataType"},
    {"vol.fieldAsset.fieldClass", "UsdSpec.Domains.VolumeValidation.tokenProperty"},
    {"vol.fieldAsset.filePath", "UsdSpec.Domains.VolumeValidation.fieldAssetFilePath"},
    {"vol.fieldAsset.vectorDataRoleHint", "UsdSpec.Domains.VolumeValidation.tokenProperty"},
    {"vol.particleField.arraySize", "UsdSpec.Domains.VolumeValidation.particleArraySize"},
    {"vol.particleField.kernel", "UsdSpec.Domains.VolumeValidation.particleProvider"},
    {"vol.particleField.position", "UsdSpec.Domains.VolumeValidation.particleProvider"},
    {"vol.particleField.projectionModeHint", "UsdSpec.Domains.VolumeValidation.tokenProperty"},
    {"vol.particleField.shCount", "UsdSpec.Domains.VolumeValidation.sphericalHarmonicsSize"},
    {"vol.particleField.shDegree", "UsdSpec.Domains.VolumeValidation.sphericalHarmonicsDegree"},
    {"vol.particleField.sortingModeHint", "UsdSpec.Domains.VolumeValidation.tokenProperty"},
    {"vol.volume.fieldRel", "UsdSpec.Domains.volumeFieldRelationship"},
    {"vol.volume.fieldTargetCount", "UsdSpec.Domains.volumeFieldTargetCount"},
    {"vol.volume.fieldTargetType", "UsdSpec.Domains.volumeFieldTargetType"},
    {"vol.volume.fieldTargetMissing", "UsdSpec.Domains.volumeFieldTargetMissing"},
};
// END LEAN_REFLECTION_TABLE

std::string GetValidationRuleLeanRef(const std::string& rule) {
  for (const auto& row : kLeanReflectionTable)
    if (rule == row.first) return row.second;
  return std::string();
}

ValidationRuleMetadata GetValidationRuleMetadata(const std::string& rule) {
  ValidationRuleMetadata m;
  m.category = "recommendation";
  const std::string spec = "https://github.com/aousd/specifications-public/blob/main/core/1.1.0/core_spec.md";
  if (Starts(rule, "checker.")) m.category = "coverage";
  else if (Starts(rule, "arkit.")) m.category = "profile";
  else if (Starts(rule, "geom.") || Starts(rule, "shade.") || Starts(rule, "physics.") ||
           Starts(rule, "lux.") || Starts(rule, "render.") || Starts(rule, "vol.") ||
           rule == "core.schema.attributeType" || rule == "core.schema.propertyKind" || rule == "core.apiSchema.kind") m.category = "schema";
  else if (Starts(rule, "crate.") || Starts(rule, "package.")) {
    m.category = "normative"; m.specification = spec;
    if (rule == "package.entry.extension" || Starts(rule, "package.dependency.") ||
        rule == "package.entry.path" || rule == "package.entry.duplicate" ||
        rule == "package.entry.dataDescriptor") {
      m.category = "recommendation";
      m.specification.clear();
    }
  } else if (Starts(rule, "core.")) {
    // Normative, representable document constraints. Delivery conventions and
    // schema-specific advice must not be promoted into Core requirements.
    static const std::set<std::string> normative = {
      "core.prim.name", "core.prim.customData", "core.prim.assetInfo",
      "core.attr.customData", "core.attr.timeSamples", "core.relationship.customData",
      "core.layer.customLayerData", "core.layer.expressionVariables",
      "core.layer.subLayers", "core.layer.subLayerOffset",
      "core.layer.timeCodesPerSecond", "core.layer.framesPerSecond",
      "core.layer.layerRelocates.ancestor", "core.layer.layerRelocates.duplicateSource",
      "core.layer.layerRelocates.duplicateTarget", "core.layer.layerRelocates.identity",
      "core.layer.layerRelocates.nestedSource", "core.layer.layerRelocates.root",
      "core.layer.layerRelocates.targetUnderSource",
      "core.layer.layerRelocates.source", "core.layer.layerRelocates.target",
      "core.composition.inherits", "core.composition.specializes",
      "core.composition.reference", "core.composition.payload",
      "core.composition.variantSet", "core.composition.variantCycle",
      "core.composition.propertyKindMismatch", "core.composition.error",
      "core.listop.duplicateItems", "core.dict.duplicateKey",
      "core.schema.familyVersion", "core.schema.selfFamilyInherit"};
    if (normative.count(rule)) { m.category = "normative"; m.specification = spec; }
  }
  // These policies intentionally differ from the literal per-entry reading.
  if (rule == "core.layer.layerRelocates.targetIsSource" ||
      rule == "core.layer.layerRelocates.targetUnderOtherSource") {
    m.category = "compatibility";
    m.specification = spec;
  }
  m.lean_ref = GetValidationRuleLeanRef(rule);
  // Rule -> specification reference errors, from the C11 core (binary search
  // over the rule-sorted rows).
  size_t ref_index = 0, ref_count = 0;
  if (lu_validation_ref_error_find(lu_view{rule.data(), rule.size()}, &ref_index,
                                   &ref_count) == LU_OK) {
    for (size_t i = 0; i < ref_count; i++) {
      lu_view v;
      if (lu_validation_ref_error_item(ref_index, i, &v) != LU_OK) break;
      m.reference_errors.emplace_back(v.data != nullptr ? v.data : "", v.len);
    }
  }
  return m;
}
 }  // namespace lightusd
