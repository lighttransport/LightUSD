# Web regression gate

Use `node tests/webgpu-mtlx-chrome.mjs --hardware --numeric-only` to isolate
WGSL value-kernel validation from renderer pipeline compilation. This uses a
blank same-origin page and includes blackbody branch/clamp/HDR checks,
color-correction operation ordering and alpha, compositing, and trianglewave.
It does not replace the full renderer/ShaderBall gate below.

`node tests/webgpu-mtlx-chrome.mjs --hardware --opacity-only` renders analytic
emissive cutouts at opacity 0, .25, .5 and 1, plus six transparent planes
crossing a dispatch boundary. It checks known Bernoulli expectations and exact
transparent/opaque endpoints on the physical path tracer. The test server
disables HMR so edits cannot reload a running validation page.

`node tests/webgpu-mtlx-chrome.mjs --hardware --frames-only` checks actual
normal-map orientation on standard, rotated and mirrored UVs. Every physical
pixel is compared to analytic linear emission, and raster capture pixels are
compared after the display transfer. Numeric checks include degenerate UVs.

`node tests/webgpu-mtlx-chrome.mjs --hardware --bump-only` checks six analytic
textured bump/height-to-normal scenes in physical and raster modes, including
rotated/mirrored UVs. The numeric gate checks constant/procedural heights and
height-to-normal scaling, transformed texcoords, encoding and degenerate UVs.

`--library-only` executes ten cases through the pinned stdlib/cmlib/pbrlib
definitions, including graph-expanded bump and glossiness, default geometry
bindings, and both artistic IOR outputs. It complements the direct emitters
tested by `--numeric-only`.

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
The optional `--authored-materials` ShaderBall flag loads every library file from
the pinned 1.39.5 catalog and
attempts strict translation of every composed Material prim, reporting explicit
per-material diagnostics for unsupported graphs without replacing render slots.
Mesh subset ranges are expanded to per-triangle material IDs and overlapping or
misaligned ranges are rejected before scene upload.
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

The subsurface closure now uses a distinct bounded diffusion-profile evaluator
and cosine sampler; true random-walk BSSRDF transport remains future work.

The `normalmap` scene covers authored normal connections in both path and
realtime shader pipelines.

The `open-pbr-normal` scene covers the OpenPBR `geometry_normal` alias.

Graph assertions cover both `color3` and `vector3` normal-map inputs.

The `normalmap-image` fixture covers a packed raw-color image feeding the
normal-map node. The graph/packing check passes in Node; its focused spectral
Chrome run is currently isolated from the default matrix because it stalls
before reporting a sample.

The `ops` fixture covers rotate, remap, conditional, extraction, and typed
combine nodes in a single compiled surface graph.

The `ops-advanced` fixture covers vector conversion/swizzle, separate, power,
dot/cross, and equality-selection nodes.

The `layered` fixture covers an Oren–Nayar BSDF layered over an anisotropic VDF
interior in the compiled scene path.

The `edf` fixture covers a uniform EDF connected to a surface terminal and
emission-only realtime rendering.

The `native-film` fixture covers thin-film controls on a direct dielectric BSDF.
Chrome verification is pending for its native-film pipeline compile.
Compiler coverage also checks thin-film propagation on a direct conductor BSDF.

The displacement fixture uses two levels of linear refinement before baking;
this is still not Catmull-Clark subdivision or adaptive tessellation.
The bake also smooths normals across matching material/UV vertices while
retaining seam boundaries.

The `opacity` scene covers realtime opacity discard/scaling behavior.

Its shader assertion also guards opacity scaling on direct-light contributions.
The compute-preview realtime branch is covered by the same generated shader
source and opacity fixture.
The physical preview source also asserts stochastic alpha continuation for
partial opacity.

The normal-map fixture also covers authored-normal use in the realtime raster
lighting path.

The `thin-walled` fixture also compiles the realtime environment-refraction
approximation.


The `bump` scene covers scalar `bump3` height-to-normal fallback in both
pipelines; it does not claim texture-aware finite-difference conformance.

For expensive focused scenes, `--reference-samples=N` bounds the reference-image
loop for diagnostics; the default and pre-merge matrix remain 32 spp.
Focused diagnostics also accept `--reference-mode=path-physical` to separate
image-resource failures from spectral transport; the default remains
`path-spectral`. `--reference-mode=realtime` runs one raster frame and enables
the image-backed normal-map fixture without requiring linear path capture.

The Chrome resource check now verifies bounded downsampling and rejection for
oversized regular raster images, matching the existing EXR budget coverage.

The authored ShaderBall gate reuses the already-loaded authored scene for its
diagnostics; hardware Chrome now completes seven compiled authored slots and
51,008 triangles without a duplicate asset decode.
Realtime loads compile the physical pipeline lazily; the gate therefore checks
the authored raster path without waiting on unused path-tracing compilation.
Material replacement follows the same demand-driven pipeline path, preventing
concurrent physical shader builds during interactive edits.
Scene-load mode changes are also covered: realtime image scenes no longer wait
on a physical pipeline inherited from the previous scene.
The graph suite validates supported MaterialX unit annotations and rejects
unknown unit names; the `ops` realtime fixture exercises the updated compiler.
Path shader generation also checks that image sampling receives a nonzero UV
footprint estimate for mip selection; exact ray differentials remain future
work.
The image-backed path reference gate is opt-in with
`--include-image-path`; its current physical transport case does not converge
within the bounded browser harness and remains under investigation.
Diagnostic runs may override its workload with `--reference-width` and
`--reference-height`; the default reference resolution remains 192x128.
Normal-map transport applies a geometric-hemisphere correction before BSDF
sampling; realtime image coverage remains the browser regression for this path.

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
## Testing an isolated next WASM build

The Node profile includes `tests/next-c-dispatch.test.mjs`: object-ID generation
retirement, stale/wrong-class calls, explicit disposal, argument validation,
numeric/Unicode JSON fidelity, and malformed validation options. Run it on both
wasm32 and memory64 after changing the C dispatch or post-JS wrapper.

To exercise an isolated measurement build without replacing generated app
files, run from `web/js` (use an absolute module path):

```sh
LIGHTUSD_NEXT_MODULE="$PWD/../build_ninja/refactor-next/artifacts/lightusd_next.js" \
  NODE_OPTIONS="--loader $PWD/tests/next-module-override.mjs" \
  node tests/run-regression.mjs --profile node
```

For memory64, set `LIGHTUSD_WASM64=1` and select `lightusd_next_64.js` from its
build directory. The override always loads the matching `.wasm`, even if a
test supplies `locateFile` for the application's normal artifacts. This is a
Node-only testing helper, not a production loader or a substitute for the
browser/physics gates below.

## Complete gate

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

Lucia Code browser smoke uses an ephemeral localhost port by default to avoid
collisions between concurrent runs. Set `LUCIA_SMOKE_PORT` to a specific port
when a CI environment requires fixed routing; the harness parses Vite's
advertised URL and reports child-process startup diagnostics on failure.

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
