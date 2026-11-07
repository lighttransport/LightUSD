// Interface for MCP(ModelContextProtocol)
#pragma once

#include <cstddef>
#include <string>
#include <map>

#include "core/security-policy.hh"

namespace lightusd {

namespace tydra {
namespace mcp {

struct MCPServerOptions {
  // Security-first defaults. A local trusted deployment can override these.
  size_t max_request_body_bytes = security_policy::kMCPMaxRequestBodyBytes;
  bool require_session = true;
  bool log_request_body = false;
};

class MCPServer
{
 public:
  MCPServer();
  ~MCPServer();
  bool init(int port, const std::string &host, const MCPServerOptions &options);
  bool init(int port, const std::string &host = "localhost");
  bool run();
  // Run a stdio (newline-delimited JSON-RPC) transport loop, blocking until
  // stdin closes. Does not need init(); registers methods internally.
  bool run_stdio();
  bool stop();

 private:
  class Impl;
  Impl *impl_ = nullptr; // Pointer to implementation details

};

} // namespace mcp
} // namespace tydra
} // namespace lightusd
