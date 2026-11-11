# Resume: AOUSD Core conformance for LightUSD `core`

Handoff for a fresh coding-agent session. The AOUSD supplemental **composition**
gate is fully closed (138/138); this file lists what remains and how to work on it.

## Orientation

- **Repo / branch:** `/mnt/nvme02/work/lightusd-repo/physics-2026`, branch `physics-2026-fix2`.
- **Module under work:** `src/` — a standalone C++14 USD core (parser, crate,
  pcp composition, eval, validation, writer). It is NOT built by the top-level
  CMake; it has its own project + tests.
- **Conformance report (read first):** `doc/ousd-vs-lightusd.md`. Every subsection ends
  with a `**Remaining:**` note; the closing summary (~line 778) names the critical path.
- **ARKit/USDZ checker notes:** `doc/openusd-usdz.md`.

### Build & test

```sh
# Debug build (tests use assert(); a Release build compiles them out — do not use -DNDEBUG)
cmake --build build-next -j16
ctest --test-dir build-next --output-on-failure        # 32/32 must pass

# Threaded build — exercises the parallel-warm merge path (findings that only bite under threads)
cmake --build build-next-threaded-debug -j16 && ctest --test-dir build-next-threaded-debug
```

### The supplemental corpus (the conformance oracle)

- **Fetch (pinned, stable path):** `bash scripts/fetch-aousd-supplemental.sh`
  clones `aousd/core-spec-supplemental-public` at tag `release/1.0.1.post0`
  (commit `c15ae0cad3ed`) and prints the suite root:
  `~/.cache/lightusd/core-spec-supplemental-release_dec2025/releases/1.0.1`.
  It is intentionally NOT vendored.
- `build-next` is configured with `-DLIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT` pointing there.
- **pxr oracle (PINNED):** OpenUSD **26.05** dist at
  `/mnt/nvme02/work/lightusd-repo/OpenUSD/dist` — use its `bin/usdcat` and its
  python (`PYTHONPATH=.../dist/lib/python`, `Usd.GetVersion()==(0,26,5)`) for
  ALL differential gates and expectation baking. Do not mix with
  `/home/syoyo/local/USD` or the v26.03 source checkout.
- Run the gate directly:

```sh
python3 tests/next/run-aousd-supplemental.py \
  --suite-root /tmp/mxpv-openusd/vendor/core-spec-supplemental-release_dec2025 \
  --lusdcat build-next/next_usdcat \
  --aousd-test build-next/test_aousd_conformance \
  --max-composition-fail 0                 # ratchet is 0; keep it there
```

- Each case dir has readable `usda/` copies of its crate `.usd` layers, a `pcp.json`
  (expected composed prim paths / prim stacks / child names / prohibited names /
  attribute connections), and a `pcp.txt` (pxr prim-index dump — very informative).
- Reproduce one case:
  `cd <case dir> && build-next/next_usdcat -f --instance-mode native usda/root.usd`
  (add `--require-prim /Some/Path` to fail loudly on a missing prim).

### Wider regression gates (run before finishing any change)

```sh
bash tests/run-usdcat-compare.sh    # legacy roundtrip; BASELINE = 413 equiv / 0 diff (USDA), 222/1 (USDC)
                                    #   the 1 USDC diff + 2 USDA parse errors are PRE-EXISTING legacy-parser gaps
cmake --build build -j16            # main lib must still build (next compiles into it)
```

## Composition-engine map (for the composition items below)

The `core` PCP composition lives in `src/pcp/`. Key pieces after the 22-gap work:

- `namespace-mapping.hh` — `NamespaceMapping` is a longest-prefix-first SET of
  (source→target) pairs (pxr `PcpMapFunction`): one per arc + one per relocate.
- `prim-index.hh` — `StackRelocates` (per-layer-stack namespace edits) + `LayerStack`.
- `cache-layer-stack.inc` — `BuildStackRelocates` (validity rules + raw-path normalization).
- `cache-arc-expansion.inc` — `ProcessArc`, `ExpandArcs`, `ExpandList`,
  `SourcesForSite` / `DeriveChildSources` / `SourcesForRelocateSource`
  (relocate-aware source derivation), implied-class block (`Src::arc_chain`).
- `cache-compose.inc` — `ComposeOpinions` / `ComposeChildNames` (post-relocation child list).
- `cache-specs-instances.inc` — `SelectVariants` / `FallbackSelection`, instance keys.
- `cache-parallel-merge.inc` — `MergeSources` (remaps worker stack/map/arc_chain indices;
  skips stack-qualified `@N:`/`!N:` cache keys).

---

## Status (2026-07-13 session)

Items 1–6 below are ALL DONE. Commits, in order: `a36323902` (item 2),
`46d02c244` (item 1), `05e9a6bf0` (item 3), `b87303a74` (item 5),
`e556b252d` (item 6), `5c2aa33fc` (item 4a), `12385cded` (item 4b
increment), `808f2b790` (item 4b registry table), `7e0b3e739` (item 4c
variant storage + deep diff), `a0a91ce67` (differentials). All gates green
at each commit (both ctest builds, supplemental ratchet 0, roundtrip
baseline, main-lib build). Only item 7 (product-driven domain-schema
breadth) remains untouched.

### 1. Sampled-value oracle for value resolution — DONE
`tests/next/test_aousd_value_resolution.cc` translates the corpus's expected
resolved values (ctest `next_test_aousd_value_resolution`, self-skips without
the corpus; also `run-aousd-supplemental.py --aousd-value-test`, blocking).
Fixed three real resolver bugs it exposed: LVRPS clip strength (per-property
`clipShadowedProps` recorded during pcp compose), `times` jump-discontinuity
ordering, out-of-range stage-time mapping. The follow-up commit also routed
Tydra's animation bake through the core resolver (ParseValueClipSets +
ResolveValueClipFromSets; duplicate Tydra clip semantics deleted).

### 2. Composition specifier edge case — DONE
Ported pxr `_GetPrimSpecifierImpl`: ancestral-inherit class resolves in plain
strength order; only a DIRECT (non-ancestral) inherit class is weaker than a
def. `Src` gained an `ancestral` bit. Regression: `test_specifier_resolution`.

### 3. Data-type round-trip generated from the normative table — DONE
`TestNormativeTypeMatrix` embeds the §6 table (from the corpus JSON,
de-duplicated) and generates default/array/time-sampled/dictionary/alias
contexts with declared-name + byte-identical crate round-trip assertions.

### 4. Elective authored-state + list-op breadth — DONE
Done (4a): authored bits + explicit-empty round-trip for layer
`relocates`/`subLayers` and prim `relocates`/`variants`
(`TestAuthoredStateBits`). Explicit-empty arc lists were already covered
(parser ArcEdit + crate `\x01E` marker).
Done (4b increment): cross-site `reorder` now applies with pxr SdfListOp
semantics (`ApplyStringListOrder`, shared with clipSets/variantSet-name
edits); `test_cross_layer_string_listop_matrix` covers every qualifier
combination across a sublayer stack.
Done (follow-up session, commits `808f2b790` `7e0b3e739` `a0a91ce67`):
- Registry-driven field table: `src/layer/listop-field-table.hh`
  registers apiSchemas/variantSetNames/clipSets with ONE shared
  stronger-over-weaker merge; the three bespoke composition blocks are
  gone; variantSetNames upgraded from fill-absent to true list-op merge
  (`TestStringListOpFieldTable`).
- Variant-scope generic storage: `VariantData.unknownMeta/unknownFields`
  plumbed through usda parser/writer + crate (via MaterializeVariantHolders);
  layer-diff walks variants per field (`meta:variantSets:<set>/<opt>:<field>`)
  instead of stringified names (`TestVariantExtensionFields`,
  `test_variant_deep_diff`).
- Differentials: `blocked` diff reason for value->block transitions,
  `<field>(type-conflict)` for dict-vs-scalar keys, and a serial-compositor
  `Dictionary type conflict` diagnostic naming the dotted key
  (`test_blocked_and_type_conflict_diff`,
  `test_dictionary_type_conflict_diagnostic`). The parallel pcp fill stays
  silent by design (issues_ not thread-safe from worker fills).


### 5. General `SdfVariableExpression` function grammar — DONE
Full recursive-descent typed evaluator (literals, `${VAR}` with recursion +
cycle detection, escapes, if/and/or/not/eq/neq/lt/leq/gt/geq/contains/at/
len/defined; nesting/expansion caps). Wired at reference/payload arcs,
sublayer paths (vs the stack ROOT layer's expressionVariables), and variant
selections; `None` = no opinion. Tests: `TestVariableExpressionGrammar`,
`test_expression_sublayer_and_variant_selection`. The follow-up commit closed
the last gap: layer-stack identity now includes expression variables
(`LayerStack::cache_key`; inherited vars feed sublayer expressions;
`test_layer_stack_identity_expression_vars`).

### 6. Identifier validation at authoring boundaries — DONE
`Path::is_valid`/`Path::Parse`; validating `append_child`/`append_property`;
`Layer::define_prim_at_path` component checks; validation module's weak
byte-≥0x80 approximation replaced by the shared strict validator. Generated
UTF-8/XID boundary cases in `TestUnicodeAndPaths`. `PrimSpec::set_name`
deliberately stays raw (reader-internal, lexer-validated input).

### 7. Non-core OpenUSD domain-schema breadth — SCHEMA/VALIDATION PASS DONE
(Product parity, not conformance.) The registry now defines UsdVol
(Volume + VolumeFieldBase/VolumeFieldAsset/OpenVDBAsset/Field3DAsset chains,
pxr fallbacks), UsdRender (RenderSettingsBase/RenderSettings/RenderProduct/
RenderVar), and the UsdGeom breadth types (HermiteCurves, TetMesh, NurbsPatch
incl. trimCurve:*, NurbsCurves knots/orders/ranges/pointWeights/ids); the
validator adds `vol.volume.fieldRel`, `vol.fieldAsset.*` and `render.*`
structural checks and knows the new types' Gprim/non-Xformable placement.
Tests: `test_domain_schema_breadth` (registry), `test_vol_render_rules`
(validator). REMAINING product work: Tydra RENDERING converters for
volumes/tet meshes/NURBS (a resource + shader design, per doc ~:557) and
shader-node coverage beyond UsdPreviewSurface/UsdUVTexture/UsdPrimvarReader/
MaterialX — prioritize from application requirements.

### Housekeeping (not conformance, but tracked)
- 3 legacy-parser roundtrip fixtures fail (`aousd-namespace-order.usda`,
  `aousd-unknown-property-metadata.usda`, one USDC variant) — the LEGACY binary,
  which `core` does not feed. XFAIL; only relevant if touching `src/usda-*`.
- Oracle pinning: RESOLVED — standardized on the OpenUSD 26.05 dist
  (`/mnt/nvme02/work/lightusd-repo/OpenUSD/dist`, binary + python) for all
  differential gates; see "The supplemental corpus" section above.

## Working rules

- Keep the composition ratchet at **0**. Any change that regresses a supplemental
  case (composition/data_types/file_formats/value_resolution) fails the gate.
- Add a regression FIRST (in-memory `LayerBuilder` in `tests/next/test_pcp.cc` or a
  fixture under `tests/next/fixtures/`, NOT the shared legacy corpus).
- After any `src/pcp` change, run BOTH `build-next` and
  `build-next-threaded-debug` ctest (the parallel-warm merge path is thread-only).
- Diagnosis is cheap: `next_usdcat -f --instance-mode native <case>/usda/root.usd`
  diffed against `<case>/pcp.json` tells you exactly what's missing.
- Reference memory: `[[aousd-supplemental-composition]]`, `[[legacy-next-deep-audit-2026-07]]`.
