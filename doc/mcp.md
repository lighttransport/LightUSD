# MCP (Model Context Protocol) Interface

LightUSD provides an MCP server for AI agents (Claude, GPT, etc.) to interact with USD data. The server exposes USD scene graph editing, composition arcs, querying, and scripting tools.

## Architecture

```
┌─ MCP Session ──────────────────────────────────────┐
│                                                      │
│   Context                                            │
│   ├── Stage (composed, for scene graph queries)      │
│   ├── Layers (uncomposed, for composition editing)   │
│   ├── Assets (for USDZ packaging & viewer)           │
│   ├── QuickJS Engine (persistent, per session)       │
│   └── Screenshots (for viewer workflow)              │
│                                                      │
│   Tools Layer                                        │
│   ├── Stage Tools      (load, create, export, info)  │
│   ├── Scene Graph      (prim list/get/create/remove) │
│   ├── Attributes       (list, get, set, block)       │
│   ├── Composition      (refs, payloads, variants)    │
│   ├── Query/Discover   (find by type, schema list)   │
│   ├── Validation       (usd_validate: AOUSD Core)    │
│   ├── Scripting        (run_script: JS + lightusd.*) │
│   └── Legacy           (assets, screenshots)         │
│                                                      │
└──────────────────────────────────────────────────────┘
```

## Server

### C++ Native Server

A C++ MCP server using CivetWeb HTTP library. JSON-RPC 2.0 over HTTP POST on `/mcp` endpoint.

```bash
# Build with MCP server
cmake .. -DLIGHTUSD_WITH_MCP_SERVER=ON
make -j16

# Run the example MCP server (source: examples/mcp_server/example-mcp-server.cc)
./build/mcp_server            # optional: --port <N>
```

Default port: 8085. Body-size limits are configured via `MCPServerOptions`.

### JS/WASM Server

An Express-based MCP server bridging to LightUSD WASM. See `web/mcp-server/`.

## Tools

### Stage Lifecycle

| Tool | Description | Key Args |
|------|-------------|----------|
| `stage_new` | Create new empty stage | `upAxis`, `defaultPrim`, `metersPerUnit` |
| `stage_load` | Load USD file into session | `uri`, `options` |
| `stage_load_data` | Load USD from base64 data | `data`, `format` |
| `stage_export` | Export stage to file | `uri`, `format` |
| `stage_to_string` | Export stage to USDA string | `format` |
| `stage_info` | Get stage metadata | — |
| `get_version` | Get LightUSD MCP server version | — |

### Scene Graph

| Tool | Description | Key Args |
|------|-------------|----------|
| `prim_list` | List prims at/under path | `path`, `max_depth`, `include_attributes` |
| `prim_get` | Get full prim details | `path`, `include_attributes`, `include_metadata` |
| `prim_create` | Create a new prim | `path`, `type_name`, `specifier` |
| `prim_remove` | Remove a prim | `path` |
| `prim_rename` | Rename a prim | `path`, `new_name` |
| `prim_get_metadata` | Get prim metadata | `path` |

### Attributes

| Tool | Description | Key Args |
|------|-------------|----------|
| `attr_list` | List attributes on a prim | `path` |
| `attr_get` | Get attribute value | `path`, `attr_name`, `time` |
| `attr_set` | Set attribute value | `path`, `attr_name`, `value` |
| `attr_block` | Block (set to None) an attribute | `path`, `attr_name` |
| `attr_connections` | Get/set connections | `path`, `attr_name`, `set` |

### Composition Arcs

| Tool | Description | Key Args |
|------|-------------|----------|
| `reference_add` | Add reference arc | `path`, `asset_path`, `prim_path` |
| `reference_list` | List references | `path` |
| `reference_clear` | Clear all references | `path` |
| `payload_add` | Add payload arc | `path`, `asset_path` |
| `payload_list` | List payloads | `path` |
| `inherit_add` | Add inherit arc | `path`, `prim_path` |
| `specialize_add` | Add specialize arc | `path`, `prim_path` |
| `variant_list_sets` | List variant sets | `path` |
| `variant_define` | Define a variant set / variant on a prim | `path`, `variant_set`, `variant_name` |
| `variant_get_selection` | Get variant selection | `path`, `variant_set` |
| `variant_set_selection` | Set variant selection | `path`, `variant_set`, `variant` |

### Query / Discovery

| Tool | Description | Key Args |
|------|-------------|----------|
| `query_prims_by_type` | Find prims by type | `type_name` |
| `schema_list_types` | List all registered prim types | — |
| `schema_get_type` | Get schema for a type | `type_name` |
| `search` | Search prim names | `query`, `scope` |

### Validation

| Tool | Description | Key Args |
|------|-------------|----------|
| `usd_validate` | Validate against AOUSD Core semantic rules | `data`, `uri`, `layer_uuid`, `groups`, `name` |

Validates the first input that is present (`data` base64 → `uri` file path →
`layer_uuid` session layer), otherwise the current session stage (serialized to
USDA and re-parsed as an uncomposed Layer). `groups` selects rule groups: any of
`"core"` (default), `"geom"`, `"shade"`, or `"all"`. Returns
`{ ok, error_count, warning_count, spec_version, source, checked_groups: [...],
issues: [{ severity, rule_id, location, message }] }`. `checked_groups` reports
which groups actually ran, so a core-only `ok` is not mistaken for full coverage.

### Diff (USD layer comparison)

| Tool | Description | Key Args |
|------|-------------|----------|
| `diff_open` | Diff two USD layers (path/data/uuid), optionally flattened, ULP-tolerant float compare; caches the result in the session and returns a summary | `left`, `right`, `flatten` |
| `diff_summary` | Summary (counts + reason tally) of the cached diff | — |
| `diff_paths` | Filtered, paginated list of changed paths from the cached diff; `group_by` returns aggregated `{key,count}` groups | `reason`, `path_substr`, `kind`, `group_by`, `offset`, `limit` |
| `diff_tree` | Subtree rollup of change counts under a path (navigable overview) | `path`, `depth` |
| `diff_prim` | Full per-prim diff detail (reasons + old/new values) from the cached diff | `path` |
| `diff_text` | Full rendered text diff (token-heavy escape hatch) | — |
| `diff_json` | Full structured JSON diff (token-heavy escape hatch) | — |

### USDZ / Texture

| Tool | Description | Key Args |
|------|-------------|----------|
| `usdz_convert` | Convert a USD file to an ARKit-friendly USDZ (flatten + texture resize/re-encode with fpnge) | `input`, `output`, `fitMinTextureSize`, `fitMinQuality` |
| `usdz_pack` | Pack the current session stage into a USDZ (file or base64) | `uri`, `arkitCompatible` |
| `texture_resize` | Resize a base64 image and re-encode (PNG via fpnge) | `data`, `format`, `pngEncoder` |
| `texture_repack` | Merge channels from base64 images into one image (e.g. R=gloss, G=roughness) | `r`, `g`, `b`, `a`, `width`, `height`, `format` |

### Scripting

| Tool | Description | Key Args |
|------|-------------|----------|
| `run_script` | Execute JS against session stage | `script` (JS code) |

### Legacy (Viewer Workflow)

| Tool | Description |
|------|-------------|
| `store_asset` | Store asset with preview + geometry params |
| `read_asset` | Read stored asset |
| `read_asset_preview` | Read asset preview image |
| `get_asset_description` | Get asset description |
| `get_all_asset_descriptions` | Get all asset descriptions |
| `select_assets` | Select + arrange assets with transforms |
| `get_selected_assets` | Get currently selected assets |
| `save_screenshot` | Save a screenshot |
| `list_screenshots` | List screenshots |
| `read_screenshot` | Read screenshot image |
| `load_usd_layer_from_file` | Load USD as Layer from file (C++ native only) |
| `load_usd_layer_from_data` | Load USD as Layer from base64 |
| `load_usd_layer_from_asset` | Load USD as Layer from a stored asset |
| `to_usda` | Convert a loaded Layer to USDA text |
| `list_primspecs` | List root PrimSpecs in a loaded Layer |
| `get_usd_description` | Get description of a loaded USD Layer |
| `get_all_usd_descriptions` | Get descriptions of all loaded USD Layers |
| `debug_primspec_dump` | Dump a loaded Layer PrimSpec as deterministic JSON for debugging | `path`, `max_depth` |

> Note: `attr_connections` has a dispatch entry but is not advertised because
> the implementation currently returns an explicit “not implemented” error.
> `payload_list` and `variant_define` are implemented and advertised in
> `tools/list`.

## JavaScript Scripting

The MCP server includes a QuickJS engine (when built with `-DLIGHTUSD_WITH_QJS=ON`). JS scripts can be executed via the `run_script` tool and have access to the `lightusd.*` API:

```javascript
// Stage info
var info = lightusd.stage.info();
console.log("Up Axis: " + info.upAxis);

// List children
var result = lightusd.prim.listChildren("/World");
for (var i = 0; i < result.prims.length; i++) {
    console.log(result.prims[i].name + " (" + result.prims[i].type + ")");
}

// Get prim details
var prim = lightusd.prim.get("/World/mesh0");
console.log("Type: " + prim.type + ", Children: " + prim.childCount);

// Find all meshes
var meshes = lightusd.query.findAllByType("Mesh");
console.log("Found " + meshes.count + " meshes");

// Export to USDA
var usda = lightusd.stage.exportToString();
```

## Value Format

Attributes and values use structured JSON format:

```json
// Scalar
{"type": "float", "value": 1.5}

// Compound
{"type": "float3", "value": [1.0, 2.0, 3.0]}
{"type": "color3f", "value": [0.5, 0.2, 0.8]}

// Array
{"type": "float3[]", "value": [[1,2,3], [4,5,6]]}

// Matrix
{"type": "matrix4d", "value": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]}

// Quaternion
{"type": "quatf", "value": [0, 0, 0, 1]}

// Asset
{"type": "asset", "value": {"assetPath": "texture.png", "resolvedPath": ""}}

// Dictionary
{"type": "dictionary", "value": {"key1": {"type": "int", "value": 42}}}

// Blocked
{"type": "None"}
```

## Protocol

- Transport: HTTP POST to `/mcp`
- Protocol: JSON-RPC 2.0
- MCP spec version: 2025-03-26
- Session ID: `mcp-session-id` header (returned by `initialize`)
- Capabilities: `tools`, `resources`

## Security

- Max request body: 16 MB
- Max base64 input: 64 MB
- Max decoded payload: 48 MB
