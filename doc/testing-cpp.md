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

Native `lusdzconvert` currently requires `LIGHTUSD_NATIVE_PRODUCT=legacy` and
`LIGHTUSD_BUILD_TOOLS=ON`; use a separate Ninja tree such as
`build_ninja/legacy-convert` (see [converter build instructions](../tools/lusdzconvert/README.md#build)).
With tests enabled, `lusdzconvert-regression-driver` checks JPEG asset remapping,
material deduplication, directory root discovery, both USDA/USDC package roots,
and both flat layer formats. Flat outputs written into a different directory
must retain reachable references to the original external images.

UDIM baking has registered `usdz_convert_udim_bake_test`,
`usdz_convert_udim_layout_test`, and `usdz_convert_udim_clip_test` cases for
flat-path rebasing, retained-atlas budgets, atlas orientation/limits, PNG16/EXR precision, clipping
area/winding, and discrete attributes. The shared WASM/native fixture in
`web/js/tests/udim-bake.test.mjs` adds grid/dense layer edits, deformation and
texture-file animation, skins, blendshapes/in-betweens, subdivision, variants,
instances, streaming, shared-tile references, overlapping dependency tile names,
and transactional sidecars. Set
`LIGHTUSD_NATIVE_USDZCONVERT` to the built converter to exercise all three native
output formats. See [UDIM conversion options](../tools/lusdzconvert/README.md#bake-udims-into-one-texture).

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

## JSON backend

The internal USD-to-JSON converter uses the repository's `minijson`
implementation as its canonical representation. Deprecated overloads that
expose `nlohmann::json` are disabled by default; enable them explicitly with
`-DLIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT=ON` when maintaining an application
that still uses that API.

Tydra/MCP JSON interfaces and vendored third-party readers currently retain
their nlohmann-compatible boundary. They should be migrated through a typed
minijson adapter before changing those public interfaces.

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

#### External asset gates

Point the ignored repository-local `usd-assets` symlink at an existing
`usd-wg/assets` checkout. Set `LIGHTUSD_USD_ASSETS_ROOT` at configure time as
well as `USD_ASSETS_ROOT` at runtime. The batch runner resolves symlink roots
after argument parsing, including `--root`, so its corpus scan cannot silently
run against an empty list.

```bash
# If the repository-local link does not already exist:
ln -s "$USD_ASSETS_ROOT" usd-assets
cmake -S . -B build_ninja/legacy-convert -G Ninja \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy -DLIGHTUSD_BUILD_TOOLS=ON \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_GUI_VIEWER=ON \
  -DLIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD=ON \
  -DLIGHTUSD_USD_ASSETS_ROOT="$PWD/usd-assets"
cmake --build build_ninja/legacy-convert -j16
xvfb-run -a -s "-screen 0 1280x800x24" env \
  USD_ASSETS_ROOT="$PWD/usd-assets" LUSDVIEW_RUN_GOLDEN=1 \
  LUSDVIEW_RUN_USD_ASSETS_BROAD=1 LUSDVIEW_RUN_USD_ASSETS_ANIMATION=1 \
  LUSDVIEW_RT_ALLOW_COLD_COMPILE=1 \
  ctest --test-dir build_ninja/legacy-convert --output-on-failure
```

The external native gates cover the material resolver, alpha sorting, three
stacked-glass modes, OpenChess materials/path tracing, corpus parsing, broad
and animation rendering, cold MaterialX promotion, and golden fingerprints.
The thirteenth gate, Island production smoke, also needs `ISLAND_USD` pointing
at that separately downloaded scene. The small stacked-glass fixture is now
tracked under `tests/usda/`, so those three gates need no model download.
The native corpus test also receives the configured asset root directly.
The Vulkan render smoke also requires meshes with custom scalar float/int and
vector rest-position primvars to survive the public API buffer copy. Sizes are
measured in scalar elements; using the chunk-container size discards geometry
such as the composed public Teapot asset even though composition succeeds.
The CPU `lusdview_next_mesh_adapter_test` checks copied bounds, independently
interpolated UV sets, indexed/custom primvars, proxies, and ownership after the
temporary render record and stage are destroyed. Secondary-UV interpolation
uses a formerly reserved byte of the public mesh info, preserving its ABI layout.
`lusdview-next-mesh-materials` compares ordinary meshes with PointInstancer
and native-instance panels using generated textures under Vulkan ray query.
Both panels must use the secondary UV set for preview bindings and the full
material for `--material-purpose full`. A malformed-topology panel also checks
that GeomSubset bindings retain authored face numbering after invalid faces
are removed; the public render buffer API preserves that remap. A nonblank
capture alone cannot pass these color checks.
The same test includes direct and indirect PointInstancer prototype cycles.
The loader must diagnose and skip each cyclic branch while rendering the valid
meshes. Prototype expansion also stops at depth 64. Placement products are
bounded before allocation, and budget truncation marks the scene truncated.
Additional panels check analytic Cube prototypes, PointInstancer prototype roots,
inactive descendants, and instance-budget admission. Hidden or unresolved
placements must not consume the budget; omitted visible placements must set the
structured truncation report. `--case` selects an individual fixture for diagnosis.
Island report validation expects schema version 2. The cold-promotion test
warms only the compact hardware pipeline before checking a cold full shader;
it sets its own cold-compile policy independently of the surrounding suite.
It uses a self-contained procedural MaterialX fixture in path-tracing mode,
which requests full promotion. Optional promotion requires a readiness marker
for the requested shader, since a shared cache can contain unrelated shaders.
The OpenChess production gate is separately enabled with
`LUSDVIEW_RUN_OPENCHESS_PATH=1`; select `LUSDVIEW_OPENCHESS_PT_BACKENDS=cuda`
to omit HIP. It requires actual geometry and completed sample targets, as well
as image/report output. Its temporary referencing rig uses the explicit
`--allow-parent-paths` local-file compatibility mode. The viewer now applies
that mode to the document input policy while keeping finite memory limits.

Golden comparison is opt-in and the checked-in fingerprints are a per-machine
baseline. Inspect failures and their images before refreshing fingerprints.
Finish the build before starting a corpus sweep: linking a new viewer binary
while a batch is launching it can produce spurious permission errors.

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
| `lusdview-camera-clipping-planes` | Authored camera clipping on GL/Vulkan raster | `xvfb-run`, Vulkan backend |
| `lusdview-camera-dof-raster` | Thin-lens response on GL/Vulkan raster | `xvfb-run`, Vulkan backend |
| `lusdview-camera-motion-raster-export` | Native/companion Vulkan shutter accumulation and closed shutter | Vulkan backend |
| `lusdview-camera-motion-raster-legacy` | Legacy-loader native/companion camera shutter parity | Vulkan backend |
| `lusdview-camera-motion-raster-gl` | Native/companion OpenGL shutter accumulation and closed shutter | `xvfb-run` |
| `lusdview-camera-motion-vulkan` | Vulkan RT temporal poses retain progressive accumulation | Vulkan RT adapter |
| `lusdview-shadow-alpha-inst` | Alpha-cutout + PointInstancer shadow | No GPU |
| `lusdview-aousd-conformance` | AOUSD spec render conformance | No Vulkan backend |
| `lusdview-gl-vk-parity` | GL/VK raster image agreement | No GPU |
| `lusdview-raster-shadow-map` | Raster shadow map regression | No GPU |
| `lusdview-raster-multilight` | GL/Vulkan linked finite and directional light parity | `xvfb-run`, Vulkan backend |
| `lusdview-raster-geometry-light` | GL/Vulkan resolved GeometryLight mesh-sample parity | `xvfb-run`, Vulkan backend |
| `lusdview-area-light-vulkan` | Vulkan RT finite-light soft-shadow response | Hardware Vulkan RT |
| `lusdview-area-light-cuda` | CUDA finite-light soft-shadow response | CUDA device |

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

USDC reader hardening regressions can be run with:

```sh
./build_ninja/unit-test-lightusd usdc_reader_layer_memory_limit_test \
  usdc_reader_truncated_array_test security_stream_exact_read_test \
  security_stream_relative_seek_test
```

These cover stage/layer decoding-budget parity, rejection of incomplete array
payloads in default and strict loading (including zero-copy options), and
overflow-safe relative seeks. Crate payload reads require all requested bytes;
the stream's separate partial-read API retains its existing behavior. Run the
stream tests under UBSan as well to catch signed-overflow regressions. The
fixtures are generated in memory and do not require external scene assets.

Collection and material-binding writer regressions can be run with:

```sh
./build_ninja/unit-test-lightusd usdc_reader_collection_binding_roundtrip_test
ctest --test-dir build_ninja -R '^scope-imageable-roundtrip$' --output-on-failure
node tests/next/test-compare-usda-booleans.cjs
```

The writer test covers typed bindings and collections on Model, Scope, and
Xform through USDA and USDC, including membership-expression metadata, blocked
values, and declarations without values. The comparison test accepts equivalent
boolean spellings only on matching `bool` declarations, while preserving real
value differences. It also runs in the standalone `next` CTest suite.

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

#### AOUSD Core supplemental inputs

The official [Core supplemental repository](https://github.com/aousd/core-spec-supplemental-public)
keeps releases under `releases/`, with the license at the checkout root.
Download the pinned public release into an ignored build directory, then point
the standalone Debug suite at the release directory:

```bash
git clone --depth 1 --branch release/1.0.1.post0 \
  https://github.com/aousd/core-spec-supplemental-public.git \
  build_ninja/aousd-supplemental
cmake -S src/next -B build_ninja/next-debug -G Ninja \
  -DLIGHTUSD_NEXT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DLIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT="$PWD/build_ninja/aousd-supplemental/releases/1.0.1"
cmake --build build_ninja/next-debug -j16
ctest --test-dir build_ninja/next-debug --output-on-failure
ctest --test-dir build_ninja/next-debug -L aousd -V
```

This release pins commit `c15ae0cad3ed9e07a25dffd6699627d2c166cab0`.
The supplemental bridge loads 54 file-format fixtures, checks expected prim
population across 138 composition cases, and checks resolved samples in eight
value-resolution cases. Its data-type bridge runs the library's AOUSD type
checks; it does not execute the upstream Python/OpenUSD test runner or compare
every PCP field. Keep these coverage limits separate from full spec conformance.

Reference `customData` tests now require strict USDC loads, typed nested-value
retention, qualified list-op identity, and memory/streaming writer roundtrips.
Legacy metadata roundtrip tests also require explicit `None` opinions for
relationships, inherits, and specializes. WASM layer tests cover reference
dictionaries through USDA, USDC, and JSON exports.

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
  FIELD/FIELDSET/SPECS payloads. Generated valid compressed float/half arrays
  also exercise integer and lookup-table codecs in eager and lazy readers,
  including half rounding and quaternion swizzling. Widened half/uchar outputs
  must fit the configured memory budget. Additional cases reject repeated
  long token/string/asset/path-expression copies and token/string vectors,
  preserve deferred memory limits after reader destruction, and charge
  compressed bool integer scratch before allocation. Prefer compact cases when
  a malformed input can be described structurally.
- `next_test_crash_regressions` replays minimized fuzzer-found binary inputs
  from `tests/next/crash_regressions/`. Prefer this only when the exact byte
  sequence matters.
- `next_test_usdc_roundtrip` includes focused roundtrip cases plus a dense
  generated fixture covering layer metadata, dictionaries, composition arcs,
  mesh arrays, per-property metadata, relationships, connection-flagged
  properties, Shader/Material links, PointInstancer arrays/prototypes, ids, and
  time samples. It also checks half/quaternion arrays with distinct component
  values in both eager and lazy readers, plus empty and full-byte-range
  uchar/bool arrays with non-aligned tails. These cover overlap-safe half
  widening and bounded source views used by eager array decoding. Reference
  and payload offsets/scales retain binary64 precision through two crate cycles,
  including tiny scales that must not round to zero.
- `next_test_lazy_array` checks that separate materializations share a cumulative
  allocation budget (including after source-owner release). Threaded builds
  race independent lazy copies under a budget admitting only one decode.
  The byte-backed bool factory must canonicalize nonzero lanes while adopting
  the original buffer.

Crate allocation limits continue after loading: lazy sources retain the same
cumulative budget as their reader. Temporary codec buffers are charged
conservatively, and repeated materialized copies consume the shared budget;
borrowed array views and verbatim write-through do not decode array storage.
A deferred decode that exceeds the budget follows the existing failure contract:
`materialized_copy()` returns an empty Value and typed access returns null.

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

After changing the WASM shading snapshot binding, rebuild both web modules and
run `node tests/webgpu-mtlx-usd-graph.mjs` from `web/js`; the aggregate Node
profile includes this typed connection/default preservation fixture.

The separate JavaScript/WebGPU renderer has a focused Chrome hardware gate:
run `node tests/webgpu-mtlx-chrome.mjs --hardware` from `web/js`. It covers
analytic transport, pinned MaterialX graph kernels and resource loading; add
`--shaderball --authored-lights` for the authored RectLight smoke scene, or
`--shaderball --performance` for the diagnostic 720p raster gate. These tests do
not establish full MaterialX/reference-rendering conformance. See
[WebGPU renderer status](../web/js/docs/webgpu-mtlx.md) and the
[web regression procedure](../web/js/docs/regression.md).

Use `--bump-only` for analytic textured bump and height-to-normal image chains
in both physical and raster modes. `--frames-only` isolates UV normal frames,
`--opacity-only` checks stochastic cutout coverage, and `--numeric-only` runs
value kernels without the demo pipeline. These focused checks complement,
not replace, the broader renderer gate.

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

CTest-integrated tool checks use the first `python3`/`python` found on `PATH`
by default. Configure a specific interpreter when running from a virtualenv or
when the build and test interpreters differ:

```bash
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_PYTHON_EXECUTABLE="$VIRTUAL_ENV/bin/python"
```

`python/tests/` contains pytest-based tests for the pure CPython C-API Python
binding. They remain opt-in because the extension must be built and installed
separately. After `pip install -e . --no-build-isolation`, register them in the
native CTest tree with `-DLIGHTUSD_BUILD_PYTHON_TESTS=ON`; CMake registers the
test only when the selected interpreter can import both `pytest` and `lightusd`.
For the reproducible local setup, use the [`uv` workflow](python_binding.md#recommended-uv-workflow)
and pass `.venv/bin/python` through `-DLIGHTUSD_PYTHON_EXECUTABLE`.

```bash
pip install -e . --no-build-isolation
python3 -m pytest python/tests -q

# Equivalent opt-in CTest registration:
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_PYTHON_TESTS=ON
ctest --test-dir build_ninja -R '^python-lightusd-tests$' --output-on-failure
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

`security_sha256_digest_test` checks empty/null handling, standard ASCII and
binary SHA-256 vectors, and both padding/block boundaries. The asset-cache
fixture in `web/js/tests/apply-variant-selection-overload.test.mjs` compares
against Node's crypto implementation on wasm32 and memory64, where native-only
coverage would miss size_t-width bugs in bit-count encoding.

## Workflow tools and next examples

`next_workflow_examples` generates small inputs and checks composition, typed
queries, USDA/USDC reopening, payload/variant edits after snapshot publication,
cancellation, sampled/spline/skeletal evaluation and portable package relocation.
It is also registered in standalone next builds with examples enabled.
All four C++ workflow examples consume the public ownership facade; the
animation traversal keeps stage-retaining `Prim` objects through its work list.
The interactive workflow exercises the installed-header C++ document/render
session facade over the C boundary, including RAII snapshot ownership and
retained-snapshot prim queries. `next_test_document_session_c` separately compiles as strict
C11 and checks retained snapshots, revisions, render updates, stale snapshots,
prepare/abort/commit, and cancellation; it is registered when Tydra is enabled.

`workflow_tools` additionally exercises JSON crate limits, bounded dependency
reports and all-authored variants, variant/property provenance, checker baseline
and SARIF output, directory/package diffs, textured GLB export and strict losses.
When `lusdrender` is built, it runs the two-camera synthetic capture and checks
raw depth, world normals and source IDs numerically. Temporary assets are removed
after the run. `next_gltf_export` tests API budget failures, invalid attribute
indices, malformed corner mappings, non-finite values and cyclic node graphs.
`next_test_render_session_c` also checks the public GLB export's binary result
and output-limit validation through a strict C11 consumer. The GLB writer is
isolated in its own object file; a render-only static consumer does not link it.
`lusdview_lod_stream_test` covers the C/POD viewer pre-pass with a composed
camera and two transformed districts, checking the generated promotion wrapper.
`next_test_c_api` loads the instancing fixture in both holder and native modes
to verify the new traversal option without changing the export default.

```sh
ctest --test-dir build_ninja -R '^(workflow_tools|next_workflow_examples|next_gltf_export)$' --output-on-failure
```

These headless checks require no GPU or display. They supplement the native and
standalone next regression gates; they do not replace viewer GPU tests.

## Core/render boundary regression

For C ABI 4 and the C++ ownership facade, run both configurations. The core-only
build must not compile Tydra or link the legacy library. The facade smoke test
covers stage retention, moves, generation invalidation, and render ownership
when enabled. C smoke tests also reject cross-owner evaluation, validate
caller-owned size queries, and exercise the animation and material payload
POD entrypoints' invalid-handle behavior.
The core C smoke test also covers optioned attribute evaluation at numeric and
default time, including held/linear selection and invalid modes.
The `lusdcat --explain` path is also a public-C-API consumer and should be
smoke-tested with a USDA fixture when changing property-trace behavior.

```sh
cmake -S src/next -B build_ninja/next-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DLIGHTUSD_WITH_TYDRA=OFF
cmake --build build_ninja/next-core
ctest --test-dir build_ninja/next-core --output-on-failure
cmake -S src/next -B build_ninja/next-render -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DLIGHTUSD_WITH_TYDRA=ON
cmake --build build_ninja/next-render
ctest --test-dir build_ninja/next-render --output-on-failure
```

To validate the root product-selection path and its public consumer gate:

```sh
cmake -S . -B build_ninja/root-next -G Ninja \
  -DLIGHTUSD_NATIVE_PRODUCT=next -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_WITH_TYDRA=OFF
cmake --build build_ninja/root-next --target next_c_api_example
ctest --test-dir build_ninja/root-next -R '^next_c_api_example_smoke$' \
  --output-on-failure
```

Rebuild the Python extension after C ABI changes. Validate installed consumers
with `find_package(LightUSD CONFIG)` using only the installed public headers.
For comparable full compile measurements, use target `lightusd_render_c` after
the split: measuring only `lightusd_c` would omit render functionality present
in the old combined target. `scripts/bench-compile.py --target ... --changed
src/next/stage/stage.hh` measures incremental rebuild time and compilation
count, restoring the input timestamp afterward. Run measurements without
concurrent builds or regression workloads.

The combined WASM loading fixture in
`web/js/tests/apply-variant-selection-overload.test.mjs` checks synchronous,
layer/cached and async loading, validation JSON, lifecycle/progress state, and
memory-probe size_t results on wasm32 and memory64. It also exercises the typed
C task/record validation and JS ownership across async yields, including a
callback deleting the original loader and a callback throwing `TypeError`.
Memory probes reject negative lengths and lengths whose conservative scratch
estimate exceeds the configured load-memory limit. The combined async binding
uses C++17 tasks and remains available independently of the legacy-only
`LIGHTUSD_WASM_COROUTINE` option. Select isolated rebuilt modules with
`LIGHTUSD_COMBINED_MODULE`; see `web/js/docs/regression.md` for the Node override
loader and full product regression procedure.

The same combined fixture covers MCP context isolation across sessions and
loader instances, counted Unicode/NUL-containing inputs, tool/resource JSON,
invalid C calls and stale JS receivers. It instruments all six MCP operations
and verifies that a dispatch `TypeError` is propagated without repeating a
potentially mutating tool call. These checks exercise the combined compatibility
backend, not next-only MCP feature parity.

Layer/export coverage in the combined fixture checks empty and populated
text/JSON exports, current JSON importer limitations for custom properties,
flatten/render conversion, owned USDC copies and reopening. The raw C export
result is retained across subsequent exports, loader reset and destruction to
verify ownership independently of JS wrapper behavior. Null/short records,
invalid operation IDs and stale receivers are covered on both pointer widths.

The completed combined export-family fixture covers caller-supplied USDC
buffers (validation order, duck-typed outputs, subviews, retry and reentrant
copies), USDZ root formats/ARKit overrides, asset filtering and remapping,
optimization aliases and field limits, and stage-snapshot behavior across JS
getters. Actual optimization fixtures check preview material deduplication,
aggregate mesh creation with deactivated source prims, and merge admission
limits. Raw C checks include malformed maps, invalid option masks, cross-loader
package handles and cleanup after throwing JS callbacks. USDC buffer exports
still materialize a native byte vector; USDZ results retain their borrowed-view
contract. Oversized byteLength values are rejected before integer conversion.

Combined render-query regressions cover counts, root ID/URI/up-axis,
configuration booleans, camera data, scene metadata and texture records. They
exercise authored time/unit metadata, owned arrays, camera FOV calculation,
transforms/bias/scale and both sparse and atlas UDIM linkage. The UDIM fixture
uses generated one-pixel PNGs in the asset cache and a generated USDZ package;
it catches filesystem probes in filesystem-free WASM builds. Cached sparse
loading and packaged atlas loading pass. Missing cache tile names still need
an existence-aware resolver test before cache-atlas extent parity is claimed.

Combined inspection JSON regressions cover MetaHuman attributes/relationships,
Unicode strings, authored shader connections, color values and asset paths.
The fixture pins unloaded/reset behavior and verifies that retained JS strings
survive reset. Raw C calls reject null loaders and unknown query IDs; adapter
checks cover wrong argument counts, stale receivers and TypeError propagation
without repeating native inspection. Both methods use the existing native JSON
producers and bounded string-table copies through `render_json`.

Combined instance query fixtures verify scaled/translated local and world
matrices, prototype/material/mesh IDs, missing IDs, fractional-index coercion,
ordered mesh matches and independent returned arrays. Native instances without
geometry explicitly exercise matching mesh ID `-1`. C record checks cover
null/short outputs and null loaders; JS checks include wrong types/counts,
stale receivers and rejection of result sizes exceeding the 32-bit allocator.

Combined root-node queries are covered by a hierarchy fixture with multiple
roots, ordered siblings, Unicode display names, parent-relative transforms and
reset-transform descendants. Separate resource rows pin mesh/camera/light
categories and the existing native-instance flags/IDs. Cursor tests verify
missing roots, null/short records without traversal advancement, repeated end
of traversal, cleanup after a TypeError and receiver retention through a query.
Returned child/matrix arrays remain owned JS values after scene reset.

Combined sparse-UDIM metadata tests preserve the tile container's existing
iteration order, tile coordinates/image IDs, asset identifier and owned JS
results. The unresolved-image fixture compares results with the image catalog
in image order and covers Unicode paths, reset, and missing results. Boundary
checks reject null pointers, invalid JS receivers/arguments and tile counts
that would overflow the 32-bit temporary allocator. This does not close the
separately recorded cache-atlas tile-discovery limitation.

Combined image-query regressions cover decoded pixels, encoded USDZ bytes,
matching metadata across borrowed-view/pointer/copy methods, independent owned
copies and metadata arrays, and custom ColorSpaceDefinitionAPI matrices without
pixel buffers. A forced heap growth during string-table copying verifies that
image bytes are read from the current heap. Warning checks preserve once-per-
loader state across clones/reset, including a throwing console callback. Raw C
records reject null/short outputs and invalid modes; JS rejects stale receivers
and invalid arguments without replaying a failed lazy-copy dispatch.

Combined light-query checks compare complete structured objects and exact JSON/
XML strings with `web/js/tests/fixtures/combined-light-queries.json`, captured
from the pre-migration combined product on both pointer widths. Signed zero
is encoded as `{"$negativeZero":true}` in this baseline. The checked light
fixtures cover the legacy output as observed, including omitted spectral data;
their filenames alone are not evidence of spectral or mesh-light support.
An injected C-record test separately checks owned spectral pair copies, empty
samples and invalid-span rejection. Other checks cover error precedence,
counted format inputs, null/short records, stale receivers and one-shot format
dispatch. The legacy GeometryLight fixture fails before querying and remains
an explicit backend limitation, not a passing migration gate.

Combined skeleton queries now have a skinned branching fixture whose preorder
joint IDs differ from authored indices. It checks names/paths, Unicode display
metadata, bind/rest matrices, parent indices, owned outputs and reset behavior.
A two-rig animation fixture checks all-skeleton enumeration and animation IDs.
Cursor checks cover null/short records without advancement, repeated end of
traversal, cleanup after exceptions and receiver retention during flat output.

Combined animation queries use the checked per-width snapshot in
`web/js/tests/fixtures/combined-animation-queries.json`, captured before their
C-boundary migration. The fixture retains native clip ordering, signed zero
and Float32Array payloads across transform, skeletal, blend-shape and custom
property scenes. It checks summaries, track filtering, independent sampler and
track arrays, and ownership after reset. Boundary checks reject null/short
records, invalid indices and unsafe sampler spans, and exercise retained loader
ownership and exceptions without replay. STEP/CUBICSPLINE interpolation mapping
uses injected C records; this does not establish native value-clip parity.
The negative animation-summary index check is a bounds-fix regression rather
than a baseline assertion against the formerly unchecked legacy access.

Combined mesh-operation tests cover indexed authored primvar inspection through
layer loading and deferred tangent generation using a normal-map material.
They verify repeat queries, reset, invalid IDs, argument validation, retained
receivers and exceptions without replaying mutations. Baseline checks retain
the current direct-load empty-primvars result and the pointer-width difference
in the float-array JSON type label; these limitations are not backend parity
passes.

Combined bone-texture tests cover every influence-rounding threshold, RGBA
packing, padding, vertex offsets, positive-weight filtering and the existing
cap-before-sort order. Both pointer-width baselines pass; memory64's former
size_t-to-Emval dimension bug is recorded separately, and migrated tests require
numeric dimensions. Raw C results retain both arrays across another generation
and loader reset. Adapter checks cover invalid records/arguments, cleanup after
invalid spans, exceptions without replay and retained receiver ownership.

The complete mesh accessor snapshot (`combined-mesh-queries.json`) compares
owned/view outputs and normalized pointer descriptors on both WASM widths,
including subset expansion, skinning, UV slots, display colors/opacity and
packed tangent ordering. Its sources use eager tangent computation: the
separate deferred probe with `c-core-mesh-queries.usda` revealed quantized normals
being reinterpreted as float3 in `ComputeDeferredTangents()` and the legacy
Vec3 tangent decoder, with potentially mismatched attribute counts. That
unsafe path is an open issue requiring dedicated native/WASM regressions;
the snapshot is not evidence that it is fixed.

`tydra_deferred_packed_normals_test` now closes the reproduced deferred-normal
issue described above. It covers Float3/SNorm8/SNorm16/1010102 input under
Lengyel, MikkTSpace, FastMikkTSpace and Hybrid computation, a mixed-indexing UV
seam, equal-count remapping, indexed normals and rejected short/invalid buffers.
The web fixture repeats deferred load/compute/reset and compares copied,
borrowed and descriptor tangent payloads against eager generation. It also
checks that reset and meshes without tangents cannot expose stale cached data.
The eager baseline snapshots remain unchanged and continue to check unrelated
mesh payload behavior.

The typed `getMeshPtr()` boundary runs against the existing complete mesh
snapshots and repeated deferred-normal fixture. Record tests cover metadata,
attribute and submesh validation; end-of-list reads; independent `uv0`/UV-slot
descriptor objects; invalid span cleanup; exceptions without replay; and loader
retention when the original receiver is deleted during reads. Ordinary spans
remain borrowed; tangents remain owned JS arrays.

The mesh value boundary tests verify borrowed `getMesh()` arrays versus owned
`getMeshCopy()` arrays, signed joint indices, double bind matrices, once-only
warnings across reset/clones and exceptions, result cleanup and retained copies
when the original receiver is deleted during reads. The complete mesh snapshot
is still the pre-migration baseline; its memory64 UV-count expectations apply
an explicit correction for the old size_t-to-Emval BigInt bug. This does not
claim new area-light loading support or close general backend parity.

The combined material-query snapshot (`combined-material-queries.json`) stores
pre-migration JSON/XML byte hashes and complete legacy property payloads for
both WASM widths. Fixtures cover textured PreviewSurface, specular workflow,
MaterialX configuration with Unicode strings and OpenPBR's missing-PreviewSurface
error. Tests also cover empty-format legacy output, error precedence, owned
arrays after reset, invalid records/arguments, all thirteen texture slots via
injected records, exceptions without replay and loader retention during reads.
These checks establish boundary compatibility, not new material backend support.

Combined schema utilities use the pre-migration per-width
`combined-schema-utilities.json` baseline for exact scene-export/physics-JSON
hashes and diagnostics. It exercises sample creation, malformed URDF, visual
and collision mesh uploads, copied input lifetime and registry clearing.
Boundary checks cover heap-backed inputs, failed replacement preserving the
previous mesh, invalid pointer/count/alignment combinations, empty-name
diagnostic precedence, stale receivers and exceptions without replay. These
are combined-loader checks; next-only schema authoring parity is a separate gate.

`combined-image-encoding.json` pins pre-migration encoded-byte hashes and
diagnostics for PNG/BMP/TIFF/DNG/EXR with one through four channels on both
WASM widths. It includes unsupported/case-sensitive formats, invalid
dimensions, short pixel buffers and extra bytes. The typed encoding tests
also validate C records, JS arguments, borrowed-output aliasing and bounds,
exceptions without replay and receiver retention. Explicit copies survive
reset; returned heap views retain the existing borrowed-buffer lifetime.

`next_test_render_session_c` covers the render C configuration's extended
controls: unchanged v4 layout/defaults, fan triangulation, each tangent method,
animation suppression, geometry release, instancer arrays required for draw
expansion, persistent resolver lifetime and rejected invalid control values.
`tydra-next-c-api-smoke` exercises the `tydra_to_renderscene --next` facade path
with animation enabled/disabled and a mesh triangulation/tangent configuration;
it checks both exit status and scene counts. The command's legacy conversion
path remains separate.

`lusdview_incremental_scene_update_test` now builds without linking next core
and exercises the public C change-record planner interface. It retains the
slot/remap/deformation/material/texture cases and checks short records, null
path/property spans, unsupported flags, stale revisions, full resync, and
counted property names whose trailing bytes must not affect classification.
The `lusdview-incremental-*` Vulkan/OpenGL tests cover the viewer's temporary
adapter from native next changes to those borrowed C records during real
topology, layer-reload and texture updates.

`next_test_document_session_c` also creates a temporary shader layer in its
build directory, reloads property/stage-metadata edits, and verifies the full
caller-owned snapshot change export. It checks required counts, flags,
revisions, property names, short-buffer atomicity, invalid arguments, empty
changes and snapshot-backed string lifetime after later publications and
session destruction. `next_workflow_examples` verifies the C++ facade exposes
the variant-edited `/World.level` property in the interactive-session example.

The same C document test covers uncomposed loading, source-layer-only cache
retention and memory estimates using temporary root/reference layers. It checks
that trimming reduces transient bytes while keeping source layers, rejects
invalid controls/outputs, and verifies stage transfer is blocked by retained
snapshots. After transfer, snapshot access fails until reopen; transferred data
remains valid and independently editable after reopen and session destruction.
The interactive workflow also exercises the C++ memory query and stage transfer.

`next_test_document_session_c` also tests batch variant replacement and payload
loading on two sibling prims. It verifies single-revision publication, omitted
override removal, empty batches, malformed paths, duplicate selections, count
overflow, retained snapshots, and cancellation rollback followed by rebuild.
Loading both sibling payloads exercises pseudo-root cache invalidation with
previously retained composition data. `next_workflow_examples` uses the C++ batch
facade for its variant edit and deferred payload loads.

Document-session coverage also checks public state/dependency queries and cache
controls: bounded counted UTF-8 copies, invalid indices, retained dependency
ordering after release, zero source-cache accounting, unchanged snapshot/revision,
restored source layers on rebuild, preserved variant/payload rules, repeated
release and BUSY for cache mutation from a progress callback. The interactive
workflow exercises these operations through the installed C++ facade.

The render C test also checks public capacity/texture-budget planning, fixed-width
record sizes, all texture-fit policies and quality tiers, suffix parsing,
overflow/invalid-input rejection, and unchanged outputs on failure. The existing
`lusdview-texture-fit` Vulkan test exercises the viewer's public C/C++ budget
path and validates its resize/compression choices, including explicit policies,
capacity-dependent thresholds and full-fidelity behavior.

Document-session tests cover immutable snapshot-backed C stage handles through
the existing stage API: read-only detection, rejection of root and prim/property
authoring, independent editable flattening, USDC export/reload, retained prim
handles after edits and session destruction, and BUSY on destructive transfer
until views are released. Direct and session render conversion accept the views;
scene data survives release of the view owner. The interactive C++ workflow also
queries a snapshot via ordinary Stage/Prim wrappers.

Preview callback tests use a composed mesh with extent and a non-spatial property.
They verify both phases and flags, retained read-only stage handles, absence of
the non-spatial property in the spatial preview, cancellation without publication,
root reload, disabled delivery, authored-only previews for uncomposed loads, and
stage lifetime after the session is destroyed. The interactive workflow exercises
registration and retention through the C++ facade.

Geometry-release coverage verifies per-prim release with retained snapshots,
thresholds, repeated and whole-stage release, payload/element counts, omitted
per-prim byte scans, preserved animation/non-geometry data, unchanged revision,
cache retirement followed by reconstruction, and invalid/closed/uncomposed/BUSY
requests. Native tests explicitly pass a prim from a retained snapshot through
the legacy per-prim entry point to catch stale-layer use after copy-on-write.

`lusdview_vchar_control_map_test` exercises the public-C facial-control reader:
depth-first first-match selection, string/token names, short optional arrays,
inverted ranges, malformed numeric types, empty data and retained snapshot views.
It also checks owned dictionary string-list lifetime and error handling. The
portable C++ API test covers DictionaryView's string-array copy operation without
requiring the viewer build.

`lusdview_preview_cache_test` uses public document snapshots and Stage handles to
verify USDC extent roundtrips, lifetime after document/snapshot destruction,
retained cache hits after replacement, corrupt and wrong-format payload misses,
and failed writes without manifest publication. It also covers fingerprint,
dependency and malformed-manifest invalidation. Run both migrated leaf tests with:

```sh
ctest --test-dir build_ninja -R '^lusdview_(preview_cache|vchar_control_map)_test$' --output-on-failure
```

For a live composition-preview cache check, run the interactive viewer twice
against the same reference scene with authored mesh extents and an initially
empty cache directory. Use `--next --backend vk --no-threaded
--large-scene-profile balanced --preview-cache auto --preview-cache-dir <dir>
--timing`, under the documented Xvfb/offload environment when needed. Verify
`preview cache: stored` on the cold run, then `preview cache: hit` and
`composition preview uploaded` on the warm run. Headless/fixed-frame runs and
the default threaded interactive Vulkan path bypass this progressive cache path.

The document C regression also checks payload policy/notification callbacks:
selective composition against both default load policies, replacement followed by
rebuild, cache release/restoration, root reload, explicit load/unload precedence,
clearing callbacks without stale userdata delivery, retained snapshot stability,
and BUSY on reentrant registration. Callback counters use serial composition in
this fixture; the API requires thread-safe userdata for parallel composition.
The interactive C++ workflow demonstrates an atomic selection counter.

Pre-open document configuration tests cover initial variants on the first
published snapshot, copied input strings, invalid replacement atomicity,
duplicate/count validation, root reload, BUSY while open or reentrant, and
reconfiguration after stage transfer. A referenced-layer fixture containing a
parent path verifies the untrusted default, explicit trusted resolution, trusted
resolution with parent paths disabled, and cache restoration for each policy.

The viewer's public render-session ownership is exercised by the five
`lusdview-incremental-*` GL/Vulkan tests. They require preparation at document
revision 2 and reject commit-failure fallback, in addition to checking GPU updates.
The remaining native-stage query adapter is covered by the viewer bridge tests
for shared stage retention and independent read-only views. Document revisions
now pass directly through the public API, with coverage described below.

`lusdview_document_test` covers the viewer's public document owner and temporary
native query views: initial variants, payload loading, change revisions, cache
retirement, geometry release, root reload, callback cleanup, cancellation and
retained stage lifetime after owner destruction. Run it alongside the GPU
incremental, VRAM-budget and blendshape tests when changing viewer ownership.
The document C test checks owned bulk dependency/deferred lists against indexed
queries and verifies that list storage survives edits and document destruction.

Document render preparation also accepts caller-aggregated changes. Its C test
skips an intermediate document revision, checks that abort leaves the render
revision unchanged, and commits at the final snapshot revision. It covers forced
resync and invalid/overflowing record counts. The viewer uses this public path
directly; the former native document-snapshot bridge has been removed.

For changes to the viewer's retained public Stage owner, include
`lusdview-camera-motion-cpu`, `lusdview-camera-motion-vulkan`, and
`lusdview-camera-motion-raster-gl` alongside incremental, VRAM-budget and
blendshape regressions. These exercise native camera/animation borrowers while
the public handle supplies stage lifetime across rendering and reloads.

`lusdview_public_stage_queries_test` verifies the public metadata/traversal
helpers, including inactive prims, ordering, empty stages, failure preservation
and early stopping. The two incremental-layer-reload GL/Vulkan regressions also
exercise live MCP `stage_info`, `list_prims`, `prim_list`, `query_prims_by_type`
and `search` calls, checking caps and filters before their reload checks.

The incremental-topology regression also checks MCP `prim_get`, `attr_list` and
`attr_get` through the public stage API, including schema fallback visibility and
array type/count summaries. Variant queries preserve the empty enumeration of a
flattened composed snapshot; subsequent topology/material edits still verify
recomposition. `next_test_c_api` covers owned resolved property-name lists,
schema defaults, scalar/string views, array summaries without data buffers,
blocked/missing defaults, invalid handles and list lifetime after stage destruction.

When changing viewer document payload interfaces, run
`lusdview-usdz-deferred-payload` alongside the document unit and incremental
regressions. It checks the exact deferred prim/arc reported through MCP, the
next loader's proxy geometry, and successful deferred USDZ payload loading in
both loaders. The document unit verifies owned path-list contents survive edits
and memory queries return the public C stats layout.

The viewer document unit queries published snapshots using public C handles.
Its geometry-release case retains a prior Stage view, checks that its points
survive while the new publication loses the released default, and covers repeated
release and missing paths. Include VRAM-budget, USDZ deferred-payload and
incremental GL/Vulkan tests when changing loader snapshot or release ownership.

Viewer document callbacks now expose public C events; preview handles are
borrowed until explicitly retained. The document unit retains a preview past
owner destruction and checks callback cleanup and cancellation. For preview
handoff changes, also verify an interactive non-threaded Vulkan load with
`--large-scene-profile balanced --preview-cache refresh`, then `auto`, using an
isolated config/cache directory and a fixture with a composition arc. Require
cold cache storage, warm cache hit and composition-preview upload in the logs.

Viewer document change aggregation is covered by `lusdview_document_test`:
consecutive revision ranges, combined flags, property deduplication, metadata
changes, owned strings and full resync for a revision gap. Run incremental
GL/Vulkan regressions after changing these records to check render preparation
and the GPU planner consume the resulting public C views correctly.

The facial-control unit obtains its retained read-only view through the public
document API and verifies control data after session destruction. The former
private native-snapshot retention adapter has been removed; its symbol should
not appear in shared-library exports.

The GUI scene hierarchy uses public prim handles and retains its own Stage
owner. `next_test_c_api` covers its authored-payload presence query. Include
reload/incremental, deferred-payload and camera/animation runtime tests when
changing GUI Stage ownership; native inspector/deformation borrowers must remain
valid across replacement and recomposition.

`lusdview_public_stage_queries_test` also checks inspector summary formatting
from public default views: scalars, strings/assets, schema fallback, blocked
defaults and arrays without data buffers. Pair it with incremental regressions
and a visible-window selected-prim run when changing inspector property queries.

The C API suite includes `c-core-relationships.usda` with native instances. It
checks reference target remapping, local overrides, empty targets and inherited
lookup after deleting a local relationship opinion, plus owned-list lifetime
and invalid-handle behavior. The inspector consumes these public queries; pair
API checks with incremental/payload regressions and a selected-prim GUI smoke
run when modifying relationship inspection.

For inspector composition metadata, the C unit checks all arc kinds and authored
variant-selection keys (including sets without local definitions). Use a visible
`--no-composition` inspector smoke run to retain authored arcs and variant sets,
and run incremental variant/payload regressions for edits. Material-link display
uses the first unforwarded relationship target, matching its previous helper.

Stage metadata authored-state coverage distinguishes an explicit zero time code
from an absent field and covers empty strings/default values and setter updates.
The viewer public-stage-query unit checks those timing flags plus frame rate and
comment/documentation fields. Include camera/animation and incremental reload
regressions when changing this shared metadata helper.

The public arc-text query is covered for reference, payload, inherit and
specialize encodings, invalid handles/types and out-of-range indices. The GUI
payload panel queries only deferred prim paths; run the deferred USDZ payload
and incremental regressions after changes to this lookup/display path.

For the public-handle deformation/posed-bounds boundary, run blendshape morph,
skin-animation, combined morph-skin and instanced-prototype RT regressions.
The GUI retains the Stage during synchronous evaluation; native borrowing is
private to the loader. Report hardware RT skips separately: normal cold startup
can choose software tracing, as documented in `doc/lusdview.md`.

The bone-frame/morph-coefficient viewer entry points accept public Stage handles.
Use blendshape, skin-animation and combined morph-skin regressions for those
interfaces. Animated mesh-world updates query the public transform API directly;
include CPU/Vulkan/OpenGL camera-motion tests for their matrix and bounds users.

Camera lookup/shutter sampling use public Stage handles and C queries. Run
`lusdview-camera-record-equivalence`, `lusdview-camera-clipping-planes`,
`lusdview-camera-shutter-contract`, `lusdview-camera-stereo-contract`, and the
CPU/Vulkan/OpenGL camera-motion regressions after changing these queries.
Clipping-plane coverage must exercise complete float4 components; scalar
defaults remain unsampled while camera transforms sample the requested time.

For camera-gathering changes, also run
`bash examples/lusdview/tests/run-backplate-display.sh ./build_ninja/lusdview .`
under the documented NVIDIA offload/Xvfb environment. It checks the selected
camera's public BackPlate handoff, including visibility, multiple instances,
Vulkan images and OpenGL draw/resource counts (pixel differences when supported).

The C API backplate test covers applied/missing instances, scalar/vector
fallbacks, midpoint interpolation, authored image/alpha/depth paths, visibility,
stale and wrong-owner prims, invalid time, failure output clearing, and result
lifetime after stage mutation/destruction. Pair it with the backplate-display
regression when modifying the schema/query boundary.

`lightusd_attr_copy_default` coverage checks schema fallbacks, failure output
clearing, owned array lifetime and lazy materialization without stage memory
growth. `lusdview_public_stage_queries_test` pins preview extent precedence,
float/double arrays, short arrays, blocked fallback and unsampled defaults.
For checkpoint-preview changes, run the preview-cache/document/query units and
cold/warm interactive preview probes through full scene presentation. Camera
regressions cover clipping arrays that share this default-copy query.

The public-stage-query unit covers blend-weight source resolution and sampling,
including explicit ancestor bindings, SkelRoot fallback, inactive/nested roots,
clamped endpoints, midpoint interpolation, unequal sample lengths and empty
name fallback. Run noninstanced blendshape, blendshape morph, combined
morph-skin and instanced-prototype deformation regressions to compare live
weights with load-time CPU baking after changes to this helper.

The C API skeleton-sample test checks parent topology, rest matrices, evaluated
animation channels, absent animation, invalid indices, and ownership after
Stage destruction. For posing changes, run the `lusdview-deform-skin-xform`,
`lusdview-deform-skin-animation`, `lusdview-deform-morph-skin`, and
`lusdview-deform-instanced-proto` rendering regressions; they exercise both
load-time baking and live bone rows.

The C API material-binding query test checks inherited all-purpose and
purpose-specific bindings, the preview fallback chain, unbound purposes, and
invalid handles. It also checks surface-terminal lookup and scalar shader-port
value evaluation. Pair it with `lusdview-material-binding-inheritance`,
`lusdview-degraded-material`, `lusdview-displacement-udim`, and
`lusdview-rt-geomsubset-material` when changing viewer material binding or
terminal queries.

Render extraction category/traversal views point into address-stable storage:
composed native-instance proxies may exceed authored `GetPrimCount()`. The
Tydra `TestRenderExtract` growth test checks pointer validity and release. Run
`lusdview-deform-instanced-proto` on Vulkan after extraction storage changes;
the regression now treats allocation aborts and process signals as failures,
not backend skips.

The shared UDIM layer JSON engine accepts an optional selected shader scope.
Default conversion still requires complete plans for the entire layer; selected
conversion requires complete default/time-sampled plans for every selected
shader. The WASM UDIM regression exercises grid/dense isolation and rejects
unknown, duplicate, empty, and incomplete selections against the same C++ core.
Lucia's browser gate checks staged texture loading, atlas pixels, package roots,
and atomic USD/asset undo and cancellation.
Lucia's activity-gate and browser checks cover superseded imports, New Project
while an edit worker is being cancelled, busy undo/redo, failed creation rollback,
and every built-in scene template. Validation tests cover native failure flags,
malformed results, validation superseded by New Project, blocked failed exports,
and warning-only exports. The UDIM browser check rejects Apply when an edited
tile limit is invalid. `npm run test:lucia:performance` measures
unchanged asset snapshot retention and bounded tile streaming; the browser smoke
also reports a 131072-triangle viewport workload. Shared WASM UDIM tests cover
HDR-to-EXR and EXR round-trip thumbnails on combined/next wasm32/memory64, with
16-bit atlas precision preserved. Rebuild next-only WASM with
`LIGHTUSD_WASM_WITH_EXR=ON` to enable both EXR reading and writing.


## Strict lusdchecker regression

Configure `build_ninja` with `LIGHTUSD_BUILD_TOOLS=ON` and
`LIGHTUSD_BUILD_TESTS=ON`. Run `ctest --test-dir build_ninja -R
'lusdchecker|validation_registry' --output-on-failure`. The contracts assert
exact exit status and structured diagnostics for strict/Core profiles, schema
and shader manifests, compiled callbacks, sampled topology, reachable variants,
resource limits, JSON/SARIF failures, and baselines. Generated table freshness
is checked separately. Standalone Debug `next` testing and AOUSD supplemental
checks remain required when changing the shared validators or compositor.

`tests/checker-parity.py` compares exact IDs, severity, sites, and rejection
status against the pinned OpenUSD checker, including generated physics and
package fixtures. Set `USDCHECKER_PATH` and `REQUIRE_USDCHECKER=1` in reference
CI; use `--report /tmp/checker-parity.json` to retain observed and unexercised
error IDs. See [the checker guide](../tools/lusdchecker/README.md) for profile
scope, extension APIs, manifest formats, and coverage limits.

The native checker and `Module.checkUSD` in the next WASM product share their
runner and report implementation. For web changes, also run the
[WASM checker and browser demo regressions](../web/js/docs/regression.md#full-lusdchecker-wasm-validation).
They compare native/WASM reports and exercise both existing web applications.
