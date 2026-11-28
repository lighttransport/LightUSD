# JS/WASM refactor (P0~P2) — work in progress

Branch: `js-refactor-p0-p2`. This note records what has landed and what is
still open. The work is **not ready to merge** yet.

## Landed

- **P0 MaterialX cleanup**: removed the legacy synchronous
  `convertOpenPBRToMeshPhysicalMaterial` / `applyNormalMapLegacy` paths; only
  the async, loaded-resource variants remain.
- **`lightusd_tydra` WASM module**: independently linked feature with its own
  heap. Compiles the render binding TUs and exposes the full
  `RenderStream` / `AssetStore` / `LayerDocument` / `FlattenSession` API plus a
  copy-in `convertToRenderScene(bytes)` bridge. Generated glue is kept in
  `web/js/src/lightusd/` for both wasm32 and wasm64.
- **RenderStream relocation (JS level)**: core's post-JS now ends with
  `web/render-delegate.js`, which resolves the render classes through
  `loadFeature('tydra')` and explicitly forwards `_lightusd_wasm_render_*`,
  `_asset_store_*` and `_flatten_*` C hooks to the feature module that owns
  the objects. `lightusd.js` contains only 2 `RenderStream` references (the
  delegate) versus 205 in `lightusd_tydra.js`; geometry/validator/exr modules
  contain none.
- **`core-api.js` split**: 8751 → ~2575 lines plus `render-api.js` (~6088)
  and `image-api.js`. Feature-module link flags are derived from a shared base
  so core-only post-JS files cannot leak into feature modules.
- **One canonical texture manager**: `TextureLoadingManager.js` owns the
  queue/dedup/concurrency/lifecycle; `RenderSceneUtils.js` and
  `LightUSDLoaderUtils.js` extend it with path-specific drivers. Cache keys
  unified in `TextureCacheKeys.js`.
- **`MeshBatching`** exported from the npm package and covered by tests.
- **Three-free `lightusd/core` entry**: `web/npm/core.js`, verified by a smoke
  fixture with no `three` installed.
- **CMake**: `web/cmake/LightUSDWasm.cmake` helper with `EXCLUDE_FROM_ALL`,
  shared binding source lists for core and tydra, and correct `LINK_DEPENDS`
  for every post-JS file.
- **npm packaging**: staging/validation/build scripts and README updated for
  tydra, the core entry and MeshBatching.

## Verification performed

- `npm run test:node` (web/js): **67 passed / 0 failed** on wasm32 and wasm64.
- `core-features-browser.test.mjs`: PASS (both widths, main thread + Worker).
- npm `build:stage` / `validate` / `test`: **533 passed / 0 failed**.
- Native C++ build `[680/680]`, native `ctest` 109/109 (earlier in the series;
  no native code changed after that run).
- `git diff --check` clean; all changed JS files pass `node --check`.

## Remaining items

1. **True C++ ownership split.** Core still *compiles* the render binding TUs
   and `binding-core-api.cc` (154 `RenderStream` references). Removing them
   from the core target fails to link because the object-dispatch TU is
   shared. The public JS API is delegated, but the C++ code is still duplicated
   in both modules. Needs an internal split of `binding-core-api.cc` /
   `binding-core.cc` / `binding-core-scene.cc` into render and non-render TUs.
2. **Test-hook migration hardening.** C-level hooks in several tests were
   moved from `module` to the tydra module and buffer views now use
   `stream.__lightusdModule.HEAPU8`. This works but is scattered; a shared
   test helper would be cleaner, and any new render test must use the feature
   heap explicitly.
3. **Browser GPU gate.** `core-materialx-demos.test.mjs` cannot run on this
   headless host: Vulkan/SwiftShader flags give WebGL1 only (three.js r185
   needs WebGL2), and flag sets that give WebGL2 expose no WebGPU adapter.
   Evidence is recorded in `web/js/docs/regression.md`. Needs a GPU-backed
   run on a developer machine.
4. **Two TextureLoadingManager subclasses remain** (one base, two path
   drivers). Further collapse into a single class with a generic loader hook
   is optional follow-up, not required for P0~P2.
5. **Docs / status hygiene**: update module-level JSDoc and any published API
   inventory JSONs for the new delegated classes; re-run the full web/WASM
   regression profiles on a GPU host before merging.
6. **Commit hygiene**: generated glue `lightusd_tydra{,_64}.js` is tracked in
   source (matching the existing glue files); `.wasm` binaries stay ignored.
   Confirm nothing unrelated is staged before merge.
