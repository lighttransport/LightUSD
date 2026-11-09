#include "mcp-js-bridge.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "../lightusd.hh"
#include "js-script.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

// ===========================================================================
// EnsureJSEngineReady
// ===========================================================================
bool EnsureJSEngineReady(Context &ctx, std::string &err) {
  if (!ctx.js_engine) {
    ctx.js_engine = std::unique_ptr<JSEngineState>(new JSEngineState());
  }

  if (!ctx.js_engine->initialized) {
    if (!InitJSEngine(*ctx.js_engine, err)) {
      return false;
    }
  }

  // Re-register the USD module if the stage changed or the module isn't set
  // We do this lazily on each run_script call for now
  if (ctx.stage) {
    // Re-register the module (this replaces the previous module)
    if (!RegisterUSDModule(*ctx.js_engine, ctx.stage.get(), err)) {
      return false;
    }
  }

  // Expose lightusd.diff.* when a diff is cached in the session. Done after
  // RegisterUSDModule so it augments (rather than is clobbered by) the
  // lightusd global; works standalone too (no stage required).
  if (ctx.diff) {
    if (!RegisterDiffModule(*ctx.js_engine, ctx.diff.get(), err)) {
      return false;
    }
  }

  return true;
}

// ===========================================================================
// RunScript
// ===========================================================================
bool RunScript(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err) {
  if (!args.contains("script") || !args["script"].is_string()) {
    err = "Missing 'script' argument (JavaScript code string)";
    return false;
  }

  std::string js_code = args["script"].get<std::string>();

  if (!EnsureJSEngineReady(ctx, err)) {
    return false;
  }

  nlohmann::json return_value;
  if (!RunJSScript(*ctx.js_engine, js_code, return_value, err)) {
    // Return the error gracefully rather than failing the tool call
    result["error"] = err;
    result["result"] = nullptr;
    err.clear(); // Don't propagate as MCP error
    return true;
  }

  result["result"] = return_value;
  return true;
}

} // namespace mcp
} // namespace tydra
} // namespace lightusd
