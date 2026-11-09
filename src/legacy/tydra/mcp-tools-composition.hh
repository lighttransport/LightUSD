#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// reference_add: Add a reference arc to a prim.
/// Args: { path: string, asset_path: string,
///         prim_path?: string, offset?: number, scale?: number }
bool ReferenceAdd(Context &ctx, const nlohmann::json &args,
                  nlohmann::json &result, std::string &err);

/// reference_list: List references on a prim.
/// Args: { path: string }
bool ReferenceList(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// reference_clear: Clear all references on a prim.
/// Args: { path: string }
bool ReferenceClear(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err);

/// payload_add: Add a payload arc to a prim.
/// Args: { path: string, asset_path: string,
///         prim_path?: string, offset?: number, scale?: number }
bool PayloadAdd(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);

/// payload_list: List payloads on a prim.
/// Args: { path: string }
bool PayloadList(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err);

/// inherit_add: Add an inherit arc.
/// Args: { path: string, prim_path: string }
bool InheritAdd(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);

/// specialize_add: Add a specialize arc.
/// Args: { path: string, prim_path: string }
bool SpecializeAdd(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// variant_list_sets: List variant sets on a prim.
/// Args: { path: string }
bool VariantListSets(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err);

/// variant_get_selection: Get variant selection for a variant set.
/// Args: { path: string, variant_set: string }
bool VariantGetSelection(Context &ctx, const nlohmann::json &args,
                         nlohmann::json &result, std::string &err);

/// variant_set_selection: Set variant selection.
/// Args: { path: string, variant_set: string, variant: string }
bool VariantSetSelection(Context &ctx, const nlohmann::json &args,
                         nlohmann::json &result, std::string &err);

/// variant_define: Define a new variant in a variant set.
/// Args: { path: string, variant_set: string, variant_name: string }
bool VariantDefine(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
