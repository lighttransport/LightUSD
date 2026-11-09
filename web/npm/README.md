# LightUSD WASM/JS

JS/WASM distribution of LightUSD.

## Demos

[GitHub Pages demos](https://lighttransport.github.io/lightusd/demos/)

## Install

```bash
npm install lightusd
```

## Quick usage

The package now exposes a root entrypoint for the common loader APIs:

```js
import { LightUSDLoader, LightUSDLoaderUtils } from 'lightusd';

async function loadScenes() {
  const loader = new LightUSDLoader();

  // it is recommended to call init() before loadAsync()
  // (init() does wasm loading/compiling)
  await loader.init();

  const suzanneUrl = './assets/suzanne-pbr.usda';
  const usdScene = await loader.loadAsync(suzanneUrl);
  const usdRootNode = usdScene.getDefaultRootNode();
  const defaultMtl = LightUSDLoaderUtils.createDefaultMaterial();

  const options = {
    overrideMaterial: false // override USD material with defaultMtl(default false)
  };

  const threeNode = LightUSDLoaderUtils.buildThreeNode(
    usdRootNode,
    defaultMtl,
    usdScene,
    options
  );

  //
  // Add threeNode to the Three.js scene.
  //
}
```

`LightUSDLoader` selects the next-only module by default. Select the legacy
product explicitly when an application depends on the legacy conversion path:

```js
await loader.init({ backend: 'legacy' });
const scene = await loader.loadAsync(url, { backend: 'legacy' });
```

Deep imports such as `lightusd/LightUSDLoader.js` remain supported for existing code.

## Using zstd-compressed WASM

LightUSD's WASM module is roughly 2 MB uncompressed. The npm package also ships zstd-compressed WASM binaries, which are substantially smaller.

If you want to use zstd-compressed WASM, set `useZstdCompressedWasm` to `true` in the `init()` options.

```js
const loader = new LightUSDLoader();
await loader.init({ useZstdCompressedWasm: true });
```

## Use the wasm64 build

The npm package also contains a MEMORY64 build. You can request it with:

```js
await loader.init({ useMemory64: true });
```

If the wasm64 module is unavailable in the runtime or bundle, the loader falls back to the standard wasm32 build.

## More examples

See the LightUSD web demos:

https://github.com/lighttransport/LightUSD/tree/main/web/demo

## Packaging workflow

The npm package is assembled from:

- `web/npm/scripts/stage-package.mjs`
- `web/npm/scripts/validate-package.mjs`
- `.github/workflows/wasmPublish.yml`

### Local packaging

The checked-in `web/npm/package.json` is a package template. It is marked `private: true` so it is not published directly.

The default local entrypoint is now:

```bash
cd web/npm
npm run build -- --release-version=1.2.3
```

`npm run build` is a one-shot wrapper around:

- local Node.js bootstrap
- local emsdk bootstrap
- clean wasm32 + wasm64 build
- npm package staging into `web/npm/dist`
- package validation

Packaging requires Node.js v24+ because `stage-package.mjs` uses Node's built-in Zstd compression APIs to generate `*.wasm.zst`.

If your system Node.js is older, download and activate the repo-local Node.js build first:

```bash
cd web/npm
./download-nodejs.sh
source ./setup-nodejs.sh
```

`download-nodejs.sh` downloads a Node.js v24.x binary into `web/npm/.nodejs/`, and `setup-nodejs.sh` prepends its `bin/` directory to `PATH` for the current shell.

WASM builds also require Emscripten SDK. The local helper below downloads and activates a repo-local `emsdk` checkout in `web/npm/.emsdk/` and configures it to use the same local Node.js v24 install:

```bash
cd web/npm
./download-emsdk.sh
source ./setup-emsdk.sh
```

`setup-emsdk.sh` sources the local `emsdk_env.sh`, exports `EM_CONFIG` for the local SDK config, and keeps local Node.js v24 at the front of `PATH` so Emscripten uses it consistently.

The publishable package is generated into `web/npm/dist` by `stage-package.mjs`. That staging step:

- copies JS modules from `web/js/src/lightusd/`
- copies wasm32 and wasm64 assets
- generates `.wasm.zst` files with Node.js built-in Zstd compression
- copies `LICENSE` and `README.md`
- writes a publish-ready `package.json`

If you want the build steps separately instead of the one-shot wrapper, `build-wasm.sh` mirrors the local configure/build flow used by `.github/workflows/wasmPublish.yml` and builds both wasm32 and wasm64 under `web/`.

```bash
cd web/npm
./build-wasm.sh
```

`build-wasm.sh` sources both `setup-nodejs.sh` and `setup-emsdk.sh` automatically.

The script configures and builds:

- `web/cmake-build` for wasm32 legacy (`LIGHTUSD_WASM_PRODUCT=legacy`)
- `web/cmake-build64` for wasm64 legacy
- `web/cmake-build-next` for wasm32 next (`LIGHTUSD_WASM_PRODUCT=next`)
- `web/cmake-build-next64` for wasm64 next

`web/CMakeLists.txt` then writes the generated outputs into `web/js/src/lightusd/`.

You can override a few knobs when needed:

```bash
CMAKE_BUILD_TYPE=Debug ./build-wasm.sh
PARALLEL_JOBS=8 ./build-wasm.sh
```

After `build-wasm.sh`, the following files should exist in `web/js/src/lightusd/`:

- `lightusd.js`
- `lightusd.wasm`
- `lightusd_64.js`
- `lightusd_64.wasm`
- `lightusd_next.js`
- `lightusd_next.wasm`
- `lightusd_next_64.js`
- `lightusd_next_64.wasm`

The `lightusd_next*` modules are the lean next-core + Tydra-next builds used
by `LightUSDLoader` by default. The deprecated `useNextOnlyWasm` option and
`nextWasm` query alias remain accepted during migration.

The next distribution also contains `lightusd_geometry`, `lightusd_raytrace`,
`lightusd_validator`, and `lightusd_exr`, each with `.js` / `.wasm` files and
matching `_64` variants. Keep these files beside the main module, or supply
`locateFeature(filename)` to resolve both feature JS and WASM files.

```js
const module = await createLightUSD();
const geometry = await module.loadFeature('geometry');
const simplifier = new geometry.MeshoptSimplifier();
const subdiv = new geometry.SubdivStreamer();
const raytrace = await module.loadFeature('raytrace');
const tracer = new raytrace.LightRTPathTracer();
const validator = await module.loadFeature('validator');
const exr = await module.loadFeature('exr');
```

Each feature has its own heap. Pass typed arrays to its methods; the wrappers
transfer counted data and return owned copies where documented. Subdivision
callback arrays borrow the geometry heap and must be copied to retain them.
`loadFeature` shares pending and successful promises, allows retry after failure,
and rejects mismatched ABI versions, pointer widths, or layouts. `featureInfo`
reports that metadata. The old `loadEXR` / `loadValidator` loaders and feature
constructors on the main next module have been removed. Load EXR before direct
synchronous EXR rendering/encoding calls, and validator before
`LayerDocument.validateLoadedLayer()`.

The equivalent manual sequence is:

```bash
cd web/npm
npm ci
./build-wasm.sh
npm run build:stage -- --release-version=1.2.3
npm run validate
```

Useful commands:

- `npm run build`
  Full local bootstrap + clean wasm build + package stage + validate
- `npm run build:wasm`
  Clean wasm32 + wasm64 build only
- `npm run build:stage -- --release-version=...`
  Stage the npm package only
- `npm run validate`
  Validate the staged package
- `npm run pack:dry-run`
  Tarball content check only

`npm run validate` checks the staged file set, runs `npm pack --dry-run`, and performs a smoke test for:

- root imports such as `import { LightUSDLoader } from 'lightusd'`
- legacy deep imports such as `import { LightUSDLoader } from 'lightusd/LightUSDLoader.js'`
- direct wasm asset resolution through the package exports map

### GitHub publish workflow

The manual publish workflow is:

- workflow: `.github/workflows/wasmPublish.yml`
- trigger: `workflow_dispatch`
- inputs:
  - `release_version`
  - `npm_tag`

The workflow does the following:

1. sets up Node.js v24+
2. builds wasm32 and wasm64 with Emscripten
3. runs `npm ci` in `web/npm`
4. stages the package into `web/npm/dist` with `npm run build:stage -- --release-version=...`
5. validates the staged package
6. uploads `web/npm/dist` as a workflow artifact
7. publishes from `web/npm/dist` with `npm publish --provenance`

### Notes

- The root package entrypoint is `lightusd`, but deep import paths remain supported for compatibility.
- If the wasm64 module is missing at runtime, `LightUSDLoader` falls back to wasm32.
- `setup-emsdk.sh` must be sourced, not executed, because it updates `PATH` and `EM_CONFIG` in the current shell.
- `setup-nodejs.sh` must be sourced, not executed, because it updates `PATH` in the current shell.
- `web/npm/dist` is generated output and should not be edited by hand.
