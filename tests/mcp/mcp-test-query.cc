#define TEST_NO_MAIN
#include "acutest.h"

#include <string>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "tydra/mcp-context.hh"
#include "tydra/mcp-tools-scene.hh"
#include "tydra/mcp-tools-query.hh"
#include "tydra/mcp-tools.hh"
#include "tydra/js-script.hh"

using namespace lightusd::tydra::mcp;
using json = nlohmann::json;

void mcp_tool_list_registration_test(void) {
  Context ctx;
  json result;

  TEST_CHECK(GetToolsList(ctx, result));
  TEST_CHECK(result.contains("tools"));
  TEST_CHECK(result["tools"].is_array());

  bool has_payload_list = false;
  bool has_variant_define = false;
  bool has_attr_connections = false;
  for (const auto &tool : result["tools"]) {
    const std::string name = tool.value("name", std::string{});
    has_payload_list = has_payload_list || name == "payload_list";
    has_variant_define = has_variant_define || name == "variant_define";
    has_attr_connections = has_attr_connections || name == "attr_connections";
  }

  TEST_CHECK(has_payload_list);
  TEST_CHECK(has_variant_define);
  TEST_CHECK(!has_attr_connections);
}

// Helper: create a context with a variety of root-level prims
// (non-root prim creation is not yet supported)
static Context make_stage_with_prims() {
  Context ctx;
  json result;
  std::string err;

  StageNew(ctx, json::object(), result, err);

  auto create = [&](const std::string &path, const std::string &type) {
    json a;
    a["path"] = path;
    a["type_name"] = type;
    result = json::object();
    PrimCreate(ctx, a, result, err);
  };

  create("/Sphere1", "Sphere");
  create("/Sphere2", "Sphere");
  create("/Cube1", "Cube");
  create("/Lights", "Xform");
  create("/DomeLight1", "DomeLight");

  return ctx;
}

// ---------------------------------------------------------------------------
// query_prims_by_type
// ---------------------------------------------------------------------------
void mcp_query_prims_by_type_test(void) {
  Context ctx = make_stage_with_prims();
  json args, result;
  std::string err;

  // Query for Spheres
  args["type_name"] = "Sphere";
  TEST_CHECK(QueryPrimsByType(ctx, args, result, err));
  TEST_CHECK(result.contains("paths"));
  TEST_CHECK(result["paths"].is_array());
  TEST_CHECK(result["paths"].size() == 2);
  TEST_CHECK(result["count"] == 2);

  bool found1 = false, found2 = false;
  for (const auto &p : result["paths"]) {
    std::string s = p.get<std::string>();
    if (s.find("Sphere1") != std::string::npos) found1 = true;
    if (s.find("Sphere2") != std::string::npos) found2 = true;
  }
  TEST_CHECK(found1 && found2);

  // Query for Xform
  args["type_name"] = "Xform";
  result = json::object();
  TEST_CHECK(QueryPrimsByType(ctx, args, result, err));
  TEST_CHECK(result["count"] >= 1);

  // Query for DomeLight
  args["type_name"] = "DomeLight";
  result = json::object();
  TEST_CHECK(QueryPrimsByType(ctx, args, result, err));
  TEST_CHECK(result["count"] == 1);

  // Query for non-existent type
  args["type_name"] = "NonExistent";
  result = json::object();
  TEST_CHECK(QueryPrimsByType(ctx, args, result, err));
  TEST_CHECK(result["count"] == 0);

  // Test error: missing type_name
  result = json::object();
  TEST_CHECK(!QueryPrimsByType(ctx, json::object(), result, err));

  // Test error: no stage
  Context empty_ctx;
  result = json::object();
  args["type_name"] = "Xform";
  TEST_CHECK(!QueryPrimsByType(empty_ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// schema_list_types
// ---------------------------------------------------------------------------
void mcp_schema_list_types_test(void) {
  Context ctx;
  json args = json::object();
  json result;
  std::string err;

  TEST_CHECK(SchemaListTypes(ctx, args, result, err));
  TEST_CHECK(result.contains("types"));
  TEST_CHECK(result["types"].is_array());
  TEST_CHECK(result["count"] > 0);

  // Verify common types are present
  bool has_xform = false, has_mesh = false, has_sphere = false;
  for (const auto &t : result["types"]) {
    std::string s = t.get<std::string>();
    if (s == "Xform") has_xform = true;
    if (s == "Mesh") has_mesh = true;
    if (s == "Sphere") has_sphere = true;
  }
  TEST_CHECK(has_xform);
  TEST_CHECK(has_mesh);
  TEST_CHECK(has_sphere);
}

// ---------------------------------------------------------------------------
// schema_get_type
// ---------------------------------------------------------------------------
void mcp_schema_get_type_test(void) {
  Context ctx;
  json args, result;
  std::string err;

  // Get Mesh schema
  args["type_name"] = "Mesh";
  TEST_CHECK(SchemaGetType(ctx, args, result, err));
  TEST_CHECK(result.contains("type_name"));
  TEST_CHECK(result["type_name"] == "Mesh");
  TEST_CHECK(result.contains("attributes"));
  TEST_CHECK(result["attributes"].is_array());

  // Get Xform schema
  args["type_name"] = "Xform";
  result = json::object();
  TEST_CHECK(SchemaGetType(ctx, args, result, err));
  TEST_CHECK(result["type_name"] == "Xform");

  // Test error: missing type_name
  result = json::object();
  TEST_CHECK(!SchemaGetType(ctx, json::object(), result, err));

  // Test error: non-existent type
  result = json::object();
  args["type_name"] = "NonExistent";
  TEST_CHECK(!SchemaGetType(ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------
void mcp_search_test(void) {
  Context ctx = make_stage_with_prims();
  json args, result;
  std::string err;

  // Search by name
  args["query"] = "Sphere";
  args["scope"] = "names";
  TEST_CHECK(Search(ctx, args, result, err));
  TEST_CHECK(result.contains("byName"));
  TEST_CHECK(result["byName"].is_array());
  TEST_CHECK(result["totalMatches"] >= 2);

  // Search all (name + type)
  args["query"] = "Lights";
  args["scope"] = "all";
  result = json::object();
  TEST_CHECK(Search(ctx, args, result, err));
  TEST_CHECK(result["totalMatches"] >= 1);

  // Test error: missing query
  result = json::object();
  TEST_CHECK(!Search(ctx, json::object(), result, err));

  // Test error: no stage
  Context empty_ctx;
  args["query"] = "test";
  result = json::object();
  TEST_CHECK(!Search(empty_ctx, args, result, err));
}
