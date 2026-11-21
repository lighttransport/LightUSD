// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "schema-registry.hh"

#include "stage/stage.hh"
#include "parser/lexer.hh"
#include "parser/value-parser.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string_view>

namespace lightusd {

namespace {

struct DomainProperty {
  const char* schema;
  const char* name;
  const char* type;
  const char* fallback;
};

constexpr DomainProperty kDomainProperties[] = {
#define LIGHTUSD_DOMAIN_BUILTIN(owner, applied)
#define LIGHTUSD_DOMAIN_SCHEMA(name, parent)
#define LIGHTUSD_DOMAIN_PROPERTY(schema, name, type, fallback) \
  {schema, name, type, fallback},
#ifndef LIGHTUSD_DOMAIN_KIND
#define LIGHTUSD_DOMAIN_KIND(name, kind)
#define LIGHTUSD_DOMAIN_KIND_LOCAL 1
#endif
#include "schema/generated/openusd-domain-schema-registry.inc"
#ifdef LIGHTUSD_DOMAIN_KIND_LOCAL
#undef LIGHTUSD_DOMAIN_KIND
#undef LIGHTUSD_DOMAIN_KIND_LOCAL
#endif
#undef LIGHTUSD_DOMAIN_PROPERTY
#undef LIGHTUSD_DOMAIN_SCHEMA
#undef LIGHTUSD_DOMAIN_BUILTIN
};

constexpr std::pair<std::string_view, std::string_view> kDomainParents[] = {
#define LIGHTUSD_DOMAIN_BUILTIN(owner, applied)
#define LIGHTUSD_DOMAIN_SCHEMA(name, parent) {name, parent},
#define LIGHTUSD_DOMAIN_PROPERTY(schema, name, type, fallback)
#ifndef LIGHTUSD_DOMAIN_KIND
#define LIGHTUSD_DOMAIN_KIND(name, kind)
#define LIGHTUSD_DOMAIN_KIND_LOCAL 1
#endif
#include "schema/generated/openusd-domain-schema-registry.inc"
#ifdef LIGHTUSD_DOMAIN_KIND_LOCAL
#undef LIGHTUSD_DOMAIN_KIND
#undef LIGHTUSD_DOMAIN_KIND_LOCAL
#endif
#undef LIGHTUSD_DOMAIN_PROPERTY
#undef LIGHTUSD_DOMAIN_SCHEMA
#undef LIGHTUSD_DOMAIN_BUILTIN
};

constexpr std::pair<std::string_view, std::string_view> kDomainBuiltinAPIs[] = {
#define LIGHTUSD_DOMAIN_SCHEMA(name, parent)
#define LIGHTUSD_DOMAIN_PROPERTY(schema, name, type, fallback)
#define LIGHTUSD_DOMAIN_BUILTIN(owner, applied) {owner, applied},
#ifndef LIGHTUSD_DOMAIN_KIND
#define LIGHTUSD_DOMAIN_KIND(name, kind)
#define LIGHTUSD_DOMAIN_KIND_LOCAL 1
#endif
#include "schema/generated/openusd-domain-schema-registry.inc"
#ifdef LIGHTUSD_DOMAIN_KIND_LOCAL
#undef LIGHTUSD_DOMAIN_KIND
#undef LIGHTUSD_DOMAIN_KIND_LOCAL
#endif
#undef LIGHTUSD_DOMAIN_BUILTIN
#undef LIGHTUSD_DOMAIN_PROPERTY
#undef LIGHTUSD_DOMAIN_SCHEMA
};

// Schema kinds of the OpenUSD 26.11 domain catalogs (concreteTyped,
// abstractTyped, singleApplyAPI, multipleApplyAPI, ...).
constexpr std::pair<std::string_view, std::string_view> kDomainKinds[] = {
#define LIGHTUSD_DOMAIN_SCHEMA(name, parent)
#define LIGHTUSD_DOMAIN_PROPERTY(schema, name, type, fallback)
#define LIGHTUSD_DOMAIN_BUILTIN(owner, applied)
#define LIGHTUSD_DOMAIN_KIND(name, kind) {name, kind},
#include "schema/generated/openusd-domain-schema-registry.inc"
#undef LIGHTUSD_DOMAIN_KIND
#undef LIGHTUSD_DOMAIN_BUILTIN
#undef LIGHTUSD_DOMAIN_PROPERTY
#undef LIGHTUSD_DOMAIN_SCHEMA
};

std::string_view DomainKind(std::string_view name) {
  for (const auto& [schema, kind] : kDomainKinds) {
    if (schema == name) return kind;
  }
  return {};
}

void AppendDomainAPIs(std::string_view owner, std::vector<std::string>& result) {
  for (const auto& [schema, applied] : kDomainBuiltinAPIs) {
    if (schema != owner ||
        std::find(result.begin(), result.end(), applied) != result.end()) {
      continue;
    }
    result.emplace_back(applied);
    AppendDomainAPIs(applied, result);
  }
}

Value SchemaToken(const char* value) { return Value::MakeToken(value); }

bool MatchDefinitionName(const std::vector<std::string>& applied_schemas,
                         const SchemaPropertyDefinition& def,
                         const std::string& property_name) {
  static const std::string marker = "__INSTANCE__";
  const size_t marker_pos = def.instance_marker_pos;
  if (marker_pos == std::string::npos) return def.name == property_name;
  for (const std::string& applied : applied_schemas) {
    const std::string prefix = def.schema_type + ":";
    if (applied.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string instance = applied.substr(prefix.size());
    if (instance.empty()) continue;
    std::string instantiated = def.name;
    instantiated.replace(marker_pos, marker.size(), instance);
    if (instantiated == property_name) return true;
  }
  return false;
}

bool IsAPISchemaTypeName(const std::string& name) {
  return name.size() > 3 && name.compare(name.size() - 3, 3, "API") == 0;
}

// Rank of the applied schema `schema` within the prim's apiSchemas (list-op
// resolved order), or -1. Instance-qualified entries match their family.
int AppliedSchemaRank(const std::vector<std::string>& applied_schemas,
                      const std::string& schema) {
  int rank = 0;
  for (const std::string& applied : applied_schemas) {
    if (applied == schema ||
        (applied.size() > schema.size() &&
         applied.compare(0, schema.size(), schema) == 0 &&
         applied[schema.size()] == ':')) {
      return rank;
    }
    ++rank;
  }
  return -1;
}

}  // namespace

bool SchemaRegistry::IsAbstractTypedSchemaName(const std::string& name) {
  // OpenUSD v26.11: every typed schema name in this registry that has no
  // concrete prim definition (Usd.SchemaRegistry.FindConcretePrimDefinition).
  static const char* kAbstract[] = {
      "Typed", "APISchemaBase", "Imageable", "Xformable", "Boundable",
      "Gprim", "PointBased", "Curves", "BoundableLightBase",
      "NonboundableLightBase", "VolumeFieldBase", "FieldBase",
      "VolumeFieldAsset", "FieldAsset", "RenderSettingsBase"};
  for (const char* a : kAbstract) {
    if (name == a) return true;
  }
  const std::string_view kind = DomainKind(name);
  return kind == "abstractTyped" || kind == "abstractBase";
}

bool SchemaRegistry::IsConcreteTypedSchemaName(const std::string& name) const {
  if (name.empty() || IsAbstractTypedSchemaName(name)) return false;
  const std::string_view kind = DomainKind(name);
  if (!kind.empty()) return kind == "concreteTyped";
  if (IsAPISchemaTypeName(name)) return false;
  return IsKnownSchema(name);
}

SchemaRegistry::SchemaRegistry() {
#define LIGHTUSD_SUPPORTED_SCHEMA(name) known_schemas_.push_back(name);
#include "schema/generated/openusd-supported-schema-names.inc"
#undef LIGHTUSD_SUPPORTED_SCHEMA
  parents_ = {
      {"Xform", "Xformable"}, {"Xformable", "Imageable"},
      {"Curves", "PointBased"}, {"BasisCurves", "Curves"},
      {"NurbsCurves", "Curves"}, {"Plane", "Gprim"},
      {"Scope", "Imageable"},
      {"Boundable", "Xformable"}, {"Gprim", "Boundable"},
      {"Mesh", "Gprim"}, {"Points", "PointBased"},
      // pxr: PointBased inherits Gprim (NOT Boundable directly), so curves and
      // points must pick up Gprim's doubleSided/orientation fallbacks.
      {"PointBased", "Gprim"}, {"BasisCurves", "Curves"},
      {"NurbsCurves", "Curves"}, {"Curves", "PointBased"},
      {"Sphere", "Gprim"}, {"Cube", "Gprim"},
      {"Cylinder", "Gprim"}, {"Cone", "Gprim"},
      {"Capsule", "Gprim"}, {"Plane", "Gprim"},
      {"Camera", "Xformable"}, {"PointInstancer", "Boundable"},
      {"PhysicsJoint", "Imageable"},
      {"PhysicsFixedJoint", "PhysicsJoint"},
      {"PhysicsRevoluteJoint", "PhysicsJoint"},
      {"PhysicsPrismaticJoint", "PhysicsJoint"},
      {"PhysicsSphericalJoint", "PhysicsJoint"},
      {"PhysicsDistanceJoint", "PhysicsJoint"},
      {"GeomSubset", "Imageable"}, {"DistantLight", "Xformable"},
      {"DomeLight", "Xformable"}, {"RectLight", "Xformable"},
      {"DiskLight", "Xformable"}, {"SphereLight", "Xformable"},
      {"CylinderLight", "Xformable"},
      // Non-core OpenUSD domains (PRODUCT PARITY, not AOUSD Core):
      // UsdGeom breadth (Hermite/TetMesh/NURBS patch) + UsdVol + UsdRender.
      {"HermiteCurves", "Curves"}, {"TetMesh", "PointBased"},
      {"NurbsPatch", "PointBased"},
      {"Volume", "Gprim"},
      // pxr 25.x renamed FieldBase/FieldAsset to VolumeFieldBase/
      // VolumeFieldAsset and kept the old names as deprecated aliases; both
      // spellings resolve through the same chain here.
      {"VolumeFieldBase", "Xformable"}, {"FieldBase", "VolumeFieldBase"},
      {"VolumeFieldAsset", "FieldBase"}, {"FieldAsset", "VolumeFieldAsset"},
      {"Field3DAsset", "FieldAsset"}, {"OpenVDBAsset", "FieldAsset"},
      {"ParticleField", "Gprim"},
      {"ParticleField3DGaussianSplat", "ParticleField"},
      // UsdRender types inherit Typed in pxr (NOT Imageable/Xformable): no
      // visibility/purpose fallbacks. Typed is a terminal marker with no
      // properties; recording it makes the ancestry KNOWN so consumers can
      // derive non-Xformable/non-Gprim placement from the registry.
      {"RenderSettingsBase", "Typed"},
      {"RenderSettings", "RenderSettingsBase"},
      {"RenderProduct", "RenderSettingsBase"},
      {"RenderVar", "Typed"},
      {"RenderPass", "Typed"},
      {"ClipsAPI", "Typed"},
      {"CollectionAPI", "Typed"},
      {"MaterialBindingAPI", "Typed"},
      {"LightListAPI", "Typed"},
      {"LightAPI", "Typed"},
      {"GenerativeProcedural", "Boundable"},
      {"Backdrop", "Typed"},
  };

  auto add = [&](const char* schema, const char* name, const char* type,
                 Value fallback) {
    SchemaPropertyDefinition d;
    d.schema_type = schema;
    d.name = name;
    d.type_name = type;
    d.fallback = std::move(fallback);
    d.has_fallback = true;
    d.instance_marker_pos = d.name.find("__INSTANCE__");
    properties_.push_back(std::move(d));
  };
  auto declare = [&](const char* schema, const char* name, const char* type) {
    SchemaPropertyDefinition d;
    d.schema_type = schema;
    d.name = name;
    d.type_name = type;
    d.instance_marker_pos = d.name.find("__INSTANCE__");
    properties_.push_back(std::move(d));
  };

#define AOUSD_SCHEMA_FALLBACK(schema, name, type, fallback) \
  add(schema, name, type, fallback);
#define AOUSD_SCHEMA_DECLARE(schema, name, type) declare(schema, name, type);
#include "schema/generated/aousd-core-schema-definitions.inc"
#undef AOUSD_SCHEMA_DECLARE
#undef AOUSD_SCHEMA_FALLBACK

  // Current OpenUSD spells ColorSpaceDefinitionAPI as a multiple-apply API.
  // Keep the generated AOUSD 1.0.1 unqualified declarations above for read
  // compatibility and add the standard instantiated property namespace.
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:name",
      "token", SchemaToken("custom"));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:redChroma",
      "float2", Value::MakeFloat2(1.0f, 0.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:greenChroma",
      "float2", Value::MakeFloat2(0.0f, 1.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:blueChroma",
      "float2", Value::MakeFloat2(0.0f, 0.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:whitePoint",
      "float2", Value::MakeFloat2(1.0f / 3.0f, 1.0f / 3.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:gamma",
      "float", Value(1.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:linearBias",
      "float", Value(0.0f));
  // CollisionGroup includes CollectionAPI:colliders without an authored API
  // list. AOUSD Core 1.0.1 / 1.1 define includeRoot's fallback as false;
  // OpenUSD 26.11's generated schema currently leaves it unset.
  add("PhysicsCollisionGroup", "collection:colliders:expansionRule", "token",
      SchemaToken("expandPrims"));
  add("PhysicsCollisionGroup", "collection:colliders:includeRoot", "bool",
      Value(false));
  declare("PhysicsCollisionGroup", "collection:colliders:includes", "relationship");
  declare("PhysicsCollisionGroup", "collection:colliders:excludes", "relationship");

  // UsdLux common light inputs (each concrete light carries them; values
  // from pxr's usdLux schema.usda). DistantLight overrides intensity.
  {
    const char* kLights[] = {"DistantLight", "DomeLight",  "RectLight",
                             "DiskLight",    "SphereLight", "CylinderLight"};
    for (const char* lt : kLights) {
      add(lt, "inputs:intensity", "float",
          Value(std::string(lt) == "DistantLight" ? 50000.0f : 1.0f));
      add(lt, "inputs:exposure", "float", Value(0.0f));
      add(lt, "inputs:diffuse", "float", Value(1.0f));
      add(lt, "inputs:specular", "float", Value(1.0f));
      add(lt, "inputs:normalize", "bool", Value(false));
      add(lt, "inputs:color", "color3f", Value::MakeColor3f(1.0f, 1.0f, 1.0f));
      add(lt, "inputs:enableColorTemperature", "bool", Value(false));
      add(lt, "inputs:colorTemperature", "float", Value(6500.0f));
    }
  }
  add("DistantLight", "inputs:angle", "float", Value(0.53f));
  add("DomeLight", "inputs:texture:format", "token", SchemaToken("automatic"));
  add("SphereLight", "inputs:radius", "float", Value(0.5f));
  add("SphereLight", "treatAsPoint", "bool", Value(false));
  add("RectLight", "inputs:width", "float", Value(1.0f));
  add("RectLight", "inputs:height", "float", Value(1.0f));
  add("DiskLight", "inputs:radius", "float", Value(0.5f));
  add("CylinderLight", "inputs:length", "float", Value(1.0f));
  add("CylinderLight", "inputs:radius", "float", Value(0.5f));
  add("CylinderLight", "treatAsLine", "bool", Value(false));

  // UsdLux applied APIs.
  add("ShapingAPI", "inputs:shaping:cone:angle", "float", Value(90.0f));
  add("ShapingAPI", "inputs:shaping:cone:softness", "float", Value(0.0f));
  add("ShapingAPI", "inputs:shaping:focus", "float", Value(0.0f));
  add("ShadowAPI", "inputs:shadow:enable", "bool", Value(true));
  add("ShadowAPI", "inputs:shadow:color", "color3f",
      Value::MakeColor3f(0.0f, 0.0f, 0.0f));
  add("ShadowAPI", "inputs:shadow:distance", "float", Value(-1.0f));
  add("ShadowAPI", "inputs:shadow:falloff", "float", Value(-1.0f));
  add("ShadowAPI", "inputs:shadow:falloffGamma", "float", Value(1.0f));

  // UsdRender defaults.
  add("RenderSettingsBase", "resolution", "int2", Value::MakeInt2(2048, 1080));
  add("RenderSettingsBase", "pixelAspectRatio", "float", Value(1.0f));
  add("RenderSettingsBase", "aspectRatioConformPolicy", "token",
      SchemaToken("expandAperture"));
  add("RenderSettingsBase", "dataWindowNDC", "float4",
      Value::MakeFloat4(0.0f, 0.0f, 1.0f, 1.0f));
  add("RenderSettingsBase", "instantaneousShutter", "bool", Value(false));
  add("RenderSettingsBase", "disableMotionBlur", "bool", Value(false));
  add("RenderSettingsBase", "disableDepthOfField", "bool", Value(false));
  add("RenderSettings", "includedPurposes", "token[]",
      Value::MakeTokenArray({"default", "render"}));
  add("RenderSettings", "materialBindingPurposes", "token[]",
      Value::MakeTokenArray({"full", ""}));
  add("RenderVar", "dataType", "token", SchemaToken("color3f"));
  add("RenderVar", "sourceName", "string", Value(std::string("")));
  add("RenderVar", "sourceType", "token", SchemaToken("raw"));
  add("RenderProduct", "productType", "token", SchemaToken("raster"));
  add("RenderProduct", "productName", "token", SchemaToken(""));
  add("RenderPass", "collection:renderVisibility:includeRoot", "bool",
      Value(true));
  add("RenderPass", "collection:cameraVisibility:includeRoot", "bool",
      Value(true));

  // Retain the earlier BackPlate luma spellings for authored layers.
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:gain", "float3",
      Value::MakeFloat3(1.0f, 1.0f, 1.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:lift", "float3",
      Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:gamma", "float3",
      Value::MakeFloat3(1.0f, 1.0f, 1.0f));
  add("SemanticsLabelsAPI", "semantics:labels:__INSTANCE__", "token[]",
      Value::MakeTokenArray({}));

  // Data-driven declarations for the remaining schemas implemented by next.
  // Declarations populate HasProperty()/property-name queries without
  // inventing a fallback where the schema intentionally has none.
  const struct Decl { const char* schema; const char* name; const char* type; }
      declarations[] = {
          {"PointBased", "widths", "float[]"},
          {"Shader", "info:id", "token"},
          {"NodeGraph", "outputs", "namespace"},
          {"Material", "outputs:surface", "token"},
          {"Material", "outputs:displacement", "token"},
          {"Material", "outputs:volume", "token"},
          {"SkelBindingAPI", "skel:jointIndices", "int[]"},
          {"SkelBindingAPI", "skel:jointWeights", "float[]"},
          // UsdRender relationships + optional token.
          {"RenderSettingsBase", "camera", "relationship"},
          {"RenderSettings", "products", "relationship"},
          {"RenderSettings", "renderingColorSpace", "token"},
          {"RenderProduct", "orderedVars", "relationship"},
          {"RenderPass", "passType", "token"},
          {"RenderPass", "command", "string[]"},
          {"RenderPass", "renderSource", "relationship"},
          {"RenderPass", "inputPasses", "relationship"},
          {"RenderPass", "fileName", "asset"},
      };
  for (const Decl& declaration : declarations) {
    declare(declaration.schema, declaration.name, declaration.type);
  }

  add("Shader", "info:implementationSource", "token", SchemaToken("id"));
  // The OpenUSD 26.11 catalog supplies local declarations and ancestry for
  // these domains. Replace old definitions of those properties even when the
  // catalog moved them to a parent. Keep compatibility-only names elsewhere.
  const std::map<std::string_view, std::string_view> domain_parents(
      std::begin(kDomainParents), std::end(kDomainParents));
  parents_.erase(std::remove_if(parents_.begin(), parents_.end(),
                                [&](const auto& entry) {
                                  return domain_parents.count(entry.first) != 0;
                                }),
                 parents_.end());
  for (const auto& [name, parent] : kDomainParents) {
    if (std::find(known_schemas_.begin(), known_schemas_.end(), name) ==
        known_schemas_.end()) {
      known_schemas_.emplace_back(name);
    }
    if (!parent.empty()) parents_.emplace_back(name, parent);
  }

  std::set<std::pair<std::string_view, std::string_view>> defined;
  for (const auto& property : kDomainProperties) {
    defined.emplace(property.schema, property.name);
  }
  const auto covered = [&](const SchemaPropertyDefinition& definition) {
    std::string_view schema = definition.schema_type;
    for (auto parent = domain_parents.find(schema);
         parent != domain_parents.end(); parent = domain_parents.find(schema)) {
      if (defined.count({schema, definition.name})) return true;
      schema = parent->second;
    }
    return false;
  };
  properties_.erase(std::remove_if(properties_.begin(), properties_.end(), covered),
                    properties_.end());

  for (const auto& property : kDomainProperties) {
    if (!property.fallback) {
      declare(property.schema, property.name, property.type);
      continue;
    }
    const std::string_view type(property.type);
    const bool array = type.size() > 2 && type.substr(type.size() - 2) == "[]";
    const TypeId id = GetTypeIdFromName(type.data(), type.size() - (array ? 2 : 0));
    Lexer lexer(property.fallback, std::strlen(property.fallback));
    ParseResult parsed = array ? ParseArrayValue(lexer, id) : ParseValue(lexer, id);
    assert(parsed.success && "Generated schema fallback must parse");
    if (parsed.success) {
      add(property.schema, property.name, property.type, std::move(parsed.value));
    } else {
      declare(property.schema, property.name, property.type);
    }
  }
}

const SchemaPropertyDefinition* SchemaRegistry::FindProperty(
    const PrimSpec& prim, const std::string& property_name) const {
  // Ancestry chain as POINTERS into strings the registry (or the prim) already
  // owns. This used to build a std::vector<std::string> and copy every type
  // name into it on every call; FindProperty is called for essentially every
  // property of every prim, so that allocation alone was a measurable share of
  // a material-heavy load. Chains are a handful of entries deep -- the array
  // covers far more than any registered schema needs, and a chain that somehow
  // exceeded it simply stops extending (the same answer the old loop gave when
  // it ran out of parents).
  const std::string* schemas[16];
  size_t schema_count = 0;
  // An abstract or API typeName leaves the prim typeless (Core 1.1 "Typed
  // Schemas"): no typed chain at all.
  const std::string* current =
      IsConcreteTypedSchemaName(prim.type_name()) ? &prim.type_name() : nullptr;
  while (current && !current->empty() && schema_count < 16) {
    schemas[schema_count++] = current;
    const std::string* next = nullptr;
    for (const auto& p : parents_) {
      if (p.first == *current) {
        next = &p.second;
        break;
      }
    }
    if (!next) break;
    current = next;
  }
  auto chain_rank = [&](const std::string& schema_type) -> int {
    for (size_t i = 0; i < schema_count; ++i) {
      if (*schemas[i] == schema_type) return static_cast<int>(i);
    }
    return -1;
  };
  // Prim definition order (Core 1.1 "The Prim Definition" / "Fallback Value
  // Resolution"): the typed schema (most-derived first) is strongest, then
  // each applied schema in list-op resolved order. Pick the strongest
  // matching definition rather than the first one registered.
  const auto applied = AppliedSchemas(prim);
  const SchemaPropertyDefinition* best = nullptr;
  int best_rank = 0;
  for (const SchemaPropertyDefinition& def : properties_) {
    if (!MatchDefinitionName(applied, def, property_name)) continue;
    int rank = chain_rank(def.schema_type);
    if (rank == 0) return &def;
    if (rank < 0) {
      const int api_rank = AppliedSchemaRank(applied, def.schema_type);
      if (api_rank < 0) continue;
      rank = 16 + api_rank;
    }
    if (!best || rank < best_rank) {
      best = &def;
      best_rank = rank;
    }
  }
  return best;
}

std::vector<std::string> SchemaRegistry::PropertyNames(
    const PrimSpec& prim) const {
  std::vector<std::string> result;
  std::vector<std::string> schemas;
  std::string current =
      IsConcreteTypedSchemaName(prim.type_name()) ? prim.type_name() : "";
  while (!current.empty()) {
    schemas.push_back(current);
    auto it = std::find_if(parents_.begin(), parents_.end(),
                           [&](const auto& p) { return p.first == current; });
    if (it == parents_.end()) break;
    current = it->second;
  }
  const auto applied_schemas = AppliedSchemas(prim);
  const auto applies = [&](const SchemaPropertyDefinition& def) {
    return std::find(schemas.begin(), schemas.end(), def.schema_type) !=
               schemas.end() ||
           AppliedSchemaRank(applied_schemas, def.schema_type) >= 0;
  };
  for (const SchemaPropertyDefinition& def : properties_) {
    if (!applies(def)) continue;
    std::string name = def.name;
    const std::string marker = "__INSTANCE__";
    const size_t marker_pos = name.find(marker);
    if (marker_pos != std::string::npos) {
      for (const std::string& applied : applied_schemas) {
        const std::string prefix = def.schema_type + ":";
        if (applied.compare(0, prefix.size(), prefix) != 0) continue;
        std::string instantiated = name;
        instantiated.replace(marker_pos, marker.size(),
                             applied.substr(prefix.size()));
        if (std::find(result.begin(), result.end(), instantiated) ==
            result.end()) {
          result.push_back(std::move(instantiated));
        }
      }
      continue;
    }
    if (std::find(result.begin(), result.end(), name) == result.end()) {
      result.push_back(std::move(name));
    }
  }
  return result;
}

bool SchemaRegistry::InheritsFrom(const std::string& schema_type,
                                  const std::string& ancestor) const {
  std::string current = schema_type;
  // The parents table is a forest; a generous hop cap guards against an
  // accidentally-authored cycle.
  for (int hops = 0; hops < 32 && !current.empty(); ++hops) {
    if (current == ancestor) return true;
    auto it = std::find_if(parents_.begin(), parents_.end(),
                           [&](const auto& p) { return p.first == current; });
    if (it == parents_.end()) return false;
    current = it->second;
  }
  return false;
}

bool SchemaRegistry::HasParentEntry(const std::string& schema_type) const {
  return std::find_if(parents_.begin(), parents_.end(), [&](const auto& p) {
           return p.first == schema_type;
         }) != parents_.end();
}

bool SchemaRegistry::IsKnownSchema(const std::string& schema_type) const {
  if (std::find(known_schemas_.begin(), known_schemas_.end(), schema_type) !=
      known_schemas_.end()) return true;
  if (std::find_if(properties_.begin(), properties_.end(), [&](const auto& p) {
        return p.schema_type == schema_type;
      }) != properties_.end()) return true;
  return std::find_if(parents_.begin(), parents_.end(), [&](const auto& p) {
           return p.first == schema_type || p.second == schema_type;
         }) != parents_.end();
}

std::vector<std::string> SchemaRegistry::SchemaTypes() const {
  std::vector<std::string> result;
  auto add_unique = [&](const std::string& schema) {
    if (std::find(result.begin(), result.end(), schema) == result.end())
      result.push_back(schema);
  };
  for (const std::string& schema : known_schemas_) add_unique(schema);
  for (const auto& parent : parents_) {
    add_unique(parent.first);
    add_unique(parent.second);
  }
  for (const auto& property : properties_) add_unique(property.schema_type);
  std::sort(result.begin(), result.end());
  return result;
}

const SchemaRegistry& GetSchemaRegistry() {
  static const SchemaRegistry registry;
  return registry;
}

bool ParseSchemaFamilyAndVersion(const std::string& name, std::string* family,
                                 uint32_t* version, bool* has_version) {
  if (family) *family = name;
  if (version) *version = 0;
  if (has_version) *has_version = false;
  if (name.empty()) return false;
  size_t end = name.size();
  size_t digits = end;
  while (digits > 0 && name[digits - 1] >= '0' && name[digits - 1] <= '9') --digits;
  // Need >= 1 digit, a '_' right before them, and a non-empty family.
  if (digits == end || digits < 2 || name[digits - 1] != '_') return true;
  uint64_t v = 0;
  for (size_t i = digits; i < end; ++i) {
    v = v * 10u + static_cast<uint64_t>(name[i] - '0');
    if (v > UINT32_MAX) { v = UINT32_MAX; break; }
  }
  if (family) *family = name.substr(0, digits - 1);
  if (version) *version = static_cast<uint32_t>(v);
  if (has_version) *has_version = true;
  return true;
}

namespace {

bool IsMultipleApplyAPI(const std::string& schema) {
  static const char* kMulti[] = {
      "BackPlateAPI", "CollectionAPI", "CoordSysAPI", "PhysicsLimitAPI", "PhysicsDriveAPI",
      "PhysxLimitAPI", "PhysicsMimicJointAPI", "NewtonMimicAPI",
      "UsdPhysicsLimitAPI", "SemanticsLabelsAPI", "AccessibilityAPI",
      "PrimvarSamplingAPI"};
  for (const char* m : kMulti) {
    if (schema == m) return true;
  }
  return DomainKind(schema) == "multipleApplyAPI";
}

// Built-in (typed-schema apiSchemas) and auto-applied API schemas of a prim
// type, OpenUSD 26.11 parity for the schemas next models.
std::vector<std::string> BuiltinAPISchemas(const std::string& type) {
  // Generated from the OpenUSD 26.11 domain catalogs (including usdLux and
  // usdShade), expanded transitively: e.g. DiskLight -> LightAPI ->
  // CollectionAPI:lightLink, CollectionAPI:shadowLink.
  std::vector<std::string> result;
  AppendDomainAPIs(type, result);
  return result;
}

}  // namespace

std::vector<std::string> SchemaRegistry::AppliedSchemas(
    const PrimSpec& prim) const {
  std::vector<std::string> out = BuiltinAPISchemas(prim.type_name());
  auto push = [&out](const std::string& s) {
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  };
  for (const std::string& applied : prim.meta().apiSchemas()) {
    push(applied);
    std::vector<std::string> expanded;
    AppendDomainAPIs(applied, expanded);
    for (const std::string& e : expanded) push(e);
  }
  return out;
}

bool SchemaRegistry::HasAPI(const PrimSpec& prim, const std::string& api_schema,
                            const std::string& instance) const {
  if (api_schema.empty()) return false;
  const bool multi = IsMultipleApplyAPI(api_schema);
  if (!instance.empty() && !multi) return false;
  for (const std::string& applied : AppliedSchemas(prim)) {
    if (!multi) {
      if (applied == api_schema) return true;
      continue;
    }
    if (applied.size() <= api_schema.size() + 1 ||
        applied.compare(0, api_schema.size(), api_schema) != 0 ||
        applied[api_schema.size()] != ':') {
      continue;
    }
    if (instance.empty() ||
        applied.compare(api_schema.size() + 1, std::string::npos, instance) ==
            0) {
      return true;
    }
  }
  return false;
}

bool UsdPrim::HasAPI(const std::string& api_schema,
                     const std::string& instance) const {
  return spec_ && GetSchemaRegistry().HasAPI(*spec_, api_schema, instance);
}

std::vector<std::string> UsdPrim::GetAppliedSchemas() const {
  if (!spec_) return {};
  return GetSchemaRegistry().AppliedSchemas(*spec_);
}

}  // namespace lightusd
