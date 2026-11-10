// SPDX-License-Identifier: Apache-2.0
// Embed the core MCP server over stdio or a bounded HTTP transport.
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "external/civetweb/civetweb.h"
#include "mcp/lightusd-mcp.hh"

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
struct Transport {
  lightusd::mcp::Server server;
  std::mutex mutex;
  size_t sessions = 0;
};
int reply(mg_connection* connection, int status, const std::string& body) {
  mg_printf(connection,
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            status,
            status == 200   ? "OK"
            : status == 202 ? "Accepted"
            : status == 413 ? "Payload Too Large"
                            : "Bad Request",
            body.size());
  if (!body.empty()) mg_write(connection, body.data(), body.size());
  return status;
}
int handle(mg_connection* connection, void* data) {
  const auto* request = mg_get_request_info(connection);
  if (!request || !request->request_method) return reply(connection, 400, "{}");
  if (std::strcmp(request->request_method, "OPTIONS") == 0) {
    mg_printf(connection,
              "HTTP/1.1 204 No Content\r\nAllow: POST, OPTIONS\r\n"
              "Content-Length: 0\r\n\r\n");
    return 204;
  }
  if (std::strcmp(request->request_method, "POST") != 0)
    return reply(connection, 400, "{\"error\":\"Use POST for JSON-RPC\"}");
  constexpr size_t limit = 16 * 1024 * 1024;
  if (request->content_length > static_cast<long long>(limit))
    return reply(connection, 413, "{}");
  std::string body;
  char buffer[4096];
  int count = 0;
  while ((count = mg_read(connection, buffer, sizeof(buffer))) > 0) {
    if (static_cast<size_t>(count) > limit - body.size())
      return reply(connection, 413, "{}");
    body.append(buffer, static_cast<size_t>(count));
  }
  if (count < 0) return reply(connection, 400, "{}");
  auto& transport = *static_cast<Transport*>(data);
  // Server selects a context internally; serialize selection and dispatch so
  // concurrent HTTP clients cannot operate on another client's stage.
  std::lock_guard<std::mutex> lock(transport.mutex);
  const char* header = mg_get_header(connection, "Mcp-Session-Id");
  const std::string session = header ? header : "default";
  if (session.empty() || session.size() > 256)
    return reply(connection, 400, "{\"error\":\"Invalid session id\"}");
  if (!transport.server.SelectContext(session)) {
    // The example accepts caller-selected session IDs, with a finite ceiling.
    if (transport.sessions >= 64 || !transport.server.CreateContext(session))
      return reply(connection, 400, "{\"error\":\"Session limit exceeded\"}");
    ++transport.sessions;
  }
  const std::string response = transport.server.HandleJsonRpc(body);
  return reply(connection, response.empty() ? 202 : 200, response);
}
}  // namespace
int main(int argc, char** argv) {
  bool stdio = false;
  int port = 8085;
  std::string host = "localhost";
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: mcp_server [--stdio] [--host localhost] [--port 8085]\n";
      return 0;
    }
    if (arg == "--stdio") {
      stdio = true;
      continue;
    }
    if ((arg == "--host" || arg == "--port") && i + 1 < argc) {
      const std::string value(argv[++i]);
      if (arg == "--host")
        host = value;
      else {
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), port);
        if (parsed.ec != std::errc() ||
            parsed.ptr != value.data() + value.size() || port < 1 ||
            port > 65535) {
          std::cerr << "Invalid port\n";
          return 1;
        }
      }
      continue;
    }
    std::cerr << "Unknown or incomplete option: " << arg << '\n';
    return 1;
  }
  if (stdio) return lightusd::mcp::RunStdio(std::cin, std::cout);
  Transport transport;
  const std::string address = host + ":" + std::to_string(port);
  const char* options[] = {"listening_ports", address.c_str(), "num_threads",
                           "4", nullptr};
  mg_callbacks callbacks{};
  mg_init_library(0);
  mg_context* context = mg_start(&callbacks, nullptr, options);
  if (!context) {
    std::cerr << "Cannot listen on " << address << '\n';
    mg_exit_library();
    return 1;
  }
  mg_set_request_handler(context, "/mcp", handle, &transport);
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::cout << "http://" << address << "/mcp\n" << std::flush;
  while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  mg_stop(context);
  mg_exit_library();
  return 0;
}
