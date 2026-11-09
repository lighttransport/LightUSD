#pragma once

#include <vector>
#include <string>

#include "mcp-context.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {
namespace mcp {

/// Build the full tools/list response.
bool GetToolsList(Context &ctx, nlohmann::json &result);

/// Dispatch tool calls to the appropriate handler.
bool CallTool(Context &ctx, const std::string &tool_name,
              const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
