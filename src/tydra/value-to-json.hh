#pragma once

#include <string>
#include <vector>

#include "../value-types.hh"
#include "../core/prim-metas.hh"
#include "../core/prim-spec.hh"
#include "../minijson.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {

/// Canonical minijson APIs. The nlohmann overloads below are retained only
/// for source compatibility with older MCP/JavaScript integrations.
minijson::Value ValueToMiniJSON(const value::Value &val, uint32_t depth = 0);
nonstd::optional<value::Value> MiniJSONToValue(
    const minijson::Value &j, std::string *err = nullptr,
    uint32_t depth = 0);
minijson::Value ValueToPlainMiniJSON(const value::Value &val);
minijson::Value ValueTypeToMiniJSONSchema(const std::string &type_name,
                                          uint32_t depth = 0);
minijson::Value PrimMetaToMiniJSON(const PrimMeta &meta);
minijson::Value PrimSpecToMiniJSON(const PrimSpec &ps,
                                   uint32_t max_depth = 1,
                                   uint32_t depth = 0);
minijson::Value PropertyToMiniJSON(const Property &prop);
minijson::Value AttributeToMiniJSON(const Attribute &attr);
minijson::Value RelationshipToMiniJSON(const Relationship &rel);

/// Convert a value::Value to structured JSON representation.
///
/// Format:
///   Scalar:   {"type": "float", "value": 1.5}
///   Compound: {"type": "float3", "value": [1, 2, 3]}
///   Array:    {"type": "float3[]", "value": [[1,2,3], [4,5,6]]}
///   Matrix:   {"type": "matrix4d", "value": [[1,0,0,0],...]}
///   Quat:     {"type": "quatf", "value": [x, y, z, w]}
///   Asset:    {"type": "asset", "value": {"assetPath":"...", "resolvedPath":"..."}}
///   Dict:     {"type": "dictionary", "value": {k: v, ...}}
///   Blocked:  {"type": "None"}
///   TimeSamples: {"type": "timesamples", "value": {t: {type, value}, ...}}
/// `depth` is the internal recursion depth (callers pass 0); it guards against
/// stack overflow on pathologically/maliciously deeply nested values.
nlohmann::json ValueToJSON(const value::Value &val, uint32_t depth = 0);

/// Convert JSON back to value::Value.
nonstd::optional<value::Value> JSONToValue(const nlohmann::json &j,
                                           std::string *err = nullptr,
                                           uint32_t depth = 0);

/// Convert to plain JSON (no type wrapper) for convenience.
nlohmann::json ValueToPlainJSON(const value::Value &val);

/// Get a JSON Schema description for a USD type name.
nlohmann::json ValueTypeToJSONSchema(const std::string &type_name,
                                     uint32_t depth = 0);

/// Convert PrimMeta to JSON
nlohmann::json PrimMetaToJSON(const PrimMeta &meta);

/// Convert layer/PrimSpec internals to deterministic JSON for debugging
/// composition and MCP/JS inspection.
nlohmann::json PrimSpecToJSON(const PrimSpec &ps, uint32_t max_depth = 1,
                              uint32_t depth = 0);
nlohmann::json PropertyToJSON(const Property &prop);
nlohmann::json AttributeToJSON(const Attribute &attr);
nlohmann::json RelationshipToJSON(const Relationship &rel);

/// List all known role type names
std::vector<std::string> GetRoleTypeNames();

/// List all known prim type names
std::vector<std::string> GetPrimTypeNames();

} // namespace tydra
} // namespace lightusd
