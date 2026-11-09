// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// WASM exports of the LightUSD MCP server (LIGHTUSD_WASM_PRODUCT=mcp).
// Strings cross as NUL-terminated UTF-8; returned strings are malloc'd and
// released with lightusd_mcp_free. mcp-api.js wraps these in
// Module.LightUSDMCPServer and the legacy-named mcp* functions.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lightusd-mcp.hh"

namespace {

std::vector<std::unique_ptr<lightusd::mcp::Server>>& Servers() {
  static std::vector<std::unique_ptr<lightusd::mcp::Server>> servers;
  return servers;
}

lightusd::mcp::Server* Get(uint32_t handle) {
  auto& s = Servers();
  return handle && handle <= s.size() ? s[handle - 1].get() : nullptr;
}

char* Dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

std::string Str(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE uint32_t lightusd_mcp_create() {
  auto& s = Servers();
  for (size_t i = 0; i < s.size(); ++i) {
    if (!s[i]) {
      s[i].reset(new lightusd::mcp::Server());
      return static_cast<uint32_t>(i + 1);
    }
  }
  s.emplace_back(new lightusd::mcp::Server());
  return static_cast<uint32_t>(s.size());
}

EMSCRIPTEN_KEEPALIVE void lightusd_mcp_destroy(uint32_t handle) {
  if (Get(handle)) Servers()[handle - 1].reset();
}

EMSCRIPTEN_KEEPALIVE int lightusd_mcp_create_context(uint32_t handle, const char* id) {
  auto* s = Get(handle);
  return s && s->CreateContext(Str(id)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int lightusd_mcp_select_context(uint32_t handle, const char* id) {
  auto* s = Get(handle);
  return s && s->SelectContext(Str(id)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int lightusd_mcp_destroy_context(uint32_t handle, const char* id) {
  auto* s = Get(handle);
  return s && s->DestroyContext(Str(id)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE char* lightusd_mcp_tools_list(uint32_t handle) {
  auto* s = Get(handle);
  return s ? Dup(s->ToolsList()) : nullptr;
}

EMSCRIPTEN_KEEPALIVE char* lightusd_mcp_tools_call(uint32_t handle, const char* name,
                                                   const char* args) {
  auto* s = Get(handle);
  return s ? Dup(s->ToolsCall(Str(name), args ? Str(args) : std::string("{}"))) : nullptr;
}

EMSCRIPTEN_KEEPALIVE char* lightusd_mcp_resources_list(uint32_t handle) {
  auto* s = Get(handle);
  return s ? Dup(s->ResourcesList()) : nullptr;
}

EMSCRIPTEN_KEEPALIVE char* lightusd_mcp_resources_read(uint32_t handle, const char* uri) {
  auto* s = Get(handle);
  return s ? Dup(s->ResourcesRead(Str(uri))) : nullptr;
}

EMSCRIPTEN_KEEPALIVE char* lightusd_mcp_handle_jsonrpc(uint32_t handle, const char* message) {
  auto* s = Get(handle);
  return s ? Dup(s->HandleJsonRpc(Str(message))) : nullptr;
}

EMSCRIPTEN_KEEPALIVE void lightusd_mcp_free(char* p) { std::free(p); }

}  // extern "C"
