// SPDX-License-Identifier: Apache-2.0
// lusdview - interface the MCP server calls to read/manipulate the viewer.
//
// Every method runs on the MAIN thread (the MCP server marshals tool calls into
// the render loop), so implementations may freely touch the Stage / DrawScene /
// camera / GUI. Each returns the tool's JSON result payload; set `err` non-empty
// to signal a tool error (surfaced as a JSON-RPC error to the client).
#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json.hpp"

namespace lusdview {

class McpHost {
 public:
  virtual ~McpHost() = default;

  virtual nlohmann::json mcpLoadUsd(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpSceneInfo(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpRenderStats(const nlohmann::json& args,
                                        std::string& err) = 0;
  virtual nlohmann::json mcpGetFocusedPrim(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpSetFocus(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpViewport(const nlohmann::json& args, std::string& err) = 0;
  // Capture the current viewport to an image file (PNG/JPEG/PPM by extension), for
  // visual debugging over MCP. Returns the path + dimensions.
  virtual nlohmann::json mcpScreenshot(const nlohmann::json& args, std::string& err) = 0;
  // Synthesize a keyboard key press (render-mode / wireframe / framing keys),
  // i.e. drive the viewer as if the user pressed a key.
  virtual nlohmann::json mcpInput(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpMouse(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpPick(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpListPrims(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpLoadPayloads(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpReloadLayer(const nlohmann::json& args,
                                        std::string& err) = 0;
  virtual nlohmann::json mcpTimeline(const nlohmann::json& args, std::string& err) = 0;
  virtual nlohmann::json mcpSkinning(const nlohmann::json& args, std::string& err) = 0;
  // Change resettable, per-capture state without restarting the viewer.
  virtual nlohmann::json mcpRenderSettings(const nlohmann::json& args,
                                           std::string& err) = 0;
  virtual nlohmann::json mcpListOpenPbrMaterials(const nlohmann::json& args,
                                                 std::string& err) = 0;
  virtual nlohmann::json mcpOpenPbrMaterial(const nlohmann::json& args,
                                            std::string& err) = 0;
  // Recompile, watch, or query live Vulkan/CUDA/HIP shader modules without
  // restarting the viewer. Implementations keep the last good module on error.
  virtual nlohmann::json mcpShaderReload(const nlohmann::json& args,
                                         std::string& err) = 0;
  virtual nlohmann::json mcpVirtualHuman(const std::string& tool,
                                         const nlohmann::json& args,
                                         std::string& err) = 0;

  // Forward an unrecognized tool name to the lightusd library tool dispatcher
  // (tydra::mcp::CallTool), run against a snapshot of the loaded Stage.
  virtual nlohmann::json mcpCallLibraryTool(const std::string& name,
                                            const nlohmann::json& args,
                                            std::string& err) = 0;
};

}  // namespace lusdview
