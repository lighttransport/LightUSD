#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/prim.hh"
#include "tydra/mcp-server.hh"
#include "tydra/command-and-history.hh"
#include "arg-parser.hh"

int main(int argc, char **argv) {

  using namespace lightusd;

  using namespace lightusd::legacy;  // NOLINT

  argparser::ArgParser parser;
  parser.add_option("--port", true, "Port number for the MCP server");
  parser.add_option("--host", true, "Hostname(default `localhost`)");
  parser.add_option("--stdio", false,
                    "Use stdio transport (newline-delimited JSON-RPC on "
                    "stdin/stdout) instead of HTTP");

  if (!parser.parse(argc, argv)) {
    std::cerr << "Error parsing arguments." << std::endl;
    parser.print_help();
    return -1;

  }

  // stdio transport: blocks until stdin closes. Protocol on stdout; keep
  // diagnostics off stdout so they never corrupt the JSON-RPC stream.
  if (parser.is_set("--stdio")) {
    tydra::mcp::MCPServer server;
    if (!server.run_stdio()) {
      std::cerr << "Failed to run MCP server (stdio).\n";
      return -1;
    }
    return 0;
  }

  int port = 8085;

  double portval;
  if (parser.is_set("--port")) {
    if (!parser.get("--port", portval)) {
      std::cerr << "--port is missing or invalid\n";
      return -1;
    }
    port = int(portval);
  }


  std::string hostname = "localhost";
  if (parser.is_set("--host")) {
    if (!parser.get("--host", hostname)) {
      std::cerr << "--host is missing or invalid\n";
      return -1;
    }
  }


  std::cout << "http://" + hostname << ":" << port << "/mcp" << "\n";

  tydra::mcp::MCPServer server;
  if (!server.init(port, hostname)) {
    std::cerr << "Failed to init MCP server.\n";
    return -1;
  }

  bool done =false;
  while (!done) {

#ifdef _WIN32
      Sleep(1000);
#else
      sleep(1);
#endif

    //Layer empty;
    //
    //tydra::EditHistory hist;
    //hist.layer = std::move(empty);

    //tydra::HistoryQueue queue;
    //if (!queue.push(std::move(hist))) {
    //  return -1;
    //}
  }

  server.stop();

  return 0;

}
