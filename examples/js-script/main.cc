// SPDX-License-Identifier: Apache-2.0
// Embed QuickJS over a core Stage's authored layer metadata.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "external/quickjs-ng/quickjs.h"
#include "load-usd.hh"
#define JSON_NOEXCEPTION
#include "external/jsonhpp/nlohmann/json.hpp"

namespace {
JSValue print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  for (int i = 0; i < argc; ++i) {
    const char* text = JS_ToCString(ctx, argv[i]);
    if (!text) return JS_EXCEPTION;
    if (i) std::cout << ' ';
    std::cout << text;
    JS_FreeCString(ctx, text);
  }
  std::cout << '\n';
  return JS_UNDEFINED;
}
JSValue metadata(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  const auto* stage =
      static_cast<const lightusd::Stage*>(JS_GetContextOpaque(ctx));
  if (!stage || !stage->GetRootLayer()) return JS_NULL;
  const auto& meta = stage->GetRootLayer()->meta();
  nlohmann::json value = {{"upAxis", meta.upAxis},
                          {"defaultPrim", meta.defaultPrim},
                          {"comment", meta.comment},
                          {"doc", meta.doc},
                          {"playbackMode", meta.playbackMode},
                          {"autoPlay", meta.autoPlay},
                          {"metersPerUnit", meta.metersPerUnit},
                          {"timeCodesPerSecond", meta.timeCodesPerSecond},
                          {"framesPerSecond", meta.framesPerSecond},
                          {"kilogramsPerUnit", meta.kilogramsPerUnit},
                          {"startTimeCode", meta.startTimeCode}};
  value["endTimeCode"] = meta.endTimeCode_set ? nlohmann::json(meta.endTimeCode)
                                              : nlohmann::json(nullptr);
  value["subLayers"] = nlohmann::json::array();
  for (size_t i = 0; i < meta.subLayers.size(); ++i) {
    const auto offset = i < meta.subLayerOffsets.size()
                            ? meta.subLayerOffsets[i]
                            : std::make_pair(0.0, 1.0);
    value["subLayers"].push_back(
        {{"assetPath", meta.subLayers[i]},
         {"layerOffset",
          {{"offset", offset.first}, {"scale", offset.second}}}});
  }
  value["primChildren"] = nlohmann::json::array();
  for (const auto& prim : stage->GetRootPrims())
    value["primChildren"].push_back(prim.GetName());
  // Replace invalid input UTF-8 rather than throwing from the optional JSON
  // serializer. QuickJS propagates its own allocation/parse exceptions.
  const std::string json =
      value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  return JS_ParseJSON(ctx, json.data(), json.size(), "<layer-metadata>");
}

struct Rejections {
  std::vector<JSValue> promises;
  bool overflow = false;
};
void rejection(JSContext* ctx, JSValueConst promise, JSValueConst, bool handled,
               void* opaque) {
  auto& state = *static_cast<Rejections*>(opaque);
  const auto found = std::find_if(
      state.promises.begin(), state.promises.end(),
      [&](JSValue item) { return JS_IsStrictEqual(ctx, item, promise); });
  if (handled) {
    if (found != state.promises.end()) {
      JS_FreeValue(ctx, *found);
      state.promises.erase(found);
    }
  } else if (found == state.promises.end()) {
    if (state.promises.size() >= 256)
      state.overflow = true;
    else
      state.promises.push_back(JS_DupValue(ctx, promise));
  }
}
int interrupt(JSRuntime*, void* opaque) {
  auto deadline =
      static_cast<const std::chrono::steady_clock::time_point*>(opaque);
  return std::chrono::steady_clock::now() >= *deadline;
}
void report(JSContext* ctx) {
  const JSValue exception = JS_GetException(ctx);
  const char* message = JS_ToCString(ctx, exception);
  std::cerr << "JavaScript error: " << (message ? message : "out of memory")
            << '\n';
  if (message) JS_FreeCString(ctx, message);
  JS_FreeValue(ctx, exception);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: js-script <USD_FILE> <JS_SCRIPT>\n";
    return 0;
  }
  if (argc != 3) {
    std::cerr << "Usage: js-script <USD_FILE> <JS_SCRIPT>\n";
    return 1;
  }
  lightusd::Stage stage;
  std::string warn, err;
  const bool loaded = lightusd::LoadUSD(argv[1], &stage, &warn, &err);
  if (!warn.empty()) std::cerr << warn << '\n';
  if (!loaded) {
    std::cerr << err << '\n';
    return 1;
  }
  std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
  const auto size = input ? input.tellg() : std::streampos(-1);
  if (size < 0 || size > std::streamoff(16 * 1024 * 1024)) {
    std::cerr << "Cannot read script, or script exceeds 16 MiB\n";
    return 1;
  }
  input.seekg(0);
  std::string script(static_cast<size_t>(size), '\0');
  if (!script.empty() &&
      !input.read(&script[0], static_cast<std::streamsize>(script.size()))) {
    std::cerr << "Failed to read script\n";
    return 1;
  }
  JSRuntime* runtime = JS_NewRuntime();
  if (!runtime) return 1;
  JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
  JS_SetMaxStackSize(runtime, 1024 * 1024);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  JS_SetInterruptHandler(runtime, interrupt, &deadline);
  JSContext* ctx = JS_NewContext(runtime);
  if (!ctx) {
    JS_FreeRuntime(runtime);
    return 1;
  }
  Rejections rejections;
  JS_SetHostPromiseRejectionTracker(runtime, rejection, &rejections);
  JS_SetContextOpaque(ctx, &stage);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, print, "log", 1));
  JS_SetPropertyStr(ctx, global, "console", console);
  JS_SetPropertyStr(ctx, global, "getLayerMetas",
                    JS_NewCFunction(ctx, metadata, "getLayerMetas", 0));
  JS_FreeValue(ctx, global);
  JSValue result =
      JS_Eval(ctx, script.data(), script.size(), argv[2], JS_EVAL_TYPE_GLOBAL);
  bool ok = !JS_IsException(result);
  if (!ok) report(ctx);
  JS_FreeValue(ctx, result);
  JSContext* job_ctx = nullptr;
  int job = 0;
  while (ok && (job = JS_ExecutePendingJob(runtime, &job_ctx)) > 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "JavaScript job deadline exceeded\n";
      ok = false;
    }
  }
  if (job < 0) {
    report(job_ctx);
    ok = false;
  }
  if (!rejections.promises.empty() || rejections.overflow) {
    std::cerr << "Unhandled JavaScript Promise rejection\n";
    ok = false;
  }
  for (JSValue promise : rejections.promises) JS_FreeValue(ctx, promise);
  JS_SetHostPromiseRejectionTracker(runtime, nullptr, nullptr);
  JS_FreeContext(ctx);
  JS_FreeRuntime(runtime);
  return ok ? 0 : 1;
}
