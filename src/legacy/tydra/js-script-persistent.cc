#include "js-script.hh"
#include "value-to-json.hh"

#include "../lightusd.hh"
#include "../stage.hh"
#include "../pprint-enum.hh"

#if defined(LIGHTUSD_WITH_QJS)

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "external/quickjs-ng/quickjs.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// QuickJS's JS_UNDEFINED/JS_NULL (-> JS_MKVAL(...)) macros trip clang's
// -Wdisabled-macro-expansion at every use site in the body below. Ignore just
// that diagnostic for the rest of this TU (clang-only warning).
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif

#include <sstream>
#include <functional>
#include <cstdio>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "mcp-tools-diff.hh"

namespace lightusd {
namespace tydra {

namespace {

// ---------------------------------------------------------------------------
// Helpers: JSValue <-> JSON marshalling
// ---------------------------------------------------------------------------
JSValue JSONToJSValue(JSContext *ctx, const nlohmann::json &j) {
  std::string json_str = j.dump();
  return JS_ParseJSON(ctx, json_str.c_str(), json_str.length(), "<json>");
}

nlohmann::json JSValueToJSON(JSContext *ctx, JSValue val) {
  // Serialize via JSON.stringify: JS_ToCString on an object/array yields
  // "[object Object]" (invalid JSON). undefined stringifies to a NULL/"undefined"
  // result -> treated as null. Non-throwing parse (built with -fno-exceptions).
  JSValue json_val = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json_val)) {
    JS_FreeValue(ctx, json_val);
    return nullptr;
  }
  nlohmann::json j = nullptr;
  const char *json_str = JS_ToCString(ctx, json_val);
  if (json_str) {
    j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_discarded()) j = nullptr;
    JS_FreeCString(ctx, json_str);
  }
  JS_FreeValue(ctx, json_val);
  return j;
}

// ---------------------------------------------------------------------------
// Current context for JS function callbacks
// ---------------------------------------------------------------------------
// These global pointers are set before executing each script. Since
// QuickJS is single-threaded per runtime, this is safe within a session.
static Stage *g_js_stage = nullptr;
static mcp::DiffSession *g_js_diff = nullptr;

// ---------------------------------------------------------------------------
// JS function: lightusd.stage.info()
// ---------------------------------------------------------------------------
static JSValue js_stage_info(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic,
                             JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)argv; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }

  nlohmann::json info;
  const auto &metas = g_js_stage->metas();
  info["upAxis"] = lightusd::to_string(metas.upAxis.get_value());
  info["defaultPrim"] = metas.defaultPrim.str();
  info["metersPerUnit"] = metas.metersPerUnit.get_value();
  info["timeCodesPerSecond"] = metas.timeCodesPerSecond.get_value();
  info["framesPerSecond"] = metas.framesPerSecond.get_value();
  info["startTimeCode"] = metas.startTimeCode.get_value();

  double endTC = metas.endTimeCode.get_value();
  if (std::isinf(endTC)) {
    info["endTimeCode"] = nullptr;
  } else {
    info["endTimeCode"] = endTC;
  }
  info["rootPrimCount"] = g_js_stage->root_prims().size();

  return JSONToJSValue(ctx, info);
}

// ---------------------------------------------------------------------------
// Recursive prim tree to JSON helper
// ---------------------------------------------------------------------------
static void PrimToJSJSON(const Prim &prim, nlohmann::json &j, int max_depth) {
  j["name"] = prim.element_name();
  j["type"] = prim.prim_type_name();
  switch (prim.specifier()) {
    case Specifier::Def: j["specifier"] = "def"; break;
    case Specifier::Over: j["specifier"] = "over"; break;
    case Specifier::Class: j["specifier"] = "class"; break;
    default: j["specifier"] = "invalid"; break;
  }

  const auto &children = prim.children();
  if (max_depth != 0 && !children.empty()) {
    nlohmann::json child_arr = nlohmann::json::array();
    int next_depth = (max_depth > 0) ? max_depth - 1 : max_depth;
    for (const auto &child : children) {
      nlohmann::json c;
      PrimToJSJSON(child, c, next_depth);
      child_arr.push_back(c);
    }
    j["children"] = child_arr;
  } else if (!children.empty()) {
    j["childCount"] = children.size();
  }
}

// ---------------------------------------------------------------------------
// JS function: lightusd.prim.list(path, opts?)
// ---------------------------------------------------------------------------
static JSValue js_prim_list(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic,
                            JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }

  // Default: list root prims at "/"
  std::string path_str = "/";
  int max_depth = -1;

  if (argc > 0 && JS_IsString(argv[0])) {
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { path_str = s; JS_FreeCString(ctx, s); }
  }
  if (argc > 1 && JS_IsObject(argv[1])) {
    nlohmann::json opts = JSValueToJSON(ctx, argv[1]);
    if (opts.contains("maxDepth") && opts["maxDepth"].is_number()) {
      max_depth = opts["maxDepth"].get<int>();
    }
  }

  if (path_str == "/") {
    nlohmann::json result = nlohmann::json::array();
    for (const auto &prim : g_js_stage->root_prims()) {
      nlohmann::json j;
      PrimToJSJSON(prim, j, max_depth);
      result.push_back(j);
    }
    nlohmann::json wrap;
    wrap["prims"] = result;
    wrap["count"] = result.size();
    return JSONToJSValue(ctx, wrap);
  }

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    return JS_ThrowTypeError(ctx, "Invalid path: %s", path_str.c_str());
  }

  const Prim *prim = nullptr;
  std::string err;
  if (!g_js_stage->find_prim_at_path(path, prim, &err)) {
    return JS_ThrowTypeError(ctx, "Prim not found: %s", path_str.c_str());
  }

  nlohmann::json j;
  PrimToJSJSON(*prim, j, max_depth);
  nlohmann::json wrap;
  wrap["prim"] = j;
  return JSONToJSValue(ctx, wrap);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.prim.get(path)
// ---------------------------------------------------------------------------
static JSValue js_prim_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic,
                           JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "prim.get() requires a path string");
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  std::string path_str(s);
  JS_FreeCString(ctx, s);

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    return JS_ThrowTypeError(ctx, "Invalid path: %s", path_str.c_str());
  }

  const Prim *prim = nullptr;
  std::string err;
  if (!g_js_stage->find_prim_at_path(path, prim, &err)) {
    return JS_ThrowTypeError(ctx, "Prim not found: %s", path_str.c_str());
  }

  nlohmann::json j;
  j["name"] = prim->element_name();
  j["path"] = prim->absolute_path().full_path_name();
  j["type"] = prim->prim_type_name();
  switch (prim->specifier()) {
    case Specifier::Def: j["specifier"] = "def"; break;
    case Specifier::Over: j["specifier"] = "over"; break;
    case Specifier::Class: j["specifier"] = "class"; break;
    default: j["specifier"] = "invalid"; break;
  }

  // Children
  nlohmann::json children = nlohmann::json::array();
  for (const auto &c : prim->children()) {
    nlohmann::json cc;
    cc["name"] = c.element_name();
    cc["type"] = c.prim_type_name();
    children.push_back(cc);
  }
  j["children"] = children;
  j["childCount"] = prim->children().size();

  // Metadata
  j["metadata"] = PrimMetaToJSON(prim->metas());

  return JSONToJSValue(ctx, j);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.stage.exportToString()
// ---------------------------------------------------------------------------
static JSValue js_stage_export(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)argv; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }

  std::string usda = g_js_stage->ExportToString();
  return JS_NewString(ctx, usda.c_str());
}

// ---------------------------------------------------------------------------
// JS function: lightusd.prim.listChildren(path)
// ---------------------------------------------------------------------------
static JSValue js_prim_listChildren(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic,
                                     JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }

  std::string path_str = "/";
  if (argc > 0 && JS_IsString(argv[0])) {
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { path_str = s; JS_FreeCString(ctx, s); }
  }

  const Prim *prim = nullptr;
  if (path_str == "/") {
    nlohmann::json result = nlohmann::json::array();
    for (const auto &p : g_js_stage->root_prims()) {
      nlohmann::json j;
      j["name"] = p.element_name();
      j["type"] = p.prim_type_name();
      j["childCount"] = p.children().size();
      result.push_back(j);
    }
    nlohmann::json wrap;
    wrap["prims"] = result;
    wrap["count"] = result.size();
    return JSONToJSValue(ctx, wrap);
  }

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    return JS_ThrowTypeError(ctx, "Invalid path: %s", path_str.c_str());
  }

  std::string err;
  if (!g_js_stage->find_prim_at_path(path, prim, &err)) {
    return JS_ThrowTypeError(ctx, "Prim not found: %s", path_str.c_str());
  }

  nlohmann::json result = nlohmann::json::array();
  for (const auto &c : prim->children()) {
    nlohmann::json j;
    j["name"] = c.element_name();
    j["type"] = c.prim_type_name();
    j["childCount"] = c.children().size();
    result.push_back(j);
  }

  nlohmann::json wrap;
  wrap["prims"] = result;
  wrap["count"] = result.size();
  return JSONToJSValue(ctx, wrap);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.query.findAllByType(typeName)
// ---------------------------------------------------------------------------
static void CollectPrimsByType(const Prim &prim, const std::string &type_name,
                               std::vector<std::string> &results) {
  if (prim.prim_type_name() == type_name) {
    results.push_back(prim.absolute_path().full_path_name());
  }
  for (const auto &child : prim.children()) {
    CollectPrimsByType(child, type_name, results);
  }
}

static JSValue js_query_findAllByType(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic,
                                       JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)magic; (void)func_data;
  if (!g_js_stage) {
    return JS_ThrowTypeError(ctx, "No stage loaded");
  }
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "findAllByType() requires a type name string");
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  std::string type_name(s);
  JS_FreeCString(ctx, s);

  std::vector<std::string> found;
  for (const auto &prim : g_js_stage->root_prims()) {
    CollectPrimsByType(prim, type_name, found);
  }

  nlohmann::json result = nlohmann::json::array();
  for (const auto &p : found) {
    result.push_back(p);
  }

  nlohmann::json wrap;
  wrap["paths"] = result;
  wrap["count"] = found.size();
  return JSONToJSValue(ctx, wrap);
}

// ---------------------------------------------------------------------------
// Module initialisation helper: register a function on an object
// ---------------------------------------------------------------------------
static void RegisterFunction(JSContext *ctx, JSValue obj, const char *name,
                             JSCFunctionData *func, int length) {
  JSValue f = JS_NewCFunctionData(ctx, func, length, 0, 0, nullptr);
  JS_SetPropertyStr(ctx, obj, name, f);
}

static void RegisterObject(JSContext *ctx, JSValue parent, const char *name,
                           JSValue obj) {
  JS_SetPropertyStr(ctx, parent, name, obj);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.diff.summary()
// ---------------------------------------------------------------------------
static JSValue js_diff_summary(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic,
                               JSValueConst *func_data) {
  (void)this_val; (void)argc; (void)argv; (void)magic; (void)func_data;
  if (!g_js_diff) {
    return JS_ThrowTypeError(ctx, "No diff loaded (call diff_open first)");
  }
  nlohmann::json out;
  mcp::DiffComputeSummary(*g_js_diff, out);
  return JSONToJSValue(ctx, out);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.diff.paths(filter?)
// ---------------------------------------------------------------------------
static JSValue js_diff_paths(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic,
                             JSValueConst *func_data) {
  (void)this_val; (void)magic; (void)func_data;
  if (!g_js_diff) {
    return JS_ThrowTypeError(ctx, "No diff loaded (call diff_open first)");
  }
  nlohmann::json filter = nlohmann::json::object();
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    filter = JSValueToJSON(ctx, argv[0]);
    if (!filter.is_object()) filter = nlohmann::json::object();
  }
  nlohmann::json out;
  mcp::DiffComputePaths(*g_js_diff, filter, out);
  return JSONToJSValue(ctx, out);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.diff.prim(path)
// ---------------------------------------------------------------------------
static JSValue js_diff_prim(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic,
                            JSValueConst *func_data) {
  (void)this_val; (void)magic; (void)func_data;
  if (!g_js_diff) {
    return JS_ThrowTypeError(ctx, "No diff loaded (call diff_open first)");
  }
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "diff.prim() requires a path string");
  }
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  std::string path(s);
  JS_FreeCString(ctx, s);

  nlohmann::json out;
  mcp::DiffComputePrim(*g_js_diff, path, out);
  return JSONToJSValue(ctx, out);
}

// ---------------------------------------------------------------------------
// JS function: lightusd.diff.tree({path?, depth?})
// ---------------------------------------------------------------------------
static JSValue js_diff_tree(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic,
                            JSValueConst *func_data) {
  (void)this_val; (void)magic; (void)func_data;
  if (!g_js_diff) {
    return JS_ThrowTypeError(ctx, "No diff loaded (call diff_open first)");
  }
  std::string root = "/";
  int depth = 2;
  if (argc >= 1 && JS_IsObject(argv[0])) {
    nlohmann::json o = JSValueToJSON(ctx, argv[0]);
    if (o.is_object()) {
      if (o.contains("path") && o["path"].is_string()) {
        root = o["path"].get<std::string>();
      }
      if (o.contains("depth") && o["depth"].is_number()) {
        depth = o["depth"].get<int>();
      }
    }
  }
  if (root.empty()) root = "/";
  nlohmann::json out;
  mcp::DiffComputeTree(*g_js_diff, root, depth, out);
  return JSONToJSValue(ctx, out);
}

// ---------------------------------------------------------------------------
// console.{log,info,warn,error}(...) -> stderr
// ---------------------------------------------------------------------------
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic,
                              JSValueConst *func_data) {
  (void)this_val; (void)magic; (void)func_data;
  std::string line;
  for (int i = 0; i < argc; i++) {
    const char *s = JS_ToCString(ctx, argv[i]);
    if (s) {
      if (i) line += ' ';
      line += s;
      JS_FreeCString(ctx, s);
    }
  }
  std::fprintf(stderr, "%s\n", line.c_str());
  return JS_UNDEFINED;
}

} // namespace

// ===========================================================================
// Public: InitJSEngine
// ===========================================================================
bool InitJSEngine(JSEngineState &engine, std::string &err) {
  if (engine.initialized) {
    return true; // already initialized
  }

  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    err = "Failed to create JavaScript runtime";
    return false;
  }

  // Set memory limits (64MB stack, 256MB total)
  JS_SetMemoryLimit(rt, 256 * 1024 * 1024);
  JS_SetMaxStackSize(rt, 64 * 1024 * 1024);

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    err = "Failed to create JavaScript context";
    JS_FreeRuntime(rt);
    return false;
  }

  // Minimal console.{log,info,warn,error} -> stderr. We deliberately avoid
  // quickjs-libc's js_std_add_helpers (not built into the library, and its
  // console writes to stdout, which would corrupt the stdio MCP transport).
  {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    RegisterFunction(ctx, console, "log", js_console_log, 1);
    RegisterFunction(ctx, console, "info", js_console_log, 1);
    RegisterFunction(ctx, console, "warn", js_console_log, 1);
    RegisterFunction(ctx, console, "error", js_console_log, 1);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
  }

  engine.runtime = rt;
  engine.context = ctx;
  engine.initialized = true;
  return true;
}

// ===========================================================================
// Public: DestroyJSEngine
// ===========================================================================
bool DestroyJSEngine(JSEngineState &engine) {
  if (!engine.initialized) return true;

  JSContext *ctx = static_cast<JSContext *>(engine.context);
  JSRuntime *rt = static_cast<JSRuntime *>(engine.runtime);

  if (ctx) {
    JS_FreeContext(ctx);
  }
  if (rt) {
    JS_FreeRuntime(rt);
  }

  engine.runtime = nullptr;
  engine.context = nullptr;
  engine.initialized = false;
  return true;
}

// ===========================================================================
// Public: RegisterUSDModule
// ===========================================================================
bool RegisterUSDModule(JSEngineState &engine, Stage *stage,
                       std::string &err) {
  if (!engine.initialized) {
    err = "JS engine not initialized";
    return false;
  }

  JSContext *ctx = static_cast<JSContext *>(engine.context);
  g_js_stage = stage;

  // Create the lightusd namespace
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue lightusd = JS_NewObject(ctx);

  // --- stage sub-module ---
  JSValue stage_mod = JS_NewObject(ctx);
  RegisterFunction(ctx, stage_mod, "info", js_stage_info, 0);
  RegisterFunction(ctx, stage_mod, "exportToString", js_stage_export, 0);
  RegisterObject(ctx, lightusd, "stage", stage_mod);

  // --- prim sub-module ---
  JSValue prim_mod = JS_NewObject(ctx);
  RegisterFunction(ctx, prim_mod, "list", js_prim_list, 2);
  RegisterFunction(ctx, prim_mod, "get", js_prim_get, 1);
  RegisterFunction(ctx, prim_mod, "listChildren", js_prim_listChildren, 1);
  RegisterObject(ctx, lightusd, "prim", prim_mod);

  // --- query sub-module ---
  JSValue query_mod = JS_NewObject(ctx);
  RegisterFunction(ctx, query_mod, "findAllByType", js_query_findAllByType, 1);
  RegisterObject(ctx, lightusd, "query", query_mod);

  // --- value helper ---
  JSValue value_mod = JS_NewObject(ctx);
  // value.create will be a JS function that wraps values
  RegisterObject(ctx, lightusd, "value", value_mod);

  // Set the module
  JS_SetPropertyStr(ctx, global, "lightusd", lightusd);
  JS_FreeValue(ctx, global);

  return true;
}

// ===========================================================================
// Public: RegisterDiffModule
// ===========================================================================
// Attach a `lightusd.diff.{summary,paths,prim}` submodule bound to `diff`.
// Creates the `lightusd` global if it does not already exist (e.g. a diff-only
// session with no stage), otherwise augments it in place.
bool RegisterDiffModule(JSEngineState &engine, mcp::DiffSession *diff,
                        std::string &err) {
  if (!engine.initialized) {
    err = "JS engine not initialized";
    return false;
  }

  JSContext *ctx = static_cast<JSContext *>(engine.context);
  g_js_diff = diff;

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue lightusd = JS_GetPropertyStr(ctx, global, "lightusd");
  const bool created = JS_IsUndefined(lightusd);
  if (created) {
    lightusd = JS_NewObject(ctx);
  }

  JSValue diff_mod = JS_NewObject(ctx);
  RegisterFunction(ctx, diff_mod, "summary", js_diff_summary, 0);
  RegisterFunction(ctx, diff_mod, "paths", js_diff_paths, 1);
  RegisterFunction(ctx, diff_mod, "prim", js_diff_prim, 1);
  RegisterFunction(ctx, diff_mod, "tree", js_diff_tree, 1);
  RegisterObject(ctx, lightusd, "diff", diff_mod);

  if (created) {
    // Transfers our `lightusd` reference to the global.
    JS_SetPropertyStr(ctx, global, "lightusd", lightusd);
  } else {
    // We augmented the existing global object in place; release our extra ref
    // obtained from JS_GetPropertyStr.
    JS_FreeValue(ctx, lightusd);
  }
  JS_FreeValue(ctx, global);

  return true;
}

// ===========================================================================
// Public: RunJSScript (persistent engine)
// ===========================================================================
bool RunJSScript(JSEngineState &engine, const std::string &js_code,
                 nlohmann::json &return_value, std::string &err) {
  if (!engine.initialized) {
    err = "JS engine not initialized";
    return false;
  }

  JSContext *ctx = static_cast<JSContext *>(engine.context);

  // Evaluate
  JSValue result = JS_Eval(ctx, js_code.c_str(), js_code.length(),
                           "<eval>", JS_EVAL_TYPE_GLOBAL);

  if (JS_IsException(result)) {
    JSValue exception = JS_GetException(ctx);
    const char *error_str = JS_ToCString(ctx, exception);
    if (error_str) {
      err = "JavaScript error: ";
      err += error_str;
      JS_FreeCString(ctx, error_str);
    } else {
      err = "JavaScript error: unknown";
    }
    JS_FreeValue(ctx, exception);
    return_value = nullptr;
    return false;
  }

  // Capture return value
  return_value = JSValueToJSON(ctx, result);
  JS_FreeValue(ctx, result);
  return true;
}

} // namespace tydra
} // namespace lightusd

#else // !LIGHTUSD_WITH_QJS

namespace lightusd {
namespace tydra {

bool InitJSEngine(JSEngineState &engine, std::string &err) {
  (void)engine;
  err = "JavaScript (QuickJS) not enabled. Build with -DLIGHTUSD_WITH_QJS=ON";
  return false;
}

bool DestroyJSEngine(JSEngineState &engine) {
  (void)engine;
  return true;
}

bool RegisterUSDModule(JSEngineState &engine, Stage *stage,
                       std::string &err) {
  (void)engine; (void)stage;
  err = "JavaScript (QuickJS) not enabled.";
  return false;
}

bool RegisterDiffModule(JSEngineState &engine, mcp::DiffSession *diff,
                        std::string &err) {
  (void)engine; (void)diff;
  err = "JavaScript (QuickJS) not enabled.";
  return false;
}

bool RunJSScript(JSEngineState &engine, const std::string &js_code,
                 nlohmann::json &return_value, std::string &err) {
  (void)engine; (void)js_code; (void)return_value;
  err = "JavaScript (QuickJS) not enabled.";
  return false;
}

} // namespace tydra
} // namespace lightusd

#endif // LIGHTUSD_WITH_QJS
