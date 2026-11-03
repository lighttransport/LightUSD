// SPDX-License-Identifier: Apache-2.0
// lusdview - embedded MCP (Model Context Protocol) server.
//
// JSON-RPC 2.0 over two transports: stdio (newline-delimited messages on
// stdin/stdout) and HTTP (POST a JSON-RPC request, get a JSON response; civetweb).
// Protocol/handshake methods are answered on the transport thread; stateful
// `tools/call`s are queued and executed on the main thread via drain().
#pragma once

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "external/jsonhpp/nlohmann/json.hpp"

struct mg_context;
struct mg_connection;

namespace lusdview {

class McpHost;

class MCPServer {
 public:
  explicit MCPServer(McpHost* host) : host_(host) {}
  ~MCPServer();

  // Start the stdio reader thread (owns stdin/stdout for JSON-RPC).
  void startStdio();
  // Start the HTTP server on `port`. Returns false if it could not bind.
  bool startHttp(const std::string& hostname, int port);
  void stopHttp();  // may be restarted without stopping the MCP dispatcher

  // Execute queued tool calls on the MAIN thread. Call once per frame.
  void drain();
  // Stop transports, resolve any pending calls, join threads. Idempotent.
  void stop();

  // Dispatch one JSON-RPC message; returns the response text ("" = notification).
  std::string dispatchLine(const std::string& text);

 private:
  // A stateful tool call marshalled to the main thread.
  struct Command {
    std::string tool;
    nlohmann::json args;
    std::promise<nlohmann::json> result;  // result payload
    std::string err;                      // non-empty => tool error
  };

  nlohmann::json buildToolsList() const;
  // Enqueue a tool call and block (bounded) until the main thread runs it.
  nlohmann::json runTool(const std::string& name, const nlohmann::json& args,
                         std::string& err);

  static int HttpHandler(mg_connection* conn, void* user);

  McpHost* host_{nullptr};

  std::mutex queueMu_;
  std::deque<std::shared_ptr<Command>> queue_;
  std::atomic<bool> running_{true};

  std::thread stdioThread_;
  bool stdioStarted_{false};

  std::string httpToken_;
  mg_context* httpCtx_{nullptr};
};

}  // namespace lusdview
