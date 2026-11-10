# USDZ to JSON

This core example writes scene JSON and a separate asset sidecar. Build `usd_to_json` with `LIGHTUSD_BUILD_EXAMPLES=ON`; no legacy library
or Tydra dependency is required.

```sh
./build_ninja/usd_to_json model.usdz output
# output_usd.json: runtime scene JSON (version + primChildren)
# output_assets.json: archive filenames, byte sizes and base64 data
```

The default preserves the legacy runtime scene envelope, geometry aliases,
base64 mesh/curve arrays and scalar geometry defaults. Complete authored
property records are retained alongside the runtime fields, including prims
that the old serializer dropped. Use `--layer-json` before the filename for the
same authored layer JSON contract as the WASM exporter:

```sh
./build_ninja/usd_to_json --layer-json model.usdz authored
```

The first archive entry selects the root, following USDZ package semantics.
USDA and USDC roots are supported. Asset entries include the root and dependent
layers, binary payloads and empty files. Base64 is streamed to the sidecar so
encoding does not retain a second copy of the archive. Existing output files
are overwritten. With no output prefix, the input basename is used in the
current directory. Invalid input and file-write errors return a nonzero status.

The shared C++ API is `lightusd::WriteLayerToJSON(const Layer&)`, declared in
`writer/layer-json.hh` and returning `nonstd::expected<std::string, std::string>`.
It exports layer and prim metadata, authored attributes, relationships and
variants with the same traversal/output limits as the core WASM API.
