#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// stage_new: Create a new empty stage.
/// Args: { upAxis?, defaultPrim?, metersPerUnit?, timeCodesPerSecond?,
///         framesPerSecond?, startTimeCode?, endTimeCode? }
bool StageNew(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err);

/// stage_load: Load a USD file into the session stage.
/// Args: { uri: string, options?: { loadPayloads?: bool, ... } }
bool StageLoad(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err);

/// stage_load_data: Load USD from base64 data.
/// Args: { data: string, name?: string, format?: string }
bool StageLoadData(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// stage_export: Export stage to file.
/// Args: { uri: string, format?: "usda"|"usdc"|"usdz" }
bool StageExport(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err);

/// stage_to_string: Export stage to USDA string.
/// Args: { format?: "usda"|"usdc" }
bool StageToString(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// stage_info: Get stage metadata.
/// Args: {}
bool StageInfo(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err);

// ---------------------------------------------------------------------------
// Scene graph tools
// ---------------------------------------------------------------------------

/// prim_list: List prims at/under a path.
/// Args: { path?: string, max_depth?: int, include_attributes?: bool }
bool PrimList(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err);

/// prim_get: Get full details of a prim.
/// Args: { path: string, include_attributes?: bool, include_metadata?: bool,
///         include_composition?: bool }
bool PrimGet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err);

/// prim_create: Create a new prim at path.
/// Args: { path: string, type_name: string, specifier?: "def"|"over"|"class",
///         attributes?: [...] }
bool PrimCreate(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);

/// prim_remove: Remove a prim at path.
/// Args: { path: string }
bool PrimRemove(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);

/// prim_rename: Rename a prim.
/// Args: { path: string, new_name: string }
bool PrimRename(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);

/// prim_get_metadata: Get prim metadata.
/// Args: { path: string }
bool PrimGetMetadata(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err);

// ---------------------------------------------------------------------------
// Attribute tools
// ---------------------------------------------------------------------------

/// attr_list: List attributes on a prim.
/// Args: { path: string }
bool AttrList(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err);

/// attr_get: Get attribute value (optionally at a time code).
/// Args: { path: string, attr_name: string, time?: number }
bool AttrGet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err);

/// attr_set: Set attribute value.
/// Args: { path: string, attr_name: string,
///         value: { type: string, value: any }, time?: number }
bool AttrSet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err);

/// attr_block: Block an attribute.
/// Args: { path: string, attr_name: string }
bool AttrBlock(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err);

/// attr_connections: Get or set attribute connections.
/// Args: { path: string, attr_name: string,
///         set?: [string (target paths)] }
bool AttrConnections(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
