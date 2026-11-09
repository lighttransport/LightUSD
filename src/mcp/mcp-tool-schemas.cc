// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
#include "mcp-tool-schemas.hh"

#include <string>

namespace lightusd {
namespace mcp_schema {

void AppendToolSchemas(nlohmann::json &result) {
  result["tools"] = nlohmann::json::array();

  // Helper to add a tool definition
  auto add_tool = [&](const std::string &name, const std::string &desc,
                       nlohmann::json schema) {
    nlohmann::json j;
    j["name"] = name;
    j["description"] = desc;
    j["inputSchema"] = schema;
    result["tools"].push_back(j);
  };

  auto str_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "string"}, {"description", desc}};
  };
  auto num_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "number"}, {"description", desc}};
  };
  auto bool_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "boolean"}, {"description", desc}};
  };
  auto int_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "integer"}, {"description", desc}};
  };

  // =========================================================================
  // Utility
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_version", "Get LightUSD MCP server version", schema);
  }

  // =========================================================================
  // Stage tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    schema["properties"]["upAxis"] = str_prop("Up axis (X, Y, or Z)");
    schema["properties"]["defaultPrim"] = str_prop("Default prim name");
    schema["properties"]["metersPerUnit"] = num_prop("Meters per unit");
    add_tool("stage_new", "Create a new empty USD stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("USD file path to load");
    schema["properties"]["options"] = {{"type", "object"},
                                        {"description", "Load options"}};
    schema["required"] = nlohmann::json::array({"uri"});
    add_tool("stage_load", "Load a USD file into the session stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded USD data");
    schema["properties"]["format"] = str_prop("Format hint: auto, usda, usdc");
    schema["required"] = nlohmann::json::array({"data"});
    add_tool("stage_load_data", "Load USD from base64 data", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["format"] =
        str_prop("Export format: usda (default), usdc, usdz");
    add_tool("stage_to_string",
             "Export the current stage to a USDA/USDC string", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("stage_info", "Get stage metadata (upAxis, prims, etc.)", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("Output file path");
    schema["properties"]["format"] = str_prop("Export format: usda, usdc, usdz");
    schema["required"] = nlohmann::json::array({"uri"});
    add_tool("stage_export", "Export stage to a file", schema);
  }

  // =========================================================================
  // Scene graph tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path (default: /)");
    schema["properties"]["max_depth"] =
        int_prop("Max recursion depth (-1 = unlimited)");
    schema["properties"]["include_attributes"] =
        bool_prop("Include attribute definitions");
    add_tool("prim_list",
             "List prims at or under a path in the scene graph", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path (e.g. /World/mesh0)");
    schema["properties"]["include_attributes"] =
        bool_prop("Include attribute details");
    schema["properties"]["include_metadata"] =
        bool_prop("Include prim metadata");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_get", "Get full details of a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path for the new prim");
    schema["properties"]["type_name"] = str_prop("USD prim type (e.g. Xform, Mesh)");
    schema["properties"]["specifier"] =
        str_prop("Specifier: def (default), over, or class");
    schema["required"] = nlohmann::json::array({"path", "type_name"});
    add_tool("prim_create", "Create a new prim in the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path of prim to remove");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_remove", "Remove a prim from the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path of prim to rename");
    schema["properties"]["new_name"] = str_prop("New element name");
    schema["required"] = nlohmann::json::array({"path", "new_name"});
    add_tool("prim_rename", "Rename a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_get_metadata", "Get prim metadata (kind, active, etc.)",
             schema);
  }

  // =========================================================================
  // Attribute tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("attr_list", "List attributes on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name");
    schema["properties"]["time"] = num_prop("Optional time code for sampled attributes");
    schema["required"] = nlohmann::json::array({"path", "attr_name"});
    add_tool("attr_get", "Get an attribute value (with optional time code)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name");
    schema["properties"]["value"] = {
        {"type", "object"},
        {"description",
         "Value as {type: string, value: any} (see structured JSON format)"}};
    schema["required"] = nlohmann::json::array({"path", "attr_name", "value"});
    add_tool("attr_set", "Set an attribute value", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name to block");
    schema["required"] = nlohmann::json::array({"path", "attr_name"});
    add_tool("attr_block", "Block (set to None) an attribute", schema);
  }

  // =========================================================================
  // Composition tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["asset_path"] = str_prop("Referenced USD file path");
    schema["properties"]["prim_path"] =
        str_prop("Prim path within the referenced file");
    schema["properties"]["offset"] = num_prop("Layer offset");
    schema["properties"]["scale"] = num_prop("Layer time scale");
    schema["required"] =
        nlohmann::json::array({"path", "asset_path"});
    add_tool("reference_add", "Add a reference arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("reference_list", "List references on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("reference_clear", "Clear all references on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["asset_path"] = str_prop("Payload USD file path");
    schema["required"] =
        nlohmann::json::array({"path", "asset_path"});
    add_tool("payload_add", "Add a payload arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("payload_list", "List payload arcs on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["prim_path"] = str_prop("Prim path to inherit from");
    schema["required"] = nlohmann::json::array({"path", "prim_path"});
    add_tool("inherit_add", "Add an inherit arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["prim_path"] = str_prop("Prim path to specialize from");
    schema["required"] = nlohmann::json::array({"path", "prim_path"});
    add_tool("specialize_add", "Add a specialize arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("variant_list_sets",
             "List variant sets and their selections on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set"});
    add_tool("variant_get_selection",
             "Get the current variant selection for a variant set", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["properties"]["variant"] = str_prop("Variant name to select");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set", "variant"});
    add_tool("variant_set_selection",
             "Set the variant selection for a variant set", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["properties"]["variant_name"] = str_prop("Variant name to define");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set", "variant_name"});
    add_tool("variant_define", "Define a variant in a variant set", schema);
  }

  // =========================================================================
  // Query tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["type_name"] = str_prop("USD prim type to search for");
    schema["required"] = nlohmann::json::array({"type_name"});
    add_tool("query_prims_by_type",
             "Find all prims of a given type in the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("schema_list_types",
             "List all registered USD prim type names", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["type_name"] = str_prop("USD prim type name");
    schema["required"] = nlohmann::json::array({"type_name"});
    add_tool("schema_get_type",
             "Get the schema definition for a prim type", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["query"] = str_prop("Search query string");
    schema["properties"]["scope"] =
        str_prop("Search scope: names, all (default)");
    schema["required"] = nlohmann::json::array({"query"});
    add_tool("search", "Search prim names across the stage", schema);
  }

  // =========================================================================
  // Validation
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] =
        str_prop("Base64-encoded USD to validate (usda/usdc/usdz)");
    schema["properties"]["uri"] =
        str_prop("USD file path to validate (native builds only)");
    schema["properties"]["layer_uuid"] =
        str_prop("UUID of a previously-loaded session layer to validate");
    schema["properties"]["name"] =
        str_prop("Filename hint when validating `data`");
    schema["properties"]["groups"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description",
         "Rule groups to run: any of \"core\", \"geom\", \"shade\", "
         "\"lux\", \"physics\", \"crate\", or \"all\". Default [\"core\"]."}};
    add_tool("usd_validate",
             "Validate USD against AOUSD Core semantic rules. Validates "
             "`data`/`uri`/`layer_uuid` if given, else the current session "
             "stage. Returns structured issues (severity, rule_id, location, "
             "message).",
             schema);
  }

  // =========================================================================
  // Scripting
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["script"] =
        str_prop("JavaScript code to execute (uses lightusd.* API)");
    schema["required"] = nlohmann::json::array({"script"});
    add_tool("run_script",
             "Execute JavaScript code against the session stage. "
             "Use lightusd.stage, lightusd.prim, lightusd.query etc. "
             "When a diff is loaded, lightusd.diff.{summary,paths,prim} are also "
             "available for efficient ad-hoc diff queries.",
             schema);
  }

  // =========================================================================
  // Diff tools (value-level, ULP-tolerant USD layer diff)
  // =========================================================================
  {
    nlohmann::json side;
    side["type"] = "object";
    side["properties"]["path"] = str_prop("Filesystem path to a USD file");
    side["properties"]["data"] =
        str_prop("base64-encoded USD bytes (alternative to path)");
    side["properties"]["uuid"] =
        str_prop("UUID of an already-loaded layer (alternative to path)");
    side["properties"]["name"] = str_prop("Display name (optional)");

    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["left"] = side;
    schema["properties"]["right"] = side;
    schema["properties"]["ulps"] =
        int_prop("Float ULP tolerance (default 1; 0 = bitwise-exact)");
    schema["properties"]["eps"] =
        num_prop("Absolute float epsilon (optional; OR'd with ULP)");
    schema["properties"]["compareMetadata"] =
        bool_prop("Compare attribute/prim/layer metadata (default true)");
    schema["properties"]["flatten"] = bool_prop(
        "Flatten (compose sublayers/refs/payload/inherits/variants) before "
        "diff (default false)");
    schema["required"] = nlohmann::json::array({"left", "right"});
    add_tool("diff_open",
             "Diff two USD layers (each given by path/data/uuid), optionally "
             "flattened, with ULP-tolerant float compare. Caches the result in "
             "the session and returns a summary (counts + reason tally). Drill "
             "down with diff_paths / diff_prim, or query via run_script "
             "(lightusd.diff.*).",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("diff_summary",
             "Summary (counts + reason tally) of the cached diff.", schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["reason"] = str_prop(
        "Keep only paths whose reasons contain this substring "
        "(e.g. 'value', 'type', 'meta:kind')");
    schema["properties"]["path_substr"] =
        str_prop("Keep only paths containing this substring");
    schema["properties"]["kind"] = str_prop(
        "Filter by change-kind substring: added|deleted|modified|prim|prop");
    schema["properties"]["group_by"] = str_prop(
        "If set, return aggregated {key,count} groups instead of a path list: "
        "reason | property | prim | kind");
    schema["properties"]["offset"] = int_prop("Pagination offset (default 0)");
    schema["properties"]["limit"] =
        int_prop("Max paths to return (default 200)");
    add_tool("diff_paths",
             "Filtered, paginated list of changed paths from the cached diff. "
             "Pass group_by to get aggregated counts instead.",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] =
        str_prop("Subtree root (default \"/\")");
    schema["properties"]["depth"] =
        int_prop("Levels below root to roll up (default 2)");
    add_tool("diff_tree",
             "Subtree rollup of change counts under a path (navigable "
             "overview: which subtrees changed most).",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path, e.g. /Root/Foo");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("diff_prim",
             "Full per-prim diff detail (reasons + old/new values) from the "
             "cached diff.",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("diff_text",
             "Full rendered text diff (token-heavy escape hatch).", schema);
    add_tool("diff_json",
             "Full structured JSON diff (token-heavy escape hatch).", schema);
  }

  // =========================================================================
  // Existing: USD Layer tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_all_usd_descriptions",
             "Get description of all loaded USD Layers", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("get_usd_description",
             "Get description of a loaded USD Layer", schema);
  }

#if !defined(__EMSCRIPTEN__)
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("USD file path");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"uri", "name"});
    add_tool("load_usd_layer_from_file",
             "Load USD as Layer from file (C++ native only)", schema);
  }
#endif

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded USD data");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"data", "name"});
    add_tool("load_usd_layer_from_data",
             "Load USD as Layer from base64 data", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name to load as layer");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("load_usd_layer_from_asset",
             "Load USD as Layer from a stored asset", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("to_usda", "Convert a loaded USD Layer to USDA text", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uuid"] = str_prop("Layer UUID");
    schema["properties"]["name"] = str_prop("Layer name");
    add_tool("list_primspecs",
             "List root PrimSpecs in a loaded USD Layer", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uuid"] = str_prop("Layer UUID");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["properties"]["path"] = str_prop("Absolute PrimSpec path");
    schema["properties"]["max_depth"] =
        int_prop("Maximum child depth to include");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("debug_primspec_dump",
             "Dump a loaded Layer PrimSpec as deterministic JSON for debugging",
             schema);
  }

  // =========================================================================
  // Existing: Asset management tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded asset data");
    schema["properties"]["name"] = str_prop("Asset name");
    schema["properties"]["description"] = str_prop("Optional description");
    schema["required"] = nlohmann::json::array({"data", "name"});
    add_tool("store_asset",
             "Store an asset (USD, texture) with optional preview", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_asset",
             "Read asset data, transform, and metadata", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_asset_preview",
             "Read preview image from an asset", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["include_preview"] =
        bool_prop("Include preview image data");
    add_tool("get_all_asset_descriptions",
             "Get descriptions of all assets", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["properties"]["include_preview"] =
        bool_prop("Include preview image data");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("get_asset_description",
             "Get description of a specific asset", schema);
  }

  // =========================================================================
  // Existing: Scene arrangement tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["assets"] = {
        {"type", "array"},
        {"description",
         "Array of asset objects with name and optional transforms"}};
    schema["required"] = nlohmann::json::array({"assets"});
    add_tool("select_assets",
             "Select and arrange assets with transforms", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_selected_assets",
             "Get currently selected assets", schema);
  }

  // =========================================================================
  // Existing: Screenshot tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded image data");
    schema["properties"]["name"] = str_prop("Screenshot name");
    schema["properties"]["mimeType"] = str_prop("MIME type (image/png, image/jpeg)");
    schema["required"] = nlohmann::json::array({"data", "name", "mimeType"});
    add_tool("save_screenshot", "Save a screenshot image", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("list_screenshots", "List screenshot names", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Screenshot name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_screenshot", "Read a screenshot image", schema);
  }

  // =========================================================================
  // USDZ conversion / texture tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["input"] = str_prop("Input USD(A/C/Z) file path");
    schema["properties"]["output"] = str_prop("Output .usdz file path");
    schema["properties"]["flatten"] = bool_prop("Compose/flatten before writing (default true)");
    schema["properties"]["arkitCompatible"] = bool_prop("Apply ARKit-friendly metadata");
    schema["properties"]["metersPerUnit"] = num_prop("Override metersPerUnit");
    schema["properties"]["upAxis"] = str_prop("Up axis X/Y/Z");
    schema["properties"]["resizeTextures"] = int_prop("Cap texture longest edge (pixels)");
    schema["properties"]["textureFormat"] = str_prop("keep | png | jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    schema["properties"]["jpegQuality"] = int_prop("JPEG quality 1-100");
    schema["properties"]["reencode"] = bool_prop("Re-encode unmodified textures (default true)");
    schema["properties"]["targetTextureBytes"] = num_prop("Total texture byte budget; shrink all textures to fit");
    schema["properties"]["fitStrategy"] = str_prop("Fit lever: size | quality");
    schema["properties"]["fitMinTextureSize"] = int_prop("Min longest edge for size-fit (default 64)");
    schema["properties"]["fitMinQuality"] = int_prop("Min JPEG quality for quality-fit (default 30)");
    schema["required"] = nlohmann::json::array({"input", "output"});
    add_tool("usdz_convert",
             "Convert a USD file to an ARKit-friendly USDZ (flatten + texture "
             "resize/re-encode with fpnge)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("Output .usdz path. If omitted, returns base64 data.");
    schema["properties"]["arkitCompatible"] = bool_prop("Apply ARKit-friendly metadata");
    add_tool("usdz_pack",
             "Pack the current session stage into a USDZ (file or base64)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64-encoded input image");
    schema["properties"]["max_size"] = int_prop("Cap the longest edge (pixels)");
    schema["properties"]["width"] = int_prop("Target width (with height)");
    schema["properties"]["height"] = int_prop("Target height (with width)");
    schema["properties"]["format"] = str_prop("Output format: png (default) or jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    schema["required"] = nlohmann::json::array({"data"});
    add_tool("texture_resize",
             "Resize a base64 image and re-encode (PNG via fpnge)", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["channels"] = int_prop("Output channel count 1-4");
    schema["properties"]["width"] = int_prop("Output width (default: max of inputs)");
    schema["properties"]["height"] = int_prop("Output height (default: max of inputs)");
    schema["properties"]["format"] = str_prop("Output format: png (default) or jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    nlohmann::json chan = {
        {"type", "object"},
        {"description",
         "Channel source: { data: base64, channel: int } or { const: int }"}};
    schema["properties"]["r"] = chan;
    schema["properties"]["g"] = chan;
    schema["properties"]["b"] = chan;
    schema["properties"]["a"] = chan;
    add_tool("texture_repack",
             "Merge channels from base64 images into one image (e.g. R=gloss, "
             "G=roughness)",
             schema);
  }

}

}  // namespace mcp_schema
}  // namespace lightusd
