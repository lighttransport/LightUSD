// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// LightUSD MCP server (next product). Hosts per-session contexts and exposes
// the MCP tools/resources surface two ways:
//  - the session string API used by embedders (same contract as the legacy
//    LightUSDLoaderNative mcp* methods), and
//  - a JSON-RPC 2.0 handler for stdio/HTTP transports.
#pragma once

#include <iosfwd>
#include <memory>
#include <string>

namespace lightusd {
namespace mcp {

const char* ServerName();

class Server {
 public:
  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Sessions. Create fails for an existing id; Select fails for a missing one.
  bool CreateContext(const std::string& session_id);
  bool SelectContext(const std::string& session_id);
  bool DestroyContext(const std::string& session_id);

  // JSON strings for the selected session. ToolsCall returns the tool result
  // object, or {"isError": true, "content": [...]} on failure.
  std::string ToolsList();
  std::string ToolsCall(const std::string& tool_name, const std::string& args_json);
  std::string ResourcesList();
  std::string ResourcesRead(const std::string& uri);

  // One JSON-RPC 2.0 message; returns the response ("" for notifications).
  // Uses the selected session, creating "default" when none exists.
  std::string HandleJsonRpc(const std::string& message);

 private:
  std::string HandleJsonRpcImpl(const std::string& message);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Newline-delimited JSON-RPC over the given streams (MCP stdio transport).
int RunStdio(std::istream& in, std::ostream& out);

}  // namespace mcp
}  // namespace lightusd
