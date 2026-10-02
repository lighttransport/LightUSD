// SPDX-License-Identifier: Apache-2.0
#include "validation-context.hh"
#include "usd-validation-internal.hh"
#include "../schema/schema-registry.hh"
#include "../types/spline.hh"
#include "../prim/identifier.hh"
#include "../../minijson.hh"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace lightusd { namespace next {
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

ValidationRegistry::ValidationRegistry() {
  ValidationSchemaDefinition* current = nullptr;
  const auto schema = [&](const char* name, const char* parent, const char* kind,
                          const char* ns, const char* source) {
    auto& s = schemas_[name];
    s.name = name; s.parent = parent; s.kind = kind;
    s.property_namespace = ns; s.source = source; current = &s;
  };
  const auto property = [&](const char* name, const char* type, bool rel,
                            const char* variability,
                            std::vector<std::string> tokens) {
    current->properties[name] = {type, rel, variability, std::move(tokens)};
  };
  const auto shader = [&](const char* id, const char* source_type,
                          const char* source, std::map<std::string, std::string> inputs,
                          std::map<std::string, std::string> outputs) {
    shaders_[{id, source_type}] = {id, source_type, source,
                                  std::move(inputs), std::move(outputs)};
  };
#include "generated-definitions.inc"
}

const ValidationRegistry& GetBuiltinValidationRegistry() {
  static const ValidationRegistry registry;
  return registry;
}

nonstd::expected<bool, std::string> ValidationRegistry::LoadSchemaDefinitions(
    const std::string& text) {
  Json root;
  if (!lightusd::minijson::Parse(text, &root) || !root["formatVersion"].is_number_integer() || root["formatVersion"].get_int() != 1 ||
      !root["schemas"].is_array())
    return nonstd::make_unexpected(std::string("expected schema manifest formatVersion 1"));
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
      return nonstd::make_unexpected(std::string("invalid or duplicate schema definition: ") + s.name);
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
        return nonstd::make_unexpected(std::string("invalid property definition: ") + s.name + "." + name);
    }
    const auto found = candidate.find(s.name);
    if (found != candidate.end() && !SameSchema(found->second, s))
      return nonstd::make_unexpected(std::string("conflicting schema definition: ") + s.name);
    candidate[s.name] = std::move(s);
  }
  for (const auto& pair : candidate) {
    std::set<std::string> seen;
    std::string name = pair.first;
    while (!name.empty()) {
      if (!seen.insert(name).second)
        return nonstd::make_unexpected(std::string("cyclic schema inheritance: ") + name);
      auto i = candidate.find(name);
      if (i == candidate.end()) {
        if (GetSchemaRegistry().IsKnownSchema(name)) break;
        return nonstd::make_unexpected(std::string("undefined schema ancestor: ") + name);
      }
      name = i->second.parent;
    }
  }
  schemas_ = std::move(candidate);
  return true;
}

nonstd::expected<bool, std::string> ValidationRegistry::LoadShaderDefinitions(
    const std::string& text) {
  Json root;
  if (!lightusd::minijson::Parse(text, &root) || !root["formatVersion"].is_number_integer() || root["formatVersion"].get_int() != 1 ||
      !root["shaders"].is_array())
    return nonstd::make_unexpected(std::string("expected shader manifest formatVersion 1"));
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
      return nonstd::make_unexpected(std::string("invalid or duplicate shader definition: ") + s.identifier);
    const auto old = candidate.find(key);
    if (old != candidate.end() &&
        (old->second.inputs != s.inputs || old->second.outputs != s.outputs))
      return nonstd::make_unexpected(std::string("conflicting shader definition: ") + s.identifier);
    candidate[key] = std::move(s);
  }
  shaders_ = std::move(candidate);
  return true;
}

nonstd::expected<bool, std::string> ValidationRegistry::RegisterValidator(
    ValidatorDefinition rule) {
  if (rule.id.empty() || !rule.callback)
    return nonstd::make_unexpected(std::string("validator needs an id and callback"));
  size_t count = 0;
  const auto* builtins = GetValidationRuleTable(&count);
  for (size_t i = 0; i < count; ++i)
    if (rule.id == builtins[i].id)
      return nonstd::make_unexpected(std::string("reserved validator id: ") + rule.id);
  for (const auto& existing : validators_)
    if (existing.id == rule.id)
      return nonstd::make_unexpected(std::string("duplicate validator id: ") + rule.id);
  validators_.push_back(std::move(rule));
  return true;
}
const ValidationSchemaDefinition* ValidationRegistry::FindSchema(const std::string& name) const {
  const auto i = schemas_.find(name);
  return i == schemas_.end() ? nullptr : &i->second;
}
bool ValidationRegistry::InheritsFrom(const std::string& name, const std::string& ancestor) const {
  std::string current = name;
  for (size_t i = 0; i <= schemas_.size(); ++i) {
    if (current == ancestor) return true;
    const auto* s = FindSchema(current);
    if (!s) return GetSchemaRegistry().InheritsFrom(current, ancestor);
    if (s->parent.empty()) break;
    current = s->parent;
  }
  return false;
}
const ValidationPropertyDefinition* ValidationRegistry::FindProperty(
    const PrimSpec& prim, const std::string& name) const {
  std::vector<std::pair<std::string, std::string>> schemas = {{prim.type_name(), ""}};
  for (const auto& api : prim.meta().apiSchemas()) {
    const size_t colon = api.find(':');
    schemas.emplace_back(api.substr(0, colon), colon == std::string::npos ? "" : api.substr(colon + 1));
  }
  for (const auto& applied : schemas) {
    std::string current = applied.first;
    for (size_t hops = 0; hops <= schemas_.size(); ++hops) {
      const auto* s = FindSchema(current);
      if (!s) break;
      std::string local = name;
      if (s->kind == "multipleApply") {
        if (applied.second.empty()) break;
        const std::string prefix = s->property_namespace + ":" + applied.second;
        if (local == prefix) local = "__INSTANCE_NAME__";
        else if (Starts(local, prefix + ":")) local = local.substr(prefix.size() + 1);
        else break;
      }
      auto property = s->properties.find(local);
      if (property != s->properties.end()) return &property->second;
      current = s->parent;
    }
  }
  return nullptr;
}
const ValidationShaderDefinition* ValidationRegistry::FindShader(
    const std::string& id, const std::string& source_type) const {
  const auto exact = shaders_.find({id, source_type});
  if (exact != shaders_.end()) return &exact->second;
  if (!source_type.empty()) return nullptr;
  const ValidationShaderDefinition* candidate = nullptr;
  for (const auto& s : shaders_) {
    if (s.first.first != id) continue;
    if (candidate) return nullptr;  // ambiguous source types need an explicit selection
    candidate = &s.second;
  }
  return candidate;
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
      const auto* definition = registry.FindProperty(prim, name);
      if (definition && !definition->relationship)
        AddWarning(result, "core.schema.propertyKind", prim.path().str() + "." + name,
                   "Schema requires an attribute, authored a relationship");
    }
    for (const auto& slot : prim.properties().slots()) {
      const std::string name(GetPropNameTable().get(slot.name_id));
      const auto* d = registry.FindProperty(prim, name);
      // Supplemental implemented schema definitions not in the pinned manifest.
      const auto* builtin = d ? nullptr : GetSchemaRegistry().FindProperty(prim, name);
      if (!d && !builtin) continue;
      const std::string expected = d ? d->type : builtin->type_name;
      const bool rel = d ? d->relationship : expected == "rel";
      if (rel != slot.is_relationship()) {
        AddWarning(result, "core.schema.propertyKind", prim.path().str() + "." + name,
                   "Schema requires " + std::string(rel ? "a relationship" : "an attribute"));
      } else if (!rel) {
        const std::string actual = DeclaredType(prim, name);
        if (!actual.empty() && actual != expected)
          AddWarning(result, "core.schema.attributeType", prim.path().str() + "." + name,
                     "Schema requires '" + expected + "', authored '" + actual + "'");
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
      std::set<int32_t> seen;
      std::set<std::pair<int32_t, int32_t>> seen_pairs, possible_pairs;
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
        const auto* values = indices ? indices->as_int_array() : nullptr;
        if (element == "edge" || element == "segment") {
          possible_pairs.clear();
          if (element == "edge" && registry.InheritsFrom(p.type_name(), "Mesh")) {
            const auto* cv = p.property_value("faceVertexCounts");
            const auto* iv = p.property_value("faceVertexIndices");
            const auto* counts = cv ? cv->as_int_array() : nullptr;
            const auto* indices = iv ? iv->as_int_array() : nullptr;
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
            const auto* counts = cv ? cv->as_int_array() : nullptr;
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
            if (a < 0 || b < 0 || (have_count && !possible_pairs.count(pair)))
              AddWarning(result, "geom.subset.indices", subset->path().str() + ".indices", "Subset pair is outside parent elements");
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
          if (index < 0 || (have_count && static_cast<size_t>(index) >= count))
            AddWarning(result, "geom.subset.indices", subset->path().str() + ".indices", "Subset index is outside parent elements");
        }
      }
      if (family_type == "partition" && have_count &&
          (element_type == "edge" || element_type == "segment" ? seen_pairs.size() : seen.size()) != count)
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
    std::vector<const ValidationShaderDefinition*> definitions;
    const std::string id = Text(prim, "info:id");
    if (implementation == "id") {
      const auto* d = registry.FindShader(id);
      if (d) definitions.push_back(d);
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
          const auto* d = registry.FindShader(id, source_type);
          if (d) definitions.push_back(d);
          else if (options.require_complete)
            Incomplete(result, "checker.coverage.shader", prim.path().str() + "." + name,
                       "No exported shader definition for source type '" + source_type + "'");
        }
      }
      if (source_types.empty())
        AddWarning(result, "shade.shader.sourceType", prim.path().str(), "Source-based shader has no source type");
    }
    std::map<std::string, std::string> expected;
    for (const auto* d : definitions) {
      for (const auto& input : d->inputs) {
        const auto old = expected.find(input.first);
        if (old != expected.end() && old->second != input.second)
          AddWarning(result, "shade.shader.sourceTypeConflict", prim.path().str() + ".inputs:" + input.first,
                     "Shader definitions disagree on input type");
        expected[input.first] = input.second;
      }
    }
    for (const auto& input : expected) {
      const std::string name = "inputs:" + input.first;
      if (prim.property(name) && DeclaredType(prim, name) != input.second)
        AddWarning(result, "shade.shader.typeMismatch", prim.path().str() + "." + name,
                   "Shader definition requires '" + input.second + "', authored '" + DeclaredType(prim, name) + "'");
    }
  }
}

USDValidationResult ValidateStageSamples(const Stage& stage,
    const ValidationContext& context, const EvalOptions& eval_options) {
  USDValidationResult result;
  result.checked_groups.core = false;
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
    for (auto& issue : sample.issues) {
      issue.has_time = true; issue.time = time;
      issue.source_asset = context.source_asset; issue.variants = context.variants;
    }
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

ValidationRuleMetadata GetValidationRuleMetadata(const std::string& rule) {
  ValidationRuleMetadata m;
  m.category = "recommendation";
  const std::string spec = "https://github.com/aousd/specifications-public/blob/main/core/1.0.1/core_spec.md";
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
      "core.layer.layerRelocates.source", "core.layer.layerRelocates.target",
      "core.composition.inherits", "core.composition.specializes",
      "core.composition.reference", "core.composition.payload",
      "core.composition.variantSet", "core.composition.variantCycle",
      "core.composition.propertyKindMismatch", "core.composition.error"};
    if (normative.count(rule)) { m.category = "normative"; m.specification = spec; }
  }
  static const std::map<std::string, std::vector<std::string>> reference = {
#include "generated-reference-errors.inc"
  };
  const auto i = reference.find(rule);
  if (i != reference.end()) m.reference_errors = i->second;
  return m;
}
} }  // namespace lightusd::next
