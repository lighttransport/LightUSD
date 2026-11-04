# LightUSD Verification Plan against AOUSD Core Spec v1.1.0

**Spec**: USD Core Specification v1.1.0 (e8f44c0), AOUSD Core Spec Working Group,
vendored verbatim at [core/1.1.0/core_spec.md](core/1.1.0/core_spec.md). The
phase plan below was written against v1.0.1 (b7bc21a, 2025-12-12; vendored at
[core/1.0.1](core/1.0.1/core_spec.md)) and still applies; the v1.1.0 deltas are
audited in [the section below](#v110-delta-audit-2026-11-04).
**Target**: LightUSD (this repository)
**Supplemental corpus**: `aousd/core-spec-supplemental-public` tag
`release/1.0.1.post0` (c15ae0c) — still the newest corpus release; upstream
publishes no 1.1 corpus yet.

## v1.1.0 delta audit (2026-11-04)

Each normative change in the v1.1.0 changelog was checked against OpenUSD
26.11 (the spec correlates with OpenUSD 25.08) on purpose-built layers.

| v1.1.0 change | Result |
|---|---|
| `timeCodesPerSecond` auto-scaling across layer stacks (sublayer, reference + offset, `framesPerSecond` fallback) | Already conformant; values identical to pxr |
| Value clip `times` out-of-range clamp | Already conformant |
| Time sample query outside the authored range (hold) | Already conformant |
| Variability does not affect value resolution | Already conformant |
| Type-mismatch boundary (strongest opinion) | Already conformant |
| Quaternion ordering (USDA `(w,i,j,k)`, crate `(i,j,k,w)`) | Already conformant (USDA->USDC->pxr round trip identical) |
| Crate 0.13.0 (spline tangent algorithms): read, and write only when needed | Already conformant |
| Duplicate keys in crate variant-selection maps / dictionaries | **Fixed**: variant selection maps now keep the last value (+warning); dictionaries already did |
| Spline grammar: tangent optional after any `post` mode | **Fixed**: a bare `post curve` left the spline unevaluable |
| Splines: linear extrapolation slope from the adjacent segment | **Fixed** |
| Splines: loop reset at the period boundary | **Fixed** |
| Splines: inner loops (`LoopParameters`) | **Fixed**: were parsed but never evaluated |
| Splines: evaluation-time anti-regression must be Keep Ratio | **Fixed** (exact non-regressive limit; pxr stops ~1e-5 short, so values agree to ~1e-5) |
| Piecewise-linear approximation | **Implemented** (optional feature): `SampleSplineData` (src/next/types/spline.hh) — polylines split at value discontinuities and value-block gaps like pxr `TsSpline::Sample`; covers inner loops and every extrapolation mode; vertical deviation <= tolerance (pxr bounds perpendicular distance in scaled space, so its vertical error can exceed it near steep slopes). Also tightened the Bezier time solve near the Keep Ratio vertical tangent |
| Native instancing: ignored non-arc specs/arcs under an instance, ancestral arcs, implied-arc sharing (spec examples 1-3 + variants) | Already conformant: prototype sharing, flatten and instance-proxy values identical to pxr; stable cases added as `tests/usda/aousd11-instancing-*` gate fixtures |
| Schema families and versioning | **Implemented**: `ParseSchemaFamilyAndVersion` (spec grammar; agrees with OpenUSD except `_1`, which pxr splits as family `''`), and schema manifests reject a versioned schema inheriting its own family |

Typed/applied schema-definition rules and the UI-hint relocation (`uiHints`)
were audited separately in
[doc/validation/aousd11-schema-defs-uihints-2026-11-04.md](../doc/validation/aousd11-schema-defs-uihints-2026-11-04.md)
(26 rules: 12 agree, 8 fixed, 3 unspecified/OpenUSD-deviates, 3 n/a).

Pinned by `TestSplineCore11Parity` and `TestSplinePiecewiseLinear` (tests/next/test_aousd_conformance.cc; the latter holds pxr 26.11 `Sample` output as expected data).

**Spec erratum found:** the LZ4 framing table now gives "Number of Chunks:
8 bytes" and "Chunk Size: 1 byte", and the prose says a 64-bit chunk size.
OpenUSD's `TfFastCompression` (and LightUSD) use a **1-byte** chunk count and
an **int32** per-chunk size. LightUSD keeps the OpenUSD layout.

---

## Compliance Scope (Spec Section 4.4)

A compliant implementation must pass conformance tests in three areas:

1. **Composed Stage Population** -- produce the correct set of scene graph objects and authored field values from a set of root layers
2. **Value Resolution** -- correctly resolve metadata, attribute, and relationship values at specified times
3. **Format Implementations** -- read/write usda, usdc, usd, and usdz producing the same specs and values as the conformance tests

---

## Phase 1: Foundational Data Types (Spec Ch 6)

### 1.1 Scalar Types (6.2)
| Spec Type | LightUSD Type | Verify |
|-----------|---------------|--------|
| `bool` | `value::bool` | roundtrip usda/usdc |
| `uchar` | `value::uchar` | roundtrip |
| `int`, `uint` | `value::int32`, `value::uint32` | roundtrip |
| `int64`, `uint64` | `value::int64`, `value::uint64` | roundtrip |
| `half` | `value::half` | roundtrip, IEEE 754-2008 precision |
| `float` | `value::float` | roundtrip, IEEE 754 |
| `double` | `value::double` | roundtrip, IEEE 754-1985 |
| `string` | `std::string` | UTF-8, no C0/C1 controls |
| `token` | `value::token` | hash/equality amortized constant time |
| `asset` | `value::asset` | UTF-8, variable substitution deferred |
| `timecode` | `value::timecode` | unitless f64 |

**Test**: unit tests parsing each scalar type in usda and usdc, verifying exact roundtrip.

### 1.2 Dimensioned Types (6.3)
- `float2..4`, `double2..4`, `half2..4`, `int2..4`
- `matrix2d`, `matrix3d`, `matrix4d`
- `quatd`, `quatf`, `quath` -- (i,j,k),r ordering, unit length advisory
- Row-major matrix layout; row-vector pre-multiplication `v_t = v * S * R * T`

**Test**: matrix roundtrip; quaternion (i,j,k,r) component order; matrix indexing [row,col].

### 1.3 Semantic Aliases (6.5)
- `color3f/d/h`, `color4f/d/h`, `normal3f/d/h`, `point3f/d/h`, `vector3f/d/h`
- `texCoord2f/d/h`, `texCoord3f/d/h`, `frame4d`
- `group` (opaque)
- **Type agreement**: `color3f` agrees with `float3` but they are not equivalent

**Test**: parse semantic aliases from usda; verify `typeName` stored separately from underlying type; verify agreement rules.

### 1.4 Container Types (6.6)

#### Arrays (6.6.1)
- `T[]` for any scalar/dimensioned type
- Empty arrays permitted; arrays of `opaque` not permitted
- Semantic alias arrays: `color3f[]` agrees with `float3[]`

#### Dictionaries (6.6.2)
- `dictionary` -- unordered map<string, heterogeneous value>
- Nested dictionaries supported
- Must NOT store semantic aliases or list operations as values
- **Combining** (union): recursive merge with stronger-wins on key conflicts

#### List Operations (6.6.3)
- Types: `listop<int>`, `listop<int64>`, `listop<uint>`, `listop<uint64>`, `listop<string>`, `listop<token>`, plus `listop<ObjectPath>`
- Explicit vs composable list ops
- **Combining** (section 6.6.3.6): explicit wins over composable; composable merge rules
- **Reducing** (6.6.3.8): remove cross-sequence duplicates
- **Chaining** (6.6.3.9): associative but not commutative
- **Deprecated**: `reorder` and `add` operations -- not normatively specified

**Test**: list op combining/chaining unit tests; dictionary recursive merge; deprecated `add` -> `append` migration.

---

## Phase 2: Document Data Model (Spec Ch 7)

### 2.1 Spec Forms (7.3)
- Layer Spec, Prim Spec, Attribute Spec, Relationship Spec, Variant Set Spec, Variant Spec
- Hierarchy semantics: unique absolute paths; parent-child containment
- Property specs are always leaf elements

### 2.2 Spec Names (7.3.3)
- PrimName: `([XID_Start] / '_') [XID_Continue]*`
- VariantSetName: same as PrimName
- VariantName: dot/minus/pipe/XID_Continue sequences
- PropertyName: PrimName-like with optional `:namespace:component`

**Test**: parse valid/invalid identifiers per PEG grammar; Unicode XID classes.

### 2.3 Metadata Fields (7.4)
- FieldName grammar: `([XID_Start] / '_') [XID_Continue]*`
- No semantic aliases in metadata field types (7.4.2.1)
- Core specialized types: `ObjectPath`, `Spline`, `References`, `Payloads`, `Retiming`, `Relocates`, `VariantValue`, `TimeSamples`, `EnumVariability`, `EnumSpecifier`
- Cubic splines (7.4.2.4): bezier/hermite curves; SplineKnot structure; interpolation modes (none/held/linear/curve); extrapolation modes (none/held/linear/sloped/looprepeat/loopreset/looposcillate)

### 2.4 Core Metadata Fields (7.6)
Verify presence and correct types for all core fields on each spec form:

**Layer Spec fields**: `subLayers`, `subLayerOffsets`, `defaultPrim`, `upAxis`, `metersPerUnit`, `timeCodesPerSecond`, `startTimeCode`, `endTimeCode`, `framesPerSecond`, `documentation`, `customLayerData`, `colorConfiguration`, `colorManagementSystem`, `owner`, `hasOwnedSubLayers`, `expressionVariables`, `layerRelocates`

**Prim Spec fields**: `specifier` (def/over/class), `typeName`, `kind`, `active`, `documentation`, `customData`, `comment`, `hidden`, `references`, `payload`, `inheritPaths`, `specializes`, `variantSetNames`, `variantSelection`, `instanceable`, `apiSchemas`, `sdrMetadata`

**Attribute Spec fields**: `typeName`, `variability` (varying/uniform), `custom`, `default`, `timeSamples`, `spline`, `connectionPaths`, `allowedTokens`, `colorSpace`, `documentation`, `customData`, `comment`, `hidden`, `displayName`, `displayGroup`, `sdrMetadata`

**Relationship Spec fields**: `variability`, `custom`, `targetPaths`, `documentation`, `customData`, `comment`, `hidden`, `displayName`, `displayGroup`, `sdrMetadata`

**Test**: for each spec form, create a usda with all core fields authored; parse and verify all fields roundtrip.

---

## Phase 3: Paths (Spec Ch 8)

### 3.1 Path Grammar (8.3, 8.5)
- Absolute paths: `/Foo/Bar.propName`
- Relative paths: `../Foo`
- Variant selections in paths: `/Foo{vset=variant}/Bar`
- Property paths: `/Foo.bar`, `/Foo.ns:attr`
- Target paths (mapper/expression): `/Foo.bar[/Target.src]`
- Empty path, "no path"

### 3.2 Element Ordering (8.2)
- Properties before child prims
- Namespace-grouped properties ordered by declaration
- Implementation must track and preserve authored ordering

### 3.3 Compatibility with Legacy Content (8.7)
- Relational attributes (deprecated), mapper paths, expression paths

**Test**: parse all path forms from spec examples; roundtrip through usda writer; verify element ordering preserved.

---

## Phase 4: Resource Interface (Spec Ch 9)

### 4.1 Resource Identifiers (9.2)
- Asset paths as URIs/IRIs per RFC 3986/3987
- Relative resource identifiers (9.4)
- `file` scheme (RFC 8089)

### 4.2 Packaged Resources (9.7)
- USDZ archive resolution: `archive.usdz[path/inside]`

### 4.3 Security Considerations (9.10)
- URI/IRI security per RFC specs

**Test**: relative path resolution; USDZ internal path resolution; variable substitution in asset paths.

---

## Phase 5: Composition (Spec Ch 10)

This is the most complex area. LightUSD has composition support in `src/composition.{hh,cc}`.

### 5.1 Composition Algorithm (10.2)
- Input: root layer, prim path, payload flag
- Output: strength-ordered list of (layer, spec path) opinions
- Recursive ancestral composition

### 5.2 Composition Operators (10.3)

#### Sublayers (10.3.1)
- Layer stack construction from `subLayers` field
- Sublayer time offsets: `offset` and `scale` (scale > 0, default offset=0, scale=1.0)
- Cycle detection: ignore cyclic sublayers

#### References (10.3.2.1)
- Asset path + prim path tuple
- Internal references (no asset path -> same layer stack)
- `defaultPrim` fallback when no prim path specified
- Namespace mapping: `[(/A, /B)]` for external, `[(/A, /B), (/, /)]` for internal
- Reference time offsets
- Error handling: missing layer stack or no specs -> ignore

#### Payloads (10.3.2.2)
- Same as references but conditionally loaded/ignored
- `payload` field instead of `references`

#### Inherits (10.3.2.3)
- `inheritPaths` field
- Composed across the layer stack where the arc is introduced + all upstream layer stacks
- **Implied inherits**: propagate through all upstream layer stacks to root
- Namespace mapping: same as references plus identity mapping

#### Specializes (10.3.2.4)
- Same mechanism as inherits via `specializes` field
- **Globally weaker** strength ordering (see 10.4.1)

#### Variants (10.3.2.5)
- `variantSetNames` + `variantSelection` dictionary
- Variant selection computed strong-to-weak
- Deferred evaluation (after all other arcs evaluated)
- Namespace mapping: identity `[/, /]`

#### Relocates (10.3.2.6)
- `layerRelocates` field on layer spec
- Map source path to target path in namespace
- Restrictions: no pseudo-root; no ancestor/descendant conflicts; 1:1 mapping
- Namespace mapping composition with other arcs

### 5.3 Strength Ordering -- LIVERPS (10.4)
- **L**ocal, **I**nherits, **V**ariants, **R**elocates, **R**eferences, **P**ayloads, **S**pecializes
- Same layer stack: stronger layer wins
- Cross layer stack: local > remote
- Same arc type, same layer stack: deeper namespace > shallower; authored > implied; earlier in list > later
- **Specializes exception**: globally weaker than all other opinions

**Test strategy**: reproduce all 6 composition examples from spec section 10.4.2:
1. Sublayer strength ordering
2. Composition arc ordering (LIVERPS)
3. Namespace depth strength ordering
4. Implied vs authored ordering
5. Multiple arc list ordering
6. Specializes global weakness

### 5.4 Namespace Mappings (10.5)
- Each composition arc produces a namespace mapping
- Mappings compose when arcs are nested

### 5.5 Composition Errors (10.6)
- Graceful handling: cyclic sublayers, missing layers, invalid relocates

---

## Phase 6: Stage Population (Spec Ch 11)

### 6.1 Populating the Stage (11.3)
- Compose root prim, then recursively compose children
- `primChildren` ordering metadata
- Prim child discovery across composed opinions

### 6.2 Scene Graph Model Hierarchy (11.4)
- `kind` metadata: `component`, `assembly`, `group`, `subcomponent`, model hierarchy rules
- `instanceable` prims and instancing

### 6.3 Stage Queries (11.5)
- Active, Loaded, Model Hierarchy, Abstract, Defined, Instance predicates

**Test**: multi-layer stage with sublayers/references; verify composed prim set matches expected; verify `kind` hierarchy; verify `active`/`defined`/`abstract` predicates.

---

## Phase 7: Value Resolution (Spec Ch 12)

### 7.1 Metadata Resolution (12.2)
- Strongest opinion wins (default)
- **specifier**: special rules -- `over` means all opinions are `over`; `class` vs `def` rules
- **typeName**: determined from prim definition, not strongest opinion
- **variability**: from prim definition; fallback = weakest opinion
- **custom**: true if ANY opinion in stack says true
- **Dictionaries**: recursive merge (6.6.2.1)
- **List ops**: chained combination across opinion stack
- **Layer metadata**: root layer spec only, no composition

### 7.2 Attribute Resolution (12.3)
- Priority: `timeSamples` > `spline` > `default` > `clips` > fallback
- At default time (no time specified): use `default` field
- At specific time: check timeSamples, then spline, then default, then clips
- **Blocked values**: stop iteration, return fallback
- **Layer offset/scale** applied to time queries

### 7.3 Time Samples (12.3.2)
- Interpolation between knots: held or linear
- Extrapolation: clamp to nearest sample

### 7.4 Spline Evaluation (12.3.3)
- Bezier/Hermite curves
- Knot structure with pre/post tangents
- Inner loops, extrapolation modes (7 modes)

### 7.5 Value Clips (12.3.4)
- Clip sets (explicit and template metadata)
- `assetPaths`, `active`, `times`, `primPath`, `manifestAssetPath`
- Template: `templateAssetPath`, `templateStartTime`, `templateEndTime`, `templateStride`, `templateActiveOffset`
- Active clip determination, stage time -> clip time mapping
- `interpolateMissingClipsValue`
- Strength: just weaker than Local in LIVERPS
- Manifest-based attribute discovery

### 7.6 Relationships and Connections (12.4)
- `targetPaths` / `connectionPaths` list op resolution
- Forwarded relationships (deprecated)
- Attribute connections: provide value source

### 7.7 Interpolation (12.5)
- Held: constant between samples
- Linear: lerp for numeric types
- Not applicable: string, token, bool, asset, opaque, relationship targets

**Test**: create multi-layer stages with competing default/timeSamples/spline values; verify resolution priority; test value clips with explicit and template metadata; test blocked values; test layer offset/scale on timeSamples.

---

## Phase 8: Schemas (Spec Ch 13)

### 8.1 Schema Types (13.3)
- IsA schemas (typed prims) vs API schemas (applied behaviors)
- Single-apply vs multi-apply API schemas
- `apiSchemas` listop on prim spec
- Schema inheritance

### 8.2 Core Schema Types (13.4)
- Verify LightUSD built-in schema types match spec-defined core schemas

**Test**: parse prims with `apiSchemas` applied; verify schema-defined fallback values.

---

## Phase 9: Color (Spec Ch 14)

### 9.1 Color Spaces (14.1)
- Supported: `lin_srgb`, `srgb`, `lin_rec709`, `rec709`, `lin_ap0`, `lin_ap1`, `ace_cg`, `g22_rec709`, `g22_ap1`, `lin_adobergb`, `adobergb`, `lin_displayp3`, `displayp3`, `lin_srgb_texture`, `srgb_texture`, `raw`

### 9.2 Core Metadata Extensions (14.2)
- `colorConfiguration` and `colorManagementSystem` on layer spec
- `colorSpace` on attribute spec

**Test**: parse `colorSpace` metadata on attributes; verify known color space tokens.

---

## Phase 10: Collections (Spec Ch 15)

### 10.1 CollectionAPI (15.1)
- Multi-apply API schema: `collection:<name>`
- `includeRoot`, `expansionRule` (explicitOnly / expandPrims / expandPrimsAndProperties)
- `includes` and `excludes` relationship targets

**Test**: parse collection-bearing prims; verify membership computation.

---

## Phase 11: File Formats (Spec Ch 16)

### 11.1 Text Format -- USDA (16.2)
- PEG grammar for full USDA syntax
- All metadata fields, spec forms, value literals
- String escaping, multi-line strings
- Custom metadata, dictionary values
- Comment syntax (`#` and `//`)
- Version line: `#usda 1.0`

### 11.2 Binary Format -- USDC (16.3)
- Bootstrap: 64 bytes, magic `PXR-USDC`, version triple, TOC offset
- Sections: TOKENS, STRINGS, FIELDS, FIELDSETS, PATHS, SPECS
- Value representation: inlined vs out-of-line
- Compression: LZ4 for arrays; integer coding for ints
- Version compatibility (16.1): current version 0.13.0

### 11.3 Package Format -- USDZ (16.4)
- ZIP archive (uncompressed, 64-byte aligned)
- First file determines layer format
- Allowed file types: .usda, .usdc, .usd, .png, .jpg/.jpeg, .exr, .m4a, .mp3, .wav

**Test strategy**:
- **USDA**: roundtrip every spec feature through lusdcat; compare with pxrUSD usdcat output
- **USDC**: binary roundtrip; verify bootstrap/TOC/section structure; version compatibility
- **USDZ**: create and read archives; verify alignment; verify allowed file types

---

## Verification Methods

### Method A: Roundtrip Comparison (lusdcat vs usdcat)
```bash
USDCAT_PATH=~/local/USD/dist/bin/usdcat LUSDCAT_PATH=./build/lusdcat \
  bash tests/run-usdcat-compare.sh
```

### Method B: Unit Tests (ctest)
```bash
cd build && ctest --output-on-failure
```

### Method C: Targeted Conformance Tests
Create spec-specific test fixtures in `tests/usda/spec/` organized by chapter:
```
tests/usda/spec/
  ch06_types/           # foundational data types
  ch07_data_model/      # document data model, metadata fields
  ch08_paths/           # path grammar edge cases
  ch10_composition/     # LIVERPS examples from spec
  ch11_population/      # stage population, kind hierarchy
  ch12_value_resolution/ # timeSamples, splines, clips, blocked values
  ch13_schemas/         # API schemas, typed prims
  ch16_formats/         # format-specific edge cases
```

### Method D: Differential Testing vs OpenUSD
Use the `aousd/` OpenUSD build to cross-validate:
```bash
source aousd/setup_env.sh
python aousd/compare_usd_example.py test_file.usda
```

---

## Priority Order

| Priority | Area | Spec Sections | Risk |
|----------|------|---------------|------|
| P0 | USDA parse/write roundtrip | 16.2 | Core functionality |
| P0 | USDC parse/write roundtrip | 16.3 | Core functionality |
| P0 | All foundational types | 6 | Everything depends on correct types |
| P0 | Core metadata fields | 7.6 | Needed for any meaningful USD |
| P1 | Composition (sublayers, references, payloads) | 10.3.1-10.3.2.2 | Most used arcs |
| P1 | Value resolution (default, timeSamples) | 12.2-12.3.2 | Core query path |
| P1 | Stage population | 11 | Scene graph construction |
| P1 | Path grammar | 8 | Addressing correctness |
| P2 | Composition (inherits, specializes, variants) | 10.3.2.3-10.3.2.5 | Less common arcs |
| P2 | Strength ordering (LIVERPS) | 10.4 | Complex multi-layer scenes |
| P2 | Spline evaluation | 12.3.3, 7.4.2.4 | New in spec |
| P2 | Value clips | 12.3.4 | Specialized feature |
| P2 | USDZ package | 16.4 | Packaging |
| P3 | Relocates | 10.3.2.6 | Rarely used |
| P3 | Collections | 15 | API schema |
| P3 | Color spaces | 14 | Metadata only |
| P3 | List op edge cases (reducing, congruence) | 6.6.3.7-6.6.3.10 | Formal correctness |
| P3 | Schema system | 13 | Extension mechanism |

---

## Implementation Status Assessment (2026-03-22)

### Composition Arcs (Spec Ch 10)

| Arc | Parsing | Composition | Strength Ordering | Namespace Mappings | Notes |
|-----|---------|-------------|-------------------|--------------------|-------|
| Local (def/over/class) | YES | YES | YES | N/A | OverridePrimSpec, CombinePrimSpecRec |
| Inherits | YES | YES | YES | YES | CompositeInheritsRec; implied arcs single-level |
| Variants | YES | YES | YES | N/A (identity) | Deferred evaluation via CompositeAllArcs |
| Relocates | YES | YES | N/A | YES | CompositeRelocates; namespace remapping |
| References | YES | YES | YES | YES | CompositeReferencesRec; layer offset applied |
| Payloads | YES | YES | YES | YES | CompositePayloadRec; layer offset applied |
| Specializes | YES | YES | YES | YES | Globally weaker per Spec 10.4.1 |

**LIVERPS strength ordering**: Implemented via CompositeAllArcs(). Namespace mappings in src/namespace-mapping.hh.

### Value Resolution (Spec Ch 12)

| Feature | Status | Notes |
|---------|--------|-------|
| Metadata resolution (strongest opinion) | **YES** | typeName (12.2.2), custom OR (12.2.4), variability (12.2.3), specifier (12.2.1) |
| Attribute default values | YES | Implemented |
| TimeSamples | YES | Parsing and basic interpolation (held/linear) |
| Attribute value resolution priority | **YES** | Spec 12.3 priority: timeSamples > default at specific time; default > timeSamples at default time |
| Spline evaluation (bezier/hermite) | **YES** | src/spline-eval.hh: Bezier/Hermite cubic, all interpolation/extrapolation modes |
| Value clips | **YES** | Clip set parsing, template expansion, active clip finding, time remapping, asset loading + caching, attribute evaluation fallback |
| Blocked values (`None`) | YES | Full support: TYPE_ID_VALUEBLOCK, is_blocked(), set_blocked() |
| Layer offset/scale on time queries | **YES** | Applied during sublayer, reference, and payload composition |

### Data Model & Types (Spec Ch 6-7)

| Feature | Status | Notes |
|---------|--------|-------|
| All scalar types | **YES** | All 13 spec types parse: bool, uchar, int, uint, int64, uint64, half, float, double, string, token, asset, timecode |
| Dimensioned types | YES | vecN, matrixNd, quatN |
| Semantic aliases | **YES** | All 25 role types parse: color{3,4}{h,f,d}, normal3{h,f,d}, point3{h,f,d}, vector3{h,f,d}, texCoord{2,3}{h,f,d}, frame4d. Pretty-print incomplete for color{3,4}h, frame4d, uchar |
| Arrays | YES | T[] for all types |
| Dictionaries | YES | Nested dictionary support |
| List operations | YES | Explicit and composable; all ListEditQual values |
| Unicode XID identifiers | YES | Full XID_Start/XID_Continue tables in unicode-xid.hh |
| `group` (opaque) type | N/A | `group` in spec (6.2) is a type category hint, not a distinct value type; Kind::Group in prim-enums.hh covers the model hierarchy usage |

### Layer Metadata (Spec 7.6)

| Field | Status | Notes |
|-------|--------|-------|
| subLayers, subLayerOffsets | YES | |
| defaultPrim | YES | |
| upAxis, metersPerUnit | YES | |
| timeCodesPerSecond, start/endTimeCode | YES | |
| framesPerSecond | YES | |
| documentation, customLayerData | YES | |
| colorConfiguration, colorManagementSystem | **YES** | Added 2026-03-21: parse, store, print |
| owner | **YES** | Added 2026-03-21: parse, store, print |
| hasOwnedSubLayers | **YES** | Added 2026-03-21: parse, store, print |
| expressionVariables | **YES** | Added 2026-03-21: parse, store, print (as dictionary) |
| layerRelocates | **YES** | Parse, store, print, composition via CompositeRelocates() |

### File Formats (Spec Ch 16)

| Format | Read | Write | Notes |
|--------|------|-------|-------|
| USDA (text) | YES | YES | Hand-written parser; pprinter for output |
| USDC (binary/crate) | YES | EXPERIMENTAL | crate-reader production; crate-writer experimental |
| USDZ (zip package) | YES | YES | Read and write supported; SaveAsUSDZToFile/Memory, ValidateUSDZ |
| USD (auto-detect) | YES | N/A | Format detection implemented |

---

## Known Gaps -- Resolved and Remaining

### Resolved (Investigated)
- [x] ~~Blocked attribute values~~: **FULLY SUPPORTED** -- TYPE_ID_VALUEBLOCK, set_blocked(), is_blocked() in attribute.hh, primvar.hh, timesamples.hh
- [x] ~~Unicode XID_Start/XID_Continue~~: **FULLY SUPPORTED** -- unicode-xid.hh with lookup tables, is_valid_utf8_identifier() in str-util.cc
- [x] ~~Spline specialized type~~: **IMPLEMENTED** -- SplineData storage in PrimVar, spline evaluation wired into attribute resolution priority chain (timeSamples > spline > default); held interpolation for knots, full cubic via spline-eval.hh
- [x] ~~Value clips~~: **IMPLEMENTED** -- clips Dictionary parsed, template expansion, active clip finding, time remapping, asset loading + caching, attribute evaluation fallback in tydra

### Resolved (Implemented)
- [x] ~~expressionVariables on layer spec~~: **IMPLEMENTED** 2026-03-21 -- parse, store, print as dictionary
- [x] ~~hasOwnedSubLayers layer metadata~~: **IMPLEMENTED** 2026-03-21 -- parse, store, print
- [x] ~~All scalar types~~: **IMPLEMENTED** 2026-03-21 -- uchar and timecode added to parser and pprinter
- [x] ~~All semantic aliases~~: **IMPLEMENTED** 2026-03-21 -- all 25 role types parse/store/print including half/double variants, texCoord3, frame4d

### Remaining Gaps (Not Implemented)
- [x] ~~LIVERPS strength ordering~~ (Spec 10.4): **IMPLEMENTED** via CompositeAllArcs() -- applies arcs in I>V>R>P>S order
- [x] ~~Namespace mappings~~ (Spec 10.5): **IMPLEMENTED** in src/namespace-mapping.hh -- reference, inherit, relocate mappings + composition
- [x] ~~Relocates~~ (Spec 10.3.2.6): **IMPLEMENTED** -- CompositeRelocates() applies layerRelocates with prim renaming and namespace remapping of relationships/connections
- [x] ~~Implied inherit/specialize arcs~~ (Spec 10.3.2.3): **IMPLEMENTED** -- ArcOrigin tracking, arc origin tagging in sublayer/reference composition, multi-level implied inherit propagation with cycle detection in CompositeInheritsRec
- [x] ~~Variant deferred evaluation~~ (Spec 10.3.2.5): **IMPLEMENTED** -- two-phase variant processing in CompositeAllArcs: opinions collected from I/R/P arcs before variant selection applied
- [x] ~~Specializes global weakness~~ (Spec 10.4.1): **IMPLEMENTED** -- specializes applied last in CompositeAllArcs()
- [x] ~~layerRelocates USDA parsing~~ (Spec 10.3.2.6): **IMPLEMENTED** -- parse, store, print for `relocates = { <path> : <path>, ... }` syntax
- [x] ~~Time-value spline evaluation~~ (Spec 12.3.3): **IMPLEMENTED** in src/spline-eval.hh -- Bezier/Hermite cubic, held/linear/curve per-segment, held/linear/sloped/loop extrapolation, anti-regression
- [x] ~~Value clip evaluation~~ (Spec 12.3.4): **IMPLEMENTED** -- template expansion, active clip finding, time remapping, clip asset loading with caching, attribute fallback in EvaluateAttribute
- [x] ~~Template clip metadata~~ generation (12.3.4.1.3): **IMPLEMENTED** in src/value-clip-utils.hh -- templateAssetPath expansion with ### patterns, stride, activeOffset
- [x] ~~specifier resolution rules~~ (Spec 12.2.1): **IMPLEMENTED** -- over+def/class composites to def/class; defining spec propagates through sublayer composition
- [x] ~~typeName resolution from prim definition~~ (Spec 12.2.2): **IMPLEMENTED** -- typeName only taken from defining specs (def/class), not from over
- [x] ~~variability resolution from prim definition~~ (Spec 12.2.3): **IMPLEMENTED** -- SchemaRegistry singleton in schema-registry.hh with ~20 built-in uniform properties; CombinePrimSpecRec consults registry as weakest fallback
- [x] ~~custom field resolution~~ (Spec 12.2.4): **IMPLEMENTED** -- custom flag OR'd across all opinions in composition
- [x] ~~Deprecated `add` list op~~: **IMPLEMENTED** -- treated as `append` with deprecation warning per Spec 6.6.3.10
- [x] ~~Integer coding and LZ4 compression in USDC~~: **VERIFIED** -- LZ4 (src/lz4/), integer coding (src/integerCoding.cpp), bootstrap magic "PXR-USDC". Version support extended to 0.13.x per spec 16.3
