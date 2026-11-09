// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// End-to-end coverage of the next MCP server: every tool group through the
// session string API, plus the JSON-RPC transport.
#include <cassert>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>

#include "lightusd-mcp.hh"
#include "mcp-json.hh"
#include "mcp-util.hh"

using lightusd::mcp::Server;
using json = nlohmann::json;

namespace {

json Call(Server& s, const std::string& tool, const json& args = json::object()) {
  const json r = json::parse(s.ToolsCall(tool, args.dump()));
  if (r.value("isError", false)) {
    std::cerr << tool << " -> " << r.dump() << "\n";
  }
  return r;
}

bool IsError(const json& r) { return r.value("isError", false); }

std::string B64(const std::string& s) {
  return lightusd::mcp::EncodeBase64(reinterpret_cast<const uint8_t*>(s.data()),
                                     s.size());
}

std::string Text(const json& r) { return r["content"][0]["text"].get<std::string>(); }

void TestSessionsAndToolList() {
  Server s;
  assert(s.ToolsList().find("invalid session_id") != std::string::npos);
  assert(s.CreateContext("a"));
  assert(!s.CreateContext("a"));
  assert(s.CreateContext("b"));
  assert(s.SelectContext("a"));
  assert(!s.SelectContext("missing"));
  const json list = json::parse(s.ToolsList());
  std::set<std::string> names;
  for (const json& t : list["tools"]) {
    names.insert(t["name"].get<std::string>());
    assert(t.contains("inputSchema"));
  }
  assert(names.size() >= 55);
  for (const char* n : {"stage_new", "attr_set", "diff_open", "usd_validate", "usdz_pack"}) {
    assert(names.count(n));
  }
  for (const char* n : {"run_script", "texture_resize", "texture_repack"}) {
    assert(!names.count(n));
  }
  assert(IsError(Call(s, "run_script", {{"script", "1"}})));
  assert(IsError(Call(s, "no_such_tool")));
  assert(Text(Call(s, "get_version")).size() > 0);
  // Sessions are isolated.
  assert(!IsError(Call(s, "stage_new")));
  assert(s.SelectContext("b"));
  assert(IsError(Call(s, "stage_info")));
}

void TestStageAndAttributes(const std::string& usda_dir) {
  Server s;
  s.CreateContext("t");
  json r = Call(s, "stage_new", {{"upAxis", "Z"}, {"metersPerUnit", 1.0}});
  assert(r["success"] == true);
  r = Call(s, "stage_info");
  assert(r["upAxis"] == "Z" && r["metersPerUnit"] == 1.0);

  assert(!IsError(Call(s, "prim_create", {{"path", "/World"}, {"type_name", "Xform"}})));
  assert(!IsError(Call(s, "prim_create", {{"path", "/World/Mesh"}, {"type_name", "Mesh"}})));
  assert(!IsError(Call(s, "prim_create", {{"path", "/World/Cube"}, {"type_name", "Cube"}})));
  assert(IsError(Call(s, "prim_create", {{"path", "/World/1bad"}, {"type_name", "Xform"}})));

  r = Call(s, "attr_set", {{"path", "/World/Mesh"}, {"attr_name", "points"},
                           {"value", {{"type", "point3f[]"},
                                      {"value", json::array({json::array({0, 0, 0}),
                                                             json::array({1, 0, 0}),
                                                             json::array({0, 1, 0})})}}}});
  assert(r["success"] == true);
  r = Call(s, "attr_get", {{"path", "/World/Mesh"}, {"attr_name", "points"}});
  assert(r["hasValue"] == true);
  assert(r["value"]["type"] == "point3f[]" && r["value"]["count"] == 3);
  assert(r["value"]["value"][1] == json::array({1, 0, 0}));

  assert(!IsError(Call(s, "attr_set", {{"path", "/World/Mesh"}, {"attr_name", "subdivisionScheme"},
                                       {"uniform", true},
                                       {"value", {{"type", "token"}, {"value", "none"}}}})));
  assert(!IsError(Call(s, "attr_set", {{"path", "/World/Mesh"}, {"attr_name", "doubleSided"},
                                       {"value", {{"type", "bool"}, {"value", true}}}})));
  // A string value for a non-string type is a USDA literal.
  assert(!IsError(Call(s, "attr_set",
                       {{"path", "/World"}, {"attr_name", "xformOp:transform"},
                        {"value", {{"type", "matrix4d"},
                                   {"value", "( (1,0,0,0), (0,1,0,0), (0,0,1,0), (2,3,4,1) )"}}}})));
  r = Call(s, "attr_get", {{"path", "/World"}, {"attr_name", "xformOp:transform"}});
  assert(r["value"]["value"][3] == json::array({2, 3, 4, 1}));
  assert(IsError(Call(s, "attr_set", {{"path", "/World/Mesh"}, {"attr_name", "x"},
                                      {"value", {{"type", "float3"}, {"value", "(1, 2"}}}})));

  r = Call(s, "prim_get", {{"path", "/World/Mesh"}});
  bool saw_uniform = false;
  for (const json& a : r["prim"]["attributes"]) {
    if (a["name"] == "subdivisionScheme") saw_uniform = a["variability"] == "Uniform";
  }
  assert(saw_uniform);
  r = Call(s, "attr_list", {{"path", "/World/Mesh"}});
  bool saw_fallback = false;
  for (const json& a : r["attributes"]) saw_fallback |= a.value("schemaFallback", false);
  assert(saw_fallback);

  assert(!IsError(Call(s, "attr_block", {{"path", "/World/Mesh"}, {"attr_name", "doubleSided"}})));
  r = Call(s, "attr_get", {{"path", "/World/Mesh"}, {"attr_name", "doubleSided"}});
  assert(r["isBlocked"] == true);

  r = Call(s, "attr_connections", {{"path", "/World/Mesh"}, {"attr_name", "inputs:x"},
                                   {"add", "/World/Cube.outputs:y"}});
  assert(r["count"] == 1 && r["connections"][0] == "/World/Cube.outputs:y");

  r = Call(s, "prim_rename", {{"path", "/World/Cube"}, {"new_name", "Box"}});
  assert(r["path"] == "/World/Box");
  assert(!IsError(Call(s, "prim_get", {{"path", "/World/Box"}})));
  assert(IsError(Call(s, "prim_get", {{"path", "/World/Cube"}})));
  assert(IsError(Call(s, "prim_rename", {{"path", "/World/Box"}, {"new_name", "Mesh"}})));

  r = Call(s, "prim_list", {{"max_depth", 1}});
  assert(r["count"] == 1 && r["prims"][0]["children"].size() == 2);
  r = Call(s, "prim_list", {{"path", "/Nope"}});
  assert(r.contains("availableRootPrims"));

  // Composition arcs and variants.
  r = Call(s, "reference_add", {{"path", "/World/Mesh"}, {"asset_path", "ref.usda"},
                                {"prim_path", "/Asset"}, {"offset", 5.0}, {"scale", 2.0}});
  assert(r["success"] == true);
  r = Call(s, "reference_list", {{"path", "/World/Mesh"}});
  assert(r["count"] == 1);
  assert(r["references"][0]["asset_path"] == "ref.usda");
  assert(r["references"][0]["prim_path"] == "/Asset");
  assert(r["references"][0]["offset"] == 5.0 && r["references"][0]["scale"] == 2.0);
  assert(!IsError(Call(s, "payload_add", {{"path", "/World/Mesh"}, {"asset_path", "p.usdc"}})));
  assert(Call(s, "payload_list", {{"path", "/World/Mesh"}})["count"] == 1);
  assert(!IsError(Call(s, "inherit_add", {{"path", "/World/Mesh"}, {"prim_path", "/_class"}})));
  assert(!IsError(Call(s, "specialize_add", {{"path", "/World/Mesh"}, {"prim_path", "/_base"}})));
  r = Call(s, "prim_get_metadata", {{"path", "/World/Mesh"}});
  assert(r["metadata"]["inherits"].size() == 1 && r["metadata"]["specializes"].size() == 1);
  assert(!IsError(Call(s, "reference_clear", {{"path", "/World/Mesh"}})));
  assert(Call(s, "reference_list", {{"path", "/World/Mesh"}})["count"] == 0);

  assert(!IsError(Call(s, "variant_define", {{"path", "/World"}, {"variant_set", "shape"},
                                             {"variant_name", "round"}})));
  assert(!IsError(Call(s, "variant_define", {{"path", "/World"}, {"variant_set", "shape"},
                                             {"variant_name", "square"}})));
  assert(!IsError(Call(s, "variant_set_selection", {{"path", "/World"}, {"variant_set", "shape"},
                                                    {"variant", "square"}})));
  r = Call(s, "variant_get_selection", {{"path", "/World"}, {"variant_set", "shape"}});
  assert(r["selection"] == "square");
  r = Call(s, "variant_list_sets", {{"path", "/World"}});
  assert(r["count"] == 1 && r["variantSets"][0]["variants"].size() == 2);

  // Query, schemas, validation.
  r = Call(s, "query_prims_by_type", {{"type_name", "Mesh"}});
  assert(r["count"] == 1 && r["paths"][0] == "/World/Mesh");
  r = Call(s, "search", {{"query", "box"}});
  assert(r["byName"][0] == "/World/Box");
  r = Call(s, "schema_list_types");
  assert(r["count"].get<int>() > 20);
  r = Call(s, "schema_get_type", {{"type_name", "Mesh"}});
  bool saw_fvi = false;
  for (const json& a : r["attributes"]) saw_fvi |= a["name"] == "faceVertexIndices";
  assert(saw_fvi);
  assert(IsError(Call(s, "schema_get_type", {{"type_name", "NoSuchType"}})));
  r = Call(s, "usd_validate");
  assert(r.contains("ok") && r["source"] == "stage");
  r = Call(s, "usd_validate", {{"groups", json::array({"all"})}});
  assert(r["checked_groups"].size() > 1);

  // Serialization round trip through stage_load_data.
  r = Call(s, "stage_to_string");
  const std::string usda = r["usda"].get<std::string>();
  assert(usda.find("def Mesh \"Mesh\"") != std::string::npos);
  assert(usda.find("point3f[] points") != std::string::npos);
  r = Call(s, "stage_to_string", {{"format", "usdc"}});
  assert(r["length"].get<size_t>() > 0);
  r = Call(s, "usdz_pack");
  std::string usdz;
  assert(lightusd::mcp::DecodeBase64(r["data"].get<std::string>(), &usdz, nullptr));
  assert(usdz.compare(0, 2, "PK") == 0);

  Server s2;
  s2.CreateContext("t");
  r = Call(s2, "stage_load_data", {{"data", B64(usda)}, {"format", "usda"}});
  assert(r["success"] == true && r["upAxis"] == "Z");
  r = Call(s2, "attr_get", {{"path", "/World/Mesh"}, {"attr_name", "points"}});
  assert(r["value"]["count"] == 3);

  // File-based load (native builds).
  r = Call(s2, "stage_load", {{"uri", usda_dir + "/xform-resetxformstack-001.usda"}});
  assert(r["success"] == true);
  assert(IsError(Call(s2, "stage_load", {{"uri", usda_dir + "/does-not-exist.usda"}})));
}

void TestLayersAndDiff() {
  Server s;
  s.CreateContext("t");
  const std::string a = "#usda 1.0\ndef Xform \"Root\" {\n  float size = 1\n  def Mesh \"M\" {}\n}\n";
  const std::string b = "#usda 1.0\ndef Xform \"Root\" {\n  float size = 2\n  def Mesh \"N\" {}\n}\n";
  json r = Call(s, "load_usd_layer_from_data", {{"name", "L1"}, {"data", B64(a)}});
  const std::string uuid = Text(r);
  assert(uuid.size() == 36);
  r = Call(s, "load_usd_layer_from_data", {{"name", "L1"}, {"data", B64(a)}});
  assert(Text(r) == uuid);  // same name, same layer slot
  assert(Text(Call(s, "to_usda", {{"name", "L1"}})).find("def Xform \"Root\"") !=
         std::string::npos);
  assert(Text(Call(s, "list_primspecs", {{"name", "L1"}})) == "Root");
  r = Call(s, "debug_primspec_dump", {{"uuid", uuid}, {"path", "/Root"}});
  assert(json::parse(Text(r))["children"].size() == 1);
  // Arc edits reach loaded layers when no stage holds the prim.
  assert(!IsError(Call(s, "reference_add", {{"path", "/Root"}, {"asset_path", "x.usda"}})));
  assert(Call(s, "reference_list", {{"path", "/Root"}})["layer"] == "L1");
  assert(Call(s, "usd_validate", {{"layer_uuid", uuid}})["source"] == "layer_uuid");
  Call(s, "load_usd_layer_from_data",
       {{"name", "L2"}, {"data", B64(b)}, {"description", "second"}});
  assert(Text(Call(s, "get_usd_description", {{"name", "L2"}})) == "second");
  assert(Call(s, "get_all_usd_descriptions")["content"].size() == 2);

  r = Call(s, "diff_open", {{"left", {{"data", B64(a)}, {"name", "a.usda"}}},
                            {"right", {{"data", B64(b)}, {"name", "b.usda"}}}});
  assert(!IsError(r));
  r = Call(s, "diff_summary");
  assert(!IsError(r));
  r = Call(s, "diff_paths");
  assert(!IsError(r));
  r = Call(s, "diff_prim", {{"path", "/Root"}});
  assert(!IsError(r));
  r = Call(s, "diff_tree");
  assert(!IsError(r));
  assert(Call(s, "diff_text")["text"].get<std::string>().find("size") != std::string::npos);
  assert(!IsError(Call(s, "diff_json")));
  assert(!IsError(Call(s, "diff_open", {{"left", {{"uuid", uuid}}},
                                        {"right", {{"data", B64(b)}}},
                                        {"flatten", true}})));
}

void TestAssetsAndResources() {
  Server s;
  s.CreateContext("t");
  json r = Call(s, "store_asset", {{"name", "chair"}, {"data", B64("chair-bytes")},
                                   {"description", "a chair"},
                                   {"preview", {{"data", B64("png")}, {"mimeType", "image/png"}}}});
  assert(!IsError(r));
  assert(!IsError(Call(s, "get_asset_description", {{"name", "chair"}})));
  assert(Call(s, "get_all_asset_descriptions")["content"].size() == 1);
  r = Call(s, "read_asset_preview", {{"name", "chair"}});
  assert(r["content"][0]["type"] == "image");
  assert(IsError(Call(s, "read_asset", {{"name", "chair"}})));  // not selected
  assert(!IsError(Call(s, "select_assets", {{"assets", json::array({{{"name", "chair"}}})}})));
  assert(!IsError(Call(s, "get_selected_assets")));
  assert(!IsError(Call(s, "read_asset", {{"name", "chair"}})));
  assert(!IsError(Call(s, "save_screenshot", {{"name", "shot"}, {"data", B64("img")},
                                              {"mimeType", "image/png"}})));
  assert(!IsError(Call(s, "list_screenshots")));
  assert(!IsError(Call(s, "read_screenshot", {{"name", "shot"}})));
  const json res = json::parse(s.ResourcesList());
  assert(res["resources"].size() == 1 && res["resources"][0]["name"] == "chair");
  assert(json::parse(s.ResourcesRead("chair"))["contents"].size() == 1);
  assert(json::parse(s.ResourcesRead("nope")).value("isError", false));
}

void TestJsonRpc() {
  Server s;
  auto rpc = [&](const json& m) {
    const std::string out = s.HandleJsonRpc(m.dump());
    return out.empty() ? json() : json::parse(out);
  };
  json r = rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                {"params", {{"protocolVersion", "2025-06-18"}}}});
  assert(r["result"]["serverInfo"]["name"] == "lightusd-mcp");
  assert(r["result"]["capabilities"].contains("tools"));
  assert(rpc({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
  r = rpc({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
  assert(r["result"]["tools"].size() >= 55);
  r = rpc({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
           {"params", {{"name", "stage_new"}, {"arguments", json::object()}}}});
  assert(r["result"]["structuredContent"]["success"] == true);
  assert(r["result"]["content"][0]["type"] == "text");
  r = rpc({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
           {"params", {{"name", "prim_get"}, {"arguments", {{"path", "/Missing"}}}}}});
  assert(r["result"]["isError"] == true);
  r = rpc({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "bogus"}});
  assert(r["error"]["code"] == -32601);
  assert(json::parse(s.HandleJsonRpc("{not json"))["error"]["code"] == -32700);
  r = rpc({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "ping"}});
  assert(r["result"].is_object());
  // Untrusted arguments: wrong types become tool errors, never a crash.
  r = rpc({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
           {"params", {{"name", "prim_list"}, {"arguments", {{"max_depth", "deep"}}}}}});
  assert(r["result"]["isError"] == true);
  r = rpc({{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"},
           {"params", {{"name", "store_asset"}, {"arguments", {{"name", 5}, {"data", "aGk="}}}}}});
  assert(r["result"]["isError"] == true);
  r = rpc({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"}, {"params", "oops"}});
  assert(r["error"]["code"] == -32602 && r["id"] == 9);
  assert(json::parse(s.ToolsCall("attr_set", "{\"path\": 1}")).value("isError", false));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string usda_dir = argc > 1 ? argv[1] : "tests/usda";
  TestSessionsAndToolList();
  TestStageAndAttributes(usda_dir);
  TestLayersAndDiff();
  TestAssetsAndResources();
  TestJsonRpc();
  std::printf("next MCP server tests: PASSED\n");
  return 0;
}
