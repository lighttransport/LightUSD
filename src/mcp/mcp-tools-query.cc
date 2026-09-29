// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Query, schema and validation tools of the next MCP server.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/layer/prim-spec.hh"
#include "next/schema/schema-registry.hh"
#include "next/stage/stage.hh"
#include "next/validation/usd-validation.hh"

namespace lightusd {
namespace mcp {

namespace {

bool ContainsCI(const std::string& haystack, const std::string& needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(),
                     needle.end(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     }) != haystack.end();
}

// Map the optional `groups` argument; no `groups` -> core only, "all" ->
// every group (lusdcat --validate-all).
next::ValidationOptions ParseGroups(const json& args) {
  next::ValidationOptions opts;
  if (!args.contains("groups") || !args["groups"].is_array()) return opts;
  opts.core = false;
  for (const json& g : args["groups"]) {
    if (!g.is_string()) continue;
    const std::string name = g.get<std::string>();
    if (name == "all") return next::MakeValidateAllOptions();
    if (name == "core") opts.core = true;
    else if (name == "geom") opts.geom = true;
    else if (name == "shade") opts.shade = true;
    else if (name == "lux") opts.lux = true;
    else if (name == "physics") opts.physics = true;
    else if (name == "render") opts.render = true;
    else if (name == "package") opts.package = true;
    else if (name == "crate") opts.crate = true;
    else if (name == "arkit") opts.arkit = true;
  }
  if (!opts.core && !opts.geom && !opts.shade && !opts.lux && !opts.physics &&
      !opts.render && !opts.package && !opts.crate && !opts.arkit) {
    opts.core = true;
  }
  return opts;
}

void ValidationToJSON(const next::USDValidationResult& v, json& result) {
  result["ok"] = v.ok();
  result["error_count"] = v.error_count();
  result["warning_count"] = v.warning_count();
  result["spec_version"] = next::GetAOUSDCoreSpecVersionString();
  json groups = json::array();
  for (const std::string& g : next::GetValidationGroupNames(v.checked_groups)) {
    groups.push_back(g);
  }
  result["checked_groups"] = std::move(groups);
  json issues = json::array();
  for (const next::USDValidationIssue* issue : next::GetOrderedValidationIssues(v)) {
    issues.push_back({{"severity", issue->severity == next::USDValidationSeverity::Error
                                       ? "error" : "warning"},
                      {"rule_id", issue->rule_id},
                      {"location", issue->location},
                      {"message", issue->message}});
  }
  result["issues"] = std::move(issues);
}

}  // namespace

bool QueryPrimsByType(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("type_name") || !args["type_name"].is_string()) {
    err = "Missing 'type_name' argument";
    return false;
  }
  const std::string type = args["type_name"].get<std::string>();
  json paths = json::array();
  for (const next::UsdPrim& p : NativeStage(ctx.stage)->GetPrimsOfType(type)) {
    paths.push_back(p.GetPath().str());
  }
  result["type_name"] = type;
  result["count"] = paths.size();
  result["paths"] = std::move(paths);
  return true;
}

bool SchemaListTypes(Context&, const json&, json& result, std::string&) {
  const std::vector<std::string> types = next::GetSchemaRegistry().SchemaTypes();
  result["count"] = types.size();
  result["types"] = types;
  return true;
}

bool SchemaGetType(Context&, const json& args, json& result, std::string& err) {
  if (!args.contains("type_name") || !args["type_name"].is_string()) {
    err = "Missing 'type_name' argument";
    return false;
  }
  const std::string type = args["type_name"].get<std::string>();
  const next::SchemaRegistry& reg = next::GetSchemaRegistry();
  if (!reg.IsKnownSchema(type)) {
    err = "Unknown prim type: " + type + ". Use schema_list_types for available types.";
    return false;
  }
  next::PrimSpec probe("Probe");
  probe.set_type_name(type);
  json attrs = json::array();
  for (const std::string& name : reg.PropertyNames(probe)) {
    const next::SchemaPropertyDefinition* def = reg.FindProperty(probe, name);
    json a;
    a["name"] = name;
    a["type"] = def ? def->type_name : std::string();
    if (def) a["schema"] = def->schema_type;
    if (def && def->has_fallback) a["fallback"] = ValueToJSON(def->fallback, def->type_name);
    attrs.push_back(std::move(a));
  }
  result["type_name"] = type;
  result["schema_name"] = type;
  result["attributes"] = std::move(attrs);
  result["metadata"] = {
      {"active", {{"type", "bool"}, {"description", "Whether the prim is active"}}},
      {"hidden", {{"type", "bool"}, {"description", "Whether the prim is hidden"}}},
      {"instanceable", {{"type", "bool"}, {"description", "Whether the prim is instanceable"}}},
      {"kind", {{"type", "token"}, {"description", "Kind of prim (e.g. 'component', 'group')"}}},
      {"documentation", {{"type", "string"}, {"description", "Documentation string"}}}};
  return true;
}

bool Search(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("query") || !args["query"].is_string()) {
    err = "Missing 'query' argument";
    return false;
  }
  const std::string query = args["query"].get<std::string>();
  const std::string scope = args.value("scope", std::string("all"));
  json by_name = json::array(), by_type = json::array();
  NativeStage(ctx.stage)->Traverse([&](const next::UsdPrim& p) {
    if ((scope == "names" || scope == "all") && ContainsCI(p.GetName(), query)) {
      by_name.push_back(p.GetPath().str());
    }
    if (scope == "all" && p.GetTypeName() == query) by_type.push_back(p.GetPath().str());
    return true;
  });
  result["query"] = query;
  const size_t total = by_name.size() + by_type.size();
  if (!by_name.empty()) result["byName"] = std::move(by_name);
  if (!by_type.empty()) result["byType"] = std::move(by_type);
  result["totalMatches"] = total;
  return true;
}

bool UsdValidate(Context& ctx, const json& args, json& result, std::string& err) {
  const next::ValidationOptions options = ParseGroups(args);
  next::USDValidationResult validation;
  std::string warn;
  std::string source;
  if (args.contains("data") && args["data"].is_string()) {
    std::string binary;
    if (!DecodeBase64(args["data"].get<std::string>(), &binary, &err)) return false;
    const std::string name = args.value("name", std::string("memory.usd"));
    if (!next::ValidateUSDFromMemoryAgainstAOUSDCore(
            reinterpret_cast<const uint8_t*>(binary.data()), binary.size(), name,
            options, &validation, &warn, &err)) {
      return false;
    }
    source = "data";
  } else if (args.contains("uri") && args["uri"].is_string()) {
#if defined(__EMSCRIPTEN__)
    err = "Validating from a file URI is not supported in this build";
    return false;
#else
    lightusd_load_options opts;
    lightusd_load_options_init(&opts);
    lightusd_stage* stage = nullptr;
    const std::string uri = args["uri"].get<std::string>();
    if (lightusd_stage_load(uri.c_str(), &opts, &stage) != LIGHTUSD_OK) {
      err = std::string("Failed to load USD: ") + lightusd_last_error();
      return false;
    }
    StageRef ref(stage);
    validation = next::ValidateLayerAgainstAOUSDCore(
        *NativeStage(ref)->GetRootLayer(), options);
    source = "uri";
#endif
  } else if (args.contains("layer_uuid") && args["layer_uuid"].is_string()) {
    const std::string uuid = args["layer_uuid"].get<std::string>();
    auto it = ctx.layers.find(uuid);
    if (it == ctx.layers.end()) {
      err = "No layer found for layer_uuid: " + uuid;
      return false;
    }
    validation = next::ValidateLayerAgainstAOUSDCore(
        *NativeStage(it->second.stage)->GetRootLayer(), options);
    source = "layer_uuid";
  } else {
    if (!ctx.stage) {
      err = "No input provided and no stage loaded. Pass `data`, `uri`, "
            "`layer_uuid`, or load a stage first.";
      return false;
    }
    validation = next::ValidateLayerAgainstAOUSDCore(
        *NativeStage(ctx.stage)->GetRootLayer(), options);
    source = "stage";
  }
  ValidationToJSON(validation, result);
  result["source"] = source;
  if (!warn.empty()) result["warn"] = warn;
  return true;
}

}  // namespace mcp
}  // namespace lightusd
