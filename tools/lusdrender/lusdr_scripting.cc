// SPDX-License-Identifier: Apache-2.0
// lusdrender — embedded QuickJS scripting + MCP server (stdio / HTTP). Compiles
// to nothing unless LIGHTUSD_WITH_QJS is defined.
#ifdef LIGHTUSD_WITH_QJS
#include <fstream>
#include <sstream>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "tydra/js-script.hh"
extern "C" {
#include "external/quickjs-ng/quickjs.h"
}
#include "lusdr_context.hh"

namespace lusdr {

namespace qjs {

constexpr double kPiD = 3.14159265358979323846;
static RenderContext *g_render_ctx = nullptr;

double ArgD(JSContext *ctx, int argc, JSValueConst *argv, int i, double def) {
  if (i >= argc) return def;
  double d = def;
  if (JS_ToFloat64(ctx, &d, argv[i]) < 0) return def;
  return d;
}

JSValue Vec3JS(JSContext *ctx, const Vec3 &v) {
  JSValue a = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, v.x));
  JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, v.y));
  JS_SetPropertyUint32(ctx, a, 2, JS_NewFloat64(ctx, v.z));
  return a;
}

void LookAt(CameraFrame &c, const Vec3 &eye, const Vec3 &target, const Vec3 &up,
            float fov_deg) {
  c.origin = eye;
  c.forward = Normalize(Sub(target, eye));
  c.right = Normalize(Cross(c.forward, up));
  if (Length(c.right) < 1.0e-6f) c.right = Vec3{1.0f, 0.0f, 0.0f};
  c.up = Normalize(Cross(c.right, c.forward));
  c.yfov = fov_deg * float(kPiD) / 180.0f;
  c.ortho = false;
  const float dist = Length(Sub(target, eye));
  c.znear = std::max(1.0e-4f, dist * 1.0e-4f);
  c.zfar = std::max(1000.0f, dist * 100.0f);
}

// lusdrender.setCamera(ex,ey,ez, tx,ty,tz, [fovDeg], [ux,uy,uz])
JSValue js_setCamera(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                     int, JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  Vec3 eye{float(ArgD(ctx, argc, argv, 0, 0)), float(ArgD(ctx, argc, argv, 1, 0)),
           float(ArgD(ctx, argc, argv, 2, 0))};
  Vec3 tgt{float(ArgD(ctx, argc, argv, 3, 0)), float(ArgD(ctx, argc, argv, 4, 0)),
           float(ArgD(ctx, argc, argv, 5, 0))};
  float fov = float(ArgD(ctx, argc, argv, 6,
                         double(g_render_ctx->camera.yfov) * 180.0 / kPiD));
  Vec3 up = AxisVec(g_render_ctx->up_axis);
  if (argc >= 10) {
    up = Vec3{float(ArgD(ctx, argc, argv, 7, up.x)),
              float(ArgD(ctx, argc, argv, 8, up.y)),
              float(ArgD(ctx, argc, argv, 9, up.z))};
  }
  LookAt(g_render_ctx->camera, eye, tgt, up, fov);
  return JS_UNDEFINED;
}

// lusdrender.orbit(azimuthDeg, elevationDeg, distanceScale) -- position the
// camera around the bounds center looking at it (animation-friendly).
JSValue js_orbit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int,
                 JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  RenderContext &rc = *g_render_ctx;
  const Vec3 center = Mul(Add(rc.bounds.lo, rc.bounds.hi), 0.5f);
  const float radius =
      std::max(1.0e-3f, Length(Sub(rc.bounds.hi, rc.bounds.lo)) * 0.5f);
  const double az = ArgD(ctx, argc, argv, 0, 0.0) * kPiD / 180.0;
  const double el = ArgD(ctx, argc, argv, 1, 20.0) * kPiD / 180.0;
  const double dist = radius * ArgD(ctx, argc, argv, 2, 2.5);
  const Vec3 up = AxisVec(rc.up_axis);
  const Vec3 ref = (std::fabs(up.y) < 0.9f) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  const Vec3 e0 = Normalize(Cross(up, ref));   // azimuth 0
  const Vec3 e1 = Normalize(Cross(up, e0));    // azimuth 90
  const float ce = float(std::cos(el)), se = float(std::sin(el));
  const Vec3 horiz = Add(Mul(e0, float(std::cos(az)) * ce),
                         Mul(e1, float(std::sin(az)) * ce));
  const Vec3 dir = Add(horiz, Mul(up, se));
  const Vec3 eye = Add(center, Mul(dir, float(dist)));
  LookAt(rc.camera, eye, center, up,
         float(double(rc.camera.yfov) * 180.0 / kPiD));
  rc.camera.znear = std::max(1.0e-4f, float(dist) * 1.0e-3f);
  rc.camera.zfar = float(dist) + radius * 4.0f;
  return JS_UNDEFINED;
}

JSValue js_setResolution(JSContext *ctx, JSValueConst, int argc,
                         JSValueConst *argv, int, JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  g_render_ctx->width =
      std::max(1, int(ArgD(ctx, argc, argv, 0, g_render_ctx->width)));
  g_render_ctx->height =
      std::max(1, int(ArgD(ctx, argc, argv, 1, g_render_ctx->height)));
  return JS_UNDEFINED;
}
JSValue js_setAmbient(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                      int, JSValueConst *) {
  if (g_render_ctx)
    g_render_ctx->opt.ambient =
        float(ArgD(ctx, argc, argv, 0, g_render_ctx->opt.ambient));
  return JS_UNDEFINED;
}
JSValue js_setBackground(JSContext *ctx, JSValueConst, int argc,
                         JSValueConst *argv, int, JSValueConst *) {
  if (g_render_ctx)
    g_render_ctx->opt.bg = Vec3{float(ArgD(ctx, argc, argv, 0, 0)),
                                float(ArgD(ctx, argc, argv, 1, 0)),
                                float(ArgD(ctx, argc, argv, 2, 0))};
  return JS_UNDEFINED;
}
JSValue js_setShadows(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                      int, JSValueConst *) {
  if (g_render_ctx && argc > 0)
    g_render_ctx->opt.shadows = JS_ToBool(ctx, argv[0]) != 0;
  return JS_UNDEFINED;
}
JSValue js_setSamples(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                      int, JSValueConst *) {
  if (g_render_ctx)
    g_render_ctx->opt.samples =
        std::max(1, int(ArgD(ctx, argc, argv, 0, g_render_ctx->opt.samples)));
  return JS_UNDEFINED;
}
JSValue js_autoframe(JSContext *ctx, JSValueConst, int, JSValueConst *, int,
                     JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  g_render_ctx->opt.autoframe = true;
  g_render_ctx->opt.camera.clear();
  ResolveCameraNext(*g_render_ctx);
  return JS_UNDEFINED;
}
// lusdrender.setTime(t) -- re-evaluate geometry/transforms at time t and rebuild
// the BVH. Call with no/NaN arg for the default time. Returns {triangles}.
JSValue js_setTime(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                   int, JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  const double t =
      ArgD(ctx, argc, argv, 0, std::numeric_limits<double>::quiet_NaN());
  if (!ExtractAndBuildBVH(*g_render_ctx, t)) {
    return JS_ThrowInternalError(ctx, "failed to rebuild geometry at time");
  }
  g_render_ctx->geometry_bounds = g_render_ctx->bounds;
  RefreshVolumesNext(*g_render_ctx, t);
  ResolveCameraNext(*g_render_ctx);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "time", std::isnan(t) ? JS_NULL : JS_NewFloat64(ctx, t));
  JS_SetPropertyStr(ctx, o, "triangles",
                    JS_NewInt64(ctx, int64_t(g_render_ctx->tris.size())));
  return o;
}
JSValue js_bounds(JSContext *ctx, JSValueConst, int, JSValueConst *, int,
                  JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  RenderContext &rc = *g_render_ctx;
  const Vec3 center = Mul(Add(rc.bounds.lo, rc.bounds.hi), 0.5f);
  const float radius = Length(Sub(rc.bounds.hi, rc.bounds.lo)) * 0.5f;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "lo", Vec3JS(ctx, rc.bounds.lo));
  JS_SetPropertyStr(ctx, o, "hi", Vec3JS(ctx, rc.bounds.hi));
  JS_SetPropertyStr(ctx, o, "center", Vec3JS(ctx, center));
  JS_SetPropertyStr(ctx, o, "radius", JS_NewFloat64(ctx, radius));
  return o;
}
JSValue js_stats(JSContext *ctx, JSValueConst, int, JSValueConst *, int,
                 JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  RenderContext &rc = *g_render_ctx;
  lrt_tri_stats st;
  std::memset(&st, 0, sizeof(st));
  lrt_tri_scene_stats(rc.scene, &st);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "triangles", JS_NewInt64(ctx, int64_t(rc.tris.size())));
  JS_SetPropertyStr(ctx, o, "meshes", JS_NewInt64(ctx, int64_t(rc.stats.meshes)));
  JS_SetPropertyStr(ctx, o, "bvhNodes", JS_NewInt64(ctx, int64_t(st.node_count)));
  JS_SetPropertyStr(ctx, o, "bvhBytes", JS_NewInt64(ctx, int64_t(st.memory_bytes)));
  JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, rc.width));
  JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, rc.height));
  return o;
}
JSValue js_render(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv,
                  int, JSValueConst *) {
  if (!g_render_ctx) return JS_ThrowTypeError(ctx, "no render context");
  if (argc < 1 || !JS_IsString(argv[0]))
    return JS_ThrowTypeError(ctx, "render(path) requires a path string");
  const char *p = JS_ToCString(ctx, argv[0]);
  const std::string path = p ? p : "";
  if (p) JS_FreeCString(ctx, p);
  const double secs = RenderFrameTo(*g_render_ctx, path);
  if (secs < 0.0) return JS_ThrowInternalError(ctx, "failed to write image");
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "path", JS_NewString(ctx, path.c_str()));
  JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, g_render_ctx->width));
  JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, g_render_ctx->height));
  JS_SetPropertyStr(ctx, o, "seconds", JS_NewFloat64(ctx, secs));
  return o;
}

void RegFn(JSContext *ctx, JSValue obj, const char *name, JSCFunctionData *fn,
           int len) {
  JS_SetPropertyStr(ctx, obj, name,
                    JS_NewCFunctionData(ctx, fn, len, 0, 0, nullptr));
}

void RegisterLusdrenderModule(lightusd::tydra::JSEngineState &engine,
                              RenderContext *rc) {
  JSContext *ctx = static_cast<JSContext *>(engine.context);
  g_render_ctx = rc;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue mod = JS_NewObject(ctx);
  RegFn(ctx, mod, "setCamera", js_setCamera, 10);
  RegFn(ctx, mod, "orbit", js_orbit, 3);
  RegFn(ctx, mod, "setResolution", js_setResolution, 2);
  RegFn(ctx, mod, "setAmbient", js_setAmbient, 1);
  RegFn(ctx, mod, "setBackground", js_setBackground, 3);
  RegFn(ctx, mod, "setShadows", js_setShadows, 1);
  RegFn(ctx, mod, "setSamples", js_setSamples, 1);
  RegFn(ctx, mod, "autoframe", js_autoframe, 0);
  RegFn(ctx, mod, "setTime", js_setTime, 1);
  RegFn(ctx, mod, "bounds", js_bounds, 0);
  RegFn(ctx, mod, "stats", js_stats, 0);
  RegFn(ctx, mod, "render", js_render, 1);
  JS_SetPropertyStr(ctx, global, "lusdrender", mod);
  JS_FreeValue(ctx, global);
}

std::string ReadFile(const std::string &path, bool *ok) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) { *ok = false; return {}; }
  std::stringstream ss;
  ss << ifs.rdbuf();
  *ok = true;
  return ss.str();
}

}  // namespace qjs

int RunJSScriptMode(const Options &opt, const std::string &script_path) {
  RenderContext ctx;
  if (!BuildRenderContext(opt, ctx)) return EXIT_FAILURE;
  if (opt.stats) PrintRTStats(ctx);

  bool ok_read = false;
  const std::string code = qjs::ReadFile(script_path, &ok_read);
  if (!ok_read) {
    std::cerr << "Cannot open script: " << script_path << "\n";
    return EXIT_FAILURE;
  }

  lightusd::tydra::JSEngineState engine;
  std::string err;
  if (!lightusd::tydra::InitJSEngine(engine, err)) {
    std::cerr << "JS engine init failed: " << err << "\n";
    return EXIT_FAILURE;
  }
  qjs::RegisterLusdrenderModule(engine, &ctx);

  nlohmann::json ret;
  const bool ok = lightusd::tydra::RunJSScript(engine, code, ret, err);
  if (!ok) {
    std::cerr << "JS error: " << err << "\n";
  } else if (!ret.is_null()) {
    std::cerr << "JS result: " << ret.dump() << "\n";
  }
  lightusd::tydra::DestroyJSEngine(engine);
  qjs::g_render_ctx = nullptr;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Build a lusdrender.* JS expression from an MCP tool call and evaluate it
// against the persistent engine, returning the JSON result.
bool McpCallTool(lightusd::tydra::JSEngineState &engine, const std::string &name,
                 const nlohmann::json &args, nlohmann::json &out,
                 std::string &err) {
  auto num = [&](const char *k, double d) -> double {
    return args.contains(k) && args[k].is_number() ? args[k].get<double>() : d;
  };
  auto arr3 = [&](const char *k, double a, double b, double c, double v[3]) {
    v[0] = a; v[1] = b; v[2] = c;
    if (args.contains(k) && args[k].is_array() && args[k].size() == 3) {
      for (int i = 0; i < 3; ++i)
        if (args[k][i].is_number()) v[i] = args[k][i].get<double>();
    }
  };
  std::string code;
  char buf[512];
  if (name == "eval") {
    if (!args.contains("script") || !args["script"].is_string()) {
      err = "eval requires a 'script' string";
      return false;
    }
    code = args["script"].get<std::string>();
  } else if (name == "set_camera") {
    double e[3], t[3];
    arr3("eye", 0, 0, 0, e);
    arr3("target", 0, 0, 0, t);
    const double fov = num("fov", 0.0);
    std::snprintf(buf, sizeof(buf),
                  "lusdrender.setCamera(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g%s)", e[0],
                  e[1], e[2], t[0], t[1], t[2],
                  fov > 0.0 ? (std::string(",") + std::to_string(fov)).c_str()
                            : "");
    code = buf;
  } else if (name == "orbit") {
    std::snprintf(buf, sizeof(buf), "lusdrender.orbit(%.9g,%.9g,%.9g)",
                  num("azimuth", 0.0), num("elevation", 20.0),
                  num("distance", 2.5));
    code = buf;
  } else if (name == "set_time") {
    if (args.contains("time") && args["time"].is_number()) {
      std::snprintf(buf, sizeof(buf), "lusdrender.setTime(%.9g)",
                    args["time"].get<double>());
      code = buf;
    } else {
      code = "lusdrender.setTime()";  // default time
    }
  } else if (name == "set_resolution") {
    std::snprintf(buf, sizeof(buf), "lusdrender.setResolution(%d,%d)",
                  int(num("width", 960)), int(num("height", 540)));
    code = buf;
  } else if (name == "render") {
    const std::string path =
        args.contains("path") && args["path"].is_string()
            ? args["path"].get<std::string>()
            : "out.png";
    code = "lusdrender.render(" + nlohmann::json(path).dump() + ")";
  } else if (name == "bounds") {
    code = "lusdrender.bounds()";
  } else if (name == "stats") {
    code = "lusdrender.stats()";
  } else {
    err = "unknown tool: " + name;
    return false;
  }
  if (!lightusd::tydra::RunJSScript(engine, code, out, err)) {
    return false;  // err set
  }
  return true;
}

nlohmann::json McpToolList() {
  auto tool = [](const char *n, const char *desc, nlohmann::json props,
                 std::vector<std::string> req) {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = props;
    if (!req.empty()) schema["required"] = req;
    return nlohmann::json{{"name", n}, {"description", desc},
                          {"inputSchema", schema}};
  };
  auto vec3 = []() {
    return nlohmann::json{{"type", "array"},
                          {"items", {{"type", "number"}}},
                          {"minItems", 3},
                          {"maxItems", 3}};
  };
  auto num = []() { return nlohmann::json{{"type", "number"}}; };
  nlohmann::json tools = nlohmann::json::array();
  tools.push_back(tool("eval",
                       "Run JavaScript against the persistent render context "
                       "(lusdrender.* API). Returns the script value.",
                       {{"script", {{"type", "string"}}}}, {"script"}));
  tools.push_back(tool("set_camera",
                       "Position the camera with eye+target (+optional fov deg).",
                       {{"eye", vec3()}, {"target", vec3()}, {"fov", num()}},
                       {"eye", "target"}));
  tools.push_back(tool("orbit",
                       "Orbit the camera around the scene center.",
                       {{"azimuth", num()}, {"elevation", num()},
                        {"distance", num()}},
                       {}));
  tools.push_back(tool("set_time",
                       "Re-evaluate geometry/transforms at a time code (omit "
                       "for the default time) and rebuild the BVH.",
                       {{"time", num()}}, {}));
  tools.push_back(tool("set_resolution", "Set output image resolution.",
                       {{"width", num()}, {"height", num()}}, {}));
  tools.push_back(tool("render",
                       "Render the current camera to a PNG path (reuses the BVH).",
                       {{"path", {{"type", "string"}}}}, {"path"}));
  tools.push_back(tool("bounds", "Get the scene bounding box.",
                       nlohmann::json::object(), {}));
  tools.push_back(tool("stats", "Get triangle/BVH/resolution stats.",
                       nlohmann::json::object(), {}));
  return tools;
}

int RunMCPMode(const Options &opt) {
  RenderContext ctx;
  if (!BuildRenderContext(opt, ctx)) return EXIT_FAILURE;
  if (opt.stats) PrintRTStats(ctx);

  lightusd::tydra::JSEngineState engine;
  std::string err;
  if (!lightusd::tydra::InitJSEngine(engine, err)) {
    std::cerr << "JS engine init failed: " << err << "\n";
    return EXIT_FAILURE;
  }
  qjs::RegisterLusdrenderModule(engine, &ctx);
  std::cerr << "lusdrender MCP server ready (stdio JSON-RPC). Tools: eval, "
               "set_camera, orbit, set_resolution, render, bounds, stats.\n";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    nlohmann::json req;
    try {
      req = nlohmann::json::parse(line);
    } catch (...) {
      continue;
    }
    const std::string method = req.value("method", std::string());
    const bool has_id = req.contains("id");
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    if (has_id) resp["id"] = req["id"];
    auto send = [&]() {
      std::cout << resp.dump() << "\n";
      std::cout.flush();
    };

    if (method == "initialize") {
      resp["result"] = {
          {"protocolVersion", "2024-11-05"},
          {"serverInfo", {{"name", "lusdrender"}, {"version", "1.0"}}},
          {"capabilities", {{"tools", nlohmann::json::object()}}}};
      send();
    } else if (method == "tools/list") {
      resp["result"] = {{"tools", McpToolList()}};
      send();
    } else if (method == "tools/call") {
      const auto params = req.value("params", nlohmann::json::object());
      const std::string tname = params.value("name", std::string());
      const auto targs = params.value("arguments", nlohmann::json::object());
      nlohmann::json out;
      std::string e2;
      const bool ok = McpCallTool(engine, tname, targs, out, e2);
      if (!ok) {
        resp["result"] = {
            {"isError", true},
            {"content", nlohmann::json::array(
                            {{{"type", "text"}, {"text", e2}}})}};
      } else {
        resp["result"] = {
            {"content", nlohmann::json::array(
                            {{{"type", "text"}, {"text", out.dump()}}})}};
      }
      send();
    } else if (has_id) {
      resp["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
      send();
    }
    // notifications (no id) other than the above are ignored.
  }

  lightusd::tydra::DestroyJSEngine(engine);
  qjs::g_render_ctx = nullptr;
  return EXIT_SUCCESS;
}

}  // namespace lusdr
#endif  // LIGHTUSD_WITH_QJS
