# LightUSD refactor checkpoint


## Current continuation (2026-10-18)

The C11 storage / compact-view / independent-WASM-feature refactor is implemented
on top of `abc530f7c`. This request explicitly authorizes ABI breakage and renewed
compile-time/object-size work; the earlier stopping points below are historical.
Native C API major is 5 and core C API major is 2. Python now requires 3.11,
uses the real read-only buffer protocol and retains C11 storage pins. Optional
WASM APIs live on modules returned by `await module.loadFeature(name)`.

See [the implementation report](doc/refactor-c-core.md#c11-storage-compact-views-and-split-features-2026-10-18)
for ownership, ABI migration, measurements and regression results. Core build,
original-artifact and comparison reports are under ignored
`build_ninja/c11-storage/`. No commit or push has been made for this continuation.

- Native clean build wall: 19.44 → 14.81 s; CPU: 252.60 → 203.71 s.
- wasm32 full-distribution wall: 24.17 → 23.25 s; CPU is effectively unchanged.
- Optimized object text/data: native -0.06%, wasm32 -0.40%. Full distributed
  JS/WASM grows 4.65% raw / 5.48% gzip because features have separate runtimes
  and subdivision remains linked into both main and geometry.
- Native optimized, Debug, threaded and ASan/UBSan checks pass. All 890 fixture
  rewrites match baseline. Python abi3/free-threaded: 136 passed, 10 skipped each.
- Follow-up compatibility build passes with `lusdview` (OpenGL/Vulkan), examples,
  tools, C API and standalone MCP enabled. Missed renderer view migrations and
  legacy C API joint fallback calls are fixed. Focused checks: 99 passed, one
  OpenUSD skip; headless capture/CPU rendering pass on software Vulkan. See the
  [consumer build report](doc/refactor-c-core.md#viewer-example-and-tool-build-follow-up).
- Headless hardware follow-up passes on NVIDIA RTX 5060 Ti: three direct Vulkan
  raster/hardware-ray-query/CUDA renders and all eight selected GPU CTests.
  HIP is unavailable and OptiX is disabled. Results and screenshots are under
  ignored `build_ninja/viewer-refactor-check/hardware/`; see the
  [hardware report](doc/refactor-c-core.md#headless-hardware-gpu-follow-up).
- Pinned USD-WG/AOUSD corpora are downloaded under `.cache/lightusd-verification/`.
  All 280 USD files parse in both native products and WASM32/memory64; AOUSD
  native conformance and 83 viewer fixtures through both loaders pass. Python
  corpus checks pass on 3.11/3.14t (16 passed, two optional skips combined).
  The complete 160-case hardware/CPU render profile and 28 animation renders
  pass, retaining warning and `no_renderable` classifications. The WASM corpus
  runner now uses the next `LayerDocument` API. See the
  [corpus report](doc/refactor-c-core.md#pinned-external-corpus-follow-up).
- The complete web matrix passes across final profiles: 57 Node suites per
  width, MuJoCo smoke/simulation and all 67 Menagerie closures per width, five
  regular browser models and all 67 Worker models. Feature contracts, validation
  demos and UDIM browser cases pass. Fresh combined builds repair three missed
  array-view migrations; shared clip fixtures and validator diagnostics are
  fixed. Npm staging and compatibility loading pass at the earlier checkpoint.
- Pinned OpenUSD v26.05 with Python and MaterialX passes checker parity (685
  fixtures), sampled values (304), serialization and the USD-assets ratchet.
  Strict flatten originally reported 15 unlisted differences, all matching the
  saved pre-refactor binary; the strict follow-up below fixes them. A
  single-worker serialization retry passes after
  one intermittent oracle shutdown crash. See the
  [oracle and web report](doc/refactor-c-core.md#openusd-and-complete-web-regression-follow-up).
  The previous full legacy-native gap is closed by the complete run below.
- Broader native NVIDIA/Xvfb coverage finishes with 115 passes, six skips and
  no remaining failures after focused retries. Both GPU-assisted hardware RT
  captures (headless/threaded) and renderer/viewer OpenPBR parity pass. Mesh and
  curve clip points share a compiled fallback, including WASM's lazy mesh path;
  the public mesh converter's clip loader and its opt-out have regression
  coverage. Final Node profiles pass 57/57 per width, and focused browser checks
  pass again. See the
  [broader GPU report](doc/refactor-c-core.md#broader-native-gpu-regression-follow-up).
- Full legacy native CTest now passes (2026-10-19): 361 passed, ten skipped,
  zero failed across 371 registered tests, including all 1,023 legacy unit
  cases. The initial run took 26.5 minutes; two external tool skips pass after
  supplying `USD_ASSETS_ROOT`. All 81 material-resolver renders pass across
  27 assets; resolver differences remain advisory. NVIDIA/Xvfb viewer coverage,
  examples/tools, roundtrips, feature tests, MCP and 685-fixture checker parity
  pass. Skips are five HIP tests, an unavailable external production scene and
  four opt-in profiles. No product source fixes were needed. See the
  [full native report](doc/refactor-c-core.md#full-legacy-native-regression-follow-up-2026-10-19)
  and ignored `build_ninja/legacy-native-full/final-summary.json`.
- Strict next/OpenUSD flatten now passes all 840 inputs: 814 matches, 18
  existing intentional/oracle differences, eight fixture skips, zero failures.
  A shared composition eligibility check fixes all 15 differences without
  changing the comparator, tolerances or expected-failure ledger. Local array
  edits, relationship/connection list ops, explicit arc clears, relocates and
  inactive subtrees now reach composition through public loading and flatten
  paths. Ordinary layers retain their original storage. Focused native tests,
  standalone Debug (58 passes, one optional AOUSD skip), and affected WASM
  checks on both widths pass. HIP is explicitly excluded from this follow-up.
- Parser fuzz smoke passes with ASan, UBSan and leak checking: 10,000
  iterations each for legacy USDA and USDC, seeded by 646 and 232 unique fixture
  inputs. The USDC harness now reconstructs the stage to cover value/array
  decoding. The full legacy sanitizer gate completed on 2026-10-19 (see
  [resume-test.md](resume-test.md)): 361 passes and five justified skips across
  366 non-HIP CTests, with zero project sanitizer reports. It found a pool
  shutdown leak, now fixed by freeing cached blocks. Strict Clang consumer
  build warnings are repaired. Reports are under ignored
  `build_ninja/regression-flatten-sanitize/`.


## Previous continuation (2026-10-16)

The requested seven-area maintainability refactor is implemented: the load /
session facade, layer diff, crate reconstruction, shader extraction, URDF/MJCF
authoring, RenderStream bindings, and layer-document bindings have focused
compiled boundaries. Loader result handling, session memory accounting,
streaming asset normalization and matching bounded string copies are shared.
Public APIs, product defaults and optimization flags are unchanged.

This continuation started from clean commit `7659fa4fc`.
New private headers remain implementation-only and uninstalled. Session Impl
and its synchronization stay together. Crate scheduling, URDF authoring order,
JSON budgets, ownership and distinct copy protocols retain their behavior.

The accepted earlier WASM-size stopping point still applies. The new request
explicitly authorized these TU/binding splits; it did not reopen size tuning,
raw-value-buffer migrations, portable POD bindings, or product-default work.
The completed PCP split described below remains historical context.

See [the continuation report](doc/refactor-c-core.md#continued-maintainability-refactor-2026-10-16)
for boundaries, measurements and limitations. Key results:

- Native affected-TU slowest median: 3.15 → 2.03 s; aggregate isolated compile
  time: 12.20 → 21.34 s. wasm32: 2.47 → 1.99 s and 11.60 → 25.39 s.
  Splitting increases aggregate compilation work; no clean-build win is claimed.
- Current next-only raw WASM: 1,954,631 bytes (wasm32), 2,174,316 bytes
  (memory64), measured with Emscripten 5.0.1. The older PCP table used 4.0.14
  and is not directly comparable. Compressed size grows slightly against the
  matching archived baseline. This is not a renewed size target.
- Root native CTest: 113 passed, two optional skips. Standalone Debug,
  threaded Debug, and optimized threaded ASan/UBSan without PCH: 57 passed,
  one optional skip each. Sanitizers disable leak detection in the sandbox
  and GCC vptr checks for RTTI-disabled libraries.
- Next and legacy Debug builds also pass with examples and tools enabled
  and the GUI viewer disabled. Additional legacy checks report 154 passed,
  four skipped and two failures; the unit suite was stopped after ten minutes.
  The full legacy gate remains incomplete; details are in the continuation report.
- Sixteen standalone header syntax checks pass; 705 fixture rewrites retain
  identical exit status, stdout and stderr. No OpenUSD usdcat is available.
- Next-only and combined wasm32/memory64 builds pass. The new raw C-boundary
  copy/name test passes on both baseline and refactored modules at both widths.
- Node profiles pass 49 suites with seven known baseline failures per width.
  The full web gate remains incomplete because these failures and missing
  Menagerie data remain. Both validation-app browser checks and eight UDIM
  main-thread/Worker cases pass after providing Chrome.
- Alternating generated-mesh and 1,000-link robot runtime probes preserve
  geometry hashes and byte-identical URDF USDA output. They do not establish
  comprehensive runtime parity.

Reports and the original source snapshot are under ignored
`build_ninja/continued-refactor/`. The no-PCH check also repaired missing
includes in the earlier USDA-printer and schema-predicate headers; do not
remove those explicit dependencies. Generated validator/EXR JS glue is ignored.

## Historical accepted stopping point

The user accepted approximately **1.6 MB raw WASM** and requested that TU and
WASM-size optimization stop after finishing the PCP TU refactor. That refactor
is finished. The earlier <512 KiB goal remains superseded. The measurements
and verification below describe that older checkpoint, not the current build.

## Finished PCP refactor

Private implementation types live in `src/next/pcp/cache-internal.hh`.
Non-template compiled boundaries separate:

- `cache.cc`: public forwarding API and remaining coordination.
- `cache-arcs.cc`: arc and variant expansion.
- `cache-relocates.cc`: relocation and ancestral source resolution.
- `cache-layers.cc`: layer-stack construction and expression fingerprints.
- `cache-opinions.cc`: strong-to-weak opinion composition.
- `cache-storage.cc`: container construction/destruction, including worker caches.

The existing composition algorithms, ownership and threading behavior are
preserved. The private implementation header must not become a consumer header.
No source-local optimization workaround was added.

## Historical PCP measurements

Emscripten 4.0.14, MinSizeRel, SIMD, wasm32, without LTO:

| Item | Result |
| --- | ---: |
| Raw next + Tydra-next WASM | 1,626,756 bytes |
| Comparable pre-refactor WASM | 1,689,884 bytes |
| Raw size reduction | 63,128 bytes (3.7%) |
| Memory64 WASM | 1,827,671 bytes |
| Original PCP cache TU, isolated pass | 9.00 s |
| Final cache.cc, isolated pass | 6.25 s |
| Final cache-arcs.cc | 5.43 s |
| Final cache-relocates.cc | 4.55 s |
| Final cache-layers.cc | 4.38 s |
| Final cache-opinions.cc | 4.21 s |
| Final cache-storage.cc | 4.13 s |

Final PCP timing is one cache-disabled serial pass, not a multi-run median.
Earlier three-run measurements and the broader production sweep are described
in `doc/refactor-c-core.md`. Splitting reduces the slowest TU but increases
aggregate compilation work and aggregate pre-link object text; do not claim a
whole-product clean-build speedup. The pre-refactor product retained its old
Tydra coordinator's source-local optimization override; the current build does
not. A separate LTO trial at the preceding checkpoint produced 1,667,514 bytes,
so LTO was not adopted.

## Historical PCP verification

After the final PCP lifecycle extraction:

- Native next CTest: 37 passed; one optional conformance case skipped.
- Threaded ASan/UBSan CTest: 37 passed; the same optional case skipped.
- wasm32 and memory64 products built successfully.
- wasm32 USDA composition suite passed.
- Memory64 next-only composition/variant/diff/validation cases passed;
  dispatch lifetime/JSON and Tydra/flatten smoke suites passed.
- The mixed memory64 composition suite's legacy case fails because the
  existing legacy module lacks `nextFlattenAsyncBegin`; it is outside the
  next-only product and was not changed.

Before the final lifecycle-only extraction: the rebuilt Python extension passed
133 tests with seven skips; legacy MiniJSON unit tests passed; the full Node
profile using the rebuilt next module passed 18 suites, with the helper suite's
three previously observed EXR failures remaining. The standalone checker built,
dumped the immutable rule registry and validated the cube fixture.

These results do not declare the full browser/legacy parity gate green.

## Other changes retained from this work

See `doc/refactor-c-core.md` for the API changes: C-dispatched next WASM classes
(still using emval internally), split render bindings, compiled POD chunk
storage with explicit mutation, Dragonbox-backed MiniJSON number serialization,
and split validation with an immutable pointer/count rule registry.

The larger raw-value-buffer, portable POD binding ABI and default-product
migrations remain unimplemented roadmap items, not follow-up instructions.
A mixed-function GCC buffer microbenchmark showed a register-spill slowdown,
while an isolated read kernel did not and a generated 30,000-triangle WASM
conversion measured about 47 ms versus 49 ms with equal heap usage. Comprehensive
runtime parity is not established; this caveat is retained rather than treating
the size acceptance as proof of runtime parity.
