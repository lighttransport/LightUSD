#pragma once

#include <vector>
#include <string>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "mcp-resources.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

// for 'resources/list'
bool GetResourcesList(
  const Context &ctx,
  nlohmann::json &resources);

bool ReadResource(const Context &ctx, const std::string &uri, nlohmann::json &content);


} // namespace mcp
} // namespace tydra
} // namespace lightusd
