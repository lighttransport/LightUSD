# USD JSON conversion

LightUSD's USD-to-JSON and JSON-to-USD converters use the bundled `minijson`
library by default. The nlohmann compatibility API is optional and is disabled
in the normal build. The `lightusd::json::JsonWriter` facade also uses this
minijson representation for both `Layer` and `Stage`; `set_indent(0)` emits
compact JSON and the default indentation is two spaces.

## Layer representation

`to_json_string(Layer, ...)` emits a layer object containing:

- `name` and `typeName: "Layer"`;
- authored layer metadata in `metas`;
- `primSpecs`, keyed by prim path;
- recursive `children` objects in each prim spec;
- `properties`, with attribute or relationship records;
- authored composition list-ops (`references`, `payloads`, `inherits`,
  `specializes`, `variantSets`, and `variants`);
- optional `buffers`, `bufferViews`, and `accessors` when buffer array mode is used.

Layer metadata includes custom layer data and the supported AOUSD layer fields
for color management, ownership, expression variables, relocates, and unknown
metadata. Composition list-ops use `{ "op": ..., "items": [...] }` records so
the authored operation (explicit, prepend, append, add, delete, or order) is
not lost during a JSON round-trip.

Attributes preserve their USD `typeName`, variability, metadata, connection
paths, default value, blocked state, and time samples. Default and time-sample
values are stored in the canonical USDA value spelling so custom registered
value types remain inspectable. `customData` and `sdrMetadata` dictionaries
accept nested JSON objects and scalar JSON values.
Typed metadata arrays and registered role values use the structured
`{"type": ..., "value": ...}` representation so their value types survive a
JSON round-trip; existing scalar and dictionary forms remain plain JSON.

## Arrays

The default array mode stores raw bytes as base64 in an object containing
`data`, `count`, and `type`. `ArraySerializationMode::Buffer` stores the same
bytes in embedded buffer/accessor records. JSON import validates base64 size,
array counts, accessor references, byte ranges, and configured security limits.
Both Layer and Stage JSON support buffered mesh and basis-curve arrays on
export and import.

Accessor import accepts tightly packed and interleaved data (`byteStride`),
honors accessor and view offsets/lengths, and validates `componentType` before
copying data. Vector accessors may use either the logical vector count or the
legacy LightUSD scalarized count emitted by older writers.

External buffer URIs are disabled by default. Applications that intentionally
allow them can call the overload taking `JSONToUSDOptions`, provide an
`AssetResolutionResolver`, and set an appropriate
`max_external_buffer_bytes` limit. Embedded data URLs remain subject to the
normal JSON decoded-byte limit.

The string-based `JSONToGeomMesh` API accepts the same options overload as
`JSONToStage`, so a standalone buffered mesh can be imported without wrapping
it in a stage document. The nlohmann JSON APIs remain available for legacy
MCP/URDF/JS integration; the USD JSON codec and runtime value bridge use
minijson.

## Round trips and limitations

Layer prim specs, nested children, typed defaults, relationships, connections,
blocked values, metadata, and time samples are reconstructed by `JSONToLayer`.
Stage JSON is intended primarily as a composed-scene interchange/debug format;
`JSONToStage` reconstructs the runtime hierarchy emitted by `ToJSON(Stage)` for
Xform, GeomMesh, GeomBasisCurves, GeomSphere, GeomCube, GeomCone,
GeomCylinder, GeomCapsule, and GeomPlane prims, including stage
up-axis/comment, child ordering, and prim specifiers. Analytic geometry
preserves its scalar dimensions and axis; basis curves preserve their point,
normal, curve-count, and width arrays. Schemas without a typed runtime carrier
are reconstructed as generic `Model` prims with their authored type name and
hierarchy intact. Use layer JSON when preserving arbitrary schema-authored
properties is required.

Always apply the normal USD memory and asset-resolution limits when importing
JSON derived from untrusted input. The minijson parser and serializer both
enforce configurable nesting-depth limits to prevent stack exhaustion.
