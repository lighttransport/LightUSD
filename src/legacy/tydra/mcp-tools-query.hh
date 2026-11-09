#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// query_prims_by_type: Find all prims of a given type.
/// Args: { type_name: string }
bool QueryPrimsByType(Context &ctx, const nlohmann::json &args,
                      nlohmann::json &result, std::string &err);

/// schema_list_types: List all registered USD prim type names.
/// Args: {}
bool SchemaListTypes(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err);

/// schema_get_type: Get the schema definition for a prim type.
/// Args: { type_name: string }
bool SchemaGetType(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// search: Search prim names and attributes across the stage.
/// Args: { query: string, scope?: "names"|"attrs"|"all" }
bool Search(Context &ctx, const nlohmann::json &args,
            nlohmann::json &result, std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
