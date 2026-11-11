# WebAssembly Heap Notes

This document tracks current heap behavior for large USDZ conversion paths and
possible reductions. It is written from the `large_sample.usdz` investigation on
2026-06-05.

## Current Situation

Test asset:

- Input: `/path/to/large_sample.usdz`
- Input size: `214034431` bytes, about 205 MB
- Archive contents: 93 entries, root layer `large_sample.usdc`

The plain no-transform USDZ-to-USDZ path now has a narrow passthrough fast path
in `web/js/src/usdzconvert.js`. When the input is a single `.usdz`, textures are
kept unchanged, no resize/target budget/ARKit rewrite is requested, and
`--no-reencode` is used, the converter writes the original archive bytes instead
of unpacking and loading the scene into the WASM heap.

Observed wasm32 passthrough result:

```console
node web/js/cli/usdzconvert.js \
  /path/to/large_sample.usdz \
  -o /tmp/large_sample.wasm.usdz \
  --no-reencode --max-usdc-mb 1024 --max-mem-mb 2048 -v
```

- Output: `/tmp/large_sample.wasm.usdz`
- Output size: `214034431` bytes
- Output SHA-256: identical to input
- Max RSS: `272656 KB`
- Result: succeeds without approaching the wasm32 2 GB heap limit

The full flattened JS/WASM path is different. It unpacks the USDZ, loads the
USDC layer, composes, flattens, writes a new USDC root layer, and packages a new
USDZ. For single-USDZ inputs with unchanged textures/audio/assets, the converter
now uses a low-heap root-rewrite path: only USD dependency layers are registered
with the WASM resolver, the flattened root layer is exported directly into a
JavaScript `Uint8Array`, and non-root archive entries are copied from the
original USDZ into a newly written USDZ.

Observed wasm32 low-heap flattened result:

```console
node web/js/cli/usdzconvert.js \
  /path/to/large_sample.usdz \
  -o /tmp/large_sample.wasm32_lowheap_direct.usdz \
  --no-reencode --max-usdc-mb 1024 --max-mem-mb 2048 -v
```

- Output: `/tmp/large_sample.wasm32_lowheap_direct.usdz`
- Output size: `535500713` bytes, about 511 MB
- Root layer: `root.usdc`
- Rewritten root USDC size: `402962737` bytes, about 384 MB
- Copied non-root entries: 92
- Final measured WASM heap after conversion: `408485888` bytes, about 390 MB
- Max process RSS: `1573400 KB`, about 1.57 GB
- Result: succeeds on wasm32 and stays below the 1 GB heap target
- Archive integrity: `unzip -t /tmp/large_sample.wasm32_lowheap_direct.usdz`
  passed

Observed wasm32 flattened result before passthrough was added:

- Result: failed during load/export
- Error included a failed heap growth request around `2357128952` bytes
- wasm32 limit: `2147483648` bytes
- Max process RSS near failure: about `1.74 GB`

Observed wasm32 low-heap flattened result before direct-to-JS USDC export:

- Output: `/tmp/large_sample.wasm32_lowheap_flat.usdz`
- Output size: `535500713` bytes, about 511 MB
- Final measured WASM heap after conversion: `1308622848` bytes, about 1.22 GB
- Max process RSS: `2428952 KB`, about 2.43 GB
- Result: succeeded, but missed the 1 GB heap target because the generated root
  USDC was still retained in a WASM-side export vector

Observed wasm64 flattened result:

```console
cmake --build web/build_64_ninja --target lightusd_64.js
```

Then force the shared converter through the flattened path by importing
`web/js/src/lightusd/lightusd_64.js` and passing:

```js
{
  rootPath: 'large_sample.usdz',
  passthroughUsdz: false,
  reencode: false,
  textureFormat: 'keep',
  rootLayerFormat: 'usdc',
  arkitCompatible: false,
  flatten: true,
  maxUsdcMb: 1024,
  maxMemMb: 4096
}
```

- Output: `/tmp/large_sample.wasm64_flat.usdz`
- Output size: `513131407` bytes, about 490 MB
- Convert time: `23843 ms`
- Max process RSS: `2765440 KB`, about 2.77 GB
- Largest logged WASM heap growth: `2373320704` bytes, about 2.37 GB
- Result: succeeds under wasm64 and stays below 4 GB process RSS for this asset
- Archive integrity: `unzip -t /tmp/large_sample.wasm64_flat.usdz` passed

A native `lusdcat /tmp/large_sample.wasm64_flat.usdz > /dev/null` parse/print
attempt was stopped after roughly 90 seconds with no stderr. Treat that as
inconclusive, not as a parse validation pass.

## Why Flattening Peaks High

The flattened path currently pays for several large live data sets at once:

- the original USDZ archive bytes in JS memory
- subarray views for unpacked archive entries
- registered asset bytes copied into the WASM asset cache through `setAsset`
- the parsed root layer and composed scene data
- flattened/export-side crate data and value tables
- the generated USDC root layer bytes
- the generated USDZ archive bytes copied back out to JS

The recent `subarray` change avoids copying every USDZ member during JS unpack,
and the no-transform passthrough avoids the whole flattened pipeline. It does
not reduce the memory needed by a true flattened rewrite where a new root layer
must be generated.

## Practical Guidance

Use wasm32 for normal files, no-transform USDZ passthrough, and single-USDZ
flattened root rewrites where textures/audio/assets are unchanged. Use wasm64
for large flattened rewrites that need texture processing, ARKit rewrites,
multi-root packaging, or other paths that still route large payloads through the
WASM asset cache/export buffers.

For CLI users (see the 2026-06-09 section for the lowest-memory flattened path):

- For single-`.usdz` flattened rewrites, prefer `--pipeline core --stream-write`
  (with `--no-reencode`). This is the lowest-RSS path on wasm32 and keeps the
  whole 28-scene corpus under 1 GB without needing wasm64.
- Use `--no-reencode` for unchanged USDZ packaging. This enables the passthrough
  path when no other transform is requested.
- Use `--no-reencode` with default flattening for the low-heap root-rewrite
  path. This creates a new flattened USDZ while copying unchanged non-root
  entries from the original archive.
- Use the wasm64 build for large flattened exports. The build target is
  `lightusd_64.js`; the web build uses `-sMEMORY64 -sMAXIMUM_MEMORY=8GB`.
- Keep `--max-usdc-mb` and `--max-mem-mb` explicit for large test scenes so the
  writer caps are not the first failure mode.

## 2026-06-09 update: `core` low-memory pipeline + streaming crate writer

This section supersedes the 2026-06-05 status for single-`.usdz` flattened
rewrites. With the `core` lazy-ValueRep pipeline and the streaming crate writer,
**every scene in the 28-asset test corpus now converts under 1 GB process RSS on
wasm32, byte-identical to the buffered output, with zero OOM** — including the
~200–300 MB geometry/texture-heavy scenes (`geometry-scene`, `single-mesh-scene`,
`materialx-scene-1`, `textured-scene-2`) that previously peaked at 1.3–1.9 GB.

Enable it with `--pipeline core --stream-write`:

```console
node web/js/cli/usdzconvert.js \
  input.usdz -o out.usdz \
  --pipeline core --no-reencode --stream-write \
  --max-usdc-mb 1024 --max-mem-mb 2048
```

### Strategy (four layers, applied in order)

1. **`core` lazy-ValueRep pipeline** (`src/`, opt-in via `--pipeline core`).
   Load → compose → write all share one retained copy of the input crate. Numeric
   POD arrays are kept as `LazyArrayRef`s (a `shared_ptr` to the source buffer +
   offset), never decoded, and on write are copied verbatim from the source
   ("pass-through"). A 200 MB scene stays close to input size in heap instead of
   the 5–10x blow-up of the eager typed-stage path.

2. **Configurable zero-copy input cap (512 MiB default).** The root crate is
   streamed into a single wasm zero-copy buffer. The cap was 256 MiB, which a
   geometry-heavy root (e.g. `geometry-scene`'s 276 MB single `.usdc`) exceeded —
   declining the `core` path and silently falling back to the high-memory legacy
   flatten. Raised the default to 512 MiB and made it caller-settable via
   `--max-mem-mb` (`binding.cc allocateZeroCopyBuffer(name, size, max_bytes)`).

3. **Arc-free Compose skip.** When the root layer has no sublayers and no per-prim
   composition arcs it flattens to itself, so the structural clone `Compose()`
   would do is skipped (`FlattenLoaded` / `IsSelfContained`). Verified
   byte-identical. Note this is a *clarity/CPU* win, not a memory one — see the
   attribution below.

4. **Streaming crate writer (the decisive memory lever).** The writer emits the
   crate to a sink in file order without ever materializing the full output. See
   "Streaming crate writer" below.

### Phase-0 attribution: the write stage is the entire peak

Built with `-DLIGHTUSD_FLATTEN_MEMLOG` (compile-gated; an env gate can't be used
because emscripten `getenv()` does not see `process.env`), the flatten logs the
wasm linear-heap high-water — which is monotonic, so per-stage deltas attribute
the peak. Measured (32-bit wasm, `--pipeline core`, buffered writer):

| scene             | after-read | after-compose | after-write    |
|-------------------|-----------:|--------------:|---------------:|
| geometry-scene      |    318 MiB |   318 (Δ0)    | 676 (**+358**) |
| materialx-scene-1      |    279 MiB |   279 (Δ0)    | 612 (**+333**) |
| single-mesh-scene   |    188 MiB |   188 (Δ0)    | 424 (**+236**) |
| textured-scene-1        |     94 MiB |    94 (Δ0)    | 173 (**+79**)  |

Two firm conclusions: **Compose adds ~0** to the heap high-water (lazy arrays are
shared), and **the write stage is ~100% of the peak growth** — the buffered
writer builds the whole output crate (`buffer_`), `WriteLayerToMemory` then
*copies* it into the caller's `out`, and the binding `toOwnedUint8Array(out)`
copies it again into JS. JS-side trimming was ruled out empirically: nulling all
JS input + forcing GC left RSS unchanged (wasm heap never shrinks back to the OS,
and the cost is wasm-side).

### Streaming crate writer

`CrateWriter::WriteLayerToSink(sink, layer)` (behind `CrateWriteOptions::streaming`)
emits the crate to a `CrateWriteSink` (`bool(const uint8_t*, size_t)`) in file
order — bootstrap, VALUE section, structural sections, TOC — without holding the
full output. `WriteValueSection` is split into:

- `ComputeValueLayoutAndPatch()` — pure bookkeeping: 8-byte-aligned block offsets
  + patches every field/timesample `ValueRep` with its absolute offset. Returns
  the VALUE section size.
- `EmitValueBytesToBuffer()` (in-memory) / `StreamValueBytes(sink)` (streaming) —
  the latter streams each block straight from its source ref (the retained crate
  for pass-through arrays, or the re-encoded block).

Only the small structural tail is staged in `buffer_`; structural-section content
is position-independent, so it is built at physical offset 64 and its TOC offsets
carry a `struct_base = value_section_size` shift for the logically-spliced (and
physically absent) value section. Because the TOC and bootstrap offsets are
precomputed, **no streamed byte is ever back-patched** — so the output can stream
straight into a forward-only zip entry. Verified byte-identical to the buffered
writer across 12 scenes incl. re-encode and timesample/anim cases.

Measured wasm flatten heap (memory vs streaming writer):

| scene        | buffered writer | streaming writer |
|--------------|----------------:|-----------------:|
| geometry-scene |         677 MiB |      **318 MiB** |
| materialx-scene-1 |         612 MiB |      **335 MiB** |

### Streaming the root into the `.usdz` (`--stream-write`)

The wasm win is only realized end-to-end if the root crate also avoids being
buffered in JS. `ZipStreamWriter.addEntryStreaming(name, streamFn)` emits the
root entry's local header with placeholder CRC/size, streams the data (computing
CRC32 incrementally), then **patches** the 12-byte CRC/size field — which needs a
seekable sink. The CLI provides a `{ write, patch }` fd sink (all writes use an
explicit position). `flattenBufferToSink(uuid, lazy, chunkCb)` forwards
ordered chunks from the C++ writer as transient wasm-heap views (JS copies them
synchronously into the CRC and the fd). The flattened root therefore never
materializes — not in the wasm heap, not in JS.

This removes, together: the full output `buffer_` in wasm, the
`WriteLayerToMemory` copy, the `toOwnedUint8Array` JS copy, and the JS root
buffer held during repack.

### Result: full 28-scene corpus (wasm32, `--pipeline core --stream-write`)

Byte-identical to the buffered output, 0 OOM, **all 28 under 1 GB RSS**. Peak RSS
roughly halves on the large scenes:

| scene                   | in MB | buffered RSS | **stream-write RSS** |
|-------------------------|------:|-------------:|---------------------:|
| geometry-scene            |   264 |     1320 MB  |          **617 MB**  |
| single-mesh-scene         |   294 |     1005 MB  |          **518 MB**  |
| materialx-scene-1            |   135 |     1014 MB  |          **508 MB**  |
| textured-scene-2 |   269 |      732 MB  |          **447 MB**  |
| textured-scene-1              |   204 |      493 MB  |          **368 MB**  |
| materialx-scene-1b                 |    84 |      594 MB  |          **292 MB**  |

`--stream-write` is the **default** for the `core` pipeline when writing to a
file (it transparently falls back to buffering without a seekable sink). Disable
with `--no-stream-write` or `LIGHTUSD_STREAM_WRITE=0`. Output is byte-identical
either way; streaming only lowers memory.

Relevant commits: zero-copy cap (`4abf2ed87`), arc-free skip + attribution aid
(`7212e1ab9`), streaming crate writer (`2a68ac062`), `--stream-write` end-to-end
(`9c2d59b6b`).

## 2026-06-09 update: incremental render-data converter (RenderStream)

`web/js/streaming.js` is a self-contained raw-WebGL2 USD/USDZ renderer (no
Three.js) whose target is to **render a scene while keeping the WASM linear heap
near the input crate size** — not the 5–10x of the eager
`loadFromBinary()` → typed Stage → RenderScene path.

### Why the eager path can't hit the target

The eager render path converts the WHOLE scene at once (parse a typed Stage,
then build the RenderScene), so during the load it holds input + Stage + render
data simultaneously. Measured (32-bit wasm, `getMemoryStats()`/heap):
`geometry-scene` 264 MB crate → 1544 MB peak (5.85x); `materialx-scene-1` 135 MB →
1406 MB (10.4x). emscripten never returns grown heap to the OS, so process RSS
stays at that peak even after `reset()`. No JS trick fixes it — the cost is
WASM-side. (Textures are already kept ENCODED in the heap via
`setLoadTextureInNative(false)` and decoded per-texture in JS, so decoded RGBA
never lands in the WASM heap; the geometry conversion is the blow-up.)

### The incremental converter

A `RenderStream` binding (`web/binding.cc`) drives the `core` pipeline's LAZY
crate:

1. JS extracts the root `.usdc` from the `.usdz` and streams ONLY it into WASM
   (`RenderStream.begin`); the archive + texture entries stay in JS, off the
   WASM heap. The crate sits in the heap exactly once (`ReadOwned`, lazy arrays).
2. `getMesh(i)` materializes ONE mesh's geometry on demand and returns zero-copy
   `{points,indices,normals,uv0}` descriptors + a resolved UsdPreviewSurface /
   MaterialX material (base color / metallic / roughness / emissive / occlusion +
   texture asset paths). Arrays are read through a COPY of the lazy `Value`
   (`Value tmp = *v; tmp.as_float_array()`), so the Stage's own property stays
   lazy and the per-mesh decode never accumulates across meshes — at most one
   mesh is resident at a time.
3. JS uploads each mesh to a GL buffer before the next `getMesh()` (the scratch
   is overwritten), decodes referenced archive textures with `createImageBitmap`
   (off the WASM heap) → GL textures, then `end()`s to free the crate.

Peak WASM heap = crate + the largest single mesh, instead of input + whole Stage
+ whole RenderScene.

### Closing the texture gaps (next-reader)

Two next-model gaps blocked textured output in this path; both are fixed:

- **Float2 (vec2f) arrays.** The next `Value`/reader only handled `Float`/`Float3`
  arrays, so `primvars:st` (UVs) decoded to nothing. Added `MakeFloat2Array`,
  `Float2` acceptance in `as_float_array`/destructor/copy/equals/hash, a `Vec2f`
  case in the (lazy + eager) crate reader, and `Vec2f` materialization. UVs now
  work — and next-pipeline flatten preserves them (it was silently dropping them).
- **Texture connection resolution.** `AttributeEval::GetConnectionPath` read the
  attribute's value as a string instead of the parsed connection targets; it now
  uses `PrimSpec::connection()`. `RenderStream` resolves each UsdUVTexture
  connection to its `inputs:file` asset path, which JS maps to an archive entry.

### Render-mesh prep: de-index + weld

When UVs/normals are face-varying or separately indexed, a mesh must be
de-indexed so each triangle corner carries its own attributes. The naive
non-indexed soup (3·triangles corners) spiked the per-mesh peak, so `getMesh`
WELDS inline — one vertex per distinct (pos[,uv][,normal]) tuple — recovering
vertex sharing while keeping seams split. Whether to weld or keep the soup is
decided per mesh by POSITION sharing (`vtxCount*3 < corner count`); face-varying
is NOT a reliable signal (the heaviest scenes are face-varying and weld well).

### Corpus result (incremental path, peak WASM heap / crate)

| scene             | crate  | eager   | **incremental** | UV / textured     |
|-------------------|-------:|--------:|----------------:|-------------------|
| geometry-scene      | 264 MB |  5.85x  |     **1.21x**   | 255 / —           |
| textured-scene-1        |  78 MB |    —    |     **1.46x**   | 89 / 96 textured  |
| textured-scene-2|  97 MB |    —    |     **1.46x**   | 18 / 18 textured  |
| textured-scene-3   |  77 MB |    —    |     **2.5x**    | 65 / 65 textured  |
| single-mesh-scene   | 155 MB |  6.18x  |     **2.51x**   | 3 / 3 textured    |
| materialx-scene-1      | 133 MB | 10.43x  |     **3.62x**   | 646 / 640 textured|

27/28 corpus scenes drive through it (the 1 is a non-USDC root → eager fallback);
all geometry + UVs + materials + textures resolve, every texture path matches an
archive entry. Small scenes are dominated by the ~17 MB wasm baseline (ratios not
meaningful). Tiny remaining gap from the eager path is acceptable; geometry-heavy
scenes hit ~1.2–1.5x. `streaming.js` defaults to this path; `?eager=1` uses the
eager textured path for non-USDC roots.

Relevant commits: streaming renderer (`3b6379a57`), incremental RenderStream
(`f4f0faa45`), Float2 + connection fixes (`d9b9c268f`), inline weld
(`b5150fb9e`), position-sharing weld heuristic (`d3f02c289`).

## Possible Memory Reduction Enhancements

### Stream USDZ Output — IMPLEMENTED (2026-06-09)

Done for the `core` pipeline via the streaming crate writer
(`CrateWriter::WriteLayerToSink`) + `flattenBufferToSink`. See the 2026-06-09
section above. The output crate is emitted to a sink in file order and never
fully materialized in wasm or JS. The CLI sink writes incrementally to the output
fd; a browser `WritableStream` sink would slot in the same way.

### Stream USDC Root Into USDZ — IMPLEMENTED (2026-06-09)

Done via `ZipStreamWriter.addEntryStreaming` + a seekable `{ write, patch }`
sink (`--stream-write`). The generated `root.usdc` streams straight into the
`.usdz` as the crate writer produces it, so the generated root and the archive
bytes never coexist. See the 2026-06-09 section above.

The STORE/64-byte-alignment caveat was handled by **patching the local header**
rather than ZIP data descriptors: the entry header is emitted with placeholder
CRC/size, CRC32 is accumulated as chunks stream, and the 12-byte CRC/size field
is patched afterward (requires a seekable sink). This keeps the archive a plain
forward-readable STORE zip — byte-identical to the buffered build.

### Avoid Asset Cache Copies

`setAsset(name, bytes)` moves JS `Uint8Array`/`Buffer` data into the WASM-side
asset resolver cache. For large USDZ inputs this duplicates texture bytes that
already exist in the original archive buffer.

Possible approaches:

- let the exporter read passthrough asset bytes from JS by callback at package
  time
- store asset references as `(archive_buffer_id, offset, size)` instead of
  copying bytes into WASM
- add a zero-copy or borrowed-asset API with clear lifetime rules

Expected benefit: reduces peak memory for texture-heavy USDZ rewrites.

### Separate Texture Repack From Root Flatten

If textures are unchanged, the converter can preserve original asset entries and
only rewrite the root layer. This still creates a new USDZ, but the non-root
entries can be copied from original archive offsets without entering WASM.

Expected benefit: keeps image payloads out of the WASM heap for flattened root
rewrites with unchanged textures.

### Earlier Lifetime Release

After each phase, release data that is no longer needed:

- clear expanded USDZ maps after assets have been registered or referenced
- release raw input archive bytes after root load if no output-copy path needs
  original entries
- delete or clear resolver asset cache after output packaging
- copy output to JS only after deleting large loader/export temporaries where
  API shape permits

Expected benefit: may not reduce absolute writer requirements, but should lower
RSS and make failure modes less sensitive to allocator growth history.

### Reduce Stage/Layer Duplication During Flatten

The full path loads a layer, composes, converts/reconstructs stage data, then
writes crate data. There may be duplicated representations of large arrays and
timesamples during this pipeline.

Possible approaches:

- keep flattened export closer to `LayerSpec`/crate data when no typed stage
  rewrite is required
- avoid reconstructing large repeated timesample arrays
- preserve read-side shared offsets where possible and write repeated `ValueRep`
  entries directly

Expected benefit: reduces the root-layer side of peak memory, not just texture
payload duplication.

### Incremental Crate Value Writing

The dedup fix now reconstructs one timesample at a time and deduplicates per
attribute. Further reductions may be possible by avoiding temporary canonical
byte buffers for very large arrays when the array already has stable binary
storage.

Possible approaches:

- hash directly from existing binary sample refs
- use borrowed spans for dedup keys where lifetime is bounded by the attribute
  write
- cap dedup key retention for very large unique arrays

Expected benefit: smaller per-attribute transient memory for animated dense
arrays.

## Open Questions

- Should the Node CLI expose an explicit `--wasm64` option for `usdzconvert.js`,
  matching `load-test-node.js`?
- Should the browser converter automatically retry large flattened exports with
  wasm64 when supported?
- Should no-transform passthrough be the default only with `--no-reencode`, or
  also when texture processing options are omitted?
- What validation should be used for very large outputs when pretty-printing the
  entire layer is too slow for routine checks?
