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
The Chrome fixture additionally translates a native USD emission graph through
the pinned MaterialX definitions and checks every rendered pixel analytically.
See [implementation status and setup](webgpu-mtlx.md) for the pinned fixtures,
generated artifacts, measured coverage, and unfinished reference-rendering work.

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
