# Web regression gate

The fresh WebGPU MaterialX baseline has a focused Windows/Chrome hardware gate:
`node tests/webgpu-mtlx-chrome.mjs --hardware --shaderball --performance`.
Use `--authored-lights` with `--shaderball` for the authored RectLight path-tracing
smoke scene (materials remain diagnostic overrides). The focused harness also
checks real pinned MaterialX library graphs, resource includes/EXR decoding, and
seeded analytic transport scenes. See the status document for coverage limits.
Its pure Node graph/scene tests are included in the aggregate Node profile.
After rebuilding the combined WASM binding, run
`node tests/webgpu-mtlx-usd-graph.mjs` for typed USD shading connections and
connected defaults; `--shaderball` also runs that fixture inside Chrome.
It covers color-space metadata and asset opinions on untyped overrides. The
ShaderBall gate requires source-layer provenance for every composed texture key.
Image tests cover bounded downsampling and mip-budget rejection; renderer options
`textureMaxDimension` and `textureMaxBytes` feed that path.
The graph suite also checks Standard Surface/OpenPBR spelling aliases and rejects
nonzero layered lobes that the approximate terminal cannot represent.
The optional `--authored-materials` ShaderBall flag loads the pinned library and
attempts strict translation of every composed Material prim, reporting explicit
per-material diagnostics for unsupported graphs without replacing render slots.
The current pinned ShaderBall corpus yields ten translated and ten WGSL-compiled
documents. Seven report missing MaterialX surface terminals. The 7,500² ground
EXR is decoded through the bounded downsample path; texture diagnostics remain
empty in the Chrome JSON.
The Chrome fixture additionally translates a native USD emission graph through
the pinned MaterialX definitions and checks every rendered pixel analytically.
See [implementation status and setup](webgpu-mtlx.md) for the pinned fixtures,
generated artifacts, measured coverage, and unfinished reference-rendering work.

Authored-render preflight note: oversized EXR maps are diagnosed from their
header before allocation and omitted from the partial authored-material render;
remaining maps use bounded downsampling.

The synthetic reference-image gate also covers the `hair` MaterialX scene;
Chrome hardware completed it at 32 spp alongside native copper/glass, SSS and
displacement scenes.

The same gate covers the `thin-film` scene, exercising the bounded RGB
interference Fresnel path in Chrome hardware.

It also covers the `subsurface` scene, exercising authored subsurface color and
weight closure mixing; this is not yet a random-walk BSSRDF reference. Its
authored radius also feeds the bounded scattering-lobe roughness.

The `normalmap` scene covers authored normal connections in both path and
realtime shader pipelines.

The `open-pbr-normal` scene covers the OpenPBR `geometry_normal` alias.

Graph assertions cover both `color3` and `vector3` normal-map inputs.

The `opacity` scene covers realtime opacity discard/scaling behavior.

Its shader assertion also guards opacity scaling on direct-light contributions.
The compute-preview realtime branch is covered by the same generated shader
source and opacity fixture.

The normal-map fixture also covers authored-normal use in the realtime raster
lighting path.

The `thin-walled` fixture also compiles the realtime environment-refraction
approximation.


The `bump` scene covers scalar `bump3` height-to-normal fallback in both
pipelines; it does not claim texture-aware finite-difference conformance.

For expensive focused scenes, `--reference-samples=N` bounds the reference-image
loop for diagnostics; the default and pre-merge matrix remain 32 spp.

The Chrome resource check now verifies bounded downsampling and rejection for
oversized regular raster images, matching the existing EXR budget coverage.

The authored ShaderBall gate reuses the already-loaded authored scene for its
diagnostics; hardware Chrome now completes seven compiled authored slots and
51,008 triangles without a duplicate asset decode.

The synthetic hardware matrix also covers the `coat` dielectric-lobe path.

It also covers the `sheen`/`fuzz` diffuse-sheen fallback path.

The sheen fixture also exercises authored `sheen_roughness` propagation.

The `thin-walled` scene covers Standard Surface thin-walled glossy transmission
and verifies the physical transport path without a medium-stack mutation.

The `transmission-depth` scene covers bounded Beer-Lambert attenuation from
authored transmission depth/scatter controls, including the spectral pipeline.

The targeted `generalized-schlick` Chrome run covers authored color-at-normal,
color-at-grazing and exponent transport controls.

The `open-pbr-weight` scene covers OpenPBR `base_weight` propagation into the
compiled base-color contribution.

It also covers OpenPBR `specular_weight` scaling of the terminal microfacet
contribution. Graph assertions cover the diffuse-roughness aliases.

Transport assertions also cover matching the weighted specular sampling/PDF
probability.

The OpenPBR weighted fixture also covers authored `specular_color` tinting.

The `open-pbr-film` scene covers bounded OpenPBR thin-film weight, thickness,
and IOR propagation.

All web/WASM regression procedures live under `web/js`. The canonical gate is
`npm test`; it runs the assertion-based Node/WASM suites, the physics-only
MuJoCo binding smoke test, the USD Physics simulation, every pinned MuJoCo
Menagerie model through MJCF → USD → MJCF, the `urdf.html` renderer, and the
real OffscreenCanvas/Web Worker renderer. The regular UI path uses five
representative models; the CLI closure and Worker path cover every discovered
primary Menagerie model.

## Setup

```bash
# From the repository root: explicit network/setup step.
scripts/verify.sh prepare --profile web
```

The dataset is stored in the ignored root cache at
`.cache/lightusd-verification/menagerie`. Set `MUJOCO_MENAGERIE` or
`MENAGERIE_DIR` to use another checkout. The setup script detaches the checkout
at `tests/fixtures/mujoco-menagerie.lock`; a moving branch is never used by the
regression gate.

Build both WASM modules before browser tests or after changing C++/WASM
bindings:

```bash
bash ../demo/scripts/prepare-local-lightusd.sh
```

This configures `web/build_ninja` for the combined legacy module and
`web/build_next_ninja` for the next-only module, then writes the four generated
artifacts under `web/js/src/lightusd/`.

MuJoCo physics-only tests use the prepared cache when run through
`scripts/verify.sh`. Direct runs should set `MUJOCO_WASM_DIR`, or set
`MUJOCO_PHYSICS_JS` and `MUJOCO_PHYSICS_WASM` explicitly.

For a focused update and smoke test:

```bash
scripts/verify.sh prepare --target mujoco-wasm
scripts/verify.sh test --target mujoco-wasm
```

## Commands

```bash
node tests/run-regression.mjs --profile full --software
npm run test:node              # deterministic Node/WASM suites
npm run test:physics           # fixture simulation + Menagerie CLI closure
npm run test:browser           # conversion prerequisite + both browser paths
npm run test:quick             # alias for the Node/WASM profile
```

For focused debugging, the original scripts remain available, for example:

```bash
node tests/regression-usdzconvert-material-dedup.mjs
./run-mjcf-roundtrip.sh --all --closure
node tests/screenshot-urdf-batch.mjs --all --sw
node tests/screenshot-offscreen-batch.mjs --all --sw \
  --converted-dir .regression/mjcf-roundtrip
```

The full runner writes temporary output under `web/js/.regression`. Successful
runs remove it; use `--keep-output` to retain screenshots, converted USD/MJCF,
per-model summaries, and the aggregate `summary.json`.

`npm run test:browser` is independently runnable: if the Menagerie conversion
summary is absent or incomplete, it first creates the full MJCF/USD/MJCF
conversion output needed by both browser harnesses.

## Browser modes

Hardware runs should use Xvfb so Chrome remains off-screen while ANGLE/Vulkan
uses the GPU. The aggregate runner wraps each browser phase in `xvfb-run` when
it is available:

```bash
npm test
```

The browser runners automatically fall back to headless SwiftShader when the
GPU/X11 prerequisites are unavailable. Use `--software` on
`tests/run-regression.mjs` to force the fallback or `--hardware` to request the
GPU path explicitly.

The regular browser check loads each MJCF into `urdf.html`, converts it to USD,
and verifies both visible views. The OffscreenCanvas check uploads the converted
USD to `offscreengl.html`, verifies the Worker message protocol and mesh count,
captures the canvas itself, and rejects blank renders and page errors.

## Pass criteria

- Every selected Node/WASM test exits zero.
- The physics-only MuJoCo module compiles a spatial tendon and returns a
  center-of-mass Jacobian distinct from the body-origin Jacobian.
- The physics fixture extracts its Physics/MuJoCo annotations and completes the
  MuJoCo simulation.
- Every discovered primary Menagerie MJCF has a successful forward conversion,
  return conversion, and closure count match.
- The regular browser smoke models load without page errors and produce
  nonblank split-view output.
- The OffscreenCanvas Worker loads every converted model without worker errors
  and produces nonblank canvas output.
- Generated assets stay outside Git-tracked paths.

## CTest integration and WASM threading

The same aggregate runner is registered as `wasm-regression-full` when CMake
browser regression tests are enabled:

```bash
ctest --test-dir ../build_ninja -R wasm-regression-full --output-on-failure
```

The default web build is deliberately non-threaded. Both
`LIGHTUSD_ENABLE_THREAD` and `LIGHTUSD_NEXT_ENABLE_THREAD` must be `OFF`, and
generated compile commands must contain no `-pthread`, `PTHREAD`, or
`LIGHTUSD_ENABLE_THREAD` define. `std::shared_ptr` remains valid here because
it provides ownership/lifetime semantics; it does not enable parallel
execution. The next-core WASM build uses `shared_ptr::use_count()` ownership
checks because `shared_ptr::unique()` is unavailable in the active C++20
standard library.
