#define TEST_NO_MAIN
#include "acutest.h"

#include <cstring>
#include <iostream>
#include <string>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "tydra/mcp-context.hh"
#include "tydra/mcp-tools-scene.hh"
#include "lightusd.hh"
#include "tydra/js-script.hh"
#include "pprint-enum.hh"

using namespace lightusd::legacy::tydra::mcp;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// stage_new (default)
// ---------------------------------------------------------------------------
void mcp_stage_new_default_test(void) {
  Context ctx;
  json args = json::object();
  json result;
  std::string err;

  TEST_CHECK(StageNew(ctx, args, result, err));
  TEST_CHECK(ctx.stage != nullptr);
  TEST_CHECK(ctx.stage_loaded == true);

  // Verify defaults (Y up per USD default)
  const auto &m = ctx.stage->metas();
  TEST_CHECK(lightusd::legacy::to_string(m.upAxis.get_value()) == "Y");
  TEST_CHECK(m.defaultPrim.str() == "");
}

// ---------------------------------------------------------------------------
// stage_new (with metadata)
// ---------------------------------------------------------------------------
void mcp_stage_new_with_metadata_test(void) {
  Context ctx;
  json args;
  args["upAxis"] = "Y";
  args["defaultPrim"] = "World";
  args["metersPerUnit"] = 0.01;
  args["timeCodesPerSecond"] = 24.0;
  args["framesPerSecond"] = 24.0;
  args["startTimeCode"] = 0.0;
  args["endTimeCode"] = 100.0;
  json result;
  std::string err;

  TEST_CHECK(StageNew(ctx, args, result, err));
  TEST_CHECK(ctx.stage != nullptr);

  const auto &m = ctx.stage->metas();
  TEST_CHECK(lightusd::legacy::to_string(m.upAxis.get_value()) == "Y");
  TEST_CHECK(m.defaultPrim.str() == "World");
  TEST_CHECK(m.metersPerUnit.get_value() == 0.01);
  TEST_CHECK(m.timeCodesPerSecond.get_value() == 24.0);
  TEST_CHECK(m.framesPerSecond.get_value() == 24.0);
  TEST_CHECK(m.startTimeCode.get_value() == 0.0);
  TEST_CHECK(m.endTimeCode.get_value() == 100.0);
}

// ---------------------------------------------------------------------------
// stage_info
// ---------------------------------------------------------------------------
void mcp_stage_info_test(void) {
  Context ctx;
  json args;
  args["upAxis"] = "Z";
  args["defaultPrim"] = "Root";
  json result;
  std::string err;

  TEST_CHECK(StageNew(ctx, args, result, err));

  result = json::object();
  TEST_CHECK(StageInfo(ctx, json::object(), result, err));

  TEST_CHECK(result.contains("upAxis"));
  TEST_CHECK(result["upAxis"] == "Z");
  TEST_CHECK(result.contains("defaultPrim"));
  TEST_CHECK(result["defaultPrim"] == "Root");
  TEST_CHECK(result.contains("rootPrimCount"));
  TEST_CHECK(result["rootPrimCount"] == 0);
  TEST_CHECK(result.contains("totalPrimCount"));
  TEST_CHECK(result["totalPrimCount"] == 0);

  // Test error case: no stage loaded
  Context empty_ctx;
  result = json::object();
  TEST_CHECK(!StageInfo(empty_ctx, json::object(), result, err));
  TEST_CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// stage_load (from embedded USDA string via LoadUSDAFromMemory)
// ---------------------------------------------------------------------------
void mcp_stage_load_usda_string_test(void) {
  Context ctx;

  // Load a simple USDA file to get a stage
  const char *usda = R"(#usda 1.0
def Xform "hello"
{
  def Sphere "world"
  {
    float3[] extent = [(-10, -10, -10), (10, 10, 10)]
  }
}
)";

  auto stage = std::unique_ptr<lightusd::legacy::Stage>(new lightusd::legacy::Stage());
  std::string warn, load_err;
  bool ok = lightusd::legacy::LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", stage.get(), &warn, &load_err);
  TEST_CHECK(ok);

  ctx.stage = std::move(stage);
  ctx.stage_loaded = true;

  // Verify via stage_info
  json result;
  std::string err;
  TEST_CHECK(StageInfo(ctx, json::object(), result, err));
  TEST_CHECK(result["rootPrimCount"] == 1);

  // Verify prim list
  result = json::object();
  TEST_CHECK(PrimList(ctx, json::object(), result, err));
  TEST_CHECK(result.contains("prims"));
  TEST_CHECK(result["prims"].size() == 1);
  TEST_CHECK(result["prims"][0].contains("name"));
  TEST_CHECK(result["prims"][0]["name"] == "hello");
}

// ---------------------------------------------------------------------------
// stage_to_string
// ---------------------------------------------------------------------------
void mcp_stage_to_string_test(void) {
  Context ctx;
  json args, result;
  std::string err;

  // Create a stage with a prim
  TEST_CHECK(StageNew(ctx, json::object(), result, err));

  args["path"] = "/World";
  args["type_name"] = "Xform";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // Export to string
  result = json::object();
  TEST_CHECK(StageToString(ctx, json::object(), result, err));
  TEST_CHECK(result.contains("usda"));
  TEST_CHECK(result["usda"].is_string());
  std::string usda_str = result["usda"].get<std::string>();
  TEST_CHECK(usda_str.find("World") != std::string::npos);

  // Test error: no stage
  Context empty_ctx;
  result = json::object();
  TEST_CHECK(!StageToString(empty_ctx, json::object(), result, err));
}
