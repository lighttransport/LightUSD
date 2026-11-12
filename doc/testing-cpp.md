# C++ Testing Guide

How to build, run, and extend the LightUSD C++ test suite.

## Overview

The C++ test infrastructure is split into four layers:

1. `ctest`-registered tests for parser coverage, roundtrip coverage, feature tests, and the main Acutest unit suite.
2. A large Acutest executable, `unit-test-lightusd`, which aggregates the unit coverage from `tests/unit/unit-main.cc`.
3. Standalone/manual runners under `tests/` for broader corpus checks, especially parser and Tydra conversion sweeps.
4. Fuzzer targets under `tests/fuzzer/` built separately with Meson/libFuzzer.

Core functionality is tested by the parser, reader, writer, composition, and crate-writer coverage. Tydra is covered in both the Acutest suite and the manual `tydra_to_renderscene` corpus runner.

## Explicit runtime configuration

Library-owned code and `lusdrender` do not read process environment variables.
Configuration is supplied per call or per renderer instance; vendored dependency
and driver configuration is separate.

- USDC layer parse/reconstruction diagnostics: `USDLoadOptions::profile_crate`.
- Crate write diagnostics: `USDWriteOptions::profile_crate`,
  `CrateWriter::Options::profile_crate`, or the final `profile_crate` argument
  to `usdc::SaveAsUSDCToMemory`.
- Parallel composition diagnostics:
  `ReferencesCompositionOptions::profile_layer_cache` and
  `PayloadCompositionOptions::profile_layer_cache`.
- Preview compute: `ScenePreviewOptions::compute_options` carries
  `PreviewComputeOptions::{software_only,debug_validate,trace_pipeline}`.
  The same options can be passed directly to `CreatePreviewCompute` and
  `PreviewVolumeRenderer::load`.
- Preview cache: set `ScenePreviewOptions::cache_directory`. The default is
  `.cache/lightusd/scene-preview` relative to the working directory.
- Path variables: `io::ExpandFilePathWithVariables(path, variables)` accepts
  a caller-owned string map (including `HOME` for `~`). `ExpandFilePath`
  expands globs but leaves variable and tilde text literal.
- Temporary directory: `io::GetTempDir(directory)` accepts an explicit path;
  its default is `/tmp` on POSIX or the Windows system temporary directory.

The old crate/flatten profiling and preview software environment controls no
longer alter library calls. Renderer limits and probes are explicit CLI flags
listed by `lusdrender --help` and in its README. The CLI robustness test checks
invalid limits and verifies inherited renderer environment variables cannot
change a render. The I/O unit test interleaves two independent variable maps;
the preview compute test checks per-instance software selection.

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

Run standalone core tests with threading both off and on after primitive changes.
For memory checks, use an optimized sanitizer build (for example Debug with
`-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`): the large-instance
suite has a wall-clock bound that an unoptimized sanitizer build can exceed.
Keep assertions enabled. Run Python tests against an extension rebuilt from
this checkout, and the web gate against freshly built next and combined modules;
a legacy-only `lightusd.js` lacks APIs required by the combined-module tests.

After TU or include-boundary changes, also build next with
`LIGHTUSD_USE_PCH=OFF` and `LIGHTUSD_TYDRA_USE_PCH=OFF`. The threaded optimized
ASan/UBSan configuration above can serve this purpose. This checks actual
dependencies that a precompiled header can otherwise conceal, including the
session logger, USDA printer metadata/threading types, schema predicates, and
shader texture extraction. Run both `core_tydra` and
`core_urdf_to_usd` when splitting the next render/robot converters.
For GCC sanitizer builds with the default RTTI-disabled libraries, add
`-fno-sanitize=vptr` to the C++ flags: otherwise RTTI-enabled test TUs can emit
references to library typeinfo that is deliberately absent. Address checking
and the other undefined-behavior checks remain enabled.

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

Profiles are `native`, `core`, `web`, `oracle`, `assets`, `gpu`, and `full`.
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

Native `lusdzconvert` builds against the core with
`LIGHTUSD_BUILD_TOOLS=ON`; use `build_ninja/` (see
[converter build instructions](../tools/lusdzconvert/README.md#build)).
With tests enabled, `core_lusdzconvert_regression` checks JPEG asset remapping,
directory root discovery, both USDA/USDC package roots,
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

> **Scope:** The experimental `core` module (`src/`, `lightusd_core`) and
> its tests under `tests/core/` are **not** part of this regression suite. They
> are a standalone CMake project, are not built by the main `build/` (so they do
> not appear in `ctest`), and are not run by the Pixar comparison runner. Do not
> treat `core` results as part of the regression gate. See
> [Experimental `core` library tests](#experimental-next-library-tests) for how
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

# 2. Run the stable core module tests. Keep this Debug: tests use assert().
cmake -S src -B build-core -DLIGHTUSD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j16
ctest --test-dir build-core --output-on-failure

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
failure does not hide later failures, and exits nonzero for unexpected USDA,
USDC, or written-crate checker failures. Declaration-only relationships have no
target list-op; the comparator ignores their unused qualifiers while preserving
the distinction between declarations and authored `None`/`[]` target opinions.

### pxr crate fidelity (does OpenUSD read our crates as the source layer?)

`run-usdcat-compare.sh` compares our *reader* against pxr's (both tools read the
same input) and asks `usdchecker` whether our crates validate like the source.
A third failure mode is left uncovered by both: a crate our own reader accepts
but pxr decodes differently. `tests/run-pxr-crate-fidelity.sh` covers it — for
each fixture it writes our crate with `lusdcat`, then has *pxr's* `usdcat` print
both the source and our crate and diffs the two prints:

```bash
# tests/usda/*.usda by default; pass a pattern/binding for another corpus
LUSDCAT_PATH=./build/lusdcat USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-pxr-crate-fidelity.sh

LUSDCAT_PATH=./build/lusdcat USDCAT_PATH=ref/dist/bin/usdcat \
  bash tests/run-pxr-crate-fidelity.sh 'tests/usdc/*.usdc'

SHOW=40 bash tests/run-pxr-crate-fidelity.sh tests/usda/variantSet-003.usda
```

It reports byte-identical / text-only / semantic counts and exits nonzero on a
semantic difference. Textual differences (spec field and property ordering) are
re-classified through `tests/compare-usda.js` so they do not mask data loss.
Fixtures `lusdcat` cannot write standalone are skipped and remain covered by
`run-usdcat-compare.sh`'s checker pass; a source pxr cannot parse is skipped
too.

This is the check that found variant specs without a `properties` field,
integer-compressed `string[]`/`token[]`/`asset[]` index arrays, `= None`
relationships and payloads written as `ValueBlock`, the `relocates` /
`layerRelocates` crate field rename, and the shader/material node types that had
no writer (`MtlxAutodeskStandardSurface`, `OpenPBRSurface`, MaterialX
`outputs:mtlx:surface`). Each of those was invisible to a self-round-trip
because our reader accepts both encodings.

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

For a downloaded USD-WG assets checkout, also export
`USD_ASSETS_ROOT=/path/to/usd-assets` so external tool tests find it. The native
parser corpus can use CMake's `LIGHTUSD_USD_ASSETS_ROOT` or the parser harness's
`USD_WG_ASSETS_DIR`; those do not set the tool harnesses' environment variable.
The viewer golden, broad-render and animation profiles have additional opt-in
gates and report skips unless explicitly enabled.

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

`lusdview-value-clip-carrier` uses the shared small fixtures in
`tests/feat/value-clip/carrier/`. The CPU `lusdview_core_mesh_adapter_test` checks
the clip points at both times, ownership after stage release, and filesystem
resolver opt-out. The WASM dispatch suite checks the same supplied clip assets
through the lazy mesh path and the clips enable flag; disabled geometry retains
its mesh slot. Run it on both pointer widths after editing shared clip readers.
Vulkan live reload additionally requires `glslc`; `LUSDVIEW_GLSLC` selects its
executable or SDK launcher. Embedded SPIR-V remains sufficient for normal builds.

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
The CPU `lusdview_core_mesh_adapter_test` checks copied bounds, independently
interpolated UV sets, indexed/custom primvars, proxies, and ownership after the
temporary render record and stage are destroyed. Secondary-UV interpolation
uses a formerly reserved byte of the public mesh info, preserving its ABI layout.
`lusdview-core-mesh-materials` compares ordinary meshes with PointInstancer
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
| `bench-render-convert-next` | Same, + Tydra pipeline (only when `LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON`) | `build/bench-render-convert-next` |
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

`core_test_tydra` also checks sampled MaterialX volume coefficients, spatial
sample contexts and named-space transforms, reusable evaluator and texture-cache
behavior, invalid-context rejection, resolver-backed textures, missing assets,
traversal rejection, and mip filtering under a decoded-memory cap. The
shared texture decoder's MaterialX mip test covers odd dimensions, linear-light
conversion, HDR values, decoded-image integration and resident-budget leases.
The CPU-only `mtlx_mip_evaluator_test` checks the volume closure work limit and the
texture reader's decoded-memory limit. Run them headless with
`scripts/xctest.sh ctest --test-dir build_ninja -R '^(core_test_tydra|mtlx_mip_evaluator_test)$' --output-on-failure`.
`tool-lusdrender-materialx-validation` also renders a minified checker with
`--mtlx-mipmaps`, checks the filtered node AOV against its linear mean, and
checks that base pixels and mip levels share the configured texture budget.

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
| `lusdview-core-nonmesh-extraction` | Default-loader Points/Curves records | Vulkan backend |
| `lusdview-gl-nonmesh-render` | GL carrier-render for Points/Curves | No GL context |
| `lusdview-vk-nonmesh-render` | VK carrier-render for Points/Curves | No Vulkan backend |
| `lusdview-dome-orientation` | DomeLight IBL orientation | No GPU |
| `lusdview-light-record-equivalence` | Next/legacy light record parity | `xvfb-run` |
| `lusdview-camera-record-equivalence` | Next/legacy camera record parity | `xvfb-run` |
| `lusdview-camera-clipping-planes` | Authored camera clipping on GL/Vulkan raster | `xvfb-run`, Vulkan backend |
| `lusdview-camera-dof-raster` | Thin-lens response on GL/Vulkan raster | `xvfb-run`, Vulkan backend |
| `lusdview-camera-motion-raster-export` | Native/companion Vulkan shutter accumulation and closed shutter | Vulkan backend |
| `lusdview-camera-motion-raster-static` | Static-scene native/companion camera shutter parity | Vulkan backend |
| `lusdview-camera-motion-raster-gl` | Native/companion OpenGL shutter accumulation and closed shutter | `xvfb-run` |
| `lusdview-camera-motion-vulkan` | Vulkan RT temporal poses retain progressive accumulation | Vulkan RT adapter |
| `lusdview-shadow-alpha-inst` | Alpha-cutout + PointInstancer shadow | No GPU |
| `lusdview_shading_normal_test` | Analytic tangent-frame / normal-map transform | No GPU |
| `lusdview-cpu-normal-map` | CPU normal map shades like the equivalent geometric normal | `xvfb-run` when headless |
| `lusdview-cpu-graph-tangent` | MaterialX tangent/bitangent nodes use the hit triangle frame | `xvfb-run` when headless |
| `lusdview-cpu-uv-transform` | CPU texture UV transforms and `st1` routing match pre-transformed UVs | `xvfb-run` when headless |
| `lusdview-cpu-lane-uv-transform` | CPU metallic/occlusion/clearcoat/emissive lanes apply the per-slot UV transform | `xvfb-run` when headless |
| `lusdview-cpu-material-lanes` | CPU metallic/roughness/emissive/specular/coat/occlusion lanes match constant equivalents | `xvfb-run` when headless |
| `lusdview-cpu-coat-normal` | CPU OpenPBR coat normal uses its own map/frame and affects only the coat lobe | `xvfb-run` when headless |
| `lusdview-cpu-authored-tangent` | CPU normal-map shading uses `primvars:tangents` and its handedness | `xvfb-run` when headless |
| `lusdview-cpu-graph-lanes` | CPU MaterialX metallic/roughness/emission routes affect the render | `xvfb-run` when headless |
| `lusdview-cpu-graph-chain` | CPU MaterialX multi-node chain (image→multiply→mix) equals its constant equivalent | `xvfb-run` when headless |
| `lusdview-cpu-ptex-routing` | CPU PTex material shows the decoded Island face and matches Vulkan RT | `xvfb-run`, Island corpus |
| `lusdview-cpu-ptex-element` | Real quad-topology element pixels match the independent OpenPtex decoder; no fallback | Island corpus, OpenPtex build, `xvfb-run` |
| `lusdview-aousd-conformance` | AOUSD spec render conformance | No Vulkan backend |
| `lusdview-gl-vk-parity` | GL/VK raster image agreement | No GPU |
| `lusdview-raster-shadow-map` | Raster shadow map regression | No GPU |
| `lusdview-raster-multilight` | GL/Vulkan linked finite and directional light parity | `xvfb-run`, Vulkan backend |
| `lusdview-raster-geometry-light` | GL/Vulkan resolved GeometryLight mesh-sample parity | `xvfb-run`, Vulkan backend |
| `lusdview-area-light-vulkan` | Vulkan RT finite-light soft-shadow response | Hardware Vulkan RT |
| `lusdview-area-light-cuda` | CUDA finite-light soft-shadow response | CUDA device |

The `lmetalview` example (native macOS Swift + Metal viewer, `core` product
only) registers its own tests. Configure with
`-DLIGHTUSD_NATIVE_PRODUCT=next -DLIGHTUSD_BUILD_EXAMPLES=ON
-DLIGHTUSD_BUILD_LMETALVIEW=ON`; both are also part of `ctest -R lmetalview`.

| Name | What it tests | Skip condition |
|------|---------------|----------------|
| `lmetalview_core_test` | Preview-scene ABI constants, mesh/material/texture extraction, UsdSkel skinning/blend shapes, retained-stage time scrubbing, in-place node/light/PointInstancer/visibility/skin/morph updates (including animated light intensity/color, instance visibility, prototype switching and count growth), and fallback detection for baked animation channels, shared camera controller, UDIM/Ptex/EXR/KTX2 fixtures | None (compiled unit) |
| `lmetalview-headless-smoke` | Runtime MSL compilation, offscreen render, PNG output | No Metal device (`77`) |
| `lmetalview-headless-{udim,ptex,exr,ktx2,ktx2bc1}` | The corresponding texture path through the Metal shader (texture arrays, Ptex atlas + face ids, EXR/KTX2 decode, block-compressed BC upload) | No Metal device (`77`) |
| `lmetalview-headless-{instanced,pointinstancer,nonmesh}` | Native/point instancing expansion and the point-splat/curve-ribbon pipelines | No Metal device (`77`) |
| `lmetalview-headless-animated-fast` | In-place GPU playback: load a skinned/blend-shape asset at frame 1, update node transforms/palettes/weights to frame 2, render | No Metal device (`77`) |
| `lmetalview-headless-animated-light` | In-place playback of an animated distant-light transform | No Metal device (`77`) |
| `lmetalview-headless-animated-pointinstancer` | In-place playback of animated PointInstancer positions/orientations/scales | No Metal device (`77`) |
| `lmetalview-headless-animated-camera` | In-place playback of an animated authored camera | No Metal device (`77`) |
| `lmetalview-headless-animated-visibility` | In-place playback of time-sampled mesh visibility | No Metal device (`77`) |
| `lmetalview-headless-animated-material` | Animated material inputs force the re-conversion fallback | No Metal device (`77`) |
| `lmetalview-headless-animated-instance-visibility` | In-place playback of PointInstancer `invisibleIds` | No Metal device (`77`) |
| `lmetalview-headless-animated-instance-reveal` | Reveals a PointInstancer instance hidden at load | No Metal device (`77`) |
| `lmetalview-headless-animated-proto-index` | In-place PointInstancer prototype switching | No Metal device (`77`) |
| `lmetalview-headless-animated-instance-count` | In-place PointInstancer instance-count growth/shrink | No Metal device (`77`) |

```bash
# lmetalview only
ctest -R '^lmetalview' --output-on-failure
```

The `ld3d12view` example (native Windows Direct3D 12 viewer, `core` product
only, Windows only) shares `lmetalview_core`, so `lmetalview_core_test` also runs
on Windows. Configure with `-DLIGHTUSD_NATIVE_PRODUCT=next
-DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_BUILD_LD3D12VIEW=ON
-DLIGHTUSD_BUILD_TESTS=ON`. Every `ld3d12view-*` test uses the WARP software
adapter and a hidden window, so none needs a GPU; the input and loading tests
post real window messages (`WM_DROPFILES`, `WM_KEYDOWN`, mouse clicks, `WM_SIZE`)
through the window procedure. Tests marked "log" also check the program's stderr
with a regular expression.

| Name | What it tests |
|------|---------------|
| `ld3d12view-smoke-warp` | Device/swapchain/grid pipeline, HLSL compile, screenshot readback |
| `ld3d12view-{mesh,instanced,shadow,env,points,gaussians,curves}-warp` | Mesh/subset materials, instancing, directional shadow map, dome IBL, point splats, curve ribbons |
| `ld3d12view-{textured,exr,bc1,udim,ptex}-warp` | 8-bit, float EXR, BC1 (KTX2), UDIM texture-array and Ptex-atlas texture paths (UDIM/Ptex log: `uploaded, 0 unsupported`) |
| `ld3d12view-{alpha-modes,transparency}-warp` | Blend-mode sorting and alpha-tested cutouts (including in the shadow map) |
| `ld3d12view-openpbr-*-warp` | OpenPBR extras: coat, glass/transmission, subsurface, brick, metallic, emissive, multi-object |
| `ld3d12view-background-{env,off}-warp` | Dome/gradient background and `--no-background` (env: log `dome environment`) |
| `ld3d12view-animated-{xform,light,camera,visibility,material,pointinstancer,skinned,skeletal}-warp` | In-place playback paths and the re-conversion fallback (material), including GPU skinning and blend shapes |
| `ld3d12view-playback-warp` | The wall-clock playback pump running past the end of a clip |
| `ld3d12view-space-{pause,play,static}-warp` | Space toggles playback through a synthetic key press (log: `paused at` / `playing at` / `no animation`) |
| `ld3d12view-hud-{play-click,timeline-click,resize,hidden}-warp` | HUD button and timeline clicks, window resize (swapchain + HUD texture), `--no-hud` |
| `ld3d12view-load-{async,progress,cancel,supersede,missing}-warp` | Worker-thread loading: phases, Esc cancel, a drop superseding a running load, a missing command-line file (expected to fail) |
| `ld3d12view-drop-{open,replace,bad}-warp` | Drag and drop through `WM_DROPFILES`: open into an empty viewer, replace a scene, a bad file keeps the old scene |
| `ld3d12view-ibl-warp` | Metal spheres, roughness 0 to 1, under the HDR dome: the GGX-prefiltered specular environment |
| `ld3d12view-bc7{,-off}-warp` | Load-time BC7 compression of large 8-bit textures (log: `compressed to BC7`) and `--no-compress-textures` |
| `ld3d12view-shadow-cascades-warp` | 3-cascade shadows on a 240 m ground plane with boxes at increasing distance, seen from an authored camera |
| `ld3d12view-pick-{hit,miss,material}-warp` | Click picking through a synthetic left click: prim path, a miss, material lookup (log: `picked /World/...`) |
| `ld3d12view-{outliner,variant-click,variant-cli,outline-select}-warp` | Outliner rows, variant switching by click and by `--variant` (reload with the next variant), selecting a prim row |
| `ld3d12view-camera-{cycle,index}-warp` | `C` stepping through authored cameras and back to the free view; `--camera N` |
| `ld3d12view-bookmark-{save,load}-warp` | Camera bookmarks: store/disturb/recall round trip, then reload from the persisted file (the pair shares a bookmark file under the build directory) |
| `ld3d12view-capture-setup`, `ld3d12view-shot-warp`, `ld3d12view-turntable-{warp,files}` | `P` screenshot, and a headless 8-frame turntable whose files are checked by `tests/check_turntable.cmake` (frame count, non-empty, first and middle frame differ) |
| `ld3d12view-hw-*` (only with `-DLD3D12VIEW_HW_TESTS=ON`) | The same viewer on the real GPU; fails if the adapter is WARP |

Every `ld3d12view-*` test runs with `LD3D12VIEW_DEBUG=1` (the D3D12 debug layer,
when Graphics Tools is installed) and also fails on any `D3D12 ERROR` line.

```bat
:: ld3d12view only
ctest --test-dir build -R "^ld3d12view" --output-on-failure
```

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

## Native Metal viewer regression (macOS)

With the GUI viewer and tests enabled, build `lusdview_metal_renderer_test` and
run `ctest --test-dir build_ninja -R '^lusdview-metal-'
--output-on-failure`. This windowless GPU test checks depth, transforms,
instancing, visibility, sampled textures and streaming bounds, eviction,
clipping, resize and ImGui composition with a dynamic font atlas. The Python integration test also verifies default/explicit backend selection, USD
loading, capability reporting and scene/UI captures. No Metal device
returns skip code 77; shader or pixel failures fail. Run with GPU access when a
sandbox hides Metal devices. See [Native Metal on macOS](lusdview.md#native-metal-on-macos)
for configuration, CLI screenshots and current feature limits.

## Regression Test Procedure

Local edits can require composition even when a layer has no external arcs.
The public-loader tests check sparse arrays, relationship/connection list ops,
explicit arc clears, relocates and inactive-subtree pruning. They also verify
that ordinary explicit values/targets retain the original prim storage. The
memory pipeline checks local array edits through both composition engines:

```sh
ctest --test-dir build_ninja \
  -R '^core_(pcp|memory_flatten)$' --output-on-failure
```

The strict OpenUSD flatten runner additionally compares the complete USDA,
USDC and optional supplemental composition corpora without relaxing list-op
semantics or numeric tolerances:

```sh
PXR_WORK_THREAD_LIMIT=1 node tests/core/run-core-pxr-flatten-diff.mjs \
  --lusdcat build_ninja/lusdcat \
  --usdcat ref/dist/bin/usdcat --jobs 4 \
  --suite-root /path/to/aousd/releases/1.0.1
```

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
node tests/core/test-compare-usda-booleans.cjs
```

The writer test covers typed bindings and collections on Model, Scope, and
Xform through USDA and USDC, including membership-expression metadata, blocked
values, and declarations without values. The comparison test accepts equivalent
boolean spellings only on matching `bool` declarations, while preserving real
value differences. It also runs in the standalone `core` CTest suite.

Explicit empty list-op regressions can be run with:

```sh
./build_ninja/unit-test-lightusd usdc_reader_prim_metadata_roundtrip_test
node tests/core/test-compare-usda-listops.cjs
```

AOUSD Core 1.0.1 makes `None` and `[]` equivalent for list ops. Both author an
explicit empty opinion without a list-op qualifier; a bare relationship authors
no target opinion. The native test checks both spellings through USDA → USDC →
USDA and keeps bare declarations and populated targets distinct. The legacy
writer uses `None` as its canonical empty spelling. The comparator accepts both
spellings for relationship targets and known list-op metadata, preserves
list-op qualifiers and multiple edits on one relationship, and does not equate
attribute value blocks with empty arrays. The comparator test is also registered
as `next_compare_usda_listops` in the standalone `core` suite.

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
- Tydra `lightusd::tydra::RenderSceneConverter::Convert` (when built with
  `PERFRC_ENABLE_CORE`: the `bench-render-convert-next` target, or the
  `bench_tydra_render` target in the standalone `src` tree).

It generates a deterministic synthetic multi-mesh scene (`--prims N`, mixed
sizes; ~1.5M tris at the default 2048) or loads `--scene <file>`, runs
`--iters` conversions and prints the median wall time plus a stable FNV-1a
scene checksum. The checksum is the byte-identity gate: serial vs parallel
runs of the same build must print the same hash.

```bash
# A/B: parallel vs serial conversion (hashes must match)
./build/bench-render-convert --prims 2048 --legacy-threads 0 --json
./build/bench-render-convert --prims 2048 --legacy-threads 1 --json

# Tydra with explicit worker count
./build/bench-render-convert-next --prims 2048 --threads 8
```

Report-only: never fails on timings. Threading follows the repo-wide
`LIGHTUSD_ENABLE_THREAD` / `LIGHTUSD_ENABLE_THREAD` CMake options. The
**legacy** `LIGHTUSD_ENABLE_THREAD` stays opt-in (default OFF = fully serial);
the **core**'s `LIGHTUSD_ENABLE_THREAD` now defaults **ON for native
builds** (WASM/Emscripten stays OFF, and `web/CMakeLists.txt` forces it off) —
that switch enables the next parallel paths (source warm, arc-layer prefetch,
parallel opinion fill, parallel crate-writer build). Pass
`-DLIGHTUSD_ENABLE_THREAD=OFF` to force the serial core path; without
either switch the tool still runs and reports, just single-threaded.

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

### Core library tests

CI builds the default `core` product with the MCP server enabled and with
ASan/UBSan. The C ABI test stays compiled as C11 and links through the C++
driver so Clang supplies the C++ UBSan runtime used by the shared library.
The legacy product with `LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON` and examples
also needs a configure check: the unified `lusdcat` target must be created
only once. Keep compact writer-layout assertions valid on both 32-bit and
64-bit targets.

`core` (`src/`, library `lightusd_core`) has a standalone regression suite.
Run it explicitly when changing shared code or the core product:

- It is a **standalone CMake project** (`src/CMakeLists.txt` with its own
  `project()`). Its tests do not appear in the default legacy `ctest` tree.
  The root build can also select it with `LIGHTUSD_NATIVE_PRODUCT=next`; use a
  separate Ninja build tree for each product.
- Its tests are gated behind `LIGHTUSD_BUILD_TESTS` (**OFF by default**).
- These checks are a required regression gate in addition to the default
  native suite; passing one does not replace the other.

The standalone suite also checks the shared C11 array grammar and its C++
adapter: bounded/truncated input, element/scalar limits, eager versus retained
source arrays, integer boundaries, and all 65,536 binary16 encodings. TypeInfo
tests check stable pointers and field parity with the C table. On GCC/Clang,
`next_c11_syntax` compiles every first-party C core source (including generated
schema data) with `-std=c11 -Wall -Wextra -Wpedantic -Werror`.

`next_c11_storage` checks core ABI layout revision 2 and the reported value
size, alignment, storage offset, type-descriptor size and chunk size against
the consumer headers. The C API test also checks that `lightusd_abi_get()`
forwards the same core layout revision and value dimensions. Consumers must
reject a mismatched core major/revision or target layout before passing value
storage across a shared-library boundary.

`core_int64_listop` checks signed 64-bit list-op metadata through USDA,
USDC, composition and PointInstancer masking. It covers all six authored runs,
empty versus explicit clear, exact IDs beyond 2^53, malformed/count-limited
payloads, three-layer edits, and deterministic comparisons with the string
list-op model. A 9,001-prim case checks serial/parallel staged and streaming
writer parity. Retained operation storage is included in memory estimates.
Nested list-op values outside metadata are not covered by this codec.
The CPU preview test checks inactiveIds visibility and rejection of preview
cache version 3; current preview and source geometry versions are 4 and 2.

`core_stream_residency` compares staged/streamed bytes for owned and
observable mapped sources around the 32 MiB digest threshold. It checks serial
and parallel encoders (9,001 prims), source-range and cross-source content
deduplication, page advice before writing, bounded sink calls, lazy-value reuse
and a sink failure after the first window. It also checks that ordinary
zero-budget writes preserve their non-windowed path. Real SSD measurements are also
required: source advice changes read faults and cannot be assessed from
`/dev/null` timings alone. Use guards and check both low and regular profiles.

The PCP test checks low-profile discovery at frontier caps 0, 1, 2, 3, 4, 5,
8 and SIZE_MAX.
Once expanded children plus one entry per unvisited root cannot fit, discovery
must retain the previous whole frontier without loading later references.
The later references and all children must still appear in the final composed
stage. Run the fixture composition sweep and low-profile batch targets 1, 400,
and 50 with frontier 64 when changing this pruning rule.

The PCP writer lifetime regression uses eight >8,192-prim batches with short
and long paths, exceeding a packed path-storage block in each batch. Both
serial and parallel low-memory output must match regular staged output after
the source batches have been released. Property paths and external target
paths exercise both borrowed and separately owned path text. Run composition,
writer rewrite and real-output hash gates when changing this ownership.


For C-core size measurements, configure a Ninja tree with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, then use the shared measurement harness:

```bash
scripts/bench-core-port.sh --build-dir build_ninja/core-release --repeat 3
scripts/bench-core-port.sh --build-dir web/build_ninja --wasm --repeat 3
```

The wrapper discovers transitive objects and linked artifacts from Ninja's
actual target commands. It writes `core-port.json` in that build directory,
including source/cache fingerprints, compiler versions, clean-build timing,
object sections, and raw/gzip WASM sizes. Use `--output` for a different report,
`--target` for another target, `--size-tool` to override section inspection, and
`--artifact` to include an additional linked file. Runs clean the selected
target before rebuilding; preserve baseline artifacts elsewhere first. Run
before/after timing passes without competing builds, and keep compiler, flags,
features, SIMD, memory width, and threading identical. Compare repeated load
and export timings as well as size; investigate repeatable regressions above
5% rather than accepting a smaller binary on size alone.

> **Moved.** The Lean model (`verif/lean`), its proofs, the findings registry, the
> citation/model-split/reflection gates, the Unicode gate and the replays of
> Lean vectors against stock OpenUSD (`next_lean_build`, `next_lean_*_vectors`,
> `next_lean_reflection_*`, `next_lean_axioms`, `next_lean_unicode_conformance`, ...)
> now live in the separate `usdspec-conformance` repository. Only the C++
> consumers (`core_lean_vectors*`, `core_name_order`) remain here, reading
> the pinned copy in `tests/core/generated/lean/`. Paths and test names in the
> Lean-specific text below that refer to `verif/lean`, `scripts/*lean*` or
> `next_lean_*` describe that repository.

The Lean Unicode gate requires a verified official UCD cache. Before running
Lean-labelled CTest checks, run `python3 scripts/fetch-lean-unicode-data.py` from
the repository root. This fetches only missing files and verifies every SHA256
against `verif/lean/data/unicode-15.1.0.json`; corrupted cached files fail.
`next_lean_unicode_conformance` checks independent Unicode 15.1 generation,
native XID parity and four negative trust controls, then runs Lean NFC/NFD on
all 19,074 official test rows and 1,095,035 scalars omitted from Part1. It performs
2,380,810 checks. The generators do not use host `unicodedata` or copy native
ranges as their oracle. CI caches and verifies the same version-pinned files.
This does not replace the open general NFC idempotence proof, and does not
normalize authored USD identifiers.

The spline proofs certify the rational Keep Ratio transform for arbitrary fuel
and connect bisection to the unique real time root. The value-error bound is
`K/2^(n+1)` for an explicit control-point constant `K`; fuel 64 gives `K/2^65`.
The compiled axiom gate covers the new SplineBounds module and the native vector
consumer continues to check the existing exact spline cases. The proof covers
the rational model; floating-point rounding in native C++ is separate. See
[the proof audit](validation/lean-spline-bounds-audit.md) for assumptions and code
linkage. Run the focused legacy spline tests when changing shared spline code.

`next_lean_primindex_vectors` consumes the Lean-generated
`prim-index-vectors.json` through the production PCP JSON CLI. Its 256 cases
cover four arc fields, stronger/weaker list edits, global payload loading and
ancestral children. It checks ordered prim and marker-property stacks at 512
paths, including child absence after a clear or deferral. Run the script with
`--oracle-python` and `--oracle-pythonpath` to require pinned stock parity;
the Lean CI job does so. The generator drift gate covers this eighteenth
generated artifact. See [the model scope audit](validation/lean-prim-index-audit.md).

`next_lean_layerstack_vectors` checks 94 Lean-generated sublayer graphs, including
cycles, missing files, shared diamonds and a 25-layer chain. Layer order and
complete prim/property stacks must match; cycle/missing diagnostics must be
present. CI also checks pinned stock. The model's sublayer traversal terminates
by a finite-set measure rather than a fuel cutoff. The C++ API regression checks
the ABI-preserving diagnostic overload of Cache::Open, warning append semantics,
typed unreadable-sublayer issues, permissive surviving content and strict failure.
Initial sublayer messages reach the CLI, session and composition helper outputs.

#### Lean-generated conformance vectors

`verif/lean` formalizes the AOUSD Core Spec v1.1.0 in Lean 4 + Mathlib and
emits X-macro tables into `tests/core/generated/lean/` (committed). The
`core_lean_vectors` ctest replays them against `src` and never
skips; `core_lean_vectors_combine_chain` replays the corpus list-op
chains. When `lake` is on `PATH` or in `~/.elan/bin`, `next_lean_build`,
`next_lean_test` and `next_lean_vectors_check` also run (the first build
needs `lake exe cache get` for Mathlib). `next_lean_reflection_table`
checks validator `lean_ref` links. `next_lean_axioms` audits all compiled theorem
dependencies, including private helpers, allowing only `propext`,
`Classical.choice`, and `Quot.sound`. `next_lean_axiom_audit_tests` verifies
acceptance of imported kernel proofs and rejection of explicit axioms, `sorry`,
and native proof tactics. Both are also required by the Lean CI workflow.
This trust check does not establish completeness of the specification models.
Path-order proofs include a general decimal-tokenizer left inverse and joint
key-encoding injectivity, so comparator equality implies actual string equality
for arbitrary inputs. Leading zeros and digit-run lengths are unbounded. The
model's trichotomy and antisymmetry do not establish OpenUSD byte-order parity;
that distinct production comparator retains its independent stock tests.
PATHS proofs cover arbitrary decoded-row trees, all four jump forms, arbitrary
prefix/suffix framing, and exact preservation of sparse path-index sequences.
Encoded row count is sufficient decoder fuel; chains prove this bound tight for
every length. This is a theorem about the decoded-row model, separate from native
binary bounds checks and validation of total table size or duplicate indices.
The supported USDA AST printer uses a quoted-string codec with a general inverse
proof for arbitrary strings and trailing input. Forty Lean-generated vectors
cover every ASCII control, quotes, backslashes, Unicode and NUL followed by a
digit. Native consumers compare values after parsing and USDA/USDC transport;
`test-usda-strings.py` independently checks emitted C++ literal values and pinned
stock semantics and runs in Lean CI. USDC's NUL-terminated token table cannot
represent embedded NUL: native writing rejects it explicitly, including through
parallel field workers. The stock 26.11 oracle's two NUL truncations are classified
and retained. The supported-AST round trip is now proved by `parseLayer_print`;
hexadecimal and one/two-digit octal source escapes remain outside this model.
Subset lexical proofs separately cover identifiers, numeric lexemes and
delimiter-framed asset/path values with explicit boundary conditions. General
list proofs establish the required fuel and are instantiated for printed
path-reference lists. These are compositional proof obligations, not a claim
of complete normative lexical or semantic validation.
Offset/reference/sublayer proofs cover the four offset states, asset-only,
path-only and combined references, arbitrary list lengths and following input.
They establish local parser fuel and whitespace behavior. Their explicit
lexical predicates do not assert finite values, nonzero scales or valid URIs
and object paths; semantic validation remains separate from AST recovery.
Sample-map proofs preserve arbitrary-length lists and authored pair order with
explicit numeric-lexeme conditions. Attribute proofs use the actual `attrRest`
and `primItems` functions, cover default and sampled statements, and establish
sufficient fuel for unbounded attribute-only bodies. They do not assert finite,
sorted or unique timestamps or value-resolution semantics.
Recursive metadata, nested prims, top-level forests and Layer assembly now have
general inverses with sufficient input-length fuel. Dictionary keys are arbitrary
strings: the model quotes all keys, and `EntriesWellFormed_all` removes the former
identifier-only restriction. The [constructor audit](validation/lean-usda-roundtrip-audit.md)
records all supported AST cases and remaining normative-model limitations.
A seventeenth generated table supplies 135 integer/string/nested dictionary-key
cases. Native consumers check exact keys through USDA/USDC transport, including
explicit crate rejection for embedded NUL. `test-usda-keys.py` checks a separate
45-key manifest and all 135 cases against pinned stock OpenUSD; CI requires it.
Namespace separators in dictionary keys must be quoted; the native writer
regression includes `namespace:key` in every entry kind.
The validator checks layer root `primOrder`, prim `primOrder`, and
`propertyOrder` name syntax. These are recommendations (warnings): speculative
names do not need to exist locally, and property names may be namespaced.
Validation preserves the authored order vectors and does not impose an
additional duplicate-name restriction. Programmatic tests cover malformed,
empty, Unicode and namespaced tokens and group toggling. These syntax checks
are separate from the Lean population-order algorithms.
Layer-level `relocates` now receives the same identity/path/duplicate-source
checks as legacy prim metadata, with findings at `<layer>`. Pseudo-root targets
are rejected; root-prim targets and empty deletion targets remain allowed.
Checker CLI contracts also verify these rule IDs, warning/strict exit codes and
layer-level relocate locations. The help banner identifies Core 1.1.0; strict
semantic fixtures use required statement separators.
Authoring component rules now cover root sources/pseudo-root targets,
ancestor/descendant targets, conflicting sources/targets and pre-relocation
nested sources. The nine Lean references name individual predicates, including
separate uniqueness components. OpenUSD cross-entry target/source and
arrival-under-other-source policies use the `compatibility` category and are
excluded by normative-only filtering. Root-source errors remain normative;
root-prim targets remain accepted under the explicit LEAN-515 policy.
Tests cover both layer and prim locations, path-component boundaries, multiple
deletes, already-relocated namespaces and category/profile filtering. Full
composed-stack/error-policy coverage remains open; these checks are local
authoring constraints, not sublayer winner selection or a proof of native composer equivalence.
The composition cache validates each relocate before selecting the strongest
valid source opinion. Stronger deletes suppress weaker targets; invalid stronger
opinions permit valid weaker replacements. Invalid path entries cannot poison a
valid edit through the pairwise target-conflict scan. Layer metadata, including
an authored empty list, supersedes legacy prim edits in that same layer.
The PCP JSON contract checks complete ordered stock stacks for valid strength
and root-relative cases. Invalid-path and legacy-prim text controls retain stock
parse rejection explicitly; native permissive/extension behavior is checked
separately. Programmatic tests cover invalid sources that bypass text parsing.
Relative relocate prim paths share one anchoring helper across parsing,
validation, composition and USDC writing. Layer paths anchor at `/`; legacy
prim paths anchor at the owner, including leading `..`. Strict parsing rejects
above-root paths; permissive parsing preserves them for diagnostics and
composition ignores the entire entry. USDC writing rejects them in serial and
parallel workers. Failed anchoring never substitutes the empty deletion target.
Authoring validation normalizes temporary values without changing metadata.
Tests include canonical root-relative crate roundtrips, nested parallel parser
owners, late parallel writer shards, raw programmatic parent-relative metadata,
and invalid-target composition controls. The stock contract explicitly retains
OpenUSD 26.11's above-root-target conversion to deletion as malformed-input
reference loss, not valid-input parity or a specification defect (Core 1.1.0
L3370–3375 already prohibits anchoring above the root).

The PCP JSON contract also checks an inherited class variant that selects a
root variant. Empty options still contribute authored prim-stack sites: the
class option must precede the dependent root option, just as an option with
inline properties does. Pending inherited option records are emitted before
the host variant, while stronger hosts remain pending during class traversal.
Empty and inline-property options, payloads, sibling inherits and nested
inherits compare complete ordered prim and property stacks with stock.
The source arc-site trail supplies nesting depth, including same-stack class
arcs; pending sibling records are flushed before the next sibling spec.

Subroot reference/payload grafts preserve an inherited source's existing
implied-class strength anchor, unless an enclosing implied block already owns
that anchor. Implied inherits stay ahead of the referenced stack's local
opinions; specializes retain their weaker position. A stock contract compares
the complete property stack, and native value controls check both the implied
inherit winner and the referenced local winner when only specializes override
that value.

Implied subroot inherits whose targets exist only in ancestor variants resolve
that independent ancestor branch in each expressing stack. A root-layer `z`
selection and referenced-layer `y` selection therefore both contribute their
own variant sites, in strength order. Explicit empty and absent root selections
retain the referenced opinion. A shared ancestor of the inheriting prim keeps
the current prim-index context instead, avoiding incompatible variants such as
both `full` and `tidscene`. Tests compare exact property provenance for all four
independent selection controls with stock; the retained corpus covers the shared
ancestor control.

The PCP JSON contract checks implied specializes variant selection across a
reference: direct selections on the specializing prim stay stronger than an
ancestor-stack selection on the specialized class. Explicit empty selections
suppress the variant; absent selections still inherit the class selection. The
four controls compare complete ordered v2 stacks with pinned stock OpenUSD.
Nested-variant controls also check complete selection-chain identity when a
variant-set name recurs deeper in the same chain. Metadata-only outer options,
inline properties, and child content must produce one ordered site per selected
option, without dropping an enclosing site or duplicating a nested one.

The PCP JSON contract includes authored attribute/relationship interleaving in
ordinary prims and inline/content-backed variants, and reads LightUSD-written
crate files through the pinned stock oracle. Native writer regressions cover raw
property order through USDA (`sort_properties=false`), USDC and USDC-to-USDA,
independently of authored `propertyOrder` metadata and sorted lookup slots.
`core_name_order` checks the shared compatibility comparator and separates
stage property ordering from raw PCP property children. The stock comparison
also runs in Lean CI and can be reproduced with:

```bash
python3 tests/core/test-name-order.py \
  --tool build_ninja/next_debug/test_name_order \
  --oracle-python "$ORACLE_PYTHON" --oracle-pythonpath "$ORACLE_SITE_PACKAGES"
```

Use the isolated `usd-core==26.11` wheel for those oracle variables. The script
verifies distribution provenance, compares 80,089 byte/name pairs (including
long numeric runs), and checks default and partial stage `propertyOrder` over
mixed attributes/relationships. These finite checks do not establish general
Lean comparator laws.
The PCP contract also covers arc targets deleted beneath a relocated parent:
internal references/inherits/specializes, external references/payloads and targets
below the deleted subtree must not reintroduce its specs or children. A sibling
at the relocated destination remains a valid target.
The Lean vector executable also includes the eight generated crate list-op wire
vectors. They exercise the shared framing decoder used by path, reference,
payload, token, string and Int64 readers; reject every truncated vector prefix,
reserved bits and over-limit counts; and compare exact production Int64 writer
payload bytes and reader-retained vectors with the Lean expectations. Type-specific
byte/index validation and allocation budgets remain in each production consumer.
Non-canonical header semantics use the separate Lean `pxrRead` compatibility
model: 256 generated mode vectors cover all 128 legal headers with empty and
populated vectors. `tests/core/test-listop-modes.py` also compares 2,048 real crate
fields against stock 26.11 across Int64, token, string, path, reference and payload
list ops, plus relationship targets and attribute connections. Discarded explicit vectors remain validated: malformed indices warn and drop the
field in compatibility mode and reject strict AOUSD loads; memory/count limits
remain hard failures.
Run it with:

```bash
python3 tests/core/test-listop-modes.py \
  --tool build_ninja/next_debug/test_int64_listop \
  --oracle-python "$ORACLE_PYTHON" --oracle-pythonpath "$ORACLE_SITE_PACKAGES"
```

Lean CI requires this stock comparison.

The arc-cycle contract covers cross-file returns to an ancestor of an earlier
site, exact same-file parent cycles, and a noncyclic branch through the same
external file. It asserts that the noncyclic opinions preceding the rejected
arc survive and that no recursive child subtree appears.
Regenerate after changing the model:

```bash
cd verif/lean && lake exe usdspec-gen --out ../../tests/core/generated/lean
```

See `doc/validation/lean-verification-2026-11-06.md` and `verif/lean/README.md`.

#### AOUSD Core supplemental inputs

The official [Core supplemental repository](https://github.com/aousd/core-spec-supplemental-public)
keeps releases under `releases/`, with the license at the checkout root.
Download the pinned public release into an ignored build directory, then point
the standalone Debug suite at the release directory:

```bash
git clone --depth 1 --branch release/1.0.1.post0 \
  https://github.com/aousd/core-spec-supplemental-public.git \
  build_ninja/aousd-supplemental
cmake -S src -B build_ninja/core-debug -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DLIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT="$PWD/build_ninja/aousd-supplemental/releases/1.0.1"
cmake --build build_ninja/core-debug -j16
ctest --test-dir build_ninja/core-debug --output-on-failure
ctest --test-dir build_ninja/core-debug -L aousd -V
```

This release pins commit `c15ae0cad3ed9e07a25dffd6699627d2c166cab0`.
The supplemental bridge loads 54 file-format fixtures, checks all 138
composition cases, and checks resolved samples in eight value-resolution cases.
Three composition roots contain invalid text paths and must be rejected:
`BasicInherits_root`, `ErrorRelocateWithVariantSelection_root`, and
`SubrootReferenceAndVariants_root`. Their classification in
`tests/core/aousd-invalid-text.json` pins each text SHA-256 and diagnostic;
accepted text, changed bytes, a different failure, or an unreadable binary
dependency fails the gate. The release supplies no binary root for these three
cases; all six available original binary dependencies are still required loads.
With the stock oracle enabled, it independently confirms the text rejections
and reads those binaries too. These three rejection cases are reported
separately from the 135 structural comparisons, keeping all 138 cases covered.
An upstream `Errors` list alone does not grant a case an exemption. Run the full
corpus instead of the default 50-case cap with `LIGHTUSD_AOUSD_CAP=0`.
Its data-type bridge runs the library's AOUSD type checks; it does not execute
the upstream Python test runner or compare every PCP field. Keep these coverage
limits separate from full spec conformance. The 2026-11-03 run is recorded in
[the supplemental report](validation/aousd-supplemental-2026-11-03.md).

The `lusdcat --dump-pcp-json -` default remains the corpus-compatible v1 JSON.
Use `--pcp-json-version 2` for ordered prim/property stack arrays of
`{"layer": "...", "path": "..."}` sites. This retains repeated layers and repeated
sites reached through distinct reference offsets. External layer identifiers
retain their relative directory components in v2. `next_pcp_json_contract` tests
both versions, variant provenance and duplicate-site preservation.

Authored Layer root/child index vectors stay in their original order through
finalization. Stage child/root queries and traversal apply ordered views, while
PCP applies each spec's restatement after merging its names with weaker sources.
The namespace-order regression checks raw vectors, `GetChildAt`, traversal,
clone/move and edits. Stock-oracle contract cases exercise per-layer and per-spec
ordering, more than 32 children, USDA/USDC inputs, relocation rename positions,
shadow-name collisions, missing sources, and arrivals before local children.

For an independent lossless comparison, install `usd-core==26.11` into an
isolated directory and run:

```bash
python3 -m pip install --target build_ninja/stock-oracle --no-deps 'usd-core==26.11'
python3 tests/core/test-pcp-json.py --lusdcat build_ninja/core-debug/lusdcat \
  --oracle-python "$(command -v python3)" \
  --oracle-pythonpath build_ninja/stock-oracle
python3 tests/core/run-aousd-supplemental.py \
  --suite-root build_ninja/aousd-supplemental/releases/1.0.1 \
  --lusdcat build_ninja/core-debug/lusdcat --category composition \
  --pcp-oracle-python "$(command -v python3)" \
  --pcp-oracle-pythonpath build_ninja/stock-oracle
```

The oracle verifies the imported package distribution/version and uses stock
Pcp in USD mode. This comparison counts property ordering and all stack sites;
its default difference ceiling is zero and it has no engine allowlist. It is
an active gap-detection gate; current differences are recorded in
[the execution ledger](validation/lean-followup-status.md).

Reference `customData` tests now require strict USDC loads, typed nested-value
retention, qualified list-op identity, and memory/streaming writer roundtrips.
Legacy metadata roundtrip tests also require explicit `None` opinions for
relationships, inherits, and specializes. WASM layer tests cover reference
dictionaries through USDA, USDC, and JSON exports.

Regression coverage to keep in mind when touching `core`:

- `test_tydra.cc` also covers the compiled chunk-storage engine: exact-tail
  compaction repeated under allocation accounting, self-append and COW,
  over-aligned POD elements, move/reuse, allocation-budget refusal, and
  read-only iteration/indexing without detachment. Writes now use
  `mutable_at()` / `mutable_chunk_data()` explicitly.
- `unit-test-lightusd` includes `minijson_shortest_double_roundtrip_test`:
  finite binary64 boundary values and 4096 deterministic bit-pattern samples
  must serialize/reparse exactly (with a separate signed-zero spelling check).
  Run this legacy unit target as well as standalone core tests after changing
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
- `test_tydra.cc` — the animation-extraction gate
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
scripts/run-core-checks.sh
```

Useful environment overrides:

```bash
scripts/run-core-checks.sh --help
BUILD_DIR=build-core-tsan BUILD_TYPE=Debug THREADS=ON scripts/run-core-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release scripts/run-core-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release BENCH_LAZY_VERTS=4000000 BENCH_LAZY_CLONES=32 scripts/run-core-checks.sh
```

The equivalent manual commands are:

```bash
# Configure the standalone next project with its tests enabled.
cmake -S src -B build-core \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_ENABLE_THREAD=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j16
ctest --test-dir build-core --output-on-failure
```

> **Caveat — use a Debug build.** The `tests/core/` programs validate via bare
> `assert()`. Under `NDEBUG` (i.e. `Release`/`RelWithDebInfo`) `assert(expr)`
> does not evaluate `expr`, so these tests check nothing — and any side effect
> placed inside an `assert()` is silently dropped (this previously caused a
> null-dereference SIGSEGV in `test_usdc_roundtrip` when the `toc`-populating
> `ParseUSDCBinary()` call was compiled out). Build with `-DCMAKE_BUILD_TYPE=Debug`
> for meaningful validation, and keep side-effecting calls out of `assert()`.

Current USDC-focused next coverage:

- `core_usdc_malformed` uses generated in-memory crate fixtures for bad
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
- `core_crash_regressions` replays minimized fuzzer-found binary inputs
  from `tests/core/crash_regressions/`. Prefer this only when the exact byte
  sequence matters.
- `core_usdc_roundtrip` includes focused roundtrip cases plus a dense
  generated fixture covering layer metadata, dictionaries, composition arcs,
  mesh arrays, per-property metadata, relationships, connection-flagged
  properties, Shader/Material links, PointInstancer arrays/prototypes, ids, and
  time samples. It also checks half/quaternion arrays with distinct component
  values in both eager and lazy readers, plus empty and full-byte-range
  uchar/bool arrays with non-aligned tails. These cover overlap-safe half
  widening and bounded source views used by eager array decoding. Reference
  and payload offsets/scales retain binary64 precision through two crate cycles,
  including tiny scales that must not round to zero.
- `core_lazy_array` checks that separate materializations share a cumulative
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

The full oracle gate also needs OpenUSD Python bindings and MaterialX support.
Without MaterialX, shader-registry and `.mtlx` resolver findings differ from the
reference configuration used by checker parity. After preparing the pinned
checkout, build the core, tools, validators, Python and MaterialX without imaging:

```bash
python3 ref/openusd/build_scripts/build_usd.py ref/dist \
  --build ref/openusd-build --generator Ninja --onetbb \
  --python --tools --usdValidation --materialx \
  --no-imaging --no-usdview --no-tests --no-examples --no-tutorials \
  --no-docs --no-python-docs --no-openvdb --no-ptex --no-prman \
  --no-openimageio --no-opencolorio --no-alembic --no-draco -j 16
```

Use an interpreter with development headers; run the sampled-value harness with
the same Python major/minor version as the oracle bindings. Verify `usdcat` on a
known valid fixture before interpreting reference skips in a restricted sandbox.
Both differential corpus runners reject a run with no readable reference input.
Keep reference crashes visible in the report. `PXR_WORK_THREAD_LIMIT=1` can be
used for a deterministic oracle retry without changing comparison tolerances.

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

Core libFuzzer harnesses are built with the canonical CMake project:

```bash
CC=clang CXX=clang++ cmake -S . -B build_ninja/fuzz -G Ninja \
  -DLIGHTUSD_BUILD_FUZZERS=ON
cmake --build build_ninja/fuzz
```

See `tests/fuzzer/README.md` for core harness targets and corpus setup. The
standalone Meson harnesses use the legacy implementation and require explicit
`-Dlegacy=true`; keep those builds compile-only.

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
- The experimental `core` module (`src/`, `tests/core/`) is a standalone CMake project excluded from `build/` `ctest` and the regression gate by design (`LIGHTUSD_BUILD_TESTS=OFF`); its `assert()`-based tests are only meaningful in Debug builds. See [Experimental `core` library tests](#experimental-next-library-tests).

`security_sha256_digest_test` checks empty/null handling, standard ASCII and
binary SHA-256 vectors, and both padding/block boundaries. The asset-cache
fixture in `web/js/tests/core-c-dispatch.test.mjs` compares
against Node's crypto implementation on wasm32 and memory64, where native-only
coverage would miss size_t-width bugs in bit-count encoding.

## Workflow tools and core examples

`core_workflow_examples` generates small inputs and checks composition, typed
queries, USDA/USDC reopening, payload/variant edits after snapshot publication,
cancellation, sampled/spline/skeletal evaluation and portable package relocation.
It is also registered in standalone core builds with examples enabled.
All four C++ workflow examples consume the public ownership facade; the
animation traversal keeps stage-retaining `Prim` objects through its work list.
The interactive workflow exercises the installed-header C++ document/render
session facade over the C boundary, including RAII snapshot ownership and
retained-snapshot prim queries. `core_document_session_c` separately compiles as strict
C11 and checks retained snapshots, revisions, render updates, stale snapshots,
prepare/abort/commit, and cancellation; it is registered when Tydra is enabled.

`workflow_tools` additionally exercises JSON crate limits, bounded dependency
reports and all-authored variants, variant/property provenance, checker baseline
and SARIF output, directory/package diffs, textured GLB export and strict losses.
The GLB checks retain textures from both loose layers and USDZ package entries;
package reads obey the export archive and entry size limit.
When `lusdrender` is built, it runs the two-camera synthetic capture and checks
raw depth, world normals and source IDs numerically. Temporary assets are removed
after the run. `next_gltf_export` tests API budget failures, invalid attribute
indices, malformed corner mappings, non-finite values and cyclic node graphs.
`core_render_session_c` also checks the public GLB export's binary result
and output-limit validation through a strict C11 consumer. The GLB writer is
isolated in its own object file; a render-only static consumer does not link it.
`lusdview_lod_stream_test` covers the C/POD viewer pre-pass with a composed
camera and two transformed districts, checking the generated promotion wrapper.
`core_c_api` loads the instancing fixture in both holder and native modes
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
cmake -S src -B build_ninja/core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_WITH_TYDRA=OFF
cmake --build build_ninja/core
ctest --test-dir build_ninja/core --output-on-failure
cmake -S src -B build_ninja/core-render -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_WITH_TYDRA=ON
cmake --build build_ninja/core-render
ctest --test-dir build_ninja/core-render --output-on-failure
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
src/stage/stage.hh` measures incremental rebuild time and compilation
count, restoring the input timestamp afterward. Run measurements without
concurrent builds or regression workloads.

The core WASM loading, ownership, and async contracts are checked by
`web/js/tests/core-c-dispatch.test.mjs`,
`web/js/tests/core-allocation-failure.test.mjs`, and
`web/js/tests/core-usda-composition.test.mjs`. Core uses C++17 tasks.
Legacy coroutine support is isolated behind `LIGHTUSD_WASM_COROUTINE` in the
optional standalone legacy product. See `web/js/docs/regression.md` for the
module override loader and complete regression procedure.

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

`core_render_session_c` covers the render C configuration's extended
controls: unchanged v4 layout/defaults, fan triangulation, each tangent method,
animation suppression, geometry release, instancer arrays required for draw
expansion, persistent resolver lifetime and rejected invalid control values.
`core_render_scene_c_api_smoke` retains the former facade smoke cases against
`tydra_to_renderscene --core` (compatibility alias for its core default):
animation enabled/disabled and mesh triangulation/tangent configuration, with
exit-status and scene-count checks. The example has no legacy conversion path.

`core_volume_c` covers the UsdVol render C surface: volume/field info
(binding, grid name, value type, components, bounds), dense voxels,
`lightusd_render_volume_field_nanovdb` (grid magic, header grid type, cached
buffer identity), `lightusd_render_volume_field_sample` in world space
(nearest and trilinear, scalar and vec3) and the unsupported bool grid. It
also compares sparse samples with dense voxel values under translated,
rotated and scaled field transforms, covers batches above 256 points, and
checks rejection of a singular field transform. When the optional corpus
contains `utahteapot.vdb`, it checks that sparse world sampling still reaches
interior and exterior values after dense extraction exceeds its voxel budget.
It checks sample count overflow, and has been run as a 32-bit Linux executable
to verify that the 8 GiB request cap does not wrap on a 32-bit `size_t`.
It needs the out-of-tree tinyvdbio reference corpus (`TINYVDBIO_DATA_DIR` or
argv[1], expected under `reference/`) and prints SKIP without it, like the
`usdvol-vdb-corpus` C++ test.
The legacy `usdvol_vdb_corpus_test` also checks that a dense grid rejected by
its voxel limit reports both the requested cell count and the limit. It checks
header and sparse decoder budget failures release partial allocations and
rejects sampling below the retained grid-and-sampler footprint.

On a 32-bit native build, `volumetric-tet-overflow-32` (legacy) or
`core_tet_overflow_32` (next) checks that a lattice whose position buffer
would overflow `size_t` is rejected before allocation or sampling. A standalone
build against a 32-bit tinyvdb checkout can run the same fixture:

```sh
cc -m32 -std=c11 -O2 -I/path/to/tinyvdb/src tests/volumetric/tet-overflow-32.c \
  /path/to/tinyvdb-build/libtinyvdb_mesh_ops.a \
  /path/to/tinyvdb-build/libtinyvdb.a -lm -pthread -o /tmp/tet-overflow-32
/tmp/tet-overflow-32
```

The experimental CPU sparse renderer can be checked with the optional teapot
corpus (the test skips when the file is unavailable):

```sh
TINYVDBIO_DATA_DIR=/path/to/tinyvdb/data \
  ctest --test-dir build_ninja/sdf-legacy \
  -R '^tool-lusdrender-sparse-volume-cpu$' --output-on-failure
```

It checks a volume-only render, field translation, 180-degree rotation, uniform
and nonuniform scaling, foreground and coincident-volume depth ordering,
identical images with one and two
threads, low-memory diagnostics, and the shared budget across two VDB files.
When `icosahedron.vdb` is available, it compares visible images from the dense
and sparse paths byte-for-byte. It also compares animated field-transform and
VDB file-path frames with equivalent single-time renders.
The foreground check fails with the old scaled-field depth
calculation. This compares the two LightUSD paths, not an external reference. Custom
asset readers allocate their own file buffers outside the cache budget.
When `LIGHTUSD_WITH_QJS=ON`, CMake adds `--with-qjs` to this regression. It
also renders both time samples through the persistent `-js` `setTime` path and
compares them byte-for-byte with the corresponding frame renders.

For CPU mesher changes, build each tinyvdb revision separately and compare a
level-set fixture on the same host (the test uses two workers and writes only
temporary files):

```sh
bash tests/volumetric/compare-mesh-identity.sh \
  /path/to/old-tinyvdb /path/to/old-build \
  /path/to/new-tinyvdb /path/to/new-build /path/to/level-set.vdb 0.15
```

The command compares the bytes of surface vertices and triangles, tetrahedral
vertices and cells, and boundary triangles. It reports SHA-256 hashes and fails
on differences or empty output. Choose a spacing appropriate for the fixture;
run representative fixtures to cover the change. Since the serialization is
native binary, compare builds on the same host with the same ABI. This does
not independently check internal scan-budget counts.

`volumetric-tet-worker-identity` checks that the uniform stuffing output stays
byte-identical with 1, 2, 4 and 8 workers for an analytic sphere. When
`TINYVDBIO_DATA_DIR` contains `icosahedron.vdb`, it repeats the comparison
through the real level-set sampler, including its per-worker accessor state.
Run it with:

```sh
TINYVDBIO_DATA_DIR=/path/to/tinyvdb/data \
  ctest --test-dir build_ninja/sdf-legacy \
  -R '^volumetric-tet-worker-identity$' --output-on-failure
```

For experimental CPU graded stuffing, compare an analytic sphere, box and
torus using two builds on the same host:

```sh
bash tests/volumetric/compare-graded-identity.sh \
  /path/to/old-tinyvdb /path/to/old-build \
  /path/to/new-tinyvdb /path/to/new-build 0.2
```

This compares counts and all vertex, tetrahedron and boundary array bytes.
The refinement suite also tests a high-valence edge; these three outputs do
not cover every topology or numeric scale.
The `volumetric-tet-float-range` CPU test checks that both uniform and graded
stuffing reject coordinates outside float range and tetrahedra that collapse
when a small object is represented at a large world offset, while preserving
the caller's existing output.
`core_tydra` exercises bounded VDB decoding, grid-selection fallback and
allocator ownership across moves. The Python volumetric suite additionally
tests that a compressed MJCF VDB exceeding the decoded-memory budget is
rejected and that a larger budget succeeds.

`volumetric-level-set-allocator` (legacy) and `core_level_set_allocator`
(next) sweep allocation failures through the CPU mesh-to-level-set pipeline.
They verify unchanged existing outputs/reports, complete cleanup, exact-cap
success and just-below-cap rejection, byte-identical serialized grids versus
normal allocation and multiple worker counts, and independent concurrent calls.
The initial tetrahedron fixture covers 350 allocation sites. The sweep also
passes Valgrind with zero errors and no leaked blocks.

`core_tydra` additionally checks generated-field peak/retained accounting,
move ownership, bounded replacement failure and retry, and serialized identity.
The Python volumetric suite exercises repeated distinct SDF assets within one
import budget and successful retry after optional generation failures.

The Blender CPU runtime script `blender/tests/test_usd_physics_runtime.py`
checks fractional timestep restoration, finite/unlimited hinge and slider limits,
D6 locked/free axes, skipped unsupported limits and unlimited export. Its
motor-driven hinge must rotate more than a full revolution. Run with
`ALSOFT_DRIVERS=null blender -b --factory-startup --python
blender/tests/test_usd_physics_runtime.py`; require the final
`BLENDER_USD_PHYSICS_RUNTIME_PASS` marker as well as process success, since
Blender can exit zero after a Python exception. The lightweight Python hook
suite also covers invalid timesteps and one-sided/out-of-range limits.

For the corresponding single-thread analytic sphere instruction profile:

```sh
cc -O2 -I/path/to/tinyvdb/src tests/volumetric/profile-graded.c \
  /path/to/build/libtinyvdb_mesh_ops.a /path/to/build/libtinyvdb.a \
  -lm -pthread -o /tmp/profile-graded
nice valgrind --tool=callgrind --callgrind-out-file=/tmp/graded.callgrind \
  /tmp/profile-graded 0.2
```

The hash is a quick diagnostic, not a substitute for the byte comparison.
Pass `1` as a second argument to `/tmp/profile-graded` to enable the optional
surface guard; compare it separately because it intentionally changes the mesh.

For a CPU-only wasm32 graded regression, after building the next WASM target
with `LIGHTUSD_TINYVDB_GPU=OFF`, run the refinement and complete stencil tests
with Emscripten and Node. Set `EM_CACHE` to a writable cache if needed:

```sh
EMCC_CORES=2 emcc -O2 -std=c11 -Isrc/external/tinyvdb \
  tests/volumetric/test-graded-refine.c src/external/tinyvdb/tinyvdb_tet_refine.c \
  -sENVIRONMENT=node -sEXIT_RUNTIME=1 -o /tmp/graded-refine-wasm.js
node /tmp/graded-refine-wasm.js
EMCC_CORES=2 emcc -O2 -std=c11 -Isrc/external/tinyvdb \
  tests/volumetric/test-graded-stencil.c src/lz4/lz4.c \
  web/build_ninja/vol-next/lightusd_core_build/tydra_build/liblightusd_tinyvdb.a \
  -sENVIRONMENT=node -sEXIT_RUNTIME=1 -o /tmp/graded-stencil-wasm.js
node /tmp/graded-stencil-wasm.js
```

The tests check topology, positive volumes, accuracy and limits on each
platform. Identical serialized output across native and wasm32 is not
guaranteed: the three analytic fixtures had equal counts but different array
bytes across the tested builds.
The tinyvdb `test_sparse_tools` suite additionally checks meshing when the
background is below the isovalue, alongside its threaded determinism and
budget-failure cases.

For a reproducible one-worker surface-mesher instruction count, build the same
driver against each revision and use the same VDB and host:

```sh
cc -O2 -I/path/to/tinyvdb/src tests/volumetric/profile-surface.c \
  /path/to/build/libtinyvdb_mesh_ops.a /path/to/build/libtinyvdb.a \
  -lm -pthread -o /tmp/profile-surface
nice valgrind --tool=callgrind --callgrind-out-file=/tmp/surface.callgrind \
  /tmp/profile-surface /path/to/level-set.vdb
```

This includes file loading and one-worker surface construction; compare
instruction counts rather than inferring CPU speed from a loaded host.

For the corresponding tetrahedral stuffing workload, build the `profile-tet`
target and pass the VDB path, spacing and optional worker count (default 1):

```sh
nice cmake --build build_ninja/sdf-legacy --target profile-tet -j2
nice valgrind --tool=callgrind --callgrind-out-file=/tmp/tet.callgrind \
  build_ninja/sdf-legacy/profile-tet /path/to/level-set.vdb 1.0 8
```

This includes file loading, lattice sampling, stuffing and boundary assembly.
The worker argument makes the parallel stuffing path reproducible; use wall
clock comparisons only on an idle host.

To profile CPU mesh-to-level-set construction with a reproducible closed mesh,
build `bench_volumetric` from tinyvdb's `tests/bench_volumetric.c` against its
CPU libraries, then run:

```sh
python3 tests/volumetric/make-profile-sphere.py /tmp/profile-sphere.obj
nice valgrind --tool=callgrind --callgrind-out-file=/tmp/vol.callgrind \
  /path/to/bench_volumetric /tmp/profile-sphere.obj --res 32 --tets 0 --threads 1
callgrind_annotate --inclusive=yes --threshold=95 /tmp/vol.callgrind
```

The fixture has 1,106 vertices and 2,208 triangles. At tinyvdb `33a9532` this
run recorded 10.86 billion instructions for 48,458 candidates; winding-number
evaluation accounted for 94.7% inclusive. At `f990ab8`, skipping winding for
candidates outside both band widths reduced the count to 5.01 billion and
produced the same VDB bytes on that fixture. The tinyvdb mesh-level-set test
also compares a full evaluator against the optimized path for asymmetric bands,
open and closed meshes, both winding modes and partial evaluator fallback.
Use representative meshes and retain instruction counts when comparing runs
under shared CPU load. `--host-only`
uses a synthetic evaluator, so its output is not a correctness comparison.

`lusdview_incremental_scene_update_test` now builds without linking core
and exercises the public C change-record planner interface. It retains the
slot/remap/deformation/material/texture cases and checks short records, null
path/property spans, unsupported flags, stale revisions, full resync, and
counted property names whose trailing bytes must not affect classification.
The `lusdview-incremental-*` Vulkan/OpenGL tests cover the viewer's temporary
adapter from native next changes to those borrowed C records during real
topology, layer-reload and texture updates.

`core_document_session_c` also creates a temporary shader layer in its
build directory, reloads property/stage-metadata edits, and verifies the full
caller-owned snapshot change export. It checks required counts, flags,
revisions, property names, short-buffer atomicity, invalid arguments, empty
changes and snapshot-backed string lifetime after later publications and
session destruction. `core_workflow_examples` verifies the C++ facade exposes
the variant-edited `/World.level` property in the interactive-session example.

The same C document test covers uncomposed loading, source-layer-only cache
retention and memory estimates using temporary root/reference layers. It checks
that trimming reduces transient bytes while keeping source layers, rejects
invalid controls/outputs, and verifies stage transfer is blocked by retained
snapshots. After transfer, snapshot access fails until reopen; transferred data
remains valid and independently editable after reopen and session destruction.
The interactive workflow also exercises the C++ memory query and stage transfer.

`core_document_session_c` also tests batch variant replacement and payload
loading on two sibling prims. It verifies single-revision publication, omitted
override removal, empty batches, malformed paths, duplicate selections, count
overflow, retained snapshots, and cancellation rollback followed by rebuild.
Loading both sibling payloads exercises pseudo-root cache invalidation with
previously retained composition data. `core_workflow_examples` uses the C++ batch
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
empty cache directory. Use `--core --backend vk --no-threaded
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
recomposition. `core_c_api` covers owned resolved property-name lists,
schema defaults, scalar/string views, array summaries without data buffers,
blocked/missing defaults, invalid handles and list lifetime after stage destruction.

When changing viewer document payload interfaces, run
`lusdview-usdz-deferred-payload` alongside the document unit and incremental
regressions. It checks the exact deferred prim/arc reported through MCP, the
core loader's proxy geometry, and successful deferred USDZ payload loading in
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
owner. `core_c_api` covers its authored-payload presence query. Include
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
HDR-to-EXR and EXR round-trip thumbnails on combined/core wasm32/memory64, with
16-bit atlas precision preserved. Rebuild next-only WASM with
`LIGHTUSD_WASM_WITH_EXR=ON` to enable both EXR reading and writing.

The Lucia source-delta gate checks compact history, Unicode, stale/damaged
records, and equal-asset snapshot sharing without mutable restore aliases.
It also checks bounded multiple intervals for distant edits, shifted undo
offsets, and lossless fallback when the line structure changes.
Its fidelity gate checks typed native transforms, atomic failed batches,
unindexed face-varying corner data, nested dependencies, and worker input
parity. The large browser workload also asserts that Health analysis yields
to the UI timer. Full native next checks include generated empty-property
metadata round trips, unknown property metadata through USDC, strict retained
and streaming conversion, approximation diagnostics, and inert light filters.
Malformed Points/Curves must produce failed diagnostics in both retained and
streaming conversion; strict streaming conversion must reject those skips.


## Strict lusdchecker regression

Configure `build_ninja` with `LIGHTUSD_BUILD_TOOLS=ON` and
`LIGHTUSD_BUILD_TESTS=ON`. Run `ctest --test-dir build_ninja -R
'lusdchecker|validation_registry' --output-on-failure`. The contracts assert
exact exit status and structured diagnostics for strict/Core profiles, schema
and shader manifests, compiled callbacks, sampled topology, reachable variants,
resource limits, JSON/SARIF failures, and baselines. Generated table freshness
is checked separately. Standalone Debug `core` testing and AOUSD supplemental
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

### Typed authoring and sampled-value follow-up

The standalone `core_c_api` covers empty numeric arrays, integer-vector
arrays, and authored custom flags. `core_aousd_conformance` generates 30
round-trip cases from the elective-field inventory: explicit empty/default
values must differ from unauthored fields through both USDA and USDC.
Structural type/order fields retain their separate syntax-specific matrices.
An additional unknown-metadata matrix checks 39 accepted raw list-op opinions
across layer, prim and variant scopes, plus 15 invalid qualified layer spellings.
This locks in `add` versus `append` authoring and composable inferred variant-set
names through both formats without inventing semantics for extension fields.

Local unknown-metadata assignment folding is covered separately across layer,
prim, variant and property scopes. Repeated qualifiers replace their earlier
value; a bare assignment removes prior qualified opinions and suppresses later
qualified edits. `tests/core/test-raw-metadata.py` retains all 36 fixtures against
pinned stock OpenUSD 26.11: 32 accepted inputs produce 64 comparisons through
USDA and native USDC round trips, and four qualified-empty-list inputs are
explicitly classified as stock text rejections. Native conformance tests retain
coverage of those compatible opaque inputs. This checks local authoring, without
assigning schema-defined cross-layer merge semantics to unregistered fields.
The native crate check also covers raw property metadata without extra quoting;
it does not claim stock interoperability for private raw metadata storage.

Relationship-default regressions distinguish absent defaults from authored empty
paths (`rel r.default = <>`), retain relative spelling, and check stronger
opinions against weaker defaults through composition and USDA fixpoints. These
use a separate authored bit from relationship target-list opinions. They do not
close the USDC requirement: pinned stock 26.11 itself rejects exporting SdfPath
relationship defaults to crate, and native crate preservation remains outstanding.

When the AOUSD checkout and pinned OpenUSD Python package are available,
`next_aousd_sampled_value_pxrdiff` checks 304 DefaultTime/numeric queries across
held/linear interpolation and clip transitions. Set
`LIGHTUSD_PXR_PYTHON` to relocate the oracle. The isolated stock 26.11 run
matches all 304 queries, with zero unexpected or classified timing differences.
The harness retains explicit classifications for previously observed corpus
timing differences; any unclassified mismatch fails. Held
interpolation now propagates into nested clips and missing-value interpolation.
Clip activation validation also covers unsorted unique times and duplicate
times with equal or differing asset indices, including nonadjacent authored
duplicates. Unique times are sorted before interval selection; duplicate times
reject the clip set. This differs from clip `times` mappings, where two entries
at the same stage time can encode a jump. Lean keeps literal authored-list
intervals separate from compatibility preprocessing, with kernel proofs of
sortedness, uniqueness, permutation preservation, and acceptance of unique input.

Lucia's `workflow-depth.test.mjs` and `project-storage.test.mjs` cover typed mesh
and primvar authoring, semantic undo, IndexedDB snapshot contracts, nested
composition, relocated dependencies, LOD selectors, worker budgets/cancellation
and sampled rig diagnostics. Run the complete Lucia Node suite and browser smoke
suite after editing those paths. Printing/archival quality gates now require
verified wall-thickness/provenance evidence; missing evidence fails explicitly.

## C11 storage and binding ABI regression

The value/storage refactor uses native C API major 5 and core C API major 2.
Build consumers against the new headers and libraries together. Value payloads,
ordered dictionary ownership, numeric buffers and Tydra chunk allocations now
use retained C11 storage. C++ string/array/dictionary accessors return borrowed
views; keep the owning value alive and reacquire views after edits. Numeric
parser and eager/lazy crate decoding fill the final allocation directly.

`next_c11_storage` checks alignment, retained budgets, COW, allocation-failure
atomicity, inline strings, string arenas, ordered/deep dictionaries, lazy
publication, descriptors, bounded numeric ingestion and shared chunk tables.
It links with the C compiler and needs no C++ runtime. The C API test also pins
numeric and inline-scalar buffers across source mutation/destruction. Retained
render pins keep their immutable scene catalog alive.

Run the standalone Debug suite as documented above. For a sanitizer pass without
precompiled headers, configure a separate Ninja directory with
`LIGHTUSD_USE_PCH=OFF`, `LIGHTUSD_ENABLE_THREAD=ON`, and ASan/UBSan on
both C and C++ compilation/linking. This repository disables C++ RTTI, so use
`-fno-sanitize=vptr`. Leak detection may need disabling in sandbox environments.
Check the individual `render-data*.hh` headers with `-fsyntax-only` and no PCH.

Python requires 3.11+. Run `python -m pytest python/tests -q` after rebuilding
with `pip install -e . --no-build-isolation`. Run the same tests with
`PYTHON_GIL=0` on free-threaded CPython. `test_buffer_lifetime.py` verifies typed,
read-only memoryviews, retained NumPy views, stage replacement/closure, render
catalog lifetime, strided/big-endian authoring and integer overflow atomicity.
Use `memoryview(array)`; the old `.memoryview()` and `__array_interface__`
interfaces have been removed.

The independent WASM feature contracts are in
[web regression documentation](../web/js/docs/regression.md#independent-next-feature-modules).
Build and ship the complete `lightusd_core_distribution`, including geometry,
raytrace, validator and EXR for each pointer width. `loadFeature(name)` is the
feature loader; constructors and functions belong to its returned module.

### Viewer, examples and tool consumers

The compatibility product configures `lusdview` and the older examples/tools,
while also linking the core/Tydra libraries for their core paths.
Use this build in addition to the standalone next checks:

```sh
cmake -S . -B build_ninja -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_EXAMPLES=ON \
  -DLIGHTUSD_BUILD_TOOLS=ON -DLIGHTUSD_BUILD_GUI_VIEWER=ON \
  -DLIGHTUSD_WITH_TYDRA=ON -DLIGHTUSD_WITH_C_API=ON \
  -DLIGHTUSD_WITH_MCP_SERVER=ON -DLIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_EXAMPLES=OFF \
  -DLIGHTUSD_BUILD_C_API_EXAMPLE=OFF \
  -DLIGHTUSD_ENABLE_THREAD=ON -DLIGHTUSD_ENABLE_THREAD=ON
cmake --build build_ninja --parallel 12
ctest --test-dir build_ninja --parallel 4 --output-on-failure \
  -R '^lusdview_.*_test$|^lusdview-gpu-material-abi$|^lusdchecker_|^lusddiff_|^workflow_tools$|^next_(workflow_examples|comprehensive_example)$'
```

Keep the two standalone next example options off in this compatibility tree:
the top-level example directories already define several of the same targets.
Build the full standalone next example set in its separate Ninja tree.
For display/GPU execution, follow [the viewer procedure](lusdview.md); these
consumer checks do not replace the full native and GPU regression gates.

For the full legacy sanitizer gate, use a separate Clang Debug Ninja tree with
`-O1`, ASan/UBSan on both C and C++, assertions enabled and PCH disabled. Keep
leak checking enabled on a host that supports LeakSanitizer, and save sanitizer
diagnostics separately from CTest output: a negative-input harness can accept
a child failure that still needs inspection. Run the viewer suite under Xvfb
with the verified GPU environment above. Exclude HIP only when requested with
`-E '(^|[-_])hip([-_]|$)'`; the broader expression `hip` also matches relationship
tests. For instrumented material-resolver runs, the existing
`LUSDR_MATERIAL_RESOLVER_TIMEOUT` override allows extra render time without
changing the normal regression threshold.
The AOUSD viewer sweep starts both loaders for every fixture. Its CTest budget
is 1,200 seconds with ASan or UBSan enabled, versus 300 seconds normally, to
cover the instrumentation cost across those processes.

Image checks include `usdz_convert_srgb_resize_test` and the
`png_stream_resize_*` cases. They exercise SIMD sRGB encoding and streaming
pixel callbacks, including null backing-image pointers. Keep these checks
enabled when updating the vendored image resizer.

For the external corpora, `scripts/prepare-usd-assets.sh` populates the pinned
USD-WG checkout under `.cache/lightusd-verification/usd-assets`. Run
`scripts/fetch-aousd-supplemental.sh` with
`AOUSD_SUPPLEMENTAL_CACHE="$PWD/.cache/lightusd-verification/aousd-supplemental"`
to prepare the pinned AOUSD release. Set `LIGHTUSD_USD_ASSETS_ROOT` to the first
checkout, `USD_WG_ASSETS_DIR` when configuring the next corpus test, and
`LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT` to the AOUSD `releases/1.0.1` directory.
The viewer's `lusdview-aousd-conformance` test instead accepts `AOUSD_ROOT`
pointing at the AOUSD checkout root, which contains `README.md`.

Run `lusdview-usd-assets-broad-smoke` with `LUSDVIEW_RUN_USD_ASSETS_BROAD=1`, and
`lusdview-usd-assets-animation-smoke` with `LUSDVIEW_RUN_USD_ASSETS_ANIMATION=1`.
Their output directories can be retained through `LUSDVIEW_USD_ASSETS_OUT` and
`LUSDVIEW_USD_ASSETS_ANIM_OUT`. The broad profile requires all 40 selected scenes
and runs Vulkan raster/RT plus renderer CPU/Vulkan. Keep `no_renderable` and
warning statuses visible in the report. See [the viewer corpus procedure](lusdview.md#external-usd-assets-smoke-rendering)
and [the WASM corpus parser](../web/js/docs/regression.md#usd-wg-corpus-parsing).

### Large-scene core performance gates

Use `tests/core/bench-large-scene.py` with two saved Release `lusdcat` binaries
built with the same compiler, flags, allocator, and thread support. Its JSON
manifest is a list of local cases, for example:

```json
[
  {"name":"scene-usdc", "input":"/assets/scene.usda", "format":"usdc", "payloads":true},
  {"name":"scene-usda", "input":"/assets/scene.usda", "format":"usda", "payloads":true},
  {"name":"crate-parse", "input":"/dev/shm/validation.usdc", "mode":"parse"},
  {"name":"layer-rewrite", "input":"/assets/layer.usda", "mode":"rewrite", "format":"usda"}
]
```

```sh
python3 tests/core/bench-large-scene.py \
  --baseline /tmp/lusdcat.baseline --candidate /tmp/lusdcat.candidate \
  --manifest /tmp/scenes.json --output /tmp/scene-results
```

The default is one warmup per binary and three alternating measured pairs.
Increase to seven pairs when the spread obscures the result. Do not compile or
run unrelated workloads concurrently. `--legacy-baseline` supports the CLI
before `--output-format`; USDC output uses a `.usdc` symlink to `/dev/null`.
That legacy CLI cannot perform an uncomposed USDC rewrite. Its compose-only
fallback also writes USDA, so compare the composition phase, not process time,
for that specific comparison.

All throughput output goes to `/dev/null`. The harness records binary hashes,
commands, phase timings, process wall/user/system time, faults, and peak RSS.
Per-second samples retain RSS, swap, thread count, CPU ticks and page faults;
the metadata records the clock-tick frequency for interpreting CPU samples.
Sampled peak swap and RSS-plus-swap are diagnostic metrics; sampling can miss
short peaks. The RSS gate uses the process high-water mark from `/usr/bin/time`.
It checks a 16 GiB host reserve and a 48 GB aggregate `/dev/shm` cap, terminates
its own process group on timeout/resource exhaustion, and rejects measured
maximum peak RSS growth above 5%. Reader and worker phase sums can overlap;
they are work attribution, not additive end-to-end wall time. Normal process
teardown is included; `--fast-exit` is diagnostic only.

The CLI accepts independent `--parse-threads`, `--compose-threads`, and
`--write-threads` hints, `--compose-only`, and `--output-format usda|usdc`.
`--rewrite-layer` preserves authored composition arcs for either format;
`-f` composes them. Automatic arc-layer prefetch divides its loading budget
between files and parsers: small inputs stay serial, while inputs of at least
64 MiB can use up to eight parser threads. The product of prefetch workers and
their parser hints stays within the composition thread budget. Other warm
workers keep automatic parsing serial; explicit parser hints remain honored.
`LayerLoadOptions::min_parallel_parse_bytes` applies the size gate to files,
memory buffers and individual USDZ entries (default 0 leaves normal hints
unchanged). The USDA and crate reader options expose the same threshold and
use their actual input/source length without an extra filesystem probe. The
USDA hint is restored after successful and failed parses for parser reuse.
Timing is opt-in through options in the library and
`LIGHTUSD_TIMING=1` in the CLI.

Qualify Caldera proxy, one full district, then all 45 full districts before
Island and the merged ALab techvar scene. Treat baked procedural frame 1004 as
a separate ALab case, leaving the other baked frames deferred. Record variant,
payload and thread policies with every result. Use matching baseline/candidate
SHA-256 hashes of both formats and prim/spec counts; a faster partial scene is
not a pass. Keep validation files only in task-owned `/dev/shm` directories,
remove them promptly, and use a streaming hash for text larger than the cap.
Reopen retained validation files with next and OpenUSD. Validation writes and
hashing are separate from throughput measurements.

Focused correctness coverage includes owning/borrowed lexer lookahead and
rewind, parallel crate reader cancellation/reuse, writer reuse with target-path
table growth, string-array range escaping and threaded byte parity, and file
layer reclamation while the returned Stage retains lazy arrays. Run these in
the standalone Debug suite, a thread-disabled build, ASan/UBSan without PCH,
and TSan when supported by the host.

Large flattened files may exceed the conservative default crate table counts
or input budgets. For known local data, pass explicit `--max-crate-elements N`
and `--max-memory-mib N` in the manifest's `args` list. These override the CLI's
array/table counts and per-input/read memory budget respectively; library
security defaults and path-depth checks remain unchanged. Record the same
limits for both binaries. If the old CLI lacks those controls, rebuild the
unchanged baseline library with the benchmark driver and verify its output
hash against the saved original executable before measuring it.

The reader decodes validated field value-representation tables directly into
their final buffer, using an explicit decompression budget. The generic C
blob decoder keeps its 1 GiB default cap. `core_crate_alloc_guard` checks
explicit-budget rejection, truncated multi-chunk frames, and (on 64-bit native
Unix) sparse mappings above 2 GiB without committing gigabytes of test RAM.
Large-file readback must also exercise a real table above 1 GiB; the all-full
Caldera crate expands its value-representation table to 1,413,255,776 bytes.

## Native OBJ/robot imports and Python physics

The native CLI and Python bindings share the C11 OBJ reader and native
URDF/MJCF frontends. `core_asset_import` checks the public import C API;
`core_urdf_to_usd` checks stage authoring, and
`physics_obj_c11_import_test` checks the legacy usdObj adapter.
`python/tests/test_native_import.py` covers OBJ/MTL data and texture packaging,
URDF/MJCF frames/inertia, include and package resolution, missing assets,
resource limits, physics snapshots, and authoring.

Use isolated build trees when another checkout consumer is compiling:

```sh
cmake -S src -B build_ninja/physics-import-next -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_BUILD_TESTS=ON
cmake --build build_ninja/physics-import-next -j8
ctest --test-dir build_ninja/physics-import-next --output-on-failure
LIGHTUSD_PY_BUILD_DIR="$PWD/build_ninja/physics-import-python/native" \
  python3 setup.py build --build-base build_ninja/physics-import-python/package
```

Run pytest against that build's `lib.*` directory, or install its wheel into
an isolated environment; do not replace another worker's editable extension.
The vendored tessellator's reflex-on-diagonal fix is covered by the concave OBJ
area regression. For sanitizer verification, build both C and C++ sources with
AddressSanitizer/UBSan; environments running under ptrace require
`ASAN_OPTIONS=detect_leaks=0` for the test process.

`core_obj_reader` additionally checks both C11 precision families, strict
`vw` errors, influence metadata and mixed Mesh/BasisCurves/Points imports. It
also runs with `LIGHTUSD_WITH_TYDRA=OFF`; that configuration creates no XML
frontend target. `LIGHTUSD_WITH_USDOBJ=OFF` removes the layer adapter entirely.
`python/tests/test_obj_layers_skin.py` covers seam copies, signed weights, more
than four influences, composition arcs, package dependencies and texture
lifetime. Enable its OpenUSD deformation oracle with a separate compatible
Python executable and OpenUSD's Python/plugin paths:

```sh
OPENUSD_PYTHON=python3.11 \
OPENUSD_PYTHONPATH="$OPENUSD_ROOT/dist/lib/python" \
PXR_PLUGINPATH_NAME="$OPENUSD_ROOT/dist/lib/usd:$OPENUSD_ROOT/dist/plugin/usd" \
PYTHONPATH="$ISOLATED_PYTHON_PACKAGE" \
  python3 -m pytest python/tests/test_obj_layers_skin.py
```

### Generated large OBJ tests

`core_obj_large` builds OBJ text entirely in memory and checks the C11
reader and typed USD conversion. It covers overflowed/invalid indices, global
limits across groups and primitive types, allocation-failure cleanup, bounded
blank-line handling, non-terminated guarded input, large-coordinate normals and
small decimals. Normal regressions cover subnormal-to-large position scales,
area-weighted smoothing across objects with mixed explicit/generated normals,
cancelled normals and avoiding smoothing allocations for fully authored normals.
The optional `--smooth` stress flag exercises the fully authored case at scale.
`python/tests/test_obj_large.py` checks generated face/skin
arrays through bytes, bytearray and memoryview inputs, plus expansion budgets.

The normal CTest case is small. For optional dense-geometry and >4 GiB byte-offset
stress runs, use the separate-build commands in
[Large OBJ inputs](usdobj.md#large-inputs-and-generated-stress-tests).
The generator has explicit size arguments and writes no asset files. Run the
small case under ASan/UBSan as well as the ordinary native build; do not use the
multi-gigabyte configuration as a routine sanitizer/CI requirement.

`core_obj_materials` supplies generated MTL bytes to both C precision
families through standalone parsing, referenced OBJ materials and callbacks.
It rejects malformed/non-finite numeric fields and overflowing integer options,
checks optional transform components and numeric texture filenames, and sweeps
allocator failures for cleanup. Its 2,001-material / 30,000 reverse-key case
checks sorted custom properties and stable duplicate order without large assets.
`python/tests/test_obj_materials.py` verifies the same numeric failures and
texture-transform behavior through an in-memory asset resolver. Resolver error
tests check that fatal MTL failures stop dependency reads; Python checks that
oversized MTL/texture dependencies fail through resolvers and filesystem reads.
Material lookup tests use 4,096 generated names in ascending, descending and
alternating order, preserve first-definition IDs for duplicates, distinguish
prefix names, and verify each repeated/missing library is resolved once per
load. Allocation-failure sweeps cover both indexes in scene and callback paths.
`--lookup-materials COUNT` increases this generated workload for opt-in stress
runs. Python cases preserve per-definition texture anchors and `Pr`/`Pm` flags
for duplicate names across BOM and LF/CRLF/CR source variants.

`core_obj_textures` checks request/canonical texture caching, separate
resolution and I/O, immutable shared ownership through layer cloning, per-import
cache lifetimes, compatibility with combined read callbacks, resource limits
and authoritative resolver failures. `core_usdz_writer` compares owning
and borrowed asset views byte-for-byte through memory and streaming writers,
including empty assets, invalid pointers and budget failures.
`python/tests/test_import_textures.py` checks OBJ/URDF/MJCF texture aliasing under
a tight memory budget, resolver removal, composed OBJ references and package
deletion. Both save methods must produce one texture entry and shader paths
that reopen from the resulting USDZ without the original resolver. The source
stage's paths remain unchanged.

`core_asset_import` covers owned report queries, failure reports, option
validation and the shared unsupported-feature policies through the C API.
Its MJCF flexcomp cases reject incomplete pin tuples, fractional or out-of-range
pin indices, and out-of-bounds reversed ranges; they also check valid tuples
split across pin elements and valid empty ranges. Direct flexcomp connectivity
rejects fractional, out-of-range and missing vertex indices while accepting a
valid triangle. `python/tests/test_mjcf_raw_flex.py` verifies raw flex index
diagnostics in warn and error policies.
`core_obj_textures` also checks native collector reuse, bounded diagnostic
storage, malformed UTF-8 and explicit truncation counts.
`python/tests/test_import_report.py` exercises independent input/memory/depth
limits, warning drainage, concurrent failed imports, resolver exception identity,
included XML policies, preservation budget failures and preserved metadata
round trips through USDA/USDC/USDZ. Reports remain runtime data after reopening.
The contract is documented in [import-report.md](import-report.md).

`core_physics_query` checks the shared native C++/C typed snapshot path:
coherent numeric time, overlapping groups, authored types/variability,
relationship targets, owned array lifetime, shared immutable storage and
snapshot memory limits. It links only `lightusd_c`.
`python/tests/test_physics_query.py` verifies the same behavior through
`physics.inspect`, including DefaultTime, blocks, empty declarations, unknown
engine properties, dictionary metadata, large array views, stage edits/closure
and invalid arguments. The new C header is also valid C11.
Generated standard physics descriptor cases verify uniform/varying authoring,
allowed-token metadata and drive/limit instance namespaces. The descriptor
tests also compare immutable OpenUSD, MuJoCo and Newton version and commit
provenance against the generated schema manifests. The descriptor
lookups also link from a pure C11 consumer of `lightusd_core_c`, without the
C++ validation engine.

`core_validation_report` checks the owned C/C++ report, native streaming
issue/byte limits, merge accounting, input validation, stage-independent lifetime
and explicit failure when all findings are omitted. Existing validation registry
and semantic suites cover the shared engine. `python/tests/test_physics_validation.py`
checks native rule IDs, group selection, immutable ownership, coherent sampled
mass values (with and without defaults), coverage/sample limits, large generated
invalid-body reports and independent inertia fallbacks. MJCF cases compare
inherited partial solver vectors with MuJoCo when installed, check canonical
option tokens and zero tolerances, and exercise ellipsoid approximation policies.
The optional Menagerie `--validate-physics` gate records semantic findings
separately from conversion/reference success. Add `--tessellate-collision-shapes`
to test explicit mesh colliders; see
[physics-validation.md](physics-validation.md).

### Updated Menagerie native/Python sweep

`tests/core/run-menagerie-import.py` inventories git-tracked XML/OBJ files at the
local checkout's actual revision. It does not update or pin the dataset. XML
fragments are accounted for separately, including the auxiliary Aloha actuator
fragment that has worldbody sites but cannot compile without its parent bodies.
Every standalone entry is compiled with Python MuJoCo and imported in strict
asset mode. The runner compares authored body rest frames, joint anchors/limits
and engine record counts with MuJoCo, and records physics parity separately
from conversion success. MuJoCo compiler inference and tendon surface wrapping
are not implemented by the import frontend.

Pass both independently built native CLIs to check USDA/USDC/USDZ emission,
Python reopening and OpenUSD reopening, in addition to Python import/export.
Use frozen copies of the extension and CLIs during a long sweep so a concurrent
rebuild cannot replace a mapped binary. Reports contain per-file warnings,
reference mismatches, timing and peak RSS; failed case artifacts are retained.

```sh
PYTHONPATH="$ISOLATED_PYTHON_PACKAGE" python3 tests/core/run-menagerie-import.py \
  --menagerie "$MENAGERIE_ROOT" --out build_ninja/obj-skel-report/final \
  --cli "$LEGACY_IMPORT_CLI" --cli "$NEXT_IMPORT_CLI" \
  --usdcat "$OPENUSD_USDCAT" --workers 4
```

The runner requires `mujoco` and the isolated `lightusd` package in the selected
Python. `--cases-from retry.json` accepts a JSON array of tracked relative paths
for focused retries. A conversion pass does not claim simulator equivalence;
inspect `reference_checks` and warnings in the JSON report as well.
See [OBJ mapping and skinning](usdobj.md) for supported records and binding rules.

### Downstream Newton and OpenUSD checks

`tests/core/run-newton-compare.py` builds two Newton models per MJCF file in
one process: `ModelBuilder.add_mjcf` on the source and `add_usd` (with
`SchemaResolverMjc` and `SchemaResolverNewton`) on LightUSD's output for each
`--profile`. It compares DOF, collider, site and body counts, per-body mass,
COM and inertia, joint types and per-DOF limits, armature, damping and
friction, and sorted shape friction. When `mujoco` is importable its compiled
model arbitrates: a body or collider count where LightUSD matches MuJoCo but
Newton's own MJCF importer does not is recorded under `reference_differs`.
Newton needs `warp-lang`, `mujoco`, `mujoco-warp`, `usd-core` and
`newton-usd-schemas`; CPU is sufficient.

```sh
PYTHONPATH="$ISOLATED_PYTHON_PACKAGE" "$NEWTON_PYTHON" tests/core/run-newton-compare.py \
  --menagerie "$MENAGERIE_ROOT" --profile newton --out newton-compare.json
```

At Menagerie `cda5236f` with Newton 1.8.0.dev0 (`2c86f4a4`), warp 1.18 and
MuJoCo 3.15 (Newton's pin), 65 of 69 first models pass in the `newton`
profile. The remaining four are understood: thin-shell `ms_human_700` rib
masses (within 1.2% of MuJoCo), the `stretch` gripper body whose mesh has
non-convex n-gons up to 215 sides that MuJoCo triangulates differently (0.2%
inertia), an all-zero authored `ufactory_lite6` base inertia left unauthored
(as mujoco-usd-converter does), and `rby1`, where Newton's MJCF importer
applies a default-class range despite `limited="false"` (LightUSD keeps the
wheel unlimited, as MuJoCo does) and Newton's USD importer has no way to keep
an explicitly massless body with colliders massless. The harness skips
include fragments without `<worldbody>`, writes each input to a unique file
(USD caches layers by path), matches joints one-to-one, and reports a merged
D6 whose DOFs differ only in order as `dof_order` (Newton orders merged
joints by sorted prim path, not authored order).

`tests/core/run-openusd-physics-check.py` saves every profile as USDA, runs all
registered OpenUSD `UsdValidation` validators (including
`usdPhysicsValidators:*`) and reports what `UsdPhysics.LoadUsdPhysicsFromRange`
recovers. It fails if the four physics validators are unavailable, a converted
stage cannot be opened, or a validator reports an issue. Repeat `--require-kind
KIND=COUNT` to require at least that many parsed records in each result; for
example:

```sh
PYTHONPATH="$ISOLATED_PYTHON_PACKAGE:$OPENUSD_PYTHONPATH" "$OPENUSD_PYTHON" \
  tests/core/run-openusd-physics-check.py --require-kind RevoluteJoint=1 \
  --require-kind CubeShape=2 tests/feat/physics-oracle/hinge.urdf
```

`python/tests/test_openusd_physics_harness.py` checks missing validators,
failed stage opens and missing required parser records without needing OpenUSD.
These checks establish validation and parser coverage, not dynamics equivalence.

With Python OpenUSD available, `python/tests/test_openusd_physics_semantics.py`
also checks a rotated URDF hinge's parsed world-space anchor, axis, limits and
body relationships across three profiles and both layouts. It checks parsed
site-frame connect and weld joints across the two physics profiles and both
layouts. These are rest-frame semantic checks; they do not simulate dynamics.

The urdf-usd-converter test corpus (`tests/data/*.urdf`, 81 files) is a useful
URDF sweep: all non-`error_*` files except those referencing deliberately
missing meshes (strict by default; use `allow_missing`) convert, including the
COLLADA, planar-joint, ghost-link and ASCII-STL cases.

The URDF frontend also tests noncommuting fixed-axis RPY rotations against the
`Rz(yaw) * Ry(pitch) * Rx(roll)` convention used by
[urdfdom](https://github.com/ros/urdfdom_headers/blob/rolling/include/urdf_model/pose.hpp).
The public JSON bridge rejects scalar records, and the converter regression
includes heterogeneous nested arrays to protect builders holding PrimSpec
pointers during layer growth. Run that case under ASan/UBSan with the same RTTI
setting as the core (`-fno-rtti`).

`tests/core/test-obj-wasm.mjs` checks the isolated WASM OBJ layer parser, changed
skin weights, geometry-property parity with a native USDA import, binary/package
roundtrips and physics JSON authoring. Pass its module path and a native USDA
fixture exported from the inline OBJ source in the script. This avoids replacing
another worker's WASM artifacts.

Parallel instance-claim repair (large-scene §10.91) needs fresh coverage in
`core_pcp_parallel` and `core_instance_group`: compare serial and
parallel prototype selection/child order when a discarded subtree owns a
speculative claim, including nested repairs, donations and earlier-batch seeds.
Check depth-limit/unresolved-source fallback as well. This qualification is
deferred under the current implementation-first workflow; existing passing
results do not validate the new repair path.

Projected SPECS delta encoding (large-scene §10.92) additionally needs byte
parity against the C encoder for most-common-delta ties, arbitrary/wrapped u32
values, wide-delta candidate pruning and counts across the parallel threshold.
Exercise mapped and fallback spec views, invalid-fieldset fallback to zero,
and complete writer parity. A small standalone native/no-thread codec smoke
has passed; the integration and regression work remains deferred.

Heavy field chunk refinement (large-scene §10.93) needs complete writer parity
for multiple heavy prims in one original chunk, a heavy prim on either chunk
edge, combined smaller arrays, time samples and variable-size elements. Check
consume-prims reaping and pre/post-first-commit failures, plus unchanged fixed
ranges for low-memory/deferred-fill inputs. Real-SSD Island measurements must
include chunk_plan overhead and peak RSS; equal context counts alone do not
prove equal memory use. These runtime checks remain deferred.

Discovery sizing and prefetch fan-out (large-scene §10.94) need coverage for a
single seed revealing several targets, queue drain/shutdown and explicit parser
budgets. Compare the complete discovered count to BuildStage for inactive prims,
arcs and namespace edits. Frontier/depth caps, active instanceable subtrees and
callback policies must keep conservative sizing; fixed batches remain fixed.
Retained-plan release (§10.95) needs one-shot output/resource parity, failed
Plan/writer startup cleanup and confirmation that public repeated Plan and
dependency manifests retain their previous behavior. Runtime checks are deferred.

For bounded readback/Tydra profiling, build the opt-in `bench_scene_readback`
target and run `bench_scene_readback scene.usdc 1 retained` (or `4 metadata`).
It uses 512 MiB reader/converter budgets; still use the process memory guard.
Its native chunk-copy timing excludes JS extraction and is not a browser result.
See large-scene §10.96 for baseline provenance and progress-gate limitations.

ChunkedArray::copy_bytes_to and the web geometry bridge (§10.97) need permanent
native/WASM coverage for aligned/unaligned copies, exact/short capacities,
partial chunk tails, f32/u32/u16 payloads, heap growth and complete mesh/points/
curves output parity. The native focused smoke and Emscripten syntax check have
passed; full regressions/current-runtime performance checks remain deferred.

Scoped web mesh preparation (large-scene §10.98) needs native dispatch/runtime
coverage for begin/end/nesting, one actual geometry build across repeated
size/header/payload calls, failed builds and intervening scratch builds. Check
all ten streams for indexed/soup/merged/analytic/skinned/blend-shape inputs,
flag changes and supplied-clip replacement/removal. JS must close on preflight,
header and copy failures and block deletion during the operation. Syntax and
a small mock-dispatch lifecycle check pass; fresh WASM output/performance and
heap-growth checks remain deferred.

Direct compressed-section emission (large-scene §10.99) needs permanent coverage
for byte parity with the owning/C APIs, zero/partial inputs, callback lifetime,
compression failures without emission and destination failures without raw
fallback. Check C bound overflow on 32-bit/memory64 and near the 255-chunk limit
without allocating giant fixtures. Large no-thread u32 columns need independent
C byte parity across threshold/chunk-code boundaries. Focused native/no-thread
smokes pass; complete section/scene parity and SSD/RSS qualification are deferred.

Pass-1 lifetime coverage should include the late back-reference/stateful-reader
case in large-scene §10.101: discovery and the first batch have not loaded the
later root, whose layer then reaches an earlier shared source. The original
reader must not be invoked again across planning/run; dependency identifiers
must stay complete. A focused two-batch probe passes. Parallel authority checks
should also inspect next_walk speculative_rewalks/seed_rewalks/source_rewalks
and serial_fallback so an unexercised branch or silent serial fallback cannot
masquerade as repair-path coverage.

Focused authority checks (§10.102) now establish a real 64K-gated parallel walk
with byte parity and no fallback, depth-triggered fallback matching the serial
partial-stage/typed-diagnostic contract, and two injected stale-claim repairs
with nested child/type/order checks. The injected probe compiles current
implementation sources directly; it does not establish worker race coverage.
Permanent cases should retain these distinct gates instead of treating a
successful parallel run with zero re-walks as repair coverage.

A focused wide-writer check (§10.103) now exercises real range refinement
(261 versus 256 chunks), serial/fixed/refined byte parity and consume/reaper
cleanup using a bounded synthetic input. The isolated fixed-chunk control
changes only the refinement boolean; both parallel probes use identical
thread/consumption/allocator settings. This does not cover production Island,
lazy-array fan-out, time-sampled/variable-size workloads or failure paths.

Focused spill-view checks (§10.104) now cover typed view/legacy parity, appends
after snapshots, mapped and no-mmap builds, mapping fallback, interrupted and
partial writes, truncated prefixes, tail-write failures and fallback-read EOF.
A batched writer fixture crosses real chunk thresholds and observes four
actual mapped spill views; memory-only, mapped-spill and forced-ENOMEM fallback
writes produce identical regular SSD files. Ordinary WriteLayerToFile alone
does not trigger boundary eviction and cannot establish this coverage. These
bounded checks do not replace large-scene, 32-bit or multi-chunk LZ4 gates.

Fresh wasm32 scoped-copy checks (§10.105) now link the current module and
compare complete copied outputs with scopes enabled/disabled in that same
runtime. Fixture geometry-read counters establish one versus 24 preparations
per mesh. Indexed, explicit indices-disabled, merged and a real face-varying
soup case pass; nested/mismatched scope rejection and failure cleanup pass.
Use fresh streams for bounded A/B checks: the crate reader's cumulative decode
budget charges temporary decodes as well as retained storage. This does not
replace skinned/blend-shape, clip-mutation, heap-growth or memory64 coverage.

Focused discovery/prefetch checks (§10.106) now establish the complete-count
target boundary, unknown sizing under frontier/depth limits and reader/payload
callbacks, an authored inactive leaf, and fixed-root batch preservation. Their
pass-1 path order matches serial BuildStage. A controlled single-seed reader
proves two automatic workers overlap after fan-out, explicit parser settings
retain the former one-worker policy, and stopping during a read joins without
loading newly found children. This does not establish actual parser throughput,
namespace-edit coverage or the full callback/load-rule matrix.

Low-memory PATHS node-storage reuse (§10.107) needs coverage around the 131,072
encoded-node threshold, deep trees, variants, relative/property and empty paths,
and serial/parallel node preparation. A bounded wide fixture now establishes
ordinary, batched memory-only, mapped-spill, forced mapping-fallback and
no-thread mapped-spill byte parity with saved pre-change output. Exact-size
projected encoding also applies to the preorder in the selected policy. This
check does not establish whole-process memory or production throughput gains;
the two eliminated arrays account for eight bytes per encoded path specifically
at column preparation, while retained traversal/codec state remains live.

Mixed-array scheduling checks (§10.108) now cover time-sampled-only arrays,
default-plus-sample arrays, the variable-size token proxy, multiple small arrays
per prim and accumulated work across prims, including original chunk edges.
The expected refined range count, complete serial/parallel byte parity and
consume/reaper removal of both properties and time samples pass. Large lazy
payloads and pre/post-commit failures still need separate coverage; serial
versus parallel timings do not isolate the benefit of refinement itself.

Earlier-batch prototype seeds must survive a failed parallel walk (§10.109).
The focused 64K-gated reproduction now checks the member's IsInstance and
GetPrototype results, typed depth diagnostics and complete post-fix crate
parity against serial. Before the fix, primitive/byte counts were unchanged
despite the member being incorrectly promoted to a new prototype. Permanent
coverage must inspect prototype identity as well as output counts and retain
the explicit serial_fallback timing gate. Full seeded-batch/donation coverage
remains deferred.

Source-miss repair (§10.110) must distinguish a repeated miss from progress to
an unseeded prototype. A focused current-source probe now establishes the
source -> seed -> forced-rewalk transition, seed-only and stale-claim repairs,
and depth-triggered failure after partial repair. Successful cases inspect
child/type/order and confirmed authority after temporary spec stores are gone;
failure cases inspect pause/TLS cleanup. Keep the repeated-miss and unresolved
forced-seed guards when extending coverage. This does not replace concurrent
worker/donation checks or measure production fallback frequency.

Medium low-memory structural delta encoding (§10.111) has focused writer parity
with a C-entry-point counter: batched memory-only, mapped spill, mapping fallback
and no-thread spill outputs match the saved file and avoid medium C delta calls;
ordinary zero-budget output retains those calls. Future coverage should include
131,072/four-million-element boundaries and pathological wide-delta histograms.
Lower full-delta/output scratch does not bound histogram state or prove a
production-scene RSS/throughput gain.

The bounded Island comparison (§10.113) now establishes full-file parity and
the default-profile refinement's observed RSS reduction with a matched
fixed-chunk control. Three alternating pairs use regular SSD files, per-run
fsync, normal teardown, identical jemalloc/flags and unchanged encoded-context
counts. Parallel planning uses disjoint boundary masks merged in order; the
mixed-array consume check also retains its exact output/range count, and
no-thread compilation passes. Full suites and cold/other-scene performance
remain deferred; a near-equal write median is not a general throughput claim.

The residual scoped WASM extraction profile (§10.112) times 64 exported budget
queries separately from mesh-buffer calls. Any future budget-query reuse must
preserve accounting across lazy materialization and asset/export mutations;
the current change retains all checks. Its recorded module is a profiling
artifact, not a newly qualified WASM distribution.

The PCP path-lifetime fixture also compares the prepared writer path against
ordinary output across ~72000 prims, exercising mapped encoded-operation
scratch and multiple 1024-prim contexts. Three smaller checks cover late
instanceable-field removal, moving the first surviving token use, authored-false
updates, raw token-vector/array dedup and rejection of a second commit before
it can truncate the output. CLI focused checks compare retained/prepared and
ordinary output with 1/4 compose threads, require preparation before Plan
completion with no Run pass, and verify scratch-limit failure cleanup. See
[planning/encoding overlap](large-scene.md#10117-planning-overlaps-writer-encoding-and-scratch-io).

The `core_pcp` suite also checks opt-in immutable source snapshots:
read-once callback replay, concurrent initial/replay loads, parsed-layer
expiration, byte-limit failures, native-file removal, original asset anchors,
pass-1 back references, retained mapped USDC values after registry cleanup and
USDZ entry replay after archive removal. Source snapshots require a selected
POSIX disk directory; preloads and foreign dependency owners remain retained.
See [large-scene snapshot details](large-scene.md#10116-immutable-source-snapshots-and-pass-1-eviction)
for options, focused CLI parity evidence and remaining qualification.

The `core_spill_array` test checks append/resolve equivalence with spilling
enabled and disabled. On Windows, requesting spill uses an in-memory fallback;
the test verifies that no bytes spill and no I/O failure is reported. The
structural-column spill budget and directory have no effect on Windows.

### Opt-in NVIDIA level-set regression

For bounded OpenGL, Vulkan, CUDA and OptiX surface checks, use
`examples/lusdview/tests/check-level-set-gpu.py` and the optional
`profile-gpu-level-set` fixture/profiling target. The runner verifies actual
backend selection, transformed silhouettes and mesh occlusion, and monitors
additional VRAM without changing other GPU processes. Build, Xvfb, validation
layer and profiler instructions are in [lusdview.md](lusdview.md). These hardware
checks are opt-in and are not part of the ordinary CPU regression suite.

### Cached large-scene preview regression

Build `lusdpreview` and `test_scene_preview`, then run
`ctest --test-dir build_ninja -R '^core_scene_preview$' --output-on-failure`.
The generated fixtures cover all three representations, bounded outputs,
portable export, cross-batch native instances, forward PointInstancers,
texture baking, dependency invalidation, malformed/truncated cache recovery,
cancellation and default VRAM sizing. Additional cases cover PointInstancer
targets inside native instance copies, prototype-local transforms, adaptive
volume bricks with mixed-scale environment geometry, portable four-level USD
variants, hysteresis and aggregate LOD budget selection.

Run `python3 tools/lusdpreview/check-lod.py --viewer build_ninja/lusdview
--output build_ninja/preview-lod-check` for 27 headless captures across all three
representations and CPU / Vulkan / CUDA. Near/far captures must trigger automatic
refinement within budget; the finest capture must match disabled-LOD rendering
within one 8-bit rounding step. Run under GPU-assisted validation as below.
LOD reloads upload the complete replacement scene before capturing a settled
frame. The camera runner forces device-local staged mesh uploads to cover scene
replacement before the first raster draw; scene teardown must flush pending
copies before destroying their destination buffers.
The `lusdview-vk-gaussian-chunks` hardware-RT test also checks the shader
feature declarations required by the NVIDIA ray-query path.

For headless hardware qualification run
[check-renderers.py](../tools/lusdpreview/check-renderers.py) with the built
viewer/renderer, source scene, cache and output directories. It exercises both
applications on CPU / Vulkan / CUDA for Gaussian, point and volume previews,
records actual per-process NVML VRAM and RSS, and requires hardware allocations.
Use the [Vulkan validation layer procedure](lusdview.md#gpu-assisted-validation-shader-faults--device-loss)
for shader/descriptor qualification. Keep large source assets, cached sidecars,
images and measurement logs in ignored build directories.


For extended previews, `core_scene_preview_compute` checks CPU conversion
and direct UsdVol SDF/albedo rendering, rejects malformed compute buffers and
verifies camera updates reuse immutable grid data. Run its executable with
`vulkan` and `cuda` on hardware to compare each kernel against the CPU reference;
an unavailable requested backend fails. `core_light_filter` checks the
portable light-filter registry, `lightusd:filter:kind`/parameter parsing with
clamping, the fold into the render-ready light intensity/color, warnings for
unknown kinds and missing targets, custom-kind registration/unregistration, and
the folded values recorded in a cached preview. The compute test also checks
preview light and shadow linking (mask exclusion/inclusion, independent light and
shadow masks, CPU/lavapipe parity), and `check-source-refinement.py --software`
runs the 12-capture refinement matrix against Mesa without NVIDIA probing.
Custom filter kinds register through the C API
(`lightusd_register_light_filter`, covered by `core_light_filter_c`) and
through Python (`lightusd.register_light_filter`, covered by
`python/tests/test_light_filter.py`); both surfaces share the C++ registry used
by conversion and preview caches. `core_scene_preview` also checks
that regional source resampling produces finer detail and retains the background,
including PointInstancer targets inside native instances and nested prototype
triangle/placement graphs.
Run `tools/lusdpreview/check-source-refinement.py` for 18 automatic-resampling
captures and run it under Xvfb with `--threaded --owner vk` / `--owner gl` for six
captures per actual render owner. The JSON report must confirm an active render
thread, finer source cells and increased sample counts; direct volume captures
must report the grid path. Keep logs, caches and images in ignored build folders.

Persistent-index qualification must request an uncached camera region and reject
the `open` progress phase, while observing `source-index`. The retained sample
count and source detail must increase for a targeted surface. Changing a source
layer must invalidate the index; keep original geometry and generated indices
out of Git. Index creation may revisit the whole scene once.

The compute test also checks analytic Beer-Lambert absorption and single
scattering, emission and phase changes, density-only UsdVol loading/framing,
and authored scene light effects. Passing a third argument to the executable
retains its fixtures in that directory for application-level checks. Run
`check-renderers.py ... --native-volume` on its `fog.usda`, and use
`--kinds volume --volume-mode medium` to qualify participating media on larger
previews. `check-particle-radiance.py` guards against relighting converted point
colors with extreme scene lights; captures must remain nonblank and identical.

Textured-emission and multiple-scattering qualification extends the compute test
with analytic Monte Carlo Beer-Lambert transmission, nonnegative higher-order
energy, fixed-seed repeatability, colored dome/rect lighting, rotated dome
transforms and malformed texture/sample descriptors. CPU, CUDA and Vulkan must
pass; Vulkan runs under GPU-assisted validation. The conversion test checks
portable linear-HDR sidecars after deleting the original texture, and upgrades
a legacy lighting cache while rejecting geometry resampling. For application
checks, retain the compute fixtures and run:

```sh
python3 tools/lusdpreview/check-renderers.py FIXTURES/textured-fog.usda \
  --native-volume --volume-mode medium --volume-bounces 4 --volume-samples 8 \
  --viewer build_ninja/preview-render/lusdview \
  --renderer build_ninja/preview-render/lusdview_lightrt_c_build/lusdrender \
  --cache build_ninja/textured-volume-cache --output build_ninja/textured-volume-check
```

Use the same scattering controls with `--kinds volume --no-source-refine` for
Caldera and larger cached scenes. Keep the 2048 MiB process VRAM gate and inspect
noise and lighting variation in the captures; stochastic paths need an RMS
comparison, since tiny floating-point differences can change collision branches.

Emitter-importance checks add single-pixel off-axis dome and rect maps and
compare their raw radiance against integrated energy at one light sample.
Validate truncated/nonmonotone CDFs, the 1–64 CLI bound and render-cache
invalidation when the sample count changes. Headless qualification should also
use a sparse bright map whose nonzero texels occupy less than 0.1% of the image.
Run CPU, CUDA and GPU-assisted Vulkan; keep captures within the 2048 MiB gate.

Projection and accumulation checks cover angular/mirrored-ball emission energy
against numerical spherical integrals, including partial texels at disk edges.
The compute test compares both GPU backends to CPU and checks statistically
bounded Monte Carlo energy. Accumulation must reduce image MSE, respect its cap,
reset on camera/viewport/shading/scene changes, and retain its count when a final
capture reads the last frame. App reports include `preview_accumulated_frames`;
require a value above one for real progressive viewer/render-thread tests.

For software-only preview development, run compute checks through
`scripts/with-software-rendering.sh` and application checks with
`tools/lusdpreview/check-renderers.py --software`. This pins Mesa lavapipe and
llvmpipe, disables the preview NVIDIA-memory probe, skips CUDA and distinguishes
software results from hardware qualification. Do not force an NVIDIA device
when using this mode. Hardware tests remain an explicit separate qualification.

Higher-quality medium/light checks include independent partial overlaps,
extinction-weighted emission, finest-level halo priority, a thin density peak,
diagonal cubic-density optical depth, rays beyond 4096 steps, scattering beyond
eight bounces, and explicit ray/shadow-budget exhaustion. Finite rect/disk/
sphere/cylinder illumination is checked against geometric solid-angle integrals;
a conservative medium in a unit environment checks radiance conservation.
The shared scalar kernel runs on CPU and Mesa lavapipe only for current goal
qualification. See `doc/preview-lighting-goal.md` for the mandatory no-hardware
constraint and completion audit.

## OpenUSD / lusdrender offscreen comparison

[`tools/ousddiff`](../tools/ousddiff/README.md) provides JSON-configured external
corpus inventory, deterministic probe/full selection, shared camera/frame/variant
preparation, process time/RSS measurements, image comparison, historical gates,
and offline JSON/HTML reports. Presets cover Caldera, Island, merged ALab,
USD WG assets, and MJCF-to-USD Menagerie inputs. Dataset and executable paths
remain configurable; generated inputs and reports stay outside tracked files.

For a bounded 1% CPU validation probe without complete large scene roots:

```sh
python3 tools/ousddiff/runner.py run --preset usd-assets \
  --mode probe --sample-percent 1 --no-full-scenes --max-input-mib 16 \
  --renderers embree,cpu --out /tmp/ousddiff-probe
```

Sampling rounds up per stratum; inspect `selection.json` for effective coverage.
An entry layer's small file size does not bound its composed dependencies.
Cross-renderer image metrics are informational until reviewed thresholds are
configured. Crashes, timeouts, resource limits, required-backend failures,
invalid images and configured threshold failures fail the run. Hardware checks
reject software GPU fallbacks. The complete measurement definitions, baseline
compatibility rules and exit codes are documented in the tool README.

The `tool-ousddiff-harness` CTest is available in both native product
configurations with `LIGHTUSD_BUILD_TESTS=ON` and Pillow in the configured Python
interpreter. Use `-DPython3_EXECUTABLE=/path/to/python3` if CMake selects another
Python. Real-renderer integration is opt-in:

```sh
OUSDDIFF_RUN_INTEGRATION=1 python3 -m unittest discover -s tools/ousddiff -v
ctest --test-dir build_ninja -R '^tool-ousddiff-harness$' --output-on-failure
```

`LIGHTUSD_OUSDDIFF_EXTERNAL_TESTS=ON` additionally registers an optional 1% CPU
USD-assets probe; absent datasets return CTest skip code 77. Full-corpus and
complete large-scene runs require explicit invocation.

For the AMD hardware matrix, use `--profile amd`. This selects Storm, Vulkan
compute, and Vulkan ray queries with explicit AMD-device/backend checks. Run
outside the managed sandbox when `/dev/dri` is hidden. The opt-in
`LIGHTUSD_OUSDDIFF_AMD_TESTS=ON` CMake option registers
`tool-ousddiff-amd-hardware`, a tiny offscreen test requiring real AMD hardware.

Use `--failed-from <results.json>` to retry unexpected failures only, including
individual failed animation frames. The runner writes `failed_ids.json` and
structured diagnostic codes; JSON summaries and HTML filters distinguish
unsupported geometry, MaterialX failures, context/device failures and visual
threshold failures. These details and the profile configuration rules are in
the tool README.


### Analytic primitive renderer coverage

`tool-lusdrender-analytic-primitives` checks schema-default Cube/Sphere geometry,
payload material bindings and transforms, native and point instances, masks,
visibility/purpose, generated UVs, and animated radius/BVH invalidation. It uses
small generated fixtures and requires no external assets. The Tydra suite
also checks default-time and interpolated analytic dimensions. See
[headless GPU commands and texture coverage](lusdrender.md#analytic-geometry-regression).
The same fixtures verify PreviewSurface image routing on generated and authored
geometry, UV rotation, and packed-channel scalar scale/bias on CPU and ray queries.
The legacy compute preview uses constant roughness.

### Static batch culling audit

`lusdview_rt_lod_test` includes CPU-only static-chunk projected-radius checks:
projection Y sign, off-axis view depth, viewport height, camera-intersecting
spheres, orthographic scale and degenerate projection fallback. Run it with
`ctest --test-dir build_ninja -R '^lusdview_rt_lod_test$' --output-on-failure`.
The Vulkan chunk path retains direct draws for primitive/source-face diagnostics
and Ptex, gates indirect commands on enabled device features, and splits lists
at the physical device limit. Camera-record equivalence, noninstanced LOD,
persistent MCP scene replacement and incremental topology gates exercise the
surrounding camera/resource lifecycle; they do not replace external large-scene
performance measurements.

`lusdview-wire-depth-fill` renders an authored quad in shaded and wireframe
modes with a fixed camera. It requires a visible shaded interior and an
unpainted wireframe interior, catching an opaque shader specialization replacing
the color-write-disabled depth pipeline. It runs on software or hardware Vulkan.
The deformation depth-parity harness disables the grid as well as skeleton
helpers: grid placement follows posed bounds and is composited after the AOV,
so it can differ even when CPU/GPU geometry matches.

### Linked lusdview / usdview diagnostics

The opt-in linked viewer controller, authentication, capture reports and live
regression commands are documented in [tools/view_control/README.md](../tools/view_control/README.md).
Run the Python unit tests there and the live test against an explicitly launched
small fixture when changing the shared camera/MCP protocol. Hardware live tests
require the GPU/display access described in the viewer documentation. An installed
older USD tested through the development bootstrap does not validate the rebuilt
OpenUSD fork.

For linked viewer changes, also run the opt-in retained-frame and bone regression
in `tools/view_control/test_frames_live.py` against a controller session launched
with `tests/usda/view-control-skeleton.usda` and the installed OpenUSD fork. It
checks independent bone visibility with guide purpose disabled, unchanged skin
and descendant geometry after toggling, exact native framebuffer readback,
retained pixels after display/selection changes, token idempotency and release.
The lusdview **Tools > Compare viewers** panel must be checked on GL and Vulkan,
including image replacement and renderer switching; the native `compare_panel`
tool can save its composited window for visual review. Full paired captures need
the updated fork's mono raster frame-barrier protocol.

Headless agent comparison is documented in
[`tools/view_control/HEADLESS.md`](../tools/view_control/HEADLESS.md). Run its
`test_headless_live.py` suite on CPU and available GPU backends after changing
lusdrender camera generation, shared GPU camera matrices or the fork's usdrecord
EGL/FrameRecorder code. The suite removes DISPLAY/WAYLAND_DISPLAY, validates
same-view silhouettes and clipping, and checks actual diagnostic annotation
pixels, edit atomicity and source immutability. Run `test_headless_usd.py` with
the installed SDK environment for camera/default-value and USDZ dependency checks.

### Paired headless asset/time/AOV validation

Use `tools/view_control/control.py validate --config
 tools/view_control/jobs/usd-assets.json --assets-root ../usd-assets --out
 /tmp/usd-assets-validation` (write the command on one line), with GPU device
access for `vkr`/EGL. The JSON supports uniform, seeded random and explicit USD
time-code samples, with per-scene overrides. See
[headless validation jobs](../tools/view_control/HEADLESS.md#json-validation-jobs-assets-animation-times-and-shading-aovs)
for AOV spaces, CPU versus GPU capture, material diagnostic limits and gates.
Every sampled time renders both tools at the same resolved camera and exact time.
Failures and unsupported captures remain explicit in the manifest; the command
returns nonzero unless all requested comparisons pass.

For unlit assets, the default beauty contract is `reference_camera_light: true`
in the job: `lusdrender` ignores authored lights and uses the same
camera-attached constant-attenuation light as `UsdAppUtilsFrameRecorder`.
[jobs/usd-assets-beauty-refcam.json](../tools/view_control/jobs/usd-assets-beauty-refcam.json)
passes 8/9 frames (RiggedSimple 0.9879, mid-pose 0.9859, RiggedFigure
0.981-0.984, TextureCoordinateTest 0.9575, NormalsTextureBiasAndScale 0.9652;
RoughnessTest 0.0206/0.8896 after the UsdPreviewSurface
transparent-opacity, Smith-G2, energy-compensation and preview-F0 fixes; its
MAE passes and the remaining SSIM gap is Storm's alpha/opacity composition). `beauty_lighting`
(`{"key":1.0,"fill":0.25}`) remains as an explicitly named camera-light
approximation for the older wrapper runs. See
[the default-contract report](validation/beauty-default-contract-2026-11-04.md).

The [Caldera AMD measurement](validation/caldera-amd-2026-11-02.md) records the
original geometry failure separately from temporary pose-baked diagnostic controls.
The [USD-WG sampled-time/AOV report](validation/usd-assets-amd-2026-11-02.md)
includes the versioned job config and portable per-frame metrics.

### Independent physics-property reload checks

[The Menagerie report](validation/menagerie-2026-11-02.md) records source-compiler,
semantic, loaded-field/geometry and renderer gates separately. Use
`tools/view_control/menagerie_validate.py` with the current installed LightUSD
Python package and MuJoCo; it retains three serialized formats and invokes a
separate SDK interpreter for independent loading. See
[the headless commands](../tools/view_control/HEADLESS.md#menagerie-conversion-and-independent-physics-loading).

The small `tests/usda/physics-load-compare-{base,root}.usda` fixtures exercise
references, variants, units, drive targets and midpoint property/world-transform
evaluation. `test_physics_compare.py` supplies negative mutation/geometry-digest
checks; `test_physics_snapshot.py` runs in SDK and native Python environments.
The C++ `test_validation` includes valid linear/angular drive instances and
invalid/distance drive cases while retaining distance limits.

[ALab](validation/alab-2026-11-02.md) and
[Island](validation/island-2026-11-02.md) reports preserve capture failures and
separately identify derived controls/subsets; those are not full-scene passes.

The PCP regression suite also checks inherited value clips on descendants with
no local child spec: clips must beat weaker reference defaults, preserve stronger
local defaults, retain their authoring-layer asset anchor, and leave DefaultTime
queries distinct from numeric sampling. Renderer diagnostics must evaluate points
at the same numeric time as topology; a successful render alone does not certify
image agreement or shading.

`tool-lusdrender-capture-normals` verifies distinct raw geometric and interpolated
normal channels on an instanced plane with nonuniform scale against analytic
values. Tydra's extraction regression also checks indexed faceVarying UVs when
point and corner counts are equal, plus uniform normals and scratch lifetimes.

Dataset gap diagnostics are tracked in
[the implementation ledger](validation/gap-closure-status.md). The bounded
physics runner `tools/view_control/dynamics.py` checks analytic motion and real
post-impact contact from source MJCF and converted USD. Failed/unsupported
consumer behavior is retained and cannot count as source fidelity. Its pure
harness checks run with `python3 -m unittest discover -s tools/view_control`.
The asset-free SDK articulation repro is
`tests/verification/repros/openusd-single-body-articulation.py` and runs with
an OpenUSD-enabled Python interpreter. It deliberately fails on the unpatched
single-body parser behavior.

### Raw AMD geometry and bounded instancing checks

The `tool-lusdrender-vk-data-aovs`, `tool-lusdrender-vk-instanced-curves` and
`tool-lusdrender-vk-blas-batches` tests exercise raw depth/geometric/shading normals,
nonuniform/reflected transforms, `primvars:normals` priority, nested curve
instances, zero-width taper spans, evaluated cubic ribbons and nearest-hit merges
across BLAS batches and TLAS slices. The batch test also compares one-triangle
flat chunks against the instanced path within 1e-6, guarding the normal-array
append offset. Hardware results must identify the actual Vulkan device; sandbox
llvmpipe runs do not establish AMD coverage. See the tests' registered names in
`ctest --test-dir build_ninja -N -R 'lusdrender.*vk'` for the current build.

Original-source AOV jobs and retained failures are indexed in
[the gap closure ledger](validation/gap-closure-status.md). They distinguish
opaque geometry, authored-normal skinning, material channels and beauty. Do not
accept missing/invalid SDK normal coverage, unsupported channels or consumer
omissions as a full dataset pass.

`tool-lusdrender-texture-value-modifiers` independently checks two materials
sharing a one-pixel texture with different RGB scale/bias, in raw and sRGB source
spaces, through both material resolvers. The expected pixels come from the
analytic transfer function, not an SDK screenshot. The GPU variant runs the same
contract through Vulkan; force `-gpuTriangleChunk 1` to test preservation of
material IDs and UVs across chunks.

Material-channel validation uses `-diagnosticEmission`: unlit emission with
standard display sRGB and no photographic tone map. This isolates the source
channel from BRDF/light differences. CPU and flat Vulkan are calibrated;
unsupported native backends report missing channel coverage explicitly. Beauty
continues to use its normal rendering model.

The roughness/metallic dataset gates additionally render the original native
scene with `-diagnosticMaterial roughness|metallic`. A passing shared emissive
wrapper alone cannot establish that the native material field is correct.
`tool-lusdrender-diagnostic-material` and its GPU counterpart use distinct RGB
and alpha channels, scale/bias, shared materials and UV1, with independent raw
and sRGB expectations. RGB scalar reads honor authored color space while alpha
stays linear. The sRGB case fails before the scalar sampler/resolver fix.

GPU diagnostic regressions report a CTest skip only for explicit Vulkan engine
startup codes `NOT_BUILT` (4) or `UNSUPPORTED` (8). Other renderer failures,
allocation failures, crashes and validation errors remain failures. The shared
classifier is tested by `tool-lusdrender-gpu-test-status`, including negative
controls for out-of-memory and device-loss messages. Headless controller tests
also run without Pillow; image decoding/comparison imports it on demand.

`tool-lusdrender-diagnostic-opacity-chunks-gpu` forces one triangle per Vulkan
chunk and checks two-layer compositing against independent math. Continuation
rebuilds one GPU chunk at a time and preserves nearest-hit global IDs and texture
footprints. Diagnostic-only indexed positions stay on the CPU until shading ends.
The CPU-chunks counterpart checks transmission rays retain their BVH chunk list.
Both reject a 20-layer diagnostic with no partial output image; the diagnostic
limit is 16 layers. Ordinary preview recursion keeps its existing limit.
The same fixture puts the back layer beyond an authored far clip: transmission
must reduce its remaining ray range after each surface, on CPU and Vulkan.

Dataset gap acceptance is available through `tools/view_control/control.py verify`.
The [session ledger](validation/gap-closure-status.md) links actual hashed evidence,
fixed thresholds, independent physics contracts and outstanding failures. Native
instance handles have C/Python traversal and generation tests. MJCF Plane texture
domains and varying-texture indexed/sequential GPU chunk copies have independent
regressions; a constant one-pixel texture cannot detect missing UV coordinates.

### Large-scene dependency failures and clip boundaries

`test_load.py` exercises decoded-array limits separately from byte/structural
limits through files, buffers and referenced crates. Negative, overflowing and
nonintegral limits must be rejected before loading. Strict PCP builds and
untrusted document sessions fail on rejected/unreadable references; trusted
compatibility sessions can still explicitly request partial composition.
`test_attribute_resolution.py` checks linear normal/scalar interpolation across
one-sample clip files without `interpolateMissingClipValues`, plus held mode.
`test_connection_to_clip_keeps_lazy_file_loader` requires the C API's stage-level
value-clip stage cache to revalidate each cached clip against its file stamp, so a
rewritten `clip.usda` is observed by the next resolve; the AOUSD conformance test
(`core_aousd_conformance`) separately requires a caller-owned
`ValueClipStageCache` to keep reusing stages across queries (`clip_load_count ==
1`), which is why file-stamp revalidation is an opt-in cache property rather than
unconditional. `TestConcurrentValueClipCache` runs eight threads against one
shared cache and `AttributeEval`: all threads must resolve the same value, and a
warmed cache must not load the clip again.
`test_p0_stage_prim.py::test_flattened_instance_members_keep_their_subtree`
requires a default (holder-flattened) load to expose the same mesh set as a
native-instance load for shared and nested instances; the extracted-prototype
writer representation keeps its deliberately orphaned members
(`core_pcp`).
The small `tests/usda/pcp-ancestor-variant` fixtures check ancestor variant
strength against direct child reference/payload/inherit arcs and a weaker
referenced ancestor, in serial/parallel and expanded/native-instance modes.

`lusdrender` CLI robustness additionally rejects malformed/nonfinite memory
budgets and invalid `-maxArrayElements` values, and fails on missing referenced
geometry. Dataset jobs retain explicit `native_load_limits` in their replay JSON
and renderer commands. Complete-scene acceptance requires successful dependency
loading as well as screenshot coverage and resource budgets.

The CPU reference renderer now applies tangent-space normal maps. Its oracle
(`lusdview-cpu-normal-map`) renders a plane whose MaterialX `normalmap` decodes
to `(0.6, 0, 0.8)` next to the same world normal authored as geometry normals;
the two images must be byte-identical, a flat map must be neutral, and a
mirrored-UV pair must match its flipped-bitangent control. The
transform itself has an analytic unit test (`lusdview_shading_normal_test`)
covering sheared UVs, degenerate parametrizations and malformed vectors. A third
check (`lusdview-cpu-graph-tangent`) proves the MaterialX `tangent` and
`bitangent` graph nodes return that same frame: `tangent * (1,0,0)` and
`bitangent * (0,0,-1)` must shade byte-identically to their constant controls.
The Tydra converter also carries unit checks that (a) a texture asset
forwarded through a referenced material interface (`inputs:file.connect =
</Mat.inputs:surfaceMap>`) resolves against the layer that authors the value,
not the referenced look layer (`TestForwardedTextureAssetAnchor`), and (b) a
material exposing only the RenderMan `outputs:ri:surface` terminal maps its
PxrDisney inputs onto the OpenPBR preview while a universal `outputs:surface`
terminal still wins when both exist (`TestPxrDisneyRiSurfaceMaterial`, see
[the mapping](validation/island-pxrdisney-2026-11-04.md)).

`lusdview-cpu-uv-transform` covers the packed per-slot UV transform and UV-set
selector for the lanes the CPU samples (base colour, normal map including its
tangent frame, opacity): each must shade byte-identically to the same texture
sampled with pre-transformed `st`, with a no-normal control proving the normal
rows are not vacuous.

`lusdview-cpu-graph-chain` pins MaterialX graph *topology* rather than a single
route: a three-node chain (`ND_image_color3` -> `ND_multiply_color3` ->
`ND_mix_color3`) feeds `emission_color`, and the render must match, to within one
channel step, a scene that authors the computed constant directly (white texture
x 0.5, mixed 50/50 with 0.2 = 0.35). A white texture keeps the oracle
independent of colour-space decoding, and an image-only control must differ, so
a renderer that stops after the first node cannot pass.

`lusdview-cpu-lane-uv-transform` extends the same contract to the non-base
semantic lanes the CPU preview shades (metallic, roughness, occlusion,
clearcoat, emissive): each lane must match the pre-transformed-`st` reference to
within one channel step and must differ from its constant-lane control, so a
lane that is silently ignored cannot pass. The roughness lane only became
observable after the packed specular colour was neutralised for the metallic
workflow, which had been zeroing the entire specular lobe (see the
[gap-closure ledger](validation/gap-closure-status.md) and the
[lane analysis](validation/cpu-preview-lanes-and-ptex-2026-11-04.md)).

`tool-lusdrender-vkr-ptex-openptex` checks both Vulkan Ptex routes against
OpenPtex: the descriptor atlas path and forced mesh chunks with lazy CPU
sampling. Chunked sampling selects per-face mips from ray differentials;
only currently visible Ptex triangles retain positions for this calculation.
`-ptexProbe` records face, mip, intrinsic UV and linear RGB for the
independent verifier. `-noPtex` disables Ptex loading. Descriptor atlas
face edges honor `-texMaxSize` up to 512 texels. The test requires the
optional Island corpus, OpenPtex >=2.4 sources and a working Vulkan ray-query device.
Instanced/streamed paths and non-base Ptex slots remain unsupported.

`lusdview-cpu-ptex-element` renders the real `isGardeniaA` bonsai element
(quad topology, 53 meshes, 53 PTex maps), requires no representative-face
fallback, and verifies every visible base-colour sample with a probe dump
against OpenPtex 2.4.3 (`pixels=471 textures=11 faces=166 max_error=0`). A
second 16x16 minified render makes the filter footprint span face boundaries
and requires the independently decoded neighbouring-face blend
(`pixels=29 faces=28 cross_face_taps=29 max_error=0`); the test fails if no tap
crosses an edge. The loader fix behind it also computes per-face source ids
without a Uniform attribute and maps subdivided children into their quadtree
sub-rectangle of the base Ptex face.

The CPU preview's PTex path now streams per-face pages and selects a mip from
the pixel footprint. `lusdview_ptex_atlas_test` compares face 0 mips 1-2 against
OpenPtex 2.4.3 raw texels byte-for-byte (fixtures and provenance in
`tests/usdc/ptex-mip/`), checks mip selection and a byte-bounded eviction, and
`lusdview-cpu-ptex-routing` requires a minified quad to report non-base mip
samples under `LIGHTUSD_CPU_PTEX_STATS=1`.

`lusdview-cpu-authored-tangent` authors a tangent basis rotated 90 degrees from
the triangle-derived frame and requires the same tangent-space normal to shade
like its equivalent geometry normal, while the derived-frame render differs;
a flat map stays neutral. `lusdview_shading_normal_test` covers authored-frame
projection, flipped handedness and rejection of a degenerate tangent.

`lusdview-cpu-coat-normal` isolates the OpenPBR coat lobe (black base and
specular weight zero) and renders a plane with a coat normal map. A flat coat
map must be neutral, a map decoding to `(0.6, 0, 0.8)` must shade
byte-identically to the same world normal authored as geometry normals, the
`+X` and `-X` maps must differ, and both maps must be inert at coat weight
zero. The lane uses `matTex` slot 11 with its own packed UV set/transform and
scale/bias and a tangent frame rebuilt from that lane's UVs; disabling it makes
the `+X`/`-X` pair identical (0) and the geometry control diverge (53/255).
None of these replaces calibrated beauty illumination or SDK material parity.

The emissive diagnostic opacity regression now covers PreviewSurface
`transparent` (full emission), `presence` (attenuated emission), and masked
cutouts, with custom UV sets, shared material caches and alpha scale/bias.
CPU and AMD Vulkan results are compared with analytic two-layer math.
Referenced-package texture equivalence also covers an empty USDZ root with a
nested sublayer and relative texture path; its shell harness propagates pixel
assertion failures. The SDK material wrapper's optional Pillow scalar adapter
has independent r/g/b/a channel and parameter-preservation tests in
`tools/view_control/test_validation_stage.py`.

`test_pcp` also checks the high-level composed loader's explicit thread budget:
a2048-payload fixture records callback thread IDs and requires both0 and1 to
remain on the caller thread. Previously1 silently retained the automatic
hardware worker count. Negative remains automatic; larger positive counts are
bounded by the execution policy.

Python default traversal prunes ancestors before visiting children,so it uses
the local default-predicate C query once per prim instead of rewalking all
ancestors at every descendant. The effective public predicate remains
available for arbitrary handles. Default-predicate traversal tests cover
inactive,over/class and instance-proxy ancestry. Load benchmarks now record
explicit native thread,input-policy and array limits and fingerprint the
frozen Python package as well as its extension/library. Trusted/mmap profiles
must be reported separately from the unchanged untrusted default.

The internal `core_c_api_lazy_view` regression requires numeric arrays to
remain undecoded during wrong-type probes and aliases validated uncompressed
POD storage through a C buffer pin. A counting source verifies zero decodes,
exact source-pointer identity and source retention after value destruction.
Compressed and misaligned inputs fall back to decoding; half storage retains
its existing float widening. Python lifetime tests cover an actual USDC view
across property replacement,stage close,owner collection and source unlink
under both input policies. Borrowing never changes the untrusted snapshot
policy or the decoded-array count ceiling.

### Resumed dataset verification, 2026-11-03

The [resumed report](validation/session-resumed-2026-11-03.md) records the final
276-test native sweep, separate Python/core/AOUSD/WASM checks and AMD captures.
[USD-assets sampled diagnostics](validation/usd-assets-native-parameters-2026-11-03.md)
now require actual native roughness/metallic field checks in addition to shared
emissive adapters. [Island subset masks](validation/island-subset-population-2026-11-03.md)
retain relationship/connection dependencies without modifying source layers.
Reference completeness checks reject known shader/GL/resource loss even after
exit zero; lighting truncation affects beauty but not independent unlit AOVs.
The [normal-AOV control](validation/normal-aov-contract-2026-11-03.md) prevents
unperturbed mesh normal agreement from being reported as normal-map validation.
Failed and unsupported catalogue contracts remain open in the hashed evidence.

The [OpenUSD staging-buffer control](validation/openusd-staging-buffer-2026-11-03.md)
independently reproduces the AMD/Mesa 4 GiB boundary. Its candidate SDK patch is
validated through an isolated preload, including 300 MiB byte-exact transfers,
overlapping write order and zero-capacity fallback. It is not installed or a
complete SDK regression pass; missing VDB/shader capabilities remain open.

### Explicit host capabilities and builds without native I/O

`LIGHTUSD_NATIVE_IO` defaults to `ON`. Configure with
`-DLIGHTUSD_NATIVE_IO=OFF` to disable built-in filesystem readers, writers,
metadata queries, mapping, temporary spill/snapshot storage, preview caches,
and native host services. File entry points report an unsupported/disabled
error before opening their paths. Memory parsing, memory serialization,
application-owned output streams, registered memory assets, and custom asset
readers remain available. This is a library backend policy, not an OS sandbox:
application callbacks and standard runtime allocation/synchronization retain
their own platform behavior.

`LIGHTUSD_NATIVE_RESOLVER_IO=OFF` disables only the resolver's filesystem
backend. `LIGHTUSD_NATIVE_HOST_SERVICES=OFF` disables only the optional native
host-provider factory. Neither narrower option replaces `LIGHTUSD_NATIVE_IO`.
The exported CMake library targets propagate the no-I/O compile definitions.

Default memory-oriented operations do not discover the current directory,
query process/system memory, invoke allocator/page hints, or discover GPU
libraries. Applications can pass `HostServices` from `core/host-services.hh`
with only the callbacks they authorize. Callbacks must support concurrent
calls and must not throw. `CreateNativeHostServices()` explicitly supplies
native callbacks; creating it performs no probing. Its library loader requires
absolute filenames unless `NativeHostServicesOptions::allow_library_search`
is explicitly enabled by the application.

Resolvers use lexical `.` when no working directory is supplied. Set
`ResolverConfig::working_directory` explicitly, or supply its `host_services`
for native directory lookup. Set `allow_filesystem=false` (C API:
`lightusd_asset_resolver_set_filesystem_access`) for callback/memory-only asset
access. This policy applies to root layers, typed C loads, and dependencies.
GLB export without a resolver does not read texture paths; use
`lightusd_render_export_glb_with_resolver` or the C++ resolver overload to
provide asset access explicitly.

Preview GPU compute requires `PreviewComputeOptions::host_services` and
explicit `vulkan_library` or `cuda_driver_library`/`cuda_compiler_library`
filenames. Without them Auto uses CPU. CLI equivalents are
`--preview-native-host`, `--preview-vulkan-library FILE`,
`--preview-cuda-driver-library FILE`, and
`--preview-cuda-compiler-library FILE`. The preview qualification scripts
accept these library arguments too. Unknown GPU capacity uses a 2 GiB preview
budget; applications can supply a measured capacity explicitly. Memory-budget
observations, page-fault counters, and allocator/page hints likewise require an
explicit provider.

Focused no-I/O verification (the ordinary disk-fixture tests require native
I/O and should run in the default build):

```sh
cmake -S src -B build_ninja/no-native-io -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_NATIVE_IO=OFF
cmake --build build_ninja/no-native-io --target test_host_services test_host_services_c
scripts/xctest.sh ctest --test-dir build_ninja/no-native-io \
  -R 'core_host_services' --output-on-failure
```

## Legacy tests (compiled, not run)

Since 1.1 the legacy implementation (`src/legacy/`, `-DLIGHTUSD_BUILD_LEGACY=ON`)
is built but registers no CTest cases. Add `-DLIGHTUSD_WITH_LEGACY_TYDRA=ON`
to compile the legacy scene converter and its test binaries. The legacy test programs live under
`tests/legacy/` and still compile. Core replacements for the legacy-only
shell drivers are `core_scope_imageable_roundtrip`, `core_variant_chain` and
(with a pinned OpenUSD usdcat) `core_pxr_crate_fidelity`.

The canonical asset-resolution and variant-listing example sources now use core
APIs; the old `*-next.cc` duplicates and their legacy example registrations have
been removed. `core_asset_resolution_example` and `core_variant_lister_smoke`
exercise the public example entry points. The OpenChess tracing worker loads
`loadFeature('raytrace')` from the core distribution. Demo production builds copy
all optional feature JS/WASM pairs, and shared composition/export demo loading
uses the core path. `web/js/tests/core-features.test.mjs` checks feature loading,
independent heaps, ray tracing, and Worker transfers. The former backend comparison
page now compares the source with a USDC-rooted USDZ roundtrip. After building
`web/demo` with Vite, run `cd web/js && npm run test:demo-core` to check
all 27 production demo entry points and the landing-page links in headless
Chromium. Coverage includes 12 shared-foundation pages with offline USDA,
USDC and USDZ fixtures; the custom animation, composition/streaming, inspector,
material, viewer, diff and packager pages; actual MuJoCo simulation and
validation; and the production OpenChess raytrace Worker. Packager checks
exercise all three download formats. `--custom-only` and `--page=<name>` allow
focused custom-page runs. The URDF converter source and example CMake entry point also use core
APIs exclusively; `--format all` exercises USDA, USDC, and USDZ output.

`core_test_geometry_optimize` checks mesh merging with nonuniform/mirrored
transforms, inverse-transpose normals, indexed face-varying continuity,
element-sized and uint2 primvars, subdivision creases/corners/holes, material
subsets, size limits, transactional budget failure, and Crate output without
reviving removed meshes. `core_lusdzconvert_geometry` exercises flat USDA/USDC
and both USDZ root formats. `web/js/tests/core-converter-geometry.test.mjs` checks
the shared WASM exporter, source-stage isolation, and numeric option validation.

`core_test_asset_rebase` verifies source-layer-relative asset defaults and time
samples, inline variants, shared-content clone isolation, and material dedupe
that distinguishes equal filenames anchored to different directories. The
resumable WASM flatten path rebases source values before merging opinions, so a
weak layer's texture remains anchored correctly when a stronger layer overrides
another field on the same shader.

`web/js/tests/core-converter-orchestration.test.mjs` exercises the public folder
and streaming converter APIs with sublayers/references, variant overrides,
source-relative texture paths and animated asset paths, both root formats,
material dedupe, mesh merging, append-only ZIP sinks, missing dependencies, and
input limits. Core conversion now flattens by default; `flatten: false` retains
single-root authoring unless optimization or variant overrides require a
composed stage. The single-USDZ passthrough shortcut excludes these options so it
cannot silently ignore requested optimization. Texture-free scenes accept the
default CLI/UI image settings. Converted USDZ inputs retain their embedded assets
with `repackUsdz: false`, and custom asset processors disable passthrough.

`core_lusdzconvert_images` covers ordinary uint16 PNG resize and JPEG asset
remapping. The shared image engine also powers counted core WASM conversion,
channel packing and size/quality fitting. `core-image-convert.test.mjs` covers
bounded scanline fallback, precision, colorspaces, optional EXR and HDR output;
`core-converter-textures.test.mjs` checks folder/streaming remaps and name
collisions for both root formats. Format-only configured exports use the direct
writer without imposing the remap/optimization clone's working-set budget.

The MCP embedding example now uses `lightusd::mcp::Server` with stdio and
HTTP transports. `core_mcp_example` checks both entry points, notifications,
invalid CLI ports and bounded request bodies on loopback. Build it with
`-DLIGHTUSD_WITH_MCP_SERVER=ON -DLIGHTUSD_BUILD_EXAMPLES=ON`; the standalone
`lightusd-mcp` executable and `core_test_mcp` use the same core server library.

Lucia now separates core MCP authoring from the core RenderStream preview
adapter. Loose/embedded assets are packaged with the preview root, validation
uses a core LayerDocument, USDZ export uses the core converter, and channel
packing uses the counted image API. Its fidelity tests require Node 23+ when
running both WASM32 and WASM64. Geometry-node assertions locate prim blocks
instead of treating braces in quoted graph JSON as the end of a prim.
The xatlas worker loads the core geometry feature; `core-xatlas.test.mjs` checks
UV generation, options, source-vertex maps, output ownership and worker remapping.
Generated primitives now extract authored primvars before triangulation.
`core-generated-colors.test.mjs` checks constant/indexed-uniform display colors,
opacity, analytic-output welding at attribute discontinuities, and the shared
Three.js preview. Lucia builds through that shared core scene builder, including
material conversion and archive textures. Its full headless browser gate covers
UDIM, concurrency, sculpting, geometry nodes, persistence and a 131,072-triangle
grid (with an explicit successful-import assertion).
The standalone USD export demo uses the core converter; the validation worker
uses the independent core validator for every URL/backend selection.
`standalone-core-demos.test.mjs` checks browser downloads and worker validation.
The high-level loader's progress/async methods also use the core adapter.
`core-loader-progress.test.mjs` checks phase completion, cancellation, memory
limits, callback failures, concurrent loads and native-owner cleanup. Native
progress callbacks are installed only across the synchronous native call.
The `test:progress` CLI uses the same adapter and requires real phase callbacks,
100% completion and the expected mesh counts on its three generated scenes.
The MaterialX GL inspector uses the core adapter, preserves structured parameter
metadata during edits, and rebuilds physical/OpenPBR materials while retaining
textures and source color transforms. The production standalone gate also checks
its custom shader compilation. `core-texture-bindings.test.mjs` checks pending
textures after material cloning/replacement and custom OpenPBR color hooks.
The MaterialX WebGPU and debug inspectors now retain core RenderStreams. Their
shared inspector imports packaged assets into a counted core store, and named
root/cached/streamed inputs retain authoring-layer anchors. A matching anchored
memory asset wins over a same-name root-level key. `core-render-inspector.test.mjs`
checks this on WASM32/64, including owned decoded bytes after stream deletion and
invalid source-name records; the native Tydra test checks the same collision.
`core-image-texture.test.mjs` checks RGB expansion, integer normalization,
Float16/Float32 HDR data and owned storage. The shared texture helpers use actual
image bytes rather than requiring the removed legacy buffer IDs.
`core-materialx-demos.test.mjs` loads textured USDZ input into both inspectors,
checks Z-up conversion and exercises both WebGPU shader modes. Its Chromium
software-Vulkan setup avoids incompatible headless swapchain backends. The
`web/demo` smoke gate alone does not cover these standalone inspectors.
The UDIM demo also uses core RenderStream and an imported asset store, with
owned geometry copies and root-relative tile resolution. Sparse export preserves
the original layer and tile bytes. The standalone browser regression checks both
sparse and combined modes and all four exported tiles; `MUJOCO_WASM_DIR` also
enables the browser physics demo's real MuJoCo startup check.
The streaming viewer now uses core RenderStream for both load paths, including
loose USDA and nested USDZ roots. Its material fast path resolves authoring-layer
texture anchors; image payloads stay off the WASM heap. The browser regression
`core-streaming-demo.test.mjs` checks production/development rendering, a
same-name texture decoy, world bounds, non-uniform normal matrices, native handle
cleanup and GL buffer deletion across successful and failed loads.
The HTTP resolver, streamed loading utility, UDIM demo and development inspection
entry points now use core APIs. The JavaScript USD CLI uses core composition,
retains arcs with `--no-flatten`, and preserves nested USDZ roots and texture
anchors. Its tests run in the Node regression profile alongside core asset-store
allocation/readback/release diagnostics and the USD Physics/URDF roundtrip CLI. The high-level loader now imports the package asset store and passes the root
identifier through the named render entry point. Nested-root and referenced-layer
texture regressions include an identically named decoy at the archive root.
Sparse UDIM pattern anchoring still needs equivalent cross-layer coverage.

The browser physics simulation and physics/USD-to-MJCF CLIs use the core converter's physics
extraction; render geometry comes from the core adapter. OBJ vertices are baked
into the owning rigid body's frame, including intermediate transforms and
mirrored winding. `core-physics-cli.test.mjs` checks loose USDA and nested USDZ
inputs, real OBJ output and independent physics extraction with malformed
render-only UVs. The `physics` regression profile also runs the robot-arm
simulation against MuJoCo.

The `large-scene-load` example now uses core StageSession snapshots and
payload edits. `core_large_scene_example` exercises deferred/eager loading,
byte/extent policies, load/unload, USDC roots, relative paths and invalid CLI
arguments. `--no-dedup` now releases parsed-layer/composition caches between
edits; source layers still share storage within each composition. Memoized
payload-policy decisions keep byte/extent admissions stable across those
recompositions. The diagnostic reports source-layer count rather than claiming
a cumulative parser-invocation count.

The variant analyzer now reads core authored variants directly, with real nested
depth, overflow-safe capped combinations and JSON escaping. `core_variant_analyzer`
checks empty reports, nesting, cap behavior and USDA/USDC parity. `tydra_api`
uses core scene access, UsdShade and attribute evaluation; `core_tydra_api_example`
checks external references, parent paths, inherited bindings and shader inputs.

The optional `js-script` example embeds a stage-independent QuickJS runtime and
reads core authored layer metadata. `core_js_script_example` checks Unicode,
sublayer offsets, root prim names, JavaScript errors and execution limits for
both loops and Promise jobs. Enable it with `LIGHTUSD_WITH_QJS=ON`; the same
runtime supplies `lusdrender` scripting without linking the legacy library.
`tool-lusdrender-qjs` checks scene statistics, bounds, scripted resolution,
PNG output and script-error exit status on that core renderer path.

`usd-texcomp` now authors companions through core Stage/layer APIs and the
shared image codec library. `core_usd_texcomp_example` verifies USDA/USDC input,
existing customData preservation, mip count, optional Zstd supercompression,
HDR BC6H output, exported layer reload and missing-image failure. It requires
Tydra and textools, with no legacy library dependency.

`progressive_composition` now uses the core loader, resolver and compositor.
Its cumulative LIVRPS passes recompose from the authored root. Selective passes
preserve disabled arc values and list-op qualifiers, covered by
`core_test_composition`; `core_progressive_composition_example` checks all six
controls, later reload, variants, relative mesh paths, missing dependencies and
USDA/USDC/nested USDZ inputs. Default compositor flattening remains unchanged.

The render-scene example now uses core loading/composition, retained conversion,
JSON/YAML diagnostics, animation sample inspection, image decoding and core
OBJ/USDA exporters. It is registered with the core examples, independently of
legacy. Its default index builder preserves attribute seams, skin weights, morphs
and material subsets. `--fast-index-build` expands corners without deduplication;
`--noidxbuild` retains separate attribute streams. `core_mesh_index` checks these
modes, polygon topology, tangent winding, indexed primvars, memory limits and
invalid input. Deduplication keeps distinct source points separate; triangulated
output receives canonical triangle face counts and corner mappings.
`usda_parser` and `usdc_parser` now use format-specific core readers and share
core CLI/composition handling. They build with examples enabled or their existing
`LIGHTUSD_WITH_TOOL_USDA_PARSER` / `LIGHTUSD_WITH_TOOL_USDC_PARSER` flags,
independently of legacy reader module flags. `core_usda_parser_example` and
`core_usdc_parser_example` cover all six arc controls, retained disabled list
edits, output reload, format rejection, malformed input and CLI errors.
`triangulation_method_example` now builds as a core Tydra example.
`core_triangulation_example` checks Earcut and fan modes, original polygon
statistics, triangle totals, external mesh references and invalid arguments.
`usd_to_json` now uses the core archive reader and shared
`WriteLayerToJSON` API. The WASM layer exporter delegates to that same core
serializer and shares its typed metadata conversion helpers. `core_usdz_json_example`
checks USDA/USDC scene parity, first-entry root selection in the presence of a
USDC dependency, default runtime schema fields/arrays, an explicit authored
layer JSON mode, exact base64 sidecars (including empty files, Unicode/quoted
names and chunk boundaries), default prefixes and invalid input/output paths.
Public examples, tools, and demo entry points use the core implementation.
The native build, optional viewer/texture benchmark, Node profile, and both
production demo sites are checked independently; legacy is a separate
compile-only product.

Migration follow-ups and coverage are tracked below:

| Area | Gap | Found by |
|---|---|---|
| lusdcat | Core `--explain` uses the public C API property tracer; workflow checks require sampled values, selected-variant provenance and connection chains | `workflow_tools` |
| usd_to_gltf | Core C API example; workflow checks require GLB structure, strict loss refusal, UVs and embedded textures for loose files and USDZ | `workflow_tools` |
| lusdzconvert | `-optimizeMaterials dedupe` is now shared core functionality with restored regression coverage; PreviewSurface graph deduplication now canonicalizes surface shader names and ignores unused shader children, with conservative exact comparison for unsupported graphs; atlas mode generates shared PNG diffuse atlases for compatible static PreviewSurface materials and preserves indexed UVs; static whole-mesh geometry merging is now shared with the core WASM configured-export API; native UDIM grid/dense baking now shares the WASM implementation. High-level core JS composition/variant overrides and dedupe/mesh merging are wired; core image conversion, channel packing, size/quality fitting, folder/streaming texture remapping, and audio processor hooks are wired and covered by `core-image-convert.test.mjs` and `core-converter-textures.test.mjs`; core JS ARKit policy forces composition, a USDC root, Y-up metadata and PNG/JPEG textures; non-flattened dependency packaging preserves archive directories and source bytes | `tests/run-lusdzconvert-regression.sh` |
| lusdview | `lusdview_{lighting,texture_pipeline,geometry_primvar,openpbr_material}_test` exercised the removed legacy RenderScene -> DrawScene converter and were deleted; core-path coverage is the screenshot/backend suites in `examples/lusdview/tests.cmake` | removed with the legacy loader |
| lusdview | Light/camera gizmos now use retained core records and the Render scene hierarchy uses composed draw-record paths. The MCP `vchar_physics` preview now uses public C API physics queries and an independent editable core stage | `lusdview_scene_gizmos_test`, `lusdview_physics_preview_test`, `lusdview-core-physics-preview` |
| lusdview | The opacity and both HIP texture-semantic tests pass with matching host and device texture descriptors. The backend parity test passes in headless mode. | `examples/lusdview/tests.cmake` |
| lusdview | `lusdview-dome-orientation` passes with texture tools enabled; it checks that rotation changes core IBL and loose/USDZ renders agree. Camera/light record checks likewise compare loose input with USDZ instead of the ignored legacy-loader flag | `examples/lusdview/tests.cmake` |

The multi-UV normal-map fixture uses an oblique key light so both tangent
directions affect shading. Its `lusdrender` normal-map assertions are enabled;
an axis-aligned key made the previous tilted/flat control vacuous.

`core_lusdzconvert_ktx2` verifies KTX2-to-PNG conversion, archive contents,
shader asset remapping and reopening the resulting USDZ. It is registered
when `LIGHTUSD_WITH_TEXTOOLS` is enabled (the core default).

`core_reader_parity_sceneLibrary-001` and `core_reader_extension_parity`
compare USDA raw metadata opinions with decoded USDC extension fields. The
negative control edits `sceneName` and requires a metadata differential;
raw spelling and unregistered-field storage remain available for round trips.

Core example migration checks include `core_api_tutorial` (authoring, shader
connections, schema access, indexed UV expansion, time interpolation and USDA
reload), `core_file_format_example` (custom binary assets preloaded as core
layers and composed through references), `core_file_format_missing_root`
(input-error propagation), and `core_c_api_example` (portable C ABI load,
traverse and export). These targets require `LIGHTUSD_BUILD_EXAMPLES=ON`.

USDC readers now expose inline variant metadata/content like the USDA reader;
bracketed holder specs are transient reconstruction details. The
`core_test_variant_inline` API test checks that representation and a second
Crate round trip. `core_reader_variant_parity` covers paired fixtures, selected
composition, nested options, property metadata/time samples and unselected
body-value edits. `variantSet-prop-001.usdc` genuinely lacks the selection in
its USDA namesake (confirmed with OpenUSD); the test requires that real
differential to remain visible.

The scalar/array `uint2` fixture parity cases are covered by
`core_reader_parity_uint2-type-001` and `core_reader_parity_uint2-array-001`.
`core_test_aousd_conformance` checks compatibility acceptance with a warning,
strict rejection, and Crate round-trip fidelity for `varying rel`.

`core_lusdzconvert_udim` verifies native grid/dense atlas pixels, USDA/USDC
package roots and flat outputs, authoring-layer anchors, USDZ input,
non-flattened variant contents, animated-file common layouts, 16-bit PNG and
(with `LIGHTUSD_WITH_EXR`) floating-point EXR round trips. Failure controls
cover tile/memory limits, crossing-face reject/split and precision-losing output
requests; pre-existing atlas-name collisions must not overwrite unrelated files.
The WASM `udim-bake.test.mjs` suite exercises the same extracted core adapter.

The HTTP asset resolver demo uses core `FlattenSession` and the incremental
RenderStream renderer. `core-http-demo.test.mjs` checks remote dependency layers,
texture paths anchored to different authoring directories, collision-free USDZ
packaging, preloaded assets without network requests, and missing-layer errors.
`FlattenSession.setRebaseAssetPaths(true)` preserves asset provenance before
composition even when no remap is supplied; call it after `begin`. URI path
normalization preserves the authority and opaque scheme payloads.

The export CLI uses the core converter and runs directly with Node. The Node
profile includes `core-export-cli.test.mjs`: sample generation, USDA/USDC/USDZ
exports, reimported USDZ output in all formats, and invalid-input errors. The
browser conversion benchmark reloads its generated root through `LayerDocument`.

Core streaming transfer coverage is in `core-stream-transfer.test.mjs` (Node
profile, also runnable with `LIGHTUSD_WASM64=1`). It checks known/unknown HTTP
lengths, overrun/truncation rejection, limits, cancellation, callback failures,
caller-owned streams, chunked files, layer loading and full-progress loading.
Successful `streamFetchToWasm`/`streamFileToWasm` results own a core RenderStream;
call `beginStreamedAsset(assetPath)` to parse and `delete()` when finished. The
completed transfer remains a streaming root buffer until parsed. The examples
and `cli/load-test-node.js` use this API and release both comparison scenes.

The material-dedup demo defaults to core in both main-thread and worker modes;
its GUI offers the shipped backend. `core-material-dedup-demo.test.mjs` compares
renderable sample geometry and material counts across both paths. It also runs
against the production bundle with `--production` after a Vite build.

The experimental OpenPBR `mtlx-webgpu.html` viewer now uses core RenderStream
and owned mesh/image records. `core-openpbr-webgpu-demo.test.mjs` runs it on
headless SwiftShader WebGPU and checks authored transforms, repeated loads,
invalid-input cleanup, exact packaged RGBA texture uploads and GPU validation.
Run with `--production` after building Vite to check the bundled entry. The Node
profile's `core-webgpu-texture.test.mjs` covers grayscale/alpha expansion and HDR
half-float uploads without clipping values above one.

The ShaderBall MaterialX adapter uses core HTTP composition, LayerDocument graph
inspection and RenderStream geometry. `core-shaderball-adapter.test.mjs` checks a
selected variant with references to separate authoring directories, distinct
same-name texture provenance, deferred image fetches, UVs, mesh primvars, material
bindings and camera optics. Its `--production` mode drives the bundled
MaterialX WebGPU page's scene selector using the same fixture. Graph validation
also runs against the core WASM
module (`webgpu-mtlx-usd-graph.mjs`) and retains connected defaults and color-space
metadata. HTTP flatten callers may request `packageAssets:false` to retain the
rebased external asset paths and receive owned flattened `rootBytes`; optional
`variantOverrides` are applied before composition.

`core_obj_export` exercises the core RenderScene OBJ/MTL exporter used by the
render-scene example migration: polygon/triangle topology, hole faces, source
corner UVs, uniform normals, authored subset names, PBR texture references,
normal maps, skin weights, atomic failure, output budgets and invalid indices.
Core conversion now retains GeomSubset names across split runs and triangulation
so diagnostic exports can keep authored group names. Mesh coordinates and scene
up-axis remain unchanged in OBJ output.

`core_usd_export` reparses and reconverts the core RenderScene USDA export. It
checks triangulated source-corner UVs and uniform normals, material subsets,
texture channels and normal-map scale/bias, asset quoting, skeleton bindings,
blend shapes, instance transforms, RGBA alpha multiplied by displayOpacity,
matrix primvars, output budgets and atomic rejection of invalid data. Animation
clips remain an explicit export error, matching the legacy exporter; non-mesh
resources are reported as omitted by this diagnostic mesh-export API.

`core_render_scene_example` covers the migrated native diagnostic CLI: JSON/YAML,
source metadata, automatic/forced/disabled tangents and all tangent methods,
SNorm8 storage, low-memory source release, polygon preservation during normal
computation, evaluated time codes, OBJ/MTL/USDA reload, local and USDZ texture
pixels, USDA/USDC/USDZ loading, references/payload controls, animation samples and
invalid options. The converter's polygon mode now computes normals using a
shared-payload temporary triangulation, leaving authored output topology intact.

The colorspace browser oracle compares the core scene adapter and direct
RenderStream material paths against independently transformed expected pixels.
The separate WASM parity test intentionally loads the combined module as its
legacy reference. The optional Pixar math example uses `math-trig.hh`, with
exact quarter turns and bounded reduction for large finite angles.

The JavaScript production build includes every top-level demo HTML page (the
validation-body fragment and test harness pages are excluded). Its ES2022 target
supports the top-level await already used by demo modules. Local preparation
also builds the independent C11 texture codec demo. The material validator uses
Three.js `colorSpace` for both color and data textures; removed `encoding`
constants no longer bypass normal/roughness/metalness/AO checks.

Retained-layer JS export keeps dependency and image/audio bytes unchanged, matching
the native converter's no-flatten processing policy. Root format conversion
keeps the original root as an alias when its filename changes, preserving
back-references; filename collisions select a fresh output root name. The folder
and streaming tests reopen these packages, compose their external layers, check
texture anchors, verify exact dependency bytes and reject unsafe archive paths.
UDIM baking across retained dependency layers still requires flattening.

The public `examples/lusdcat` entry point configures the canonical core CLI;
historical sources are archived under `src/legacy/examples/lusdcat`. The optional
legacy PCP bridge reuses an existing core target and leaves global CLI/test
options intact instead of adding the core project twice.

Core physics JSON snapshots describe meshes with `pointCount`/`faceCount`; raw
vertex, index, normal, UV, skin and other render arrays belong to RenderStream.
Expanding those arrays twice as JSON made small compressed scenes exhaust the
WASM heap during physics-only conversion. Physics/engine array properties,
transform ordering, bounds, analytic geometry and relationships remain in the
snapshot. The core physics regression includes a large indexed mesh to verify
bounded snapshot size and preserved counts.

The production copy plugin includes complete 32-bit and, when built, 64-bit
core/feature factory pairs (and optional MCP pairs). The production codec and
colorspace regression checks every demo HTML entry and loads the copied 64-bit
core, geometry and validator with independent heaps in capable browsers.
`core-shaderball-adapter.test.mjs --production --hardware` validates the actual
production page, adapter input and packed GPU scene under private Xvfb; run it
through `scripts/xctest.sh`. The hardware flag requires a non-fallback adapter.

The public `LightUSDComposer` helper now consumes caller-owned core
`LayerDocument` objects and uses `FlattenSession` for dependency discovery.
`core-composer.test.mjs` covers nested layer anchors, references, optional
payload loading, preloaded partial composition and resolver errors.
`setCompositionMask` selects sublayers (1), references (2), payloads (4),
inherits (8), specializes (16), and variants (32); disabled arcs stay authored
for subsequent passes. Call it after `begin`; the default mask is 63.

The USD-independent collision/dynamics implementation lives in
`src/tydra/rb-{solver,collision}.cc`; both the core preview and the optional
legacy bridge link `lightusd_rb_solver`. `core_test_rb_solver` ports eleven
numerical/contact/hinge/sleep/capacity checks without running legacy tests.
The full-capacity case exposed an island offset overrun caused by reusing
root-map storage as per-island counts; counts now live in island records.
The solver suite also passes AddressSanitizer and UndefinedBehaviorSanitizer.

`lusdview_physics_preview_test` covers core render-catalog transform retention,
world-space contacts/writeback under a transformed parent, rotation and scale,
immutable source stages, kinematic velocities, compound colliders, joint axes
and angle units, bounded mesh-point scans and reset. `lusdview-core-physics-preview`
drives the real MCP operations and verifies GPU pixel motion/reset, the mesh
world matrix and bounds, hide/status, invalid steps and reset. The preview uses primitive
colliders and conservative mesh bounds; it is a diagnostic solver, not an
engine-exact MuJoCo/PhysX replacement. Unsupported geometry or non-rigid frames
return an explicit error. Mesh bounds scan at most 16 million points using a
fixed scratch buffer. Physics records are limited to 4096 and 64 MiB.

Viewer regression scripts no longer select the ignored `--legacy-load` alias.
Material, lighting, proxy and AOUSD checks run core semantic assertions;
camera clipping, supported material outputs, GeomSubset materials and
texture/displacement tests compare loose files with stored USDZ inputs where
applicable. The static camera shutter control is
`lusdview-camera-motion-raster-static`; RT texture archive shards are named
`lusdview-texture-semantic-rt-package-parity-*`.

The optional `LIGHTUSD_BUILD_TEXTURE_GPU_BENCH=ON` target is restored at the
root and links the core image/Ptex providers explicitly. Its executable is
`build_ninja/lusdview_texture_gpu_bench`; it no longer needs a legacy target.
The Vulkan synthetic smoke runs BC1/3/5/6H/7 and ASTC through two mip levels.
Native TIFF/DNG support (`LIGHTUSD_WITH_TIFF=ON`) now reaches the core image
provider. `core_test_native_tiff` checks raw/deflate RGB8, RGB16 and float HDR,
metadata, JPEG-in-TIFF, tight decode budgets, oversized dimensions and embedded
JPEG frame sizes. TIFF preflight accounts aggregate IFD pixels and strip/tile
scratch before decoding; embedded JPEG dimensions are inspected independently
of the IFD dimensions. The texture benchmark normalizes 16-bit integer LDR
inputs and validates their buffer sizes before uploading.

Final migration verification (2026-11-11): the native Ninja suite registers
276 tests, with zero failures and nine expected optional-fixture/capability
skips. The optional legacy tree compiles without running legacy tests.
The final viewer checks pass physics-preview pixel movement/reset, static
camera control, material-binding inheritance and CPU normal-map shading.
Both WASM widths build; all 27 production website demos plus the landing page
pass, as do production codec/colorspace and independent memory64 feature
loading. The Node regression profile passes 76 checks. Pixar comparison covers
499 USDA and 227 USDC inputs with no unexpected differences; usdchecker parity
compares 499 inputs without failures. The optional texture benchmark passes
all six Vulkan compression formats and native 8/16-bit TIFF inputs (hardware
and software Vulkan smoke runs were checked separately).

Material atlas regressions run through `core_lusdzconvert_materials` and
`web/js/tests/core-converter-materials.test.mjs`. They check shared material
bindings, atlas UVs and loose sidecars, and confirm that an incompatible
repeat-wrapped material retains its texture. The web regression samples PNG
tile and gutter pixels and reloads the folder and streaming packages with
both root formats.

The Ptex atlas fallback test covers both sides of its minimum byte budget.
An 80-byte budget covers the two face-table records and rejects the build.
An 88-byte budget also holds two fallback pixels and verifies virtual
downsampling. The test checks that a build succeeded before indexing its output.
CPU Ptex routing also checks that a texture forwarded through a referenced
material interface resolves at the scene anchor when the inherited look-layer
anchor has no matching file. The viewer enables the public material converter's
default resolver for this lookup.
