// SPDX-License-Identifier: Apache-2.0
#include "mcp/mcp_server.hh"

#include <chrono>
#include <exception>

#include "log.hh"
#include "mcp/mcp_host.hh"
#include "tydra/js-script.hh"    // complete JSEngineState (held in Context by value)
#include "tydra/mcp-context.hh"  // lightusd::tydra::mcp::Context
#include "tydra/mcp-tools.hh"    // GetToolsList (library tool schemas)

namespace lusdview {

using nlohmann::json;

namespace {

enum JsonRpcError {
  PARSE_ERROR = -32700,
  INVALID_REQUEST = -32600,
  METHOD_NOT_FOUND = -32601,
  INVALID_PARAMS = -32602,
  INTERNAL_ERROR = -32603,
};

std::string makeResult(const json& id, const json& result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}
std::string makeError(const json& id, int code, const std::string& msg) {
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"error", {{"code", code}, {"message", msg}}}}
      .dump();
}

// Tool schema helper.
json tool(const char* name, const char* desc, const json& props,
          const json& required = json::array()) {
  json schema = {{"type", "object"}, {"properties", props}};
  if (!required.empty()) schema["required"] = required;
  return {{"name", name}, {"description", desc}, {"inputSchema", schema}};
}
json numProp(const char* desc) { return {{"type", "number"}, {"description", desc}}; }
json strProp(const char* desc) { return {{"type", "string"}, {"description", desc}}; }

}  // namespace

MCPServer::~MCPServer() { stop(); }

json MCPServer::buildToolsList() const {
  json tools = json::array();
  tools.push_back(tool("load_usd", "Load a USD path or inline USDA text into "
                                   "the viewer (async; poll get_scene_info).",
                       {{"path", strProp("Path to a USD file")},
                        {"usda", strProp("Inline USDA text (maximum 16 MiB)")}}));
  tools.push_back(tool("get_scene_info",
                       "Get the loaded scene: filepath, mesh/triangle counts, up "
                       "axis, and the world-space bounding box.",
                       json::object()));
  tools.push_back(tool("get_scene_bbox", "Get the scene world-space bounding box "
                                         "(alias of get_scene_info).",
                       json::object()));
  tools.push_back(tool("get_render_stats",
                       "Get UI/render FPS and the current adaptive-quality tier, "
                       "render scale, and target FPS.", json::object()));
  tools.push_back(tool("get_focused_prim",
                       "Info about the currently focused/selected prim: path, "
                       "vertex/triangle counts, bbox, and material.",
                       json::object()));
  tools.push_back(tool("set_focus", "Select/focus a prim by its absolute path; "
                                    "returns the same info as get_focused_prim.",
                       {{"path", strProp("Absolute prim path, e.g. /root/Mesh")}},
                       json::array({"path"})));
  tools.push_back(tool(
      "viewport",
      "Manipulate the camera. op=orbit|pan {dx,dy pixels} | dolly|forward|backward "
      "{amount} | fit "
      "{frame the scene} | home | isometric | front | back | right | left | top | bottom "
      "| bookmark_save {slot} | bookmark_load {slot} | focus_dof {focus the "
      "thin lens on the picked/selected object} | set "
      "{absolute target[3],yaw,pitch,distance}.",
      {{"op", strProp("orbit | pan | dolly | forward | backward | fit | home | isometric | front | back | right | left | top | bottom | bookmark_save | bookmark_load | focus_dof | set")},
       {"dx", numProp("pixels (orbit/pan)")},
       {"dy", numProp("pixels (orbit/pan)")},
       {"amount", numProp("dolly amount (+ zooms in)")},
       {"slot", {{"type", "integer"}, {"description", "bookmark slot 1..3"}}},
       {"target", {{"type", "array"}, {"items", {{"type", "number"}}}}},
       {"yaw", numProp("azimuth radians (set)")},
       {"pitch", numProp("elevation radians (set)")},
       {"distance", numProp("distance to target (set)")}},
      json::array({"op"})));
  tools.push_back(tool(
      "screenshot",
      "Capture the current viewport to an image file (PNG/JPEG/PPM by extension) for "
      "visual debugging. Camera moves from the 'viewport' tool apply on the next "
      "frame, so orbit/pan/dolly then call screenshot as a separate request.",
      {{"path", strProp("output image path (.png or .ppm)")}},
      json::array({"path"})));
  tools.push_back(tool(
      "input",
      "Synthesize a keyboard key as if pressed in the viewport. "
      "key=v (cycle wireframe: off/wire/wire+shade) | w/s (forward/backward) | "
      "f or a (frame all) | i (show selected only) | u (show all) | "
      "0 (home) | 5 (isometric) | 1 (front) | 3 (right) | 7 (top).",
      {{"key", strProp("v | w | s | f | a | i | u | 0 | 1 | 3 | 5 | 7")}},
      json::array({"key"})));
  tools.push_back(tool(
      "mouse",
      "Inject viewport mouse input. type=move or button; coordinates are viewport pixels. "
      "For button use button=0 left, 1 middle, 2 right and down=true/false.",
      {{"type", strProp("move | button")},
       {"x", numProp("viewport pixel x")},
       {"y", numProp("viewport pixel y")},
       {"button", {{"type", "integer"}, {"description", "0 left, 1 middle, 2 right"}}},
       {"down", {{"type", "boolean"}, {"description", "button press or release"}}},
       {"shift", {{"type", "boolean"}}},
       {"ctrl", {{"type", "boolean"}}},
       {"alt", {{"type", "boolean"}}}},
      json::array({"type", "x", "y"})));
  tools.push_back(tool(
      "pick",
      "Pick a viewport pixel using the selection ID/depth buffer. Pass x1/y1 to "
      "pick a rectangular region; the prim owning the most depth-tested covered "
      "pixels wins. Set select=false for a read-only query. Pixel picks return "
      "hit_distance, which is also retained as the focus_dof target when selected.",
      {{"x", numProp("viewport pixel x")},
       {"y", numProp("viewport pixel y")},
       {"x1", numProp("optional opposite region corner x")},
       {"y1", numProp("optional opposite region corner y")},
       {"select", {{"type", "boolean"}, {"description", "update viewer selection (default true)"}}}},
      json::array({"x", "y"})));
  tools.push_back(tool("list_prims", "List renderable mesh prim paths in the scene.",
                       {{"max", {{"type", "integer"}, {"description", "cap (default 1000)"}}}}));
  tools.push_back(tool(
      "load_payloads",
      "Load deferred USD payloads (lazy loading). Omit 'paths' to load all "
      "deferred payloads; otherwise pass prim paths from get_scene_info's "
      "deferred_payloads. Async; poll get_scene_info.",
      {{"paths", {{"type", "array"},
                  {"items", {{"type", "string"}}},
                  {"description", "Deferred-payload prim paths (omit = all)"}}}}));
  tools.push_back(tool(
      "reload_layer",
      "Re-read the root layer or one resolved path from get_scene_info's "
      "layer_dependencies, recompose the retained next Stage, and apply a "
      "transactional incremental renderer update. Omit path to reload the root.",
      {{"path", strProp("resolved dependency-layer path (omit = root layer)")}}));
  tools.push_back(tool(
      "timeline",
      "Control animation playback. op=play | pause | stop (reset to start) | "
      "seek {time}. Returns the playback state (playing, time, start, end, fps).",
      {{"op", strProp("play | pause | stop | seek")},
       {"time", numProp("target time code (seek)")}},
      json::array({"op"})));
  tools.push_back(tool(
      "skinning",
      "Query or set the viewer skinning mode. Omit mode to query; mode=auto|cpu|gpu.",
      {{"mode", strProp("auto | cpu | gpu")}}));
  tools.push_back(tool(
      "render_settings",
      "Query or set resettable per-capture options without restarting lusdview.",
      {{"mode", strProp("shaded | wireframe | normals | material-id | geom-normal | uv | depth | albedo | facing | roughness | metallic | emissive | opacity")},
       {"grid", {{"type", "boolean"}, {"description", "show the ground grid"}}},
       {"transparency", strProp("auto | weighted | sorted; weighted warms asynchronously on Vulkan")},
       {"adaptive_quality", {{"type", "boolean"}, {"description", "enable adaptive Vulkan quality"}}},
       {"target_render_fps", numProp("adaptive render FPS floor")},
       {"camera", strProp("named USD camera to resolve on the next load")}}));
  tools.push_back(tool(
      "list_openpbr_materials",
      "List live OpenPBR-capable render materials, including dual-authored "
      "PreviewSurface/OpenPBR materials and connection state.",
      json::object()));
  tools.push_back(tool(
      "openpbr_material",
      "Get or live-edit a render material's complete OpenPBR constant block. "
      "Pass values to update named constants; make_constant=true detaches "
      "shading texture/nodegraph connections for session-local tweaking.",
      {{"material_id", {{"type", "integer"}}},
       {"values", {{"type", "object"}}},
       {"make_constant", {{"type", "boolean"}}}},
      json::array({"material_id"})));
  tools.push_back(tool(
      "shader_reload",
      "Live GPU shader development in the persistent viewer. action=status | "
      "reload | watch; backend=active|vulkan|cuda|hip|all. Reload compiles and "
      "transactionally swaps a module, keeping the last good shader on error. "
      "Watch polls the source every 250 ms; use screenshot for visual feedback.",
      {{"action", strProp("status | reload | watch")},
       {"backend", strProp("active | vulkan | cuda | hip | all")},
       {"source", strProp("optional local GLSL or shared CUDA/HIP kernel path")},
       {"watch", {{"type", "boolean"},
                  {"description", "enable or disable file watching"}}}},
      json::array({"action"})));
  tools.push_back(tool(
      "vchar_status",
      "Report virtual-human renderer, deformation, hair and physics capabilities.",
      json::object()));
  tools.push_back(tool(
      "list_blendshapes",
      "List facial/body USD blendshape targets available for live control.",
      json::object()));
  tools.push_back(tool(
      "set_blendshape_weights",
      "Set session-local facial blendshape overrides by target name.",
      {{"weights", {{"type", "object"},
                     {"additionalProperties", {{"type", "number"}}}}}},
      json::array({"weights"})));
  tools.push_back(tool(
      "list_facial_controls",
      "List named facial controls. Initially each USD blendshape is a direct control.",
      json::object()));
  tools.push_back(tool(
      "set_facial_controls",
      "Set named facial controls. V1 maps control names directly to blendshapes.",
      {{"controls", {{"type", "object"},
                      {"additionalProperties", {{"type", "number"}}}}}},
      json::array({"controls"})));
  tools.push_back(tool(
      "vchar_debug",
      "Select a virtual-human diagnostic view and skeleton overlay.",
      {{"mode", strProp("shaded | skin-weights | blend-influence | normals | tangents")},
       {"skeleton", {{"type", "boolean"}}}}));
  tools.push_back(tool(
      "autorigger_inspect",
      "Describe the JSON-RPC 2.0 stdio auto-rigger contract and current USD input.",
      json::object()));
  tools.push_back(tool(
      "apply_rig_overlay",
      "Compose an auto-rigger USDA/USDC overlay non-destructively over the loaded asset.",
      {{"path", strProp("path to generated rig overlay layer")}},
      json::array({"path"})));
  tools.push_back(tool(
      "vchar_physics",
      "Inspect, initialize, step, reset, or hide the realtime UsdPhysics preview.",
      {{"op", strProp("status | initialize | step | reset | hide")},
       {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}}},
       {"timestep", numProp("optional seconds per simulation step")}},
      json::array({"op"})));
  tools.push_back(tool(
      "vchar_hair_diagnostics",
      "Inspect BasisCurves groom topology, widths, density, bounds and backend representation.",
      json::object()));
  tools.push_back(tool(
      "vchar_skin_profile",
      "Apply a tuned realtime multilobe skin preset; omit material_id to auto-detect face/skin/head/body.",
      {{"material_id", {{"type", "integer"}}},
       {"strength", numProp("SSS strength in [0,1], default 0.65")}},
      json::array()));
  tools.push_back(tool(
      "vchar_deformer",
      "Inspect deformer adapters, run the configured auto-rigger, or inspect/evaluate MetaHuman DNA.",
      {{"op", strProp("status | autorig | apply-overlay | body-playback | dna-inspect | dna-evaluate")},
       {"path", strProp("source asset for autorig or USDA/USDC overlay for apply-overlay")},
       {"dna_path", strProp("MetaHuman DNA file for DNA operations")},
       {"lod", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65535}}},
       {"control_space", strProp("raw | canonical")},
       {"controls", {{"type", "array"}, {"items", {{"type", "number"}}},
                     {"maxItems", 65536}}},
       {"output", strProp("optional autorig output overlay path")},
       {"rig_layer", strProp("body animation rig layer; defaults to loaded asset")},
       {"joints_path", strProp("canonical body joint list for body-playback")},
       {"track_path", strProp("canonical body track for body-playback")},
       {"autoplay", {{"type", "boolean"}, {"description", "start timeline after body layer loads"}}},
       {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 600000}}}},
      json::array({"op"})));

  // Append the lightusd library's USD tools (stage/prim/attr query, composition,
  // search, run_script, ...). GetToolsList emits static schemas (no stage), so it
  // is safe to call here on the transport thread.
  try {
    lightusd::tydra::mcp::Context ctx;
    json libList;
    if (lightusd::tydra::mcp::GetToolsList(ctx, libList) && libList.contains("tools") &&
        libList["tools"].is_array()) {
      for (auto& t : libList["tools"]) tools.push_back(t);
    }
  } catch (const std::exception& e) {
    LOGD("MCP: GetToolsList failed: %s", e.what());
  }
  return tools;
}

std::string MCPServer::dispatchLine(const std::string& text) {
  json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return makeError(nullptr, PARSE_ERROR, "Parse error");
  }
  const json id = j.contains("id") ? j["id"] : json(nullptr);
  const bool isNotification = !j.contains("id") || j["id"].is_null();
  const std::string method = j.value("method", std::string());
  const json params =
      (j.contains("params") && j["params"].is_object()) ? j["params"] : json::object();

  if (j.contains("jsonrpc") && j["jsonrpc"].is_string() &&
      j["jsonrpc"].get<std::string>() != "2.0") {
    return isNotification ? std::string() : makeError(id, INVALID_REQUEST,
                                                      "Unsupported jsonrpc version");
  }
  if (method.empty()) {
    return isNotification ? std::string() : makeError(id, INVALID_REQUEST, "Missing method");
  }

  if (method == "initialize") {
    json result = {{"protocolVersion", "2025-03-26"},
                   {"serverInfo", {{"name", "lusdview-mcp"}, {"version", "1.0.0"}}},
                   {"capabilities", {{"tools", json::object()}}}};
    return makeResult(id, result);
  }
  if (method == "notifications/initialized") return "";  // notification
  if (method == "ping") return makeResult(id, json::object());
  if (method == "tools/list") return makeResult(id, json{{"tools", buildToolsList()}});

  if (method == "tools/call") {
    if (isNotification) return "";
    if (!params.contains("name") || !params["name"].is_string()) {
      return makeError(id, INVALID_PARAMS, "tools/call requires string 'name'");
    }
    const std::string name = params["name"].get<std::string>();
    const json arguments =
        (params.contains("arguments") && params["arguments"].is_object())
            ? params["arguments"]
            : json::object();
    std::string err;
    json payload = runTool(name, arguments, err);
    if (!err.empty()) return makeError(id, INVALID_PARAMS, err);
    json result = {
        {"content", json::array({json{{"type", "text"}, {"text", payload.dump(2)}}})},
        {"structuredContent", payload}};
    return makeResult(id, result);
  }

  return isNotification ? std::string()
                        : makeError(id, METHOD_NOT_FOUND, "Method not found: " + method);
}

json MCPServer::runTool(const std::string& name, const json& args, std::string& err) {
  auto cmd = std::make_shared<Command>();
  cmd->tool = name;
  cmd->args = args;
  auto fut = cmd->result.get_future();
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    if (!running_) {
      err = "server shutting down";
      return json::object();
    }
    queue_.push_back(cmd);
  }
  // Runtime compiler cold starts (especially NVRTC on a large transport kernel)
  // can exceed the ordinary interactive tool deadline. Keep other MCP calls
  // bounded at 30 seconds while allowing an explicit shader iteration to finish
  // and return its compiler diagnostics.
  const auto timeout = name == "shader_reload" ? std::chrono::seconds(180)
      : name == "vchar_deformer"
          ? std::chrono::milliseconds(std::min(600000,
                std::max(30000, args.value("timeout_ms", 30000) + 10000)))
          : std::chrono::seconds(30);
  if (fut.wait_for(timeout) != std::future_status::ready) {
    err = "tool call timed out";
    return json::object();
  }
  json payload = fut.get();
  if (!cmd->err.empty()) err = cmd->err;
  return payload;
}

void MCPServer::drain() {
  for (;;) {
    std::shared_ptr<Command> cmd;
    {
      std::lock_guard<std::mutex> lk(queueMu_);
      if (queue_.empty()) break;
      cmd = queue_.front();
      queue_.pop_front();
    }
    json payload;
    std::string err;
    const std::string& t = cmd->tool;
    try {
      if (!host_) {
        err = "no host";
      } else if (t == "load_usd") {
        payload = host_->mcpLoadUsd(cmd->args, err);
      } else if (t == "get_scene_info" || t == "get_scene_bbox") {
        payload = host_->mcpSceneInfo(cmd->args, err);
      } else if (t == "get_render_stats") {
        payload = host_->mcpRenderStats(cmd->args, err);
      } else if (t == "get_focused_prim") {
        payload = host_->mcpGetFocusedPrim(cmd->args, err);
      } else if (t == "set_focus") {
        payload = host_->mcpSetFocus(cmd->args, err);
      } else if (t == "viewport") {
        payload = host_->mcpViewport(cmd->args, err);
      } else if (t == "screenshot") {
        payload = host_->mcpScreenshot(cmd->args, err);
      } else if (t == "input") {
        payload = host_->mcpInput(cmd->args, err);
      } else if (t == "mouse") {
        payload = host_->mcpMouse(cmd->args, err);
      } else if (t == "pick") {
        payload = host_->mcpPick(cmd->args, err);
      } else if (t == "list_prims") {
        payload = host_->mcpListPrims(cmd->args, err);
      } else if (t == "load_payloads") {
        payload = host_->mcpLoadPayloads(cmd->args, err);
      } else if (t == "reload_layer") {
        payload = host_->mcpReloadLayer(cmd->args, err);
      } else if (t == "timeline") {
        payload = host_->mcpTimeline(cmd->args, err);
      } else if (t == "skinning") {
        payload = host_->mcpSkinning(cmd->args, err);
      } else if (t == "render_settings") {
        payload = host_->mcpRenderSettings(cmd->args, err);
      } else if (t == "list_openpbr_materials") {
        payload = host_->mcpListOpenPbrMaterials(cmd->args, err);
      } else if (t == "openpbr_material") {
        payload = host_->mcpOpenPbrMaterial(cmd->args, err);
      } else if (t == "shader_reload") {
        payload = host_->mcpShaderReload(cmd->args, err);
      } else if (t == "vchar_status" || t == "list_blendshapes" ||
                 t == "set_blendshape_weights" ||
                 t == "list_facial_controls" ||
                 t == "set_facial_controls" || t == "vchar_debug" ||
                 t == "autorigger_inspect" || t == "apply_rig_overlay" ||
                 t == "vchar_physics" || t == "vchar_hair_diagnostics") {
        payload = host_->mcpVirtualHuman(t, cmd->args, err);
      } else if (t == "vchar_skin_profile" || t == "vchar_deformer") {
        payload = host_->mcpVirtualHuman(t, cmd->args, err);
      } else {
        // Not a viewer tool -> forward to the lightusd library tool dispatcher.
        payload = host_->mcpCallLibraryTool(t, cmd->args, err);
      }
    } catch (const std::exception& e) {
      err = std::string("tool exception: ") + e.what();
    }
    cmd->err = err;
    cmd->result.set_value(std::move(payload));
  }
}

void MCPServer::stop() {
  if (!running_.exchange(false)) return;  // already stopped

  // Wake any transport thread blocked on a queued tool call BEFORE joining the
  // transports (the civetweb worker that pushed it is blocked on its future, and
  // mg_stop() would otherwise deadlock waiting for that worker to return).
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    for (auto& cmd : queue_) {
      cmd->err = "server shutting down";
      cmd->result.set_value(nlohmann::json::object());
    }
    queue_.clear();
  }
  stopHttp();  // mg_stop joins civetweb worker threads
  if (stdioThread_.joinable()) stdioThread_.join();
  LOGD("MCP server stopped");
}

}  // namespace lusdview
