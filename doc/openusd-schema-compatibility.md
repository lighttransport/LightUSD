# OpenUSD schema compatibility baseline

LightUSD pins its schema-difference baseline to OpenUSD **26.08**, commit
`ee47c679abde5b467a7b6a41f3b2285564a4222e`. The checked-in manifest contains
the schema kind, inheritance, namespace prefix, property type, variability,
fallback, and allowed-token data for all 134 upstream schemas. OpenUSD is not a
runtime or normal CI dependency.

Support must be described by capability tier rather than a single percentage:

1. USDA/USDC preservation and roundtrip
2. legacy typed prim API
3. next registry fallback and validation
4. next typed convenience accessors
5. Tydra extraction
6. Vulkan raster and CPU/Vulkan RT rendering
7. optional GL/CUDA/HIP rendering

Unknown schemas and properties are normally preserved generically; that does
not imply typed, validated, or rendered support. `doc/api-status.md` retains the
historical legacy API inventory, while this manifest is the source for upstream
schema-diff work.

The generated
[`lightusd-capabilities-openusd-26.08.json`](generated/lightusd-capabilities-openusd-26.08.json)
is the product-scope contract layered on that upstream inventory. It makes the
required supported domains and deliberate exclusions explicit and records the
pipeline capabilities every in-scope schema must eventually satisfy. It does
not infer completion from generic preservation or from the existence of a C++
struct.

The same ledger generates two regression artifacts:

- `tests/usda/generated/openusd-supported-schema-26.08.usda` authors one
  preservation case for every required schema. Concrete types use their schema
  type; applied APIs use `apiSchemas`; abstract and non-applied schemas use a
  generic marker prim.
- `src/schema/generated/openusd-supported-schema-names.inc` is the next
  registry's recognition table.

Registry recognition means LightUSD knows that a schema identifier belongs to
the supported surface. It does not claim that every property has a fallback,
validator, typed accessor, Tydra extractor, or renderer implementation. Those
remain separate capability tiers. The next regression passes the generated
USDA through USDA read, USDC write, and USDC read and requires all 92 markers
and schema identifiers to survive.

## Refresh

From an OpenUSD source checkout at the pinned tag:

```sh
python3 scripts/generate-openusd-schema-manifest.py \
  --openusd-root /path/to/OpenUSD \
  --version 26.08 \
  --commit ee47c679abde5b467a7b6a41f3b2285564a4222e \
  --output doc/generated/openusd-schema-26.08.json
python3 scripts/check-openusd-schema-manifest.py
python3 scripts/generate-lightusd-capability-ledger.py
python3 scripts/generate-lightusd-capability-ledger.py --check
python3 scripts/generate-openusd-supported-schema-fixture.py
python3 scripts/generate-openusd-supported-schema-fixture.py --check
```

Review the JSON diff before changing the pin. New schemas must be explicitly
classified as typed, registry-only, preserve-only, planned, or out of renderer
scope; new properties on supported schemas require fallback/validation tests.

## OpenUSD 26.08 deltas currently in scope

- `UsdGeomBackPlateAPI`: registered fallbacks, allowed-token validation, lazy
  typed accessors, Tydra extraction, multiple-instance propagation, and
  offline/GL/Vulkan display.
- `UsdGeomModelAPI.model:cardVisibility`: registered, validated, and exposed
  through a lazy typed accessor with inherited `full`/`simple` face selection.
- `UsdHydraHydraRenderPassAPI`: preserved generically and intentionally outside
  the renderer-neutral LightUSD API.
- `UsdVol` ParticleField schemas: registered and validated, with lazy typed
  accessors and Gaussian conversion recognizing official float/half attributes.
- `UsdRender` settings, products, variables, and passes: registered, validated,
  and exposed through typed accessors. `lusdrender` resolves settings/products/
  passes, resolution, camera, purposes, and local render-visibility pruning.
  External pass commands are preserved but never executed.
- `UsdSemanticsLabelsAPI`: registered with typed multiple-apply label access.
