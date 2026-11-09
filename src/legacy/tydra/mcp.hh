///
/// @file mcp.hh
/// @brief MCP (Model Context Protocol) integration for LightUSD
///
/// Provides basic data structures for MCP server functionality,
/// allowing LightUSD to act as an MCP server for USD scene manipulation
/// and querying through language model interfaces.
///
/// MCP enables AI assistants to interact with USD scenes through
/// standardized protocols for reading, writing, and analyzing 3D content.
///
#pragma once

#include <string>

namespace lightusd {
namespace tydra {

///
/// Basic MCP command structure for USD operations.
/// Represents a command with arguments that can be processed
/// by the MCP server interface.
///
struct MCPCommand
{
  std::string cmd;  ///< Command name (e.g., "list_prims", "get_mesh")
  std::string arg;  ///< Command arguments (JSON or string format)
};


} // namespace tydra
} // namespace lightusd
