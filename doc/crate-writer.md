# USDC Crate Writer

The LightUSD USDC Crate Writer writes USD Stage data to binary USDC (Crate) format version 0.8.0. Status: experimental but functional with comprehensive test coverage.

> **Two crate writers in the current codebase.** This document primarily covers
> the **classic** writer (`src/crate-writer*.cc`, driven by `Stage` via
> `stage-converter.cc`, exposed by `lusdcat`). The refactored **`next`** engine
> ships a second, independent crate writer under `src/next/crate/` (used by the
> `next` pipeline — `next_usdcat -f -o out.usdc`, the wasm `--pipeline stream-next`
> USDZ path, etc.). The two share the on-disk format and the dedup principles
> below, but are separate implementations. The dedup/format reference sections
> apply to both; the [`next` crate writer performance](#next-crate-writer-performance--remaining-opportunities)
> section at the end is specific to `src/next/crate/`.

## Integration Points

**Classic writer:**
- Core writer: `src/crate-writer.cc` (sections/TOC/compression), `src/crate-writer-values.cc` (value-data encoding + dedup), `src/crate-writer-inline.cc` (`TryInlineValue`), `src/crate-writer.hh`
- Stage converter: `src/stage-converter.cc` (Stage/Prim/property → crate spec+field model)
- CLI: `examples/lusdcat` with `-o/--output` option
- Unit tests: `tests/unit/unit-crate-writer.cc`

**`next` writer (`src/next/crate/`):** a single `crate-writer.cc` translation unit
that `#include`s the `Impl` body split across `crate-writer-{impl,write,tables,passthrough,values,properties,fields,sections}.inc` (+ `crate-writer-types.{hh,cc}`, `crate-writer.hh`). Public API in `src/next/writer/usdc-writer.{hh,cc}` (`WriteUSDCToFile`/`WriteUSDCToMemory`/`WriteLayerToUSDC*`); built directly from a composed `next::Layer` (no `stage-converter` step). Tests: `tests/next/test_usdc_{writer,roundtrip,malformed}.cc` (the reader is
exercised by `test_usdc.cc` / `test_usdcat_roundtrip.cc`). A lazy-array **pass-through** (`crate-writer-passthrough.inc`, `TryPassThrough`) copies a POD array block verbatim from the memory-mapped source crate when the type/version are compatible, so a read→write of an unchanged array never re-encodes.

## Usage

```bash
# Basic conversion
./lusdcat input.usda -o output.usdc
./lusdcat output.usdc  # Verify readback

# Flattened composition
./lusdcat --flatten --composition=r,p input.usda -o flattened.usdc
```

## Implemented Features

**Core**: Binary v0.8.0 writing, file header, TOC, LZ4 compression, NaN-aware value deduplication (XXH3), path tree encoding, integer-array compression. Tagged float/double-array compression via a runtime writer option (see [below](#tagged-floatdouble-array-compression); off by default).

**Data Types**: Basic types (int, float, double, bool, string, token), vectors, matrices, paths, TimeSamples with ValueRep format, ListOp<T> for references/payloads/inherits, TokenListOp, StringListOp, PathListOp, Dictionary values, VariantSelectionMap.

**Geometry**: Mesh (with animated points, normals, advanced features, blend shapes), Cone, Cylinder, Capsule, GeomPoints, GeomCamera, GeomBasisCurves, GeomNurbsCurves, GeomSubset, GeomPointInstancer (with instance offsets), Model, Scope, GPrim properties (visibility, purpose, extent, normal interpolation).

**Materials**: Material, Shader, NodeGraph, UsdPreviewSurface, UsdUVTexture, UsdPrimvarReader, UsdTransform2d, material binding.

**Lighting**: SphereLight, RectLight, DistantLight, DomeLight, multiple lights, light filters.

**Skeletal**: Skeleton, SkelAnimation, SkelRoot, skeletal binding.

**Metadata**: Prim/attribute/relationship metadata, CustomData, AssetInfo, layer metadata (framesPerSecond, startTimeCode, endTimeCode, upAxis, metersPerUnit).

**Composition**: References (single + multiple) with customData, payloads, inherits, sublayers with LayerOffset, specializes arc, variantSets/variantSelection.

**Specs**: Separate `Attribute`, `Relationship`, `Connection`, `VariantSet`, and `Variant` specs (no longer embedded in the parent prim — see the `AddSpec(..., SpecType::*)` calls in `stage-converter.cc`). Property `variability` (Uniform) is written for uniform attributes.

**Safety**: Error context reporting, memory limits, filesize limits, validation (enable/disable), compression.

## Resolved Issues

### TypeName Encoding

Token index (uint32_t) was stored instead of the token object. Fixed to store the `value::token` (see `stage-converter.cc`, "Store the token, not the index!").

### SpecTypePseudoRoot Ordering

Spec sorting didn't guarantee PseudoRoot at index 0. The comparator in `crate-writer.cc` always sorts PseudoRoot first, with post-sort validation that the first spec is the PseudoRoot.

### Token Index 0 Sentinel

Index 0 is reserved for the pxrUSD magic token `;-)` before any real token is registered. Because `-0 == 0`, a property/path element at index 0 would otherwise be misread (the path-encoding bit games make element 0 ambiguous).

### TimeSamples Binary Format

TimeSamples are written using embedded `ValueRep` structures (indirection format matching pxrUSD). The earlier POD-vs-`_samples` storage split has been removed — `TimeSamples` now uses a single unified storage (see memory-and-performance.md / the refactor below).

### Dictionary (customData) Binary Format

Dictionaries (`customData`, and any nested dict value) use a recursive offset
layout, matching what the reader expects (`ReadCustomData` in
`src/crate-reader-values.cc`; `RecursiveRead` in pxrUSD's `crateFile.cpp`):

```
uint64_t count
repeat count times:
  StringIndex  key       // 4 bytes
  int64_t      offset    // 8 bytes, relative to the byte AFTER this field
  ValueRep     value     // 8 bytes (or points to an out-of-line value)
```

`offset == 8` means the `ValueRep` immediately follows the offset field; the
reader seeks `offset - 8` from the current position to reach the value.

Write ordering matters: the writer first **reserves** `8 + 20*count` bytes for
the dict frame (8 for the count + 4+8+8 per entry), packs each value (nested
out-of-line writes then land *after* the reserved frame), and finally seeks
back to backfill the count, keys, offsets, and `ValueRep`s
(`src/crate-writer-values.cc`). Writing the entries inline first — as OpenUSD's
`WriteMap` does — produces a stream this reader cannot parse: it misreads a
`ValueRep` as an offset and fails with `Invalid offset value: <huge number>`.

## Debugging

```bash
# Enable debug output
export LIGHTUSD_ENABLE_DCOUT=1
./lusdcat input.usda -o output.usdc

# Hex dump header
xxd -l 128 output.usdc
```

---

# Crate Format Deduplication (pxrUSD Reference)

Analysis of the deduplication system in OpenUSD v25.08 (`pxr/usd/sdf/crateFile.cpp`). This serves as reference for LightUSD's crate writer implementation.

**Principle**: Write each unique value exactly once, reference by offset or index.

## Deduplication Levels

### 1. Structural (Global)

Tables in `_PackingContext` deduplicate fundamental elements:

| Table | Purpose |
|-------|---------|
| `tokenToTokenIndex` | All tokens |
| `stringToStringIndex` | All strings |
| `pathToPathIndex` | All paths |
| `fieldToFieldIndex` | All fields |
| `fieldsToFieldSetIndex` | Field sets |

Written to dedicated sections: TOKENS, STRINGS, FIELDS, FIELDSETS, PATHS.

### 2. Value-Level (Per-Type)

**OpenUSD reference**: `_ValueHandler<T>` template deduplicates data values with separate dedup maps for scalars and arrays per concrete type, lazy allocation, cleared after write.

**LightUSD implementation**: out-of-line values (including TimeSamples) are deduplicated using NaN-aware hashing (`NanAwareHash` in `crate-writer.hh`). This follows the OpenUSD `TfHash` pattern where +0.0 and -0.0 are treated as identical (both canonicalized to zero bits before hashing). The hash function is XXH3_64bits (from xxHash v0.8.3), which is 1.1x–12.3x faster than FNV-1a depending on buffer size (see benchmark below). Collision verification uses NaN-aware byte equality (`buffers_equal`).

Dedup map type: `unordered_multimap<size_t, ValueDedupEntry>` keyed by XXH3 hash, with byte content stored for collision verification.

Float/double element types: canonicalize +0/-0, then XXH3 on the full buffer.
Non-float types (int, string, token, half): XXH3 on raw bytes directly.

### Value Classification

LightUSD inlining lives in `CrateWriter::TryInlineValue` (`crate-writer-inline.cc`). A `ValueRep` carries a 48-bit payload; a value is inlined iff it fits:

1. **Always inlined**: token / string / AssetPath (as their TOKENS/STRINGS index), bool, int32, uint32, half, float, and the `Specifier`/`Permission`/`Variability` enums.
2. **Conditionally inlined**: int64 / uint64 — only when the value fits in 48 bits; otherwise written out-of-line.
3. **Never inlined**: double (64 bits > 48-bit payload), vectors, matrices, and all arrays — written to the VALUE section.
4. **Value-deduplicated**: out-of-line values hashed (NaN-aware XXH3) and stored once.
5. **Array-deduplicated**: separate path; empty arrays inlined.

### Global (writer-lifetime) Value Deduplication

Beyond per-value hashing, the classic writer's dedup map is **writer-lifetime**:
`CrateWriter::value_dedup_map_` is a `std::unordered_multimap<size_t, ValueDedupEntry>`
keyed by `NanAwareHash(wire_tag, bytes)` and shared by *every* out-of-line value
of the whole write (all attributes, including every TimeSamples block) — it is
never cleared per-attribute. This recovers sharing *across* attributes (identical
rest poses, identity transforms, shared sub-curves) that a per-attribute map
misses. Keys are computed by `ComputeValueDedupDescriptor`
(`src/crate-writer-values.cc`), with a memory budget honored via
`CanRetainDeduplicatedValue` → `GetValueDedupBudgetBytes`
(default `max_memory_bytes/4`, capped at 512 MB) so a pathological all-unique
scene cannot balloon writer memory. See `doc/timesamples.md` for the motivating
case (the `bool[]` visibility-mask blow-up) and the column-oriented scalar
encoding that remains open.

## Array Compression (v0.5.0+)

Arrays can be both deduplicated and compressed. The decision is made per array at
write time:

- **Integer** (`int`/`uint`/`int64`/`uint64`): `Sdf_IntegerCompression` when the
  array has ≥ `MinCompressedArraySize` (= **16**) elements; otherwise raw. No code
  byte — the element type already tells the reader the layout.
- **Floating point** (`half`/`float`/`double`, crate ≥ 0.6.0, ≥ 16 elements):
  *tagged* compression — integer encoding if every value is exactly an int32,
  else a lookup table if there are few distinct values, else uncompressed. See
  [Tagged Float/Double Array Compression](#tagged-floatdouble-array-compression).
- **Other types**: Uncompressed.

For *which* values are eligible to be written out-of-line at all (vs. inlined in
the 48-bit ValueRep payload), see [Value Classification](#value-classification).

### Tagged Float/Double Array Compression

This is the format OpenUSD calls "compressed floating point arrays" (crate
version 0.6.0+). Reference: `_WritePossiblyCompressedArray<float/double/GfHalf>`
in `pxr/usd/sdf/crateFile.cpp` (OpenUSD v25.08). The matching LightUSD reader is
`ReadFloatArray`/`ReadDoubleArray`/`ReadHalfArray` in `src/crate-reader-arrays.cc`.

#### When OpenUSD creates a compressed float/double array

For a `VtArray<float|double|GfHalf>`, OpenUSD decides in this exact order:

1. **Gate.** If the crate version is `< 0.6.0`, or the array has fewer than
   `MinCompressedArraySize` (**16**) elements → write **uncompressed** (raw
   contiguous values, ValueRep *not* marked compressed, no code byte).

2. **Integer encoding — code `'i'`.** Test every element with
   `isIntegral(fp) = (int32_min <= fp && fp <= int32_max) && T(int32_t(fp)) == fp`.
   If *all* elements pass, the array is rewritten as a compressed `int32` stream.
   The reader reconstructs each value as `T(int32)`. Notes:
   - The range check happens *before* the cast (avoids UB on NaN/Inf/out-of-range).
   - `-0.0` satisfies `T(int32_t(-0.0)) == -0.0` (since `+0.0 == -0.0`), so the
     `'i'` path **collapses `-0.0` to `+0.0`**. This is intrinsic to the encoding.

3. **Lookup table — code `'t'`.** Otherwise OpenUSD builds a table of distinct
   values, bailing out as soon as it would exceed
   `maxLutSize = min(array.size() / 4, 1024)` distinct entries (a profitability
   bound; the 1024 ceiling also bounds the linear-search cost). If the table fits,
   the array is written as the table plus a compressed `uint32` index stream.
   OpenUSD finds duplicates with `operator==`, so it merges `+0.0`/`-0.0` and never
   merges `NaN` (each `NaN` is distinct, which usually pushes a `NaN`-heavy array
   over `maxLutSize` and into the uncompressed fallback).

4. **Uncompressed fallback.** If neither `'i'` nor `'t'` applies, write raw values
   with the ValueRep *not* marked compressed (so no code byte).

The ValueRep's `IsCompressed` bit is what selects the branch on read: when set and
`count >= 16`, the reader expects a code byte; otherwise it reads raw values.

#### Wire format

Element `count` is always written first. Then, only when the ValueRep is marked
compressed *and* `count >= MinCompressedArraySize`:

```
uint64  count
int8    code                     // 'i' or 't'
  'i':  <compressed int32 stream> // uint64 compSize + compSize bytes (Sdf_IntegerCompression)
  't':  uint32  lutSize
        T[lutSize]               // raw little-endian lookup-table entries
        <compressed uint32 stream> // uint64 compSize + compSize bytes (the indices)
```

If `count < 16` (even on the "compressed" path) or the ValueRep is not marked
compressed, the payload is simply `uint64 count` followed by raw `T[count]` — no
code byte.

#### LightUSD implementation (runtime option, default off)

LightUSD writes float/double arrays **uncompressed by default**. The tagged
encoding above is implemented and toggled by a runtime writer option (no compile
flag), so the default output is unchanged:

| Layer | Flag (default `false`) |
|-------|------------------------|
| Low-level crate writer | `CrateWriter::Options::enable_float_array_compression` |
| High-level USDC writer | `USDWriteOptions::compress_float_arrays` (→ `SaveAsUSDCToFile`) |
| `lusdcat` CLI | `--compress-float-arrays` |

```cpp
lightusd::USDWriteOptions wopts;
wopts.compress_float_arrays = true;  // default false
lightusd::usdc::SaveAsUSDCToFile("out.usdc", stage, &warn, &err, wopts);
```
```bash
lusdcat --compress-float-arrays input.usda -o out.usdc   # default: off
```

- Helpers: `CrateWriter::WriteCompressedFloatArray` /
  `WriteCompressedDoubleArray` (`src/crate-writer.cc`), invoked from the
  `std::vector<float>` / `std::vector<double>` branches of `WriteValueData`
  (`src/crate-writer-values.cc`). Compression is attempted only when both
  `Options.enable_float_array_compression` *and* `Options.enable_compression` are
  set (the latter gates the integer-stream compressor the `'i'`/`'t'` payloads use).
- Matches OpenUSD: `MinCompressedArraySize = 16`, the `'i'` integral test with the
  pre-cast range guard, and the `min(count/4, 1024)` LUT bound.
- Difference from OpenUSD (still Pixar-readable): the LUT is keyed on the **raw bit
  pattern** rather than `operator==`, so `-0.0` and `NaN` round-trip *exactly*
  through the `'t'` path (OpenUSD merges `±0.0` and explodes the LUT on `NaN`). The
  `'i'` path still collapses `-0.0 → +0.0`, identical to OpenUSD.
- `half[]` arrays are left uncompressed by LightUSD even with the option on (already
  2 bytes/element; only `float[]`/`double[]` are handled).
- Verified: `crate_writer_float_double_array_compression_roundtrip_test` runs every
  data pattern with the option both ON and OFF (`'i'`/`'t'`/raw, bit-exact) plus a
  file-size-shrinks assertion; Pixar `usdcat` reads both `'i'` and `'t'` LightUSD
  output with exact values.

## Performance

| Operation | Complexity |
|-----------|-----------|
| Dedup lookup | O(1) average |
| Hash computation | O(n) for value size n |
| Write value | O(n) only on first occurrence |

### XXH3 vs FNV-1a Throughput (NaN-aware, buffer canonicalize + hash)

Benchmark: `tests/feat/hash/hash_bench.cc`, 1M iterations, clang -O2.

| Buffer type | FNV-1a | XXH3 | Speedup |
|-------------|--------|------|---------|
| float3 (12B) | 1,173 ms | 1,075 ms | 1.1x |
| float[8] (32B) | 3,282 ms | 1,486 ms | 2.2x |
| float[100] (400B) | 48,140 ms | 9,289 ms | 5.2x |
| float[1000] (4KB) | 458,282 ms | 63,954 ms | 7.2x |
| matrix4d (128B) | 14,189 ms | 1,953 ms | 7.3x |
| int32[100] (400B) | 42,944 ms | 3,477 ms | 12.3x |

Zero collisions for both at 1M unique random inputs.

## Best Practices for USD Authors

- Reuse value objects rather than creating duplicates
- Use standard defaults (0, identity) that dedup well — +0.0 and -0.0 now dedup automatically
- Share time arrays across attributes when possible

## Enhancement Roadmap (classic writer)

Separate attribute/connection/relationship specs and Variant/VariantSet authoring are now implemented (see Implemented Features). Remaining:

- Performance optimizations (incremental writing, parallelism).
- Broader binary-compatibility verification against OpenUSD-written files.

---

# `next` Crate Writer Performance & Remaining Opportunities

Specific to `src/next/crate/`. Profiled on the public large scenes via
`LIGHTUSD_NEXT_TIMING=1 build-next-release/lusdcat -f -o out.usdc <root>` (`next_usdcat` remains a CMake target alias)
(`--compose-threads N` / `--compose-threads-auto` control *composition* threads),
write to a **seekable file**, not `/dev/null`
— the crate writer seek-patches headers). `CrateWriteOptions::num_threads`
(the *write* thread count; auto-capped in the library)
controls the parallel write paths; output is byte-identical across thread counts.

## What the write phase is bound by

On geometry-heavy scenes the write phase is dominated by the **serial per-spec
structural build** (`BuildFieldsAndSpecs` in `crate-writer-fields.inc`), NOT value
encoding. On Moana Island (4.2M specs) the original 49 s breakdown was roughly:
path-tree sort ~21%, malloc/free churn ~18%, token/string interning ~10%,
`InternBlock` value dedup ~6%, with `EncodeDeltaU32` (the only real array encode)
only ~2.6% — encoding the 2.44 GB of arrays is just ~3–5 s. Integer arrays use
delta+LZ4 (`compress_arrays`, ≥16 elems); float/double/vec/quat/matrix arrays are
stored raw; lazy uncompressed arrays pass through verbatim (no re-encode).

## Landed optimizations (all byte-identical, verified by `cmp` + Pixar `usdcat`)

Island USDC write **49 s → 29.8 s (−39%)**, Caldera ~7.6 → ~6 s, peak RSS reduced:

1. **Path-section sort** (`crate-writer-sections.inc`, `WritePathsSection`): sort
   `uint32` path INDICES (not `{string, uint32}` copies), then **parallel** sort
   (`ParallelSortIndices` in `crate-writer-impl.inc`: sort P chunks, pairwise-merge
   runs) — the sort was ~21% / ~10 s of write.
2. **Ancestor-synthesis set** keyed on `string_view` into the stable `paths_`
   (+ a deque for synthesized ancestors) instead of copying all 4.2M path strings.
3. **Reserve table/map capacity** up front (`paths_`/`specs_`/`fields_`/`value_data_`
   + the `path_to_index_`/`block_dedup_` maps) from a cheap prim+property count.
4. **Parallel value-byte assembly** (`EmitValueBytesToBuffer`): place each block at
   its precomputed 8-byte-aligned offset into one pre-sized, zero-filled buffer
   (disjoint sub-ranges → parallel, byte-balanced) instead of an `align()`+append
   loop; plus `WriteToMemory`/`WriteLayerToMemory` **move** the finished buffer out
   (`take_buffer()`) instead of copying the multi-GB crate.

## Measured DEAD END — do not retry: per-spec build map-reduce

The obvious next step — parallelize the per-prim `BuildFieldsAndSpecs` across
workers — was implemented end-to-end in **two** variants and **both were slower**
than the 29.8 s serial-build baseline on Island, with a multi-GB RSS blow-up:

- **freeze-tokens-and-paths**: serial pre-pass interns all tokens/strings/paths;
  workers do read-only lookups → 37.5 s, 15.9 GiB.
- **merge-time path resolution**: workers intern unique spec paths locally (parallel
  string construction), serial merge re-interns globally + rewrites the local path
  indices embedded in `PathListOp` (relationship/connection) blocks → 38.7 s, 17.5 GiB.

Root cause: the build is dominated by **serial GLOBAL dedup interning** — the path
table (4.2M unique paths forming a *tree* namespace) and the content-addressed
value-block `InternBlock`. That work is inherently un-parallelizable (global
uniqueness), and a map-reduce does not remove it — it **doubles** it (worker-local
interning + serial merge re-interning) and adds the worker-data memory blow-up. The
parallelizable part (field construction/encode) is too small a fraction to offset
that. (The inert `BuildOnePrim` factor-out + `frozen_tok_/frozen_str_` scaffolding
were kept as committed groundwork; `crate-writer-tables.inc`.) The
`PathListOp`/`StoreVariantSelectionMap` blocks embed path/string indices, and
`TimeSamples`/dict blocks embed BLOCK indices, which is what makes a deterministic
merge intricate — another reason the payoff did not justify it.

## Remaining opportunities (do NOT touch the serial global-dedup floor)

Each is byte-identical-safe and worth at most a few percent — they shave the tail
and the dedup constants, not the O(specs) global-interning floor:

1. **Precompute block hashes in parallel / faster `InternBlock`.** `InternBlock`
   (`crate-writer-impl.inc`) recomputes an FNV-1a hash over each block's head/mid/tail
   on the serial path. Compute hashes during value encoding (or in a parallel pre-pass
   over `value_data_`) and pass them in, so the serial dedup is just the multimap
   insert + a memcmp on true collisions. ~the 6% `InternBlock` slice.
2. **Parallel section LZ4.** TOKENS / FIELDS / FIELDSETS / SPECS each LZ4-compress
   independently in `Write*Section` (`crate-writer-sections.inc`); compress them on
   the worker pool and write in order. (PATHS already has the parallel sort.)
3. **Parallel value ENCODE for non-lazy arrays.** Island's ASCII source means arrays
   are non-lazy and fully re-encoded (delta+LZ4 / raw memcpy); that is the one
   embarrassingly-parallel chunk of the build (independent per block). It is only
   ~3–5%, but unlike the structural build it is safe to parallelize: pre-produce the
   block bytes + hash per array in parallel keyed by the source `Value`, then the
   serial walk consumes them (byte-identical, like the USDA writer's chunked path).
4. **Tagged float/double array compression** (port the classic writer's `'i'`/`'t'`
   encoding; see above) as an opt-in — smaller output, not faster.
5. **The only way past the floor** is to parallelize the global dedup ITSELF — a
   concurrent/sharded path + block hashmap, or eliminating the local↔global double
   interning. High risk, uncertain payoff, and it breaks deterministic-across-threads
   output; treat ~29.8 s (−39%) as the practical ceiling unless this is attempted.

## Verification expectations

`num_threads=1` `cmp`-equals `num_threads=8/16` (deterministic). USDC tests:
`ctest --test-dir build-next -R usdc` (THREADS OFF) + the threaded build. For
stream-next/wasm (single-threaded → serial path), the produced root `.usdc` is
byte-identical to the pre-change writer — confirmed on a large UE-export scene by
building both and comparing (root crate md5-identical; mesh count matches OpenUSD;
Pixar `usdcat` valid).
