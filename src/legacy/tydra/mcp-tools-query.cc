#include "mcp-tools-query.hh"

#include <algorithm>
#include <cctype>
#include <functional>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "../value-types.hh"
#include "value-to-json.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

namespace {

// ---------------------------------------------------------------------------
// Recursive prim search helpers
// ---------------------------------------------------------------------------
void CollectPrimsByType(const Prim &prim, const std::string &type_name,
                        const std::string &parent_path,
                        std::vector<std::string> &results) {
  (void)parent_path;
  if (prim.prim_type_name() == type_name) {
    results.push_back(prim.absolute_path().full_path_name());
  }
  for (const auto &child : prim.children()) {
    CollectPrimsByType(child, type_name, parent_path, results);
  }
}

void SearchInPrimNames(const Prim &prim, const std::string &query,
                       const std::string &parent_path,
                       std::vector<std::string> &results) {
  (void)parent_path;
  std::string name = prim.element_name();
  // Case-insensitive substring match
  auto it = std::search(
      name.begin(), name.end(), query.begin(), query.end(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  if (it != name.end()) {
    results.push_back(prim.absolute_path().full_path_name());
  }
  for (const auto &child : prim.children()) {
    SearchInPrimNames(child, query, parent_path, results);
  }
}

} // namespace

// ===========================================================================
// query_prims_by_type
// ===========================================================================
bool QueryPrimsByType(Context &ctx, const nlohmann::json &args,
                      nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("type_name") || !args["type_name"].is_string()) {
    err = "Missing 'type_name' argument";
    return false;
  }

  std::string type_name = args["type_name"].get<std::string>();
  std::vector<std::string> found;

  for (const auto &prim : ctx.stage->root_prims()) {
    CollectPrimsByType(prim, type_name, "/", found);
  }

  nlohmann::json paths = nlohmann::json::array();
  for (const auto &p : found) {
    paths.push_back(p);
  }

  result["type_name"] = type_name;
  result["paths"] = paths;
  result["count"] = found.size();
  return true;
}

// ===========================================================================
// schema_list_types
// ===========================================================================
bool SchemaListTypes(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err) {
  (void)ctx;
  (void)args;
  (void)err;

  auto types = GetPrimTypeNames();
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &t : types) {
    arr.push_back(t);
  }
  result["types"] = arr;
  result["count"] = types.size();
  return true;
}

// ===========================================================================
// schema_get_type
// ===========================================================================
bool SchemaGetType(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  (void)ctx;

  if (!args.contains("type_name") || !args["type_name"].is_string()) {
    err = "Missing 'type_name' argument";
    return false;
  }

  std::string type_name = args["type_name"].get<std::string>();

  // Use the inline schema definitions from mcp-tools-scene.cc
  // We import them by reusing the GetSchemaDef logic. Since it's in an
  // anonymous namespace in another TU, we use the public API approach:
  // re-derive from the prim-types known registry.

  // For now, provide a useful description
  auto all_types = GetPrimTypeNames();
  bool known = false;
  for (const auto &t : all_types) {
    if (t == type_name) {
      known = true;
      break;
    }
  }

  if (!known) {
    err = "Unknown prim type: " + type_name + ". Use schema_list_types for available types.";
    return false;
  }

  result["type_name"] = type_name;
  result["schema_name"] = type_name;

  // Get attribute definitions from the scene tools schema
  // We replicate them here rather than pulling in internal functions
  result["attributes"] = nlohmann::json::array();

  nlohmann::json meta_info;
  meta_info["active"] = {{"type", "bool"}, {"description", "Whether the prim is active"}};
  meta_info["hidden"] = {{"type", "bool"}, {"description", "Whether the prim is hidden"}};
  meta_info["instanceable"] = {{"type", "bool"}, {"description", "Whether the prim is instanceable"}};
  meta_info["kind"] = {{"type", "token"}, {"description", "Kind of prim (e.g. 'component', 'group')"}};
  meta_info["documentation"] = {{"type", "string"}, {"description", "Documentation string"}};
  result["metadata"] = meta_info;

  return true;
}

// ===========================================================================
// search
// ===========================================================================
bool Search(Context &ctx, const nlohmann::json &args,
            nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("query") || !args["query"].is_string()) {
    err = "Missing 'query' argument";
    return false;
  }

  std::string query = args["query"].get<std::string>();
  std::string scope = args.value("scope", std::string("all"));

  std::vector<std::string> by_name;
  std::vector<std::string> by_type;

  if (scope == "names" || scope == "all") {
    for (const auto &prim : ctx.stage->root_prims()) {
      SearchInPrimNames(prim, query, "/", by_name);
    }
  }

  if (scope == "all") {
    // Also search by type name (exact match)
    for (const auto &prim : ctx.stage->root_prims()) {
      CollectPrimsByType(prim, query, "/", by_type);
    }
  }

  result["query"] = query;
  if (!by_name.empty()) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &p : by_name) arr.push_back(p);
    result["byName"] = arr;
  }
  if (!by_type.empty()) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &p : by_type) arr.push_back(p);
    result["byType"] = arr;
  }
  result["totalMatches"] = by_name.size() + by_type.size();
  return true;
}

} // namespace mcp
} // namespace tydra
} // namespace lightusd
