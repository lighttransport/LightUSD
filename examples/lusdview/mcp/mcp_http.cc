// SPDX-License-Identifier: Apache-2.0
// HTTP transport for the MCP server (civetweb raw C API). One JSON-RPC response
// per POST (minimal "Streamable HTTP"; no SSE / server-initiated messages).
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>

#include "external/civetweb/civetweb.h"
#include "log.hh"
#include "mcp/mcp_server.hh"

namespace lusdview {

int MCPServer::HttpHandler(mg_connection* conn, void* user) {
  auto* self = static_cast<MCPServer*>(user);
  const mg_request_info* ri = mg_get_request_info(conn);
  const char* method = ri && ri->request_method ? ri->request_method : "";

  if (!self->httpToken_.empty()) {
    const char* auth = mg_get_header(conn, "Authorization");
    const std::string expected = "Bearer " + self->httpToken_;
    const std::string supplied = auth ? auth : "";
    unsigned difference = static_cast<unsigned>(supplied.size() != expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
      difference |= static_cast<unsigned>(expected[i] ^ (i < supplied.size() ? supplied[i] : 0));
    if (difference) {
      mg_printf(conn, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
      return 401;
    }
  }
  if (std::strcmp(method, "POST") == 0) {
    std::string body;
    char buf[2048];
    int n;
    while ((n = mg_read(conn, buf, sizeof(buf))) > 0) {
      if (body.size() + static_cast<size_t>(n) > 32u * 1024u * 1024u) {
        mg_printf(conn, "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n");
        return 413;
      }
      body.append(buf, static_cast<size_t>(n));
    }
    const std::string resp = self->dispatchLine(body);  // marshals to main thread
    if (resp.empty()) {
      mg_printf(conn, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");
      return 202;
    }
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Content-Length: %d\r\n\r\n%s",
              static_cast<int>(resp.size()), resp.c_str());
    return 200;
  }
  if (std::strcmp(method, "OPTIONS") == 0) {
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
              "Access-Control-Allow-Headers: Content-Type, mcp-session-id\r\n"
              "Content-Length: 0\r\n\r\n");
    return 200;
  }
  const char* info = "lusdview MCP server. POST JSON-RPC 2.0 to this endpoint.\n";
  mg_printf(conn,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n\r\n%s",
            static_cast<int>(std::strlen(info)), info);
  return 405;
}

bool MCPServer::startHttp(const std::string& hostname, int port) {
  if (httpCtx_) return true;
  httpToken_.clear();
  if (const char* tokenPath = std::getenv("LUSDVIEW_MCP_TOKEN_FILE")) {
    std::ifstream tokenFile(tokenPath);
    if (!std::getline(tokenFile, httpToken_) || httpToken_.size() < 16 || httpToken_.size() > 4096) {
      LOGE("MCP token file is unreadable or invalid");
      return false;
    }
  }
  const std::string ports = hostname.empty()
                                ? std::to_string(port)
                                : hostname + ":" + std::to_string(port);
  const char* options[] = {"listening_ports", ports.c_str(), "num_threads", "4",
                           nullptr};
  httpCtx_ = mg_start(nullptr, this, options);
  if (!httpCtx_) {
    LOGE("MCP: failed to start HTTP server on %s", ports.c_str());
    return false;
  }
  mg_set_request_handler(httpCtx_, "/", HttpHandler, this);
  mg_set_request_handler(httpCtx_, "/mcp", HttpHandler, this);
  LOGI("MCP HTTP server listening on http://%s/mcp", ports.c_str());
  return true;
}

void MCPServer::stopHttp() {
  if (httpCtx_) {
    mg_stop(httpCtx_);
    httpCtx_ = nullptr;
  }
}

}  // namespace lusdview
