#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// Ensure the session's JS engine is initialized and the lightusd.* module
/// is registered against the current stage.
bool EnsureJSEngineReady(Context &ctx, std::string &err);

/// run_script: Execute JavaScript code against the session stage.
/// Args: { script: string }
/// Returns: { result: <any>, error?: string }
bool RunScript(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
