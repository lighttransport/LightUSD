# WebGPU MaterialX implementation status

This is an initial renderer milestone, **not the completed spectral reference
renderer**. The original complete plan remains in the untracked repository-root
`webgpu-mtlx.md`. Existing WebGPU/Three.js demos are unchanged.

## Running

From `web/js`:

```sh
npm install
node scripts/prepare-webgpu-mtlx.mjs
npm run dev:webgpu-mtlx
```

On Windows PowerShell, use `npm.cmd` if execution policy blocks `npm.ps1`.
The isolated Vite configuration permits synthetic scenes without WASM. For the
ShaderBall geometry diagnostic, also run the standard
`web/demo/scripts/prepare-local-lightusd.sh` in an Emscripten environment.
This produces the combined and next-only modules in ignored output locations.

Asset setup pins USD assets to `3b75c2dad6a494897557dcca0098257bcf42a8c6` and
MaterialX 1.39.5 to `7b64921ef1d42f2d57871e9d2c43dc11f041f26b`. Downloads live
in `.cache/lightusd-verification` at the repository root. Existing checkouts
with a different revision or local edits are rejected without modification.
Retain upstream license/attribution files. `USD_WG_ASSETS_DIR` and
`MATERIALX_DIR` can override the respective local server roots.

## Implemented

- Typed numeric graph-to-WGSL compilation, named outputs, nested graph-defined
  NodeDefs, inheritance, interface binding, shared expressions and diagnostics.
  XML import rejects DTD/entity declarations. Includes, arbitrary source nodes,
  units and color-space transforms are rejected when encountered.
- Scalar/vector value emitters and an explicitly approximate Standard Surface /
  OpenPBR terminal mapping. The UI inventories 807 upstream NodeDefs, but this
  count is **not a supported-node count**; individual overloads remain unverified.
- Worker-built median triangle BVH, stackless compute traversal, per-hit UV/value
  evaluation, GGX/diffuse RGB path preview with progressive accumulation and a
  12-bounce limit. The preview uses a procedural sky and directional light.
- Raw WebGPU raster preview, traced hard shadows, adjustable internal resolution
  and an optional 30-fps resolution controller. Lighting remains approximate.
- Camera controls, pause/resume, accumulation invalidation, PNG and uncompressed
  float32 linear RGB EXR export. Exposure affects display only. EXR metadata
  explicitly records that the image is not a validated reference render.
- ShaderBall composition and authored-camera geometry diagnostic, selecting the
  triangulated variant (51,008 triangles in the pinned asset). It explicitly
  overrides materials and lighting. Asset provenance is retained across layer
  loading to resolve relative references inside variants.
- Procedural synthetic sphere scenes and small MaterialX arithmetic/stripe
  fixtures, deterministic numeric GPU tests, scene validation and EXR roundtrip.

## Not implemented

Full MaterialX coverage; spectral transport and measured-spectrum overrides;
reference BSDF closure composition; random-walk SSS; hair; transmission and
dispersion; volumes; texture/UDIM evaluation; bump and geometric displacement;
faithful authored ShaderBall materials/lights/EXR maps; independent physical
reference-image validation; long-path continuation and variance estimates.
`setMode('reference')` fails rather than substituting the RGB preview. Nonzero
transmission and unsupported surface inputs fail compilation.

The current render color convention is linear RGB without a validated ACEScg
pipeline. Do not use these outputs as ground truth or claim the full plan done.

## Verification

```sh
npm run test:webgpu-mtlx
node tests/webgpu-mtlx-chrome.mjs --hardware --shaderball --performance
```

`CHROME_PATH` overrides the Chrome executable. Without `--hardware`, the focused
harness requests SwiftShader; software runs do not satisfy the hardware gate.
Screenshots and JSON reports go to `web/js/.regression/webgpu-mtlx`.

Verified on Chrome 152.0.7977.76, NVIDIA Ampere hardware:

- Ten Node tests pass, including EXR decoding through Three.js independently.
- 27 numeric WGSL cases pass at `1e-5 + 1e-4 * abs(expected)` tolerance.
- ShaderBall geometry, accumulation/reset, exposure, resize, worker scene loading,
  graph replacement and mode switching pass without GPU or page errors.
- Fixed 1280×720 ShaderBall raster diagnostic: 30-second run, 1,741 frames,
  median 16.9 ms, p95 19.6 ms. These measurements cover the diagnostic shaders,
  not the planned complete MaterialX realtime renderer.
- Combined and next-only WASM builds complete with cached Emscripten 4.0.8.
- Node regression profile in WSL: 19 reported passes, zero failures (the native
  validation-parity check is skipped because lusdcat is not built).

Full Windows `npm test` is not green: three existing USD tests construct an
invalid doubled-drive WASM path, and two dataset gates lack MuJoCo Menagerie.
The same Node profile passes under WSL. See `docs/regression.md` for full setup.
