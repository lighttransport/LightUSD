# lusdchecker

`lusdchecker` validates USDA, USDC, USDZ, and MaterialX `.mtlx` documents using
LightUSD's dependency-free `next` core. It runs AOUSD Core 1.0.1 semantic checks and the
available structural schema checks for UsdGeom/UsdSkel, UsdShade/MaterialX,
UsdLux, and UsdPhysics (including the supported physics extensions).

```sh
cmake -S . -B build_ninja -G Ninja -DLIGHTUSD_BUILD_TOOLS=ON
cmake --build build_ninja --target lusdchecker

build_ninja/lusdchecker scene.usdz
build_ninja/lusdchecker --core-only scene.usda
build_ninja/lusdchecker --groups core,geom,physics --strict scene.usdc
build_ninja/lusdchecker --composed scene.usda
build_ninja/lusdchecker --groups package,crate scene.usdz
build_ninja/lusdchecker --arkit scene.usdz
build_ninja/lusdchecker --require-all-groups --all scene.usdz
build_ninja/lusdchecker --list-groups
build_ninja/lusdchecker --json -o report.json scene.usdz
build_ninja/lusdchecker material.mtlx
build_ninja/lusdchecker --dump-rules
build_ninja/lusdchecker --usdchecker-compat --composed scene.usda
build_ninja/lusdchecker --composed scene.usda                 # all variant combos
build_ninja/lusdchecker --composed --skip-variants scene.usda # authored selections
build_ninja/lusdchecker --variants shape:sphere,lod:high scene.usda
build_ninja/lusdchecker --variant-sets shape scene.usda
```

The default is equivalent to `--all`. `--arkit` adds the opt-in ARKit /
RealityKit delivery profile (`arkit` group + `core,geom,shade,package`), the
lightusd counterpart of `usdchecker --arkit`: Y-up stage metadata, the
ARKit prim-type whitelist, `id`-based UsdPreviewSurface / UsdUVTexture shading,
portable texture formats, normal-map scale/bias, resolvable material bindings,
and a `.usdc`-rooted package containing only layers and exr/jpg/jpeg/png
textures. See `doc/openusd-usdz.md` for the full rule mapping. `arkit` is a
delivery profile rather than a defect class, so it is *not* part of `--all`.
Reports use stable rule identifiers such
as `core.layer.defaultPrim`, `geom.mesh.topology.index`, and
`physics.joint.limit`. `--strict` makes warnings fail validation. Normal parsing
accepts safely-readable implementation extensions; `--strict-parse` additionally
rejects non-conforming or unsupported format data before semantic validation.
MaterialX checks cover XML parsing, material-to-shader references, terminal
connections, `MaterialXConfigAPI`, version/source metadata, and shader outputs.

Exit status is `0` for valid input, `1` for validation failure, and `2` for a
command-line, I/O, or parse error.

## usdchecker parity

`lusdchecker` supports OpenUSD `usdchecker` CLI aliases and built-in validation families. The pxr flag
spellings are accepted as aliases (`--dumpRules`, `--includeKeywords`,
`--skipVariants`, `--rootPackageOnly`, `--noAssetChecks`,
`--disableVariantValidationLimit`), and pxr validator-keyword names map onto
rule groups in `-g`/`--include-keywords` (for example `UsdGeomValidators` →
`geom`, `UsdzValidators` → `package,crate`).

- `--dump-rules` prints every rule id with its group and a one-line doc
  (the counterpart of `usdchecker --dumpRules`).
- With `--composed`, every combination of authored variant selections is
  composed and validated (like usdchecker's default), deduplicating repeated
  findings; the sweep is capped at 1000 combinations unless
  `--disable-variant-validation-limit` is given, and truncation is always
  reported. `--skip-variants` validates only the authored selections;
  `--variants set:variant,...` pins explicit selections (implies `--composed`);
  `--variant-sets a,b` restricts the sweep to the named sets.
- `--no-asset-checks` disables the defaultPrim presence rule
  (`core.layer.defaultPrim.missing`), matching `usdchecker --noAssetChecks`;
  the upAxis/metersPerUnit presence rules stay on, also matching pxr.
- `--root-package-only` skips dependency-following checks (nested layer type
  audits and `core.dependency.unresolvable`).
- Severity defaults are lusdchecker's own; `--usdchecker-compat` raises the
  rules usdchecker reports as errors (missing stage metadata, MaterialBindingAPI,
  unresolvable dependencies, encapsulation, sdr type mismatches, ...) to error
  severity so exit codes line up with pxr's.

`tests/run-lusdchecker-vs-usdchecker.sh` is the differential parity harness: it
runs both checkers over `tests/usda` and fails if an exact mapped reference
finding lacks a matching lusdchecker finding and site. It runs in ctest as
`lusdchecker_vs_usdchecker` and self-skips when pxr is not installed.

## Current scope

Validation is performed on the authored `next::Layer` by default. `--composed`
resolves external arcs, validates the flattened result, and reports composition
errors and cross-arc attribute/property-kind type conflicts, and enumerates
every combination of authored variant selections (see "usdchecker parity"
above). Schema checks are structural and cover the schemas known to
LightUSD; they are not a plugin registry. The `package` group checks USDZ root
ordering, store mode, encryption/data-descriptor policy, 64-byte alignment,
CRC-32, central-directory consistency, safe/unique paths, portable extensions,
and authored dependency containment, including dictionary, clip, property
metadata, and time-sampled asset values.
The `crate` group performs a bounded decode plus token/path/field/fieldset/spec
cross-table checks on direct USDC files and USDC entries in USDZ packages.
Automatic fixers and OpenUSD's dynamically discovered validator plugins remain
out of scope. `--require-all-groups` turns a requested but inapplicable group
into an error; JSON reports requested, checked, and skipped groups separately.

## Strict and AOUSD Core profiles

```sh
build_ninja/lusdchecker --profile strict --json scene.usdz
build_ninja/lusdchecker --profile aousd-core-1.0.1 --sarif scene.usda
build_ninja/lusdchecker --all-time-samples --max-samples 10000 scene.usda
```

`--profile strict` enables strict parsing, composition, all applicable built-in
rule groups, reachable variants, numeric samples, and failure on warnings.
Missing schema/shader definitions, unavailable resolvers, evaluation failures,
and exhausted coverage limits fail the gate with `complete: false`. Coverage
findings cannot be suppressed by a baseline. `--strict` retains its original,
smaller meaning: fail on warnings in the selected checks.

`--profile aousd-core-1.0.1` runs the implemented normative document constraints
for the pinned Core version. It accepts valid layer fragments without requiring
asset-delivery metadata such as defaultPrim, upAxis, or metersPerUnit. It excludes
schema recommendations and the ARKit profile. A passing result is scoped to the
implemented checks; it is not certification of every requirement of the spec.
Newer USDA header versions are reported as outside the pinned Core coverage.
Both full profiles reject flags that reduce coverage, regardless of argument
order. Package and crate checks apply only to applicable containers.

Time validation includes DefaultTime plus the union of authored time samples,
spline knots, clip boundaries, and mapped clip samples. Related attributes are
evaluated at the same time. This is sample validation, not continuous-time proof
of spline extrema. The sample cap defaults to 10,000; exceeding it is an explicit
incomplete result. Layer loading uses the memory budget and a run-local cache.
Reachable variants include sets introduced by references and selected nested
variants. Traversal defaults to at most 1,000 composition passes.

JSON report version 2 adds `profile`, `complete`, `executionSuccessful`,
`conformance`, and `conformanceScope`. Each issue includes its category,
reference error identifiers, source asset, variant selections, and numeric time
when applicable. `conformance` describes the implemented Core constraints;
`valid` describes the selected validation policy, and `gatePassed` includes the
baseline decision. A schema error can therefore fail `valid` while Core
`conformance` passes. Composed findings use the stage root as their source;
authored-layer and container findings identify the inspected asset.
Baseline identities include source, variants, and time. Recreate older baselines
to use these identities. Parse, I/O, and usage failures produce structured
JSON/SARIF with exit 2; validation or incomplete coverage uses exit 1.

## Declarative schema and shader definitions

```sh
build_ninja/lusdchecker --profile strict \
  --schema-definitions vendor-schemas.json \
  --shader-definitions vendor-shaders.json scene.usda
```

Both flags are repeatable. Manifests are JSON with `formatVersion: 1`; each file
is limited to 16 MiB. Imports are transactional and reject inheritance cycles,
unknown ancestors, invalid property types, and conflicting duplicates. Identical
imports are idempotent. The schema format is the same as
`doc/generated/openusd-schema-26.08.json`:

```json
{"formatVersion":1,"schemas":[
  {"name":"VendorSphere","inherits":"Sphere","kind":"concrete",
   "properties":[{"name":"weight","kind":"attribute","type":"double"}]}
]}
```

Supported schema kinds are `concrete`, `abstract`, `singleApply`,
`multipleApply`, and `nonApplied`. Multiple-apply schemas declare
`propertyNamespacePrefix`; their property names are relative to the instance
namespace. Relationship definitions use `kind: "relationship", type: "rel"`.
Schema definitions provide inheritance, applied-schema placement, and declared
property-type checks. They do not implement custom schema behavior.

```json
{"formatVersion":1,"shaders":[
  {"identifier":"VendorNode","sourceType":"vendor",
   "inputs":{"gain":"float"},"outputs":{"out":"color3f"}}
]}
```

Shader identifiers are explicit; an `ND_` prefix alone does not establish a
known shader. The shipped data covers UsdPreview shaders and the exported
MaterialX standard library, standard surface, and OpenPBR definitions. A source
shader needs a matching exported identifier/source type for complete strict
validation. Arbitrary renderer code and asset resolvers are not loaded.

Convert declarative source files outside the checker:

```sh
python3 scripts/export-checker-shaders.py shaderDefs.usda library_defs.mtlx \
  --output vendor-shaders.json
python3 scripts/generate-openusd-schema-manifest.py --help
python3 scripts/generate-checker-definitions.py --check
```

The shader exporter handles local NodeDef inheritance and rejects unsupported
MaterialX types and DTD/entity declarations. Resolve external library includes
before exporting; it does not run Sdr, renderer plugins, or shader code.

## Compiled validators

Link a custom executable to `lusdchecker_lib`, construct a caller-owned
`lightusd::next::ValidationRegistry`, register callbacks, then call
`lusdchecker::RunChecker(argc, argv, registry)`. See
[custom-checker-example.cc](custom-checker-example.cc) and
[validation-context.hh](../../src/next/validation/validation-context.hh).
The stock executable loads no dynamic plugins.

Callbacks select Layer, Stage, or Prim scope, keywords, optional prim schema
filters, and rule metadata. The synchronous context borrows the layer/stage and
carries variants, time, resource limits, and options. Schema filters include
inheritance and applied APIs. Register before sharing the registry as const;
registries and imported definitions are isolated between runs. Return an
`expected` error to report incomplete validation. Custom findings participate in
JSON, SARIF, baselines, strict gates, `--dumpRules`, and `--includeKeywords`.

## Reference coverage and regression

The checked-in mapping pins OpenUSD commit
`2095fafafd033fa23386d7ec6d58c7cc33974518`. Schema definitions retain the repository's
newer 26.08 manifest. The differential harness compares exact reference error
IDs, severity, prim sites, and process results, allows additional findings, and
reports unexercised IDs explicitly. A successful corpus run establishes parity
for that corpus, not for arbitrary third-party plugins or all possible USD.

```sh
ctest --test-dir build_ninja -R 'lusdchecker|validation_registry' --output-on-failure
USDCHECKER_PATH=/path/to/OpenUSD/dist/bin/usdchecker \
  python3 tests/checker-parity.py --require-reference --report /tmp/parity.json
```

The harness includes generated positive and negative physics/shading/package
fixtures. Crashes, timeouts, missing reports, unknown reference validators, and
unmapped findings fail comparison. It skips an unavailable reference unless
`--require-reference` or `REQUIRE_USDCHECKER=1` is set.
