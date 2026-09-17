# LightUSD Web Demos

24 interactive browser demos for USD loading, MaterialX, hair/fur, skinning, animation,
physics simulation, composition, export, and more.

## Quick Start

```bash
# Production path (no build tools needed)
npm install
npx vite build
npx vite preview          # or serve dist/ with any static server

# Development path (requires Emscripten + CMake)
npm install
npm run dev               # builds local WASM + starts Vite dev server
```

## Requirements

| Mode | Tools |
|------|-------|
| **Production** (npm package) | Node.js 18+, npm |
| **Development** (local WASM) | Node.js 18+, npm, CMake, Ninja, Emscripten (`emcmake`) |

## Development Path

The dev server (`npm run dev`) builds LightUSD WASM from source using the
local Emscripten toolchain, then serves the demo from the local source tree.

```bash
# One-time: verify Emscripten + CMake
emcmake --version && cmake --version && ninja --version

# Install JS dependencies
npm install

# Start dev server (incrementally builds WASM, then starts Vite)
npm run dev

# Or run the prepare step manually if you already built WASM
npm run prepare:local-lightusd
vite --mode development
```

### OpenChessSet MaterialX showcase

The full OpenChessSet asset is deliberately not committed. Prepare the pinned
CC BY 4.0 usd-wg asset before building the demo:

```bash
# Preferred local setup: repository-root usd-assets -> usd-wg/assets checkout
npm run prepare:openchess
USD_ASSETS_DIR=/path/to/usd-wg/assets npm run prepare:openchess
# Use -- --force to refresh an already prepared copy.
```

The preparation script first uses `USD_ASSETS_DIR`/`USD_WG_ASSETS_DIR`, then
the repository-root `usd-assets` symlink when present. If none is available it
performs a pinned sparse checkout.

The showcase defaults to lightweight WebGL2 raster rendering. It also exposes
LightRT CPU/WASM progressive path tracing and, when supported by the browser,
WebGPU raster and compute path tracing. Raster subsurface scattering is an
explicit approximation; the tracing modes are intended for slower reference
comparison. WebGL2 raster includes an optional screen-space depth-of-field pass
with orbit-target autofocus, click-to-focus USD primitive picking, manual focus,
focal-length, f-stop, maximum-blur, and resolution controls. The bundled Goegap HDRI is the default environment and
background. Lighting/background strength, blur, rotation, exposure, output
transform, and tone mapping are interactive; a lightweight ACES 2.0-style
display approximation is the default (with legacy ACES, AgX, and other curves
available for comparison).

### Hair and fur showcase

`hair-fur.html` renders USD `BasisCurves` as batched camera-facing ribbons in
WebGL2. Its lightweight Chiang/Principled Hair approximation provides R, TT,
and TRT lobes, absorption, IOR, and cuticle controls sourced from MaterialX
`ND_chiang_hair_bsdf`. Weighted blended transparency avoids sorting individual
strands. The default sample is generated in Blender 5.2.1 and contains
side-by-side `wStraight` and `wWavy` forms inspired by Cem Yuksel's public hair
models: 10,000 strands, 360,000 control points, and a 4.36 MiB USDC. Blender
exports Hair Curves natively. Since its MaterialX exporter reports Principled
Hair as unsupported, the generator installs a deterministic Chiang Hair export
hook before the final USDC conversion:

```bash
BLENDER=/path/to/blender npm run generate:blender-hair
npm run test:hair-demo
```

The smaller procedural fixture remains reproducible with
`npm run generate:hair-demo`. The demo also generates adjustable furball and
wind-animated grass scenes at runtime.

Implementation references: the MaterialX PBR specification's Chiang Hair BSDF,
NVIDIA's real-time hair course (camera-facing ribbon geometry), and McGuire and
Bavoil's *Weighted Blended Order-Independent Transparency* (JCGT 2013).

### MetaHuman WebGL2 showcase

`metahuman.html` is a WebGL2-only local-export viewer for the output from
`tools/ue-metahuman-usd`. Select **Open Export Folder** and choose the folder
containing `MetaHuman_Hero.usda`, or select **Open MetaHuman USDZ** for a
self-contained package. The demo resolves sibling head/body USD layers without
uploading or committing the character. Create a package that preserves those
composition arcs with `tools/ue-metahuman-usd/package-usdz.sh`. It uses the shared
UsdSkel GPU skinning and generic USD blendshape animation paths, MaterialX /
OpenPBR skin with a bounded wrapped-diffusion SSS approximation, and the
weighted-transparent R/TT/TRT groom ribbon renderer. The public
`assets/metahuman-fixture` is a deliberately tiny proxy used for browser QA,
not a MetaHuman asset.

DNA/RigLogic deformation and Unreal PhysicsAsset simulation are reported as
deferred. The original MetaHuman remains outside the repository; for the
offline Ada export, fitted face DNA requires the opt-in UE cloud auto-rigging
step described by the exporter documentation.

### How it works

1. `prepare-local-lightusd.sh` configures two CMake builds:
   - `web/build_ninja/` — legacy WASM (`lightusd.wasm` + `lightusd.js`)
   - `web/build_next_ninja/` — next WASM (`lightusd_next.wasm` + `lightusd_next.js`)
2. Build artifacts are copied to `web/js/src/lightusd/`
3. Vite serves JS modules from `web/js/src/lightusd/` via the `lightusd` alias
4. Hot-reload is available for JS changes; WASM changes require re-running
   `npm run prepare:local-lightusd`

### Bootstrap scripts

Pre-configured CMake bootstrap scripts in `web/`:

| Script | Description |
|--------|-------------|
| `bootstrap-linux-ninja.sh` | Legacy WASM only (fastest) |
| `bootstrap-linux-next-only.sh` | Next WASM only |
| `bootstrap-linux-demodev.sh` | Both legacy + next, dev-optimized |
| `bootstrap-linux-release.sh` | Release build (optimized) |
| `bootstrap-linux-debug.sh` | Debug build with assertions |
| `bootstrap-linux-wasm64.sh` | 64-bit WASM build |
| `bootstrap-macos-wasm64.sh` | macOS 64-bit WASM |

```bash
# Example: full dev build with both backends
bash web/bootstrap-linux-demodev.sh
```

## Production Path

The production build uses the pre-built `lightusd` npm package (no Emscripten
toolchain needed).

```bash
# Install everything
npm install

# Build to dist/
npm run build

# Preview the build locally
npm run preview

# Deploy: serve dist/ with any static file server
```

### npm dependencies

| Package | Purpose |
|---------|---------|
| `lightusd` | Pre-built WASM + JS loader (production) |
| `three` | 3D rendering (MeshPhysicalMaterial, etc.) |
| `lil-gui` | GUI controls (sliders, color pickers) |
| `@lighttransport/mujoco-wasm` | MuJoCo physics WASM |
| `fzstd` | Zstd decompression for compressed WASM |

### Production build notes

- WASM files are emitted to `dist/assets/` with content hashes
- The `lightusd` package is excluded from Vite's dependency pre-bundling
  (`optimizeDeps.exclude`) to ensure correct WASM path resolution
- COOP/COEP headers (`Cross-Origin-Opener-Policy: same-origin`,
  `Cross-Origin-Embedder-Policy: require-corp`) are required for
  `SharedArrayBuffer` support (used by WASM threading)
- `?backend=next` selects the next rendering backend (if available in build)

## QA Testing

```bash
# Automated smoke test of all 22 demo pages
node scripts/test-all-demos.mjs

# With xvfb (headless server)
xvfb-run -a node scripts/test-all-demos.mjs

# Custom Chrome path or timeout
CHROME_PATH=/opt/chrome/chrome TEST_TIMEOUT=60000 \
  node scripts/test-all-demos.mjs
```

The test script:
- Starts a Vite dev server
- Opens each demo page in headless Chrome
- Captures console errors and warnings
- Takes a screenshot for visual inspection
- Saves results to `dist/qa-report.json`
- Exits with code 1 if any page fails

## Demo Asset Info

UsdCookie.usdz : Each asset has a license declared in the readme, typically
CC0 or something highly permissive.

Images are resized to 1024×1024.

## USD Assets Browser

The interactive USD Assets Browser (`usd-assets.html`) lists 263 USD assets
from the [usd-wg/assets](https://github.com/usd-wg/assets) corpus, fetched at
runtime from `raw.githubusercontent.com`. Assets are grouped into 17 categories
with tag-based filtering.

### Asset manifest

The asset catalog in `src/usd-assets-manifest.js` is auto-generated by
`scripts/generate-asset-manifest.js`:

```bash
USD_ASSETS_DIR=/path/to/usd-wg/assets node scripts/generate-asset-manifest.js
```

### Batch preview generation

A Puppeteer-based batch runner in `../batch-runner/generate-previews.js`
renders preview images for all assets:

```bash
cd ../batch-runner
npm install
xvfb-run -a node generate-previews.js --output ./previews --timeout 180000
```

Camera positions and clear colors are controlled by
`../batch-runner/usd-assets-settings.json`. Resolution defaults to 1280×720.

## Online USD Viewer

`usd-viewer.html` accepts USD/USDA/USDC/USDZ files, folder uploads, and direct
HTTP URLs. Folder uploads preserve relative asset paths and prefer `root.*`,
`default.*`, `scene.*`, or `main.*` as the root. Each load is capped at 200 MiB
of unique USD and asset bytes across layers, references, payloads, and textures.

References compose first. Deferred payloads are exposed through a button, and
the material selector can rebuild the current scene with automatic,
UsdPreviewSurface, or MaterialX/OpenPBR shading. Animation clips expose a
small playback timeline. GitHub-hosted files are rewritten to the GitHub
Contents API, which works from GitHub Pages without a proxy; unauthenticated
GitHub API usage is still subject to rate limits. Other remote hosts must allow
CORS, and 403/429 responses are reported in the viewer.

## Demo List

| # | Demo | Key Feature |
|---|------|-------------|
| 1 | MaterialX OpenChess Rendering | WebGL/WebGPU raster and LightRT/WebGPU path tracing |
| 2 | MaterialX Node Graph | OpenPBR node graph inspector |
| 3 | MaterialX MeshPhysicalMaterial | USD → Three.js material conversion |
| 4 | USDLux Lighting | UsdLight / DomeLight rendering |
| 5 | Skinning | Skeletal animation binding |
| 6 | Node Xform + SkelAnimation | Combined transform + skeletal animation |
| 7 | USD Physics | Physics scene viewer |
| 8 | Asset Resolver Textures | HTTP texture resolution |
| 9 | USD Composition | Sublayer/reference composition |
| 10 | USD Export | USDA/USDC/USDZ export |
| 11 | USD Assets Browser | 263-asset catalog browser |
| 12 | USD Physics + MuJoCo | MuJoCo WASM physics simulation |
| 13 | Material Editor | Live PBR parameter editing |
| 14 | Animation Timeline | Timeline scrubber + speed control |
| 15 | USD Inspector | Prim hierarchy + metadata tree |
| 16 | Composition Layer Viz | Composition arc stack diagram |
| 17 | Streaming Loading Viz | WASM heap + HTTP fetch waterfall |
| 18 | Viewer Toolkit | Shading modes + exposure + toggles |
| 19 | Animation Blending | Per-clip weight crossfade |
| 20 | Procedural USD Builder | JSON → USD scene builder |
| 21 | USDZ Packager | Export with bundle visualization |
| 22 | USD Diff | Side-by-side comparison |
| 23 | Backend Comparison | Legacy vs next rendering comparison |
| 24 | Online USD Viewer | File/folder upload, deferred payloads, materials, and animation |
