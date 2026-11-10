# Memory and Performance

End-to-end memory analysis of LightUSD — from USD file loading through Tydra
RenderScene conversion to WASM/WebGL rendering — plus the durable optimization
history, measurement procedures, and benchmark results.

(Merged from the former `PACKED_ARRAY_OPTIMIZATION.md` and `memory-usage-tasks.md`.)

---

## Profiling Results (2026-03-12, refactor-2026 branch)

**Test model:** `suzanne-subd-lv6.usdc` (180 MB on disk, 12M triangulated vertices, 4M faces)

### lusdcat (Stage loading only, parse-only with `-l`)

| Metric | Value |
|--------|-------|
| Peak heap (parse only, `-l`) | **281 MB** |
| Peak heap (with USDA print) | **2.46 GB** |
| USDC parser peak (self-reported, lazy mode) | **92.3 MB** |
| USDC parser peak (self-reported, non-lazy) | **282.7 MB** |
| Stage `estimate_memory_usage()` | **217 MB** |

The self-reported peak matches actual heap within 1 MB in non-lazy mode (282.7 MB
reported vs 281 MB massif). In lazy mode (default), the reported 92.3 MB is the per-spec
concurrent high-water mark — lower than the cumulative massif peak because the scratch
buffer releases between specs.

**Peak breakdown (lusdcat `-l`, parse only, 281 MB):**
- 41% (~121 MB) — `UnpackValueRep` float3 array allocation via `DecodeFieldSet`
- 23% (~69 MB) — `ReadCompressedInts<int>` decompression buffers (2 sites, ~34 MB each)
- 35% (~104 MB) — `ReadIntArray<int>` output vectors + other allocations
- All through: `DecodeFieldSet` → `ResolveFieldValuePairs` → `BuildPropertyMap` → `ReconstructPrim<GeomMesh>`

Without `-l`, lusdcat serializes the whole Stage to stdout as USDA, pushing peak to
2.46 GB (the extra ~2.2 GB is string formatting of 12M vertices). Parse-only (`-l`)
isolates the parser footprint at 281 MB.

### tydra_to_renderscene (Full pipeline: parse + Tydra conversion)

| Metric | Value |
|--------|-------|
| Peak heap | **808 MB** |
| Mesh vertices (triangulated) | 12,091,392 |
| Normals count | 145,096,704 (12× face-vertex, not deduped) |
| Texcoords count | 96,731,136 |

| Component | MB | % | Function |
|-----------|---:|--:|----------|
| BuildVertexIndicesFastImpl normals | 138 | 17.1% | `vector<array<float,3>>::resize` |
| CrateReader UnpackValueRep float3 | 115 | 14.3% | `DecodeFieldSet` → `ReadCompressedInts` |
| TriangulateVertexAttribute normals | 138 | 17.1% | `vector<uint8_t>::resize` |
| TriangulateVertexAttribute texcoords | 92 | 11.4% | `vector<uint8_t>::resize` |
| TriangulatePolygon index buffers (×4) | 139 | 17.1% | `vector<uint32_t>::reserve` |
| ConvertMesh misc (texcoords, indices) | 55 | 6.7% | `vector<uint32_t/array<float,3>>` |
| CrateReader decompression int arrays | 24 | 2.9% | `ConvertMesh` scratch |

**Biggest finding:** triangulation + vertex-index building together consume **~530 MB
(65%)** of peak — temporary working buffers freed incrementally (see Optimization
History).

---

## Pipeline Overview

```
File on Disk (.usdc / .usda / .usdz)
    |
    v
[Stage 1] File I/O
    ├── mmap (zero-copy, preferred)
    └── ReadWholeFile (heap copy fallback)
    |
    v
[Stage 2] USDC/USDA Parsing → Stage object
    ├── CrateReader (USDC) — MemoryBudgetManager tracks allocations
    ├── AsciiParser (USDA) — CHECK_MEMORY_USAGE macro
    └── Composition (references, payloads, sublayers)
    |
    v
[Stage 3] Tydra ConvertToRenderScene → RenderScene object
    ├── ConvertMesh: points, normals, texcoords, tangents, indices
    │   (per-mesh geometry runs on a worker pool when
    │    LIGHTUSD_ENABLE_THREAD is ON and scene_config.num_threads != 1;
    │    output is byte-identical to the serial path)
    ├── Texture loading: image decode into buffers[]
    ├── Material conversion
    └── Skeleton/animation conversion
    |
    v
[Stage 4] Application consumption
    ├── Native: OpenGL/Vulkan vertex upload
    └── WASM: binding.cc → JS → Three.js / WebGL2
```

### Stage 1: File I/O — mmap vs Heap

| Method | Peak Heap | Approach |
|--------|----------|---------|
| `MMapFile()` | ~0 | OS page-maps file, no heap copy |
| `ReadWholeFile()` | file size | `std::vector<uint8_t>` allocation |

Used in `lusdcat/main.cc` and `lightusd.cc`. For a 188 MB USDC, mmap avoids 188 MB of
heap entirely. Fallback to `ReadWholeFile` when mmap is unavailable.

Limits (`USDLoadOptions`, `lightusd.hh`): `max_memory_limit_in_mb` 16384 (16 GB);
`max_allowed_asset_size_in_mb` 1024 (1 GB); `max_image_width/height` 2048.

### Stage 2: USDC/USDA Parsing

`MemoryBudgetManager` (RAII, `src/memory-budget.hh`) wraps every crate-reader allocation
(`CheckAndReserve(bytes)` / `Release(bytes)`). Every array read, string allocation, and
decompression buffer goes through it. Tracked via `USDCMemoryUsageReport`
(`current/peak/max_budget/remaining`). CLI: `lusdcat --memstat model.usdc`.

Major CrateReader hotspots: the token array (up to 64M tokens), field array (up to 256M),
spec/fieldset array (up to 256M), reused decompression buffers, and the live fieldsets map
(large with lazy loading disabled). TimeSamples dedup uses a per-array `dedup_map`
(`HashMap<uint64_t, size_t>` in `src/crate-reader-timesamples.cc`): the ValueRep's raw
data bits key to the first sample index that produced it, and repeated frames call
`duplicate_sample()` for a zero-copy share. ValueRep encodes the element type in its
bits, so keys from different types never collide (the 22 type-specific caches were
consolidated into one map).

`use_lazy_property_construction` (`src/usdc-reader.hh`, default true) enables deferred
fieldset decoding (properties unpacked on access).
The old lazy *fieldset cache* was **removed** — it reused cache entries across FVPairs
sharing field_ids, producing corrupted `xformOpOrder` comparisons; the lazy path now
decodes into a per-call scratch buffer (more CPU, correct, lower peak).

The USDA parser (`ascii-parser.cc`) uses the same `CHECK_MEMORY_USAGE` accumulator
(`_memory_usage += nbytes; if (> _max_memory_limit_bytes) return false;`), default limit
16 GB (`usda-reader.hh`).

`Stage::estimate_memory_usage()` (`stage.cc`) does a full iterative stack-based traversal
of the Prim tree: per Prim it estimates `_data` (`Value::estimate_memory_usage()`), string
members, all properties via concrete-type dispatch (GeomMesh/Xform/Material/…), and deep
attribute memory for large typed attributes. Reports **217 MB** for `suzanne-subd-lv6`.
`Layer::estimate_memory_usage()` (`layer.cc`) recursively counts PrimSpecs, metadata
strings, sublayers, and VariantSetSpec internals.

### Stage 3: Tydra RenderScene Conversion

Per-vertex memory for N vertices (after index building):

| Attribute | Format | Bytes/vertex |
|-----------|--------|-------------:|
| positions | float3 | 12 |
| normals | float3 (or quantized — see below) | 12 |
| texcoords (per UV set) | float2 | 8 |
| tangents (packed) | uint32 / half4 | 4 / 8 |
| tangents (legacy) | float3 (+float3 binormal) | 12 (+12) |
| vertex colors / opacities | float3 / float | 12 / 4 |
| joint indices / weights (4 bones) | int×4 / float×4 | 16 / 16 |

Index data: `uint32_t` per face-vertex for original and triangulated topology, plus the
original→triangulated mapping. Blend shapes: full vertex buffer per morph target.

**Textures dominate.** Stored decoded in `RenderScene::buffers[]`:

| Format | 2K×2K | 4K×4K |
|--------|------:|------:|
| RGB uint8 | 12 MB | 48 MB |
| RGBA uint8 | 16 MB | 64 MB |
| RGB float32 | 48 MB | 192 MB |
| RGBA float32 | 64 MB | 256 MB |

`preserve_texel_bitdepth = true` keeps 8-bit textures as uint8 (4× saving). Example 100K-
vertex skinned character: ~8 MB geometry + ~80 MB textures (5× 2K RGBA) ≈ 88 MB total.

`RenderMesh::estimate_memory_usage()` / `RenderScene::estimate_memory_usage()`
(`render-data.cc`) count, respectively: points/indices/normals/tangents/binormals/
texcoords/colors/opacities/JointAndWeight/ShapeTarget/MaterialSubset; and mesh details +
buffers (textures) + Node tree + RenderMaterial + AnimationClip + SkelHierarchy +
TextureImage.

`MeshConverterConfig` memory options (`src/tydra/render-data-converter.hh`):

| Option | Default | Effect |
|--------|---------|--------|
| `tangent_storage` | PackedFp16 (native) / PackedSNorm8 (WASM) | packed tangent + handedness, no separate binormal |
| `normal_storage` | Float3 (native) / PackedSNorm8 (WASM) | SNorm8x3 = 3 bytes/vertex (75% saving) |
| `compute_tangents_only_with_normal_map` | true | skip tangents without a normal map |
| `defer_tangent_computation` | false (true in WASM) | defer tangent work |
| `build_vertex_indices` | true | dedup vertices |
| `lowmem` | false (true in WASM) | free source GeomMesh after conversion |
| `preserve_texel_bitdepth` | false (true in WASM) | keep uint8 textures |
| `load_texture_assets` | true | false to skip texture loading |

(`lowmem`, `preserve_texel_bitdepth`, and `load_texture_assets` now live on
`SceneConfig` in the same header.)

WASM memory limits (`max_memory_limit_mb_` in `binding.cc`): 2 GB (32-bit), 8 GB
(64-bit/MEMORY64).

### Stage 4: WASM Binding

`web/binding.cc` exposes RenderMesh data as JS typed arrays. `tangents4_cache_[mesh_id]`
(vec4 float, unpacked from packed) and `reordered_mesh_cache_[mesh_id]` (vertex data
reordered for draw calls) add temporary memory; both are invalidated on
`computeMeshTangents()`.

---

## MMap Zero-Copy Pipeline (V2 — Deferred Reads)

When loading USDC via mmap with `USDLoadOptions::mmap_zero_copy`, large uncompressed
float/double/half arrays (points, normals, texcoords) are **deferred**: Stage stores only
a 24-byte `MMapArrayRef` sentinel + an empty typed vector. Tydra reads the data on demand
from the mmap'd buffer.

1. **CrateReader** (`crate-reader.cc`): `UnpackValueRep` calls `DescribeValueRep`; for an
   eligible uncompressed array it records byte offset / element count / element size /
   type id in an `MMapArrayRef` and returns immediately (no full unpack).
2. **USDC Reader** (`usdc-reader.cc`): mmap refs are collected into an `MMapArrayTable`
   keyed by `"prim_path\0attr_name"`, attached to the Stage after `ReconstructStage`.
3. **Stage** (`stage.hh/cc`): holds optional `MMapArrayTable` + `MMapDataSource` (both
   `unique_ptr`; not copied on Stage copy).
4. **Tydra** (`src/tydra/render-data-mesh.cc`): `TryReadMMapArray<T>()` (line ~865) validates
   bounds/alignment via `MMapDataSource::get_ptr<T>()` and `memcpy`s from mmap;
   `TryReadMMapArrayWithIndices<T>`
   expands indexed primvars (int index arrays are LZ4-compressed, so always materialized).
   Falls back to `EvaluateTypedAnimatableAttribute` / `flatten_with_indices` otherwise.
   Used for `points`, authored `normals`, `primvars:normals`, `texcoord2f` primvars.

**Eligible types** (never compressed in USDC, verified against OpenUSD `crateFile.cpp`):
VEC2/3/4 F/D/H, scalar FLOAT/DOUBLE/HALF, MATRIX2/3/4D. NOT eligible: INT/UINT/INT64/UINT64
(LZ4 + integer compression). Minimum threshold: 1024 elements.

Activation: `--mmap-lowmem` (CLI `tydra_to_renderscene`), `setMMapZeroCopy(true)` (WASM,
default off), or `USDLoadOptions::mmap_zero_copy = true` (C++). File-based loaders keep
the backing mmap/file buffer alive through `Stage` ownership; memory-based loaders require
the caller's input buffer to remain alive while zero-copy refs may be used. See
[mmap.md](mmap.md) for API usage and lifecycle details.

**Limitations:** TimeSamples are NOT deferred (V2 only handles `default` values);
`ExportToString`/pprinter and direct Stage accessors (`get_points()`, …) return empty
vectors for deferred arrays (accepted, opt-in); sub-1024-element arrays always
materialized.

**Verified** (OBJ identical to baseline): suzanne-subd-lv5/lv6 (3 deferred arrays each),
CesiumMan.usdz (4), a large production scene (233 deferred, 116.35 MB RenderScene),
timesamples-array-dedup-001/002/004.

**V2 savings** (suzanne-subd-lv6, 12M verts): Stage float arrays ~288 MB → ~0 (sentinels);
estimated peak ~600–700 MB (V1 hybrid) → ~400–450 MB (V2 deferred).

---

## Optimization History

| Optimization | Savings | Where |
|-------------|---------|-------|
| Parallel per-mesh geometry conversion (legacy Tydra) | ~2x on 16 workers for many-mesh scenes; byte-identical output | `render-data.cc` (`ConvertDeferredMeshes`), `task-arena.hh`, `render-data-material.cc` (collect mode) |
| Dynamic work distribution (tydra-next) | removes fixed-batch wave stalls on mixed-size meshes (~3.5x vs serial when `LIGHTUSD_NEXT_ENABLE_THREAD=ON`) | `tydra/next/render-converter.cc` |
| Instance-registry descendant lookup | O(instances × meshes) → O(log meshes) | `render-data.cc` |
| MikkTSpace Fast/Hybrid zero-copy inputs | one full vertex-array copy per mesh | `render-data-mesh.cc`, `fast-mikktspace.hh` |
| mmap file loading | ~file size heap | `lusdcat`, `lightusd.cc` |
| MMap zero-copy V2 (deferred reads) | ~120+ MB on large meshes; Stage arrays → sentinels | `mmap-array-ref.hh`, `crate-reader.cc`, `usdc-reader.cc`, `stage.cc`, `render-data.cc` |
| Tangent quantization (10_10_10_2 / Fp16x4) | 83% / 67% tangent storage | `render-data.cc`, `tangent-quantize.hh` |
| Normal quantization (SNorm8 default) | 75% normal storage | `render-data.cc`, `tangent-quantize.hh` |
| Quantized-normal dedup before triangulation | ~138 MB triangulation buffer for smooth meshes | `render-data.cc` |
| Zero-copy tangent computation | ~200 MB on large meshes | `render-data.cc` |
| Skip tangents for non-normal-map meshes | full tangent cost | `render-data.cc` |
| Deferred tangent computation (WASM) | initial-load reduction | `render-data.cc`, `binding.cc` |
| `preserve_texel_bitdepth` | up to 4× texture memory | `binding.cc` |
| `lowmem` GeomMesh freeing (extended: core + SubD + all primvars) | ~115 MB source data freed post-conversion | `render-data.cc` |
| Free triangulation intermediates | ~161 MB unconditional + ~112 MB with lowmem | `render-data.cc` |
| Free vertex_output / dedup buckets in BuildVertexIndicesImpl | reduces peak overlap | `render-data.cc` |
| CrateReader buffer reuse + streaming decompress | reduced peak during decompress (>1M element arrays) | `crate-reader.cc` |
| CrateReader decompression buffer shrink | ~68 MB reclaimed after parsing | `crate-reader.hh`, `usdc-reader.cc` |
| Remove lazy fieldset cache | eliminates unbounded cache growth + fixes correctness | `usdc-reader.cc` |
| Consolidate 22 dedup caches → 1 unified | less hash-map overhead, simpler code | `crate-reader.hh`, `crate-reader-timesamples.cc` |
| Remove `TypedArrayPtr` dead code | ~290 lines; eliminates ownership confusion | `typed-array.hh`, `timesamples.hh`, `timesamples-pprint.cc` |
| TimeSamples move-in (lazy mode) | eliminates deep copy in lazy property construction | `usdc-reader.cc` |
| Fix MemoryBudgetManager tracking | 49 KB → 282.7 MB reported (matches massif) | `crate-reader.cc`, `usdc-reader.cc`, `lusdcat/main.cc` |
| Fix Stage `estimate_memory_usage()` | 4 KB → 217 MB (full recursive Prim-tree walk) | `stage.cc`, `prim-types.cc`, `value-types.cc` |
| Complete RenderMesh / RenderScene / Layer estimation | JointAndWeight, ShapeTarget, MaterialSubset, Node tree, Material, AnimationClip, SkelHierarchy, VariantSetSpec | `render-data.cc`, `layer.cc` |
| Connection resolve cache shrink_to_fit | swap-with-empty releases hash buckets | `render-data.cc` |
| WASM asset cache size limit + eviction | bounds cache growth; `setAssetCacheMaxSizeBytes()` | `binding.cc` |
| Packed normal/tangent exposure in WASM | correct unpacking + raw packed export for WebGL2 | `binding.cc` |

### Notable fixes (durable)

- **MemoryBudgetManager blind spots.** Self-reported parser memory was 49 KB for a 180 MB
  file that produced 281 MB actual peak (~5700× undercount). Fixed by: moving
  `GetMemoryUsageReport()` to after `ReconstructStage()`; balanced budget release in lazy
  mode; persistent decompression-buffer tracking (reserve growth delta only, never
  release); temp-vector tracking in `ReadFloat/Double/HalfArray`.
- **TypedArray ownership.** The packed 64-bit `TypedArrayPtr<T>` smart pointer (dedup flag
  in bit 63) was **removed as dead code** — the dedup caches already store `size_t` indices
  rather than pointer objects, so its ownership/copy-semantics hazards no longer apply.
  (See the historical design note below.) `doc/TYPED_ARRAY_REVIEW_2025.md` was removed.
- **Deferred-tangent + index-build race.** With `defer_tangent_computation=true` on a mesh
  with a normal map, vertex index building was skipped → garbled textures. Fixed by
  reordering the defer check before the index-build decision.

---

## Hash Throughput: XXH3 vs FNV-1a

The USDC crate writer deduplicates out-of-line values with a NaN-aware hash (see
[crate-writer.md](crate-writer.md)). XXH3_64bits replaced FNV-1a.

Benchmark: `tests/feat/hash/hash_bench.cc`, 1M iterations, clang -O2 (NaN-aware:
canonicalize +0/-0, then hash).

| Buffer type | FNV-1a | XXH3 | Speedup |
|-------------|--------|------|---------|
| float3 (12B) | 1,173 ms | 1,075 ms | 1.1x |
| float[8] (32B) | 3,282 ms | 1,486 ms | 2.2x |
| float[100] (400B) | 48,140 ms | 9,289 ms | 5.2x |
| float[1000] (4KB) | 458,282 ms | 63,954 ms | 7.2x |
| matrix4d (128B) | 14,189 ms | 1,953 ms | 7.3x |
| int32[100] (400B) | 42,944 ms | 3,477 ms | 12.3x |

Zero collisions for both at 1M unique random inputs.

---

## Measurement Procedures

```bash
# USDC parser current/peak/budget + Stage estimate
./build/examples/lusdcat/lusdcat --memstat model.usdc

# Stage estimate after load + RenderScene estimate after Tydra conversion
# (--nodump suppresses USDA output; per-mesh and per-buffer breakdown)
./build/examples/tydra_to_renderscene/tydra_to_renderscene --memstat model.usdc

# Tangent benchmark (speed, working memory, quality, quantization error)
cd tests/feat/tangent && make
./bench_tangent --quality --sizes 32,128,512,1024 --ico-levels 3,5,7

# Peak heap profile
valgrind --tool=massif --pages-as-heap=no \
  ./build/examples/tydra_to_renderscene/tydra_to_renderscene model.usdc
ms_print massif.out.<pid>

# Leak detection
valgrind --leak-check=full --show-leak-kinds=all \
  ./build/examples/lusdcat/lusdcat model.usdc
```

WASM: Chrome DevTools → Memory → heap snapshot before/after USD load; Performance tab →
Memory for the allocation timeline. Compare `defer_tangent_computation` and `lowmem`
on/off.

C++ API:

```cpp
size_t stage_mem = stage.estimate_memory_usage();          // after loading
size_t scene_mem = render_scene.estimate_memory_usage();   // after conversion
for (const auto &mesh : render_scene.meshes) {
    size_t mesh_mem = mesh.estimate_memory_usage();
}
```

---

## Historical: PackedTypedArrayPtr (removed)

**This type has been removed from the codebase** (`TypedArrayPtr<T>` dead-code removal —
the dedup caches store `size_t` indices, not pointer objects). The design is recorded here
because the same 64-bit packing idea is still used conceptually (e.g. the unified
`_dedup_array_cache` keys encode the element type in the high bits of a `ValueRep`).

`PackedTypedArrayPtr<T>` was a memory-optimized smart pointer for `TypedArray<T>` that
packed a pointer and a dedup/mmap flag into a single 64-bit value (8 bytes, same as a raw
pointer).

```
Bit Layout (64 bits):
  Bit 63 (MSB) : Dedup/mmap flag — 1 = shared/mmap (not deleted on destruction),
                 0 = owned (deleted on destruction)
  Bits 48-62   : Reserved (15 bits)
  Bits 0-47    : Pointer to TypedArray<T> (48-bit canonical x86-64 / ARM64 address)
```

Canonical-address handling: user space `0x0000'0000'0000'0000`–`0x0000'7FFF'FFFF'FFFF`,
kernel space `0xFFFF'8000'0000'0000`–`0xFFFF'FFFF'FFFF'FFFF`. On unpack, if bit 47 is set
the pointer is sign-extended to restore canonical form.

API surface (for reference):

```cpp
PackedTypedArrayPtr();                                  // null
PackedTypedArrayPtr(TypedArray<T>* ptr, bool dedup);    // from pointer
PackedTypedArrayPtr(const PackedTypedArrayPtr&);        // shallow copy
PackedTypedArrayPtr(PackedTypedArrayPtr&&);             // move
~PackedTypedArrayPtr();                                 // deletes iff !is_dedup()

TypedArray<T>* get() const;     T* operator->() const;  T& operator*() const;
bool is_null() const;           explicit operator bool() const;
bool is_dedup() const;          void set_dedup(bool);
void reset(TypedArray<T>* = nullptr, bool dedup = false);  TypedArray<T>* release();
uint64_t get_packed_value() const;
```

Copy semantics: copying an **owned** pointer marked the copy as dedup (prevents
double-free); copying a **dedup** pointer was safe as-is. The 15 reserved bits were
intended for refcounts / type tags / cache-coherency flags.

---

## Key Files

| File | Purpose |
|------|---------|
| `src/lightusd.hh` | `USDLoadOptions` (memory / asset limits) |
| `src/memory-budget.hh` | `MemoryBudgetManager` RAII |
| `src/usdc-reader.hh` | `USDCMemoryUsageReport`, memory-tracking API |
| `src/crate-reader.cc` | budget-checked allocations, buffer reuse, streaming decompress |
| `src/stage.cc` | `Stage::estimate_memory_usage()` |
| `src/layer.cc` | `Layer::estimate_memory_usage()` |
| `src/value-types.cc` | `Value::estimate_memory_usage()` |
| `src/prim-types.cc` | Property/Attribute/Relationship estimation |
| `src/primvar.hh` | `PrimVar::estimate_memory_usage()` |
| `src/timesamples.hh` | `TimeSamples::estimate_memory_usage()`, sample storage |
| `src/mmap-array-ref.hh` | `MMapArrayRef`, `MMapArrayTable`, `MMapDataSource` |
| `src/crate-format.hh` | `CrateValue` mmap-ref attachment |
| `src/tydra/render-data.hh` | `MeshConverterConfig` (memory options) |
| `src/tydra/render-data.cc` | RenderMesh/RenderScene estimation, tangent quantization |
| `src/tydra/tangent-quantize.hh` | packed tangent/normal formats |
| `web/binding.cc` | WASM memory config, tangent cache, deferred computation |
| `src/legacy/examples/lusdcat/main.cc` | mmap loading, `--memstat` |
| `examples/tydra_to_renderscene/to-renderscene-main.cc` | per-mesh/-buffer `--memstat` |
| `tests/feat/tangent/bench_tangent.cc` | tangent memory/quality benchmark |
| `tests/feat/hash/hash_bench.cc` | XXH3 vs FNV-1a hash benchmark |
| `doc/tydra-tangent.md` | tangent computation + quantization |

---

## refactor-next Phase-0 baselines (2026-06-10, HEAD 59801312)

Baselines for the `src/next` optimization roadmap (`doc/refactor-next.md`),
captured with `bench_pcp_compose` and `bench_lazy_mem` (Release, gcc,
Linux x86-64). Re-measure after each phase and diff here. For the current
standalone `next` smoke tests and benchmark entrypoint, run:

```bash
RUN_BENCH=1 BUILD_TYPE=Release scripts/run-next-checks.sh
```

The script uses small `bench_lazy_mem` defaults for routine local checks. Set
`BENCH_LAZY_VERTS=4000000 BENCH_LAZY_CLONES=32` to reproduce the historical
large lazy/eager clone comparison below.

### Struct sizes (`bench_pcp_compose sizes`)

| struct | bytes | notes / target |
|--------|-------|----------------|
| `Value` | 160 | 136B SBO + header |
| `PrimSpec` | 656 | Phase 8 target (MetaExt split) |
| `PrimSpecMeta` | 344 | inline in every PrimSpec |
| `VariantSetData` | 88 | |
| `Layer` | 296 | |
| `Path` | 32 | plain std::string wrapper |
| `LazyArrayRef` | 64 | |
| `pcp::CompNode` | 120 | doc'd target ≤40B (interned/packed) |
| `pcp::PrimIndex` | 96 | |
| `pcp::LayerStack` | 96 | |

### Per-prim fixed cost (100k empty Xform prims)

| metric | value |
|--------|-------|
| build time | 175 ms (570k prims/sec) |
| self-reported | 1119 B/prim |
| RSS delta | 960 B/prim (93.8 MB total) |

Empty prims (no properties/samples/arcs) cost ~1 KB each — the Phase-8
`PrimSpecMetaExt` split + lazy `TimeSampleStorage` target.

### Composition (`bench_pcp_compose compose`, M=20000 prims, R=64 shared assets, 256-vert arrays)

| stage | value |
|-------|-------|
| Cache::Open | 0.2 ms |
| ComputePrimIndex ×20000 | 145.4 ms (7.3 µs/prim) |
| BuildStage | 213.6 ms |
| composed prims | 40001 |
| stage memory | 52.9 MB |
| peak RSS | 173 MB |

Phase-4 targets (FindSpecs memoization + interned keys + GraftSubtree).

### Deep reference chain (`bench_pcp_compose deep`, D=200)

| stage | value |
|-------|-------|
| ComputePrimIndex | 2.95 ms (201 nodes) |
| BuildStage | 0.08 ms |

Phase-1 target (per-arc copied cycle sets → frame chain; currently O(D²·len)).

### Lazy vs eager clone (`bench_lazy_mem`, 4M-vert usdc = 45.8 MB, K=32 clones)

| mode | peak RSS |
|------|----------|
| eager | 2,159,964 KB (2.06 GB) |
| lazy | 97,792 KB (95 MB) |

The 22× gap is what Phase-3 CoW array storage closes for *materialized*
(USDA / eager-crate-type) values; lazy crate arrays already share.

### genmany (100k prims, chain=64)

| metric | value |
|--------|-------|
| peak RSS | 771,404 KB |
| build prims | 100000 (reread OK, out=7.2 MB) |

### Phase-1 deltas (recursion/cycle hardening + lazy per-prim storage)

| metric | Phase 0 | Phase 1 | delta |
|--------|---------|---------|-------|
| empty prim, self-reported | 1119 B/prim | 975 B/prim | −13% |
| empty prim, RSS | 960 B/prim | 736 B/prim | −23% |
| deep chain D=200 index | 2.95 ms | 1.58 ms | −46% |
| compose M=20k index / BuildStage | 145 / 214 ms | 155 / 201 ms | ~noise |

Sources: lazy `values_`/`time_samples_` allocation (two heap blocks per prim
were eager), and the per-arc copied `std::set<std::string>` cycle keys replaced
by a stack-frame chain. Note: `stage_memory` self-reporting *increased*
(52.9 → 73.6 MB) because the old `ValueStorage` accounting counted a dead byte
buffer (always 0) — the new number is honest, not a regression.

### Phase-3 delta (copy-on-write array storage in Value)

`bench_lazy_mem`, 4M-vert usdc (45.8 MB), K=32 clones:

| mode | Phase 0 | Phase 3 | delta |
|------|---------|---------|-------|
| eager | 2,159,964 KB | 176,640 KB | **−92% (12×)** |
| lazy | 97,792 KB | 97,536 KB | unchanged |

`Value`'s array buffers moved from a raw owning pointer to a
`shared_ptr<ArrayStorageBase>` (VtArray `_DetachIfNotUnique`): copy = refcount
bump, first mutable access clones if shared. Eager-read clones now share the
one decoded buffer instead of deep-copying it 32×. Closes M1 for all
materialized (USDA + eager-crate-type) arrays, not just lazy crate arrays.

### Phase-4 delta (FindSpecs memoization)

`bench_pcp_compose compose` (M=20000 prims, R=64 shared assets):

| stage | Phase 0 | Phase 4 | delta |
|-------|---------|---------|-------|
| BuildStage | 213.6 ms | 163.2 ms | **−24%** |
| ComputePrimIndex ×20000 | 145.4 ms | 155.6 ms | ~noise |

The same `(stack, site)` was resolved 3–5× per composed prim; memoizing
`FindSpecs` (stable references across rehash; cleared on `InvalidateLayer`)
removes the redundant layer walks + Path parses on the BuildStage path. The
GraftSubtree child-index walk (M5) **landed** (`composition.cc` `GraftSubtree`):
it walks the source subtree via `child_indices` when they validate for the
subtree (every visited index in range, unseen, and prefixed by `src_root`), and
falls back to the full path-prefix scan otherwise — composed-in-place /
cloned-and-renamed layers can carry stale child indices, so the guarded
fallback keeps correctness (CoW already removed the per-graft array-copy cost
the earlier revert cited).

### M3 (pcp hot-map keys) — landed 2026-07-16 as open-addressed caches + memos

The originally-sketched full u32-interned-key conversion stayed too invasive,
but its targets were hit with the pattern proven on the `next-refactor` branch
(open-addressed hash→index tables over stable deque value storage, fast
8-byte-chunked FNV-mix hash, keyed by the caller's string with no per-call key
copy) plus one new memo:

1. **`StackSpecCache`** (`cache.cc`) — per-layer-stack Specs() memo replaces the
   `unordered_map<(stack,site-string), vector<SpecRef>>` whose struct key
   deep-copied the site string on every call (hits included).
2. **`Src::specs_` / `SpecsFor()`** — per-Src memo of the resolved Specs()
   pointer for the 2–3 read-side lookups per composed prim. The memo
   self-resets on any Src copy/move (`SpecsMemo`), so child-build /
   worker-seed / merge copies can never carry a stale or cross-Impl pointer;
   the one buffer-steal vector move (MergeSources) resets it explicitly.
3. **`SrcCache`** (`cache.cc`) — sources_cache (path → expanded Srcs), the
   structure pass's hottest map. Reference stability across recursive
   ExpandList inserts and `operator[]`-creates-empty semantics are preserved
   (deque values, tombstone erase for invalidation).
4. **`arc_target_memo_`** (new on dev) — ProcessArc's external-arc resolution
   (`RealAnchorOf` + resolver `Resolve` ×2 + expression-vars fingerprint +
   `stack_by_id` std::map walk) runs per arc *instance*; the resolved
   (layer, arc_id, stack_idx) depends only on (anchor source, evaluated asset
   path, expression-vars **content fingerprint** — pointer identity never hits
   because arc crossings `make_shared` a fresh dict per instance). Successes
   are memoized; failures re-run so per-instance diagnostics are unchanged.
   Cleared with the spec cache on InvalidateLayer.

Measured (serial `next_usdcat -f`, warm cache, all byte-identical): Island
load+compose 26.5→22.8 s (−14%, instance-register 322→16 ms), ALab 495→415 ms
(−16%), Caldera 4.1→3.5 s (−14%). Peak RSS flat (write-phase dominated).
Parallel compose (8 threads) byte-identical to serial and TSan-clean.

Post-conversion profile (fp-unwound): the remaining Island "sources"-phase cost
is first-load *parsing* of referenced layers attributed under
GetOrLoad→ProcessArc (parse-side, parallelized by `--compose-threads`), plus
small residues in ApplyVariantOption and ExpandArcs. The pcp string-map lever
is now exhausted on dev too; further compose wins are parse- or
allocator-side.
