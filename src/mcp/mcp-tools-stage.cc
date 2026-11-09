// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Stage, prim and attribute tools of the next MCP server.
#include <cmath>
#include <string>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/schema/schema-registry.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"

namespace lightusd {
namespace mcp {

namespace {

std::string LastError() {
  const char* e = lightusd_last_error();
  return e ? std::string(e) : std::string("unknown error");
}

bool CheckStatus(lightusd_status st, const std::string& what, std::string* err) {
  if (st == LIGHTUSD_OK) return true;
  *err = what + ": " + LastError();
  return false;
}

std::string TakeString(lightusd_string* s) {
  std::string out;
  if (s) {
    const lightusd_sv v = lightusd_string_view(s);
    out.assign(v.data ? v.data : "", v.len);
    lightusd_string_destroy(s);
  }
  return out;
}

void StageSummary(const next::Stage& stage, json& result) {
  const next::StageMeta& m = stage.GetMeta();
  result["upAxis"] = m.upAxis;
  result["defaultPrim"] = m.defaultPrim;
  result["rootPrimCount"] = stage.GetRootPrims().size();
}

bool SetStageMetaFromArgs(lightusd_stage* stage, const json& args,
                          std::string* err) {
  if (args.contains("upAxis") && args["upAxis"].is_string()) {
    const std::string axis = args["upAxis"].get<std::string>();
    if (axis != "X" && axis != "Y" && axis != "Z") {
      *err = "upAxis must be X, Y, or Z";
      return false;
    }
    const char* v = axis.c_str();
    if (!CheckStatus(lightusd_stage_set_metadata(stage, "upAxis",
                                                 LIGHTUSD_TYPE_TOKEN, v, 1),
                     "set upAxis", err)) {
      return false;
    }
  }
  if (args.contains("defaultPrim") && args["defaultPrim"].is_string()) {
    const std::string name = args["defaultPrim"].get<std::string>();
    if (!CheckStatus(lightusd_stage_set_default_prim(stage, name.c_str()),
                     "set defaultPrim", err)) {
      return false;
    }
  }
  for (const char* key : {"metersPerUnit", "timeCodesPerSecond",
                          "framesPerSecond", "startTimeCode", "endTimeCode"}) {
    if (args.contains(key) && args[key].is_number()) {
      const double v = args[key].get<double>();
      if (!CheckStatus(lightusd_stage_set_metadata(stage, key,
                                                   LIGHTUSD_TYPE_DOUBLE, &v, 1),
                       std::string("set ") + key, err)) {
        return false;
      }
    }
  }
  return true;
}

bool LoadStage(const json& args, const uint8_t* data, size_t size,
               const std::string* filename, StageRef* out, std::string* err) {
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  opts.composed = 1;
  opts.load_payloads = 1;
  if (args.contains("options") && args["options"].is_object()) {
    const json& o = args["options"];
    if (o.contains("doComposition") && o["doComposition"].is_boolean())
      opts.composed = o["doComposition"].get<bool>() ? 1 : 0;
    if (o.contains("loadPayloads") && o["loadPayloads"].is_boolean())
      opts.load_payloads = o["loadPayloads"].get<bool>() ? 1 : 0;
  }
  const std::string format = args.value("format", std::string("auto"));
  if (format == "usda") opts.format = LIGHTUSD_FORMAT_USDA;
  else if (format == "usdc") opts.format = LIGHTUSD_FORMAT_USDC;
  else if (format == "usdz") opts.format = LIGHTUSD_FORMAT_USDZ;
  lightusd_stage* stage = nullptr;
  const lightusd_status st =
      filename ? lightusd_stage_load(filename->c_str(), &opts, &stage)
               : lightusd_stage_load_from_memory(data, size, &opts, &stage);
  if (st != LIGHTUSD_OK || !stage) {
    *err = "Failed to load USD: " + LastError();
    return false;
  }
  out->reset(stage);
  return true;
}

// Declared type of an attribute on a C prim, or "" when absent.
bool FindAttribute(Context& ctx, const json& args, PrimSite* site,
                   std::string* path, std::string* name, std::string* err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    *err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("attr_name") || !args["attr_name"].is_string()) {
    *err = "Missing 'attr_name' argument";
    return false;
  }
  *path = args["path"].get<std::string>();
  *name = args["attr_name"].get<std::string>();
  return FindPrimSite(ctx, *path, site, err);
}

next::UsdPrim NativePrim(lightusd_stage* stage, const std::string& path) {
  const next::Stage* s = lightusd_internal::BorrowNativeStage(stage);
  return s ? s->GetPrimAtPath(path) : next::UsdPrim();
}

}  // namespace

bool GetVersion(Context&, const json&, json& result, std::string&) {
  SetTextContent(result, lightusd_version_string());
  return true;
}

bool StageNew(Context& ctx, const json& args, json& result, std::string& err) {
  lightusd_stage* stage = nullptr;
  if (!CheckStatus(lightusd_stage_create(&stage), "create stage", &err)) {
    return false;
  }
  StageRef ref(stage);
  if (!SetStageMetaFromArgs(stage, args, &err)) return false;
  ctx.stage = std::move(ref);
  result["success"] = true;
  result["message"] = "Created new empty stage";
  return true;
}

bool StageLoad(Context& ctx, const json& args, json& result, std::string& err) {
#if defined(__EMSCRIPTEN__)
  (void)ctx;
  (void)args;
  (void)result;
  err = "Loading from a file URI is not supported in this build; use stage_load_data";
  return false;
#else
  if (!args.contains("uri") || !args["uri"].is_string()) {
    err = "Missing 'uri' argument";
    return false;
  }
  const std::string uri = args["uri"].get<std::string>();
  StageRef stage;
  if (!LoadStage(args, nullptr, 0, &uri, &stage, &err)) return false;
  ctx.stage = std::move(stage);
  result["success"] = true;
  result["message"] = "Loaded USD: " + uri;
  StageSummary(*NativeStage(ctx.stage), result);
  return true;
#endif
}

bool StageLoadData(Context& ctx, const json& args, json& result, std::string& err) {
  if (!args.contains("data") || !args["data"].is_string()) {
    err = "Missing 'data' argument (base64 encoded)";
    return false;
  }
  std::string binary;
  if (!DecodeBase64(args["data"].get<std::string>(), &binary, &err)) return false;
  StageRef stage;
  if (!LoadStage(args, reinterpret_cast<const uint8_t*>(binary.data()),
                 binary.size(), nullptr, &stage, &err)) {
    return false;
  }
  ctx.stage = std::move(stage);
  result["success"] = true;
  StageSummary(*NativeStage(ctx.stage), result);
  return true;
}

bool StageExport(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
#if defined(__EMSCRIPTEN__)
  (void)args;
  (void)result;
  err = "Exporting to a file URI is not supported in this build; use stage_to_string";
  return false;
#else
  if (!args.contains("uri") || !args["uri"].is_string()) {
    err = "Missing 'uri' argument";
    return false;
  }
  const std::string uri = args["uri"].get<std::string>();
  const std::string format = args.value("format", std::string("usda"));
  lightusd_save_options opts;
  lightusd_save_options_init(&opts);
  if (format == "usdc") opts.format = LIGHTUSD_FORMAT_USDC;
  else if (format == "usdz") opts.format = LIGHTUSD_FORMAT_USDZ;
  else if (format == "usda") opts.format = LIGHTUSD_FORMAT_USDA;
  else {
    err = "Unknown export format: " + format;
    return false;
  }
  if (!CheckStatus(lightusd_stage_save(ctx.stage.stage, uri.c_str(), &opts),
                   "export", &err)) {
    return false;
  }
  result["success"] = true;
  result["uri"] = uri;
  return true;
#endif
}

bool StageToString(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  const std::string format = args.value("format", std::string("usda"));
  lightusd_string* s = nullptr;
  if (format == "usdc") {
    if (!CheckStatus(lightusd_stage_export_usdc(ctx.stage.stage, &s), "export", &err)) {
      return false;
    }
    const std::string bytes = TakeString(s);
    result["format"] = "usdc";
    result["data"] = EncodeBase64(reinterpret_cast<const uint8_t*>(bytes.data()),
                                  bytes.size());
    result["length"] = bytes.size();
    return true;
  }
  if (format != "usda") {
    err = "Unknown format: " + format + " (usda or usdc)";
    return false;
  }
  if (!CheckStatus(lightusd_stage_export_usda(ctx.stage.stage, &s), "export", &err)) {
    return false;
  }
  const std::string usda = TakeString(s);
  result["usda"] = usda;
  result["length"] = usda.size();
  return true;
}

bool StageInfo(Context& ctx, const json&, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  const next::Stage& stage = *NativeStage(ctx.stage);
  const next::StageMeta& m = stage.GetMeta();
  result["upAxis"] = m.upAxis;
  result["defaultPrim"] = m.defaultPrim;
  result["metersPerUnit"] = m.metersPerUnit;
  result["timeCodesPerSecond"] = m.timeCodesPerSecond;
  result["framesPerSecond"] = m.framesPerSecond;
  result["startTimeCode"] = m.startTimeCode;
  if (m.endTimeCode_set && std::isfinite(m.endTimeCode)) {
    result["endTimeCode"] = m.endTimeCode;
  } else {
    result["endTimeCode"] = nullptr;
  }
  result["rootPrimCount"] = stage.GetRootPrims().size();
  result["totalPrimCount"] = stage.GetPrimCount();
  json roots = json::array();
  for (const next::UsdPrim& p : stage.GetRootPrims()) roots.push_back(p.GetName());
  result["rootPrims"] = std::move(roots);
  return true;
}

bool PrimList(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  const next::Stage& stage = *NativeStage(ctx.stage);
  const std::string path = args.value("path", std::string("/"));
  const int max_depth = args.value("max_depth", -1);
  const bool include_attributes = args.value("include_attributes", false);
  if (path == "/" || path.empty()) {
    json prims = json::array();
    for (const next::UsdPrim& p : stage.GetRootPrims()) {
      prims.push_back(PrimToJSON(p, ctx.stage.stage, max_depth, include_attributes));
    }
    result["path"] = "/";
    result["count"] = prims.size();
    result["prims"] = std::move(prims);
    return true;
  }
  const next::UsdPrim prim = stage.GetPrimAtPath(path);
  if (!prim) {
    json roots = json::array();
    for (const next::UsdPrim& p : stage.GetRootPrims()) roots.push_back("/" + p.GetName());
    result["error"] = "Prim not found: " + path;
    result["availableRootPrims"] = std::move(roots);
    return true;  // not a hard error (legacy contract)
  }
  result["path"] = prim.GetPath().str();
  result["prim"] = PrimToJSON(prim, ctx.stage.stage, max_depth, include_attributes);
  return true;
}

bool PrimGet(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  const next::UsdPrim prim = NativeStage(ctx.stage)->GetPrimAtPath(path);
  if (!prim) {
    err = "Prim not found: " + path;
    return false;
  }
  result["path"] = prim.GetPath().str();
  result["prim"] = PrimToJSON(prim, ctx.stage.stage, -1,
                              args.value("include_attributes", true));
  if (args.value("include_metadata", false)) {
    result["metadata"] = PrimMetaToJSON(prim);
  }
  return true;
}

bool PrimCreate(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  const std::string type_name = args.value("type_name", std::string("Xform"));
  const std::string specifier = args.value("specifier", std::string("def"));
  uint8_t spec = 0;
  if (specifier == "over") spec = 1;
  else if (specifier == "class") spec = 2;
  else if (specifier != "def") {
    err = "specifier must be def, over, or class";
    return false;
  }
  if (!CheckStatus(lightusd_stage_define_prim(ctx.stage.stage, path.c_str(),
                                              type_name.c_str(), spec, nullptr),
                   "define prim", &err)) {
    return false;
  }
  result["success"] = true;
  result["path"] = path;
  result["type"] = type_name;
  if (!type_name.empty() && !next::GetSchemaRegistry().IsKnownSchema(type_name)) {
    result["warning"] = "Unregistered prim type: " + type_name;
  }
  return true;
}

bool PrimRemove(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  if (!CheckStatus(lightusd_stage_remove_prim(ctx.stage.stage, path.c_str()),
                   "remove prim", &err)) {
    return false;
  }
  result["success"] = true;
  result["removed"] = path;
  return true;
}

bool PrimRename(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("new_name") || !args["new_name"].is_string()) {
    err = "Missing 'new_name' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  const std::string new_name = args["new_name"].get<std::string>();
  if (!CheckStatus(lightusd_stage_rename_prim(ctx.stage.stage, path.c_str(),
                                              new_name.c_str()),
                   "rename prim", &err)) {
    return false;
  }
  const size_t slash = path.find_last_of('/');
  result["success"] = true;
  result["old_path"] = path;
  result["path"] = (slash == 0 ? std::string("/") : path.substr(0, slash) + "/") +
                   new_name;
  return true;
}

bool PrimGetMetadata(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  const next::UsdPrim prim = NativeStage(ctx.stage)->GetPrimAtPath(path);
  if (!prim) {
    err = "Prim not found: " + path;
    return false;
  }
  result["path"] = prim.GetPath().str();
  result["metadata"] = PrimMetaToJSON(prim);
  return true;
}

bool AttrList(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  const std::string path = args["path"].get<std::string>();
  const next::UsdPrim prim = NativeStage(ctx.stage)->GetPrimAtPath(path);
  if (!prim) {
    err = "Prim not found: " + path;
    return false;
  }
  const json j = PrimToJSON(prim, ctx.stage.stage, 0, true);
  json attrs = j.contains("attributes") ? j["attributes"] : json::array();
  // Schema properties the prim does not author (fallback values apply).
  if (const next::PrimSpec* spec = prim.GetPrimSpec()) {
    const next::SchemaRegistry& reg = next::GetSchemaRegistry();
    for (const std::string& name : reg.PropertyNames(*spec)) {
      if (prim.HasAuthoredProperty(name)) continue;
      const next::SchemaPropertyDefinition* def = reg.FindProperty(*spec, name);
      json a;
      a["name"] = name;
      a["type"] = def ? def->type_name : std::string();
      a["hasValue"] = false;
      a["schemaFallback"] = true;
      if (def && def->has_fallback) {
        a["fallback"] = ValueToJSON(def->fallback, def->type_name);
      }
      attrs.push_back(std::move(a));
    }
  }
  result["path"] = prim.GetPath().str();
  result["primType"] = prim.GetTypeName();
  result["count"] = attrs.size();
  result["attributes"] = std::move(attrs);
  return true;
}

bool AttrGet(Context& ctx, const json& args, json& result, std::string& err) {
  PrimSite site;
  std::string path, name;
  if (!FindAttribute(ctx, args, &site, &path, &name, &err)) return false;
  const next::UsdPrim prim = NativePrim(site.stage, path);
  const size_t max_elements =
      args.value("max_elements", static_cast<size_t>(1000));
  result["path"] = prim.GetPath().str();
  result["attr_name"] = name;
  if (!site.layer.empty()) result["layer"] = site.layer;
  const lightusd_prim cprim = lightusd_stage_prim_at_path(site.stage, path.c_str());
  lightusd_value_view view{};
  const bool blocked =
      lightusd_attr_get(cprim, name.c_str(), &view) == LIGHTUSD_OK && view.is_block;
  const std::string type = PropertyTypeName(prim, name);
  if (args.contains("time") && args["time"].is_number() &&
      prim.HasTimeSamples(name)) {
    const double t = args["time"].get<double>();
    const next::Value v = prim.GetInterpolatedValue(name, t);
    result["time"] = t;
    result["hasValue"] = true;
    result["value"] = ValueToJSON(v, type, max_elements);
  } else if (const next::Value* v = prim.GetPropertyValueOrEarliestTimeSample(name)) {
    result["hasValue"] = !blocked;
    result["value"] = blocked ? json({{"type", "None"}})
                              : ValueToJSON(*v, type, max_elements);
  } else if (prim.HasProperty(name) || !type.empty()) {
    result["hasValue"] = false;
    result["value"] = nullptr;
  } else {
    err = "Attribute '" + name + "' not found on prim " + path;
    return false;
  }
  if (prim.HasTimeSamples(name)) {
    result["timeSampleTimes"] = prim.GetTimeSampleTimes(name);
  }
  result["isBlocked"] = blocked;
  return true;
}

bool AttrSet(Context& ctx, const json& args, json& result, std::string& err) {
  PrimSite site;
  std::string path, name;
  if (!FindAttribute(ctx, args, &site, &path, &name, &err)) return false;
  if (!args.contains("value")) {
    err = "Missing 'value' argument";
    return false;
  }
  std::string type_name;
  next::Value value;
  if (!ValueFromJSON(args["value"], &type_name, &value, &err)) {
    err = "Failed to parse value: " + err;
    return false;
  }
  const bool uniform = args.value("uniform", false);
  const bool custom = args.value("custom", false);
  if (!lightusd_internal::SetNativeAttribute(site.stage, path.c_str(), name,
                                             std::move(value), type_name,
                                             uniform, custom, &err)) {
    return false;
  }
  result["success"] = true;
  result["path"] = path;
  result["attr_name"] = name;
  result["type"] = type_name;
  if (!site.layer.empty()) result["layer"] = site.layer;
  return true;
}

bool AttrBlock(Context& ctx, const json& args, json& result, std::string& err) {
  PrimSite site;
  std::string path, name;
  if (!FindAttribute(ctx, args, &site, &path, &name, &err)) return false;
  if (!CheckStatus(lightusd_attr_block(site.stage, path.c_str(), name.c_str()),
                   "block attribute", &err)) {
    return false;
  }
  result["success"] = true;
  result["path"] = path;
  result["attr_name"] = name;
  return true;
}

bool AttrConnections(Context& ctx, const json& args, json& result, std::string& err) {
  PrimSite site;
  std::string path, name;
  if (!FindAttribute(ctx, args, &site, &path, &name, &err)) return false;
  if (args.contains("add") && args["add"].is_string()) {
    const std::string target = args["add"].get<std::string>();
    if (!CheckStatus(lightusd_attr_add_connection(site.stage, path.c_str(),
                                                  name.c_str(), target.c_str()),
                     "add connection", &err)) {
      return false;
    }
  }
  const lightusd_prim cprim = lightusd_stage_prim_at_path(site.stage, path.c_str());
  json targets = json::array();
  const size_t n = lightusd_attr_connection_count(cprim, name.c_str());
  for (size_t i = 0; i < n; ++i) {
    const lightusd_sv sv = lightusd_attr_connection(cprim, name.c_str(), i);
    targets.push_back(std::string(sv.data ? sv.data : "", sv.len));
  }
  result["path"] = path;
  result["attr_name"] = name;
  result["connections"] = std::move(targets);
  result["count"] = n;
  return true;
}

}  // namespace mcp
}  // namespace lightusd
