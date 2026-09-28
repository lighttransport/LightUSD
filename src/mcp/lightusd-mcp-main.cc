// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// lightusd-mcp: MCP server over stdio (newline-delimited JSON-RPC 2.0).
#include <cstring>
#include <iostream>

#include "lightusd-mcp.hh"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      std::cout << "Usage: " << argv[0]
                << "\n  LightUSD MCP server. Reads newline-delimited JSON-RPC 2.0"
                   " requests on stdin and writes responses to stdout.\n";
      return 0;
    }
  }
  std::ios::sync_with_stdio(false);
  return lightusd::mcp::RunStdio(std::cin, std::cout);
}
