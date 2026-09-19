# LightUSD Next, Next IO, and Tydra Next

This note explains the "next" stack in LightUSD and how it is used by the web
`usdzconvert` pipeline.

## Terminology

LightUSD currently has two USD implementations:

- **Legacy core**: the production `src/` Stage/Prim implementation used by the
  original C++ API, Tydra, Python bindings, and most demos.
- **Next core**: the newer implementation under `src/next/`. It is a cleaner,
  lower-dependency USD core with flat layer storage, runtime type dispatch,
  lazy USDC arrays, and a lower-memory flatten/write path.

In this document:

- **next-core** means the in-memory USD model in `src/next/`: `Stage`, `Layer`,
  `PrimSpec`, `Value`, property indices, schema helpers, evaluation, and PCP
  composition.
- **next-io** means next-core readers/writers: USDA, USDC/Crate, USDZ package
  loading, lazy array decode, and streaming crate write.
- **tydra-next** means the render conversion layer in `src/tydra/next/`, which
  converts a `lightusd::next::Stage` into render-ready data.

The next stack is the default document and render-conversion path for
`lusdview`, and the default material-resolution path for `lusdrender`. Legacy
paths remain available as explicit compatibility modes.

## next-core

`src/next/` is a standalone next-generation LightUSD core. Its main public
header is:

```cpp
#include "next/lightusd-next.hh"
```

The design differs from the legacy core in several important ways:

- A runtime `TypeId`/`Value` system replaces much of the template-heavy legacy
  value dispatch.
- `Layer` stores prims in flat vectors with indices, which is cache-friendly and
  avoids pointer-heavy tree ownership.
- `PrimSpec` is the main authored prim record; `UsdPrim` is a lightweight handle.
- Property lookup uses an indexed property table rather than repeated string
  scans.
- USDC numeric arrays can stay lazy: the reader can retain references to source
  crate ranges instead of eagerly decoding every large POD array.
- Time sample storage includes value deduplication.

Basic C++ load/write example:

```cpp
#include "next/lightusd-next.hh"

int main() {
  lightusd::next::Stage stage;
  std::string warn;
  std::string err;

  lightusd::next::LoadUSDOptions opts;
  opts.limits.max_resident_bytes = 1024ull * 1024ull * 1024ull;

  if (!lightusd::next::LoadUSDComposed("scene.usdc", &stage, opts, &warn, &err)) {
    // handle err
    return 1;
  }

  if (!lightusd::next::WriteUSDC(stage, "flattened.usdc", &err)) {
    // handle err
    return 1;
  }

  return 0;
}
```

For interactive applications, keep a `StageSession` alive instead of repeatedly
calling the one-shot loader. The session retains the resolver and PCP layer
cache across payload and variant edits:

```cpp
lightusd::next::StageSession session;
lightusd::next::StageSessionOptions options;
options.composition.load_payloads = false;
options.progress_callback = [](const lightusd::next::ProgressEvent& event) {
  return !ApplicationRequestedCancel(event);
};

if (!session.OpenFile("scene.usdc", options)) return 1;
lightusd::next::StageSnapshot old = session.GetSnapshot();
lightusd::next::StageEditResult edit =
    session.SetVariantSelection("/World/Model", "lod", "high");
if (!edit) return 1;
edit = session.LoadPayload("/World/Model");
if (!edit) return 1;
lightusd::next::StageSnapshot current = edit.snapshot;
```

All public next loaders default to `InputPolicy::Untrusted` and finite
`ResourceLimits`. Zero-valued limits are rejected. Trusted applications that
deliberately need no shared load/session/render ceiling must opt in with
`InputPolicy::Trusted` and `ResourceLimits::Unlimited()`; lower-level
format-specific structural guards remain in force unless separately raised.

Variant overrides are scoped by prim path, so identically named variant sets on
different prims do not interfere. Diagnostics and deferred-payload state remain
available on the session after each rebuild. Successful edits atomically publish
a monotonically revisioned, immutable `StageSnapshot` and a typed
`StageChangeSet`; retained older snapshots remain coherent. `ReloadLayer()`
re-reads an edited dependency and publishes the resulting stage transactionally.
Array-backed geometry remains copy-on-write across snapshots.
Concurrent snapshot reads are supported with one serialized writer. Mutations
attempted reentrantly from progress, preview, or render-sink callbacks fail with
`OperationStatus::Busy`. `CloseAndTakeStage()` transfers without cloning only
after all published snapshots have been released.

Persistent render consumers can feed those snapshots and change sets to
`tydra::next::RenderSession`. Its `SceneUpdateSink` transaction reports stable
resource IDs plus typed removals/upserts, allowing a renderer to retain GPU
resources across payload, variant, and layer edits.

## next-io

The next IO layer is the reader/writer portion of `src/next/`:

- `reader/usda-reader.*`
- `reader/usdc-reader.*`
- `reader/usdz-reader.*`
- `crate/crate-reader.*`
- `crate/crate-writer.*`
- `pipeline/flatten.*`

The main performance feature is the lazy USDC path:

1. The USDC reader can keep large numeric arrays as lazy references into the
   retained input crate buffer.
2. Composition/flattening can preserve those lazy references when the values do
   not need semantic rewriting.
3. The crate writer can pass those arrays through verbatim.
4. The streaming writer can emit the output crate to a sink without building the
   whole output in a second large buffer.

This is the reason the web `--pipeline next --stream-write` path can keep large
single-USDZ flattened rewrites below the wasm32 2 GB heap limit. The input crate
is retained once, lazy arrays point into it, and the flattened root crate can be
streamed into the USDZ archive.

## Tydra Next

Tydra is LightUSD's USD-to-render-data layer. The legacy Tydra path converts the
legacy `Stage` into `tydra::RenderScene`. Tydra Next does the same job for
`lightusd::next::Stage` and lives under:

```text
src/tydra/next/
```

Important files:

- `render-data.hh`: GPU-oriented render scene, mesh, material, texture, light,
  camera, skeleton, and animation data.
- `scene-access.hh`: traversal and query helpers for `next::Stage`.
- `render-extract.hh`: scene extraction helpers.
- `render-converter.hh`: `RenderSceneConverter`, the main conversion entry.
- `materialx.hh`: MaterialX/OpenPBR support for the next render path.

Tydra Next is designed for fewer intermediate copies:

- mesh arrays use chunked storage,
- conversion can extract directly from next prim/value data,
- render data is shaped for WebGL/WebGPU/OpenGL/Vulkan style upload,
- texture loading can be supplied by a host callback.

Large applications can use `RenderSceneConverter::ConvertToSink()` with a
`SceneSink`. It emits geometry one prim at a time while retaining only the
lightweight scene catalog, avoiding a second full copy of mesh/point/curve data.

Shared render conversion currently covers meshes, analytic primitives, TetMesh
boundary surfaces, points, BasisCurves, NurbsCurves, HermiteCurves, materials,
lights, cameras, skeletons, animations, and point instancers. TetMesh conversion
cancels shared tetrahedron faces and retains only the external triangle surface.
Volume field loading remains application-owned because the host supplies the
OpenVDB decoder and GPU representation. NurbsPatch remains an explicit
unsupported renderable in tydra-next rather than being silently triangulated
with lossy assumptions.

## Application Defaults

- `lusdview`: next-core and tydra-next are used by default. Use
  `--legacy-load` only for compatibility investigation. `--next` remains an
  accepted no-op for existing scripts.
- `lusdrender`: next loading remains the primary render path and
  `-materialResolver tydra-next` is the default. Use
  `-materialResolver legacy` to compare against the former hand-written
  material resolver.
- Parent-relative asset paths are enabled because USD resolves them relative to
  the authoring layer. Sandboxed hosts can disable them through
  `ResolverConfig::allow_parent_paths`.

Minimal C++ conversion sketch:

```cpp
#include "next/lightusd-next.hh"
#include "tydra/next/render-converter.hh"

int main() {
  lightusd::next::Stage stage;
  std::string warn;
  std::string err;

  if (!lightusd::next::LoadUSDComposed("scene.usdc", &stage, &warn, &err)) {
    return 1;
  }

  lightusd::tydra::next::ConverterConfig config;
  config.mesh.triangulate = true;
  config.mesh.compute_normals = true;
  config.material.load_textures = true;
  config.progress_callback = [](float progress, const std::string& message) {
    // report progress to UI/log
  };

  lightusd::tydra::next::RenderSceneConverter converter(config);
  auto result = converter.Convert(stage);
  if (!result.success) {
    // handle result.error
    return 1;
  }

  const lightusd::tydra::next::RenderScene& scene = result.scene;
  // upload scene meshes/materials/textures to renderer
  return 0;
}
```

## How This Maps to `usdzconvert`

The web converter exposes four flatten/package modes:

| Mode | CLI/UI value | Core path | Best use |
|------|--------------|-----------|----------|
| Legacy | `legacy` | legacy core + legacy writer | Stable default, broad feature coverage. |
| Next | `next` | next-core/next-io for a single top-level USDC-root `.usdz` | Low-memory flattened rewrite of existing USDZ. |
| Stream | `stream` | legacy compose/write with lazy source fetching | Folder or URL-list input where textures should not be loaded all at once. |
| Stream + Next | `stream-next` | streaming source + next multi-layer flatten | Folder or URL-list input with USDC layers, low-memory root flatten, streamed textures. |

Browser UI:

```bash
cd web/js
npm run dev -- --host 127.0.0.1 --port 5174
# open http://127.0.0.1:5174/usdzconvert.html
```

Node CLI:

```bash
cd web/js
node cli/usdzconvert.js <input-dir-or-file> -o out.usdz --pipeline <mode>
```

The browser demo and CLI both call the same library in
`web/js/src/usdzconvert.js`:

- `convertFolderToUSDZ(native, assetMap, opts)` for in-memory folder/file input.
- `convertSourceToUSDZStreaming(native, source, opts)` for lazy folder/URL-list
  input.

## `usdzconvert` Examples

### 1. Stable default conversion

Use legacy mode when you want the broadest feature coverage and the scene is not
too large for in-memory processing.

```bash
cd web/js
node cli/usdzconvert.js ./model_folder \
  -o model.usdz \
  --pipeline legacy \
  --resize 1024 \
  --texture-format png \
  -v
```

### 2. Low-memory rewrite of a single USDZ

Use next mode for a single `.usdz` whose root layer is a top-level USDC file.
This is the lowest-memory path for flattening an existing package while keeping
textures unchanged.

```bash
cd web/js
node cli/usdzconvert.js input.usdz \
  -o output.usdz \
  --pipeline next \
  --no-reencode \
  --stream-write \
  --max-usdc-mb 1024 \
  --max-mem-mb 2048 \
  -v
```

Notes:

- `--stream-write` is the default for next mode when writing to a file, but
  spelling it out documents the intended path.
- If the input does not qualify, the converter logs that the next path declined
  and falls back to a legacy path.
- Texture processing hooks can disable some low-heap shortcuts because decoded
  texture data must then be materialized.

### 3. Streaming folder conversion

Use stream mode when the input is a folder or URL list and the main risk is
loading all textures into JS/WASM memory at once.

```bash
cd web/js
node cli/usdzconvert.js ./scene_folder \
  -o scene.usdz \
  --root root.usdc \
  --pipeline stream \
  --resize 512 \
  --texture-format png \
  --texture-codec best \
  --texture-memory-budget 1GB \
  -v
```

The stream path preloads USD layers for composition, then processes textures
through a bounded fetch/process/zip loop. Texture concurrency is capped at 64
even for malformed or extreme API inputs; `--texture-memory-budget` normally
selects a much smaller count from the conservative per-worker estimate.

### 4. Streaming folder conversion with next flatten

Use stream-next for folder or URL-list inputs with USDC layers when you want both
lazy texture packaging and the next multi-layer compose/flatten path.

```bash
cd web/js
node cli/usdzconvert.js ./scene_folder \
  -o scene-next.usdz \
  --root root.usdc \
  --pipeline stream-next \
  --resize 512 \
  --texture-format png \
  --texture-codec best \
  --texture-memory-budget 1GB \
  --max-usdc-mb 2048 \
  --max-mem-mb 2048 \
  -v
```

By default, stream-next packages only texture assets referenced by the composed
root. To include unreferenced texture files from the input folder:

```bash
node cli/usdzconvert.js ./scene_folder \
  -o scene-next-all-textures.usdz \
  --root root.usdc \
  --pipeline stream-next \
  --include-unused-textures
```

### 5. URL-list streaming source

For non-browser batch jobs, a URL manifest can be used instead of a local folder.

```json
{
  "baseUrl": "https://example.invalid/assets/scene/",
  "files": [
    "root.usdc",
    "materials.usdc",
    "textures/albedo.png",
    "textures/normal.png"
  ]
}
```

```bash
cd web/js
node cli/usdzconvert.js \
  --url-list manifest.json \
  --root root.usdc \
  -o scene.usdz \
  --pipeline stream-next \
  --texture-format keep \
  --no-reencode
```

## Programmatic JS Examples

### In-memory conversion

```js
import {
  loadWasm,
  convertFolderToUSDZ,
} from './src/usdzconvert.js';

const native = await loadWasm(() => import('./src/lightusd/lightusd.js'));

const assetMap = new Map();
assetMap.set('scene.usdz', inputBytes);

const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
  rootPath: 'scene.usdz',
  pipeline: 'next',
  reencode: false,
  textureFormat: 'keep',
  streamWrite: true,
  maxUsdcMb: 1024,
  maxMemMb: 2048,
  progress: ({ stage, current, total, message }) => {
    console.log(stage, current, total, message);
  },
  log: console.log,
});
```

### Streaming source conversion

```js
import {
  loadWasm,
  convertSourceToUSDZStreaming,
} from './src/usdzconvert.js';

const native = await loadWasm(() => import('./src/lightusd/lightusd.js'));

const source = {
  keys: ['root.usdc', 'materials.usdc', 'textures/albedo.png'],
  async fetch(key) {
    const res = await fetch(`/assets/${key}`);
    if (!res.ok) throw new Error(`fetch failed: ${key}`);
    return new Uint8Array(await res.arrayBuffer());
  },
};

const { usdz, stats } = await convertSourceToUSDZStreaming(native, source, {
  rootPath: 'root.usdc',
  pipeline: 'next',
  nextPreloadUsdLayers: true,
  textureFormat: 'png',
  maxTextureSize: 512,
  includeUnusedTextures: false,
  progress: ({ stage, current, total, path }) => {
    console.log(stage, current, total, path);
  },
  log: console.log,
});
```

For browser folder upload, `web/js/usdzconvert.js` uses this same streaming API
with `File.arrayBuffer()` as the source fetch function.

## Choosing a Pipeline

Use this rule of thumb:

- Use **legacy** first when correctness coverage matters more than memory.
- Use **next** for a single packaged `.usdz` with a top-level USDC root and
  unchanged textures.
- Use **stream** for folder/URL-list input when texture memory is the bottleneck
  but the legacy flatten path is acceptable.
- Use **stream-next** for large folder/URL-list input with USDC layers when you
  need next-core flattening plus streaming texture packaging.

The next paths are most effective when:

- the root USD is USDC,
- large arrays can pass through lazily,
- the output root is USDC,
- texture processing is bounded or skipped,
- streaming write is enabled.

They are less effective or may fall back when:

- the root is USDA and must be parsed into authored values,
- the package has a nested root layout not supported by the optimized path,
- texture or audio hooks force all assets through host-side processing,
- a feature has not yet been implemented in next-core/next-io.

## Testing

Next standalone checks:

```bash
scripts/run-next-checks.sh
ctest --test-dir build-next --output-on-failure -L next -LE 'benchmark|corpus'
```

Web converter tests:

```bash
cd web/js
node tests/test-usdzconvert.js
node tests/usdzconvert-next.test.mjs
```

Browser smoke test:

```bash
cd web/js
npm run dev -- --host 127.0.0.1 --port 5174
# Open /usdzconvert.html, select a folder or USDZ, choose pipeline, Convert.
```

For large-scene memory notes, see `web/js/docs/wasm-heap.md`.
