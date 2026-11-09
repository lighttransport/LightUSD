// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
#include "lightusd-mcp.hh"

#include <exception>
#include <istream>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>

#include "mcp-context.hh"
#include "mcp-tool-schemas.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"

namespace lightusd {
namespace mcp {

namespace {

constexpr size_t kMaxMessageBytes = 16 * 1024 * 1024;
constexpr const char* kProtocolVersion = "2025-06-18";

// Tools of the shared legacy schema set that the next product does not
// provide (scripting engine, image processing, unimplemented stub).
const std::set<std::string>& UnsupportedTools() {
  static const std::set<std::string> kTools = {
      "run_script", "texture_resize", "texture_repack", "load_usd_layer_from_asset"};
  return kTools;
}

const std::unordered_map<std::string, ToolFn>& ToolTable() {
  static const std::unordered_map<std::string, ToolFn> kTable = {
      {"get_version", &GetVersion},
      {"stage_new", &StageNew},
      {"stage_load", &StageLoad},
      {"stage_load_data", &StageLoadData},
      {"stage_export", &StageExport},
      {"stage_to_string", &StageToString},
      {"stage_info", &StageInfo},
      {"prim_list", &PrimList},
      {"prim_get", &PrimGet},
      {"prim_create", &PrimCreate},
      {"prim_remove", &PrimRemove},
      {"prim_rename", &PrimRename},
      {"prim_get_metadata", &PrimGetMetadata},
      {"attr_list", &AttrList},
      {"attr_get", &AttrGet},
      {"attr_set", &AttrSet},
      {"attr_block", &AttrBlock},
      {"attr_connections", &AttrConnections},
      {"reference_add", &ReferenceAdd},
      {"reference_list", &ReferenceList},
      {"reference_clear", &ReferenceClear},
      {"payload_add", &PayloadAdd},
      {"payload_list", &PayloadList},
      {"inherit_add", &InheritAdd},
      {"specialize_add", &SpecializeAdd},
      {"variant_list_sets", &VariantListSets},
      {"variant_get_selection", &VariantGetSelection},
      {"variant_set_selection", &VariantSetSelection},
      {"variant_define", &VariantDefine},
      {"query_prims_by_type", &QueryPrimsByType},
      {"schema_list_types", &SchemaListTypes},
      {"schema_get_type", &SchemaGetType},
      {"search", &Search},
      {"usd_validate", &UsdValidate},
      {"usdz_convert", &USDZConvert},
      {"usdz_pack", &USDZPack},
      {"diff_open", &DiffOpen},
      {"diff_summary", &DiffSummary},
      {"diff_paths", &DiffPaths},
      {"diff_prim", &DiffPrim},
      {"diff_tree", &DiffTree},
      {"diff_text", &DiffText},
      {"diff_json", &DiffJson},
      {"get_all_usd_descriptions", &GetAllUSDDescriptions},
      {"get_usd_description", &GetUSDDescription},
      {"load_usd_layer_from_file", &LoadUSDLayerFromFile},
      {"load_usd_layer_from_data", &LoadUSDLayerFromData},
      {"to_usda", &ToUSDA},
      {"list_primspecs", &ListPrimSpecs},
      {"debug_primspec_dump", &DebugPrimSpecDump},
      {"store_asset", &StoreAsset},
      {"read_asset", &ReadAsset},
      {"read_asset_preview", &ReadAssetPreview},
      {"get_all_asset_descriptions", &GetAllAssetDescriptions},
      {"get_asset_description", &GetAssetDescription},
      {"select_assets", &SelectAssets},
      {"get_selected_assets", &GetSelectedAssets},
      {"save_screenshot", &SaveScreenshot},
      {"list_screenshots", &ListScreenshots},
      {"read_screenshot", &ReadScreenshot},
  };
  return kTable;
}

json ErrorResult(const std::string& err) {
  json result;
  result["isError"] = true;
  result["content"] = json::array();
  json msg;
  msg["error"] = err;
  result["content"].push_back({{"type", "text"}, {"text", msg.dump()}});
  return result;
}

json ToolsListJSON() {
  json all;
  lightusd::mcp_schema::AppendToolSchemas(all);
  json tools = json::array();
  for (const json& t : all["tools"]) {
    if (!UnsupportedTools().count(t.value("name", std::string()))) tools.push_back(t);
  }
  json result;
  result["tools"] = std::move(tools);
  return result;
}

bool CallTool(Context& ctx, const std::string& name, const json& args,
              json& result, std::string& err) {
  if (UnsupportedTools().count(name)) {
    err = "Tool '" + name + "' is not provided by the next MCP product";
    return false;
  }
  const auto& table = ToolTable();
  auto it = table.find(name);
  if (it == table.end()) {
    err = "Unknown tool: " + name;
    return false;
  }
  // Tool arguments are untrusted: a wrong-typed field makes nlohmann::json
  // throw, which must become a tool error rather than end the server. The
  // library is always built with exceptions enabled for this reason.
  try {
    return it->second(ctx, args.is_object() ? args : json::object(), result, err);
  } catch (const json::exception& e) {
    err = std::string("invalid arguments for ") + name + ": " + e.what();
  } catch (const std::exception& e) {
    err = std::string(name) + " failed: " + e.what();
  }
  result = json::object();
  return false;
}

json ResourcesListJSON(const Context& ctx) {
  json result;
  result["resources"] = json::array();
  for (const auto& it : ctx.assets) {
    result["resources"].push_back({{"uri", it.second.name},
                                   {"name", it.second.name},
                                   {"mimeType", "application/octet-stream"}});
  }
  return result;
}

bool ReadResourceJSON(const Context& ctx, const std::string& uri, json& result) {
  auto it = ctx.assets.find(uri);
  if (it == ctx.assets.end()) return false;
  result["contents"] = json::array();
  result["contents"].push_back({{"uri", uri},
                                {"mimeType", "application/octet-stream"},
                                {"blob", it->second.data}});
  return true;
}

}  // namespace

const char* ServerName() { return "lightusd-mcp"; }

struct Server::Impl {
  std::map<std::string, Context> sessions;
  std::string selected;

  Context* Current() {
    auto it = sessions.find(selected);
    return it == sessions.end() ? nullptr : &it->second;
  }
};

Server::Server() : impl_(new Impl()) {}
Server::~Server() = default;

bool Server::CreateContext(const std::string& session_id) {
  if (impl_->sessions.count(session_id)) return false;
  impl_->sessions[session_id].session_id = session_id;
  impl_->selected = session_id;
  return true;
}

bool Server::SelectContext(const std::string& session_id) {
  if (!impl_->sessions.count(session_id)) return false;
  impl_->selected = session_id;
  return true;
}

bool Server::DestroyContext(const std::string& session_id) {
  if (!impl_->sessions.erase(session_id)) return false;
  if (impl_->selected == session_id) impl_->selected.clear();
  return true;
}

std::string Server::ToolsList() {
  if (!impl_->Current()) return "{\"error\": \"invalid session_id\"}";
  return ToolsListJSON().dump();
}

std::string Server::ToolsCall(const std::string& tool_name, const std::string& args_json) {
  Context* ctx = impl_->Current();
  if (!ctx) return "{\"error\": \"invalid session_id\"}";
  if (args_json.size() > kMaxMessageBytes) return ErrorResult("arguments too large").dump();
  const json args = json::parse(args_json, nullptr, false);
  if (args.is_discarded()) return "{\"error\": \"Invalid JSON\"}";
  json result = json::object();
  std::string err;
  if (!CallTool(*ctx, tool_name, args, result, err)) return ErrorResult(err).dump();
  return result.dump();
}

std::string Server::ResourcesList() {
  Context* ctx = impl_->Current();
  if (!ctx) return "{\"error\": \"invalid session_id\"}";
  return ResourcesListJSON(*ctx).dump();
}

std::string Server::ResourcesRead(const std::string& uri) {
  Context* ctx = impl_->Current();
  if (!ctx) return "{\"error\": \"invalid session_id\"}";
  json result;
  if (!ReadResourceJSON(*ctx, uri, result)) {
    result = json::object();
    result["isError"] = true;
  }
  return result.dump();
}

std::string Server::HandleJsonRpc(const std::string& message) {
  try {
    return HandleJsonRpcImpl(message);
  } catch (const std::exception& e) {
    return json{{"jsonrpc", "2.0"}, {"id", nullptr},
                {"error", {{"code", -32603}, {"message", e.what()}}}}.dump();
  }
}

std::string Server::HandleJsonRpcImpl(const std::string& message) {
  auto error_response = [](const json& id, int code, const std::string& msg) {
    return json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", msg}}}}.dump();
  };
  if (message.size() > kMaxMessageBytes) {
    return error_response(nullptr, -32600, "message too large");
  }
  const json req = json::parse(message, nullptr, false);
  if (req.is_discarded() || !req.is_object()) {
    return error_response(nullptr, -32700, "Parse error");
  }
  const bool is_notification = !req.contains("id");
  const json id = req.value("id", json());
  const std::string method = req.value("method", std::string());
  const json params = req.contains("params") ? req["params"] : json::object();
  if (!params.is_object()) {
    return is_notification ? std::string()
                           : error_response(id, -32602, "Invalid params: expected an object");
  }
  if (!impl_->Current()) {
    if (!impl_->sessions.count("default")) CreateContext("default");
    SelectContext("default");
  }
  Context& ctx = *impl_->Current();

  json result;
  if (method == "initialize") {
    result["protocolVersion"] =
        params.value("protocolVersion", std::string(kProtocolVersion));
    result["capabilities"] = {{"tools", json::object()}, {"resources", json::object()}};
    result["serverInfo"] = {{"name", ServerName()},
                            {"version", lightusd_version_string()}};
  } else if (method == "ping") {
    result = json::object();
  } else if (method == "tools/list") {
    result = ToolsListJSON();
  } else if (method == "tools/call") {
    const std::string name = params.value("name", std::string());
    const json args = params.contains("arguments") ? params["arguments"] : json::object();
    json tool_result = json::object();
    std::string err;
    if (!CallTool(ctx, name, args, tool_result, err)) {
      result = {{"isError", true},
                {"content", json::array({{{"type", "text"}, {"text", err}}})}};
    } else if (tool_result.contains("content")) {
      result = std::move(tool_result);
    } else {
      // Structured tool results travel as text content plus structuredContent.
      result["content"] = json::array({{{"type", "text"}, {"text", tool_result.dump()}}});
      result["structuredContent"] = std::move(tool_result);
    }
  } else if (method == "resources/list") {
    result = ResourcesListJSON(ctx);
  } else if (method == "resources/read") {
    const std::string uri = params.value("uri", std::string());
    if (!ReadResourceJSON(ctx, uri, result)) {
      return is_notification ? std::string()
                             : error_response(id, -32002, "Resource not found: " + uri);
    }
  } else if (method.compare(0, 14, "notifications/") == 0) {
    return std::string();
  } else {
    return is_notification ? std::string()
                           : error_response(id, -32601, "Method not found: " + method);
  }
  if (is_notification) return std::string();
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}

int RunStdio(std::istream& in, std::ostream& out) {
  Server server;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::string response = server.HandleJsonRpc(line);
    if (!response.empty()) {
      out << response << '\n';
      out.flush();
    }
  }
  return 0;
}

}  // namespace mcp
}  // namespace lightusd
