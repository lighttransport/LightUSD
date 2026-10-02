# AOUSD Core 1.0.1, OpenUSD, and LightUSD `next`/Tydra-next review

| Review input | Pinned value |
|---|---|
| Review date | 2026-07-12 |
| LightUSD revision | `d4a4053e7c501717709ac512c41e6eb31ad6f9a2` (`physics-2026-fix2`) plus the reviewed remediation working tree described below |
| OpenUSD source revision | `2095fafafd033fa23386d7ec6d58c7cc33974518` (`v26.03-278-g2095fafaf`) |
| OpenUSD comparison tool | `../OpenUSD/dist/bin/usdcat`; installed headers report `PXR_VERSION 2605` |
| Normative specification | [AOUSD Core Specification 1.0.1](../aousd/core/1.0.1/core_spec.md), source revision `b7bc21a`, 2025-12-12 |
| Vendored specification provenance | `aousd/specifications-public` at `2f9e746c4fbd7f48d6d2c9ac568133fe398bbfc0` |

The vendored Markdown and [license](../aousd/core/1.0.1/LICENSE) are exact copies from that checkout. Their SHA-256 digests are `b2feeb2c900befe224a372b2d60d0ddcc6e35048ab54c32c17d5c085b44c642d` and `9cc97638cf0185884ac800144b6246c7772f94ff2cc70686afa9574aaea4fa2b`, respectively.

## Implementation update

The gaps selected for the 2026-07-11 remediation pass and the 2026-07-12 deeper audit now have code and focused regression coverage. This update supersedes the original baseline statements below where a finding is marked fixed or mitigated.

| Requested area | Current status | Implementation |
|---|---|---|
| Unicode identifiers | **Fixed** | `next` reuses LightUSD's generated Unicode XID tables for UTF-8 lexing and prim-name validation. |
| Invalid paths | **Fixed in strict mode** | `ParseOptions::strict_aousd_conformance` and `next_usdcat --aousd-strict` enforce the AOUSD §8 path grammar. Compatibility mode retains permissive legacy ingestion. |
| Spline support | **Implemented** | Typed splines parse, evaluate (`timeSamples > spline > default`, held/linear/bezier/hermite + extrapolation + `autoEase`), and serialize to USDC type-59. Per-knot `customData` now survives USDA and the Crate `unordered_map<double, VtDictionary>` framing. |
| Unsupported typed-value loss | **Mitigated; `frame4d`/`pathExpression` first-class, `opaque`/`group` blocks supported** | Typed values round-trip; `opaque`/`group` declarations and their only agreeing value (`None`) are accepted. Other extension defaults are preserved as raw authored values, and strict mode rejects unencodable values. |
| Recursive dictionaries | **Fixed for next composition paths** | Prim and property dictionary metadata now recursively fill missing weaker keys. |
| Relationship list operations | **Fixed for composed target lists** | Strength-ordered opinion stacks apply explicit/prepend/append/delete/reorder operations across two or more sites and emit the resolved explicit list. |
| Specifier and stage queries | **Fixed for reviewed ordinary, multi-inherit, specialize, and implied-arc cases** | Specifier matrices match the reviewed oracle; active/defined/abstract/concrete/model queries are ancestry-aware. Populated stages retain deferred-payload loaded state, and implied sub-root classes are mapped into root and intermediate reference stacks. |
| Namespace ordering | **Fixed for USDA and next USDC paths** | Root, prim-child, and property order are stored, composed, applied to traversal, emitted to USDA, and encoded as distinct Crate `primOrder`/`propertyOrder` fields rather than inferred from natural child lists. |
| Schema fallback/population | **Complete for AOUSD Core schemas; broad for OpenUSD domains** | The generated AOUSD table covers every property/fallback on `ColorSpaceDefinitionAPI`, `ColorSpaceAPI`, and multiple-apply `CollectionAPI`, including instance-name expansion. The geometry/shade/skel/physics registry additionally covers (PRODUCT PARITY) UsdVol (Volume + VolumeFieldAsset/OpenVDBAsset/Field3DAsset chains), UsdRender (RenderSettingsBase/RenderSettings/RenderProduct/RenderVar with pxr fallbacks), and the UsdGeom breadth types (HermiteCurves, TetMesh, NurbsPatch incl. trimCurve, NurbsCurves knots/orders); it is still a supported subset of OpenUSD's full generated schemas. |
| Core value clips | **Implemented subset** | Core `AttributeEval` resolves explicit/template/multiple sets, manifest gating, missing-value interpolation, nested clip-bearing stages, caching, recursion limits, and cycle diagnostics. All eight supplemental value-resolution entry layers now load/compose through the adapter; assertion-level oracle comparison remains. |
| Strict versus compatibility policy | **Implemented** | USDA, USDC, composition, and evaluation expose `strict_aousd_conformance`; USDC strict reads promote lossy warnings to errors and strict writes reject raw fields that Crate cannot encode (splines now encode as type-59). |
| Relationship forwarding | **Implemented for ordinary and native instance-proxy paths** | Raw and recursively forwarded targets retain ordering/dedup/cycle behavior. Prototype targets are mapped into each instance namespace; instance roots also expose prototype properties and relationship names. |
| Elective authored state | **Generated coverage table complete for the AOUSD document-model inventory** | A generated 75-field table classifies every reviewed elective field as typed, structural, or opaque-preserved. `displayGroupOrder` and property `comment` are now typed/authored through USDA, USDC, and composition; deprecated fields remain intentionally opaque but lossless. |
| Supplemental AOUSD suite | **Integrated; ratcheted baseline recorded** | An optional external-corpus CTest adapter covers foundational data, all 54 USDA/USDC format assets, all 138 composition expectations (including expected prim paths), and all eight value-resolution entry layers without vendoring binary assets. Current composition result is **138/138 pass, 0 measured gaps** (see [Verification performed](#verification-performed)); the other integrated groups pass. |
| Expression variables | **Implemented for composition asset-path substitution** | Backtick direct-variable and quoted `${NAME}` interpolation is evaluated for reference/payload paths with closer-to-root stack precedence. Policy is explicit: disabled, warn-and-preserve, or require-resolved; undefined/non-string/unsupported expressions produce typed diagnostics. |
| USDC follow-up audit | **Fixed for bounded findings** | Omitted reference paths, compression dispatch, documentation, and sparse ordering are fixed. Unknown Layer, Prim, Attribute, and Relationship fields now retain their token, typed value, and `UnregisteredValue` source and rewrite interoperably instead of being dropped or rejected merely for being unknown. |
| Interpolation breadth | **Implemented for AOUSD §12.6 types** | Every normative scalar, vector, matrix, and quaternion family plus held fallback is covered; semantic aliases preserve their declared roles. Numeric array interpolation remains an implementation extension, not part of the §12.6 compliance claim. |

The dedicated [`test_aousd_conformance.cc`](../tests/next/test_aousd_conformance.cc) regression covers Unicode, the normative §8 valid/invalid path examples, typed spline parse/evaluate/USDC round-trip, the expanded foundational type matrix, `opaque`/`group` block USDA/USDC round trips, dictionary and multi-site relationship composition, raw/forwarded relationship targets and cycles, authored-empty defaultPrim/documentation/dictionaries/color metadata/namespace orders/relationships, the complete reviewed `apiSchemas`, `variantSetNames`, and `clipSets` sublist matrices, ordinary/direct-inherit/multi-inherit/specialize specifier matrices, ancestry and model-hierarchy queries, fixture-backed USDA/USDC namespace order, composed defaultPrim references, the exact §12.6 interpolation matrix, schema fallback/population, value clips (set ordering, manifest gating, and bracketing interpolation), and strict missing-loader behavior. Pxr-authored USDC fixtures separately verify empty order/list field presence and that unknown fields at every core spec scope cannot bypass strict loss detection.

LightUSD still must not claim full AOUSD Core 1.0.1 compliance, but the supplemental composition delta that used to be the largest measured blocker is now **closed: all 138 supplemental composition cases pass** (see [Supplemental corpus](#verification-performed)). OpenUSD domain-schema breadth and the full `SdfVariableExpression` function grammar remain outside the completed AOUSD Core schema and asset-path-substitution subsets.

## Executive conclusion

LightUSD `next` is a substantial, security-conscious USD implementation, and its tested USDA, USDC, USDZ, composition, validation, and Tydra conversion surfaces are much broader than a minimal reader. After the remediation above, it is safer and materially closer to the normative behavior, but it is still **not fully conformant with AOUSD Core Specification 1.0.1** under the specification's section 4 compliance definition.

That conclusion is not based merely on missing OpenUSD APIs. It follows from confirmed normative behavior differences in each of the three mandatory compliance areas:

- **Composed Stage Population:** the confirmed dictionary, relationship list-op, and property/child ordering differences are fixed; complete schema breadth and some specifier/field rules remain.
- **Value Resolution:** core value clips (including nested/cycle handling and bracketing interpolation), the AOUSD linear/held type matrix, ordinary/instance relationship forwarding, DefaultTime, and registry-driven AOUSD Core fallbacks now exist. Supplemental entries load; direct comparison of every upstream sampled value remains.
- **Format Implementations:** Unicode identifiers are fixed, unsupported USDA values are no longer silently lost, and typed splines (USDA parse/evaluate + USDC 0.12 type-59 codec) are implemented and OpenUSD-differential-verified. The remaining type-table risk is generated completeness across every authored container/context, not the reviewed scalar matrix; invalid paths are rejected under strict mode.

The most urgent baseline problem was **silent semantic or authored-data loss**. The remediated paths now either preserve authored USDA text, compose the missing opinions, or fail closed under strict mode. Compatibility mode remains intentionally permissive and must not be confused with a conformance run.

The review also found strong foundations worth preserving: bounded parsing, no-exception error paths, broad Crate structure support, disciplined USDZ writing, meaningful composition coverage, extensive differential tests, and Tydra diagnostics that generally expose renderer-level degradation instead of hiding it.

## Scope and interpretation

This review covers:

- `src/next` core data model, USDA/USDC/USDZ I/O, composition, resource resolution, validation, and value evaluation;
- `src/tydra/next` render-scene conversion and access behavior;
- relevant legacy LightUSD behavior where `lusdcat` provides a useful second implementation point;
- the checked-out OpenUSD source and `usdcat` as the expected behavior oracle where the AOUSD text is ambiguous;
- the normative AOUSD Core Specification 1.0.1 stored in this repository.

It does **not** require LightUSD to reproduce every OpenUSD library, plugin, Python binding, imaging subsystem, or command-line option. Those are OpenUSD parity questions, not automatically AOUSD conformance requirements. Conversely, a small API surface can still be conformant if its implemented stage, resolution, and format behavior meets section 4.

The December 2025 AOUSD supplemental release is integrated as an optional external corpus through [`run-aousd-supplemental.py`](../tests/next/run-aousd-supplemental.py). It is Apache-2.0 but is not vendored, avoiding 19 MB of binary fixtures. The Core Specification remains normative; the adapter supplies a measurable regression baseline rather than a compliance declaration.

### Principal implementation evidence

The findings below are grounded primarily in these implementation areas:

| Concern | LightUSD source |
|---|---|
| USDA property parsing and unsupported suffix handling | [`ascii-parser-prim.cc`](../src/next/parser/ascii-parser-prim.cc) |
| Foundational type registry and values | [`type-id.hh`](../src/next/types/type-id.hh), [`type-info.cc`](../src/next/types/type-info.cc), [`value.hh`](../src/next/types/value.hh) |
| Unicode identifiers and path representation | [`identifier.hh`](../src/next/prim/identifier.hh), [`path.hh`](../src/next/prim/path.hh), [`path.cc`](../src/next/prim/path.cc) |
| Composition and flattening | [`composition.cc`](../src/next/composition/composition.cc), [`flatten.cc`](../src/next/pipeline/flatten.cc), [`pcp/`](../src/next/pcp/) |
| Attribute, clip, and fallback evaluation | [`attribute-eval.cc`](../src/next/eval/attribute-eval.cc), [`value-clip.cc`](../src/next/eval/value-clip.cc), [`schema-registry.cc`](../src/next/schema/schema-registry.cc), [`interpolation.cc`](../src/next/types/interpolation.cc) |
| Crate reading and feature decoding | [`crate-reader.cc`](../src/next/crate/crate-reader.cc), [`crate-reader-decode.cc`](../src/next/crate/crate-reader-decode.cc), [`crate-format.hh`](../src/next/crate/crate-format.hh) |
| Asset resolution | [`asset-resolver.cc`](../src/next/resolver/asset-resolver.cc) |
| USDZ I/O | [`usdz-reader.cc`](../src/next/reader/usdz-reader.cc), [`usdz-writer.cc`](../src/next/writer/usdz-writer.cc) |
| Semantic validation | [`usd-validation.cc`](../src/next/validation/usd-validation.cc) |
| Tydra render conversion | [`render-converter.cc`](../src/tydra/next/render-converter.cc), [`render-data.hh`](../src/tydra/next/render-data.hh) |

### Status and priority vocabulary

| Marker | Meaning |
|---|---|
| Conformant/strong | Reviewed behavior agrees with the normative requirement or shows unusually complete support. |
| Partial | Useful support exists, but required cases or exact semantics are missing. |
| Non-conformant | A confirmed normative input or operation produces a different result. |
| OpenUSD-only gap | Difference from the reviewed OpenUSD revision outside AOUSD Core 1.0.1. |
| P0 | Blocks a conformance claim, causes silent data loss, or invalidates a core operation. |
| P1 | Material interoperability or correctness difference needing near-term repair. |
| P2 | Important completeness/API/diagnostic gap without demonstrated silent core-data loss. |
| P3 | Tooling, documentation, or lower-priority parity improvement. |

## Compliance overview

AOUSD Core 1.0.1 section 4 requires exact results in three categories, not merely successful parsing.

| Compliance area | Status | Principal evidence |
|---|---|---|
| Composed Stage Population | **Partial, improved** | Recursive dictionaries, relationship list ops, custom OR, authored ordering, reviewed ordinary/direct-inherit specifiers, and ancestry-sensitive stage queries are repaired; full schema and complex multi-arc specifier breadth remain. |
| Value Resolution | **Partial, improved** | AOUSD Core schema fallbacks, explicit/template/multiple/nested clips with cycle bounds, typed spline evaluation, DefaultTime, and ordinary/instance relationship forwarding are implemented; upstream value assertions are not yet imported as a direct oracle. |
| USDA Format Implementation | **Partial, loss-safe** | Unicode, ordering, and typed spline parse/evaluate are implemented; compatibility mode preserves other unsupported value text and strict mode rejects it. |
| USDC Format Implementation | **Partial, fail-closed option** | Spline type 59, compressed arrays including small flagged payloads, sparse population order, defaultPrim references, standard `documentation`, and exact `apiSchemas` token-/`variantSetNames` string-list-op sublists are implemented; strict mode promotes undecodable/ignored fields at every reviewed spec scope to errors. Generic extension fields and unmodeled list-op fields remain. |
| Generic `.usd` forwarding | **Partial** | Format detection and forwarding exist; correctness inherits the selected USDA/USDC gaps. |
| USDZ Format Implementation | **Strong, with reader caveats** | Writer uses stored ZIP32 entries, 64-byte alignment, root-first layout, no encryption, and no archive comment. Reader is deliberately more permissive, which the specification allows. |

### Chapter-level matrix

| AOUSD Core area | `next` assessment | Notes |
|---|---|---|
| Foundational data types (§6) | Partial, improved | Broad scalar/vector/matrix/array support; `frame4d`, `pathExpression`, typed splines, and `opaque`/`group` value blocks round-trip. The expanded matrix is still hand-maintained rather than generated from the normative table. |
| Document and layer data model (§7) | Partial | Strong spec containers and metadata coverage, but generic extension fields and empty-but-authored elective metadata/list-op states cannot be represented uniformly. |
| Paths and identifiers (§8) | Partial/strict-conformant subset | Unicode XID and authored ordering are implemented; strict mode now covers the normative valid/invalid example table, including repeated parent traversal and the absolute-path single-property restriction, while compatibility mode remains permissive. |
| Asset resolution (§9) | Partial, improved | Filesystem/package/search-path resolution, explicit URI-style scheme handlers, callbacks, context-stable identifiers, `usd-anon:` memory assets, and policy-controlled asset-path expression substitution are implemented. Full general expression functions and richer scheme contexts remain. |
| Composition (§10) | Partial | Sublayers, references, payloads, variants, inherits, specializes, relocates, cycles, and prototypes have meaningful coverage; exact field composition still differs. |
| Composed stage population (§11) | Partial, improved | Authored ordering, recursive dictionaries, custom OR, and relationship list-op results are now represented; remaining work is schema/specifier breadth. |
| Value resolution (§12) | Partial, improved | List ops, AOUSD Core schema fallbacks, blocks, bounded nested clips, typed spline evaluation, DefaultTime, and ordinary/instance relationship forwarding are implemented; all supplemental entry layers load, while direct sampled-value oracle comparison remains. |
| Schemas (§13) | AOUSD Core complete; OpenUSD-domain partial | Generated definitions cover all three AOUSD Core schemas and multiple-apply collection names. Non-core OpenUSD domain schemas remain a next-supported subset. |
| Color (§14) | Partial | Color metadata and Tydra colorspace handling exist; full normative authored/fallback semantics are not supplied by core. |
| Collections (§15) | Partial | Data structures and validation exist, but this review did not find a complete schema/fallback-backed collection evaluation implementation. |
| Core formats (§16) | Mixed, improved | USDZ writer is strong; USDA and USDC cover typed splines (type-59), order fields, compressed arrays, defaultPrim references, and standard documentation fields. Generic extension fields and exact elective authored state remain incomplete. |

## Confirmed conformance findings

### AOUSD-FMT-001 — Spline syntax (was silently discarded) (P0)

**Remediation status:** implemented. Typed splines now parse, evaluate, and serialize to USDC 0.12 (type 59); OpenUSD-differential verified in both directions.

**Normative area:** USDA Spline grammar, §§7.4.2.4, 12.3, 12.6, and USDC 0.12 type support.

At the baseline revision, the repository fixture `tests/usda/spline_basic.usda` produced five unknown-property warnings and rewrote to an empty `Splines` prim. It now round-trips with typed spline semantics.

The implementation ([`src/next/types/spline.{hh,cc}`](../src/next/types/spline.hh)) covers:

- a typed `SplineData`/`SplineKnot` model (bezier/hermite, dual-valued knots, per-knot tangents, tangent algorithms including `autoEase`, held/linear/curve interpolation, held/linear/sloped/loop extrapolation);
- USDA parse/write of the `.spline` block, with strict mode rejecting malformed splines at parse time;
- USDC 0.12 type-59 read/write, byte-compatible with `pxr/base/ts/binary.cpp` (0.13 bump when a knot carries a tangent algorithm);
- spline value resolution in `AttributeEval` at the AOUSD precedence `timeSamples > spline > default`, casting to the declared `double`/`float`/`half`, with `autoEase` tangents recomputed per `pxr/base/ts/knotData.cpp`.

**Verification:** `TestTypedSplines` in [`test_aousd_conformance.cc`](../tests/next/test_aousd_conformance.cc) (evaluate + USDC round-trip + half rounding + autoEase + declared-type preservation); OpenUSD `usdcat` opens next-written spline USDC with matching knots/tangents/extrapolation, and a pxr-authored 0.12 spline crate reads and evaluates in next.

Per-knot spline `customData` is now retained as authored USDA dictionary text and encoded/decoded using OpenUSD's trailing `unordered_map<double, VtDictionary>` Crate representation. Post-loop-extrapolation modes survive within next but are lost when re-opened in pxr because pxr's own 0.12 reader masks that field (a pxr limitation, not a next defect).

### AOUSD-TYPE-001 — Foundational declared values (were lost) (P0)

**Remediation status:** largely fixed. `frame4d` is a first-class `matrix4d`-role type whose authored value round-trips through USDA and USDC; `pathExpression` is first-class; and `opaque`/`group` declarations plus their only agreeing value, the `None` block, round-trip. The coverage matrix now includes all scalar/dimensioned families and representative alias arrays, but remains hand-maintained.

**Normative area:** §6 Foundational Data Types and format bijection.

At the baseline revision, a layer containing `frame4d frame = ...` rewrote with the declaration intact but the authored value gone. `frame4d` now preserves its 16-double value (shared `matrix4d` storage, USDC `Matrix4d` encoding); pxr reads next's `frame4d` value back byte-for-byte. `opaque` attributes round-trip as declared. `pathExpression` parses, stores, and encodes (USDC type 57, 0.10 bump).

The regression now enumerates every public `TypeId` and fails if a newly added foundational type is absent from the round-trip matrix; this caught and added the previously omitted unsigned-vector and float-matrix families. In addition, `TestNormativeTypeMatrix` GENERATES cases from the normative §6 specification table (embedded from the supplemental corpus's `foundational_data_types.json`, de-duplicated): every normative scalar and array type in the default, time-sampled, typed-dictionary (customData), and semantic-alias contexts, each asserted for declared-name fidelity and byte-identical USDA→USDC→USDA round-trip. Implementation extensions beyond the table (`matrix2f/3f/4f`, `uint2/3/4`, `pathExpression`) remain covered by the registry guard. Ordinary non-block values for `opaque`/`group` remain correctly unrepresentable.

### AOUSD-USDA-001 — Valid Unicode identifiers are rejected (P1)

**Remediation status:** fixed. `src/next/prim/identifier.hh` reuses `unicode-xid.hh`, performs strict UTF-8 decoding, and is used by the lexer and prim-name validator.

**Normative area:** §§7.3.3 and 8 identifier rules; USDA identifiers use Unicode XID semantics.

At the baseline revision, the valid prim declaration `def Xform "München"` was accepted by OpenUSD and rejected by both `next_usdcat` and legacy `lusdcat`. Remediated `next` uses validated UTF-8 plus Unicode `XID_Start`/`XID_Continue`; the legacy parser already had the shared XID utility used for this fix.

Programmatic authoring boundaries now validate: `Path::append_child` / `append_property` reject invalid components (returning an empty `Path`), `Path::is_valid()` / `Path::Parse()` expose full-grammar validation for untrusted strings, and `Layer::define_prim_at_path` rejects paths with non-XID components. The validation module's previous file-local approximation (which accepted any byte ≥ 0x80) is replaced by the shared strict validator. Raw `Path` CONSTRUCTION stays unvalidated by design — hot composition paths build Paths from already-validated parser output. Boundary cases (truncated/overlong/surrogate/lone-continuation UTF-8, U+110000, noncharacters, private-use, XID_Start vs XID_Continue) are covered in `TestUnicodeAndPaths`.

### AOUSD-PATH-001 — Invalid paths are accepted as opaque strings (P1)

**Remediation status:** fixed under `strict_aousd_conformance`. Compatibility mode deliberately preserves the previous permissive behavior.

**Normative area:** §8.3 Path Grammar.

At the baseline revision, a relationship target `</Root//Child>` loaded successfully in `next` while OpenUSD rejected it. Strict parsing now validates path-reference tokens before a stage can be returned. Programmatic authoring is now validated too: `Path::append_child`/`append_property` reject invalid components, `Path::is_valid()`/`Path::Parse()` provide full-grammar checks, and `Layer::define_prim_at_path` rejects non-XID components; raw `Path` construction stays unvalidated for hot composition paths that consume parser-validated strings.

The deeper audit added the specification's valid/invalid example table as a regression and found two boundary errors in the first validator: repeated parent traversal such as `../..` was rejected, while an absolute relational-property chain such as `/Prim.relationship.attribute` was accepted even though absolute paths use `PrimFirstPathElements` and permit at most one property element. Both are now corrected.

Permissive invalid paths can later affect namespace lookup, composition, relationship forwarding, and security policy decisions.

The canonical validator now runs at the programmatic authoring boundaries (`Path` appends, `Path::Parse`, `Layer::define_prim_at_path`) and in the validation module (replacing the byte-≥0x80 approximation), with generated boundary cases for malformed UTF-8 (truncated / lone continuation / overlong / surrogate / beyond U+10FFFF) and noncharacter / private-use / XID_Start-vs-Continue codepoints. **Remaining:** `PrimSpec::set_name` and reader-internal setters stay unvalidated by design (lexer-validated input); revisit if a public mutation API grows around them.

### AOUSD-ORDER-001 — Prim and property ordering is not preserved or composed (P1)

**Remediation status:** fixed for the reviewed USDA and next USDC paths. Authored root/child/property order is retained, applied after hierarchy construction, respected by property enumeration, and re-emitted.

**Normative area:** §8.2 path-element ordering and §11.3 ordered prim/property children.

At baseline, rewriting the OpenUSD namespace-order fixture lost root, property, and child order. `next` now retains the sparse population fields separately from natural child storage. The renewed audit found that the first USDC implementation only reordered the natural `properties` vector; it now also writes and reads the actual Crate `primOrder`/`propertyOrder` fields.

There is also an ecosystem wording issue worth separating from the implementation defect: an AOUSD example uses `reorder propertyChildren`, while the reviewed OpenUSD parser expects `reorder properties`. This remains a specification/OpenUSD grammar clarification rather than a LightUSD population-order gap.

### AOUSD-COMP-001 — Dictionaries replace instead of recursively composing (P0)

**Remediation status:** fixed for prim and property metadata composition. Nested missing keys now fill from weaker opinions while stronger leaves remain authoritative.

**Normative area:** §§6.6.2 and 12.2 dictionary field resolution.

In a two-layer probe, the weaker layer authored:

```usda
customData = { dictionary nested = { int a = 1; int b = 2 }; int weak = 3 }
```

and the stronger layer authored a stronger `nested.a` plus `strong = 4`. OpenUSD recursively composes the dictionary, retaining `nested.b` and `weak`. `next` flattening retains only the stronger dictionary.

The shared local-opinion path now recursively merges `customData`, `assetInfo`, `sdrMetadata`, clips, and typed extension dictionaries. The property copier is field-complete for both attributes and relationships: weaker `renderType`, `connectability`, `allowedTokens`, scalar metadata, raw unknown fields, and dictionary fields no longer disappear merely because a stronger property authored another metadata field. Merged unregistered dictionaries regenerate their source and survive USDC reread.

Explicit-empty authored state is now distinguishable from unauthored for layer `relocates`/`subLayers` and prim `relocates`/`variants` (dedicated authored bits, round-tripped through USDA — `relocates = {}`, `subLayers = []`, `variants = {}` — and USDC; `TestAuthoredStateBits`). Deleted/blocked and dictionary type-conflict differentials are implemented: a value → block transition reports a dedicated `blocked` diff reason, dict-vs-scalar key collisions report `<field>(type-conflict)`, and the serial compositor surfaces each shadowed weaker dictionary subtree as a `Dictionary type conflict` error naming the dotted key (`MergeWeakerDictionaryValue` conflict collector; the parallel pcp fill stays silent by design). Variant specs now carry generic unknownMeta/TypedExtensionField storage (see AOUSD-META-001).

### AOUSD-COMP-002 — Relationship list-op composition loses weaker targets (P0)

**Remediation status:** fixed for relationships and attribute connections. `PrimSpec` retains strength-ordered opinion stacks, and explicit/add/prepend/append/delete/reorder operations resolve against weaker bases before flattening to explicit target lists.

**Normative area:** §§12.2 and 12.4 list-op resolution for relationship targets and attribute connections.

With a weaker `rel r = </A>` and a stronger `prepend rel r = </B>`, OpenUSD resolves `[</B>, </A>]`. `next` flattening produces only `</B>`. When a stronger relationship already exists, the current composition path skips the weaker relationship rather than applying the list operation.

Attribute `.connect` authoring now preserves all six sublists in USDA and USDC, including cross-site prepend/delete resolution and explicit-empty state. The broader problem still generalizes to elective fields not yet registered with the shared list-op machinery; ordered and generated multi-layer combinations are not yet registry-driven.

**Remaining:** relationship results and explicit-empty `targetPaths` now round-trip, and stronger explicit-empty `apiSchemas` correctly blocks weaker applications. Both schema-name fields now have exact explicit/add/prepend/append/delete/order storage and interoperable USDC encoding, including inert and explicit-empty sublists. The reviewed cross-site API-schema delete case now resolves through the shared opinion-copy path. Cross-site `reorder` now applies with pxr SdfListOp semantics (shared `ApplyStringListOrder`, also used by clipSets/variantSet-name edits), and `test_cross_layer_string_listop_matrix` locks in every qualifier combination across a sublayer stack. Composition is now REGISTRY-DRIVEN: `src/next/layer/listop-field-table.hh` registers apiSchemas / variantSetNames / clipSets with one shared stronger-over-weaker merge (`MergeWeakerStringListOpField`), normalizing apiSchemas' legacy qualifier/vector representation to an equivalent edit; variantSetNames upgraded from fill-absent to a true list-op merge (`TestStringListOpFieldTable`).

A direct cross-site probe keeps this distinction concrete: a stronger `delete apiSchemas = ["WeakAPI"]` over weaker `prepend apiSchemas = ["WeakAPI", "KeepAPI"]` flattens to `["KeepAPI"]` in OpenUSD. Next previously produced an empty applied list; legacy and PCP composition now produce the same explicit `["KeepAPI"]` result and do not re-emit the local delete operation onto the flattened layer.

### AOUSD-COMP-003 — Non-positive sublayer scale is applied instead of rejected (P1)

**Remediation status:** fixed. Strict parsing rejects non-positive and non-finite scales; compatibility mode diagnoses them and substitutes the identity mapping, matching the reviewed OpenUSD behavior. The shared validator now independently rejects zero, negative, NaN, and infinite scales plus non-finite offsets for sublayers, reference/payload arcs, and programmatically authored prim metadata, so USDC/API construction cannot bypass the USDA parser check.

**Normative area:** §10.3.1.1 Layer Offsets.

At baseline, a sublayer offset `(offset = 10; scale = -2)` and a weaker sample at time 1 made `next` flattening move the sample to time 8. OpenUSD warns that the offset is invalid and uses the default/identity mapping, leaving the sample at time 1. The specification treats a scale less than or equal to zero as a composition error. Parser, composition, and validator regressions now cover that boundary, including non-finite API-authored values.

### AOUSD-COMP-004 — `custom` does not resolve as a logical OR (P1)

**Remediation status:** fixed for attribute and relationship flags in the shared opinion-copy path.

**Normative area:** §12.2 attribute field resolution.

Given a weaker `custom int a = 1` and stronger `int a = 2`, OpenUSD's composed result remains `custom int a = 2`; `next` emits `int a = 2`. AOUSD specifies `custom` as true if any contributing opinion is true. The composition code updates selected flags on an existing slot but does not accumulate the custom flag.

The shared opinion-copy path now accumulates `custom` independently of the winning default value. Moving all field-combination rules into generated declarative metadata remains part of schema-registry completion, not an open `custom` defect.

### AOUSD-COMP-005 — Weaker `class` did not define a stronger `over` (P1)

**Remediation status:** fixed for ordinary layer-stack and arc composition. When the strongest specifier is `over`, the strongest weaker defining opinion now supplies `def` or `class` instead of promoting only `def`.

**Normative area:** §12.2 `specifier` resolution and §11 stage population.

The deeper differential matrix found that `over + weaker class` stayed `over` in next while OpenUSD flattened it as `class`. The other simple combinations (`over + def`, `class + weaker def`, and `def + weaker class`) already matched. The paired `aousd-specifier-{strong,weak}.usda` fixtures lock all four cases.

The follow-up direct-inherit matrix also matches OpenUSD for inherited class/def alone and for weaker local def/class competing with the direct inherit. A later multi-inherit probe found an order-sensitive bug: `[ClassBase, DefBase]` stopped at class while OpenUSD resolved def in either order. Both legacy and PCP composition now treat any concrete direct-inherited definition as concretely defining when no stronger local defining specifier exists. Single and multiple specializes remain strength-ordered and match the reviewed oracle.

Implied inherit/specialize propagation now retains the namespace mapping on each expansion frame. A sub-root class is reverse-mapped into every intermediate reference stack as well as root space; the three-level nested-class regression would fail under the former root-only mapping. **Remaining:** generate deeper ancestor/specifier combinations from a declarative §12.2 resolution table.

### AOUSD-POP-001 — Stage status queries ignored ancestry (P1)

**Remediation status:** fixed for ordinary composed prim ancestry. `IsActive` requires active on the prim and all ancestors; `IsDefined` treats both `def` and `class` as defining and checks every ancestor; `IsAbstract`, `IsConcretelyDefined`, and `IsInModelHierarchy` expose the other implemented normative predicates.

**Normative area:** §11 Stage Queries.

At baseline `IsDefined()` meant only “this specifier is def,” so a class prim was reported undefined and a def below an over could be reported defined. `IsActive()` likewise returned a child's local value even below an inactive parent. The ancestry fixture covers class descendants, over descendants, and an authored active child below an inactive ancestor.

The model-hierarchy fixture covers valid group/assembly/component continuity, root components, subcomponents, and an unkinded ancestor that breaks continuity.

`UsdPrim::IsLoaded()` is now ancestry-aware and populated stages mark deferred-payload roots, so unload/reload state survives `BuildStage`. Native proxy paths and parents use instance namespace paths; broader prototype-editing APIs remain outside this reviewed query surface.

### AOUSD-VR-001 — General core value-clip resolution is absent (P0)

**Remediation status:** substantially fixed. Core `AttributeEval` parses
explicit/template clip sets, selects active clips, maps stage to clip time,
applies manifest gating, interpolates across missing clip values, loads clip
stages through a callback, and resolves clips before schema fallback. Multiple
sets now honor the separately authored/composed `clipSets` string list-op
(falling back to deterministic name order when absent), expose the selected set
in `EvalResult`, validate active indices/time mappings, preserve `clipSets`
through USDA/USDC, and cache loaded stages across queries through an explicit
caller-owned cache.

**Normative area:** §12.3.4 Value Clips.

The core stores clip metadata dictionaries but does not resolve ordinary attribute queries over clip sets. Tydra offers an optional animation-conversion bake path with a loader callback and sample cap, which is useful for rendering, but it is not stage value resolution and cannot make core queries conformant.

The AOUSD supplemental material includes basic, advanced, multiple-set, timing, and other clip cases. Those cases need a core resolver independent of Tydra.

Nested clip-bearing stages now recurse through the core resolver with a bounded
depth and an asset/property resolution stack; repeated entries produce a stable
cycle diagnostic.

The supplemental sampled-value assertions are now translated into
`tests/next/test_aousd_value_resolution.cc` (ctest
`next_test_aousd_value_resolution`; also invoked by
`run-aousd-supplemental.py --aousd-value-test`): default/timesamples
bracketing + Held interpolation, `times` jump discontinuities, out-of-range
stage-time clamping, clip-set name ordering, multi-clip `active` switching,
and LVRPS clip strength. Composition records per-property clip shadowing
(`PrimSpecMeta::clipShadowedProps`): opinions filled from sources weaker than
the clips-introducing source lose to clips, while local opinions keep
precedence. Tydra's animation bake now CONSUMES the core resolver: it parses
clip metadata once via `ParseValueClipSets` and resolves every (property,
time) sample through `ResolveValueClipFromSets` (a pre-parsed-sets overload
added for many-query callers), seeding the shared `ValueClipStageCache` from
its own loader. Tydra's former duplicate of the metadata semantics — which
had drifted (no `times` jump-discontinuity handling, stale out-of-range
mapping, no clipSets ordering edits, no manifest gating, no nested-clip
recursion) — is deleted; only the bake-specific sample-time generation
remains Tydra-side.

### AOUSD-VR-002 — Default-time queries consult time samples (P1)

**Remediation status:** fixed. `TimeQuery` explicitly distinguishes
`Default()` from `Numeric(0)`. `AttributeEval` consumes that type, and
`UsdPrim::GetPropertyValue` now performs DefaultTime resolution without
consulting samples. The former behavior remains available only through the
explicitly named `GetPropertyValueOrEarliestTimeSample` compatibility helper.

**Normative area:** §12.3 Value Resolution.

DefaultTime consults the authored default and then the schema fallback; numeric
time consults samples/splines/clips according to value-resolution precedence.
Regression coverage distinguishes DefaultTime, numeric zero, and the opt-in
earliest-sample helper.

### AOUSD-VR-003 — Interpolation coverage and held fallback are incomplete (P1)

**Remediation status:** fixed for the AOUSD §12.6 type list. Half/float/double scalars and dimensioned values, `timecode`, all three matrices, all three quaternion precisions, and held fallback are covered at exact, between, before, and after sample times. Representative semantic aliases verify declared-role preservation. Numeric array interpolation is supported as an extension but is not counted as normative §12.6 coverage.

**Normative area:** §12.6 Value Interpolation.

The implementation preserves semantic-role type IDs while interpolating their underlying numeric lanes and quantizes half values once at the result. Non-linear types now return the lower sample in linear mode rather than an empty evaluation.

### AOUSD-VR-004 — Value blocks lack schema-fallback resolution (P1)

**Remediation status:** fixed for registry-covered properties. A block no longer escapes as the consumer value; resolution proceeds to the registered schema fallback. `EvalResult::blocked` now records that suppression for both fallback-success and no-value results.

**Normative area:** §§12.3 and 13 schema fallbacks.

At baseline, `AttributeEval::EvalFromPrimSpec` could return a block as the successful consumer value and had no registry fallback. It now treats the block as suppression and continues to the registry fallback. Properties outside the compact registry still correctly resolve to no value rather than exposing the block token.

Regression coverage verifies both a blocked built-in property resolving to its
schema fallback and a blocked custom property resolving to no value.

### AOUSD-SCHEMA-001 — No general schema registry or prim-definition population (P0)

**Remediation status:** partially fixed. A shared registry now drives `HasProperty`, property enumeration, and fallback evaluation for the schemas implemented by next, including selected applied Physics APIs. The registry is intentionally compact and is not yet generated from the complete schema inventory.

**Normative area:** §§12.3.5 and 13.3–13.5.

At baseline, `next` had useful typed/convenience views but no shared registry corresponding to prim definitions. The new compact registry fixes the mechanics below for its registered schema subset; complete generated breadth remains:

- registered built-in properties are now populated consistently; unregistered schemas remain absent;
- registered fallback values are available to the core resolver; complete schema breadth is not;
- selected applied API properties are expanded, but multiple-apply namespaces are not general;
- validation distinguishes the built-in API inventory from unknown applied
  schemas, but cannot discover third-party plugin schemas dynamically;
- selected Tydra consumers hard-code defaults, producing API-dependent results.

This is a core conformance issue, not merely missing C++ wrapper classes.

**Fix needed:** generate or ingest a compact schema registry containing type inheritance, property definitions, variability, fallback values, allowed tokens, applied/multiple-apply API definitions, and field combination metadata. Make stage population, value resolution, validation, and typed views consume the same registry.

### AOUSD-REL-001 — Relationship forwarding is not exposed (P2)

**Remediation status:** implemented for ordinary composed-stage paths. `UsdPrim::GetRelationship` returns raw targets and `GetForwardedRelationshipTargets` recursively follows relationship-property targets, preserves first-seen order, removes duplicate terminal targets, and terminates cycles.

**Normative area:** §12.4 Relationships.

At baseline the core exposed only raw targets. The fixture [`aousd-relationship-forwarding.usda`](../tests/next/fixtures/aousd-relationship-forwarding.usda) now covers the normative example shape plus duplicate terminals and a cycle.

Native instance roots and proxy children now map prototype relationship targets
back into the instance namespace, including recursively forwarded relationship
properties. The API should eventually surface composition errors separately
from a valid explicit-empty result.

### AOUSD-RES-001 — Resolver behavior is filesystem-centric (P2)

**Normative area:** §9 Asset Resolution.

The resolver supports relative/absolute filesystem paths, anchors, configured search paths, deterministic recursive search, package paths, suffix fallback, custom callbacks, and explicit case-insensitive URI/IRI-style scheme handlers. Unresolved relative identifiers are anchored before cache comparison, malformed package identifiers are rejected, registered scheme identifiers are never filesystem-normalized, and in-memory registration creates stable `usd-anon:` assets that work through normal composition loaders. Composition evaluates the full SdfVariableExpression function language (`expression-variables.cc`: typed literals — int64/bool/None/lists, `${VAR}` references with recursive evaluation + cycle detection, quoted-string interpolation with escapes, and the `if / and / or / not / eq / neq / lt / leq / gt / geq / contains / at / len / defined` function set, with hard nesting/expansion caps) under the explicit disabled/evaluate/require-resolved policy. Expressions apply at reference/payload arcs, SUBLAYER asset paths (against the stack root layer's `expressionVariables`), and VARIANT SELECTIONS (against the source's composed variables); a `None` result means "no opinion" and skips the arc/sublayer/selection.

Callbacks can emulate some missing schemes, but conformance-sensitive callers need explicit identifier classification and stable resolver semantics, not implicit filesystem handling. Strict compositor, PCP cache, and layer-registry paths now disable suffix fallback per resolver call, so a permissive shared resolver cannot silently rehome an asset during strict loading.

Layer-stack identity now incorporates expression variables (pxr keys layer
stacks by (identifier, expression variables)): the referencing site's composed
variables are fingerprinted into the stack-table key (`LayerStack::cache_key`,
`InternLayerStack`/`AdoptStack`), and a referenced stack's sublayer path
expressions evaluate against the root layer's `expressionVariables` with the
inherited variables composed over them (root-most opinion wins). The same
asset referenced under two different variable contexts composes as two
distinct stacks (`test_layer_stack_identity_expression_vars`).

**Remaining fix needed:** broaden scheme-specific context objects as demanded.
Address the documented `lstat`-then-open TOCTOU window where security policy
depends on the checked path.

### AOUSD-USDC-001 — Declared 0.12 support is incomplete (P0)

**Remediation status:** substantially improved. `CrateReadOptions::strict_aousd_conformance` promotes ignored/unsupported reader warnings to errors; spline type 59, compressed arrays, sparse population order, and omitted reference prim paths now decode/encode correctly.

**Normative area:** Core File Formats, USDC version 0.12.

The compatibility reader accepts versions from 0.4 through 0.14 and the writer
defaults to 0.8, upgrading for selected features. Strict AOUSD Core 1.0.1 mode
now rejects versions newer than the standardized 0.12 profile rather than
implying support for 0.13/0.14 features. Version recognition is still not a
complete per-feature capability matrix, but the confirmed 0.12 spline and
compressed-bool gaps are closed. The deeper audit found that eager and lazy
materialization treated arrays below 16 elements as raw even when the ValueRep
compression bit was set. AOUSD only says arrays of 16 bytes or less *should
not* be compressed; the bit still defines the payload layout. Decoding now
follows the bit for every supported codec, with 1-, 8-, and 16-element
compressed integral regressions.

**Remaining fix needed:** maintain a per-feature matrix inside the accepted
0.4–0.12 window, enforced by both reader and writer. A file using an unsupported
normative feature must fail atomically. Add one fixture per normative
type/encoding and differential field-table comparisons against OpenUSD.

### AOUSD-USDC-002 — Omitted reference prim path became root path (P1)

**Remediation status:** fixed. A reference `@asset@` now stores the empty SdfPath, so the referenced layer's `defaultPrim` is used. The earlier writer stored `/`, which OpenUSD re-emitted as `@asset@</>` and changed/invalidated the arc.

**Normative area:** §10 references and the USDC `Reference` representation.

The regression covers next write/read and the paired `aousd-defaultprim-{reference,target}.usda` fixtures provide a differential composition probe.

### AOUSD-USDC-003 — Documentation used a nonstandard Crate field token (P1)

**Remediation status:** fixed. The writer now emits the core `documentation` field token at pseudo-root and prim scopes; the reader accepts both `documentation` and the older next-private `doc` spelling for backward compatibility.

**Normative area:** §§7.4, layer/prim/property `documentation`, and USDC field tables.

The deeper audit found that USDA's accepted `doc` syntax sugar had leaked into the binary writer as the actual Crate field name. OpenUSD does not apply USDA grammar aliases while reading USDC, so the metadata was not interoperable even though next-to-next round trips appeared healthy. The authored-empty regression verifies layer, prim, and property documentation through next USDA/USDC, and OpenUSD recognizes prim/property documentation in the corrected next-written crate. OpenUSD itself normalizes away authored-empty layer documentation on rewrite; AOUSD's authored-state requirement is retained as the normative expectation here.

### AOUSD-META-001 — Unknown fields were not uniformly lossless (P1)

**Remediation status:** fixed for the reviewed USDC Layer, Prim, Attribute, and Relationship scopes.

**Normative area:** §7 data model and format round-trip requirements.

`TypedExtensionField` records the field token, decoded value, and original `UnregisteredValue` source. The Crate reader stores unknown fields at all four reviewed scopes; composition fills weaker fields by token; USDA emits the authored source; and the Crate writer reconstructs OpenUSD's recursive `UnregisteredValue` wrapper. The pxr-authored four-scope fixture now passes strict load, USDC rewrite, and strict reread without losing the extension fields.

Semantic layer diffs now compare the extension fields at all three model levels. Variant specs are covered too: `VariantData` carries `unknownMeta` (raw authored text, USDA-verbatim) and `unknownFields` (typed, riding the materialized crate holder prim), and layer diffs walk variant sets per field (`meta:variantSets:<set>/<option>:<field>` reasons) instead of comparing stringified names. **Remaining:** connection/relationship-target spec categories still have no generic field home (no known producer authors fields there).

### AOUSD-META-002 — Empty authored elective fields are conflated with unauthored state (P1)

**Remediation status:** partially fixed. In addition to the earlier layer/documentation/dictionary/order/list fields, prim `kind`, `displayName`, `comment`, `instanceable`, and per-set variant selections now distinguish authored empty/false from unauthored. Strong empty values block weaker opinions and survive USDA/USDC. Empty namespace-order and explicit-empty variant-set-name spellings are not valid USDA, but pxr-authored USDC states survive read-model-write instead of becoming unauthored.

**Normative area:** §§7.4.2 authored state, 7.4.5 fallback values, and format bijection.

The data model has authored flags or exact sublist storage for the reviewed scalar fields (`defaultPrim`, `active`, `hidden`, `instanceable`, timing/unit/color metadata), prim strings, variant selections, documentation, dictionaries, namespace orders, relationship/connection clears, API schemas, and `variantSetNames`. Unmodeled generic qualified operations can still collapse to their effective/default states. USDA/OpenUSD reject some empty list/order spellings, but equivalent authored states may still arrive through USDC or authoring APIs.

**Fix needed:** give every remaining elective core field an authored-state bit (or store fields generically), and represent every explicit-empty list operation separately from unauthored/default composable operations. Generate authored-state round-trip cases from the core field table.

### AOUSD-VALID-001 — The validator is useful lint, not a conformance runner (P2)

**Normative area:** all; this finding concerns evidence quality.

The renewed checker closes several previously recorded coverage holes:

- `--composed` resolves external arcs, reports composition failures, and audits
  cross-arc attribute/property-kind type conflicts before flattening erases the
  weaker opinions;
- the `package` group checks root ordering, store/encryption/data-descriptor
  policy, 64-byte alignment, safe unique paths, portable extensions, and
  containment of authored sublayer/reference/payload/asset dependencies;
- the `crate` group performs a bounded decode and cross-validates token, path,
  field, fieldset, and spec indices for direct USDC and packaged USDC entries;
- targeted schema placement now covers SkelBinding/SkelRoot, rigid-body
  xformability, nested/static articulation roots, and point-collider policy;
- every authored variant option body is validated, including unselected and
  nested options, with `{set=option}` diagnostic locations;
- the built-in API schema inventory diagnoses unknown schemas and enforces
  known single- versus multiple-apply instance rules;
- authored relationship variability is preserved through USDA, variants,
  composition, and USDC, and `varying rel` produces the expected warning;
- authored layer `owner` is represented and round-trips through USDA/USDC,
  enabling `core.layer.owner`; authored-empty color configuration/management
  now run their corresponding validation rules.

Important unavailable checks remain:

- validator rules are compiled in rather than discovered from OpenUSD plugins.

Therefore, a clean `--validate-all` result does not establish AOUSD compliance.

**Remaining fix needed:** keep the tool labeled as semantic validation and
build a separate AOUSD conformance harness that compares composed specs,
authored fields, values, diagnostics, and format round trips. JSON reports now
distinguish `requestedGroups`, `checkedGroups`, and `skippedGroups`, and
`--require-all-groups` fails when any requested group is skipped.

## OpenUSD parity beyond AOUSD Core 1.0.1

These differences matter for interoperability with the reviewed OpenUSD revision but should not be mislabeled as AOUSD 1.0.1 violations.

### OUSD-USDC-001 — Crate `VtArrayEdit` 0.14 values are dropped (P2)

The reader recognizes newer Crate versions but does not retain OpenUSD's version-0.14 `VtArrayEdit` value. Because AOUSD Core 1.0.1 standardizes through Crate 0.12, this is an OpenUSD parity item. The same fail-closed rule should apply: accepting a newer version must not imply accepting fields that are then discarded.

### OUSD-USDC-002 — Per-reference `customData` is not retained (P2)

The Crate reader keeps a reference item but warns and skips its per-item dictionary; the writer emits an empty dictionary. AOUSD mentions this fifth field only as vestigial and explicitly says normative behavior must not depend on it, so this is OpenUSD authored-data parity rather than an AOUSD Core 1.0.1 conformance failure. Preserving it still requires a structured reference item instead of the current canonical arc string.

### OpenUSD library and service surface

| OpenUSD area | LightUSD `next`/Tydra status | Conformance significance |
|---|---|---|
| `Sdf` layer/spec/path/value model | Substantial custom equivalent | Core-relevant; exact gaps are documented above. |
| `Pcp` composition cache/index | Substantial custom PCP and legacy flattening paths | Core-relevant. Backend divergence must be reduced. |
| `Usd` stage/value/schema façade | Partial | Core-relevant where it changes population or resolution. |
| `Ar` resolver | Filesystem/package/callback subset | Core-relevant identifier semantics; plugin ecosystem is parity-only. |
| `Tf`, `Vt`, `Gf`, `Plug`, `Work`, `Trace` | Purpose-built smaller equivalents or absent | Generally API/performance parity, not automatically conformance. |
| Generated schema registry | Absent as a general shared registry | Core conformance blocker for fallbacks and applied schemas. |
| Hydra/Imaging/Storm | Tydra render-scene conversion, not a Hydra implementation | Outside AOUSD Core; product-scope difference. |
| Python bindings and `usdview` ecosystem | Separate, smaller bindings/tools | Product/API parity only. |

### Schema breadth snapshot

The reviewed OpenUSD source's `schema.usda` files declare approximately the following concrete and API schema inventory: core `usd` 7, `usdGeom` 31, `usdShade` 7, `usdLux` 21, `usdSkel` 5, `usdPhysics` 17, `usdMedia` 2, `usdMtlx` 1, `usdVol` 20, `usdRender` 5, `usdUI` 4, `usdProc` 1, `usdSemantics` 1, and `usdHydra` 1. Counts are a breadth indicator, not a compliance score.

`next`/Tydra provides meaningful native data handling for Xform, Mesh, PointInstancer, Camera, common geometry, common lights, Material/Shader and selected Preview Surface/MaterialX data, skeletal animation, physics scene/APIs/joints/collision/material, spatial audio, and project-specific AR extensions. It does not provide OpenUSD-equivalent generated wrappers or definitions for much of `usdVol`, `usdRender`, `usdUI`, `usdProc`, `usdSemantics`, `usdHydra`, or the long tail of geometry/light/API schemas.

The correct architectural target is not necessarily one handwritten C++ class per OpenUSD class. A compact generated schema registry plus generic prim/property access can close normative fallback/population gaps while typed wrappers remain demand-driven.

## Composition architecture observations

The newer PCP path has real depth: sublayers, references, payloads, variant selection, internal and external assets, deferred payload rules, inherits, specializes, relocates, implied arcs, cycles, instance keys, prototypes, and dependency invalidation are represented and tested. This is far beyond a simple overlay flattener.

There are nevertheless two observable semantics sources:

- the newer PCP cache/index path used by `next` tooling; and
- `Stage::Flatten` and legacy compositor paths, some of whose comments still declare external inherits unsupported.

The remediation deliberately placed recursive dictionaries, relationship opinion stacks, custom flags, and order metadata in the shared `Compositor::CopyLocalOpinions`/`PrimSpec` machinery consumed by PCP as well as legacy flattening. Layer-offset parser/composition behavior and validation now agree on the positive-finite scale requirement. Specifier rules and remaining generic list-op fields still need the same consolidation so public operations cannot diverge.

The renewed specifier matrices lock ordinary class/def/over, direct-inherit, order-independent multi-inherit, and reviewed specialize combinations. Implied-arc and deeper ancestor interactions still are not generated from a complete declarative §12.2 resolution table, so additional PCP-specific coverage remains necessary.

## Tydra-next feature and behavior review

Tydra is a render-ready scene conversion layer, not an AOUSD stage implementation. Its gaps should therefore be reported separately unless it overrides or masks core semantics.

### Supported conversion areas

- Meshes, points, basis curves, NURBS curves, and point instancers.
- Analytic cube, sphere, cone, cylinder, capsule, and plane conversion, including versioned cylinder/capsule handling.
- Cameras and common distant, dome, rectangle, disk, sphere, and cylinder lights.
- Material binding and selected USD Preview Surface, OpenPBR, UV texture, transform, and MaterialX-node behavior.
- Skeletons, joints, skinning, blend shapes, inherited skeleton bindings, and multiple-skeleton remapping.
- Physics annotations useful to downstream render/interaction consumers.
- Bounded diagnostics, unsupported-renderable recording, texture/colorspace handling, mesh cleanup, and animation sampling controls.

The dedicated `test_tydra_next` executable passed all reviewed tests, including render access/extraction, material parity and fallbacks, point-instancer diagnostics and visibility, MaterialX, physics annotations, curves, URDF, clip baking, skeletal parity, UV promotion, and legacy-schema parity.

### TYDRA-GEOM-001 — Unsupported renderable schemas (P2)

Hermite curves, volumes, tetrahedral meshes, and NURBS patches are explicitly unsupported renderables. Tydra records them and warns, which is preferable to an unexplained disappearance. NURBS orders and basis-curve bases outside supported subsets fall back with diagnostics.

**Fix needed:** preserve the current structured diagnostics; add converters in use-case order. Volume/field support requires a resource and shader design rather than only triangulation. For every fallback, expose whether topology or appearance changed so applications can enforce a strict conversion policy.

### TYDRA-SHADE-001 — Shader evaluation is a selected-node subset (P2)

Tydra is not an SDR/plugin shader registry. It evaluates selected Preview Surface, OpenPBR, texture, transform, and MaterialX patterns. Arbitrary `sourceAsset`, custom shader nodes, plugin lights, and filters do not have general execution semantics. Unsupported plugin lights and filters can degrade to inert or point-light behavior.

**Fix needed:** distinguish “unsupported but preserved,” “approximated,” and “failed” material/light results in the public render data. Add a strict mode that refuses approximations. If broader support is desired, use a registered node-definition/evaluator interface rather than expanding a monolithic name switch.

### TYDRA-ANIM-001 — Tydra clip baking duplicates core policy (P2) — **Fixed**

Tydra's value-clip metadata parsing, active-clip selection, and stage→clip time
mapping now come from the **core resolver** (`next/eval/value-clip.hh`:
`ParseValueClipSets` + `ResolveValueClipFromSets`) instead of a drifted local
copy (the old duplicate lacked jump-discontinuity handling, had a stale
out-of-range mapping, no `clipSets` ordering edits, no manifest gating, and no
nested-clip recursion). Tydra only retains the bake-time sampling caps as
resource policy with a truncation diagnostic: `bake_value_clips`
(`src/tydra/next/render-converter.hh`, default true) and
`max_value_clip_samples` (default 10000).

### Product-scope differences

Tydra does not attempt to replace Hydra scene indices, imaging adapters, render delegates, render products/vars/settings, procedural plugins, or the complete OpenUSD UI/semantics/volume ecosystem. These are legitimate product-scope differences, not AOUSD violations. They should be documented as such so users do not infer Hydra parity from “render scene” terminology.

## Format-specific strengths and residual risks

### USDA

Strengths include a hand-written bounded parser, useful source diagnostics, lazy arrays, broad scalar/vector/array/dictionary syntax, composition-arc parsing, string escape handling, unknown prim-metadata preservation, and extensive parse/round-trip fixtures.

Residual USDA risks are now narrower: known unsupported default values and splines are preserved in compatibility mode, strict mode rejects unsupported fields, Unicode XID is implemented, strict paths are validated, and order statements round-trip. Unknown property suffixes in compatibility mode and property metadata without a generic extension-field map can still be lossy; strict mode must remain the conformance-sensitive choice.

### USDC

Strengths include structural table decoding/encoding, lazy arrays, version gating, bounds and allocation checks, Crate dumps, malformed-input coverage, and writer upgrades for selected newer features. The reader's broad version acceptance is useful when paired with feature detection.

Residual risks include conflating “recognized version” with “fully decoded feature set,” OpenUSD-only per-reference custom data, non-interoperable private preservation of unknown layer/prim metadata, unrepresentable generic extension fields, empty authored-state conflation, and simplified authored list-op state. The byte-level checker now introspects the exposed Crate tables, and all four reviewed spec scopes diagnose field loss and fail closed in strict mode.

### USDZ

The writer closely follows the normative profile:

- entries are stored without compression or encryption;
- ZIP32 is used and ZIP64 is not emitted;
- the first entry is the root layer, normally `root.usdc`;
- file data is aligned to 64-byte boundaries;
- the end-of-central-directory comment is empty;
- asset paths and CRCs are checked/generated.

The reader scans local headers and is intentionally tolerant of archives whose central directory, CRCs, or alignment are imperfect. AOUSD permits implementations to read archives outside the writing profile, so permissiveness is not itself non-conformance. Security limits on archive size, entry count, allocation, and traversal must remain in effect.

The `lusdchecker` package group is deliberately stricter than the reader: it
validates payload CRC-32 values, EOCD/comment/disk fields, central-directory
bounds and local-header agreement, entry alignment/policy, and package
containment for asset paths found in arcs, defaults, time samples, clips, and
nested metadata dictionaries.

## Security and robustness assessment

Security is a genuine comparative strength:

- no C++ exception dependency in normal error flow;
- explicit memory and archive budgets;
- Crate bounds, table, offset, and allocation validation;
- recursion/depth/cycle limits in multiple subsystems;
- lazy data paths for large arrays;
- malformed-file and fuzz-crash regression tests;
- package path validation and stored-entry USDZ policy.

Correctness gaps can still become security-relevant. Compatibility mode's acceptance of invalid paths can bypass namespace assumptions; unsupported USDC values and resolver suffix fallback can change scene intent. Strict AOUSD parsing/Crate reading now fails closed for the reviewed cases; security-sensitive applications should additionally disable suffix fallback and report every external asset actually opened.

The resolver contains a documented check/use window around filesystem inspection and open. Where the threat model includes hostile filesystem mutation, open the file first with appropriate flags and validate the opened descriptor rather than relying on a prior `lstat`.

## Prioritized fix plan

The first remediation pass completed the loss policy, Unicode/path handling, recursive dictionaries, relationship list operations, ordering, compact schema registry, and core clip lookup described below. Remaining items are retained as the forward plan.

### Phase 0 — Prevent silent loss (completed for reviewed USDA paths)

1. Add a `LossPolicy` with a strict default for conversion/rewrite tools. Any unsupported grammar, value type, field encoding, or metadata value must fail with layer/line/field context.
2. Audit every parser warning containing “unknown,” “unsupported,” “ignored,” or “dropped.” Classify it as safely preserved, deliberate lossy mode, or fatal.
3. Add a round-trip invariant test: after load/write/reload, compare every authored spec, field, value, target, and ordering token—not only the pretty-printed stage shape.

### Phase 1 — Close P0 normative gaps (completed for the reviewed 1–9 pass)

1. Typed spline values, USDA grammar/evaluation, and the USDC 0.12 codec are complete for the reviewed cases.
2. Attribute connections now share exact explicit/add/prepend/append/delete/reorder storage and cross-site resolution with relationships; unregistered elective list-op fields remain future registry work.
3. AOUSD Core schemas are generated and completeness-checked; expanding non-core OpenUSD domain schemas remains.
4. Multiple-set/cache/nested/cycle clip cases and the external supplemental adapter are implemented; direct semantic comparison of every value-resolution assertion remains.
5. The foundational matrix is guarded by public `TypeId` enumeration, per-knot spline dictionaries round-trip, and the generated elective-field inventory prevents silent coverage drift.

### Phase 2 — Correct paths, time, and ordering

1. Extend the implemented UTF-8/XID and strict path validation to every untrusted programmatic authoring boundary.
2. Expose an explicit DefaultTime query in the older convenience API; schema fallback after blocks and AOUSD interpolation dispatch are implemented in `AttributeEval`.
3. Generalize the exact relationship/arc/`apiSchemas`/`variantSetNames` list-op records into field-registry resolution and generate cross-site composition matrices.
4. Reviewed specifier, implied sub-root/intermediate arc, layer-offset, and `custom` rules are fixed; declarative generation remains.
5. Generic typed extension fields are preserved at Layer/Prim/Property scopes and participate in semantic diffs. Remaining variant/spec categories and elective authored-state generation remain.

### Phase 3 — Make compliance measurable (adapter complete; composition ratchet active)

1. The external AOUSD Core supplemental adapter is implemented for data types, file formats, stage population, and value-resolution entry assets. Add a fixture revision manifest if a second upstream release is admitted.
2. For each case, compare LightUSD against the normative expected result and, when needed, the pinned OpenUSD oracle.
3. Produce a machine-readable manifest containing spec version, implementation revision, supported feature/version matrix, pass/fail/skip reason, and whether any lossy mode was enabled.
4. Fail a future “conformance” mode on any non-empty `skippedGroups` entry (the
   semantic checker now exposes requested/checked/skipped groups in JSON).
5. Pin both the OpenUSD source revision and the actual `usdcat` binary version; rebuild the oracle when they differ.

### Phase 4 — Broaden OpenUSD/Tydra parity by demand

1. Instance/prototype-aware relationship/property forwarding and explicit URI/anonymous resolver dispatch are complete for the reviewed paths. Add scheme-specific context objects and substitution only when required.
2. Expand schema wrappers from the generated registry rather than by hand duplication.
3. Add Tydra strict conversion and structured approximation reporting.
4. PRODUCT PARITY (partially done): the schema registry and validator now cover volume/field (UsdVol registry entries + `vol.volume.fieldRel`/`vol.fieldAsset.*` structural checks), Hermite/TetMesh/NURBS-patch property definitions, and render-settings (UsdRender registry entries + `render.*` token/relationship checks). Remaining product work is RENDERING support (Tydra converters for volumes/tet meshes/NURBS — a resource + shader design, not schema breadth) and further shader-node coverage beyond UsdPreviewSurface/UsdUVTexture/UsdPrimvarReader/MaterialX.

## Verification performed

The five directly affected AOUSD/Crate/USDC targets rebuilt and passed:

```sh
ctest --test-dir build-next \
  -R 'next_test_(aousd_conformance|crate_alloc_guard|usdc_reader|usdc_writer|usdc_roundtrip)' \
  --output-on-failure
```

Result after the continued path/forwarding/documentation/specifier/authored-state additions: **5/5 focused targets passed**. A complete rebuild of the standalone `next` tree succeeded. The former `next_test_memory_flatten` failure was a stale assertion expecting token storage for the schema-defined string `config:mtlx:version`; the property was present and correctly string-typed. The corrected regression passes. The complete current standalone tree now passes **30/30** registered tests, including validation, AOUSD conformance, Tydra, benchmark, and corpus targets.

After the subsequent 1–9 gap-fill pass, the complete standalone tree was rebuilt again and remains **30/30 passing**. New regressions cover exact connection sublists, four-scope typed extension fields and semantic diffs, recursive clip cycles, expanded schema introspection, intermediate sub-root implied arcs, instance property/relationship forwarding, retained loaded state, recursive resolver search/context identifiers, registry-enumerated foundational types, and spline knot `customData`. The native checker target was rebuilt against the same sources and all **18/18** `lusdchecker_*` tests pass.

The continuation pass then added registered scheme dispatch plus `usd-anon:` memory assets, field-complete attribute/relationship metadata composition, recursive typed-extension dictionaries with regenerated interoperable wrappers, raw unknown-field fill, authored-empty prim strings, and authored-empty variant selections. A third complete rebuild remained **30/30 passing**, and the checker rebuild remained **18/18 passing**.

The generated-coverage/supplemental pass adds 13 generated AOUSD Core schema properties, a 75-field elective coverage table, typed `displayGroupOrder` and property `comment`, policy-controlled expression-variable asset paths, C-style USDA separator comments, and tolerance for format-defined sample/type disagreement. The December 2025 supplemental baseline is:

| Supplemental group | Result |
|---|---:|
| Foundational data bridge | pass (2 upstream JSON groups plus the full internal type matrix) |
| File formats | **54/54 pass** (11 USDA and 43 USDC assets) |
| Value-resolution entry layers | **8/8 load/compose pass** |
| Composition expected paths | **138/138 pass; 0 measured gaps** |

Configure the optional full gate with `-DLIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT=/path/to/core-spec-supplemental-release_dec2025`. The ordinary tree does not require or download this external corpus; `next_aousd_generated_tables` always checks the committed generated tables.

With that external path configured, the complete standalone regression run passes **36/36** CTest targets (all `add_test` entries in `src/next/CMakeLists.txt`, including the corpus and supplemental gates). The composition ratchet (`--max-composition-fail`) is now **0**: every supplemental composition case must pass, so any file-format, data-type, value-entry, or composition regression fails the gate.

The ratchet reached 0 in three reductions: 36 → 22 → 0. The first pass fixed uppercase USDA booleans, empty internal arcs selecting the layer-stack `defaultPrim`, instance-proxy lookup without subtree duplication, invalid/conflicting relocate rejection, chained post-relocation source paths, composed-namespace internal references with cycle bounds, implied-class selection strength, and variant-option selections targeting sibling variant sets.

The final pass closed the last 22 by fixing four structural defects in `src/next/pcp`:

1. **Relocates are a per-layer-stack namespace edit, not a root-layer one.** They were collected from the root layer stack only and applied as a rename during `BuildStage`. Each `LayerStack` now carries its own resolved edit tables (`StackRelocates`, built in `InternLayerStack`), and relocation is resolved while deriving a prim's composition sources (`DeriveChildSources`): a departed child contributes nothing, a relocated-in child is pulled from its pre-relocation site — whose ancestors inside that stack may carry the arcs that actually deliver it (`SourcesForRelocateSource`). A relocate authored in a referenced/payloaded/variant layer stack therefore renames prims inside that stack's namespace and the rename maps through the arc into root space, including chained relocates and relocation to a new root prim.
2. **Namespace mappings are prefix-map SETS**, not a single (source, target) pair (pxr's `PcpMapFunction`). One pair per arc plus one per relocate, matched longest-prefix-first, so a rename under a reference composes with it — and a class arc reached through a reference still resolves sibling paths of its own namespace (the implied-class site) through that reference.
3. **Arc targets compose with their own ancestral arcs in the target layer stack.** `ProcessArc` seeded a bare `(stack, site)`, so a sub-root reference (`@char.usd@</Char/Sub>` where `/Char` itself references another layer) silently lost everything the ancestor's arcs delivered. Target sources are now derived from the target's parent chain in that stack, which also resolves targets that are relocate destinations. Variant selections still resolve in the *referencing* context.
4. **Implied class arcs need the whole arc chain.** The chain was re-seeded per child prim as `{root, current stack}`, dropping intermediate stacks; it now rides along with the source (`Src::arc_chain`), so an override authored on a class path in any intermediate layer stack composes.

Relative class-arc targets (`inherits = <../_Y>`) now resolve against the authoring prim, and unselected variant sets fall back to USD's registered global fallbacks (`CompositionOptions::variant_fallbacks`, defaulting to `standin → render, proxy`), which is what pxr's museum runner supplies. Regressions for all of this live in `tests/next/test_pcp.cc` (`test_relocates_in_referenced_layer_stack`, `test_relocate_to_new_root_prim`, `test_relative_class_arc_target`).

The native legacy USDA parser/round-trip pair was also rerun after placing the two new regressions under `tests/next/fixtures` rather than widening the shared legacy corpus. It still reports the four pre-existing unexpected files (`aousd-namespace-order.usda`, `aousd-unknown-property-metadata.usda`, `rel-inherits-none-001.usda`, and `token-string-escapes.usda`); neither new next-only fixture appears in that failure set.

A later rebuild of the current legacy library confirms that
`rel-inherits-none-001.usda` and its Pixar-authored USDC counterpart retain both
`inherits = None` and `rel r = None` and compare equal to Pixar. The old binary
used for the earlier comparison predates that preservation fix. Empty `[]`
list-op spelling is also semantically equivalent to `None` under AOUSD; the
comparator now recognizes that equivalence without accepting an unassigned
relationship or an attribute value block as an empty list. The native
`usdc_reader_prim_metadata_roundtrip_test` covers both spellings.

The corpus-labelled gate also passed:

```text
files: 280
pass: 273
warn: 7
fail: 0
timeout: 0
crash: 0
```

Six warning-bearing files concern preserved unknown prim metadata named `hide_in_stage_window` and `no_delete`; the seventh now exposes a previously silent unknown layer field named `id`. The post-remediation corpus therefore reports **273 pass / 7 warning / 0 fail / 0 timeout / 0 crash** over 280 files.

The `web/build_ninja` WASM next-core/Tydra module also rebuilt successfully. Direct `test_tydra_next` execution passed its complete registered suite. OpenUSD opened a raw next-written omitted-path reference as `@target.usda@` (without `</>`), and OpenUSD and next produced the same composed marker value for the paired defaultPrim fixtures. OpenUSD also opened the next-written namespace-order crate and retained all three sparse reorder fields. These results demonstrate a healthy regression baseline, but they do not contradict the remaining conformance findings.

### Differential probe summary

| Probe | LightUSD `next` | OpenUSD `usdcat` | Expected action |
|---|---|---|---|
| Unicode prim `München` | **Now accepts in strict and compatibility modes** | Accepts | Fixed with shared XID tables. |
| `spline_basic.usda` rewrite | Parses/evaluates typed splines; USDC type-59 round-trips | Typed splines retained | Fixed for reviewed cases. |
| `frame4d` matrix value | Typed role value survives USDA/USDC | Typed value retained | Fixed. |
| Namespace reorder fixture | Retains root/property/nameChildren order through USDA and USDC | Order retained/applied | Fixed with distinct Crate order fields. |
| Relationship target `</Root//Child>` | Compatibility accepts; **strict mode rejects** | Parse error | Fixed under conformance policy. |
| Sublayer scale `-2`/NaN | Strict rejects; compatibility warns and uses identity | Warns and uses default mapping | Fixed. |
| External reference with omitted prim path | Preserves empty path and target `defaultPrim` semantics | Uses target `defaultPrim` | Fixed; `</>` is no longer synthesized. |
| Relationship forwarding chain + cycle | Returns ordered, deduplicated terminal targets, terminates cycles, and remaps prototype targets to instance paths | Same reviewed ordinary-stage result | Fixed for ordinary and native instance-proxy paths. |
| Authored-empty documentation | Layer/prim/property authored state survives USDA and next USDC | Prim/property survive; reviewed OpenUSD normalizes empty layer documentation away | LightUSD now follows the normative authored-state requirement; Crate uses `documentation`, not USDA's `doc` alias. |
| Authored-empty `defaultPrim` | Preserved through USDA/USDC and blocks a weaker defaultPrim | Preserved through USDA/USDC | Fixed with explicit layer/stage authored state. |
| Authored-empty namespace order | Pseudo-root `primOrder` plus prim `primOrder`/`propertyOrder` survive USDC | OpenUSD authoring API preserves all three in USDC; USDA rejects empty reorder syntax | Fixed for the representable USDC path; fixture is stored as deterministic hex. |
| `variantSetNames` list-op matrix | Explicit-empty, explicit, add, prepend, append, delete, order, and mixed sublists survive OpenUSD-USDC → next → OpenUSD-USDC | Sdf reports the same authored sublists | Fixed for layer read/write; explicit-empty has no valid USDA spelling and generic cross-site composition remains. |
| `apiSchemas` list-op matrix | Explicit-empty, explicit, add, prepend, append, delete, order, and mixed sublists survive OpenUSD-USDC → next → OpenUSD-USDC | Sdf reports the same authored sublists | Fixed for layer read/write; USDA uses canonical `None` for explicit-empty. Same-site semantics match Sdf; ordered/generated cross-site matrices remain. |
| Strong API-schema delete over weaker prepend | Flattened explicit list is `["KeepAPI"]` | `["KeepAPI"]` | Fixed in the shared legacy/PCP opinion-copy path; ordered cross-site matrices remain. |
| Unknown Layer/Prim/Attribute/Relationship extension fields in USDC | Compatibility warns with field names; strict load fails | Fields retained | Loss/ambiguity is explicit at every reviewed spec scope; generic storage remains. |
| Compressed bool/integral arrays, including 1/8/16 elements | Compression bit selects compressed layout; booleans canonicalized | Decodes | Fixed; writer size advice is not treated as reader grammar. |
| Nested `customData` layers | **Recursively merged** | Recursively merged | Fixed. |
| Weaker target + stronger prepend | **Both targets, correct order** | Both targets, correct order | Fixed, including a three-site regression. |
| Weak `custom`, strong non-custom | **`custom` remains true** | `custom` remains true | Fixed. |
| Strong `over`, weaker `class` | Resolves to `class` | Resolves to `class` | Fixed for ordinary layer-stack composition. |
| Local def/class versus direct inherited def/class | Four reviewed combinations match | Same results | Basic direct-inherit specifier matrix locked; complex multi-arc cases remain. |
| Class/over/inactive ancestry queries | Defined/abstract/concrete/active include all ancestors | Ancestry-sensitive predicates | Fixed for ordinary composed paths. |
| Bare relationship vs explicit-empty targets | Bare declaration remains bare; `None`/`[]` normalize to authored `None` through USDC | Distinct authored states | Fixed. |
| Strong explicit-empty `apiSchemas`, weaker applied API | Composed result remains explicitly empty through USDA/USDC | Explicit list blocks weaker list | Fixed; USDA output is canonical `apiSchemas = None`. |
| Empty layer/prim/property dictionaries | Authored state survives USDA and USDC | Preserved at all three scopes | Fixed; OpenUSD reads next-written empty dictionaries. |
| Empty color configuration/management | Authored empty asset/token survives USDA and USDC | Preserved | Fixed; OpenUSD reads the next-written crate fields. |
| Multiple direct inherits containing class + def | Resolves def in either list order | Resolves def in either order | Fixed in legacy and PCP composition. |
| Multiple specializes containing class + def | Remains strength/order driven | Same | Reviewed behavior retained; no def-preference applied. |

## Suggested regression tests

Each fixed finding should have four layers of coverage where applicable:

1. typed unit test of the data/field algorithm;
2. USDA parse/write/reparse authored-field equality;
3. USDC read/write/re-read authored-field equality at the normative version;
4. OpenUSD differential comparison of the composed/squashed result.

At minimum add named tests for:

- every Unicode identifier class and invalid UTF-8;
- every valid/invalid AOUSD path production;
- all foundational scalar and array types;
- spline interpolation, tangents, loops, extrapolation, and malformed input;
- nested dictionary strength and blocks;
- every list-op operation and multi-layer combination;
- `custom`, variability, specifier, and type-name field rules;
- ancestry-sensitive active/defined/abstract/concretely-defined stage queries;
- child/property ordering with deletes, relocates, and variant contributions;
- positive, zero, negative, NaN, and infinite layer scales;
- DefaultTime versus numeric zero;
- interpolation dispatch for every registered type (AOUSD linear/held core matrix is now covered; retain this for generated completeness);
- value clips with manifests, multiple sets, offsets, gaps, and cycles;
- schema fallback, single-apply, and multiple-apply API schemas;
- USDC 0.8 through 0.12 feature matrices and unsupported-newer-feature failure;
- compressed-flag arrays across every supported integral/floating codec and small payload size;
- OpenUSD-only reference `customData` round trips;
- strict-loss mode for every currently ignored warning.
- relationship forwarding order, duplicates, cycles, errors, and instance/prototype namespaces;
- authored-empty state for every elective layer/prim/property field.

## Known review limitations

- The LightUSD working tree contains unrelated untracked `.tmp-*` files; they were not read as review evidence or modified. The reviewed code baseline and remediation working tree are identified separately above.
- The former MaterialX memory-flatten gate was a stale test type assertion
  (`token` versus the schema-defined `string`) and is now corrected.
- The OpenUSD source checkout and installed comparison binary are not proven to be the same build: source describes `v26.03-278`, while installed headers report version 26.05. This was recorded rather than hidden; conformance tests should rebuild and pin one oracle.
- The review used targeted source inspection, the existing test suites, and focused differential probes. It is not an assertion that every OpenUSD API or every AOUSD sentence was exhaustively exercised.
- The AOUSD supplemental adapter measures parse/load and expected composed-path coverage, but it does not constitute an official certification or import every sampled-value assertion.
- OpenUSD behavior is treated as the expected oracle only when the normative text is ambiguous, consistent with the specification's guidance. A confirmed difference from newer OpenUSD alone is labeled separately.

## Final assessment

LightUSD `next` is already credible as a bounded, portable USD ingestion and rendering foundation, especially for controlled content profiles. Its USDZ writer, broad Crate machinery, composition graph work, diagnostics, and Tydra conversion coverage are notable strengths.

For arbitrary AOUSD Core 1.0.1 content, the current implementation remains **profile-based, not fully conformant**. The renewed passes closed the reviewed defaultPrim-reference and authored-empty defaultPrim, population/order, AOUSD Core generated schemas, generated elective-field inventory, expression asset paths, `apiSchemas`/`variantSetNames` authored sublists, compression-bit, interpolation, relationship-forwarding, documentation/dictionary/color fields, reviewed specifier matrices, ancestry/model queries, explicit-empty relationship/API-schema, and strict all-spec-scope field-loss gaps. The supplemental composition delta is now closed as well (138/138; per-layer-stack relocates, prefix-map sets, ancestral arc targets, and full implied-class chains). The measured critical path is now direct sampled-value oracle comparison, non-core OpenUSD schema breadth, the general variable-expression function language, and registry-driven cross-site composition for unregistered list-op fields.

### Sampled-value differential follow-up

The pinned supplemental expected-value assertions are now paired with a
304-query OpenUSD differential grid: DefaultTime, exact/intermediate samples,
held/linear interpolation, out-of-range times, and clip transition neighbors.
This found and fixed held interpolation being replaced by linear interpolation
inside clips, including nested clips and interpolation of missing clip values.
Six clip-timing queries retain the pinned AOUSD corpus's expected extrapolation
instead of OpenUSD's clamping and are reported as explicit oracle differences.
The grid has zero unexpected mismatches. This is targeted sample coverage,
not an exhaustive conformance claim.

The generated elective-field inventory also drives 30 authored-state cases
through USDA and USDC; typed empty/default metadata must remain different from
an absent opinion. Structural type and namespace-order cases remain in their
existing specialized matrices.
Unknown-metadata coverage adds 39 accepted raw opinions and 15 rejected qualified
layer spellings. It caught lost `add` qualifiers in prim/variant metadata and an
inferred variant-set list incorrectly encoded as explicit in USDC; both now
preserve their authored/composable form.
