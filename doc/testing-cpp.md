# C++ Testing Guide

How to build, run, and extend the LightUSD C++ test suite.

## Overview

The C++ test infrastructure is split into four layers:

1. `ctest`-registered tests for parser coverage, roundtrip coverage, feature tests, and the main Acutest unit suite.
2. A large Acutest executable, `unit-test-lightusd`, which aggregates the unit coverage from `tests/unit/unit-main.cc`.
3. Standalone/manual runners under `tests/` for broader corpus checks, especially parser and Tydra conversion sweeps.
4. Fuzzer targets under `tests/fuzzer/` built separately with Meson/libFuzzer.

Core functionality is tested by the parser, reader, writer, composition, and crate-writer coverage. Tydra is covered in both the Acutest suite and the manual `tydra_to_renderscene` corpus runner.

## C-style core refactor checks

The active migration and measurement procedure is in
[refactor-c-core.md](refactor-c-core.md). Next's type/value tests cover every
built-in type-name/layout descriptor, bounded name lookup, zero-copy vector
adoption, all eight array backing kinds, and copy-on-write ownership. Dictionary
tests also exercise the shared integer string index, growth, duplicate and
embedded-NUL keys, overflow rejection without losing the old index, and
copy/move assignment. Name-table tests retain references across rehashing and
frozen-snapshot updates; threaded builds exercise concurrent interning and
array detachment.

Run standalone next tests with threading both off and on after primitive changes.
For memory checks, use an optimized sanitizer build (for example Debug with
`-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`): the large-instance
suite has a wall-clock bound that an unoptimized sanitizer build can exceed.
Keep assertions enabled. Run Python tests against an extension rebuilt from
this checkout, and the web gate against freshly built next and combined modules;
a legacy-only `lightusd.js` lacks APIs required by the combined-module tests.

## Reproducible verification entrypoint

The repository-wide harness is `scripts/verify.sh`. Preparation downloads only
the pinned external inputs from `tests/verification/manifest.json` into the
ignored `.cache/lightusd-verification/` directory. Test actions do not fetch
or update dependencies; use `--offline` to enforce that contract.

```bash
scripts/verify.sh doctor --profile native
scripts/verify.sh prepare --profile full
scripts/verify.sh test --profile full --software
scripts/verify.sh test --profile full --offline --software

# Focused dependency/test slices:
scripts/verify.sh prepare --target mujoco-wasm
scripts/verify.sh test --target mujoco-wasm
scripts/verify.sh prepare --target menagerie --offline
scripts/verify.sh test --target web-physics --software
```

Profiles are `native`, `next`, `web`, `oracle`, `assets`, `gpu`, and `full`.
The `gpu` profile is opt-in. Private large-scene inputs remain opt-in and are
not part of the reproducible public `full` profile. Each action writes a JSON
report under the ignored verification cache.

Standalone corpus runners accept explicit executable paths and return non-zero
when any input fails:

```bash
python3 tests/parse_usd/runner.py tests/usda --app build/lusdcat
python3 tests/tydra_to_renderscene/runner.py models --app build/tydra_to_renderscene
```

Preparation scripts also support partial operation:

```bash
scripts/prepare-mujoco-wasm.sh --checkout-only
scripts/prepare-mujoco-wasm.sh --build-only --offline
scripts/prepare-usd-assets.sh --checkout-only
```

`--checkout-only` updates and pins a dependency without building it. MuJoCo's
`--build-only` requires an existing checkout and rebuilds the pinned revision
without fetching.

## Build

Configure the native build with tests enabled:

```bash
mkdir build
cd build
cmake .. -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_EXAMPLES=ON
make -j16
```

Relevant options in the current build configuration:

- `LIGHTUSD_BUILD_TESTS=ON`
- `LIGHTUSD_BUILD_EXAMPLES=ON`
- `LIGHTUSD_WITH_JSON=ON`
- `LIGHTUSD_WITH_MODULE_USDA_READER=ON`
- `LIGHTUSD_WITH_MODULE_USDC_READER=ON`
- `LIGHTUSD_WITH_MODULE_USDC_WRITER=ON`
- `LIGHTUSD_WITH_TYDRA=ON`
- `LIGHTUSD_WITH_PXR_COMPAT_API=ON`
- `LIGHTUSD_TEST_FIXTURE_DIR` — source-tree root containing `tests/`; set this
  when the build directory is outside the checkout.

CTest passes the fixture root to the unit tests, so an out-of-tree build can
run the same tests without changing its working directory:

```bash
cmake -S . -B /tmp/lightusd-build -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_TEST_FIXTURE_DIR="$PWD"
cmake --build /tmp/lightusd-build
ctest --test-dir /tmp/lightusd-build -R unit-test-lightusd --output-on-failure
```

The fixture root can also be supplied directly to a test executable with the
`LIGHTUSD_TEST_FIXTURE_DIR` environment variable.

## Full Regression Tests

Run the full regression suite before changes that affect parsing, composition,
USDA/USDC writing, USDZ packaging, schema reconstruction, or tool output.

> **Scope:** The experimental `next` module (`src/next/`, `lightusd_next`) and
> its tests under `tests/next/` are **not** part of this regression suite. They
> are a standalone CMake project, are not built by the main `build/` (so they do
> not appear in `ctest`), and are not run by the Pixar comparison runner. Do not
> treat `next` results as part of the regression gate. See
> [Experimental `next` library tests](#experimental-next-library-tests) for how
> to build and run them on demand.

The full regression pass has two parts:

1. All CMake/CTest-registered tests, including parser corpus tests, roundtrip
   corpus tests, registered feature tests, benchmarks in quick mode, MCP tests,
   and the main Acutest unit suite.
2. The Node.js `lusdcat` vs OpenUSD v26.05 `usdcat` comparison runner, which
   checks LightUSD output against `usdcat` over the USDA and USDC fixture
   corpora.

Recommended command sequence from the repository root:

```bash
# Build all configured tests and examples, including lusdcat.
cmake --build build

# 1. Run all CTest-registered tests.
cd build
ctest --output-on-failure
cd ..

# 2. Run the stable next module tests. Keep this Debug: tests use assert().
cmake -S src/next -B build-next -DLIGHTUSD_NEXT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-next -j16
ctest --test-dir build-next --output-on-failure

# 3. Run the Node.js roundtrip/comparison suite against OpenUSD v26.05.
#    Build it once with: scripts/build-openusd-usdcat.sh
#    For headless environments lacking PySide, `--full` will retry with a reduced
#    full-core profile unless OPENUSD_RETRY_NO_PYSIDE=0 is set.
LUSDCAT_PATH=./build/lusdcat \
USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-usdcat-compare.sh
```

`tests/run-usdcat-compare.sh` requires Node.js and a working OpenUSD v26.05
`usdcat`. Prefer the repo-local helper install at `ref/dist/bin/usdcat`,
created by `scripts/build-openusd-usdcat.sh`. Use
`scripts/build-openusd-usdcat.sh --prepare-only` when you only need to clone or
refresh the local `ref/openusd` checkout on OpenUSD v26.05 without building
`usdcat`; use `OPENUSD_FETCH=0` to skip network fetches when the ref is already
present locally, and `scripts/build-openusd-usdcat.sh --full` when you need a full
OpenUSD release build rather than the default minimal usdcat/tool install. The
comparison script also recognizes a sibling `../OpenUSD/dist/bin/usdcat` when
the repo-local install is not present, but full regression results should use
v26.05 unless a test intentionally targets another OpenUSD release. Set
`USDCAT_PATH` explicitly when needed.

The comparison runner writes detailed logs to `tests/comparison-results/` and
prints a failure/warning summary. It continues across individual files so one
failure does not hide later failures; inspect the summary and log even when the
shell command itself completes.

### Headless NVIDIA viewer regression

The viewer tests are part of the native CTest tree only when
`LIGHTUSD_BUILD_GUI_VIEWER=ON`. On an NVIDIA Linux host, run them under a
24-bit Xvfb display and opt into CMake's NVIDIA offload environment:

```bash
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_BUILD_EXAMPLES=ON \
  -DLIGHTUSD_BUILD_GUI_VIEWER=ON \
  -DLIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD=ON
cmake --build build_ninja -j16

# Viewer-only hardware/display coverage:
xvfb-run -a -s "-screen 0 1280x800x24" \
  ctest --test-dir build_ninja -R '^lusdview' --output-on-failure

# Full native CTest matrix, including the viewer tests:
xvfb-run -a -s "-screen 0 1280x800x24" \
  ctest --test-dir build_ninja --output-on-failure
```

`LIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD=ON` is evaluated during CMake configure. If
the NVIDIA kernel device, GLVND vendor file, and an NVIDIA Vulkan physical
device are visible, CMake injects the following into `lusdview-*` tests:

```text
__NV_PRIME_RENDER_OFFLOAD=1
__GLX_VENDOR_LIBRARY_NAME=nvidia
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
LUSDVIEW_NVIDIA_OFFLOAD=1
LUSDVIEW_XVFB=1
LUSDVIEW_VK_DEVICE=nvidia
```

The Vulkan selector is added only when `vulkaninfo --summary` confirms an
NVIDIA physical device. If the host exposes the kernel/GLVND files but not the
Vulkan ICD or `/dev/nvidia0`, CMake leaves Vulkan selection automatic so the
tests can use another device or return their normal skip code. Check the
configure message before interpreting a Vulkan result.

The `lusdview-cpu-rt-*` tests are intentionally exempt from the NVIDIA GL
shader-cache warmup and PRIME environment. They use Vulkan only as a windowless
presentation shell and must remain runnable on the automatically selected
device if an Xvfb display or NVIDIA driver disappears after configuration.

The `tool-lusdrender-materialx-openpbr-parity` test also compares a fixed
six-panel OpenPBR lobe scene against committed 192x140, 16-sample PNG references.
Vulkan has its own reference and CUDA/HIP share one because their common kernel
is byte-identical for this deterministic scene. The 1% normalized-RMSE allowance
covers minor driver/compiler variation without accepting a flattened or missing
lobe.

The three GLVND variables route OpenGL only. `LUSDVIEW_VK_DEVICE=nvidia` is a
test-harness variable that forwards `--vk-device nvidia`; direct viewer runs
should use the command-line option. True headless Vulkan/CUDA/HIP tests do not
need Xvfb. For the complete variable reference, direct commands, AMD behavior,
and recovery from a broken sandbox X socket, see
[`doc/lusdview.md`](lusdview.md#vulkan-on-nvidia-primeoffload-under-xvfb).

If `xvfb-run` cannot create `/tmp/.X11-unix` sockets in a container or managed
sandbox, start Xvfb externally with Unix sockets disabled and use its TCP
display instead:

```bash
Xvfb :88 -screen 0 1280x720x24 -ac -nolisten unix -nolisten local -listen tcp
DISPLAY=localhost:88 ctest --test-dir build_ninja -R '^lusdview' --output-on-failure
```

For USD-assets smoke runs, use `LUSDVIEW_XVFB=external` with the same `DISPLAY`
and set `LUSDVIEW_NVIDIA_OFFLOAD=1 LUSDVIEW_VK_DEVICE=nvidia` only after the
Vulkan device probe succeeds.

Useful variants:

```bash
# Quieter comparison logs.
SHOW_DETAILED_DIFF=false \
LUSDCAT_PATH=./build/lusdcat \
USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-usdcat-compare.sh

# Longer per-file timeout for slow debug/ASan builds.
TIMEOUT_MS=120000 \
LUSDCAT_PATH=./build/lusdcat \
USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-usdcat-compare.sh

# Single-file comparison through the Node.js runner.
node tests/compare-usda.js \
  --lusdcat ./build/lusdcat \
  --usdcat ref/dist/bin/usdcat \
  --detailed-diff \
  tests/usda/somefile.usda
```

## ctest Suite

CMake registers these tests when the corresponding targets are built (most in the top-level `CMakeLists.txt`; `unit-test-lightusd` in `tests/unit/CMakeLists.txt` and `mcp-test` in `tests/mcp/CMakeLists.txt`):

| ctest name | Kind | Backing executable/script |
| --- | --- | --- |
| `usda-parser-unit-test` | Parser corpus runner | `python3 tests/usda/unit-runner.py --app build/test_lightusd` |
| `usda-roundtrip-test` | USDA roundtrip corpus runner | `python3 tests/usda/roundtrip-runner.py --app build/usda_roundtrip` |
| `usdc-roundtrip-test` | USDA -> USDC -> reparse corpus runner | `python3 tests/usda/usdc-roundtrip-runner.py --app build/usdc_roundtrip` |
| `usdc-parser-unit-test` | Parser corpus runner | `python3 tests/usdc/unit-runner.py --app build/test_lightusd` |
| `usdc-writer-diff-test` | USDC writer roundtrip diff (informational) | `python3 tests/usdc-writer/usdc-writer-runner.py ... --report-only` |
| `feat-value-clip` | Feature test | `build/feat-value-clip` |
| `feat-mtlx-parse`, `feat-mtlx-import`, `feat-mtlx-export` | Feature tests | `build/feat-mtlx-*` |
| `feat-mtlx-grouped-params` | Feature test (needs `LIGHTUSD_WITH_JSON`) | `build/feat-mtlx-grouped-params` |
| `feat-variant-converter`, `feat-variant-applier` | Feature tests | `build/feat-variant-*` |
| `feat-subdiv` | Feature test (tinysubdiv) | `build/feat-subdiv` |
| `feat-subdiv-verify` | Feature test (only when `LIGHTUSD_TSD_VERIFY_WITH_OSD`, label `osd-verify`) | `build/feat-subdiv-verify` |
| `bench-parse-opt` | Benchmark target (label `benchmark`) | `build/bench-parse-opt --quick` |
| `bench-render-convert` | Stage→renderable-mesh conversion benchmark (label `benchmark`) | `build/bench-render-convert --iters 1 --prims 64` |
| `bench-render-convert-next` | Same, + tydra-next pipeline (only when `LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON`) | `build/bench-render-convert-next` |
| `unit-test-lightusd` | Acutest unit suite | `build/unit-test-lightusd` |
| `mcp-test` | MCP server unit test (only when `LIGHTUSD_WITH_MCP_SERVER`) | `build/mcp-test` |

`usdc-parser-unit-test` is set to run after `unit-test-lightusd` (it globs `*-runtime.usdc` fixtures the unit suite generates).

The `ctest` labels in use today are `benchmark`, `osd-verify`, and `lusdview`;
the `textools` label exists only when the vendored textools upstream self-tests
are explicitly enabled (`-DLIGHTUSD_BUILD_TEXTOOLS_TESTS=ON`, default OFF):

```bash
ctest --print-labels
# benchmark, osd-verify, lusdview
```

The lusdview viewer example registers GPU-dependent tests under the `lusdview` ctest label:

| Name | What it tests | Skip condition |
|------|---------------|----------------|
| `lusdview_lighting_test` | Non-mesh RT proxy topology + opacity | None (compiled unit) |
| `lusdview_lightrt_bridge_test` | LightRT/OpenPBR material packing ABI | None (compiled unit) |
| `lusdview_openpbr_material_test` | OpenPBR material extraction | None (compiled unit) |
| `lusdview_texture_pipeline_test` | Image decode/mip/descriptor pipeline | None (compiled unit) |
| `lusdview_lightrt_mtlx_eval_test` | MaterialX ND_image evaluation | None (compiled unit) |
| `lusdview_lightrt_mtlx_graph_connection_test` | MaterialX graph edge resolution, forward references, graph outputs, selectors, and surface binding | None (compiled unit) |
| `lusdview_lightrt_mtlx_graph_evaluation_test` | Numerical evaluation of connected arithmetic, vector, conditional, blend, and UV-spatial MaterialX nodes | None (compiled unit) |
| `tool-lusdrender-materialx-cpu-graph` | Default CPU lusdrender evaluation of deep connected MaterialX graphs without fallback | None (headless CLI) |
| `lusdview_geometry_primvar_test` | Geometry primvar reconstruction | None (compiled unit) |
| `lusdview_camera_nav_test` | Camera navigation | None (compiled unit) |
| `lusdview-next-nonmesh-extraction` | Default-loader Points/Curves records | Vulkan backend |
| `lusdview-gl-nonmesh-render` | GL carrier-render for Points/Curves | No GL context |
| `lusdview-vk-nonmesh-render` | VK carrier-render for Points/Curves | No Vulkan backend |
| `lusdview-dome-orientation` | DomeLight IBL orientation | No GPU |
| `lusdview-light-record-equivalence` | Next/legacy light record parity | `xvfb-run` |
| `lusdview-camera-record-equivalence` | Next/legacy camera record parity | `xvfb-run` |
| `lusdview-shadow-alpha-inst` | Alpha-cutout + PointInstancer shadow | No GPU |
| `lusdview-aousd-conformance` | AOUSD spec render conformance | No Vulkan backend |
| `lusdview-gl-vk-parity` | GL/VK raster image agreement | No GPU |
| `lusdview-raster-shadow-map` | Raster shadow map regression | No GPU |

```bash
# Run every test whose name belongs to the lusdview matrix. The name filter
# includes tests that do not carry the lusdview label.
ctest -R '^lusdview' --output-on-failure

# Run only the tests explicitly tagged with the lusdview label.
ctest -L lusdview --output-on-failure

# Run individual test
ctest -R lusdview-camera-record-equivalence --output-on-failure
```

Useful commands:

```bash
cd build

# List configured tests
ctest -N

# Run everything registered with ctest
ctest --output-on-failure

# Run just the Acutest suite
ctest -R unit-test-lightusd --output-on-failure

# Run parser corpus tests only
ctest -R 'parser-unit-test' --output-on-failure

# Run roundtrip corpus tests only
ctest -R roundtrip --output-on-failure

# Exclude benchmark-labeled tests
ctest --output-on-failure -LE benchmark
```

## Regression Test Procedure

For Vulkan transparency changes, run the persistent-viewer promotion tests as
well as the fixed-frame transparency matrix:

```sh
xvfb-run -a ctest --test-dir build_ninja \
  -R 'lusdview-(vulkan-oit-promotion|vulkan-cold-start-auto|transparency|vk-oit-validation)' \
  --output-on-failure
```

The promotion test checks a responsive baseline, mode-change cancellation,
reuse without another compilation, and screenshot parity with explicit weighted
startup. The non-threaded case also loads native-carrier, instanced, and MaterialX
scenes into the same renderer. Compilation duration and resident memory are
advisory measurements; a fresh application cache does not imply a cold driver
cache. Khronos validation requires an installed or locally built layer.

The live-reload harness keeps a large finite frame budget so idle headless
frames cannot end the viewer during a cold driver compilation. Vulkan reload
launches retain a 10-second responsiveness deadline; synchronous CUDA/HIP
reloads have a separate 180-second compilation allowance.
The hardware Vulkan test also rejects an interactive reload that omits
`LUSDVIEW_RT_INTERACTIVE_ONLY`, even when a driver cache could hide the extra
compilation cost. Its completion deadline remains 180 seconds.

Use this procedure for merge-level validation, release checks, and refactor hardening:

### 1) CTest matrix (native)

```bash
cd build

# Native regression gate for parser/roundtrip/unit/feature tests
ctest --output-on-failure

# Optional focused subsets
ctest -R unit --output-on-failure
ctest -R roundtrip --output-on-failure
ctest -R feat --output-on-failure
```

### 2) Feature binaries (target-level)

Run feature targets directly when an individual behavior needs isolation:

```bash
./build/feat-mtlx-parse
./build/feat-mtlx-import
./build/feat-mtlx-export
./build/feat-variant-converter
./build/feat-variant-applier
./build/feat-mtlx-grouped-params
```

### 3) WASM regression checks

For web changes, build and validate the WebAssembly tree:

```bash
cd web
emcmake cmake -S . -B build
cmake --build web/build -j16
ctest --test-dir web/build --output-on-failure
```

If your local web build does not register ctest targets, use the web package checks documented by the web frontend pipeline as the functional equivalent.

### 4) Roundtrip comparison against Pixar USD

These checks catch cross-version serialization or compatibility drift:

```bash
# Batch script for broad coverage
USDCAT_PATH=ref/dist/bin/usdcat LUSDCAT_PATH=./build/lusdcat \
  bash tests/run-usdcat-compare.sh

# Per-file diff for regression investigation
node tests/compare-usda.js --detailed-diff \
  --lusdcat ./build/lusdcat --usdcat ref/dist/bin/usdcat \
  tests/usda/somefile.usda
```

`bench-parse-opt` is intentionally split into two profiles:

- `ctest` runs `bench-parse-opt --quick` to keep suite wall time short.
- Manual benchmark runs can still use the default full profile via `./build/bench-parse-opt`.

### Render-conversion benchmark (`bench-render-convert`)

`tests/feat/render-convert/perf-render-convert.cc` measures Stage →
raster/RT-renderable mesh conversion for both pipelines:

- legacy `tydra::RenderSceneConverter::ConvertToRenderScene` (default target),
- tydra-next `tydra::next::RenderSceneConverter::Convert` (when built with
  `PERFRC_ENABLE_NEXT`: the `bench-render-convert-next` target, or the
  `bench_tydra_render` target in the standalone `src/next` tree).

It generates a deterministic synthetic multi-mesh scene (`--prims N`, mixed
sizes; ~1.5M tris at the default 2048) or loads `--scene <file>`, runs
`--iters` conversions and prints the median wall time plus a stable FNV-1a
scene checksum. The checksum is the byte-identity gate: serial vs parallel
runs of the same build must print the same hash.

```bash
# A/B: parallel vs serial conversion (hashes must match)
./build/bench-render-convert --prims 2048 --legacy-threads 0 --json
./build/bench-render-convert --prims 2048 --legacy-threads 1 --json

# tydra-next with explicit worker count
./build/bench-render-convert-next --prims 2048 --threads 8
```

Report-only: never fails on timings. Threading follows the repo-wide
`LIGHTUSD_ENABLE_THREAD` / `LIGHTUSD_NEXT_ENABLE_THREAD` CMake options
(default OFF = fully serial); without them the tool still runs and reports,
just single-threaded.

## Fixture Coverage

Primary fixture locations in the repository:

- `tests/usda/*.usda`
- `tests/usda/fail-case/*.usda`
- `tests/usdc/*.usdc`
- `models/`

The `ctest` parser and roundtrip runners only operate on top-level `*.usda` or `*.usdc` files in their configured fixture directories. They do not recurse.

## Acutest Unit Suite

The main unit executable is built from `tests/unit/CMakeLists.txt` and registered through `tests/unit/unit-main.cc`.

The suite currently contains 600+ registered test cases. Coverage spans Core parser/value/stage/composition/writer functionality plus Tydra scene-access, RenderScene conversion, shader queries, physics, and IK/rigid-body solvers.

Major source groups in `tests/unit/` (see `tests/unit/CMakeLists.txt` for the full source list):

- Core parsing and value handling: `unit-ascii-parse`, `unit-value-types`, `unit-customdata`, `unit-primvar`, `unit-timesamples`, `unit-fp-parse-print`, `unit-minijson`, `unit-strutil`, `unit-math`, `unit-xform`, `unit-half-roundtrip`
- Scene graph and composition: `unit-stage`, `unit-composition`, `unit-composition-arcs`, `unit-composition-graph`, `unit-layer`, `unit-primspec`, `unit-prim-api`, `unit-prim-reconstruct`
- Reader/writer coverage: `unit-usda-reader`, `unit-usdc-reader`, `unit-usdc-reconstruct`, `unit-usda-writer`, `unit-usda-roundtrip`, `unit-usdz-writer`, `unit-usdc-writer`, `unit-crate-writer`, `unit-usd-validation`
- Tydra coverage: `unit-tydra`, `unit-tydra-renderscene`, `unit-tydra-shader`, `unit-materialx`
- Subdivision: `feat-subdiv` (tinysubdiv feature test under tests/feat/subdiv)
- Physics / simulation: `unit-physics`, `unit-ik`, `unit-rb-collision`, `unit-rb-dynamics`
- Security and utility coverage: `unit-security`, `unit-task-queue`, `unit-tiny-container`, `unit-tiny-hashmap`, `unit-handle-allocator`, `unit-ioutil`, `unit-pathutil`, `unit-pprint`
- PXR compat API: `unit-pxr-compat-api` (conditionally compiled with `LIGHTUSD_WITH_PXR_COMPAT_API`)
- Array/time-samples dedup: `unit-dedup` (CrateWriter value dedup, cross-attribute timeSamples dedup, shared times arrays, default scalar dedup)

The unit suite currently registers **~1,022 tests** (see `TEST_LIST` in
`tests/unit/unit-main.cc`).

Run it directly:

```bash
./build/unit-test-lightusd
```

List individual Acutest cases:

```bash
./build/unit-test-lightusd --list
```

Run one case:

```bash
./build/unit-test-lightusd crate_writer_cone_test
```

Representative Tydra-related cases:

- `tydra_connection_validation_test`
- `tydra_scene_access_helper_test`
- `tydra_shader_scene_access_test`
- `tydra_skel_scene_access_test`
- `tydra_renderscene_single_mesh_test`
- `tydra_renderscene_material_binding_test`
- `tydra_shader_get_bound_material_test`

## Parser Corpus Runners

These are the Python scripts used by `ctest`.

### USDA parser runner

`tests/usda/unit-runner.py` runs:

- every top-level `*.usda` file under `--basedir` as a success case
- every `fail-case/*.usda` file under `--basedir` as an expected failure

It exits nonzero when any success case fails to parse.

```bash
python3 tests/usda/unit-runner.py \
  --app ./build/test_lightusd \
  --basedir tests/usda
```

### USDC parser runner

`tests/usdc/unit-runner.py` runs:

- every top-level `*.usdc` file under `--basedir` as a success case
- every `failure-case/*.usdc` file under `--basedir` as an expected failure

It exits nonzero when any success case fails, or when an expected-failure file parses successfully.

```bash
python3 tests/usdc/unit-runner.py \
  --app ./build/test_lightusd \
  --basedir tests/usdc
```

### Standalone loader binary

The parser runners above use `test_lightusd`, built from `tests/test-main.cc`.

`test_lightusd`:

- dispatches by extension to `LoadUSDAFromFile`, `LoadUSDCFromFile`, `LoadUSDZFromFile`, or `LoadUSDFromFile`
- returns success/failure based on the parser result
- supports an optional `--verbose`
- is marked in source as a candidate for future deprecation in favor of `lusdcat`

## Roundtrip Runners

### USDA roundtrip

`tests/usda/roundtrip-runner.py` runs `usda_roundtrip` over top-level `*.usda` fixtures and supports:

- `--verbose`
- `--dump-on-fail`
- `--skip-file`
- `--no-skip-known`

The runner supports a `KNOWN_FAILURES` skip list.

```bash
python3 tests/usda/roundtrip-runner.py \
  --app ./build/usda_roundtrip \
  --basedir tests/usda \
  --verbose
```

### USDC roundtrip

`tests/usda/usdc-roundtrip-runner.py` runs `usdc_roundtrip` over top-level `*.usda` fixtures and reports pass/fail per file.

```bash
python3 tests/usda/usdc-roundtrip-runner.py \
  --app ./build/usdc_roundtrip \
  --basedir tests/usda \
  --verbose
```

## USDC Writer Diff Test

`usdc-writer-diff-test` exercises the full USDC write/read cycle with Layer-level diff comparison.

Pipeline per file:

1. `lusdcat input.usda -o temp.usdc` — LightUSD writes USDC
2. `lusdcat temp.usdc -o temp_rt.usda` — LightUSD reads USDC back as USDA
3. `lusddiff input.usda temp_rt.usda` — Layer-level diff of original vs roundtripped USDA

The `ctest` target runs in `--report-only` mode (informational — always exits 0) because the USDC writer does not yet preserve all property and metadata details.

For strict validation:

```bash
python3 tests/usdc-writer/usdc-writer-runner.py \
  --lusdcat ./build/lusdcat \
  --lusddiff ./build/lusddiff \
  --basedir tests/usda \
  --verbose
```

### lusddiff

`lusddiff` is the Layer-level diff tool built from `tools/lusddiff/` (requires `-DLIGHTUSD_BUILD_TOOLS=ON`). It loads both files via `LoadLayerFromFile` (PrimSpec tree) and reports added, deleted, and modified prims and properties.

```bash
# Text diff
./build/lusddiff file1.usda file2.usda

# JSON diff
./build/lusddiff --json file1.usda file2.usda

# Quiet mode (exit code only: 0 = identical, 1 = different, 2 = error)
./build/lusddiff --quiet file1.usda file2.usda
```

## Feature Tests

The feature-test layer is now partially integrated into `ctest`.

Registered feature executables include:

- MaterialX: `feat-mtlx-parse`, `feat-mtlx-import`, `feat-mtlx-export`, `feat-mtlx-grouped-params`
- Variant support: `feat-variant-converter`, `feat-variant-applier`

Some older feature work still exists outside `ctest` under `tests/feat/` as standalone programs, benchmarks, or notes. Not everything under `tests/feat/` is automatically built or registered.

For `bench-parse-opt` specifically:

- `ctest -R bench-parse-opt` exercises the reduced `--quick` profile.
- `./build/bench-parse-opt` runs the full synthetic workload for ad hoc performance work.
- `./build/bench-parse-opt --quick` runs the same reduced profile directly.

## Tydra Testing

Tydra is covered in two different ways.

### Unit coverage inside `unit-test-lightusd`

The Tydra unit sources are:

- `tests/unit/unit-tydra.cc`
- `tests/unit/unit-tydra-renderscene.cc`
- `tests/unit/unit-tydra-shader.cc`

These cover:

- scene-access helpers
- material binding validation
- texture and envmap loader policy
- skeletal animation and skin binding validation
- RenderScene conversion for empty stages, mesh stages, transforms, materials, lights, cameras, and memory estimation
- shader listing and material query behavior

### Manual corpus conversion runner

The `tydra_to_renderscene` example target is built from `examples/tydra_to_renderscene/CMakeLists.txt` and lands in `build/tydra_to_renderscene`.

`tests/tydra_to_renderscene/runner.py`:

- is not registered with `ctest`
- assumes it is launched from `build/`
- executes `./tydra_to_renderscene`
- recursively scans a USD tree with case-insensitive matching for `.usd`, `.usda`, `.usdc`, and `.usdz`
- prints success and failure lists, but does not currently `sys.exit(1)` on failures

Typical usage:

```bash
cd build
python3 ../tests/tydra_to_renderscene/runner.py ../models
```

## Standalone and Manual Targets

### Stable `next` library tests

`next` (`src/next/`, library `lightusd_next`) has a standalone regression suite.
Run it explicitly when changing shared code or the next product:

- It is a **standalone CMake project** (`src/next/CMakeLists.txt` with its own
  `project()`). Its tests do not appear in the default legacy `ctest` tree.
  The root build can also select it with `LIGHTUSD_NATIVE_PRODUCT=next`; use a
  separate Ninja build tree for each product.
- Its tests are gated behind `LIGHTUSD_NEXT_BUILD_TESTS` (**OFF by default**).
- These checks are a required regression gate in addition to the default
  native suite; passing one does not replace the other.

Regression coverage to keep in mind when touching `next`:

- `test_tydra_next.cc` also covers the compiled chunk-storage engine: exact-tail
  compaction repeated under allocation accounting, self-append and COW,
  over-aligned POD elements, move/reuse, allocation-budget refusal, and
  read-only iteration/indexing without detachment. Writes now use
  `mutable_at()` / `mutable_chunk_data()` explicitly.
- `unit-test-lightusd` includes `minijson_shortest_double_roundtrip_test`:
  finite binary64 boundary values and 4096 deterministic bit-pattern samples
  must serialize/reparse exactly (with a separate signed-zero spelling check).
  Run this legacy unit target as well as standalone next tests after changing
  the shared MiniJSON serializer.
- `test_validation.cc` covers the immutable rule table's pointer/count contract
  and idempotent severity upgrades. Run the full PCP and threaded PCP tests
  after changing the out-of-line cache implementation, not only validation.
- `test_stage.cc` — PropNameId overloads must be invalid-id-safe, the
  stage-level `HasTimeSamples()` / `HasValueClips()` scans must match the
  flat root-layer prim array, and every schema-accessor name must be
  pre-registered in `PropNameTable::register_common_names()` (a render-phase
  `intern()` miss on the frozen table unfreezes it, disabling the lock-free
  render path — see `property-index.cc`).
- `test_schemas.cc` — schema accessors (UsdGeomMesh etc.) must return empty /
  schema-fallback values on prims missing the queried arrays.
- `test_tydra_next.cc` — the animation-extraction gate
  (`animation.enabled && (HasTimeSamples() || HasValueClips())`) must keep
  emitting AnimationClips for value-clip-only stages (authored-time-sample
  stages, static stages, and `animation.enabled = false` are covered too,
  on both `Convert()` and `ConvertToSink()`); schema/tydra accessors must
  never unfreeze the frozen name table; the skeleton joint remap must resolve
  leaf-name `skel:joints` tokens through the fallback scan; and
  `retain_geometry = false` must release static stage arrays while keeping
  time-sampled ones.

Build and run them on demand in a separate build directory. The preferred
entrypoint is:

```bash
scripts/run-next-checks.sh
```

Useful environment overrides:

```bash
scripts/run-next-checks.sh --help
BUILD_DIR=build-next-tsan BUILD_TYPE=Debug THREADS=ON scripts/run-next-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release scripts/run-next-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release BENCH_LAZY_VERTS=4000000 BENCH_LAZY_CLONES=32 scripts/run-next-checks.sh
```

The equivalent manual commands are:

```bash
# Configure the standalone next project with its tests enabled.
cmake -S src/next -B build-next \
  -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DLIGHTUSD_NEXT_ENABLE_THREAD=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-next -j16
ctest --test-dir build-next --output-on-failure
```

> **Caveat — use a Debug build.** The `tests/next/` programs validate via bare
> `assert()`. Under `NDEBUG` (i.e. `Release`/`RelWithDebInfo`) `assert(expr)`
> does not evaluate `expr`, so these tests check nothing — and any side effect
> placed inside an `assert()` is silently dropped (this previously caused a
> null-dereference SIGSEGV in `test_usdc_roundtrip` when the `toc`-populating
> `ParseUSDCBinary()` call was compiled out). Build with `-DCMAKE_BUILD_TYPE=Debug`
> for meaningful validation, and keep side-effecting calls out of `assert()`.

Current USDC-focused next coverage:

- `next_test_usdc_malformed` uses generated in-memory crate fixtures for bad
  magic, truncated bootstrap, invalid TOC offsets/ranges, excessive section
  counts, missing required sections, allocation caps, and malformed
  FIELD/FIELDSET/SPECS payloads. Prefer adding compact generated cases here when
  a malformed input can be described structurally.
- `next_test_crash_regressions` replays minimized fuzzer-found binary inputs
  from `tests/next/crash_regressions/`. Prefer this only when the exact byte
  sequence matters.
- `next_test_usdc_roundtrip` includes focused roundtrip cases plus a dense
  generated fixture covering layer metadata, dictionaries, composition arcs,
  mesh arrays, per-property metadata, relationships, connection-flagged
  properties, Shader/Material links, PointInstancer arrays/prototypes, ids, and
  time samples.

### CMake targets not in ctest

These targets exist in the CMake test infrastructure but are not currently registered with `ctest`:

- `test-decompress-int` — defined in `tests/decompress-int/CMakeLists.txt` but its `add_test(...)` block is still commented out.
- `pprint_benchmark` — defined in `tests/pprint/CMakeLists.txt` as a standalone benchmark executable.

### Feature-directory standalone tools

Several directories under `tests/feat/` contain standalone programs built via local Makefiles, not through CMake or `ctest`. These are developer tools for ad hoc benchmarking and manual testing:

| Directory | Contents | Notes |
| --- | --- | --- |
| `hash/` | `hash_bench.cc` | Hash function microbenchmark |
| `tangent/` | `bench_tangent.cc` | Tangent computation benchmark |
| `tydra-mesh-build/` | `bench_mesh_build.cc` | RenderScene mesh-build benchmark |
| `zstdusd/` | `test_zstd_usd.cc`, `compress_usda.cc` | Zstd-compressed USD read/write tools |
| `nestedVariantSet/` | `test_variant_api.cpp` | Standalone variant API exerciser (the ctest-registered tests are `feat-variant-converter` and `feat-variant-applier`) |

Some earlier standalone tests have been folded into the Acutest unit suite (`typed-array-view`, `typed-array-timesamples`, `value-view`). Their `tests/feat/` Makefiles still exist but the canonical coverage now lives in `unit-test-lightusd`.

### Feature fixture directories

These directories hold `.usda` fixtures used by feature tests or as reference material. They are not scanned by any `ctest` runner:

| Directory | Contents |
| --- | --- |
| `tests/feat/lux/` | Light shader and mesh-light test scenes (7 USDA files + MaterialX reference) |
| `tests/feat/node-mtlx/` | Blender-style MaterialX node USDA files (CombineColor, MapRange, Math, etc.) |
| `tests/feat/skinning/` | Skeletal animation fixtures (static, timesampled, mixed) |

### MaterialX standalone tests

Besides the four ctest-registered targets (`feat-mtlx-parse`, `-import`,
`-export`, `-grouped-params`), `tests/feat/mtlx/` holds extra source files built
only via its local Makefile, e.g. `test_nodegraph_export.cc`,
`test_mtlx_include_traversal.cc`, `test_parser_debug.cc`, and
`threejs_mtlx_export_example.cc`.

## Additional Corpus Runner

`tests/parse_usd/runner.py` is a separate broad parser sweep tool. It is not used by `ctest`.

Important current behavior:

- it looks for `build_release/lusdcat`, not `build/test_lightusd`
- it recursively scans a target tree for `.usd`, `.usda`, `.usdc`, and `.usdz`
- it invokes `lusdcat -l <file>`
- it supports `--timeout`
- it prints failures but does not currently return a failing process exit code based on the failure list

Typical usage:

```bash
python3 tests/parse_usd/runner.py models --timeout 180
```

## Roundtrip Comparison Against OpenUSD

For the full batch comparison against OpenUSD v26.05 `usdcat`, use the second half of
the [Full Regression Tests](#full-regression-tests) sequence above. The runner
compares all top-level USDA fixtures under `tests/usda/` and all top-level USDC
fixtures under `tests/usdc/`.

Short form:

```bash
LUSDCAT_PATH=./build/lusdcat \
USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-usdcat-compare.sh
```

Single-file mode through the Node.js comparator:

```bash
node tests/compare-usda.js \
  --lusdcat ./build/lusdcat \
  --usdcat ref/dist/bin/usdcat \
  --detailed-diff \
  tests/usda/somefile.usda
```

## Python Bindings Test

`python/tests/` contains pytest-based tests for the pure CPython C-API Python
binding. This is not integrated into `ctest` and requires the Python package
to be built and installed separately.

```bash
pip install -e . --no-build-isolation
python3 -m pytest python/tests -q
```

## Fuzzing

Fuzzer targets live under `tests/fuzzer/`. They are built separately from the CMake test suite using Meson and libFuzzer.

Documented setup in `tests/fuzzer/README.md`:

```bash
CXX=clang++ CC=clang meson build -Db_sanitize=address
cd build
ninja
```

Examples from the current README:

```bash
./fuzz_lightusd -max_len=128m
./fuzz_intcoding_decompress -rss_limit_mb=8192 -jobs 4
```

Relevant fuzz entry points include:

- `lightusd_fuzzmain.cc`
- `usdaparser_fuzzmain.cc`
- `usdcparser_fuzzmain.cc`
- `lz4_decompress_fuzzmain.cc`
- `intCoding_decompress_fuzzmain.cc`

## Adding or Updating Tests

### Add a new Acutest unit

1. Declare the test function in the corresponding `tests/unit/unit-*.h` header.
2. Implement it in the matching `tests/unit/unit-*.cc` file with `TEST_CHECK` or `TEST_ASSERT`.
3. Register it in `tests/unit/unit-main.cc`.
4. Rebuild and run `ctest -R unit-test-lightusd --output-on-failure`.

### Add a new `ctest` target

1. Add the executable in the top-level `CMakeLists.txt` under `if(LIGHTUSD_BUILD_TESTS)`.
2. Register it with `add_test(...)`.
3. If it is a benchmark, label it consistently, as `bench-parse-opt` already does with `LABELS "benchmark"`.

### Add parser or roundtrip fixtures

1. Place the fixture in the relevant top-level directory:
   `tests/usda/`, `tests/usda/fail-case/`, `tests/usdc/`, or `tests/usdc/failure-case/`.
2. Keep in mind the current runners do not recurse for the `ctest` corpus tests.
3. Re-run the matching parser or roundtrip runner.

## Disabled Tests (TODO/FIXME)

The USDC memory-budget, variant PrimSpec/roundtrip, and array-dedup tests that
were disabled after the `spec-2026-mar` merge have since been **re-enabled** —
all are active registrations in `tests/unit/unit-main.cc` (`unit-dedup.cc` is
back in `TEST_SOURCES`; the dedup suite now also covers cross-attribute
timeSamples dedup, default scalar dedup, and shared times arrays), and the
static fixtures (`variantSet-collision-001.usdc`,
`variantSet-prim-001.usdc` in `tests/usdc/`) load successfully.

There are currently **no disabled Acutest tests**.

(Note: `crate_writer_validation_disabled_test` and `column_wrap_disabled_test`
are *active* tests despite "disabled" in their names — each verifies behavior
when a feature is turned off.)

## Large Test Fixtures

Test fixture files should stay under 50KB to avoid hitting `CrateReaderConfig.maxTokenLength` (default 64K). When the crate format stores string values as tokens, large strings can exceed this limit.

Files that were reduced to stay within bounds:

- `tests/usda/memory-budget-attr-customdata-001.usda` — blob reduced from 1.2MB to 32K
- `tests/usda/memory-budget-customdata-001.usda` — blob reduced from 1.2MB to 32K

Large fixture files outside `tests/usda/` and `tests/usdc/` (not affected by the token limit, listed for reference):

- `tests/feat/node-mtlx/RealisticScene.usda` (244K)
- `tests/usda/suzanne.usda` (148K) — geometry data, tokens are short
- `tests/feat/node-mtlx/ChainTest.usda` (84K)
- `tests/feat/node-mtlx/ExtractPatternTest.usda` (72K)

## Known Gaps

The current infrastructure has a few operational gaps worth keeping in mind:

- `ctest` can be fully configured even when the test executables are not yet built.
- `tests/tydra_to_renderscene/runner.py` and `tests/parse_usd/runner.py` are manual tools and do not currently fail the process based on collected failures.
- `tests/decompress-int` and `tests/pprint` are built outside the main `ctest` suite.
- Several standalone feature benchmarks and tools under `tests/feat/` (hash, tangent, tydra-mesh-build, zstdusd) use local Makefiles and are not part of CMake or `ctest`.
- The Python bindings tests (`python/tests/`) are not integrated into `ctest`.
- Feature fixture directories (`lux/`, `node-mtlx/`, `skinning/`) provide test data but are not exercised by any automated runner.
- The experimental `next` module (`src/next/`, `tests/next/`) is a standalone CMake project excluded from `build/` `ctest` and the regression gate by design (`LIGHTUSD_NEXT_BUILD_TESTS=OFF`); its `assert()`-based tests are only meaningful in Debug builds. See [Experimental `next` library tests](#experimental-next-library-tests).
