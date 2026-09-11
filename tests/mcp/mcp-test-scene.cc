#define TEST_NO_MAIN
#include "acutest.h"

#include <string>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "tydra/mcp-context.hh"
#include "tydra/mcp-tools-scene.hh"
#include "tydra/js-script.hh"

using namespace lightusd::tydra::mcp;
using json = nlohmann::json;

// Helper: create a fresh context with an empty stage
static Context make_stage() {
  Context ctx;
  json result;
  std::string err;
  StageNew(ctx, json::object(), result, err);
  return ctx;
}

// ---------------------------------------------------------------------------
// prim_create (root level)
// ---------------------------------------------------------------------------
void mcp_prim_create_root_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create Xform
  args["path"] = "/Root";
  args["type_name"] = "Xform";
  TEST_CHECK(PrimCreate(ctx, args, result, err));
  TEST_CHECK(result.contains("success"));
  TEST_CHECK(result["success"] == true);
  TEST_CHECK(result["path"] == "/Root");

  // Create Sphere
  args["path"] = "/World";
  args["type_name"] = "Sphere";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // Create Mesh
  args["path"] = "/MyMesh";
  args["type_name"] = "Mesh";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // Verify via stage_info
  result = json::object();
  TEST_CHECK(StageInfo(ctx, json::object(), result, err));
  TEST_CHECK(result["rootPrimCount"] == 3);

  // Test error cases
  args["path"] = "";
  result = json::object();
  TEST_CHECK(!PrimCreate(ctx, args, result, err));

  // Unknown type
  args["path"] = "/Bad";
  args["type_name"] = "NonExistentType";
  result = json::object();
  TEST_CHECK(!PrimCreate(ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// prim_list (root and subtree)
// ---------------------------------------------------------------------------
void mcp_prim_list_root_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create some prims
  args["path"] = "/A";
  args["type_name"] = "Xform";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  args["path"] = "/B";
  args["type_name"] = "Cube";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  args["path"] = "/C";
  args["type_name"] = "Sphere";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // List root prims
  result = json::object();
  TEST_CHECK(PrimList(ctx, json::object(), result, err));
  TEST_CHECK(result.contains("prims"));
  TEST_CHECK(result["prims"].size() == 3);
  TEST_CHECK(result["count"] == 3);

  // Verify names
  bool found_a = false, found_b = false, found_c = false;
  for (const auto &p : result["prims"]) {
    std::string name = p["name"].get<std::string>();
    if (name == "A") found_a = true;
    if (name == "B") found_b = true;
    if (name == "C") found_c = true;
    TEST_CHECK(p.contains("type"));
  }
  TEST_CHECK(found_a && found_b && found_c);

  // Test prim_list with path
  result = json::object();
  args = {{"path", "/A"}};
  TEST_CHECK(PrimList(ctx, args, result, err));
  TEST_CHECK(result.contains("prim"));

  // Test error with non-existent path
  result = json::object();
  args = {{"path", "/NonExistent"}};
  TEST_CHECK(PrimList(ctx, args, result, err)); // returns true with error field
  TEST_CHECK(result.contains("error"));
}

// ---------------------------------------------------------------------------
// prim_get
// ---------------------------------------------------------------------------
void mcp_prim_get_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create a Mesh prim
  args["path"] = "/MyMesh";
  args["type_name"] = "Mesh";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // Get prim details
  result = json::object();
  args = {{"path", "/MyMesh"}, {"include_attributes", true}};
  TEST_CHECK(PrimGet(ctx, args, result, err));

  TEST_CHECK(result.contains("path"));
  TEST_CHECK(result["path"] == "/MyMesh");
  TEST_CHECK(result.contains("prim"));
  TEST_CHECK(result["prim"].contains("type"));
  TEST_CHECK(result["prim"]["type"] == "Mesh");
  TEST_CHECK(result["prim"].contains("specifier"));
  TEST_CHECK(result["prim"]["specifier"] == "def");
  TEST_CHECK(result["prim"].contains("name"));
  TEST_CHECK(result["prim"]["name"] == "MyMesh");

  // Test error: non-existent path
  result = json::object();
  args = {{"path", "/NonExistent"}};
  TEST_CHECK(!PrimGet(ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// prim_rename
// ---------------------------------------------------------------------------
void mcp_prim_rename_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create a prim
  args["path"] = "/OldName";
  args["type_name"] = "Xform";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // Rename it through the Stage namespace-edit API.
  result = json::object();
  args = {{"path", "/OldName"}, {"new_name", "NewName"}};
  TEST_CHECK(PrimRename(ctx, args, result, err));
  TEST_CHECK(result["path"] == "/NewName");

  // The old path is gone and the new path resolves.
  result = json::object();
  args = {{"path", "/OldName"}};
  TEST_CHECK(!PrimGet(ctx, args, result, err));
  result = json::object();
  args = {{"path", "/NewName"}};
  TEST_CHECK(PrimGet(ctx, args, result, err));

  // Test error: missing new_name
  result = json::object();
  args = {{"path", "/NewName"}};
  TEST_CHECK(!PrimRename(ctx, args, result, err));

  // Test error: non-existent prim
  result = json::object();
  args = {{"path", "/NonExistent"}, {"new_name", "X"}};
  TEST_CHECK(!PrimRename(ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// prim_remove
// ---------------------------------------------------------------------------
void mcp_prim_remove_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create prims
  args["path"] = "/Keep";
  args["type_name"] = "Xform";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  args["path"] = "/Remove";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  TEST_CHECK(ctx.stage->root_prims().size() == 2);

  // Remove one
  result = json::object();
  args = {{"path", "/Remove"}};
  TEST_CHECK(PrimRemove(ctx, args, result, err));
  TEST_CHECK(result.contains("success"));

  TEST_CHECK(ctx.stage->root_prims().size() == 1);

  // Verify removed prim can't be found
  result = json::object();
  args = {{"path", "/Remove"}};
  TEST_CHECK(!PrimGet(ctx, args, result, err));

  // Test error: remove non-existent
  result = json::object();
  args = {{"path", "/NonExistent"}};
  TEST_CHECK(!PrimRemove(ctx, args, result, err));

  // Test error: no stage
  Context empty_ctx;
  result = json::object();
  TEST_CHECK(!PrimRemove(empty_ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// prim_nested (hierarchy creation and listing)
// ---------------------------------------------------------------------------
void mcp_prim_nested_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create root prims
  args["path"] = "/Parent";
  args["type_name"] = "Xform";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  args["path"] = "/Parent/Child";
  args["type_name"] = "Sphere";
  result = json::object();
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  result = json::object();
  args = {{"path", "/Parent/Child"}};
  TEST_CHECK(PrimGet(ctx, args, result, err));

  result = json::object();
  args = {{"path", "/Parent/Child"}, {"new_name", "Renamed"}};
  TEST_CHECK(PrimRename(ctx, args, result, err));
  TEST_CHECK(result["path"] == "/Parent/Renamed");

  result = json::object();
  args = {{"path", "/Parent/Renamed"}};
  TEST_CHECK(PrimRemove(ctx, args, result, err));
}

// ---------------------------------------------------------------------------
// attr_list
// ---------------------------------------------------------------------------
void mcp_attr_list_test(void) {
  Context ctx = make_stage();
  json args, result;
  std::string err;

  // Create a prim with built-in attributes
  args["path"] = "/MyMesh";
  args["type_name"] = "Mesh";
  TEST_CHECK(PrimCreate(ctx, args, result, err));

  // List attributes
  result = json::object();
  args = {{"path", "/MyMesh"}};
  TEST_CHECK(AttrList(ctx, args, result, err));

  // Mesh has built-in attributes like points, extent, etc.
  TEST_CHECK(result.contains("attributes"));
  TEST_CHECK(result["attributes"].is_array());

  // Test error: non-existent prim
  result = json::object();
  args = {{"path", "/NonExistent"}};
  TEST_CHECK(!AttrList(ctx, args, result, err));
}
