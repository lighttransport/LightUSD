# Reflection table: Lean ↔ LightUSD

One row per reflected theorem/definition. "Vectors" are the committed
`tests/next/generated/lean/*.inc` tables replayed by `tests/next/test_lean_vectors.cc`
(ctest `next_test_lean_vectors`).

| Lean | C++ (src/next) | Validator rule | Test |
|---|---|---|---|
| `ListOp.apply`, `iterate_combine` (L5), `apply_combine` (L6) | `ApplyStringListOp` layer/prim-spec-storage.cc:352; `ApplyInt64ListOps` :251 | — | `LEAN_LISTOP` vectors (7k rows, both paths) |
| `ListOp.chain`, `apply_chain` | `ApplyInt64ListOps` over a stack | — | `LEAN_LISTOP_CHAIN` vectors (1.5k rows) |
| `ListOp.applyCpp_eq_apply`, `applyCppInt64_eq_apply` | same | — | proof (Nodup hypotheses) |
| `ListOp.reduce`, `chain` | corpus `data_types/tests/combine_chain` | — | `usdspec-check-corpus` (Lean, 58/58) + `test_lean_vectors --combine-chain` (C++, 58/58), ctest `next_test_lean_vectors_combine_chain` |
| `Crate.encode32`/`decode32`, `decode32_encode32` | `EncodeDeltaU32`/`DecodeDeltaU32` crate/crate-format.cc:608 (C core: c/crate-delta-encode.c, c/crate-int.c) | — | `LEAN_INTCODE` vectors (decoder on all rows; encoder byte-identical on tie-free rows) |
| `LayerOffset.compose`, `compose_assoc`, `apply_compose` | `pcp::LayerOffset::Compose` pcp/namespace-mapping.hh:110 | `core.layer.subLayerOffset` (`LayerOffset.valid`) | `LEAN_OFFSET` vectors (400 rows) |
| `LayerOffset.cumulative` (recursive TCPS, L5466), `lightusd_order_eq_spec` | `AppendLayerAndSublayers` TCPS scaling pcp/cache-layers.cc:312,352 | — | `LEAN_TCPS_CHAIN` vectors: 340 sublayer chains written as USDA, composed by `pcp::ComposeStageFromFile`, leaf sample times compared |
| `ListOp.Legacy.applyLegacy`, `reorder_perm`, `add_not_expressible`, `reorder_not_expressible` | `ApplyStringListOp` added/ordered path, `ApplyStringListOrder` :325 | `core.listop.deprecatedAdd/Reorder` | proof; `add_ne_append` reproduces the REVIEW demo 05 pxr result |
| `Strength.strengthOrder` + spec examples 2–6 | `PrimIndex::GetStrengthOrder` | — | `decide` (Lean side); C++ comparison via `--dump-pcp-json` corpus gate |
| `Dict.merge`, `merge_assoc_of_compat`, `merge_not_assoc` | `MergeWeakerDictionary` composition/composition-merge-helpers.cc:25 | `core.dict.duplicateKey` (`Dict.WellFormed`, USDA and USDC diagnostics) | proof; pxr confirms non-associativity (LEAN-101) |
| `PathGrammar.parsePath`/`printPath`, `parse_print` | `Path::Parse`/`is_valid` prim/path.cc; `lu_is_valid_path_string` c/identifier.c | `core.prim.name` | `LEAN_PATH_VALID` (50 rows; pxr-checked). **Fixed 2 LightUSD bugs**: `a.b.c`/`.rel.attr` accepted (LEAN-203), variant-selection whitespace not canonicalized |
| `Schema.parseFamilyVersion`, `versionedMayInherit` | `ParseSchemaFamilyAndVersion` schema/schema-registry.hh:88 | `core.schema.familyVersion`, `core.schema.selfFamilyInherit` | `LEAN_SCHEMA_FAMILY` (18 rows) |
| `Crate.ValueRep` pack/unpack (bits 63/62/61) | `ValueRep` crate/crate-format.hh:166 | — | `LEAN_VALUEREP` (incl. reps observed in pxr 26.11 files) |
| `ValueResolution.TimeSamples.eval` (held/linear/blocks/clamp) | `AttributeEval::EvalWith` eval/attribute-eval.cc | — | `LEAN_VR` (218 rows, USDA → stage → eval) |
| `Composition.NamespaceMap`, `Relocates.validRelocates` | pcp/namespace-mapping.hh, `BuildStackRelocates` pcp/cache-layers.cc:84-249 (coverage list in Relocates.lean header) | `core.layer.layerRelocates.*` | proofs; corpus `--compare-pcp` gate |
| `Strength.strengthOrder`, `PrimIndex` toy composition | `Cache::GetPrimStack` (new), `PrimIndex::GetStrengthOrder` | — | `next_usdcat --dump-pcp-json` + `run-aousd-supplemental.py --compare-pcp` (138 corpus cases: 107 exact, 31 allowlisted with reasons, ratchet 0) |
| `Population.listOrderedPrimChildren` | `ComposeChildNames` pcp/cache-opinions.cc | — | corpus "Child names" via `--compare-pcp` |

The follow-up adds exact declaration resolution through `--lake`, including
negative namespace tests and a required checker-binary gate in CI. Corrected
legacy-operation references name `UsdSpec.ListOp.applyLegacy`; a module filename
does not introduce a `Legacy` namespace. `mostCommon_spec`/`mostCommon_mem` and
`Lz4.greedyBlock_endRulesOk` are general proofs, beyond the existing byte vectors.
