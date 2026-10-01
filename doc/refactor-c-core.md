# C-style core consolidation

## Consolidation scope

The renewed consolidation work prioritizes native and WASM compile time and
allows public API breakage. The former 512 KiB WASM goal is not an acceptance
criterion. The next product is now the default; remaining compatibility
decisions and consumer migrations govern eventual legacy retirement.

The default implementation is based on `src/next`, compiled as C++17 with
explicit storage and non-template algorithms. Legacy is a deprecated, explicit
compatibility product kept in the repository and CI. API changes are allowed;
removing supported capabilities is not an optimization.

## Current build and API changes

- Core C (`lightusd_c`) now links only `lightusd_next`. Rendering is a separate
  static target (`lightusd_render_c`) linking core C and Tydra. Configure
  `LIGHTUSD_WITH_TYDRA=OFF` to omit render sources and tests. Shared builds use
  one FFI library to avoid duplicating interned-name registries across DLLs.
- Ptex/miniz and the nine stage-independent subdivision sources compile once
  per build tree. Tydra no longer links the whole legacy library for these
  helpers. The next WASM target no longer recompiles subdivision sources.
- Viewer MaterialX compiler support is shared by the viewer and its tests.
- Transform evaluation is a core implementation shared with Tydra. Metadata
  merge, ordering, scene lifecycle, and traversal work moved out of headers.
  The converter keeps its implementation behind a private pointer.
- Removed the unused recursive `next::Prim`/`Attribute` model and `compat.hh`.
  The canonical internal model remains `Layer`/`PrimSpec`/`Stage`.

C ABI **4** changes the numeric values of the unsupported-renderable,
instancer, and root-node scene-record kinds to match the web path boundary:
rebuild all native bindings. ABI 3 changed the layouts of `lightusd_prim` and
the render camera info record.
Handles carry owner, layer, index and generation. Structural edits invalidate
borrowed handles; reacquire them afterward. The owner must remain alive.
`lightusd_stage_retain()` shares ownership; `lightusd_stage_destroy()` releases
one reference. Retaining a stage does not make concurrent mutation safe.

The installed public headers are `lightusd-c.h`, `lightusd-cpp.hh`, and, when
Tydra is enabled, `lightusd-render-c.h` and `lightusd-render-cpp.hh`. The C++17
facade owns C handles without importing internal scene or schema headers.
`Stage` copies retain the same stage; `Prim` retains its stage but still becomes
invalid after structural edits. `Stage` also provides root/default-prim access,
flattening, save, and USDA/USDC export helpers, while `Prim` exposes borrowed
name/type/path and child traversal views. Common native load/traverse/save
consumers therefore need no raw C calls. Other owning wrappers are move-only.

The render C API also exposes a persistent `lightusd_render_session`. It binds
asset resolution to the creating stage's source directory, publishes immutable
owning scene snapshots, and accepts POD change lists. Supported mesh topology
and primvar edits use Tydra's incremental patch path; unsupported changes and
revision mismatches safely fall back to a full resync. Update results report the
published revision, converted resources and scene upsert/remove counts. A
transactional POD event observer now exposes begin/upsert/remove/end/abort
events with stable resource IDs and keys. Consumers can resolve a committed
resource ID by kind and key through the session, while resource payloads remain
available through the owning scene query API. Reset invalidates the lookup
along with the published revision. Opaque C `prepare`/`commit`/`abort` handles
also let a render thread prepare conversion before atomically publishing it;
successful commit consumes the prepared handle.
The C++ facade forwards the same transaction lifecycle while keeping ownership
and cleanup out of consumer code.

```cmake
find_package(LightUSD REQUIRED CONFIG)
target_link_libraries(my_application PRIVATE lightusd::c)
# For render conversion:
target_link_libraries(my_application PRIVATE lightusd::render_c)
```

## Next performance port and runtime controls

The next product now contains the performance work ported from the
next-refactor line. The port was applied to the current `src/next` tree rather
than relying on a clean cherry-pick, because the parser, crate, and PCP files
had diverged. The main pieces are:

- the zmij dtoa fast path, with numeric values formatted directly into writer
  chunk buffers;
- batched deferred arrays and parallel USDA parsing by prim subtree, with a
  serial fallback when the fast path cannot reproduce serial diagnostics;
- parallel USDC stage construction, pooled PATHS storage, parallel and
  streaming crate writes, a split `(stem, name)` path table, and deterministic
  property ordering by name;
- arc-layer prefetch, parallel prototype warming with work donation, and
  releasing composition sources as opinions are filled; and
- forwarding the parser thread hint through PCP into every USDC layer load.

The following measurements used a Release `next_usdcat -f -o /dev/null` build
on a 32-thread host. They compare the pre-port tip with the current port:

| Scene | Load + compose | USDA write | Total | Peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Island | 12.4 s → 4.5 s | 12.6 s → 1.7 s | 25 s → 6.1 s | 7.3 → 6.4 GB |
| Scene C | 3.0 s → 1.5 s | 5.3 s → 0.9 s | — | — |
| Scene A | 314 → 240 ms | — | — | — |

Flattened USDA output is byte-identical for the four baseline scenes with
automatic threading and with serial composition/writing. USDC output now sorts
properties by name, so its bytes intentionally changed; the result remains
readable by Pixar `usdcat`. Profiling the Island USDC-source path found a
roughly 1.6–2.0 s USDA write to `/dev/null`; writing the 8 GB USDA result to a
regular file takes about 9.5 s and is limited by storage throughput. LZ4
decompression was a negligible share of sampled CPU time.

The crate writer reuses identical lazy source ranges without rehashing their
bytes. On the same 16-thread Island USDA-to-USDC run, measured writer time
fell from 12.57 s to 11.13 s, and total time from 17.05 s to 15.35 s. The
written crate retained the same read-back USDA hash and was read by Pixar
`usdcat`. A self-contained root layer also bypasses the PCP build walk when
there are no composition arcs or inactive subtrees.

The crate field/spec pass reuses pre-registered field-name token indices and
a scratch field vector across properties of each prim. This avoids repeated
hash lookups and temporary allocations on large layers while preserving the
written crate bytes.

The USDA parser converts numeric time-sample keys directly from their number
tokens. A generated 400,000-sample scalar layer parsed in a median 443 ms,
down from 478 ms across five alternating Release runs; rewriting the layer
remained byte-identical.

`next_usdcat` exposes independent controls for composition and writing:

```text
--compose-threads 0     automatic composition (same as -1)
--compose-threads 1     serial composition
--compose-threads N     use N composition workers, for N > 1
--write-threads 0       automatic USDA writer workers
--write-threads 1       serial USDA writing
--write-threads N       use N USDA writer workers, for N > 1
--fast-exit             exit immediately after a successful, flushed write
```

Composition defaults to automatic parallel execution. The value `0` is
accepted for compatibility and means automatic execution; use `1` when a
serial run is required. Composition and the writer have separate worker
settings. `--write-threads` applies to USDA serialization and overrides
`LIGHTUSD_NEXT_NUM_THREADS` for that writer invocation. At every worker count,
the USDA output is byte-for-byte identical. Builds without
`LIGHTUSD_NEXT_ENABLE_THREAD` execute these paths serially.

The corresponding library options are `pcp::CompositionOptions::num_threads`,
`USDAWriteOptions::num_threads`, `CrateReadOptions::num_threads`, and
`CrateWriteOptions::num_threads`. Direct PCP callers use `-1` for automatic
composition and `1` for serial; `next_usdcat` maps its CLI value `0` to `-1`.
The USDA writer uses `1` for serial and a non-positive value for automatic
worker sizing. Crate reads use `0` for automatic stage-build workers and `1`
for serial; PCP forwards its parse hint to crate reads. Crate writes use `1`
for serial and a non-positive value for automatic sizing.
`LIGHTUSD_NEXT_NUM_THREADS` remains the CLI fallback for parse, writer, and
crate-write paths where no more specific CLI setting was provided.

`--fast-exit` is opt-in for one-shot `next_usdcat` flatten jobs. It skips the
composed stage's teardown after the output stream has been flushed and closed,
reducing the observed exit delay from roughly 2–3 s to under 1 s on the Island
run. It also skips remaining destructors and `atexit` handlers, so callers that
need normal process cleanup should leave it off.

### USDC structural limits

`CrateLimits` in `src/next/crate/crate-limits.hh` centralizes bounds for crate
structural tables. `CrateReadOptions` inherits these fields, so direct USDC
loads can set them through `USDCLoadOptions::crate_options`:

```cpp
LoadUSDOptions options;
options.usdc_options.crate_options.max_tokens = 1u << 20;
options.usdc_options.crate_options.max_fieldset_indices = 64u << 20;
Stage stage;
LoadUSD("scene.usdc", &stage, options, &warn, &err);
```

The defaults are `max_tokens = 1 Mi`, `max_strings = 1 Mi`, `max_fields = 10
Mi`, `max_fieldset_indices = 64 Mi`, `max_specs = 10 Mi`, `max_paths = 10 Mi`,
and `max_path_depth = 256`. `max_fieldset_indices` has its own bound because
large flattened scenes can contain more FIELDSETS index entries than FIELDS;
using `max_fields` for both rejected otherwise valid production crates.

Composition carries the same policy through
`pcp::CompositionOptions::usdc_limits` and
`pcp::LayerLoadOptions::usdc_limits`. The limits are copied to the root USDC
layer and to referenced, payload, sublayer, and USDZ crate loads. For example:

```cpp
pcp::CompositionOptions composition;
composition.usdc_limits.max_fieldset_indices = 64u << 20;
LoadUSDComposed("scene.usda", &stage, options, &warn, &err, &composition);
```

These table-count bounds complement `CrateReadOptions::max_array_elements` and
the input memory budget; they do not replace either one. The crate-limits
regression covers direct reads, PCP layer loads, referenced USDC files, and
root USDC composition.

The native viewer uses the public C boundary, and native, Python, WASM, and JS
loader defaults select next. The 208-method WASM parity inventory records 170
behavior-verified methods, 31 known behavior differences, six explicit product
decisions, and one test-only exclusion. Legacy retirement still needs review
of those differences and migration of consumers that explicitly select the
deprecated product. Existing native internal C++ consumers remain supported.

The first WASM POD slice is `lightusd_next_render_count`, paired with the
bounded-buffer `lightusd_next_render_error`: they validate the
generation-checked RenderStream handle and return counters or error text
without allocating an emval result. All eleven public RenderStream count
methods now use that typed path. The JS dispatch and Tydra smoke regressions
run against a rebuilt isolated module; the checked-in generated application
module remains unchanged until the normal web packaging step.

The same boundary now exposes scalar node metadata (`nodeType`, `nodeParentId`,
`nodeDataId`, and `nodeVisible`) plus `nodeChildId` through checked
handle/node-index paths. `nodePathBuffer` copies each stable prim-path key into
caller-owned memory, so hierarchy traversal and resource-key correlation do
not require constructing a `getNode()` emval object. Local and world matrices
are available through the matching bounded `nodeLocalTransformBuffer` and
`nodeWorldTransformBuffer` copies.

The aggregate `renderStats()` getter now has a caller-owned POD equivalent. It
reports source and optimized resource counts, merge counts, load/copy timings,
and stage/render memory totals without constructing a JavaScript object in
native code.

Animation summaries also have a typed `animationInfo()` path covering timing,
track/target counts, clip-asset count, and skeletal/node/value-clip flags.
Animation channels now expose typed field queries plus bounded keyframe-time,
path-width keyframe-value, full array-value, and joint-remap buffers, along
with bounded target prim, property, skeleton path, joint-order, and
blend-shape-order copies. A borrowed array-view POD retains the fast
skeletal-array path. JavaScript assembles `getAnimation()`,
`getAnimationView()`, and `getAllAnimations()` from these typed queries;
the C++ aggregate emval methods and their dispatch cases are removed.
The typed keyframe copy now emits one, three, or four floats per keyframe to
match the target path. The JS wrapper also handles memory64 pointers when
reading joint-order strings. The rebuilt next-only module passes the C
dispatch and scene-adapter fixtures on wasm32 and memory64; the complete
wasm32 Node profile passes 24 suites. The current combined WASM target builds.

Path-bearing resource keys use one bounded `resourcePathBuffer()` query for
nodes, meshes, materials, textures, images, lights, cameras, skeletons,
animations, unsupported renderables, instancers, points, curves, and instance
draws. `recordPathBuffer()` exposes the same stable keys through typed scene
record enumeration, and `rootNodeId()` preserves allocation-free hierarchy
entry traversal. Together they keep resource-event correlation in the POD
boundary. Named records also expose bounded `resourceNameBuffer()` copies, so
scene adapters can build labels without aggregate emval objects.

Point-cloud positions, widths, and colors are also available through bounded
typed buffers (`pointsPositionsBuffer`, `pointsWidthsBuffer`, and
`pointsColorsBuffer`), removing another array-valued `getPoints()` aggregate
from the required emval path.

Curve control points, tessellated points, widths, and colors now use the same
typed bounded-buffer path, covering the main geometry payloads exposed by
`getCurves()`.
Curve type, basis, wrap mode, NURBS/Hermite flags, and width/color/opacity
interpolation modes are available as scalar field queries. Authored and
tessellated vertex-count arrays are available as uint32 buffers.

Point-instancer compact records and expanded position, orientation, scale,
prototype-index, and visibility arrays now have bounded typed transfers as
well, so instancer consumers do not need the aggregate `getPointInstancer()`
object for core draw data.
Prototype paths are available through bounded UTF-8 copies keyed by instancer
and prototype index. Prototype node IDs, mesh CSR bindings, and prototype-root
transforms are available through typed buffers as well.

Lights and cameras expose bounded scalar field queries for the renderer's
common type, intensity/exposure, clipping, aperture, and FOV controls without
requiring their aggregate emval records.

Skeleton summaries expose joint count, root joint, and animation linkage
through the same typed field path; joint transform arrays remain a separate
payload surface. Bind matrices, rest matrices, and joint-parent indices now
use bounded typed buffers as well.

Mesh adapters can likewise query vertex/face counts, material id, and normal
or UV presence through `meshVertexCount`, `meshFaceCount`,
`meshMaterialId`, `meshHasNormals`, and `meshHasUVs`. They can copy points,
indices, normals, and primary UV payloads through bounded caller buffers using
`meshPointsBuffer`, `meshIndicesBuffer`, `meshNormalsBuffer`, and
`meshUVBuffer`, without constructing an emval payload object. Source meshes
materialize only the bounded scratch geometry needed for the request; merged
and analytic outputs use their already-owned records.

The same mesh boundary now covers optional tangents, colors, opacities, skin
joint indices/weights, and secondary UVs through typed buffers. Optimized
records that do not retain an optional stream return an empty typed buffer,
while invalid mesh IDs and kinds remain errors.

Material adapters now have the corresponding scalar POD queries for shader
type, alpha mode, double-sided state, opacity, and roughness. The wasm32 and
memory64 modules both rebuild and pass the dispatch regression with these
queries enabled.

Texture adapters now have typed image id, dimensions, channel count, mip count,
and loaded-state queries, allowing upload decisions before requesting image
payloads. Loaded decoded image bytes are available through the bounded
`textureImageBuffer` copy. Scene adapters also expose scaled meters-per-unit, up-axis, time
range, and frame-rate metadata. The dispatch regression now exercises the
material and mesh scalar queries on a real USDA mesh/material fixture on both
wasm32 and memory64.

Mesh adapters now expose bounded caller-buffer copies for points, indices,
normals, and primary UVs. The query-then-copy contract avoids emval payload
objects and handles both wasm32 and memory64 pointer widths; the dispatch
regression verifies the point/index bytes on both module widths.

Point-instancer draw records now expose their stable scalar relationships
(source instancer, instance/prototype indices, mesh, material, and expanded
mesh IDs) through the same checked C boundary. Invalid draw IDs and field
numbers return errors without entering the emval path.

Scene name, default prim, render-settings path, and working color space now
also use bounded caller-owned UTF-8 copies. This removes the remaining string
metadata dependency on emval for the next RenderStream adapter.

Point-instance draw transforms now use a bounded 64-byte matrix copy beside
their scalar relationship fields, so draw submission data can be consumed
without constructing per-draw emval objects.

Common material inputs now have typed four-float copies and texture-ID queries
for both PreviewSurface and OpenPBR (`base/diffuse color`, `emissive`,
`metallic`, `roughness`, and `opacity`). This removes the common shading path's
dependency on the aggregate emval material object while retaining that object
for less frequent authored parameters.

Unsupported renderables now expose bounded copies for their source path, type
name, and diagnostic reason, bringing the diagnostic resource records onto the
same C/POD path as supported resources.
The dispatch regression exercises these copies with an authored `Volume` on
both wasm32 and memory64.

`lightusd_next_render_info_get` also exposes one caller-owned POD block for
all resource counts and the current error length, so adapters can refresh a
complete render summary without issuing eleven emval calls. It is an ABI
building block for the remaining typed resource getters; the JS compatibility
methods remain unchanged for now.

The shared WASM binding header no longer imports the parser, writer,
validation, subdivision, or session headers into every RenderStream translation
unit. Those dependencies remain explicit in the owning dispatcher, while the
split render files retain only the type declarations they use. The common
binding header also leaves the converter and extraction implementations to the
render header, so utility and scene consumers do not inherit that graph. The
target rebuilds cleanly after this include reduction and the C-dispatch
regression continues to pass. A measured header-triggered rebuild recompiles
13 affected translation units and relinks in 17.21 seconds with four jobs.
The render-extraction definition is now included only by the mesh and merge
files that use it; the shared render header forward-declares its array-read
type. The next WASM target and focused C-dispatch test pass after this move.
The common binding header also no longer imports the next umbrella API,
Crate reader, Layer implementation, resolver, xform, color-space, or shade
schema headers. Owning translation units include those definitions directly;
two material helpers moved out of the render header so they no longer force
the shade schema into every render file. The next WASM target and focused
C-dispatch test pass after this include-graph reduction.

The rebuilt module passes the C-dispatch lifetime test, the next Tydra smoke
test, USDA composition/variant/diff validation, and the next-only USDZ/MaterialX
suite through the isolated module override.
The standalone `build-next` CTest suite also passes 39 tests with one optional
AOUSD value-resolution test skipped because its external fixture is absent.
With four jobs and a writable Emscripten cache, a clean `lightusd_next.js`
build measured 95.27 wall seconds, 391.25 CPU seconds, and 277 MiB peak child
RSS in the current build tree.

The reproducible compile harness also measured the reduced WASM boundary after
the latest exports: `binding-next-util.cc` compiles in 2.74 seconds at 206 MiB
peak child RSS, while the render lifecycle unit compiles in 3.60 seconds at
223 MiB. The report records the Emscripten version, command lines, object
sizes, and working-tree fingerprint in
`/tmp/lightusd-binding-compile-latest.json`.
After the recent aggregate-emval removals, the same Emscripten compile
command produces a 150,081-byte render-lifecycle object versus 156,634 bytes
in that prior report (4.2% smaller). A two-run isolated median is 3.41 seconds
versus the prior single 3.60-second sample; that timing difference is
directional, not a clean-build result. The current report is
`/tmp/lightusd-binding-compile-current.json`.
Moving `getLight()` to the typed boundary removes the separate
`binding-next-render-scene.cc` translation unit. With the same Emscripten
command and two isolated runs per unit, the prior lifecycle-plus-scene objects
totaled 180,004 bytes; the updated lifecycle object is 151,666 bytes (15.7%
smaller). The serial per-unit medians sum to 5.29 seconds before versus 3.44
seconds after. These are affected-unit measurements, not a whole-product
clean-build result. The post-light report is
`/tmp/lightusd-binding-compile-post-light.json`.

The sanitizer-configured native next tree now applies ASan through the
next-only early-return path. A 155-step ASan build completed successfully; the
C++ facade and full Tydra Next suite passed with leak detection disabled and
halt-on-error enabled, covering the C session, scene-record, and render-resource
paths under sanitizer instrumentation.

The same early-return path now links UBSan runtimes for next-only executables.
A focused UBSan build of the C++ facade and Tydra Next suite passed with
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

A fresh `LIGHTUSD_WASM_PRODUCT=next -DLIGHTUSD_WASM64=ON` build completed all
181 Ninja steps. The C-dispatch lifetime and typed node/mesh metadata test now
passes against both the rebuilt wasm32 and memory64 modules; its null-pointer
error probe accounts for Emscripten's memory64 BigInt pointer ABI.
The next Tydra-node and USDA-composition smoke suites also pass against the
memory64 module.

An isolated `LIGHTUSD_NATIVE_PRODUCT=next` configure/build completed all 235
Ninja steps, and its generated CTest tree passed 42 runnable tests plus the
optional AOUSD value-resolution skip. A clean single-target measurement of the
render C library records one translation unit at 2.34 seconds wall time and
185,520 KiB peak child RSS with the current GCC 13.3 toolchain.
The split core C API incremental benchmark (one changed `lightusd-c.h` header)
currently rebuilds one translation unit in 4.33 seconds wall time and 4.31
seconds CPU time, with 332,392 KiB peak child RSS on the same Linux build.
The render C ABI header incremental benchmark rebuilds one translation unit in
2.83 seconds wall time and 2.81 seconds CPU time, with 213,612 KiB peak child
RSS.

After the stable-resource lookup addition, the render-enabled standalone suite
passes 42 runnable tests plus one optional skip. The core-only suite passes 37
runnable tests plus one optional skip. The PCP deferred-payload and extracted-
prototype fixtures, plus the StageSession preview fixture, now use per-process
scratch directories; focused core/render runs pass concurrently.
The external-reference lazy-array fixture now uses the same isolation pattern,
and the focused `lightusd`/PCP/lazy-array group passes concurrently as well.

The native render-session C fixture also validates the checked buffer-copy
boundary: size query, exact copy, short destination, and a non-null-sized view
with missing data. The consolidated render-enabled suite remains green at 42
runnable tests plus the same optional skip after this addition.
The C++ facade consumer also now asserts standard-layout/trivial-copyability
for the public render POD blocks at compile time.
Its render facade now covers checked record-key and buffer ownership copies so
native C++ consumers can stay on the facade instead of reaching through raw C
helpers; the facade fixture verifies both exact transfers as well as size
queries.
Native C and its C++ facade also expose the owning render-scene memory byte
count, matching the WASM render statistics used by memory-budget validation.
Native instancer buffers now include prototype node IDs, mesh CSR bindings, and
prototype transforms alongside instance payloads, matching the WASM boundary.
The native C and C++ facades also provide bounded prototype-path copies for
instancer/prototype correlation.
Native skeleton consumers can now query and copy each joint's ordered child
IDs into caller-owned memory. The installed-header C++ facade test checks a
three-joint chain, including size query, short-buffer rejection, and an empty
leaf; the strict C11 consumer compiles against the same function.
The native instancer info POD now reports draw range, visible-instance count,
transform and motion flags, and a borrowed validation-error view in addition
to name/path and instance/prototype counts. The installed-header C++ facade
test converts an authored two-instance scene and checks these fields; the C
layout is verified at compile time. This extends the draft ABI 3 record.
Native scene stats report the same full memory total as the explicit memory
query, including lazily materialized buffer caches.
Native C and C++ consumers now have matching point-cloud and curve payload
buffer queries, including authored and tessellated curve vertex counts.
`examples/c_api_example/next-c-api-example.c` is a small consumer fixture that
uses only the installed `lightusd-c.h` surface for loading, traversal, and
USDA export; it is enabled when the next C target is present.
The standalone next project can compile it with
`-DLIGHTUSD_NEXT_BUILD_C_API_EXAMPLE=ON` and `LIGHTUSD_WITH_TYDRA=OFF`, proving
the core C ABI does not inherit the render or legacy header graph.
`next_quickstart` now uses the public C++ facade for stage ownership and
load/save/reopen, with only primitive traversal calls reaching the C view; the
`lusdcat --explain` now uses the public C API's owned property-trace JSON
entrypoint, removing its direct next/PCP dependency. The other workflow
examples retain direct next APIs for features not yet
represented by the portable facade.
File-backed stages retain their source filename so property explanations can
reload the uncomposed layer and preserve variant/arc provenance after a stage
has been materialized or flattened.
The C API also provides optioned attribute evaluation with default-time,
held/linear interpolation, and connection-following controls, so future
animation consumers do not need to include the internal evaluator headers.
File-loaded stages also install an anchored value-clip resolver and cache for
that operation.
The C++ ownership facade exposes the same operation through `Prim::evaluate`
with an owning `Value` result. It also exposes stage-level variant-set
authoring and selection, plus prim definition/removal, through the same
generation-checked C boundary. Common typed attribute writes, token arrays,
time samples, blocks, removals, metadata, relationships, connections, and
composition arcs are available through `Stage` as well, including token-array
prim metadata used by API-schema and variant authoring.
An earlier include audit found 52 example and web translation units or headers
that imported next/Tydra internals. `lusdcat --explain` uses the public C API;
the legacy `lusdcat` mesh listing now walks its existing Stage directly, and
the asset-resolution example no longer imports an unused Tydra header.
The animation-sampling workflow now uses the installed C++ facade and C/POD
value formatting and SkelAnimation sample helpers.
The portable-package workflow uses C/POD resolver memory assets, aliases,
bounded dependency-report JSON, root-layer AOUSD Core validation, USDC export,
and USDZ-with-assets output. Its relocation check still inspects dependencies
after moving the package.
An isolated GCC 13.3 `-std=c++17 -O2` compile of that example (same include
paths, warm filesystem cache) fell from 1.53 to 0.55 seconds and peak compiler
RSS from 228 to 100 MiB; the object file fell from 31.3 to 16.3 KiB. This
measures one consumer translation unit, not the whole product build.
The remaining imports span `lusdview`, MCP, quicklook, and format
conversion examples. They remain the
explicit consumer-migration backlog rather than being hidden behind the
compatibility facade.

The `progressive_composition` example no longer uses Tydra's template-based
`ListPrims` traversal; it walks the legacy Stage iteratively for mesh paths,
avoiding recursion on deeply nested input. The example builds and still reports
the same three paths for `valuetypes-edge-001.usda`. In a same-tree GCC 13.3
compile, median time was 4.27 to 4.23 s, peak compiler RSS 362,216 to 357,992
KiB, and object size was unchanged at 118,048 bytes. This removes an
unnecessary library dependency and slightly reduces compiler memory, without
claiming a material compile-time win.

The next-only WASM module also rebuilds for both wasm32 and memory64 after the
focused binding-header changes. The native next-product CTest suite passes all
45 registered tests; its optional AOUSD value-resolution corpus test skips
because its supplemental input corpus is not installed.
The `usd_to_gltf` command now loads, converts, and exports through the C++17
facade over C/POD handles. The C export owns binary GLB bytes and loss strings,
resolves file and USDZ texture paths from the source anchor, and applies a
caller-supplied output limit. The existing workflow test covers both regular
and packaged textures and strict loss refusal.
GLB export lives in its own C translation unit so static render consumers that
do not call it leave the exporter unlinked; the C++ facade test binary has no
`ExportGLB` symbol after relinking. With GCC 13.3, `-O3`, the same include
paths and compiler flags, and three isolated uncached translation-unit builds,
the command's median compile time fell from 2.152 to 0.621 seconds and its
object from 49,704 to 20,880 bytes. These are consumer-only measurements;
whole-product build and binary-size gates remain pending.
The viewer's `lod_stream.cc` now uses C/POD stage and prim access for district
traversal, point bounds, camera/world transforms, and up-axis metadata. Its
focused fixture verifies camera-ranked promotion and wrapper metadata; the
viewer executable builds. Under the same GCC `-O3` command, three isolated
uncached translation-unit builds took a 4.715-second median with direct next
imports versus 1.818 seconds through the C boundary. The object grew from
67,416 to 68,504 bytes. A same-tree relink with the baseline object and without
`lightusd_c` was 44,094,624 bytes versus 44,284,992 bytes for the migrated
viewer before dead-code elimination. Linux static C entry points now use
function/data sections, and the viewer links with section GC. Under that same
link mode, the baseline viewer is 42,048,824 bytes and the migrated viewer is
42,071,560 bytes: the residual cost is 22,736 bytes. This is a compile-time
win with a small native binary-size cost; whole-product size work remains.
The C load options now expose native-instance preservation for traversal
consumers; the default holder flattening remains for C export/save round-trips.
The strict C11 test compares both modes on the instancing fixture, and the
LOD pre-pass requests native mode to retain its earlier composition behavior.
Within the viewer, the incremental scene-update header now forward-declares
the change set and leaves its internal definition in the implementation and
test. The VChar control reader imports only the stage and prim-metadata headers
it uses instead of the aggregate next header. These reduce header fan-out but
do not migrate either viewer feature to the C/POD boundary.
The interactive-session workflow now uses the public C/POD document and render
session handles. `lightusd-session-c.h` exposes retained immutable snapshots,
revision/change metadata, payload and variant edits, layer reload, and
progress cancellation. The render-session bridge consumes those snapshots
directly for apply or prepare/commit, avoiding a Stage clone per update. A
strict C11 consumer test covers the installed shared ABI, retained snapshot
lifetime, stale revisions, aborted candidates, commit, and initial-load
cancellation; the workflow test covers recomposition cancellation.
The installed C++17 facade also owns document sessions/snapshots and forwards
document-backed render apply and prepare/commit calls without next/Tydra types.
The C render apply and prepare entry points now share one validated POD change-set
decoder; the strict C11 test exercises invalid flags and property paths through
both calls.
An isolated GCC 13.3 `-std=c++17 -O2` compile of the interactive-session
example with the same include paths and a warm filesystem cache took 1.59 to
0.46 seconds, with compiler peak RSS falling from 241 to 86 MiB. The object
file fell from 43.8 to 13.7 KiB; this measures one consumer translation unit,
not the whole product or the newly added boundary implementation.
The same render-session fixture now publishes a prim removal, checks the
corresponding remove event and retired resource lookup, and confirms the
published empty scene remains usable. It also rejects a candidate at the
begin event and verifies that the transaction aborts without advancing the
published revision.

## Implementation status

| Milestone | Status / remaining work |
|---|---|
| Reproducible measurements | `scripts/bench-compile.py` measures clean Ninja targets and isolated translation units, fingerprints the working tree, records compiler commands/versions, timings, object sections, and raw/gzip artifact sizes. Whole-product before/after gates remain necessary. |
| Immutable built-in type metadata | Implemented: one descriptor table supplies names, layouts, components, and classifications. Constant-initialized name indexing replaces the separate allocated parser map. |
| Shared array ownership | Implemented: one tagged backing and compiled atomic retain/release/detach path replaces the virtual array subclasses and per-type shared ownership machinery. Vector storage/accessors remain transitional to preserve zero-copy adoption. |
| C-style worker dispatch | Implemented: `TaskArena` stores a context pointer plus function pointer instead of `std::function`; the synchronous template adapter preserves lambda call sites without retaining type-erased callback machinery in the worker arena. |
| Shared string index | Implemented: an overflow-checked open-addressed integer index references caller-owned keys. Dictionaries and property/schema-name tables use it without duplicate key strings; dictionary operations compile once outside the public header, with read-only entry iteration and indexed mutation. Immutable threaded snapshots retain stable name views. |
| Shared pipeline source definitions | Standalone next and combined WASM consume the same pipeline source list. Each parent continues supplying its own shared support code, including LZ4. |
| Explicit buffers and scene records | Partial: C prim handles now carry owner/index/generation. Native C and WASM now enumerate typed scene records through stable IDs and bounded path/name copies; native consumers can also use checked caller-owned `lightusd_sv_copy`, `lightusd_render_record_key_copy`, and `lightusd_buffer_copy` paths with successful size queries before allocation. Tydra extraction now owns each `RenderPrimRecord` once and keeps traversal/category pointer views, eliminating the previous full record copy per category membership; conversion phases clear category records and release the shared storage after their last use. Scene memory accounting includes catalog capacities, owned metadata strings, lookup-key storage, and lazily materialized buffer caches. Fully checked allocation handling and string interning remain open. Preserve copy-on-write, lazy Crate ownership, cold metadata, and ordered dictionaries. |

The retained Tydra memory estimate now also covers mesh primvar, blend-shape,
skin, subdivision, and hole-index storage; material shader/config strings and
shared shader records; and spare capacity in mesh, point, curve, instancer,
and image catalogs. Focused capacity-growth assertions pass in both standalone
and root next Tydra suites. This makes retained-scene memory reporting more
representative. Retained conversion now rejects a final scene that exceeds
`max_resident_bytes`, and persistent render sessions reject an oversized
candidate before `BeginUpdate`, preserving the previously published revision.
The converter's in-flight cap still relies on phase estimates and the chunk
allocator; it does not yet cover every intermediate vector growth or the
memory retained by an arbitrary streaming sink. C tests cover the empty-scene
limit and failed initial session publication; a native test covers a mesh-only
incremental update that grows beyond the limit without advancing the published
revision or invoking its event sink. The root and standalone next suites pass
(45/45 and 44/44 registered tests, with one optional AOUSD corpus skip in
each); focused wasm32/memory64 render and C-dispatch tests, the 24-suite Node
profile, and the four Lucia suites also pass after the limit change.
Streaming conversion now checks the catalog's retained size before
`BeginScene`, so an oversized catalog is rejected without transferring it to
the sink. The sink may discard or transform individual geometry resources;
their cumulative retained memory remains sink-specific and is not inferred
from bytes emitted by the converter. The root and standalone next suites and
the 24-suite Node profile pass after this catalog check; focused render tests
pass on both WASM widths.
The latest next-only linked WASM sizes after the retained-scene guard are
1,652,698 bytes (wasm32) and 1,853,222 bytes (memory64); this is a deliberate
small size increase for the limit check, not a compile-time improvement claim.
| Shared binding boundary | Partial: next WASM class registration is replaced by generation-checked C exports and JS wrappers. Existing method coverage is retained. Native C now has a persistent render session with POD change input, owning snapshots, transactional resource events with tested begin/resource/end ordering, stable resource lookup, allocation-free scene-record enumeration, scene render-settings/color-space strings, and fixed-width scene stats matching the WASM boundary. Native C now also exposes typed animation clip/channel metadata, key/value/remap buffers, bounded order/clip-asset strings, material diagnostics, fallback/displacement/volume flags, terminal paths, and retained OpenPBR, volume, and PreviewSurface-utility node-graph JSON copies. The installed C++ facade now forwards every published render C export, including MaterialX configuration, alongside the common node, mesh, material, texture, image, light, camera, skeleton, instancer, point-draw, and unsupported-record info/buffer calls without requiring consumers to spell the C handles. WASM now has typed RenderStream counts/stats/animation-channel/skeleton summaries and resource/scene-record paths, node metadata/transforms, mesh fields (including optional tangents, secondary UVs, colors, skin, bounds, skeleton association, material-subset ranges, and primvar names/metadata plus raw data/indices buffers when retained), mesh/point-cloud/curves/point-instancer and point-draw fields, curve topology/interpolation metadata, point-instancer prototype paths and binding buffers, animation target/order strings, light, camera, material scalar fields (including clearcoat), texture fields (including wrap modes, output channel, UV rotation, and a fixed-layout sampling payload), scene scalar metadata and strings, plus bounded geometry, animation, skeleton, decoded-image, and USD Physics payload copies and bounded-error exports; remaining aggregate getters outside the published inventory and other resource payloads remain pending. A standalone next-only WASM `NextAssetStore` now wraps the next `AssetResolver` through generation-checked C exports: it supports generated `usd-anon:` IDs, named registration, unregister/read, aliases, sorted identifier enumeration, per-entry RFC 4122 UUID and SHA-256 queries, UUID reverse lookup/deletion, hash verification, owned structured reads by name/UUID, cache mutation/query aliases, clear, a hard payload-memory cap (512 MiB default, configurable up to 1 GiB), and a separate legacy-compatible cache cap (zero means unlimited) with sorted-key eviction on insertion. A checked method inventory and dispatch fixtures cover registration/readback, alias lookup, hash bytes and verification, UUID replacement/deletion, structured copy ownership, limit enforcement, and lifecycle on wasm32 and memory64. The store now supports sorted-key eviction and a borrowed view invalidated by replacement, deletion, eviction, clear, or store destruction; its resolver-owned bytes remain shared without a payload copy. The general store is not injected into RenderStream composition/dependency resolution. The next-only WASM module no longer links embind or uses emval; the combined legacy module retains its embind API. |
| Tydra chunk ownership | Implemented: one compiled, element-size/alignment-aware storage engine replaces per-type vector/shared-pointer chunk ownership. The thin typed facade provides read-only indexing and explicit mutation. |
| Product selection and defaults | Root CMake accepts `LIGHTUSD_NATIVE_PRODUCT=next` and returns before configuring legacy dependencies; `next` is the default and `legacy` is deprecated but still built in CI. With `LIGHTUSD_BUILD_EXAMPLES=ON`, the next product now builds the migrated `asset_resolution_example` against `lightusd_next` plus the public `next_c_api_example`; the asset example exercises virtual memory resolution, byte reads, USDA parsing, and writing. Tydra-enabled builds also register `next_render_session_example`, which exercises persistent C render-session revision, event-sink streaming, and record enumeration. A next-native `variant-lister` consumer now builds against `lightusd_c`, loads the authored root layer without composition, and lists variant sets/options; its smoke test verifies a five-option fixture. The core-only `LIGHTUSD_WITH_TYDRA=OFF` configuration remains supported. The default switch is complete. Legacy retirement and review of documented behavior differences remain pending. Next does not silently fall back to legacy. |

The root `LIGHTUSD_NATIVE_PRODUCT=next` path now has a clean Release/Ninja
build with tests and examples enabled. All 45 registered tests pass; the
optional AOUSD value-resolution test skips because its supplemental corpus is
not installed. The compile database has 202
commands for 196 unique source files. A one-run clean build of the public C
and render libraries plus the two installed-header examples took 76.82 seconds
with four jobs and disabled compiler caches; the largest compiler child used
382,980 KiB RSS. At that checkpoint, the core archive was 8,436,868 bytes,
the render C archive 171,710 bytes, the Tydra archive 3,195,904 bytes, and
the shared C library 6,764,912 bytes. These are a next-product baseline,
not a like-for-like legacy comparison. The ignored report is
`build_ninja_next_product/next-product-compile.json`.

A fresh three-run clean measurement of the `lightusd_next` and
`lightusd_render_c` targets with GCC 13.3, four jobs, and compiler caches
disabled has a 79.00 s median (77.77–79.58 s), 306.09 CPU seconds median, and
382,984 KiB peak child RSS median. The report
`build_ninja_next_product/refactor-c-api-boundary-compile.json` fingerprints
the working tree. Its targets differ from the older one-run libraries plus
examples measurement above, so it establishes a repeatable current baseline
but does not claim a before/after speedup or a complete all-target build.

The full configured native product (`all`, tests and examples enabled) was
also measured in three clean runs with the same GCC 13.3 toolchain and four
jobs: median wall time 110.25 s (107.09–110.79 s), median CPU time 432.06 s,
and median peak child RSS 447,424 KiB. The ignored report is
`build_ninja_next_product/next-all-clean-compile.json`; it fingerprints the
same worktree and confirms 202 compile commands across 196 unique sources.
This closes the missing current all-target timing/RSS baseline, but no
equivalent pre-migration all-target report or paired whole-product artifact
size comparison exists yet, so it is not evidence of a speedup or total code
size reduction.

The C render configuration now caps `max_render_records` at `INT32_MAX`, the
range of its record IDs. The `UNLIMITED` sentinel remains accepted but uses
that representable cap. The record getter also rejects out-of-range IDs
instead of narrowing them. A C fixture checks both configuration cases, and
the render-session tests and example pass in both the root next product and
standalone next trees after the change.

## Coverage gates for consumer migration

| Capability | Existing next coverage / migration requirement |
|---|---|
| Values and metadata | `test_next`, `test_value_dict`, `test_crate_dict`, `test_roundtrip_fidelity`; retain roles, blocked opinions, nested/unknown metadata, and byte layouts. |
| USDA / USDC / USDZ I/O | Reader, writer, roundtrip, malformed Crate and crash-replay suites; port any default legacy corpus behavior missing in next before switching defaults. |
| Composition / authoring | PCP, parallel PCP, load rules, composition, layer diff, array edits and conformance suites; compare bindings' arcs, variants, instances, expression variables and time evaluation. |
| Rendering / schemas | Next schema and Tydra suites plus viewer/renderer regressions; verify non-mesh geometry, materials, textures, skinning, animation and physics/format bridges. |
| C / Python | `test_lightusd_c` and `python/tests`; preserve status reporting, retained owners, borrowed views, mutation invalidation and multi-interpreter/thread behavior. |
| WASM / JS | Full `web/js` regression gate, including conversion, dependency-layer sessions, render streaming, subdivision, Worker rendering and physics. Test both wasm32 and memory64. |

These are subsystem gates, not a declaration of complete legacy/next parity.
The remaining binding migration requires a method-level inventory of every
export in both current modules and its C API replacement before removal.

### Render aggregate inventory

| JS getter | Typed replacement | Remaining gap |
|---|---|---|
| `getNode()` | node fields, path, children, local/world transforms | none for current node payload |
| `getMesh()` | borrowed geometry/transform POD view, bounded material/texture records, output subset groups, remapped blend-shape info/buffers, and primvar metadata/name/data | none for the current JS mesh object; sources with custom primvars remain separate during mesh merge |
| `getMeshCopy()` | the same mesh records plus preflighted owned typed-array copies of each retained geometry stream | current next mesh fields are preserved; legacy descriptor aliases and optional packed formats still need contract review |
| `getMeshPtr()` | borrowed fixed-layout mesh view and bounded mesh-view strings | descriptor layout differs; next offsets address WASM memory and expire with the retained scene |
| `getPoints()` | fixed-layout count/material/bounds record, bounded name/path, and owned position/width/color buffers | none for current point payload |
| `getCurves()` | fixed-layout count/material/bounds record, typed curve enums, bounded topology and authored/tessellated geometry, width, color, and opacity buffers | none for the retained curve payload |
| `getLight()` | scalar light fields, transform/color buffers, and resource path/name | none for current light payload |
| `getCamera()` | fixed-layout optics/shutter record, bounded transform and resource path/name | none for current camera payload; native C record carries the same scalar fields |
| `getPointInstancer()` / `getPointInstanceDraw()` | typed fields, buffers, draw transform, bounded prototype paths, and prototype binding buffers | none for the currently published instancer payload |
| `getSkeleton()` | skeleton fields, linked animation source path, joint buffers, and bounded joint names/paths | none for current joint payload |
| `getAnimation()` / `getAnimationView()` / `getAnimationInfo()` | animation info, channel fields and counts, target/property/skeleton strings, order strings, key/value/remap buffers, borrowed full-array view, and clip-asset strings | none for the currently published channel payload |
| `getUnsupportedRenderables()` | bounded path/type/reason strings | none for current diagnostic payload |
| `getSceneMetadata()` | fixed-layout stage metadata and bounded scene strings | none for current metadata payload |
| `getStats()` | fixed-width stats POD, including geometry totals and memory bytes | none for currently published fields |
| material diagnostic/node-graph payloads | diagnostic POD records and bounded retained OpenPBR, volume, and PreviewSurface utility-graph copies; next WASM exposes preferred surface, PreviewSurface utility, and volume graphs through bounded copies | no remaining next RenderStream graph payload gap; `LightUSDLoaderNative.getMaterialWithFormat` JSON, XML, and PreviewSurface-object parity is covered by the combined-module audit |

The typed mesh buffer path shares the source-vertex to render-vertex skin
remap. It exposes joint indices, weights, computed tangents, and retained
optional color, opacity, and secondary-UV streams when present. A fixed-layout
mesh-view POD now materializes each output mesh once and returns borrowed
geometry pointers, transforms, material ID, skin binding, and flags; bounded
strings supply its output name/path, skeleton path, and tangent method. JS
`getMesh()` builds the current public object from that view plus typed material,
subset, and blend-shape calls. C++ geometry/object assembly and dispatch case
67 are removed. Source, skinned, tangent-bearing, analytic, merged-output,
material-subset, and sparse blend-shape fixtures pass on wasm32 and memory64.
Render-enabled low-memory conversion now retains custom primvars separately
from converted geometry. Mesh-only workers use a lazy Stage-backed custom
primvar catalog and bounded copies. Mesh merge now also merges sources with custom primvars when every primvar
is vertex, varying or constant (elementSize 1) and the sources share the same
primvar set (name, format, components; part of the merge key). Each source's
values are expanded per output vertex through the render-vertex to
source-point map, with indices and constants resolved. The merged output
exposes them as un-indexed vertex primvars in its own vertex domain. Uniform
and faceVarying primvars (whose values welding could not keep apart),
elementSize > 1, and GeomSubset meshes stay separate.
In the same next-only wasm32 build tree, the mesh-output object changed from
59,618 to 47,530 bytes (20.3% smaller); the linked `.wasm` stayed effectively
flat at 1,665,684 to 1,665,666 bytes. These are object/file sizes, not a
measured compile-time gain or whole-product size claim. The post-split Node
profile passes all 24 suites.

This inventory tracks bounded C/POD coverage and the behavior differences that
remain after switching the default product to next. A documented difference
may be an accepted product contract; it does not imply a missing implementation.

### Remaining next-only parity work after the native viewer migration

The combined-module typed-C migration and the native viewer boundary are
complete. Keep legacy retirement work separated into these explicit tracks:

| Priority | Gap | Next step | Exit evidence |
|---|---|---|---|
| 1 | Asset cache and streaming buffers | All 43 methods are behavior-verified against legacy or recorded as documented gaps (mmap only); keep the attached-store snapshot lifecycle explicit through refresh and detach. | Crosswalk covers each method in both families; bounded C/POD operations and wasm32/memory64 tests cover ownership, resolver composition, cancellation, handoff failure/retry, attached-store refresh/detach, limits, and stale handles. |
| 2 | Loader configuration and load diagnostics | Done: all 33 configuration and 21 loading/diagnostics methods are behavior-verified or pinned gaps by paired tests. Converter-level scene-count differences found by the corpus probe belong to the render-scene track. | A setting-by-setting matrix plus behavior tests for supported settings and explicit errors for unsupported settings. |
| 3 | Composition, flatten, and export | Done: all 16 composition, 17 next-flatten and 15 layer-export methods are behavior-verified by paired tests (edge differences pinned per method). | Per-method input/output/error parity fixtures on wasm32 and memory64; no silent fallback to legacy code. |
| 4 | Schema/image utilities and MCP | Done: the eight schema/image utilities are paired (two pinned gaps: URDF engine attributes, physics JSON contract); the six MCP calls remain an explicit product decision. | Each method is implemented, explicitly unsupported with a documented reason, or assigned to an adjacent product; all decisions have tests. |
| 5 | Allocation safety and product selection | Allocation-failure and budget gates, native, Python and wasm32/memory64 gates pass; done: browser sweeps pass, the AOUSD file_formats gaps are closed, and next is the default product. | Allocation-failure tests and a completed feature/API matrix; no default switch while any supported legacy contract lacks a next implementation or an explicit product decision. |

#### MCP server product

The six `mcp*` methods of the combined loader now have a dedicated next
product. It lives in `src/mcp/` and is a consumer of the public next C API,
like the native viewer.

**Surfaces:**
- **Native:** the `lightusd_mcp` library and the `lightusd-mcp` stdio
  JSON-RPC server, enabled with `-DLIGHTUSD_WITH_MCP_SERVER=ON` in next
  product builds. Legacy trees keep their own server.
- **WASM:** `LIGHTUSD_WASM_PRODUCT=mcp` builds `lightusd_mcp(_64).js`
  (`LightUSDMCPServer` plus legacy-named module functions such as
  `mcpToolsCall`). It is separate from the lean next-only scene module. The
  npm package ships it, and `web/mcp-server` prefers it and falls back to the
  legacy module.

**Tool contract:**
- Tool definitions are shared with the legacy server through
  `src/mcp/mcp-tool-schemas.cc`, so tools/list cannot drift.
- The next product provides 58 of legacy's 62 tools. `run_script` (QuickJS),
  `texture_resize`/`texture_repack` (image processing) and
  `load_usd_layer_from_asset` (a legacy stub) are omitted and rejected by
  name.
- Several tools that were stubs in legacy now work: `attr_get`, `attr_set`
  (typed JSON or USDA literals), `attr_block`, `attr_connections`,
  `variant_define`, and `schema_get_type` (from next's schema registry).
- `usdz_convert` packages relative asset references as-is and refuses
  texture-processing options explicitly.

**New next API:**
- `Layer::rename_prim_at_path` and `lightusd_stage_rename_prim`.
- Private bridges `SetNativeAttribute` and `MutableNativeRootLayer`.

**Untrusted input:** `lightusd_mcp` always builds with C++ exceptions, so a
wrong-typed argument becomes a tool error instead of aborting the server.
Base64 payloads keep legacy's 64 MiB / 48 MiB limits.

**Tests:**
- `test_mcp_next` (native ctest `next_test_mcp`) drives every tool group
  and the JSON-RPC transport.
- `next-mcp-server.test.mjs` checks, on wasm32 and memory64, that the tool
  list equals legacy's minus the exclusions, plus behavior and hardening.

#### MS-Human-700: MJCF→USD size and render cost

MS-Human-700 had 25.9M render vertices in the browser (~980 MB of vertex
buffers). There were three causes.

**1. Site spheres (95% of the vertices).** 1593 MJCF sites are tiny analytic
`Sphere` prims. The render stream tessellated each into a level-4 icosphere
and emitted it as a 15360-corner soup, because the generated normals and
UVs are per corner.
- **Fix:** RenderStream now welds analytic-shape corners that agree on
  (point, normal, uv) into indexed meshes; seams and hard edges stay split.
- **Result:** sites 24.5M → 4.1M vertices; scene 25.9M → 4.9M.

**2. Bone meshes baked as soups.** The MJCF converter expanded every STL
corner, and OBJ files lost their own indexing.
- **Fix:** the native `urdf-to-usd` welds STL positions as MuJoCo does
  (repeated-vertex removal) and computes MuJoCo-style vertex normals
  (area-weighted, excluding faces beyond acos(0.8) unless
  `smoothnormal="true"`). OBJ keeps (v, vt) indexing.
- **Result:** 1.04M STL corners → 174k vertices; native USDC 16.9 MB →
  4.0 MB. The native converter now also builds on the next product.

**3. next's URDF/MJCF converter trails legacy on MJCF-only scopes.** A
payload comparison across Menagerie models found the gaps:
- sites have no radius, transform, guide purpose or `MjcSiteAPI`, so they
  become visible 1 m spheres;
- tendon, actuator and equality relationships are missing;
- there are no material networks;
- `mjc:option`/`mjc:compiler` scene settings are missing;
- fixed-joint frames are missing and joint limits are named differently;
- lights and cameras have no transforms;
- keyframes and sensors differ in naming.

These are now ported. `src/tydra/next/urdf-to-usd.cc` has dedicated
builders for every scope (sites, tendons, equalities, MuJoCo and Newton
actuators, keyframes, sensors, contact pairs, lights, cameras, materials
with texture networks, MjcCustom/MjcPlugins). It also ports the legacy
link, joint and scene logic: inertia diagonalization, joint frames and
limits, `mjc:option`/`mjc:flag`/`mjc:compiler`, and filtered pairs.

Acceptance: `next::Diff` of next against legacy USDA for all 67 Menagerie
models shows zero unexplained differences, with prim child order also
checked. The remaining differences are:
- **Intentional next additions:** `mjc:timestep`, and collider
  `mjc:contype`/`conaffinity`.
- **Legacy USDA writer artifacts (not converter differences):**
  - `to_string(GeomCylinder)` never prints its props map;
  - typed float attributes print at 6 significant digits.

`next_test_urdf_to_usd` covers every ported scope.

#### Gap triage before the default switch

Of the 32 pinned behavior gaps, one was a functional gap and is now filled.
`createURDFPhysicsScene` authors every legacy engine attribute with the same
values: the Newton scene settings (`timeStepsPerSecond` from the timestep,
`maxSolverIterations` -1, `gravityEnabled`), plus the MuJoCo/Newton contact
and mesh-collision attributes and APIs. Next still adds `mjc:timestep` and
`mjc:contype`/`conaffinity`, so the row is now `behavior_verified` with an
edge difference.

The remaining 31 are accepted as the next contract:
- **Record shapes** (nodes, meshes, images, lights, cameras, skeletons,
  animations, instances): next shape is the product contract, and the field
  differences are pinned in `next-render-scene-contract.json`.
- **Safety and product policy:**
  - the 1 GiB memory default with a 1..8192 MiB range;
  - sparse UDIM tiles by default;
  - no mmap in WASM;
  - setters reject Embind coercions;
  - `setEnableComposition` composes (legacy ignored it).
- **Diagnostics and utilities with their own contract:** located parser
  errors, per-load progress records, crate-structure validation, and the
  physics JSON.

The non-indexed MS-Human-700 geometry is not a next gap. The converted USD
authors the meshes as triangle soups (sequential `faceVertexIndices`,
per-corner flat normals), so both products receive unshared vertices.
Welding STL-derived meshes belongs in the MJCF→USD converter.

`next-loader-config-behavior-parity.test.mjs` now pairs the combined legacy
loader with the next-only `RenderStream` (and `NextUSDZConverterNative` for
USDC export limits) on wasm32 and memory64. It covers all 33
loader-configuration methods plus `computeMeshTangents` and `numMaterials`.
Every setting's default, canonical round trip and conversion effect is
compared against legacy:
- sphere tessellation;
- skin influence reduction, target and rounding (widths, weights, indices);
- value-clip enablement, resampling and time ranges (keyframe times and
  values);
- eager and requested tangents;
- mesh merge, bake transform and material dedup (mesh order, material ids,
  origins);
- the flattened render tree;
- native texture decoding;
- memory statistics;
- USDC export caps.

Canonical inputs match everywhere. The remaining differences are pinned in the
matrix:
- next rejects inputs that legacy's Embind setters coerce;
- the four existing default gaps (UDIM combining, memory limit);
- `setEnableComposition`, which legacy stores but never applies on load;
- an unbound mesh uses a fallback material id equal to `numMaterials`
  (legacy: -1);
- textures decode from an attached `NextAssetStore` and expand to RGBA;
- next enforces the USDC memory cap as an up-front estimate.

The comparison exposed next bugs, now fixed:
- **Material ids.** Mesh `materialId` depended on the order meshes were read
  and did not index the `getMaterialRecord`/diagnostic/parameter queries,
  which used converter order. With dedup on, a mesh could report another
  material's record. Bound materials are now registered in mesh order at
  load, material queries translate the public id through its path, and
  `numMaterials` counts the public table.
- **Mesh buffers.** In merge mode, unmerged outputs returned empty points
  from `meshPointsBuffer`/`getMeshCopy` while `getMesh` showed geometry.
- **Merge order.** Merged outputs were flushed in hash-map order instead of
  mesh order.
- **Tangents.** `computeMeshTangents` failed while tangents were deferred
  (the default), and eager tangents were produced for meshes without a
  normal map. Both now follow legacy's normal-map rule through a new
  `lightusd_next_render_request_mesh_tangents` export.
- **Bake default.** The merge bake-transform default is now legacy's `true`.
- **Flatten.** `setFlattenRenderTree` was stored but ignored; it now
  produces legacy's `OptimizedRenderRoot` hierarchy with baked world
  transforms.

The next `RenderStream` behavior is the product contract for load diagnostics.
`next-load-diagnostics-behavior-parity.test.mjs` pins it against the combined
legacy loader on wasm32 and memory64.

These match legacy on fresh, loaded, failed, released and reset objects:
- `ok`, `warn`, `reset` and `releaseSourceLayer`;
- the idle and post-load results of `isParsingInProgress`, `wasCancelled`,
  `resetProgress` and `cancelParsing`.

`getProgress` and `error` are pinned `known_behavior_gap` rows:
- Next's record tracks each load: stage `complete`/`error`/`cancelled`, byte
  counts, and `errorMessage` equal to `error()`. Failures report the next
  parser's located message.
- Legacy reports `idle` with "Conversion complete" after loading and keeps that
  record after a failed parse.

Idle `cancelParsing()` cancels nothing on either product; only legacy records
it in `cancelRequested`. Next can also report and control a running parse from
its progress callback, which legacy (synchronous, with no JS callback) cannot:
- `isParsingInProgress` is true inside the callback;
- `resetProgress` throws while a parse is running;
- `cancelParsing()` or a `false` return cancels with stage `cancelled`;
- `wasCancelled` stays set until the next `begin`.

These next-only behaviors are pinned as documented edge differences.

`next-loading-behavior-parity.test.mjs` pairs the nine legacy load workflows
with next on wasm32 and memory64, driven by a curated fixture table:
- `loadFromBinary*` map to `RenderStream.begin`/`beginAsync`, with the
  progress variant using `setProgressCallback` plus `begin`;
- `loadFromCachedAsset` maps to `beginCachedAsset(store, id)`;
- the `loadAsLayer*` and `loadTest` workflows map to `NextLayerDocument`
  `load`/`loadWithProgress`, including `NextAssetStore.getAsset` for cached
  layers;
- `loadLayerFromJSON` maps to `loadJSON`.

Shared USDA, USDC and USDZ fixtures load on both products. Each row records
its differences:
- next returns result records instead of booleans;
- next's untrusted input policy rejects unregistered stage metadata and
  malformed metadata under strict AOUSD parsing, and fails
  composition on a missing sublayer or reference asset (legacy never composes
  on load);
- next loads some meshes legacy's converter rejects;
- next requires `typeName: "Layer"` in layer JSON.

`next-validation-parity.test.mjs` now claims `validateLoadedLayer` (verified,
exact match) and `validateFromBinary` (known wording gap). Every
loading-and-diagnostics row is reviewed.

A corpus probe over `tests/usda`, `tests/usdc` and USDZ models informed the
table. It found one next bug: strict parsing rejected the empty path `<>`,
which AOUSD's own corpus authors for relocate-to-none (`RelocateToNone`) and
cleared references (`BasicReference_session`). The lexer now accepts the empty
path in strict mode while still validating non-empty paths, pinned by
`TestEmptyPathReference` in `test_aousd_conformance`. Eleven gitignored
`tests/usdc/*-runtime.usdc` scratch files that next rejects as corrupt are
also rejected by OpenUSD 26.05 `usdcat`; legacy is lenient. The probe's
render-scene count differences (for example meshes of unsupported
interpolations, per-channel animations, dome-light images) are converter
parity work for the render-scene track. It also hit cumulative heap
corruption in the legacy combined module after many layer loads, which
isolated reloads do not reproduce.

Render-scene queries follow the same product decision as load diagnostics:
next `RenderStream` record shapes are the contract, and only value or count
bugs are fixed. `next-render-scene-behavior-parity.test.mjs` pairs every
legacy render-scene query with next on representative fixtures on wasm32 and
memory64: meshes and nodes, primvars, lights, cameras, skeletons and
animations, textures and images, UDIM, native instancing, render-optimization
flags, and layer shading/profile JSON.

The test pairs resources by prim path, because the two converters enumerate
them in different orders. It excludes cross-resource ids from the comparison;
legacy's skeleton, animation and instance order even differs between its
wasm32 and memory64 builds. For each call it pins the fields only legacy
returns, the fields only next returns, and the shared fields whose values
differ, in `next-render-scene-contract.json`. `PIN_UPDATE=1` regenerates that
file after a reviewed change.

- **Identical (24):** counts, scene metadata, textures, UDIM, instances-per-mesh,
  flag getters and layer JSON.
- **Pinned `known_behavior_gap` rows (20):** each has a per-method description.
- **Serialization (3):** `material-format-parity.test.mjs` claims `getMaterial`,
  `getMaterialWithFormat` and `getLightWithFormat` as exact matches.

Every render-scene row is now reviewed.

The comparison fixed three next value bugs:
- **Custom-property animation.** Custom-property animation channels dropped
  int/uint/int64/uint64/bool, matrix and array attributes and padded float
  values to four lanes. They now carry legacy's compact `value_stride` x
  `element_count` lanes in `array_values`, which the keyframe-values buffer
  returns for custom properties.
- **Channel order.** Channels were emitted in time-sample hash order and are
  now in authored property order.
- **Animation duration.** `duration` was end-minus-start; it is now the last
  key time as in legacy. Shared JS consumers enable clips only when
  `duration > 0` and build three.js clips over absolute key times.

The corpus probe also confirmed several differences as next behavior rather
than bugs:
- A malformed single-target collection binding binds no material in either
  next or OpenUSD.
- Next exports `PortalLight` as a rect light and light filters as inert point
  lights with a warning.
- Next reports a geometry-less `Mesh` prim as an empty mesh where legacy skips
  it.
- Next keeps one animation clip per prim, where legacy splits transform and
  custom-property channels into separate clips.

Converter-level count differences in the wider corpus remain converter work,
not query-contract gaps.

The remaining four families are now paired as well, and the parity-matrix
test enforces that every product method names a paired behavior test with a
reviewed status (169 `behavior_verified`, 32 `known_behavior_gap`, six MCP
product decisions, one test-only exclusion).

- **Composition (`next-composition-behavior-parity.test.mjs`).** Legacy
  composes one arc kind in place; next flattens every arc. Single-arc fixtures
  (sublayer, reference, payload, inherit, variants) produce the same prims,
  types, specifiers and values, and every arc/variant query matches.
  - Two next bugs were fixed. `NextFlattenSession.begin()` cleared variant
    overrides set before it, which is the documented workflow. `RenderStream`
    arc queries (`hasSublayers`/`hasReferences`/`hasPayload`/`extract*AssetPaths`)
    read the root layer after in-place composition had consumed its arcs; they
    now read a snapshot of the authored arcs.
- **Layer export (`next-layer-export-behavior-parity.test.mjs`).** Outputs are
  compared semantically: layers reloaded through `NextLayerDocument`, USDZ by
  entries, asset bytes and root layer. Pinned edges:
  - value-plus-connection attributes keep their connection;
  - legacy's stage export loses mesh geometry arrays;
  - flatten keeps authored relative asset paths;
  - legacy's USDZ remap leaves the packaged asset under its original name.
- **Next flatten (`next-flatten-behavior-parity.test.mjs`).** The combined
  module's `nextFlatten*` calls are byte-identical to `NextFlattenSession` for
  single buffers, sinks, remap/variant overrides, multi-buffer fetch and the
  async protocol; the legacy single-buffer entry points accept only Crate
  roots.
  - Comparing USDA and Crate roots exposed a flatten-pipeline bug in the older
    `Compositor`. Variant holder specs authored in a Crate root layer survived
    composition and made the writer emit stale `variantSets`, and an empty
    `variants = {}` was left behind. The Compositor now drops root-layer
    holders and consumed selection flags, and the Crate writer gates
    `variantSetNames`/`variantSetChildren` on the owner still having variant
    sets. Output for both formats now matches `usdcat --flatten` (OpenUSD
    26.05) and native `next_usdcat -f`.
- **Schema/image utilities (`next-schema-utilities-behavior-parity.test.mjs`).**
  - Sample scenes match.
  - Encoded images decode to the same pixels (PNG/BMP), carry the same pixel
    strip (TIFF), or are byte-identical (EXR).
  - Bone textures match for bound meshes; next also skins a mesh that lacks
    `SkelBindingAPI`.
  - Next's URDF converter now marks colliders `purpose = "guide"` like legacy.
    Engine-specific physics attributes and the physics JSON shape remain
    pinned gaps.

The wasm32 combined build directory had `LIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT=ON`
while memory64 had it OFF; the compat `ValueToJSON` reports flattened primvars
as `float[][]`, so it was reset to OFF to match the artifact the tests expect.

Track 5 now has an allocation-failure gate,
`next-allocation-failure.test.mjs`, on wasm32 and memory64. It fails every
staging allocation of 38 next-only operations one at a time, across
RenderStream, LayerDocument, NextAssetStore, NextFlattenSession and
NextUSDZConverterNative (347 injected failures). Each failure must surface as a
JS Error or failure result, never a trap. It must not write through a null
pointer, must release every staging buffer, and must leave the receiving object
able to repeat the operation. Native-side allocations are bounded by budget
preflight rather than recovered after the fact: with `-fno-exceptions` an
unchecked `new` failure aborts. The same test therefore tightens seven limits
and requires a clean rejection followed by success on the same object:

- RenderStream input, resident and provided-asset limits;
- NextAssetStore memory limit;
- NextFlattenSession input and output limits;
- converter USDC export caps.

Gate results at this checkpoint:
- **Native next:** 46 tests in `build-next` and 47 in the root next product,
  each with the optional AOUSD skip.
- **Python:** 140 passed, 2 skipped (`hypothesis` not installed; one
  large-scene test skips when its converted scene exceeds the resident-memory
  limit) with `AOUSD_CORE_SUPPLEMENTAL_ROOT` pointing at the fetched corpus.
- **Web Node regression:** 44 passed; the two Menagerie browser-dataset steps
  fail because that dataset is not downloaded.

Running the Python AOUSD tests against the corpus, which the rebrand had made
undiscoverable at its old `~/.cache/tinyusdz` path, showed that strict parsing
rejected unregistered and elective layer metadata (`framePrecision`, `foo =
bar`). The AOUSD file_formats baseline keeps both, so strict mode now preserves
unknown stage metadata losslessly, as it already did for prim and property
metadata. That also lets `aousd-unknown-property-metadata.usda` load under the
web untrusted policy. The file_formats test now loads its parser-level assets
uncomposed, and every asset now loads in strict mode (no known failures). The
AOUSD reference parser's baselines, not OpenUSD's reader, decided each case:

- `splines.usda`: block comments inside a spline body count as whitespace
  (`pre/*...*/(6.4, 6.4)`). Tangents stay `(width, slope)`, as OpenUSD writes
  them; the AOUSD JSON baseline only names the two numbers the other way.
- `primmetadata.usda`: the parser preserves any finite arc layer offset,
  including a negative scale (the baseline keeps `scale = -2.0`).
  Composition still maps a non-positive scale to identity. Strict mode
  rejects only non-finite values, and `nan`/`inf` are now read as numbers
  there instead of being skipped.
- `gen_pathexpression.usdc`: `pathExpression[]` is read, written to USDA and
  Crate, and parsed from USDA. OpenUSD reads next's Crate output back as the
  same array.

**next is now the default product.** Legacy stays available for a transition
period:

| Surface | Default | Legacy selection |
|---|---|---|
| Root CMake | `LIGHTUSD_NATIVE_PRODUCT=next` | `-DLIGHTUSD_NATIVE_PRODUCT=legacy` |
| `web/` CMake | `LIGHTUSD_WASM_PRODUCT=next` (`lightusd_next(_64).js`) | `-DLIGHTUSD_WASM_PRODUCT=legacy` or `combined` |
| `LightUSDLoader` | `backend: 'next'`, next-only module | `backend: 'legacy'` |
| `LoaderConfigUtils` helpers, `LightUSDWorker` | next | `backend: 'legacy'` |

Existing CI jobs and bootstrap scripts pin
`-DLIGHTUSD_NATIVE_PRODUCT=legacy` / `-DLIGHTUSD_WASM_PRODUCT=legacy`, so
legacy coverage is unchanged; `npm/build-wasm.sh` already selected every
product explicitly. The new `build-next-product` Linux job builds and tests
the default root configure. A fresh default configure builds the next
product and passes all 47 tests.

Demo status:
- Viewers with a next scene builder (`buildNextThreeNode`) run on next.
  `main.js` gained that branch; `offscreengl.worker.js` moved earlier.
- `LightUSDWorker` imports the legacy module only on the legacy path, so
  next-only deployments load it.
- The `web/demo` site builds local next WASM for its default backend. Its
  standalone pages use next rendering, composition, and export; the explicit
  backend comparison and path tracing worker retain the deprecated combined
  module. The online viewer composes payloads during loading. Two separate
  `web/js` demos remain pinned to `backend: 'legacy'` until they are ported:
  - `phys-sim.js` consumes legacy's physics JSON through
    `LightUSDLoaderNative`;
  - `materialx-webgpu.js` converts legacy material JSON for WebGPU.

With the pinned MuJoCo Menagerie checkout
(`71f066ad0be9cd271f7ed58c030243ef157af9f4`) the full web regression now runs
the dataset steps.

- **MJCF/USD/MJCF closure:** passes 67/67 models.
- **Browser sweeps:** these need a Chrome binary. Puppeteer's bundled Chrome is
  not installed here, so they ran with
  `PUPPETEER_EXECUTABLE_PATH=/usr/bin/google-chrome-stable`. The `urdf.html`
  sweep passes 5/5, and its MJCF and converted-USD renders match visually.
- **OffscreenCanvas sweep:** passes 64/67. The same three models as before
  (`iit_softfoot`, `ms_human_700`, `robot_soccer_kit`) time out. The cause is
  the legacy loader: the worker loads files through `LightUSDLoaderNative`,
  which runs past 90 seconds on `softfoot.usdc` in Node as well. The next
  `RenderStream` loads the three converted files in 0.6 s, 0.3 s and 10.3 s,
  and native next converts softfoot in 0.1 s.

The remaining browser-gate failure is therefore a legacy-backend failure that
moving this worker to the next backend would remove.

The OffscreenCanvas worker (`offscreengl.worker.js`) now uses the next backend.
It loads the next-only module, wraps the result in `NextRenderSceneAdapter`,
and builds the scene with `buildNextThreeNode`, as the other next demos do.
Switching surfaced four problems:

- **The sweep's blank check never failed.** It counted any pixel with
  `r+g+b > 100`, which includes the light environment background. It now
  compares each pixel against the same row's background colour and masks the
  HUD. Every earlier legacy render still scores at least 0.0066 against a
  0.002 threshold.
- **Node `dataId` pointed at the wrong mesh.** RenderStream publishes Mesh
  prims first and analytic gprims (`Cube` and similar) after them, but node
  `dataId` indexed the converter's traversal-ordered mesh list. Where both
  kinds were mixed, the wrong geometry was drawn; on so_arm100 the jaw became
  a 2 m cube. `RenderStream::remapNodeMeshIds_` now rewrites mesh-node ids
  into output space by prim path, and sources folded into a merged output
  become -1. `next-api.js`'s legacy-style tree exposed the same bug as
  `contentId`. next-c-dispatch pins the mapping with interleaved Mesh/Cube
  siblings.
- **The legacy tree builder dropped next materials.**
  `LightUSDLoaderUtils.convertMaterial` expects legacy material JSON, so every
  mesh came out default grey. `buildNextThreeNode` reads next material
  records directly.
- **Camera framing used stale world matrices.** `fitCameraToScene` measured
  the scene before any render had updated world matrices, so unscaled
  analytic geometry inflated the box and 14 models rendered too small to
  see. It now calls `updateMatrixWorld(true)` first, as
  `progress-offscreenwebgl.worker.js` already did.

Results:

- **Full web regression:** 48 of 49 steps pass. The only failure was the
  OffscreenCanvas sweep at 53/67, before the framing fix.
- **Final OffscreenCanvas sweep:** passes 67/67 on the next backend with the
  background-relative blank check. The lowest coverage is `tiago_dual` at
  0.0068, the same as legacy's 0.0066.
- **After the framing fix:** the 14 failing models pass 14/14, including
  `iit_softfoot` and `robot_soccer_kit`, which legacy could not load.
  `ms_human_700` renders the whole skeleton under next. It carries 1941 bone
  meshes expanded to one vertex per corner (25.9M vertices, about 980 MB of
  vertex buffers), the adapter takes about 60 s, and the first frame needs
  seconds to upload. The sweep therefore polls the canvas until it draws,
  within the per-model timeout, instead of screenshotting 500 ms after load.
- **Guide geometry:** RenderStream now reports each output mesh's computed
  UsdGeomImageable purpose: the nearest authored opinion on the prim or an
  ancestor (`tydra::next::ComputeInheritedPurpose`). It is exposed as
  `meshField` 11 and `meshPurpose(id)`, and as `purpose` on `getMesh()`
  records. Mesh merging keys groups by purpose, so a guide never folds into a
  renderable group. `buildNextThreeNode` hides guide meshes by default, as
  usdview does (`showGuides: true` shows them), and leaves them out of the
  scene bounds. Mesh counts are unchanged. MJCF collision copies (for example
  so_arm100 `Base1`) no longer draw grey over the visual meshes.
  next-c-dispatch covers inherited, overridden (proxy), analytic and merged
  cases; the render-scene contract snapshot gains the `purpose` field.

The resolver/cache boundary remains the first parity track. The next core has
a thread-safe memory-asset resolver, and the next-only WASM `NextAssetStore`
now adds bounded registration/read, aliases, enumeration, per-entry UUID/hash
lookup and verification, reverse UUID lookup, and UUID deletion. RenderStream
can import an explicit resolver snapshot that shares immutable payload views;
the cache and stream still have separate mutation lifetimes and budgets. Do not
model a cache UUID as a filesystem path or claim whole-family parity from
these operations alone.

The first resolver-cache parity operation is now exposed as
`lightusd_asset_resolver_unregister_memory` and the installed C++ facade's
`UnregisterMemoryAsset`. It removes a registered memory asset, reports
`NOT_FOUND` for an unknown identifier, and leaves aliases intact so they fail
to resolve until their target is re-registered. C and C++ tests cover embedded
NUL bytes, unregister/repeated-unregister behavior, and reads after removal.
This advanced the native next C boundary first. The later next-only WASM
`NextAssetStore` now supplies UUID lookup/reverse lookup, UUID deletion, and
sorted cache identifier enumeration; borrowed zero-copy lifecycle and cache
sorted-key insertion eviction have since been added to the WASM store.
RenderStream imports shared immutable payload snapshots for composition.
Stream-finalization handoff now transfers the budget lease, and attached stores
share retained-payload accounting with RenderStream imports and allocations.
Unattached objects retain independent limits; remaining cache and loader parity
decisions are documented below.

The next-only WASM `NextAssetStore` now has a separate 64 MiB default metadata
budget for identifiers and aliases, in addition to its payload-byte limit. Asset
entries charge identifier UTF-8 bytes plus 256 bytes of estimated map/resolver
bookkeeping; aliases charge both names plus 128 bytes. The limit is configurable
up to 1 GiB, replacements account against the prior entry, and rejected empty
asset or alias insertions leave prior state intact. Wasm32/memory64 dispatch
tests verify exact accounting and rejection for zero-byte assets and aliases.

The native resolver boundary now also supports an application-set aggregate
memory-asset payload limit and reports registered asset count and payload
bytes. Replacing an identifier accounts for the replaced payload; unregister
releases its bytes. Over-limit registration and lowering the cap below current
usage fail without changing resolver contents or the prior limit. Strict C11
and installed C++ facade tests cover these operations. This bounds the native
resolver's memory assets. The standalone WASM store now has corresponding
identifier/UUID/hash queries, bounded copies, and sorted-key insertion
eviction; streaming-cache accounting and full resolver parity remain open. The native C identifier query returns registered map-order keys as bounded
UTF-8 copies; size queries and short-buffer errors are covered by strict C11
tests.

Next-only WASM `RenderStream.removeAsset(name)` now removes one normalized
value-clip dependency from the same per-stream map populated by `provideAsset`.
`providedAssetNames()` now enumerates the normalized keys through a count and
bounded string-copy C ABI, sorted by normalized path. The stream also accepts
an aggregate resident-byte limit (`0` means unlimited), accounting for
normalized key bytes plus payload bytes. Inserts or replacements that exceed
the limit fail atomically, and lowering the limit below current use is
rejected; it does not silently evict an asset needed by a pending clip.
Removal returns `true` when it removed an entry and `false` when that name was
absent; invalid receivers, wrong argument counts, and empty names are
rejected. The C dispatch
and RenderStream inventory tests pass on wasm32 and memory64, and both
next-only USDZ conversion suites pass on both widths. This is per-stream
dependency-map visibility and removal only: the backing map remains
clip-specific and is not yet a general asset cache or the legacy zero-copy
stream lifecycle. This limit is a checked per-stream budget, not parity with
the legacy cache's eviction policy.

`getProvidedAsset(name)` now copies a retained dependency payload through the
same typed C boundary. It returns an owned JavaScript byte array, distinguishes
a missing name from an empty payload, and is unaffected by later mutation or
release of the caller's source view. Dispatch and converter inventory tests
pass on wasm32 and memory64.

For callers that already hold a byte view into the WASM heap,
`provideAsset(name, bytes)` now passes that view's byte offset directly
to the typed C export. It allocates only the UTF-8 name staging buffer instead
of copying the payload into a second temporary WASM buffer; C++ still copies
the bytes into stream-owned storage before returning, so the caller may reuse
or release the input view afterward. The dispatch regression checks the
reduced staging allocation on wasm32 and memory64. This improves raw-ingest
copy behavior but does not provide a borrowed cache view or the legacy
zero-copy streaming-buffer lifecycle.

The wasm32 Node regression profile passes all 24 suites against the rebuilt
combined module; the validation-parity case skips because its native `lusdcat`
dependency is absent from this web build tree. The next-only C dispatch and
converter tests independently pass on wasm32 and memory64.

The exact next-only `RenderStream.prototype` surface is captured in
`web/js/tests/renderstream-api-inventory.json` and checked against the live
module by `usdzconvert-next-only.test.mjs` (348 methods, including compatibility
getters and aliases). Typed operations map by family to the
`lightusd_next_render_*` POD/checked-copy exports in `web/binding-next-api.h`;
the corresponding native resource-info, buffer, and string-copy functions are
the `lightusd_render_*` exports in `lightusd-render-c.h`. Compatibility getters
are assembled in JS from those primitives and intentionally have no aggregate
native C object equivalent. The combined module now has a parallel runtime
inventory in `web/js/tests/lightusd-loader-api-inventory.json` for all 208
`LightUSDLoaderNative` methods, checked by the combined-WASM variant-overload
test. The checked cross-product matrix at
`web/js/tests/next-wasm-parity-gaps.json` compares every legacy method against
the current next RenderStream, USDZ converter, asset-store, layer-document, and
flatten-session inventories and classification; its verifier runs in the Node
regression profile. The current inventory has 94 same-name RenderStream
overlaps; 114 legacy methods have no same-name RenderStream method. The matrix maps 17 `next_flatten` operations plus six composition/variant operations, layer-level `flattenLayer`, and direct `remapLayerAssetPaths` to workflows on `NextFlattenSession`; six binary-loading operations to RenderStream, `LayerDocument`, and `NextAssetStore` workflows; `exportAsUSDA`, `exportAsUSDC`, and `layerToString` to `LayerDocument.load` plus the corresponding export workflow; two image accessors to RenderStream methods; owned `getMeshCopy` plus borrowed `getMeshPtr` to the bounded-copy and geometry-view mesh APIs; 26 asset/cache methods to `NextAssetStore`; and ten scene-creation, mesh-buffer, physics-extraction, export-limit, and USDZ-export methods to `NextUSDZConverterNative`; that converter applies checked asset-path remaps to a cloned stage and packaged asset names for `exportAsUSDZWithRemap` and its two-argument options form. It preflights the estimated clone, remap, and duplicate asset working set against the configured retained-payload budget, and rejects packaged-name collisions. Archive regressions verify the rewritten root path, renamed payload entry, collision and budget rejection, and unchanged subsequent exports on wasm32 and memory64. `getMeshCopy` preflights all ten typed mesh buffers against the 512 MiB aggregate and remaining-memory limits before copying; its pointers are independent of WASM heap growth. `getMeshPtr` maps to `getMeshGeometryView`, whose offsets are borrowed WASM-memory views. Thirty-two legacy methods have neither a same-name RenderStream method nor a mapped next operation. `getURI` preserves the optional source name on byte/async loads and uses the identifier for cached or streamed assets. The image mappings are `getImage` to owned `getImageCopy` and `getImagePtr` to borrowed `getImageView`; these cover retained decoded pixels with explicit ownership, but do not reproduce the legacy pointer descriptor or all legacy color metadata. Same-name and mapped entries remain candidates for contract comparison; they do not establish full signature or behavior parity. Matching scene/render families
(mesh/node, animation, camera, light, skeleton, metadata, stats) have typed
next adapters and native resource APIs. Other legacy methods group into
composition/flatten and export operations; asset cache, UUID, and zero-copy
streaming management; loader configuration and progress; MCP; and URDF/image
utilities. Some have next-core C APIs or `NextFlattenSession`/
`NextUSDZConverterNative` counterparts, while cache/MCP and several loader
controls have no next-only counterpart identified yet. The flatten workflow map at `web/js/tests/next-flatten-parity-map.json` maps buffered, sink, remap, variant, and multi-layer legacy operations to `NextFlattenSession`; its exact prototype inventory and parity-map verifier are in the Node regression profile. `next-c-dispatch.test.mjs` verifies buffered and sink output plus configured output caps, while `next-usda-composition.test.mjs` exercises remaps, variants, and multi-layer session composition on wasm32 and memory64. The checked
`lightusd-loader-api-classification.json` now assigns each method to one
semantic family, and the combined-WASM inventory test verifies exact coverage.
Native USD instance nodes are now queryable as a bounded `nativeInstanceNodeIds()`
RenderStream operation, implemented over the next scene's actual node model;
`getNode(id)` supplies the retained prototype path, transforms, visibility, and
hierarchy. This is a next-native query, not yet a replacement for legacy dense
instance records (`getInstance`/`getInstancesForMesh`), whose separate prototype,
mesh, and material indexing model is not retained by next RenderScene. The
cross-product matrix now resolves all asset/cache entries to their owning
`NextAssetStore` or `RenderStream` surface and checks mapped targets against both
runtime inventories; `next-asset-store-parity.test.mjs` continues to cover the
complete 28-method asset family. A separate composition workflow map covers
`composeSublayers`, references, payloads, inherits, variants, and explicit
variant selection through `NextFlattenSession` authoring/composition operations;
its verifier is registered beside the flatten workflow map, while
`next-usda-composition.test.mjs` verifies authored arcs, dependency supply,
variant overrides, and flatten results on both WASM widths. The loading map
cross-checks binary render/layer entry points against the RenderStream,
LayerDocument, and NextAssetStore runtime inventories; dispatch regressions
cover synchronous, async/progress, cached-asset, and layer-document loads on
both WASM widths. `loadAsLayerFromCachedAsset` is covered as the owned
`NextAssetStore.getAsset()` result passed to bounded `LayerDocument.load()`.
The layer-export workflow map also resolves `flattenLayer` through the next
flatten session and `layerToString` through layer load plus USDA export; the
dispatch and USDA-composition suites exercise those target operations, while
archive and Stage-specific exports remain unmapped.
All 208 inventoried combined loader methods now use typed-C adapters; their
legacy Embind registrations are excluded from the combined build. This does
not remove Embind from the whole combined module: construction/lifetime and
other classes still use it. Nor does it replace the legacy stage/render
implementations behind these C exports. Next-only per-method signature,
behavior and replacement decisions remain open before removing the legacy
product or switching defaults.

The matrix currently has 30 methods without a same-name RenderStream method
or mapped next workflow: render-scene queries 5, layer export/validation 5,
loader configuration 7, loading/diagnostics 7, and MCP 6. One additional
method, `validateFromBinary`, maps to a next module-level function but has
verified behavior differences. Unmatched and behavior-difference rows still
need a replacement or retirement decision; the counts do not imply every
legacy method belongs on RenderStream. `getLightWithFormat` was a missing render
query family and now uses typed next light data plus the shared legacy
serializer. Its JSON, XML, byte-format overload, and error results are compared
with the combined module for every light in `lights-full-001.usda`, including
the time-sampled light, on wasm32 and memory64. `ConvertLight` now evaluates
typed values through `AttributeEval` at the configured time instead of reading
only fallback property values; its owning Stage is passed explicitly through
the converter and C query boundary. The test also covers the legacy default
time selecting the first authored light sample.

| Family | Methods | Initial migration direction |
|---|---:|---|
| Render scene queries | 48 | All 48 use typed C in the combined product. Compare payload and format against typed RenderStream/native render C exports; names alone do not establish next-only parity. Includes `getAllLights`, reclassified from loader configuration because it aggregates `getLight`. |
| Next flatten | 17 | All 17 methods use typed C, including the callback-driven sink, multi-buffer fetch, and session-step calls. No Embind flatten methods remain. |
| Composition and variants | 16 | All 16 use typed C in the combined product; the shipped legacy product keeps its Embind registrations. Behavior is the legacy layer's; a move to next composition/session APIs remains open. |
| Layer export and validation | 15 | All 15 use typed C in the combined product. Compare against next readers, writers, converter exports, and validation results. |
| Asset resolution and cache | 28 | All 28 use typed C in the combined product, with exact size_t transfers, owned/borrowed byte records and bounded string tables. UUID, eviction, path and raw-ingest contracts have coverage; a next-only cache implementation is now partial and still lacks full resolver integration. |
| Streaming buffers | 15 | All 15 use typed C in the combined product; legacy keeps Embind. The allocation/progress/finalize/cancel lifecycle is regression-pinned on both pointer widths. A next-only counterpart remains required. |
| Loading and diagnostics | 22 | All 22 use typed C in the combined product. Separate parser/load parity from legacy progress and cancellation behavior. |
| Loader configuration | 32 | All 32 use typed C in the combined product (legacy keeps Embind). Next-only input-byte, resident-memory, composition, value-clip, sphere-tessellation, bone-reduction, and influence-rounding settings are mapped; remaining controls need next load/converter mappings or explicit product decisions. |
| Schema and image utilities | 9 | All nine use typed C in the combined product. Check available next schema and image operations individually; no blanket replacement is assumed. |
| MCP | 6 | All six use typed C in the combined product. No next-only counterpart is identified. |

The combined-product inventory above is not itself a next-only parity result.
Use this status matrix for next-product planning; “partial” means the named
next-only APIs cover a narrower contract than the whole legacy family.

| Legacy family | Next-only status | Verified coverage | Remaining gap |
|---|---|---|---|
| Render scene queries | Partial | `RenderStream.getAllLights()` enumerates the next light count and returns the same per-light records as `getLight`; `getAllSkeletons()` aggregates next skeleton records. `getSkeleton()` includes the legacy recursive `root_node` and metadata field names alongside its indexed-joint view, while `getSkeletonJointsFlat()` supplies legacy flat field names. Authored Skeleton `displayName` is preserved. Legacy scene counts `numImages`, `numMaterials`, `numTextures`, and `numRootNodes`, plus `getDefaultRootNodeId`, dispatch through typed next counts. `getRootNode` and `getDefaultRootNode` reconstruct trees iteratively from bounded node/child queries with cycle detection; `nodeChildCount` keeps traversal proportional to the authored hierarchy instead of scanning every node for each child list. `getAllImages()` now aggregates owned image records after size-querying decoded pixel bytes and UTF-8 names/paths without allocating those payloads, then preflighting them with per-record object overhead against the 512 MiB aggregate estimate; wasm32/memory64 dispatch checks metadata, record order, and oversized aggregate rejection. `extractUnresolvedTexturePaths()` now filters undecoded image records, size-queries authored asset identifiers across the aggregate before copying, and preserves image order and duplicate paths; a sparse two-tile fixture checks the behavior on wasm32 and memory64. `getMeshPrimvarsJSON()` now provides bounded mesh-primvar JSON for the retained next mesh catalog, including interpolation, index expansion, component values, and authored `elementSize` fetched from the Stage metadata; its checked RenderStream inventory and a grouped/indexed scalar fixture pass on wasm32 and memory64. Full light-type and skeletal-animation fixtures plus wasm32/memory64 dispatch verify these views and argument handling. | Root trees now add legacy `primName`/`displayName`/`absPath`/`nodeType`/`nodeCategory`/`contentId`/`globalMatrix` aliases over next node records; `nodeCategory` is derived from the next node type. Next `SkeletonJoint` now retains `matrix4d` bind/rest data through its C payload and JS flat/hierarchical records; a wasm32/memory64 fixture with a value not representable in float32 verifies this. Next nodes now retain reset-xform state and native-instance prototype paths and expose `hasResetXform`, `isInstance`, and `prototypePath`; reset-stack and native-instance fixtures cover these fields. Numeric prototype indices/instance IDs and a separate material ID are not defined for the next native-instance model, and node matrices remain float32 while legacy cursors serialize doubles. `getImageCopy()` returns metadata and an owned copy only when the retained image is decoded; `getImageView()` exposes the same metadata and a borrowed heap view when decoded pixels have a stable retained buffer. The borrowed view is invalid after RenderStream mutation/destruction and is unavailable for encoded, undecoded URI assets, which remain application-decoded. New `getMaterialRecord()`/`getAllMaterials()` expose next RenderMaterial names/paths, shader and alpha modes, sidedness, default-fallback state, common PreviewSurface/OpenPBR values, texture links, and `getMaterialDiagnostics()` returns the retained `{kind,material_path,node_path,shader_id,message}` records; `getTextureRecord()`/`getAllTextures()` assemble owned texture records with name/path, linked image ID and dimensions, sampler settings, UV primvar, source/target color spaces, and sampling values, plus a bounded 14-float color-transform payload carrying validity/bypass/data flags, gamma/bias, and the resolved 3x3 transform. The bounded `textureString()` query preserves the retained texture strings without emval payloads; wasm32/memory64 dispatch checks the authored sRGB transform flags, gamma/bias, and nine matrix elements. `getTextureRecord()` also carries `hasTransform2d`, legacy transform values, `isUDIM`, and `udimTextureId`. Next conversion resolves sparse UDIM assets into per-tile image IDs, remaps them during material-batch merge, and accounts their retained metadata. Bounded `getUDIMTextureRecord()` / `udimTileBuffer()` / `udimString()` C/JS queries preserve tile IDs, UV coordinates, image links, and legacy metadata strings. Native converter tests cover missing-tile sparsity and memory accounting; wasm32/memory64 `beginCachedAsset()` fixtures register a root USDA and two non-contiguous tiles, then verify resolver discovery and query results end to end. Atlas-combined UDIM mode and mesh UV rebaking remain open. PreviewSurface, OpenPBR, and unsupported-shader fixtures verify record fields, diagnostic strings, aggregation, and argument handling on wasm32/memory64. `getMaterial()`/`getMaterialWithFormat()` now use the shared legacy JSON/XML serializer through a typed conversion from next `RenderMaterial`; the adapter preserves authored display names, texture/image metadata, OpenPBR fields, separate sheen/fuzz values, and legacy conversion aliases. The retained C++ result cache is charged to remaining stream memory and released after the owned JS copy. JSON, XML, and legacy PreviewSurface-object results compare with the combined legacy loader on PreviewSurface and OpenPBR fixtures on wasm32 and memory64. The browser RenderStream keeps decoding application-owned, so image records normally contain metadata and URI only. Other render queries still need audit. |
| Asset resolution and cache | Partial | Next-only `NextAssetStore` exposes generated asset IDs, named registration, copied and borrowed reads, checked direct WASM-heap pointer ingestion, aliases, sorted enumeration, separate payload/cache byte limits, a hard metadata budget for stored names and aliases (UTF-8 bytes plus conservative per-entry overhead), sorted-key eviction, UUID lookup/reverse lookup, SHA-256 lookup/verification, UUID deletion, name/UUID existence queries, and owned `{name,data,sha256,uuid}` records by name or UUID. `RenderStream.beginCachedAsset(store,id)` validates the stored root size against the stream input limit before loading, attaches the store for composition dependencies and render-converter texture resolution, and runs the normal RenderStream path. `importAssetStore` imports shared immutable payload views and aliases as a snapshot; `setAssetStore` attaches a live store and refreshes before each byte or streamed-root load. `detachAssetStore` clears imported resolver entries while preserving stream-local provided assets. Store payloads and attached stream allocations reserve from one shared retained-payload budget; snapshot owners keep reservations alive until their final reference is released. wasm32/memory64 dispatch fixtures exercise cached roots, missing IDs, over-limit rejection, identity, replacement invalidation, hashing, aliasing, limits, eviction, raw-pointer span checks, shared-view lifetime, refreshed composition, detach behavior, and cross-object budget enforcement. | Explicit `importAssetStore` calls remain snapshots; callers re-import to refresh or detach to clear them. Filesystem search-path and parent-relative controls remain outside the WASM memory-store contract. |
| Streaming buffers | Covered; mmap explicitly unsupported | Next-only RenderStream supports owned chunked asset start/append/progress/finalize/cancel, UUID lookup, legacy-shaped progress records, the UUID-based zero-copy allocation/pointer/progress/mark/finalize/cancel family, bounded borrowed WASM views, random-offset writes, and byte totals/fractions under provided-asset and resident-memory limits. `finalizeStreamingAssetToStore` adopts completed bytes into `NextAssetStore`, preserves the transfer UUID, and transfers its shared-budget lease without double counting; store-limit failure retains the transfer for retry. The compatibility inventory and dispatch fixtures pass on wasm32 and memory64. | `getMMapZeroCopy`/`setMMapZeroCopy` are present as explicit unsupported operations: setters validate boolean input, both throw because WASM storage is not mmap-backed, and tests pin this result on both widths. |
| Loading and diagnostics | Partial | Synchronous bounded input, cached root loading via `beginCachedAsset(store,id)`, configured parse limits, error text, per-RenderStream phase callbacks with cancellation during USDC Crate reads and USDA parsing, plus `compositionReport()` exposing PCP dependencies and typed `{code,site,message}` issues; wasm32/memory64 tests cover successful dependencies and missing-asset diagnostics. `beginAsync` snapshots a preflight-approved input view, yields one event-loop turn, and returns the same checked result as `begin`; wasm32/memory64 tests cover deferred completion, source mutation after invocation, and byte/resident-limit rejection. USDA reports bootstrap, each completed prim, and completion; cancellation prevents publishing the parsed Stage. New next-only `LayerDocument` loads bounded single-layer bytes through the public C API, edits prims and custom typed scalar/array properties (POD numeric/vector/matrix values, half scalar/vector/array values, plus scalar and array string/token/asset values), removes prims and properties, authors/replaces, inspects, and removes relationship target lists through `setRelationshipTargets`, `getRelationshipTargets`, and `removeRelationship` with a 65,536-target bound; `setStageMetadata`/`getStageMetadata` cover supported scalar stage keys and authored state; `setPrimMetadata`/`getPrimMetadata` cover authored bool/string/token keys and bounded `apiSchemas` token arrays with authored-state reporting; `setAttributeMetadata`/`getAttributeMetadata` author and inspect supported interpolation/color-space tokens, string labels, hidden flags, element sizes, and weights. It exports USDA and USDC; dispatch tests cover wasm32/memory64, type/shape checks, failed-load atomicity, text/binary export and reload, relationship replacement/query/removal, property metadata export, and removal. Native parser and two-width dispatch tests cover progress, cancellation, and callback recovery. | `beginAsync` matches the legacy coroutine's pre-parse yield but parsing remains synchronous; USDA callbacks are coarse at completed-prim boundaries, so a single large prim cannot be interrupted mid-parse. Broader authoring and composition APIs still require parity review. |
| Composition and variants | Partial | Optional stage composition and next flatten session/converter operations. `NextFlattenSession.addSublayer(path)` appends one authored sublayer path to the retained root before flattening, reserializes through the next Layer writer, and preserves the aggregate input cap with atomic rejection. `addPrimArc(kind, primPath, assetPath, targetPath, listOp)` authors reference, payload, inherit, and specialize arcs on existing root prims; the optional list-op selects explicit, add, prepend, append, delete, or reorder edits. Path validation, external asset resolution, atomic rewrite, and aggregate input budgeting apply to every operation. WASM flatten failures preserve the core pipeline composition diagnostic list through the fixed step record and bounded copies, without changing strict failure behavior; each step also reports source layer identifiers consumed, including the final sorted dependency set on success. Next-only `hasSublayers`/`hasReferences`/`hasPayload`/`hasInherits`, additive `hasSpecializes`, and arc-path extraction inspect authored root-layer metadata. `RenderStream.listVariants` plus `hasVariants`/`extractVariants` expose root-layer variant sets, authored selection, and option names in the legacy shape; `setVariantOverride` applies strongest selections. The scoped override `/Root{lod}` is now tested against a variant set on a referenced dependency: flatten selects the low branch and emits its three-vertex mesh on wasm32 and memory64 (`next-usda-composition.test.mjs`). `lodVariantCount` reports the largest exact-uppercase `LOD` option count. Other fixtures cover sublayer and four-kind arc authoring, explicit/add/prepend/append/delete/reorder list edits, atomic input-budget rejection, arc inspection, inherit/specialize composition, and nested LOD counts. A weaker-layer reorder fixture verifies dependency request order changes from A,B to B,A, and standalone PCP tests verify no duplicate reordered references or payloads. | Other exact legacy selection/composition semantics and full composed export parity remain open. Arc extraction currently inspects authored root-layer metadata, not the composed result. |
| Layer export and validation | Partial | Next flatten and USDZ conversion paths; typed C compatibility methods exist in combined build. `NextFlattenSession.setAssetPathRemap(object)` atomically applies the next pipeline's existing asset-valued-property remap to flatten output. `remapLayerAssetPaths(object)` mutates the retained root layer, supports UTF-8 strings and byte-view replacements, and returns the count of changed asset values. Remap keys/values count against the session aggregate input budget; wasm32/memory64 tests reopen outputs as USDA, verify both forms, check changed/no-op counts, and confirm over-budget replacement preserves the earlier map. Next validation already emits legacy-shaped issue records (`severity`, `rule_id`, `location`, `message`) with summary counts, checked groups, and spec version; validation parity is covered by the existing `validation-parity.mjs` suite. `NextUSDZConverterNative.exportAsUSDZWithOptions({rootLayerFormat})` now supports `usdc` and `usda` roots while retaining packaged assets and configured archive limits; wasm32/memory64 dispatch verifies both root names/payloads and asset retention. The native USDZ writer now exposes a synchronous sink API and file writers stream aligned archive spans directly, preserving ZIP32/output-limit checks without retaining a second archive-sized vector; tests verify byte-identical output and sink-failure propagation. Next-only `LayerDocument.exportUSDC()` retains public-C Crate output, returns an owned JS byte view, and is covered by Crate magic plus binary reload/export tests on wasm32 and memory64. | Converter-facing USDZ exports still retain their returned byte vector; other export option combinations and converter-level sink integration remain open. Direct remapping is scoped to asset-valued properties, matching the next core primitive. |
| Loader configuration | Partial | Input limit, architecture-independent 1 GiB resident-memory default, composition, value-clip evaluation/sampling, sphere subdivision, bone reduction enable, target influence count, rounded influence widths, and on-demand tangent generation. `NextUSDZConverterNative.setUSDCExportLimitMB` enforces the file-size limit in Crate output and USDZ entry/central-directory writes; its working-memory argument applies an early Layer-retention plus per-spec estimate before Crate construction and charges retained USDC/assets plus ZIP output/directory storage before archive writing. Layer estimates include scalar/string-array/dictionary strings and full numeric array element widths. Native Crate/USDZ tests and wasm32/memory64 converter tests cover low and sufficient budgets. The nine general RenderStream converter flags now have strict boolean setters and matching getters through typed C, with true/false round-trip and wrong-type/count dispatch coverage on both WASM widths. | The writer memory policy is an estimate rather than an aggregate allocator/RSS quota; temporary value encodings, hash-table capacity, and exact allocator overhead are not fully charged. UDIM atlas-vs-sparse selection remains an application-layer product decision; settings without a next converter equivalent also remain open. |
| Schema and image utilities | Partial | The nine-method legacy family is crosswalked to live next-only methods in `web/js/tests/schema-image-next-api-inventory.json`, checked on wasm32 and memory64. Next-only C dispatch exposes bounded PNG/BMP/TIFF/DNG output; the baseline TIFF writer emits the same pixel representation for TIFF and DNG, with input/output caps of 256 MiB and 512 MiB. Optional next-only EXR output uses the TinyEXR v3 writer, accepts 1/3/4-channel 8-bit input, emits half-float channels, preflights output against the 512 MiB cap, and has two-width dispatch plus independent `exrheader`/OpenEXR decode verification. The `dng` selector retains legacy behavior and does not add DNG camera metadata. `RenderStream.generateBoneTexture` builds the legacy packed influence texture from typed skin buffers and mesh element size; it checks combined float-texture and vertex-offset output against a 256 MiB cap before allocation, with oversized-geometry and influence-width coverage on both widths. The URDF converter's mesh and scene APIs remain covered. `RenderStream.computeMeshTangents(meshId)` provides the legacy operation's success contract by requesting the bounded next tangent buffer, triggering deferred generation without a separate mutable cache; tests cover generation, unavailable output, wrong arguments, and invalid IDs on both widths. `NextUSDZConverterNative.createSampleScene()` authors the textured quad, PreviewSurface graph, and checkerboard path through the next USDA loader; callers supply the texture with `setAsset` before USDZ export. Two-width dispatch tests inspect USDA output and confirm the supplied asset is packaged. | EXR can be disabled with `LIGHTUSD_WASM_WITH_EXR=OFF`; DNG camera metadata and broader image utility parity remain. |
| MCP | Product decision | MCP context/resource/tool registration is an application/server integration surface, rather than a scene-core capability; it remains in the dedicated MCP server/example product. | No next-only RenderStream implementation is planned; the next-only module does not ship a server transport. |
| Allocation safety | Partial | `RenderStream.begin` and `beginAsync` now call a C-side byte/resident-budget preflight before copying input into JS/WASM staging buffers; rejected payloads preserve specific limit diagnostics and the dispatch test confirms no input-sized WASM allocation. A memory64/wasm32 regression forces WASM memory growth during attached-store refresh after capturing a heap-backed input view and verifies the root still loads correctly after offset rebinding. `LayerDocument.load` caps input at 512 MiB before heap staging, copies existing WASM-heap views by offset across possible memory growth, uses the stage loader's input/resident limits, and preserves the current document when parsing a replacement fails. USDA export retains the C-owned serialized buffer and decodes directly into an owned JS string; USDC export retains C-owned Crate bytes and copies directly into the returned JS-owned byte array. Both avoid a second output-sized WASM staging buffer, asserted by wasm32/memory64 dispatch tests. Other bounded input, resident-memory, provided-asset, and resource-limit paths remain; oversized lazy mesh materialization is rejected before scratch copies. NextFlattenSession caps root plus supplied dependency-layer bytes at 512 MiB by default (configurable to 1 GiB), applies the Crate writer hard output-size limit before output-buffer growth (also configurable up to 1 GiB), and NextUSDZConverterNative accounts the retained root input, packaged assets, and URDF mesh buffers against one configurable 1 GiB aggregate budget, while retaining per-family caps; root replacement preflight runs before the JS/WASM staging allocation. Crate export preflights a retained-Layer estimate with numeric array widths and scalar/string-array/dictionary string capacities; USDZ preflights retained USDC/assets plus archive and directory storage. The optional EXR bridge retains one encoded result for its synchronous JS copy instead of encoding twice and allocating a second WASM output buffer; dispatch tests assert that allocation shape on wasm32 and memory64. `getMaterialRecord()`/`getAllMaterials()` preflight diagnostic strings, material names/paths, and parameter buffers against a 512 MiB aggregate estimate before constructing records. `getSkeleton()` preflights matrix, parent, child, joint-name/path, and compatibility-alias string payloads against a 512 MiB estimate; `getAllSkeletons()` preflights the scene-wide sum before constructing any skeleton object, and `getSkeletonJointsFlat()` reserves a higher matrix-copy allowance for its extra flat arrays. Oversized C buffer reports are rejected by wasm32/memory64 dispatch fixtures. The hierarchy is built iteratively, and the flat getter pre-sizes its numeric output and fills it in one pass instead of allocating `flatMap` intermediates. `getAnimation()`/`getAnimationView()` and `getAllAnimations()` preflight weighted channel-buffer and order-array sizes against a 512 MiB materialization budget before copying keyframe/value/remap arrays; wasm32/memory64 tests inject oversized C buffer sizes and verify rejection before payload copies. String buffers are charged at twice their UTF-8 byte length plus 64 bytes per string; UDIM tile records and all four UDIM metadata strings are size-queried and preflighted before allocation. Residual JS engine object overhead is not exact. `getImageCopy()` borrows the retained contiguous image span only during a synchronous JS `.slice()`; non-contiguous image storage uses the bounded C copy path, with each image capped at 512 MiB. `getAllImages()` uses C size queries for the aggregate decoded pixel bytes and weighted name/path/object estimate before allocating image records or pixel copies. `getMeshPrimvarsJSON()` size-queries the mesh path, all primvar names, and raw/index buffers before allocating those strings or payloads, then bounds raw, expanded, and JSON estimates against 512 MiB; wasm32/memory64 fault-injection tests verify preflight rejection for oversized names and buffers plus indexed expansion and element-size grouping. `getTextureRecord()` rejects oversized decoded images before copying, and `getTextureRecord()` and `getAllTextures()` preflight names, URIs, three texture strings, sampling/color-transform buffers, and decoded image copies against a weighted 512 MiB per-record and aggregate estimate before materialization. Next converter byte exports expose a borrowed pointer into the retained C++ vector for the synchronous JS copy, avoiding a temporary WASM output allocation; dispatch tests verify no `_lightusd_next_alloc` call and confirm the returned Uint8Array remains owned. Native and wasm32/memory64 converter regressions cover memory-budget rejection and success. | Systematic allocation-failure behavior and coverage for all retained/render/export payloads and returned JS copies; estimates still omit exact hash-table/allocator costs and temporary value encodings. |
| Product selection | Default switched | Native, Python, WASM, and JS loader defaults select next. Legacy is deprecated and retained in CI. | Migrate explicit legacy consumers, review documented behavior differences, and retain full regression evidence before legacy retirement. |

The asset/cache row expands to this method-level crosswalk for all 28 methods
classified as `asset_resolution_and_cache` in
`web/js/tests/lightusd-loader-api-classification.json`:

| Combined-module methods | Next-only coverage | Remaining parity decision |
|---|---|---|
| `addAssetSearchPath`, `clearAssetSearchPaths`, `getAssetSearchPaths`, `getBaseWorkingPath`, `setBaseWorkingPath`, `getAllowParentRelativeAssetPaths`, `setAllowParentRelativeAssetPaths`, `hasAsset`, `assetExists` | `NextAssetStore` exposes all nine over the next `AssetResolver`'s working directory, ordered search paths (duplicates retained, as in legacy) and parent-relative flag; `hasAsset`/`assetExists` query registered names. Paired tests match legacy results for every setter/getter round trip. | The default base path is spelled `.` (resolver working directory) instead of legacy `./`. URL and package-path resolution policy beyond these settings remains an application decision. |
| `clearAssets`, `deleteAsset`, `deleteAssetByName`, `deleteAssetByUUID`, `setAsset`, `setAssetFromRawPointer` | `NextAssetStore.clearAssets`, deletion and byte-view registration are covered; raw-pointer ingestion validates the complete heap span, copies once into resolver-owned storage, and preserves the overwrite boolean. `RenderStream.importAssetStore` shares an immutable snapshot, `setAssetStore` refreshes before each load, `detachAssetStore` releases imported resolver entries without clearing stream-local assets, and `finalizeStreamingAssetToStore` transfers complete streams while retaining their UUID and shared-budget lease. | Explicit imports are snapshots; callers must re-import to refresh. Filesystem search paths remain a separate browser/application resolution policy. |
| `findAssetByUUID`, `getAllAssetUUIDs`, `getAssetHash`, `getAssetUUID`, `verifyAssetHash`, `getStreamingAssetUUID` | `NextAssetStore` covers sorted ID enumeration, UUID/name lookup, SHA-256 lookup and verification, and replacement-sensitive UUID identity. RenderStream exposes active-transfer UUID lookup and can preserve that UUID during store finalization. | Store snapshots resolve composition dependencies; re-import after source mutation remains necessary for explicit snapshots. |
| `getAsset`, `getAssetByUUID`, `getAssetCacheDataAsMemoryView` | `getAsset` and `getAssetByUUID` assemble owned `{name,data,sha256,uuid}` records; `getAssetCacheDataAsMemoryView` and RenderStream import retain shared resolver-owned bytes without payload copies. | JS borrowed views expire after store mutation/destruction; RenderStream imports hold an owning snapshot until re-import or stream destruction. Both lifetimes are covered on wasm32/memory64. |
| `getAssetCount`, `getAssetCacheSizeBytes`, `getAssetCacheMaxSizeBytes`, `setAssetCacheMaxSizeBytes` | `memoryStats`/`setMemoryLimitBytes` enforce the store's payload-only bound. Count and cache bytes report standalone entries; the compatibility cap evicts lexicographically first entries on new insertion. Attached RenderStreams share the store's retained-payload budget for imported views, provided assets, and active streams; each stream still applies its own resident-memory limit, including key and transient handoff costs. | Unattached stores and streams retain independent caps; there is no process-wide budget across unrelated objects. |

`next-asset-cache-behavior-parity.test.mjs` now runs one scripted scenario
against the combined legacy loader and the next-only `NextAssetStore` and
`RenderStream` on wasm32 and memory64. It compares every result after
normalizing per-side UUIDs and heap pointers, covering all 28 asset-cache and
15 streaming-buffer methods, and it cross-checks the parity matrix, where these
rows now carry a new `behavior_verified` status and name the test. Forty-one
methods match legacy. Six of them have a pinned `edgeDifference`, where legacy
is non-atomic or accepts degenerate input:
- `setAsset` returns an overwrite boolean and rejects an empty identifier;
- the default base path is `.`;
- an overflowing chunk is not appended;
- a zero-length stream completes and finalizes;
- an over-count zero-copy mark leaves the written count unchanged.

`getMMapZeroCopy`/`setMMapZeroCopy` remain `known_behavior_gap` rows under the
existing mmap product decision. Bringing the adapters to legacy parity changed
four observable results:
- `getAssetCount` and the cache-byte getters return Numbers on wasm32 and
  BigInts on memory64, matching Embind `size_t`. `getAssetCount` also reads the
  identifier count directly instead of copying every identifier.
- `appendAssetChunk` accepts JS strings (UTF-8) and ArrayBuffers.
- `getActiveZeroCopyBuffers` lists only `allocateZeroCopyBuffer` transfers,
  without the extra `exists` key.
- `getStreamingAssetUUID` maps to `RenderStream`, which matches legacy's
  active-transfer semantics. `NextAssetStore.getStreamingAssetUUID` remains
  an alias of `getAssetUUID`.

The unified next transfer model still lets zero-copy calls address a chunked
stream by UUID, which is a superset of legacy behavior.

This crosswalk is the working scope for the first parity family. Each row is a
separate next-only implementation or product-scope decision; the checked
method inventory and two-width dispatch suite validate the owned-record,
borrowed-view, mutation, UUID/hash, cache-limit/eviction, existence, raw-pointer,
and shared-store composition operations implemented here. Explicit snapshots
remain stale until re-import by contract; attached stores refresh before loads
and can now be detached explicitly.

The next-only material format family now routes `getMaterial()` and
`getMaterialWithFormat(id, "json"|"xml"|"legacy"|"")` through typed C. Its adapter converts
the next scene material (including PreviewSurface and OpenPBR parameters plus
texture/image metadata) into the existing serializer input, matching legacy
JSON and XML exactly on PreviewSurface and OpenPBR fixtures. The JSON result is
preflighted against escaped source sizes, a 512 MiB output cap, and remaining
stream memory; the retained C string is released after the owned JS copy.
`material-format-parity.test.mjs` compares JSON, XML, and legacy object results
against the combined loader in wasm32/memory64 regression profiles. The
object result preserves MaterialX defaults, workflow-specific PreviewSurface
fields, texture IDs, and the missing-PreviewSurface error. Its bounded typed
C result is copied into an owned JS object.

The legacy `set/getSphereSubdivisions` pair now maps to next-only
`RenderStream.setSphereSubdivisions()` / `getSphereSubdivisions()` and
`ConverterConfig.mesh.sphere_subdivisions`. The setter accepts only the next
converter's supported 0–6 range and rejects invalid values without changing
the previous setting. A generated-Sphere fixture verifies that level 2 emits
more vertices than level 0; dispatch and exact API-inventory tests pass on
wasm32 and memory64. This closes that configuration pair only; the other
loader settings listed in the matrix remain open.

The legacy `set/getEnableBoneReduction` and `set/getTargetBoneCount` controls
now configure the same `ConverterConfig.mesh` fields in RenderStream. Reduction
defaults to disabled and the target defaults to four, matching the legacy
loader defaults; valid targets are 1–128. A skinned USDA fixture verifies the
default state and that enabling reduction at a two-influence target reduces
the retained joint-weight payload. Invalid boolean types and target values
are rejected. Dispatch and API-inventory tests pass on wasm32 and memory64.
The legacy `set/getRoundBoneCount` pair now maps to the next converter's
standard-width padding option. When enabled without reduction, per-vertex
influence widths round up to 4, 8, 16, 32, 48, 64, 80, 96, or 128; authored
indices and weights are preserved and new slots are zero-filled. Reduction
takes precedence exactly as in the legacy path. The same skinned fixture
confirms widths `[4, 2]` become `[4, 4]`; wasm32 and memory64 builds,
dispatch tests, converter tests, and the RenderStream inventory pass. Padding
checks multiplication overflow and probes available allocation budget before
building temporary buffers; the converter's cumulative retained-skin budget
check still gates the final payload. The standalone native `next_debug` CTest
suite also passes all 43 runnable tests; its optional AOUSD value-resolution
test is skipped when that external fixture is unavailable.

Next-only `RenderStream` now uses its imported `NextAssetStore` resolver during
render conversion, discovers sparse UDIM tiles from those registered assets,
and exposes the legacy `isUDIM`/`udimTextureId` fields plus bounded tile and
metadata queries. A wasm32/memory64 dispatch fixture registers a root USDA and
two non-contiguous tile assets, loads through `beginCachedAsset()`, and checks
the resulting `getTextureRecord()` and `getUDIMTextureRecord()` values. The
legacy `set/getCombineUDIMTiles` atlas mode is still not implemented in the
next converter; exact parity requires bounded decoded-image input, atlas
construction, and mesh UV rebaking, so do not describe sparse support as atlas
parity.

The legacy `set/getDeferTangentComputation` switch has no direct next-only
setter because RenderStream already uses the deferred behavior: converter
geometry retention is disabled, and tangent scratch is generated when a caller
requests the tangent buffer. The next-only tangent regression checks the
request-driven buffer and confirms it is absent when tangent output is not
requested. Legacy eager retention would require changing RenderStream's
memory policy; it remains an explicit product decision rather than a
no-effect compatibility flag.

The next-only `RenderStream.maxInputBytes()` / `setMaxInputBytes()` setting
maps to `next::ResourceLimits.max_input_bytes`. It defaults to the untrusted
load policy (512 MiB), accepts 1 byte through 1 GiB, and is enforced before
`begin()` stages input bytes in WASM memory. The same value is applied to USDA,
USDC, and USDZ reader options. A wasm32/wasm64 dispatch test lowers the limit
and verifies that over-limit input returns a diagnostic without calling the
WASM allocator. This does not map legacy render or skeleton tuning controls.

Next-only `set/getMaxMemoryLimitMB` now maps the resident budget into USDA/USDZ
load limits, the direct Crate reader cap, and `ConverterConfig.limits`. It is
separate from `maxInputBytes`. The next product intentionally keeps the
architecture-independent 1024 MiB `ResourceLimits` default as its stricter
untrusted-input resident policy; the legacy WASM render defaults remain 2048
MiB on wasm32 and 8192 MiB on memory64. Dispatch tests pin the 1024 MiB default
on both pointer widths and verify that lowering it does not change the 512 MiB
input limit. The lazy RenderStream accounts for
provided asset names/payloads with root input size, passes only the remaining
budget to the reader, then reserves `Stage::GetMemoryUsage()` before giving the
remainder to conversion and lazy mesh views. The mesh-view path estimates source and
materialized geometry before allocating scratch vectors and returns a specific
error if it exceeds that remainder. A 50,000-point USDA fixture loads under a
1 MiB resident cap but refuses mesh-view materialization; the test also
confirms the input limit remains at 512 MiB. A separate test rejects a padded
root layer when its 900 KiB provided asset leaves insufficient resident space.
wasm32 and memory64 builds and dispatch tests pass. This bounds input, provided
assets, reported Stage memory, converter output and per-mesh scratch. Typed
RenderStream buffer copies now reject a single payload above 512 MiB before
allocation, then query the remaining resident allowance; shared UTF-8 string
copies, light string copies, and mesh-view diagnostic strings also query that
allowance before allocating WASM staging memory. Tests inject an oversized
typed mesh-buffer size and verify no copy allocation occurs on wasm32 and
memory64. This
is a per-copy bound: JS-owned arrays live beyond the
call and are not registered against a cumulative lease budget, while borrowed
lazy-array backing storage is not fully accounted. Those lifetime-wide
accounting gaps remain open.

Next-only RenderStream now accepts chunked dependency assets through
`startStreamingAsset`, `appendStreamingAsset`, `streamingAssetProgress`,
`finalizeStreamingAsset`, and `cancelStreamingAsset`. Staging reserves the
expected byte count up front and contributes to both provided-asset accounting
and the stream resident-memory budget; the configured asset byte cap applies
while transfers are incomplete. The fixed range-bookkeeping allowance and the
peak payload needed for a transfer replacement are preflighted before allocation;
a rejected replacement preserves the active transfer. Finalization publishes
the asset to the same resolver map used by `provideAsset`. `beginStreamedAsset` can instead consume a
complete upload as the root layer, moving the owned bytes directly into the
existing load path without assembling a second JS buffer. A dispatch regression
verifies partial progress, rejection of incomplete finalization/root load, exact
binary bytes after finalization, cancellation, root ownership transfer, direct
WASM-view writes with explicit progress marking, and both wasm32/memory64 wrappers. This closes the owned chunk-transfer lifecycle. Borrowed views can address a checked range at any offset. Written intervals
are merged and overlapping marks count unique bytes only, so finalization cannot
succeed while a hole remains. A fixed 128-interval table bounds bookkeeping;
more fragmented transfers are rejected. Views become invalid on finalize,
cancel, root consumption, replacement, clear, or stream destruction; callers
must reacquire them after any operation that could grow WASM memory. The legacy
mmap-zero-copy setting is an explicit next-WASM product decision:
`RenderStream` accepts counted memory buffers only, and
`CrateReadOptions::use_mmap` is a file-path optimization with owned-buffer
fallback on WASM. The next stream always owns/adopts the supplied bytes and
retains lazy arrays against that storage, so a second mmap switch would have no
effect on this interface. The native path reader keeps its independent mmap
setting.

`RenderStream.setProgressCallback()` now connects the next Crate reader's
phase callback to the caller and propagates a `false` return to cancel a USDC
load. Callbacks report phase, current, total, and percentage; callback exceptions
are rethrown after the reader returns, and the callback is scoped to the
synchronous load. A USDC fixture verifies bootstrap-through-complete reporting,
cancellation at bootstrap, diagnostic text, and successful reuse after removing
the callback on wasm32 and memory64. This does not claim USDA parser progress,
asynchronous load control, or cached-layer API parity.

The next-only `setEnableComposition()` control now selects whether the render
stream runs `StageNeedsComposition` / `ComposeLoadedStage` after loading. It
defaults to enabled to retain existing next behavior. An inherited-mesh fixture
checks that the derived mesh receives inherited topology when enabled and does
not when disabled on wasm32 and memory64. The similarly named legacy loader
flag is unused in its load path, so this is an explicit next product control,
not a claim of legacy setter parity.

Value-clip sampling configuration now crosses typed C through the next-only
stream: sample rate, explicit-range enable, range start, and range end each
have validated setters and queries. These values populate
`ConverterConfig.animation`; a clip-backed transform fixture confirms the
default key times `[0, 1]` become `[0, 0.5, 1]` at 2 Hz over the explicit
0-to-1 range. The sample count stays bounded by the next resource limit. This
maps the legacy value-clip sampling controls to the next converter path; the
`setEnableValueClips` control now also maps to the next converter path. It
defaults to enabled; disabling it omits the clip-stage resolver and animation
baking. On wasm32 and memory64, tests verify that the toggle suppresses both a
clip-backed transform animation and clip-backed curves. Other loader settings
remain open.

The combined `nextFlattenUSDC(data, lazyArrays)` compatibility method now uses
a counted byte input, a fixed-layout stats record, and bounded output/error
copies. Its JS wrapper keeps the existing result object, validates a live
loader receiver, handles subarray offsets and memory64 pointers, and copies
the output before releasing native buffers. `nextFlattenBuffer(uuid,
lazyArrays)` now crosses the same typed C boundary with a live loader handle
and counted UUID; the native side consumes the loader-owned zero-copy buffer
and moves its bytes into the flatten pipeline. `nextFlattenAsyncEnd(session)`
now passes its counted session ID and live loader handle through C, preserving
the `{success}` result and per-loader session map. `nextFlattenAsyncProvideLayer`
passes counted session, key, and byte-view data, preserving path normalization,
replacement of any cached parsed layer, and its result errors. Basic
`nextFlattenAsyncBegin` consumes the root buffer and returns the generated
session ID in a bounded output buffer. The remap and remap-plus-variant async
begin and buffer overloads use counted string-map records and bounded result
copies. Variant maps preserve filtering for null and empty entries. The combined
flatten regressions pass on wasm32 and memory64, including byte-view handling,
unknown buffers and sessions, invalid payloads, and repeated session
termination.
The remaining 8 callback-taking methods now cross the same boundary. They are
`nextFlattenAsyncStep`, the three `nextFlattenBufferToSink` overloads, and the
four `nextFlattenMultiBufferToSink` overloads. The JS adapter registers each
chunk-sink, layer-exists, and layer-fetch callback under a per-call ID. The
native side reaches those callbacks through EM_JS bridges, so no `emscripten::val`
remains on the flatten path. A fetched layer is copied once, into the native
layer string. The first callback exception stops that call's later callbacks,
and the adapter rethrows it after the C call returns. Before this change the
exception unwound through the running pipeline.
All three calls fill one 96-byte step record. It carries status (rejected,
error, sink-abort/ready, need-layer, done), stats, and the counts of the
need-layer key, referenced asset paths, and composition errors. The strings
are copied through a bounded indexed query. The native side publishes results
only after the pipeline returns, so a callback that re-enters another flatten
cannot clobber the running one. The adapters rebuild the previous result
objects: `data` only when buffered, `assetPaths`, at most 20
`compositionErrors` with the full count, and step status strings. They keep the
"aborted by sink" and unknown-buffer/session errors. Three things changed from
Embind:
- The overloads of one family share an adapter and accept that family's
  argument-count range.
- Non-function callbacks and non-string remap values throw `TypeError`.
- The fetch callback may also return an `ArrayBuffer`.
`parseAssetPathRemap`, `parseVariantOverrides`, `copyUint8ArrayToString`, and
the unregistered Embind async-begin remap bodies are removed. The
anchor/raw/UE-suffix key resolution shared by the multi-buffer and step
resolvers is now one helper.
New regressions were written first and passed against the Embind build on both
pointer widths. They then passed unchanged on the typed build. They cover:
- streamed output byte-equal to buffered output;
- sink abort, and a thrown sink, exists, or fetch exception;
- exists/fetch dependency loading, failed fetches, and unresolved-arc
  composition errors;
- the need-layer, provide, ready, and done step loop, plus a step parse error;
- unknown buffers and sessions.
Receiver, callback, and map-type checks and a re-entrant sink were added
afterwards. The checked classification now records all 17 next-flatten methods
as `typed-c`. The web Node regression profile passed 24/24 after the next-only
modules were rebuilt. The first run failed `next-only usdzconvert` on a stale
09-23 next-only artifact that predated the working-tree `next-api.js`. Size
savings were not measured.
The composition-and-variants family now crosses typed C in the combined
product. That covers the `has*`/`compose*` pairs for sublayers, references,
payload, inherits, and variants, plus `lodVariantCount`, both
`applyVariantSelection` overloads, the three `extract*AssetPaths` lists, and
`extractVariants`. The calls work as follows:
- The eleven boolean and count calls go through one
  `lightusd_combined_layer_op(loader, op)` export.
- The variant-selection overloads have counted-string exports.
- The string lists fill a retained string table that JS copies through a
  bounded indexed query.
- `extractVariants` flattens its prim/set/option tree into that table plus a
  u32 shape: a prim's set count followed by each set's option count. The
  adapter rebuilds the same `{primPath, variantSets: [{name, selection,
  options}]}` objects.

`binding.cc` shares one C++ collection path for asset paths and variant info
between the two products. The legacy product keeps its Embind registrations
and `emscripten::val` builders behind `!LIGHTUSD_WASM_WITH_NEXT`. The combined
product no longer registers or compiles them. The adapters keep the legacy
results and error messages. That includes the existing quirk that
`composeInherits()` on an empty loader returns `true`. Like the flatten
adapters, they throw `TypeError` for wrong argument counts, non-string names,
and dead receivers. The new regression was pinned first against the Embind
combined build on wasm32 and memory64, then passed on the typed build on both.
It covers:
- all queries before and after each pass;
- the three asset-path lists and the variant tree;
- the composed layer text;
- selection argument errors;
- empty-loader results.

The rebuilt legacy product gives identical composition results through
Embind. Rebuilding the legacy `lightusd.js` exposed a separate harness issue.
Four web regression suites (usdzconvert helpers, combined RenderStream,
next usdzconvert, and next USDA composition) load `lightusd.js` but need the
next core, which the legacy product excludes. They passed before only against
an older artifact built before the product split. These four suites now load
`lightusd_combined.js` (`lightusd_combined_64.js` under `LIGHTUSD_WASM64=1`).
They pass on wasm32: usdzconvert helpers 108/108, RenderStream 19/19, next
usdzconvert 2/2, and next USDA composition 6/6. The last three, which support
memory64, pass there too. The web Node regression profile passes 24/24 with a
true legacy `lightusd.js` in place.
The loader-configuration family now uses typed C in the combined product, with
the same legacy-only Embind gating:
- The 30 scalar getters and setters go through one keyed export pair,
  `lightusd_combined_config_set(loader, key, a, b)` and
  `lightusd_combined_config_get(loader, key, &out)`. Values cross as doubles,
  and the two-value setters (`setValueClipTimeRange`, `setUSDCExportLimitMB`)
  use `b`.
- `getMemoryStats` reads a 104-byte POD record. JS derives `bufferMemoryMB` from
  it with the same arithmetic.
- `debugLogMemory` passes a counted label and returns the heap byte length.

The native setters keep their range filters: subdivisions outside 0-6 and bone
counts outside 1-128 are still ignored. The adapters reproduce Embind's
argument conversion, which the new regression pinned against the Embind build
first:
- bools use JS truthiness;
- numeric arguments accept a number or boolean and throw `TypeError`
  otherwise;
- integers throw `TypeError` outside their C range, and otherwise truncate,
  with NaN becoming 0;
- floats round through `float`.

A side-by-side probe of the rebuilt legacy module and the combined module
returns identical values and the same throw/no-throw outcomes for every case.
The only differences are that error messages are now per-method, and that wrong
argument counts and a non-string `debugLogMemory` label throw `TypeError`
instead of Embind's `BindingError`. No source outside the generated glue
references `BindingError`. `getAllLights` was reclassified into render scene
queries, since it only loops over `getLight` and moves with that family.
The regression passes on wasm32 and memory64, and the web Node regression
profile passes 24/24.
The combined-only `testLayer` memory probe had no source or test consumers and
returned a constant zero byte count while allocating a legacy `Attribute`.
Its implementation and Embind registration are removed; the live combined
module inventory now verifies the API is 208 methods and explicitly rejects
reintroduction. The combined WASM target rebuilt and its variant-overload test
passed. Compile-time or linked-size savings were not measured against a
pre-change baseline.
The next web `getNode()` compatibility method now builds node records from
typed fields, bounded name/path copies, matrix buffers, and child IDs. The
C++ aggregate node method and its emval dispatch case are removed. The
next-only adapter test rejects that dispatch during scene loading in wasm32
and memory64.
`getUnsupportedRenderables()` now reads bounded path/type/reason strings, and
`getPoints()` combines a fixed-layout point record with owned geometry arrays.
Their C++ emval methods and dispatch cases are removed; point conversion no
longer keeps duplicate scratch vectors alive across stream updates.
Both pointer-width builds pass the C dispatch and next-only adapter suites
after this migration. The full web Node profile passes 24 suites, and the
color-space backend parity test remains green.
`getCurves()` now follows the same path. Its old C++ object builder and six
scratch vectors are removed; the bounded C export now covers tessellated
widths/colors and authored/tessellated opacities. The scalar interpolation
query also accepts fields 5–7, which its previous range guard rejected. A
curve fixture checks those values and buffers in wasm32 and memory64.
`getCamera()` also reconstructs its JS object from a fixed-layout optics
record plus the existing bounded transform and resource strings. Its C++
emval method and dispatch case are removed. The fixture checks focus,
aperture offset, exposure, stereo role, and shutter timing on both pointer
widths. The native C camera record now carries the same values, and its
installed-header C++ consumer and strict C11 C API test pass.
`getPointInstanceDraw()` now reconstructs its JS object from typed draw fields,
the bounded transform buffer, and checked mesh/material resource paths. Its
C++ emval method and dispatch case are removed. The instancer fixture checks
the fields, transform, and available paths in wasm32 and memory64.
`getPointInstancer()` now reads a fixed-layout count/flag record, bounded
prototype paths and validation text, and typed prototype binding buffers.
The missing bounded instancer-name path was added to resource-name copying;
its C++ emval method and dispatch case are removed. The same fixture passes
the C-dispatch and next-only adapter suites on both pointer widths.
`getSkeleton()` now rebuilds joint records from typed parent and matrix
buffers, bounded names/paths, a new ordered child-ID copy, and the animation
source path copy. Its C++ emval getter and dispatch case are removed. The
skeletal fixture passes C-dispatch and next-only adapter tests on wasm32 and
memory64.
`getAnimationInfo()` and `getAllAnimationInfos()` now assemble their public
summary objects from the fixed-layout clip record, bounded clip-asset paths,
and resource names. The typed target-node count now uses distinct node IDs,
matching the former aggregate getter. Both C++ emval methods and dispatch
cases are removed; the skeletal fixture passes on wasm32 and memory64.
`getStats()` now combines the existing core stats record with a compact POD
detail record for merge flags, conversion timings, cache counters, provided
asset and mesh-buffer bytes, and scene resource counts. Its JS result retains
the previous mesh-only/scene-available field behavior. The C++ emval builder
and dispatch case are removed; C-dispatch and next-only adapter tests pass on
wasm32 and memory64.
`getLight()` now uses a fixed-layout light record and bounded copies for IES
and CollectionAPI link strings, resolved mesh IDs, and dome texture paths.
The C++ aggregate getter, its dispatch case, and its now-empty translation
unit are removed. Sphere, rect, cylinder, dome, and linked-light fixtures pass
the C-dispatch tests on wasm32 and memory64; both next-only adapter suites
also pass.
The post-light full web regression passed 27 suites, including the 67-model
Menagerie MJCF closure. Its two browser sweeps could not bind a local Vite
server inside the restricted sandbox (`EPERM` on loopback). Rerunning those
same sweeps with loopback access and the installed Chrome passed 5/5 URDF
screenshots and 67/67 OffscreenCanvas models. The original single-run
`npm test` summary remains 27 passed, 2 environment-blocked failures; the
browser results are separate reruns.
`getSceneMetadata()` also reconstructs the public object in JS from one
fixed-layout POD record and bounded strings. The native emval metadata method
and dispatch case are removed. Its regression checks authored stage units,
timing and up-axis values on wasm32 and memory64, including the mesh-only
conversion path.
Tydra's retained MaterialX graph JSON now includes explicitly authored input
color-space metadata. This closes the color-space backend parity failure for
the graph path that the web renderer actually consumes.
PreviewSurface utility graphs are now retained in `RenderMaterial` and copied
through the same bounded native C API used for OpenPBR/volume graphs. The
next-WASM material adapter consumes that retained graph before its Stage-backed
fallback. The installed C++ facade graph fixture and the next-only WASM
`usdzconvert-next-only` runtime suite pass with a connected PreviewSurface
`NodeGraph` fixture on wasm32 and memory64.
The Tydra memory-accounting test also grows this retained PreviewSurface JSON
and verifies that `RenderScene::memory_usage()` tracks its string capacity;
the standalone `next_test_tydra` CTest passes after rebuilding.
The focused native `next_test_tydra`, wasm32/memory64 C dispatch and next-only
adapter suites, the color-space backend parity test, and all 24 web Node
regression suites pass after these changes.
The next WASM configuration disables the standalone shared C API target:
Emscripten otherwise maps both static and requested shared variants to the
same archive name in a fresh Ninja tree.
Rebuilt next-only wasm32 and fresh memory64 builds pass the C dispatch and scene
adapter tests after this removal. The web Node regression profile passes all
24 suites; validation parity is skipped because native `lusdcat` was not built
for that profile.

## API migration in this change

`TypeInfo` is immutable metadata. Its `construct`, `destruct`, `copy`, `move`,
and `equals` callbacks and `RegisterTypeInfo` have been removed. The old registry
had no extension ID space: every valid nonzero ID was already occupied. Use
`Value` for ownership/copy/equality and `GetTypeSize`/`GetTypeAlignment` for
layout queries. `InitTypeRegistry()` remains a compatibility no-op.

`GetTypeIdFromName(data, length)` accepts a bounded, non-NUL-terminated name.
Existing NUL-terminated callers remain supported. Type IDs and USD names have
not been renumbered.

`Dict::entries` is replaced by the read-only `entries()` accessor. Iterate
through that accessor and mutate values through `find` or `set`; direct key
replacement/reordering is no longer supported. `Dict::index_` is private, and
name-interning tables are non-copyable. Index allocation failure falls back to
searching authoritative storage; it cannot drop a value or interned name. This
does not yet make remaining vector/deque allocations recoverable on
out-of-memory.

The installed C++ ownership facade now also covers memory-backed stage loading,
warning draining, generation and stage statistics, time-code bounds, and the
typed render-resource info/buffer calls, including node children, mesh
blend-shapes, skeleton joints, and instancer buffers. These wrappers keep consumer code on
the public C/POD boundary, including scene warnings and named material
parameters, while retaining the same generation and ownership
rules as the C ABI. Prepared render-session updates have an RAII wrapper that
aborts an uncommitted transaction automatically, while guarding cleanup if the
owning session has already gone away.

The facade invalidates that cleanup token before a session reset or recreation,
so a prepared update cannot abort through a replaced native session handle.
The regression covers both destruction and explicit session recreation.

Render-scene memory accounting now includes retained USD Physics annotation
vectors, strings, extension properties, filtered pairs, and articulation roots;
physics-heavy snapshots no longer under-report their budget usage.

The facade regression also loads an in-memory USDA buffer through this public
path and verifies the resulting root prim, keeping the no-filesystem consumer
case covered.

The portable render boundary now also exposes retained USD Physics annotations
as bounded POD records: category counts, common scalar/vector fields,
extension properties, filtered-pair paths, and articulation-root paths. The
C++ facade forwards these queries, and its regression converts an in-memory
PhysicsScene to verify the path and gravity fields.

Retained authored material parameters are likewise available as typed shader,
name, texture-id, and value records, preserving unsupported/degraded material
inputs without requiring aggregate emval objects.

Scene memory accounting includes the diagnostic and retained-parameter string
storage behind those records, so exposing the payload does not create an
unbudgeted allocation category.

Skeleton bind/rest matrices and parent indices now have explicit cached typed
buffer exports alongside per-joint info, removing another repeated aggregate
copy from native consumers.

Texture info records also carry the retained KTX2 companion hint, selected UV
primvar, and source/target color-space strings, keeping the texture resource
metadata complete at the C/POD boundary.

Light records now expose the retained shaping IES asset and bounded copies of
light-link, shadow-link, and filter-target paths.

Scene info now includes the retained working-to-display-linear 3×3 color
matrix, so color-management consumers do not need to reach into Tydra scene
storage.

The Tydra-enabled install tree exports both C++ facade headers and the render
library; a standalone C++17 translation unit including the installed headers
compiles successfully. A separate CMake consumer using `find_package(LightUSD
CONFIG REQUIRED)` also configures and links against `lightusd::c` and
`lightusd::render_c` from that install tree.

The standalone next product now builds the shared C ABI by default alongside
the static archives. A pure-C `find_package` consumer links against
`lightusd::c_shared` without selecting a C++ linker; the static targets remain
available for applications that already use a C++ link step.

Render-session POD callers can use the ABI-provided initialization helpers for
update info, change sets, and event sinks; these set the ABI size field and
clear future fields before a call.
The C++ facade forwards these as `InitRender*` helpers for consumers that stay
in the ownership facade.

The Stage facade also covers metadata reads/writes, default-prim selection,
and owned sublayer-list retrieval, preserving the C ABI's generation and
ownership rules. USDZ-with-assets output is also available through the same
facade, along with owned property-explanation JSON and borrowed custom-layer
data access. `InitLoadOptions` and `InitSaveOptions` preserve the same
versioned-struct initialization rule for file I/O options.

`Prim` now forwards property enumeration/flags/type names, child lookup,
attribute metadata and connections, and local/world transforms through the
same facade, plus relationship targets and variant-set/selection queries.
Scalar/string attribute reads, property-existence checks, time-sample counts and
times, sampled value lookup, and explicit interpolation are also available
without exposing the underlying handle.

The core-only install omits render headers and targets as intended, and a
separate `find_package(LightUSD CONFIG REQUIRED)` consumer links successfully
against `lightusd::c` alone.

`PropNameTable::get` now returns `std::string_view`, replacing the former
`const std::string &`. The view is stable for the lifetime of the table,
including after later names are interned; callers that need owned storage must
make an explicit `std::string` copy. `PrimSpec` keeps its cold declared
property-type spellings as owned strings so `property_type_name()` can retain
its pointer-returning API.

### Tydra buffers and next WASM dispatch

`ChunkedArray<T>` requires trivial elements. Allocation, reference counting,
detachment, compaction and bulk copying compile once in `chunk-storage.cc`.
Over-aligned elements and exact-sized tails are supported. Allocation failure
from reserve/resize/append remains recoverable; explicit reference-returning
mutation aborts if detachment cannot allocate, so reserve first when failure
must be reported to the caller.

`operator[]`, `at`, `front`, `back`, `chunk_data`, and ordinary iteration are
now read-only, even on mutable arrays. Use `mutable_at`, `mutable_chunk_data`,
or `mutable_begin`/`mutable_end` to write. Reads never detach shared geometry.
Borrowed pointers/iterators must not survive mutation, sharing, or destruction
of their owner. No source-local optimization override is used for the Tydra
coordinator.

The next WASM post-JS wrapper keeps the existing converter, flatten session,
subdivision, render-stream methods and aliases. Objects must still be explicitly
`delete()`d; `isDeleted()` is supported. Embind-specific conveniences such as
`clone()` and `deleteLater()` are not part of this API. Native IDs are uint32
on both wasm32 and memory64, and exhausted generations retire instead of
wrapping. The exports in `web/binding-next-api.h` are the internal browser C
ABI, not the installed native C API. They use generation-checked uint32 handles
and typed counted inputs, POD records, or bounded output copies; the next-only
module has no emval object dispatcher. Do not call destruction directly from
an active object's callback; the JS wrapper rejects this operation.

Binding scene JSON now uses MiniJSON, including explicit non-finite-to-null
conversion. MiniJSON's finite-double serializer uses the existing vendored
Dragonbox implementation and integer formatting, avoiding libc++'s separate
large floating-format tables. Numeric values are preserved, but equivalent
JSON number spelling and object ordering are not promised byte-for-byte.
`RenderStream` implementation is split into small private translation units;
no per-method Embind registration templates remain.

PCP arc expansion, relocation, layer-stack construction and opinion composition
now have out-of-line private implementations. Shared implementation types live
in `pcp/cache-internal.hh`; do not expose this header to consumers. Validation
has separate physics/lux and report/registry translation units with a private,
non-template helper boundary.

PCP container construction and destruction are also compiled once in
`pcp/cache-storage.cc`, including for temporary parallel-worker caches.

`GetValidationRuleTable(size_t* count)` returns a borrowed pointer to immutable
`ValidationRuleInfo` records instead of a vector reference. Records have process
lifetime, and `count` may be null. The checker is migrated to indexed traversal.
Both the rule registry and the small severity-upgrade list use constant data,
with no dynamic initialization or hash-table allocation.

## Measurements

The root build can select the standalone next implementation explicitly:

```sh
cmake -S . -B build_ninja/next -G Ninja \
  -DLIGHTUSD_NATIVE_PRODUCT=next -DLIGHTUSD_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_ninja/next
ctest --test-dir build_ninja/next --output-on-failure
```

This exposes `lightusd_next` / `lightusd::next`, the next C API and render
targets, and next tools. It does not alias legacy C++ interfaces. The root
test/thread options initialize their `LIGHTUSD_NEXT_*` counterparts; explicit
next options take precedence. Other legacy feature options do not configure
the selected next product. Use a separate build directory for each product.

Configure a separate Ninja tree with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. Do not
run timing passes concurrently with other builds or benchmarks. Reports contain
local command paths and belong in ignored build directories, not public history.

```sh
cmake -S src/next -B build_ninja/refactor-next -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python3 scripts/bench-compile.py --build-dir build_ninja/refactor-next \
  --target lightusd_next --repeat 3 --jobs 16 \
  --artifact build_ninja/refactor-next/liblightusd_next.a \
  --output build_ninja/refactor-next/clean-build.json
python3 scripts/bench-compile.py --build-dir build_ninja/refactor-next \
  --source src/next/types/value.cc --source src/next/types/array-storage.cc \
  --source src/next/types/value-dict.cc --source src/next/core/string-index.cc \
  --source src/next/types/type-info.cc --source src/next/parser/value-parser-types.cc \
  --output build_ninja/refactor-next/core-compile.json
```

For WASM, configure through `emcmake` with MinSizeRel and
`LIGHTUSD_WASM_PRODUCT=next`. Set `LIGHTUSD_WASM_OUTPUT_DIRECTORY` to an absolute
path inside the measurement build tree to avoid replacing the application's
generated modules. Record the `.wasm` and `.js` files with `--artifact`.

Compare the sum of all affected translation units, including extracted files.
Object sections and final artifacts matter independently of object-file bytes.
RSS from the runner is the largest child process, not aggregate parallel-build
memory. Keep compiler/SDK versions, flags, feature coverage and parallelism
fixed. Evaluate LTO separately. Each optimization must improve its declared
metric beyond repeated-run noise; reproducible runtime or peak-memory regressions
over 5% block the milestone. Final acceptance needs smaller whole-product
build times, object text/data, and shipped WASM at equivalent coverage.

### Initial measurements (foundation changes)

GCC Release, 16 build jobs, three clean builds per version: median whole-core
build time was 24.89 s before and 24.76 s after, which is within measurement
noise. The six affected/extracted translation units totaled 3.27 s versus
3.57 s for the original three, with object text/data dropping from 79,689 to
68,394 bytes. The native archive shrank about 1%. MinSizeRel wasm32 decreased
from 1,680,837 to 1,676,336 bytes (about 0.27%); this is a small foundation
improvement, not completion of the size goal.

Seven alternating synthetic runtime runs measured median name interning plus
lookup at 45.3 ms versus 76.2 ms, dictionary insertion plus lookup at 95.4 ms
versus 104.7 ms, and array detachment at 82.3 ms versus 83.1 ms. Combined peak
RSS fell about 9.5%; `sizeof(Dict)` fell from 80 to 48 bytes on this platform.
These measurements precede the read-only dictionary accessor and root product
selector; rerun the commands above for the final configuration being evaluated.

## Validation of the foundation changes

### Final accepted checkpoint

The completed PCP extraction includes explicit out-of-line implementation
construction/destruction. Its final cache-disabled Emscripten 4.0.14 wasm32
pass measured cache/arcs/relocates/layers/opinions/storage at
6.25/5.43/4.55/4.38/4.21/4.13 seconds respectively, versus the original cache
TU's 9.00-second sweep result. Before the last lifecycle extraction, three-run
medians were 6.65/5.40/4.60/4.35/4.17 seconds for the first five files.
Validation main/report/physics medians were 5.90/2.01/2.28 seconds.
The 164-configuration initial production sweep found only the original PCP
and validation TUs above six seconds; those were subsequently decomposed.

Final normal wasm32 is 1,626,756 bytes (text 1,466,675; data 150,845), versus
the comparable pre-refactor 1,689,884-byte product. Memory64 is 1,827,671 bytes.
The separate LTO trial at the preceding checkpoint was larger, at 1,667,514
bytes, and was not adopted. Splitting improves maximum TU latency but increases
aggregate compile work and pre-link object text. No whole-product clean-build
speedup is claimed. These measurements do not authorize further optimization;
the accepted stopping point above supersedes the former size goal.

Final native and threaded ASan/UBSan suites each passed 37 runnable cases, with
one optional conformance skip. Both WASM widths built. wasm32 composition and
memory64 next-only composition/variant/diff/validation, dispatch and render smoke
checks passed. A mixed memory64 suite's legacy case lacks
`nextFlattenAsyncBegin` in the existing legacy module; no legacy rebuild or
workaround was introduced. Immediately before the lifecycle-only extraction,
Python passed 133 tests (seven skips), the legacy MiniJSON unit suite passed,
and the full Node profile passed 18 suites with the known EXR helper failures.

The current web Node profile was rerun after the facade and render-session
changes: 24 suites passed with no failures. It covered the next RenderStream,
USDZ conversion, C dispatch, skeleton animation, typed-array ownership, and
memory64 capability checks; validation parity was skipped because the native
`lusdcat` executable was not present in that web build directory.

A mixed-function GCC buffer microbenchmark retained a register-spill slowdown;
isolated read kernels did not reproduce it. A generated 30,000-triangle WASM
conversion measured approximately 47 ms versus 49 ms at equal heap usage.
This is not comprehensive runtime parity. See `resume-refactor.md` for the
stopped-work handoff and remaining caveats.

### Earlier foundation validation

- Standalone next, root-selected next, and threaded ASan/UBSan builds each
  passed all 37 runnable CTest cases; one optional conformance case was skipped.
  A focused ThreadSanitizer run covered the changed storage and name-table code.
- The rebuilt Python extension passed 133 tests with seven skips.
- wasm32, memory64, and combined WASM products built successfully. Both pointer
  widths passed the next-only JS suite. The full Node profile passed 17 suites;
  the helper suite retained three EXR resize/transcode/roundtrip failures also
  observed before this core change. The full web gate additionally lacks the
  Menagerie dataset in this environment.
- The existing native build, including tools and viewer, completed. Its full
  CTest run had 251 passes, 21 skips, and six viewer failures; all six passed on
  targeted rerun after enabling configure-time NVIDIA detection outside the
  sandbox and installing Pillow. The llvmpipe fallback could not support the
  full OpenGL shader. No renderer source workaround was added.
- The newer local OpenUSD oracle found 450 equivalent USDA files and 223
  equivalent USDC files, with no unexpected output differences (six USDA
  expected failures, three expected differences, four USDC expected failures).
  One malformed USDA fixture was rejected by the oracle, so this is not an
  unconditional corpus pass.
- The current native `build_ninja` CTest run completed 270 passed / 27 skipped /
  3 failed tests. The two reproducible failures are OpenGL texture-semantic
  AOV comparisons; the Vulkan, CUDA, and HIP variants pass. The workflow-tools
  failure found in that run was fixed by preserving file-backed property-trace
  provenance and now passes in isolation.

The web failures and missing coverage mean the product parity gate is still
open. This change does not complete the raw-buffer, scene-handle, binding, or
default-product migrations listed above.

## Consolidation measurements and validation

Linux, GCC 13.3 Release and Emscripten 4.0.14 MinSizeRel, Ninja `-j16`,
compiler caches disabled. Native measurements alternated the baseline
(`3d3eea8`) and changed source trees on the same disk, three samples each.
The old combined `lightusd_c` target is compared with the new
`lightusd_render_c` target and all its dependencies; both include Tydra.
WASM uses the same next-only product, SIMD and non-threaded configuration.

| Measurement | Before | After |
|---|---:|---:|
| Native clean build, median wall seconds | 27.98 | 26.62 |
| Native clean build, median compiler CPU seconds | 406.98 | 382.42 |
| WASM clean build, median wall seconds | 42.41 | 35.53 |
| WASM clean build, median compiler CPU seconds | 592.23 | 493.61 |
| WASM raw bytes | 1,654,594 | 1,653,568 |
| WASM gzip bytes | 667,117 | 666,324 |

Native paired wall-time samples were 26.89→25.43, 27.98→26.62 and
28.53→27.51 seconds. This is about a 5% native and 16% WASM median improvement.
An earlier native pass varied enough to show a small slowdown; the alternating
comparison above includes the final out-of-line mesh/material lifecycle change.
These are library-target measurements, not a timed full viewer build.
Viewer material support now compiles once instead of seven times, and next
WASM subdivision support once instead of twice.

A synthetic 512-mesh conversion smoke benchmark (nine iterations, one worker)
kept the same scene checksum, `e7168a5206495d2d`. Median conversion was
38.892→38.486 ms; process peak RSS was 59,868→60,128 KiB. This checks one
workload, not a general runtime or memory guarantee.

Reports are local ignored artifacts: `build_ninja/consolidate/final-pair-*.json`,
`build_ninja/consolidate/incremental-final.json`, and
`web/build_ninja/consolidate/after-final.json`. The benchmark records compiler
versions, source fingerprints and commands. Baseline source was separately
archived from the commit above; the report's working-tree fingerprint describes
the active checkout, not that archived baseline directory.

Validation completed for the core-only Debug build (37 passed, one optional
skip), render-enabled next (40 passed, one optional skip), threaded ASan/UBSan
(40 passed, one optional skip), Python (135 passed, seven skips), installed
core/render consumers and the shared-library C11/C++ facade tests. The native
300-test run had 271 passes, 28 skips and one viewer startup timeout; that
single test passed in isolation. Ten focused native regressions and the next
and sanitizer suites passed after the final lifecycle changes.

The current native Ninja tree also rebuilds cleanly with the viewer, next
workflow examples, and the C/POD support targets enabled. The focused
`next_gltf_export`, `next_workflow_examples`, and `unit-test-next` tests pass,
as do the render-session C example, C++ facade, and event-stream tests. This
does not close the broader viewer, browser, or parity gates below.

Representative consumer regressions also pass in the same tree:
`lusdview-next-nonmesh-extraction`, `lusdview-mcp-render-batch`, and
`lusdview-tydra-pointinstancer`. These exercise next scene extraction, the MCP
render path, and typed point-instancer data while the legacy-compatible viewer
remains enabled.

The standalone Tydra Next suite is run from a Debug build because its tests use
assertions as the oracle. That complete suite now passes, including the physics
annotation memory-accounting regression; optimized builds are not used as the
assertion-based Tydra test gate.

After the latest render-boundary additions, an isolated clean rebuild of
`lightusd_render_c` took 2.98 wall seconds / 2.96 compiler CPU seconds with
one compile and 216,260 KiB peak child RSS (`scripts/bench-compile.py`, Ninja
`-j4`). This is a current measurement point for future header-size changes,
not a whole-product timing claim.

Following the subsequent scene, texture, skeleton, material, and light POD
extensions, the same target measures 3.01 wall seconds / 2.99 compiler CPU
seconds, one compile, and 216,276 KiB peak child RSS under the same Ninja
`-j4` setup.

An isolated next-only clean build of the public C++ facade test target measures
76.42 wall seconds / 295.46 compiler CPU seconds and 394,892 KiB peak child
RSS across 193 translation units under the same Ninja `-j4` setup. This whole
target measurement keeps the one-translation-unit render-boundary result from
being mistaken for a product compile-time result.

Replacing the render C adapter's dependency on `c-session-internal.hh` and the
`lightusd-next.hh` umbrella with narrow document snapshot accessors and
`stage/change-set.hh` reduced an isolated three-sample `lightusd-render-c.cc`
compile median from 3.84 to 3.10 seconds and peak child RSS from 324,700 to
223,052 KiB. The same `app.cc` include cleanup measured 29.69 to 29.60 seconds,
within run-to-run noise, so no viewer compile-time gain is claimed. These are
single-translation-unit results; the full native build and C document-session
tests pass after the split.

The standalone C consumer test also rebuilds and passes against the installed
render-enabled boundary after the physics additions.

The render C buffer cache now keys flattened primvar, blend-shape, and
animation-channel data with the full `size_t` subresource index. Its previous
packed key truncated indices to 16 bits, so an index at 65,536 could alias
index zero and return the wrong borrowed buffer. The render-session C test and
the next-labeled suite pass after the change (39 passed, one optional skip).

The root `LIGHTUSD_NATIVE_PRODUCT=next` configuration also remains
self-contained with `LIGHTUSD_WITH_TYDRA=OFF`: its full next build completes,
and both the public C API example smoke test and next C API test pass without
configuring the legacy product.
After adding the document-session C boundary, the core-only root product still
builds its public C API example and passes its smoke test. A render-enabled
standalone next build passes its 41 next-labeled tests (40 passed, one optional
skip), and strict C11/C++17 consumers compile and run against the installed
shared boundary.

Sanitizer checks exposed a zero-count rotation shifting by 32 in vendored
MikkTSpace; both instances now mask the shift count. The memory64 Node profile
exposed a typed-array constructor receiving a BigInt length; the shared copy
helper now passes a JS Number. The rebuilt memory64 next module passes all
24 Node suites. Ancillary WASM modules used by the Node profile were not
rebuilt by this change.

The full web gate passed its Node and physics checks and all 67 Menagerie
MJCF/USD/MJCF roundtrips. Its initial browser launch failed because Puppeteer's
pinned Chrome 148 was unavailable. Rerunning the browser profile with installed
Chrome 153 passed the five-model URDF sweep and all 67 OffscreenCanvas models,
using the rebuilt next WASM module. Existing generated application modules were
restored after verification.

The browser `getMesh()` facade now builds output material objects from a
fixed-layout material record and bounded strings. Texture metadata uses a
second fixed-layout record; subset materials pass only output material IDs
through the remaining extras bridge. The unused material emval builders were
removed. The output-material and texture records have compile-time layout
checks against the JS offsets. The focused C-dispatch and next-only adapter
tests pass on wasm32 and memory64, including a material-bound GeomSubset;
the full Node profile passes 24/24.
The resulting linked next modules are 1,665,006 bytes (wasm32) and 1,867,538
bytes (memory64) at that point. Subset material IDs and triangle ranges now
come through a bounded C copy, including the stage fallback and converter
ranges. The public `getMesh()` object retains its material and submesh arrays;
the material-bound GeomSubset fixture passes on both WASM pointer widths and
the Node profile remains 24/24. The older scalar subset exports, which used
RenderScene IDs and missed stage fallback, were removed. The base material ID
now comes directly from the typed mesh view, leaving only blend-shape records
in the mesh extras bridge. The linked modules are 1,664,492 bytes (wasm32)
and 1,866,985 bytes (memory64).

Mesh blend-shape names, weights, inbetween records, and remapped point/normal
offsets now use fixed-layout info and bounded C copies. The buffer path builds
the mesh view on demand so sparse offsets align with output vertices. The JS
facade reconstructs the `blendShapes` arrays; the mesh-extras emval getter and
dispatch case are removed. Parity against the old getter was checked before
removal with sparse and normal offsets plus an authored half-weight inbetween
on wasm32 and memory64. The final focused adapter and C-dispatch suites pass
on both pointer widths; the full Node profile passes 24/24. Any scratch mesh
rebuild invalidates the offset remap cache, verified by re-reading shape data
after a mesh-buffer request. The linked next modules are 1,664,231 bytes
(wasm32) and 1,866,588 bytes (memory64).

The current wasm32 mesh translation-unit timing is recorded in the ignored
`web/build_next_ninja/mesh-typed-compile.json` report. With Emscripten 4.0.14,
`-Oz`, caches disabled, and three isolated runs, median compile wall times are
2.13 seconds for mesh output, 2.20 seconds for material output, 3.55 seconds
for render lifecycle, 2.14 seconds for the primvar unit, and 2.46 seconds for
mesh merge. The respective object files are 31,715, 28,562, 154,191, 36,536,
and 68,853 bytes; the linked WASM file is 1,668,180 bytes (671,251 gzip).
These are current-state measurements, not evidence of a compile-time speedup:
an identical-toolchain pre-migration timing was not captured.

The web RenderStream now enables `retain_custom_primvars` while leaving full
mesh geometry retention disabled. The converter extracts only non-builtin
primvars in this mode and keeps their indexed/value buffers after releasing
points, topology, UVs, colors, and normals. USDA fixtures verify the retained
values and indices in both wasm32 and memory64, and the Debug native Tydra
test checks that builtin UVs and bulk mesh geometry stay released. The native
suite and the 24-suite web Node profile pass. Mesh-only workers now enumerate
custom primvars directly from Stage; the wasm32/memory64 fixture compares
float, integer, double-to-float, and indexed channels with the render-enabled
path while excluding builtin UVs.

The merge pass consults that source catalog before combining meshes. The
two-mesh fixture demonstrates that plain meshes still merge to one output,
while custom-primvar meshes remain two outputs with their respective values
intact, in render-enabled and mesh-only modes on wasm32 and memory64. The
full Node profile remains 24/24. The linked modules are now 1,668,180 bytes
(wasm32) and 1,871,267 bytes (memory64); this preservation rule adds a small
amount of code and does not establish a compile-time speedup.

Variant-set enumeration now uses generation-checked C counts and bounded
UTF-8 copies for prim path, set name, authored selection, and variant names.
The JavaScript `listVariants()` wrapper reconstructs its existing result; the
C++ emval array builder and dispatch case are removed. Focused dispatch and
composition tests pass on wasm32 and memory64. The memory64 composition run
uses `LIGHTUSD_NEXT_ONLY=1` because that build tree has no legacy memory64
module. Linked sizes after this migration are 1,668,359 bytes (wasm32) and
1,871,437 bytes (memory64); the change is boundary cleanup, not a size win.

`RenderStream.provideAsset()` now copies a named byte view through a checked C
export instead of the emval dispatcher. The JS adapter preserves typed-array
subarray offsets and copies a WASM-heap view before an allocation that could
grow memory. The C side keeps the existing 1 GiB payload bound and path
normalization. Focused dispatch tests cover both pointer widths, stale handles,
subarrays, heap-backed views, and asset-byte accounting. Linked sizes are
1,668,455 bytes (wasm32) and 1,871,551 bytes (memory64). The 24-suite
wasm32 Node profile and the memory64 next-only adapter suite pass.

`RenderStream.begin()` and `beginOwned()` now feed a bounded byte span through
the C boundary. The C++ loader returns a success flag, and JS assembles the
previous load-result fields from typed counts and a bounded error-string
copy. The wrapper handles subarray offsets and heap-backed views; failed loads
continue to expose their parser error. The old emval load-result builders,
dispatch cases, and unused render payload helpers are removed. The wasm32
Node profile passes 24/24, and the focused load, composition, and next-only
adapter checks pass on memory64. Linked sizes are 1,666,939 bytes (wasm32)
and 1,870,018 bytes (memory64), down 1,516 and 1,533 bytes respectively
from the preceding asset-input boundary state. This is a linked-size result;
an isolated compile-time comparison has not been measured for this change.

The remaining `RenderStream` boolean settings, UTF-8 settings, variant
override, asset/override clearing, and `end()` now use checked C exports.
Previously migrated count methods are no longer registered in the generic
dispatcher either, so `NextInvokeObject` has no RenderStream branch. Typed
calls keep the existing JavaScript methods and receiver/argument checks.
The C-dispatch and variant composition fixtures pass on wasm32 and memory64;
the memory64 next-only adapter suite and the full 24-suite wasm32 Node profile
pass. The linked modules are 1,665,848 bytes (wasm32) and 1,868,963 bytes
(memory64), down 1,091 and 1,055 bytes from the preceding load-boundary
state. Other next WASM classes still use emval and remain in the migration
inventory.

An isolated three-run Emscripten 4.0.14 `-Oz` compile baseline for the
post-migration dispatcher is in the ignored
`web/build_next_ninja/render-dispatch-compile.json` report. Median wall time
is 3.60 seconds for `binding-next.cc` and 1.99 seconds for
`binding-next-api.cc`; their objects are 241,082 and 27,349 bytes. This
records current compile cost only, with no matched pre-change timing.

`NextFlattenSession.begin()`, `provideLayer()`, variant overrides, and `end()`
now use generation-checked C exports with counted UTF-8 and byte spans.
The JS methods preserve their existing result objects and accept subarrays and
heap-backed views. The C-dispatch test now actually imports the memory64
module when `LIGHTUSD_WASM64=1`; earlier invocations with that variable had
still tested wasm32. Both pointer widths now pass the direct session checks
and USDA dependency composition, including a typed override that selects the
lower-detail mesh in the flattened crate. The full wasm32 Node profile passes 24/24,
and the memory64 next-only adapter passes. Linked sizes are 1,665,593 bytes
(wasm32) and 1,868,843 bytes (memory64). At this point, the `step()` result
and streaming callback still used emval.

`NextFlattenSession.step()` now writes a fixed-layout status/statistics record.
Need-layer keys, buffered USDC output, and referenced asset paths use bounded
copies; the JS adapter releases the native step output after assembling its
result. Streaming uses a numeric callback ID and a borrowed heap view during
each synchronous chunk call. The C++ emval result builder and the session's
last generic dispatch case are removed. wasm32 and memory64 tests cover
buffered output, dependency requests, streamed chunks, callback abort, and
callback exceptions. The full wasm32 Node profile passes 24/24 and the
memory64 next-only adapter passes. Linked sizes are 1,665,379 bytes
(wasm32) and 1,868,538 bytes (memory64), down 214 and 305 bytes from the
preceding session-input state. Other next WASM classes and global utilities
still require binding migration.

The matched three-run isolated compile comparison uses the same Emscripten
4.0.14 command lines before and after the flatten-session migration; reports
are in the ignored `render-dispatch-compile.json` and
`flatten-typed-compile.json` files under `web/build_next_ninja`. Median wall
time changed from 3.60 to 3.59 seconds for `binding-next.cc` and 1.99 to
1.94 seconds for `binding-next-api.cc`; those differences are too small to
establish a compile-time improvement. The corresponding objects changed from
241,082 to 234,195 bytes and 27,349 to 28,697 bytes, a net reduction of
5,539 bytes across the two units.

An isolated `NextFlattenSession` translation unit was tried and reverted.
With the same Emscripten command lines, the dispatcher alone compiled in
3.24 rather than 3.59 seconds, but the new unit took another 2.52 seconds.
The measured serial sum for dispatcher, session, and API rose from 5.53 to
7.74 seconds, and the linked modules grew by roughly 1 KiB each. The
ignored `flatten-split-compile.json` report records that experiment; the
single-unit layout remains in the source tree.

Global `validateFromBinary()` now consumes a counted byte span and returns an
owned UTF-8 JSON buffer through C. The JS wrapper preserves its string result
and frees both allocations, including on memory64. Focused tests cover
subarrays, heap-backed views, invalid input, and parse errors on both pointer
widths. The full wasm32 Node profile passes 24/24 and the memory64 next-only
adapter passes. Linked sizes are 1,665,511 bytes (wasm32) and 1,868,745
bytes (memory64), 132 and 207 bytes larger than before this boundary move.
At this point, global `usddiff` and the converter/subdivision classes still
used emval.

Global `usddiff()` now passes two counted byte views, names, and a fixed-layout
options record to C. The result remains the same JS object after decoding an
owned JSON buffer; the last global emval dispatch case and its option-reading
helpers are removed. wasm32 and memory64 tests cover equal/changed layers,
text/JSON formats, option flags, missing options, parse errors, subarrays, and
heap-backed views. The full wasm32 Node profile passes 24/24 and the
memory64 next-only adapter passes. Linked sizes are 1,664,582 bytes
(wasm32) and 1,867,726 bytes (memory64), down 929 and 1,019 bytes from the
preceding typed-validation state.

The same-command three-run compile comparison against the earlier
`flatten-typed-compile.json` report shows `binding-next.cc` unchanged at a
3.59-second median and `binding-next-api.cc` at 1.94 versus 1.97 seconds.
Their combined object size fell by 2,453 bytes. The current report is the
ignored `web/build_next_ninja/global-typed-compile.json`; this is a code-size
gain, not a demonstrated compile-time speedup.

`SubdivStreamer.refineStream()` now takes counted float and index buffers plus
a fixed-layout option record at the C boundary. Each refined batch reaches
JavaScript through a synchronous numeric callback ID and borrowed heap views;
the JS adapter preserves the existing callback signature and error string.
`heapBytes()` reads the module heap length directly. The class's two emval
dispatch cases are gone. Focused wasm32 and memory64 checks cover UVs,
normals, callback exceptions, error text, and deleted handles. The full
wasm32 Node profile passes 24/24. Linked sizes are 1,663,498 bytes (wasm32)
and 1,866,362 bytes (memory64), down 1,084 and 1,364 bytes from the
preceding typed-diff state. The USDZ converter remains the sole next-WASM
class using generic emval dispatch.

The matched three-run isolated compile comparison uses the same Emscripten
4.0.14 commands as `global-typed-compile.json`. Median wall time moved from
3.59 to 3.56 seconds for `binding-next.cc` and from 1.97 to 1.92 seconds for
`binding-next-api.cc`; this small change is not a reliable compile-time gain.
Their combined object size fell by 6,583 bytes. The ignored result is
`web/build_next_ninja/subdiv-typed-compile.json`.

The next-only USDZ converter's `rewriteRoot()` path now takes a counted source
buffer and fixed-layout rewrite options. The converter owns its result until
the JS adapter copies it through a bounded buffer query; a POD record carries
format, size, and USDC writer counts. Error and warning text also use bounded
copies. The existing JS result shape and next-only USDZ workflow remain
intact. Focused wasm32/memory64 tests cover USDA and USDC output, subarrays,
heap-backed input, invalid limits, and parse errors; both pointer widths pass
the next-only USDZ conversion suite. The full wasm32 Node profile passes
24/24. Linked sizes are 1,662,918 bytes (wasm32) and 1,865,611 bytes
(memory64), down 580 and 751 bytes from the subdivision checkpoint. The matched
three-run isolated compile comparison (`rewrite-typed-compile.json`) has
`binding-next.cc` at 3.59 seconds versus 3.56 and `binding-next-api.cc` at
2.03 versus 1.92; this does not establish a compile-time gain. The combined
object size fell by 12,028 bytes. The converter's remaining stage, asset,
mesh-buffer, and export methods still use generic emval dispatch.

The remaining converter stage, asset, mesh-buffer, and export methods now also
use counted C buffers. A single retained output buffer serves physics JSON,
USDA, USDC, and USDZ exports. The next-core WASM object dispatcher, emval
reference API, and unused emval helpers are removed; generation-checked
handles remain. The consumer-compatible mesh adapter accepts both signed and
unsigned 32-bit indices. Focused wasm32 and memory64 tests cover failed and
successful stage creation, signed mesh indices, asset packaging, all four
export kinds, and stale/wrong-class handles. Both next-only USDZ suites pass,
as does the full wasm32 Node profile (24/24). The C header also passes a
strict C11 syntax check. Linked sizes are 1,660,461 bytes (wasm32) and
1,862,569 bytes (memory64), down 2,457 and 3,042 bytes from the typed-rewrite
checkpoint. Matched three-run isolated compile medians for `binding-next.cc`
and `binding-next-api.cc` are 3.45 and 1.89 seconds, versus 3.59 and 2.03
seconds before this step; their combined objects fell by 22,621 bytes. The
ignored measurement is `web/build_next_ninja/converter-typed-compile.json`.
The next-core converter, render, flatten, and subdivision adapter no longer
use emval. The subsequent LightRT migration also removed the next-only
target's `--bind` link flag; the combined legacy target retains embind.

The stateless meshoptimizer simplifier in `lightrt-wasm.cc` now exposes one
counted C function in the next-only module, declared in the C11-compatible
`web/lightrt-wasm-api.h`. The JS class retains its `simplify()` result shape
and deletion behavior. The combined legacy module keeps its embind class but
shares the same native simplification engine, so feature coverage is not
removed. Lucia's retopology worker now loads the next-only module for this
operation. An end-to-end worker test exposed and fixed an undefined `uvs`
variable in its UV-seam lock path. Focused tests pass on wasm32 and memory64,
the combined legacy simplifier passes, and the full wasm32 Node and Lucia
suites pass (24/24 and 4/4). Linked next-only sizes are 1,659,003 bytes
(wasm32) and 1,860,778 bytes (memory64), down 1,458 and 1,791 bytes from the
previous checkpoint.

The next-only LightRT path tracer now also uses generation-checked C handles
and counted buffers for scene build, trace, occlusion, raycast, errors, and GPU
scene data. `next-api.js` retains the JS class and result shapes; Lucia's AO
and projection workers load the next-only module. The combined legacy module
keeps its embind class. Both next-only widths pass focused path-tracer and
worker tests, the combined class passes a direct parity check, and the full
Node and Lucia suites pass (24/24 and 4/4). The next-only module links without
`--bind` and its generated JS contains no embind/emval runtime registration.
Linked sizes after C-boundary size guards are 1,651,533 bytes (wasm32) and
1,851,970 bytes (memory64), down 7,470 and 8,808 bytes from the simplifier
checkpoint. The C11 header check, stale-handle and clear tests, and both full
suites pass.

The counted LightRT build path now rejects malformed array shapes before
copying the caller's buffers into native vectors, while preserving the
established error strings and clearing a previously built scene. Its JS GPU
scene adapter copies the seven buffers in one bounded allocation instead of
seven separate allocations; focused tests compare the returned normal, color,
material-ID, and material arrays on wasm32 and memory64. A three-run isolated
compile of the next-only `lightrt-wasm.cc` translation unit has a 1.30-second
median, 125,424 KiB peak child RSS, and a 21,802-byte object. This is the
current baseline, not a matched pre-migration compile-time comparison; the
ignored report is `web/build_ninja_next/lightrt-typed-compile.json`. The
updated linked sizes are 1,651,616 bytes (wasm32) and 1,852,049 bytes
(memory64).

The browser `usdzconvert` worker now has an explicit next-only single-layer
rewrite option. It loads `lightusd_next.js` rather than the legacy WASM module
for that option, preserving the smaller product for root-layer rewrite and
asset passthrough. The UI disables flattening in this mode, and the converter
rejects flattening or variant overrides instead of silently ignoring them.
Streaming conversion and texture processing still use the legacy module.
The next-only worker test passes on wasm32 and memory64.

The viewer preview-cache header now owns its loaded `Stage` through an opaque
pointer, removing `next/stage/stage.hh` from that header's transitive includes.
The Release preview-cache test now actually executes its assertions (which
were previously compiled out by `NDEBUG`) and passes. An isolated three-run
syntax-only include probe moved from 0.259 to 0.233 seconds median; the full
`lusdview` `main.cc` compile remained 8.08 seconds median because other viewer
headers still import Stage. The viewer and preview-cache test targets build.

Native scene-record enumeration now has a bounded display-name copy beside
its key copy, including root-node names. Point-instance-draw path lookup
returns the first draw for an instancer path, matching its record key; multiple
draws can share that path, so callers enumerate the draw records for all
matches. Focused C and C++ facade fixtures pass. The web resource-path kind
numbers now match native `lightusd_render_kind` after ABI 4 for unsupported
renderables and instancers. Web record kind 11 now resolves root-list paths
and names, matching native root-node enumeration; `rootNodeId()` still returns
the underlying node ID.
The root next-product suite passes 45/45 registered tests (one optional AOUSD
corpus skip), standalone next passes 44/44 (same skip), and the focused web
dispatch test passes on wasm32 and memory64. The web Node profile passes
24/24; the in-tree CPython extension rebuilds and its suite passes 134 tests
with 8 skips. Linked next-only WASM sizes are 1,652,849 bytes (wasm32) and
1,853,379 bytes (memory64). This boundary alignment does not establish a
compile-time improvement.

Prepared C render updates now expose an optional owning scene copy before
commit. Upsert events carry a checked record index into that candidate, so a
sink can fetch the exact typed payload while processing the event, including
when paths repeat; begin/remove/end/abort use -1. The copy is caller-owned and
remains valid after commit or rejection. The C++ facade forwards it and now
releases prepared handles even when their session has already been destroyed.
Invalid update-info layouts and output-handle allocation are checked before
publication, so these failures cannot advance a revision after reporting an
error. C and C++ fixtures exercise in-callback mesh-buffer reads, failed
pre-commit validation, ownership, and revision preservation. Root next passes
45/45 and standalone next 44/44 registered tests, each with one optional
AOUSD corpus skip.

The in-tree CPython extension rebuilds against the changed render header and
its suite passes 134 tests with 8 skips. A rejected commit keeps its prepared
candidate available until the caller retries or aborts it.

The C one-step render update now delegates to the same prepare/commit path as
the explicit transaction API. This removes a second event-sink and snapshot
publication path while preserving automatic abort on failed commits. A matched
three-run isolated Release compile of `lightusd-render-c.cc` moved from 3.51
to 3.44 seconds median for `lightusd_render_c` and 3.54 to 3.49 seconds for
the shared C library variant. Their objects fell from 155,840 to 153,352
bytes and 155,336 to 152,704 bytes, respectively. These are isolated compile
and object-size measurements, not a whole-product speed or linked-size claim.
Reports are ignored under `build_ninja_next_product/render-apply-{before,after}.json`.
The root and standalone next suites pass 45/45 and 44/44 registered tests,
respectively, with one optional AOUSD corpus skip in each. The rebuilt in-tree
Python extension passes 134 tests with 8 skips.

Session-backed C render-scene handles now retain the immutable Tydra scene
owner instead of deep-copying its resource catalogs and buffers for every
published or prepared handle. Each C handle still owns its warnings and lazy
flattening caches; a direct one-shot conversion still owns its result. The
prepared and committed handles can therefore refer to the same record data,
while each remains valid after the other handle, update, or session is
destroyed. The C session fixture checks this shared backing and the existing
snapshot-lifetime checks. This removes snapshot-copy work and duplicated
payload memory; no peak-memory or whole-product compile measurement has been
made for this change.

The viewer's `app.hh` now forward-declares the Tydra render-session handle
types and defines `App`'s constructor out of line. This removes the
`render-session.hh` include from a widely shared viewer header; `app.cc`
continues to use the implementation directly for scene-update transactions.
The configured `lusdview` target builds, and its preview-cache, incremental
scene-update, LOD-stream, and scene-safety tests pass. No clean-build timing
claim is made for this header change.

The next-only JS adapter now uses one bounded-copy helper for texture image
bytes and texture sampling floats. It checks that the second native copy
returns the queried byte count before exposing an owned typed array, including
on memory64. The focused dispatch fixture exercises successful copies, the
memory64 pointer fallback, and a short-copy rejection; it passes on wasm32
and memory64 after rebuilding both modules. The full 24-suite Node profile
also passes. Native C already exposes these sampling values through
`lightusd_render_texture_info`, so this is adapter simplification and copy
validation rather than a new C payload export.

The five material-parameter buffer getters now use that same helper, removing
their second query/allocation/copy loop. The dispatch fixture checks authored
roughness values and a forced short-copy failure followed by a successful
read on wasm32 and memory64. The rebuilt 24-suite Node profile passes. From
the preceding texture-only checkpoint, `web/next-api.js` fell from 177,282
to 176,364 bytes and each generated next-only JS wrapper fell by 628 raw
bytes (wasm32 204,276 to 203,648; memory64 207,967 to 207,339). These are
raw JS file sizes, not linked WASM or compile-time measurements.

Animation channel buffers, node transforms and path, resource paths/names,
and scene-record paths now reuse the same checked C/POD copy path. Their
typed-array element widths and public error categories remain explicit at
each call site. The focused fixture also rejects an animation byte count
that cannot form complete double values. The wasm32 and memory64 next-only
modules rebuild, their focused C-dispatch fixtures pass, and the 24-suite
Node profile passes.
Relative to the material-buffer checkpoint, `web/next-api.js` fell from
176,364 to 170,637 raw bytes; the generated next-only JS wrappers fell from
203,648 to 199,340 bytes (wasm32) and 207,339 to 203,031 bytes (memory64).
These are JS sizes, not linked WASM or compile-time results.

The remaining point, curve, instancer, light-transform/color, and camera
transform/optics numeric buffer wrappers now use that checked copy path too.
This preserves their typed-array element formats and zero-length results while
rejecting a changed byte count before exposing data. Both next-only modules
rebuild; focused dispatch tests pass on wasm32 and memory64, and the 24-suite
Node profile passes. Against the preceding checkpoint, `web/next-api.js`
fell from 170,637 to 165,842 bytes; generated JS fell from 199,340 to
195,865 bytes (wasm32) and 203,031 to 199,556 bytes (memory64). These
measure raw JS files only.

Point-instance-draw transform, skeleton matrices/parents/children, mesh
primvar raw bytes, and typed mesh geometry buffers now use the same checked
copy path. Fixed-layout POD queries and strings with distinct terminator rules
retain their separate helpers. Focused next-only dispatch tests pass on
wasm32 and memory64 after rebuilding both modules, and the 24-suite Node
profile passes. From the preceding numeric-buffer checkpoint,
`web/next-api.js` fell from 165,842 to 160,926 raw bytes; generated JS fell
from 195,865 to 192,279 bytes (wasm32) and 199,556 to 195,970 bytes
(memory64). Linked WASM and compile-time effects were not measured.

`next/stage/change-set.hh` now includes only `Path` and forward-declares
`Stage`; the render-session implementation includes `stage.hh` explicitly
where it calls Stage methods. This removes a full Stage definition from every
change-set consumer, including the viewer app header, without changing
change-set ownership or adding allocations. In five isolated C++17 syntax
checks of the self-contained header, median parse time moved from 0.2646 to
0.2399 seconds on this host. This is an isolated header measurement, not a
whole-viewer compile claim. The root next product and `lusdview` build; root
next passes 45/45 registered tests, standalone next 44/44 (each with one
optional AOUSD skip), and four focused viewer tests pass.

`tydra/next/render-session.hh` now forward-declares converter configuration and
scene record types instead of including the converter and full render-data
definitions. Its no-argument constructor remains available as an out-of-line
constructor. `render-session.cc` includes the converter implementation
explicitly. A five-run isolated syntax check of the session header moved from
a 0.5823-second median to 0.2492 seconds on this host. The root next build and
45-test suite, standalone next build and 44-test suite, `lusdview` build, and
four focused viewer tests all pass; each next suite has one optional AOUSD
skip. The parse measurement is for this header alone, not whole-product
compile time.

The viewer preview-cache test now includes the focused
`next/reader/usda-reader.hh` instead of the `next/lightusd-next.hh` umbrella,
and loads its fixture through `LoadUSDAFromFile`. Its target rebuilds and the
focused CTest passes. This removes unrelated next API declarations from that
test translation unit; no compile-time gain is claimed from this single
change.

The public C++ facade now wraps memory-asset registration and aliases,
resolved-asset reads, and dependency-report JSON with RAII output ownership and
a `bool` completion result. The portable-package example migrated from raw C
resolver calls to these wrappers; its target rebuilt and the relocation workflow
passed from a fresh scratch directory. `Prim` also exposes active state,
specifier, kind, keyed prim metadata, relationships, and assetInfo through
borrowed POD/dictionary views; the installed-header facade test passes those
queries against authored and in-memory USDA.

`DictionaryView` provides validity, size, lookup, and indexed access without
exposing `next::Dict`. RAII string/list/value utilities now cover borrowed
string views, bounded string copies, string-list access, and value views. A
declaration audit finds all 78 render C exports mentioned by the C++ render
facade and 125 of 126 core C exports. The only remaining unwrapped declaration
is pseudo-root access, which intentionally returns an invalid prim because
next Stage models its pseudo-root as virtual. The installed-header test reads
prim customData and assetInfo entries through the borrowed dictionary cursor;
`next_test_cpp_facade` passes. The portable-package example now uses the C++
facade for resolver, report, and string operations; no direct C functions
remain in its source.

The latest root `LIGHTUSD_NATIVE_PRODUCT=next` build completes and all 45
registered CTests pass, with the optional AOUSD value-resolution corpus test
skipped because its supplemental fixture is absent. A fresh Release/Ninja
`LIGHTUSD_NATIVE_PRODUCT=next -DLIGHTUSD_WITH_TYDRA=OFF` configure/build also
completes; all 39 registered core-only tests pass with the same optional skip.
These runs validate both render-enabled and core-only product boundaries after
the facade additions; legacy parity and the default switch remain outstanding.
The current Tydra-disabled build also enables examples, builds
`next_c_api_example`, and passes its smoke test as part of the complete 39-test
core-only CTest run (one optional corpus skip).

## Clean product-build comparison (2026-09-23)

Measured clean Ninja builds with `scripts/bench-compile.py`, three runs each,
8 jobs, Release, using the existing legacy and next configurations. The legacy
`lightusd_static` target took a median 226.29 s (223.79–256.47 s), 1,618.44
CPU-seconds, and 797,860 KiB peak child RSS. Its compile graph contains 838
commands over 745 unique sources; `liblightusd_static.a` is 65,428,564 bytes.
The next `lightusd_render_c` dependency closure took a median 46.69 s
(44.94–47.56 s), 349.07 CPU-seconds, and 382,852 KiB peak child RSS. Its graph
contains 202 commands over 196 unique sources. The four static archives in
that closure total 9,352,336 bytes: `lightusd_next` 8,436,868,
`tydra_next` 3,173,288, `lightusd_c` 369,690, and `lightusd_render_c`
176,490 bytes. The listed archive sum is larger than the legacy archive's
text section because archive members and features differ; it is not an
executable-size comparison.

These measurements show the configured next render-C closure builds faster
and has a smaller source graph on this host. They do not establish semantic
parity: the legacy monolithic library includes broader legacy schemas,
formats, utilities, and integrations. Compiler, flags, and host match, but
the deliverable/source sets do not. The JSON reports are in `/tmp` and were
not added to the repository. Build closures rebuilt and linked successfully;
their test gates remain the separate 45/45 Tydra-on and 39/39 Tydra-off next
results above. Work still open: complete feature/API parity gap matrix and
tests, then evaluate switching the default product only after the remaining
gaps are resolved.

The installed-header C++ facade test now checks material node-graph copy
failure semantics as well as the successful size-query/copy path: a null
`required` pointer is rejected, and a too-small destination returns
`LIGHTUSD_ERR_INVALID_ARG` while still reporting the full required size. The
`next_test_cpp_facade` CTest passes after rebuilding the test executable.

`lusdview`'s preview cache no longer includes the all-in-one next header. It
uses the focused USDC reader, writer, and Stage headers, and consumes their
result-returning APIs directly. Five isolated Release compile runs on the same
configured build moved the viewer translation-unit median from 5.40 s to
4.94 s and peak child RSS from 379,168 KiB to 332,740 KiB. The separate
preview-cache-test compile command moved from 6.20 s / 391,100 KiB to 5.50 s /
334,488 KiB. Object sizes changed by less than 1.1 KiB in either command;
this is a translation-unit compile/RSS result, not a whole-viewer or linked
size claim. The preview-cache CTest passes and `lusdview` rebuilds.

`lusdview/gui.cc` also drops the all-in-one next header. Its used next-core
symbols are supplied by the prim-spec, layer, type-info, and existing Tydra
scene-access headers. Three isolated Release compiles moved from a 13.79 s
median and 771,204 KiB peak child RSS to 13.15 s and 721,256 KiB; object size
was unchanged at 546,240 bytes. This is a 4.6% translation-unit compile-time
and 6.5% peak child RSS reduction on this host. The complete `lusdview` target
rebuilds with the focused include set.

Persistent load/session declarations now live in `next/stage/stage-session.hh`,
which the unified `lightusd-next.hh` continues to include for compatibility.
The C document-session internals and `lusdview/app.cc` use this focused header
directly. The C-session translation unit's two compile-database variants
improved from 1.98/1.90 s and 229,464/229,256 KiB peak child RSS to 1.76/1.81 s
and 210,840/210,368 KiB. `lusdview/app.cc` showed overlapping compile-time
runs (32.69 s before, 32.87 s after), so no compile-time gain is claimed;
median peak child RSS fell from 1,380,808 to 1,370,816 KiB and the object
changed by 32 bytes. The viewer rebuild and C++ facade/document-session tests
pass with the new header.

`lusdview/mcp/app_mcp.cc` also uses the focused StageSession, PrimSpec, and
type-info headers instead of the umbrella. Its three-run compile medians
overlapped (20.39 s before, 20.27 s after); peak RSS and object size were
effectively unchanged (882,536 to 887,076 KiB; 1,002,432 to 1,002,480 bytes).
This removes an unnecessary dependency on the umbrella's unrelated API
surface; no compile or memory gain is claimed for this consumer.

`lusdquicklook/loader.cc` no longer includes `lightusd-next.hh`; its
persistent loader types come from `stage-session.hh`, alongside the focused
USDZ reader header it already uses. Three isolated Release compiles moved
from a 5.37 s median and 340,544 KiB peak child RSS to 5.08 s and 332,580 KiB;
the object size remained 214,248 bytes. The optional `lusdquicklook` target
builds and its smoke test passes.

The high-level load/write declarations now live in `next/load-usd.hh`, which
the unified header includes for source compatibility. The next path in
`tydra_to_renderscene` now depends on that focused header and the Tydra
converter header. Its three-run compile medians overlapped (7.64 s with the
umbrella, 7.60 s focused), while peak child RSS fell from 524,824 to 512,292
KiB; object size stayed 203,576 bytes. With
`LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON`, the full example target builds, and
`--next --nodump tests/usda/cube.usda` loads and converts successfully.

The native core C implementation now includes `next/load-usd.hh` and its
direct Layer, PrimSpec, and type-info dependencies instead of
`lightusd-next.hh`. Static and shared C libraries rebuild, and the C ABI,
facade, and document-session tests pass. Its two compile-command variants
used about 9 MiB less peak child RSS (333 MiB to 323 MiB); object sizes were
unchanged. The measured compile medians were slower (4.24 to 4.61 s and 4.18
to 4.43 s), so this is a dependency-boundary/memory reduction, not a compile
time improvement.

The large `lusdview/next_scene_loader.cc` similarly replaces the umbrella
with its focused loading, layer, value, and schema headers. Its three-run
median moved from 27.09 s to 26.52 s, peak child RSS from 865,512 to 860,772
KiB, and object size from 1,021,256 to 1,021,096 bytes. The `lusdview` target
builds with `LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON`; next non-mesh extraction
and texture-pipeline tests pass. Enabling that configuration also exposed a
guard bug: GPU format choice was hidden whenever the CPU texture-tools encoder
was disabled. The selector is now available independently of the encoder;
the next-enabled viewer links and the texture-pipeline regression passes.

The shared native string and byte-buffer copy helpers now use overlap-safe
`memmove` semantics, so callers may copy a borrowed view back into overlapping
caller storage. The render-session C fixture covers right-shifted string and
byte copies plus short-buffer rejection; both C implementation libraries build,
the fixture compiles as strict C11, `next_test_render_session_c` passes, and
`unit-test-lightusd` passes. Record-key and instancer-prototype-path string
copies now delegate to the shared bounded view helper as well, removing their
duplicate buffer checks and making empty, short, and overlapping copy behavior
consistent across native C string exports.

The two production next-only WASM binding translation units no longer include
`next/lightusd-next.hh`; they use focused loading declarations and their direct
layer, resolver, schema, writer, and Stage dependencies. The next-only module
relinks, and the C dispatch plus full next-only `usdzconvert` Node regressions
pass. Same-tree three-run compile checks show unchanged object sizes and lower
peak compiler RSS: 246,228 to 241,916 KiB for `binding-next.cc`, and 230,020 to
218,912 KiB for `binding-next-render-lifecycle.cc`. Compile medians were 3.55
to 3.55 s and 3.39 to 3.51 s respectively, so these are memory/dependency
boundary reductions rather than compile-time wins.

`binding-next-common.hh` now keeps only its declaration-level standard
headers and the Tydra render-data types it names. The Stage, mesh schema, JSON,
and scene-helper headers no longer fan out through the shared header; the two
material files that use scene helpers include `binding-next-scene.hh` directly,
and the render declaration header owns the Stage/mesh types it stores. This
removes accidental standard-header dependencies from `binding-next-util.cc`,
which now includes its own string-formatting, set, and memory-copy headers.
Both wasm32 and memory64 modules build, and the next C-dispatch and next-only
`usdzconvert` regressions pass on both widths. For
`binding-next-render-merge.cc`, a same-tree three-run compile comparison was
2.70 to 2.48 s, peak RSS 185,092 to 184,568 KiB, with object size unchanged at
69,426 bytes.

Eleven next test translation units now drop the all-in-one header. The USDZ
writer test uses its explicit USDZ/USDC reader-writer and high-level load
headers; its three-run compile median moved from 1.66 to 1.55 s and peak RSS
from 220,424 to 203,692 KiB, with object size unchanged at 58,568 bytes. The
schema test names its schema modules, Stage/LayerBuilder, Value, and USDA reader
directly; its median moved from 2.41 to 2.12 s, peak RSS from 254,956 to 196,004
KiB, and object size from 147,000 to 146,960 bytes. Both focused test targets
rebuild and pass. The full render-enabled native next suite passes all 45
registered tests, with its optional AOUSD value-resolution corpus test skipped.

The extended schema test now also avoids the umbrella, declaring the additional
volume, camera, semantics, render, and high-level load headers used by its
fixtures. It builds and passes; the same-tree compile median changed 2.71 to
2.68 s, peak compiler RSS 258,512 to 251,588 KiB, and object size stayed at
224,768 bytes. No material compile-time improvement is claimed.

The USD-cat compatibility roundtrip test now uses focused Stage, LayerBuilder,
Value, load, USDC reader, and USDC writer headers. It builds and passes; its
three-run median moved from 1.32 to 1.24 s, peak RSS from 208,640 to 191,232
KiB, and object size remained 25,840 bytes.

The PCP instance-group correctness/performance test also uses direct Layer,
Cache, resolver, Stage, and Value headers. It builds and passes; its compile
median moved 1.39 to 1.22 s, peak compiler RSS 214,664 to 187,284 KiB, and
object size remained 38,664 bytes. The full render-enabled native next suite
passes all 45 tests, with one optional AOUSD corpus skip.

The USDC roundtrip test now includes the focused load and USDA/USDC reader
headers instead of the all-in-one header. Its focused target builds and passes,
and the full render-enabled native next suite passes all 40 currently
registered tests, with the optional AOUSD value-resolution corpus test skipped.
The AOUSD sampled-value test and the self-contained value-resolution edge
matrix also use only their evaluation, composition, resolver, Stage, and load
dependencies; both targets build, the edge matrix passes, and the sampled-value
test keeps its documented skip because the supplemental corpus is unavailable.
The full native next suite remains green at 40 registered tests with that one
optional corpus skip.

The next-only `next_usdcat` CLI replaces its umbrella include with the focused
load API. The rebuilt CLI passes its USDA asset-path and boolean comparisons
and the corpus parse regression.

The PCP composition test also replaces the umbrella with the load/session
header for its package and payload integration cases. Its focused target builds
and passes.

The next benchmark executable also drops the umbrella and includes the USDA
writer API it uses directly. It rebuilds and its registered benchmark smoke
test passes.

The broad `test_next.cc` translation unit replaces the umbrella with the load
session API, physics-scene extractor, and USDC writer declarations it uses.
Its complete test executable rebuilds and passes. No `tests/next` source now
includes `next/lightusd-next.hh`.
The fresh core-only product rebuilds every affected test target and passes all
39 registered tests (one optional AOUSD corpus skip), confirming the test
include cleanup also works with Tydra disabled.

The full web Node regression profile passes all 24 suites after the typed
next-WASM boundary migrations, including the next-only adapter and memory64
capability checks. Its first invocation skipped validation parity because the
default `build/lusdcat` path was absent. Rerunning with
`LUSDCAT_PATH=.../build_ninja/lusdcat` passed all six USDA/USDC/USDZ
native-versus-WASM validation comparisons, and the aggregate 24-suite profile
then passed with no skips or failures. This closes the validation parity gate
for those six fixtures; it does not establish broader full-library parity.

`lusdquicklook` now computes mesh, scene, and queued-event byte totals with
saturating addition and multiplication, and checks geometry, texture, and
queue limits by subtraction before admission. Texture source bytes use an
atomic bounded reservation, preventing concurrent image callbacks from
wrapping or collectively exceeding their decode budget. Image expansion and
thumbnail upload check dimension products and source-buffer length before
allocation or access. The queue preserves its single-oversized-event behavior.
A focused arithmetic test covers exact capacity and integer overflow edges;
the quicklook target rebuilds and its headless end-to-end smoke passes.

The next RenderStream material string query now copies the retained volume
graph (kind 10) and PreviewSurface utility graph (kind 11) independently;
kind 5 keeps its existing preferred-surface behavior. `getOutputMaterial()`
adds `volumeNodeGraphJson` and `previewSurfaceNodeGraphJson` only when those
graphs are present. The RenderStream method inventory remains at 226 entries.
The PreviewSurface and volume graph fixtures pass through checked query/copy
calls on wasm32 and memory64, as do the C-dispatch and next-only adapter suites.
The full Node profile passes all 24 suites; validation parity skips because a
native `lusdcat` executable is not configured in this build.

The full Node profile was rerun against the isolated rebuilt wasm32 module
after the graph-copy change and passes 24/24 suites. The generated module under
`web/js/src/lightusd` predates kinds 10 and 11, so direct tests against that
artifact reject the new kinds; isolated test runs use the CMake output override
and leave packaged application artifacts untouched. The memory64 next-only
USDZ/RenderStream suite passes against its rebuilt module. Native validation
parity remains unrun because this checkout has no configured `lusdcat` binary.

The interactive-session workflow now uses the installed C++ document/render
facades throughout, replacing local `unique_ptr` aliases, raw output handles,
and manual adoption with `DocumentSession`, `DocumentSnapshot`, `RenderSession`,
and `RenderScene`. The facade adds `DocumentSnapshotHasPrim` so retained
snapshot queries also stay within that boundary. No direct C function calls
remain in the example; POD configuration and event callbacks retain their
existing C layouts. The native workflow regression passes, including variant
selection, payload load/unload, layer reload, retained snapshots, and cancelled
rebuilds. The standalone Debug next build completes with 43 tests passing and
one optional AOUSD corpus skip (44 registered). No compile-time or size gain
is claimed for this consumer migration.


The combined loader streaming family now uses counted byte inputs, a 40-byte
progress/allocation record, and bounded UUID/name/error table copies. All 15
methods are classified as typed C. The combined build excludes their Embind
registrations and aggregate emval builders; the legacy build keeps them and
shares the allocation/storage implementation. Public results retain UUIDs,
sorted active-buffer enumeration, float-rounded progress, borrowed numeric
buffer addresses, finalization/cache ownership, binary chunk contents, and
missing-key failures. Chunk appends retain their existing behavior of storing
an oversized chunk while returning false. Zero-copy byte accounting now checks
remaining capacity by subtraction, fixing a wasm32 overflow that could wrap
progress backward instead of saturating and returning false.

| Streaming method signature | Combined C replacement |
|---|---|
| `startStreamingAsset(name, expectedSize)` | `stream_size_op(STREAM_START)` |
| `appendAssetChunk(name, bytes)` | `stream_op(STREAM_APPEND)` |
| `finalizeStreamingAsset(name)` | `stream_op(STREAM_FINALIZE)` |
| `isStreamingAssetComplete(name)` | `stream_op(STREAM_COMPLETE)` |
| `getStreamingProgress(name)` | `stream_info_get(kind=0)` plus UUID copy |
| `allocateZeroCopyBuffer(name, size, maxBytes)` | `stream_allocate()` plus UUID/name/error copies |
| `getZeroCopyBufferPtr(uuid)` | `stream_op(ZERO_PTR)` |
| `getZeroCopyBufferPtrAtOffset(uuid, offset)` | `stream_size_op(ZERO_PTR_OFFSET)` |
| `markZeroCopyBytesWritten(uuid, count)` | `stream_size_op(ZERO_MARK)` |
| `getZeroCopyProgress(uuid)` | `stream_info_get(kind=1)` plus UUID/name copies |
| `finalizeZeroCopyBuffer(uuid)` | `stream_op(ZERO_FINALIZE)` |
| `cancelZeroCopyBuffer(uuid)` | `stream_op(ZERO_CANCEL)` |
| `getActiveZeroCopyBuffers()` | `stream_op(ZERO_KEYS)` plus per-UUID progress records |
| `setMMapZeroCopy(enabled)` | `stream_op(MMAP_SET)` |
| `getMMapZeroCopy()` | `stream_op(MMAP_GET)` |

The function prefix is `lightusd_combined_`; operation constants use
`LIGHTUSD_COMBINED_`. Numeric size conversion follows the module width: wasm32 keeps integer
truncation/NaN-to-zero; memory64 accepts numbers or BigInts, rejects
fractional/NaN/infinite values, and preserves the full unsigned 64-bit range
through low/high uint32 words. The older double-valued C entry points remain
range-checked; JS size-bearing calls now use the exact word transfers. Wrong argument
counts, types, and dead receivers produce `TypeError`. String inputs accept
UTF-8 strings, ArrayBuffers, and one-byte views, including heap-backed subviews
copied before an allocation can grow memory. The baseline behavioral fixture
passed first against Embind on both pointer widths, then unchanged on the
rebuilt typed modules. Additional fixtures check dispatch through the C
exports, short/null records, invalid native sizes, dead receivers, binary
heap subviews, and overflow saturation. The legacy binding passes a syntax
check with the next macro removed. This migrates the combined boundary; it
does not yet supply a next-only streaming cache or prove full product parity.

The next-only RenderStream now supplies UUID identity and legacy-shaped
progress for an in-flight streamed asset. It also provides JS compatibility
adapters for the UUID-based zero-copy buffer calls, backed by the same
bounded streaming allocation and explicit written-byte accounting. The
adapters retain the original heap address for progress records after the
write is complete; finalization/cancellation removes the UUID mapping, and
enumeration prunes transfers consumed as a streamed root. `getMMapZeroCopy` /
`setMMapZeroCopy` remain unsupported: next-only buffers are ordinary WASM
heap allocations and do not provide mmap semantics. The next-only API now
publishes both methods explicitly and throws a clear unsupported error;
argument-count and setter-type behavior plus this product error are covered
by wasm32/memory64 dispatch tests and the checked RenderStream inventory.

| Legacy next RenderStream method | Next-only implementation |
|---|---|
| `startStreamingAsset(name, size)` | Native streamed allocation; records generated UUID |
| `appendAssetChunk(name, bytes)` | Compatibility alias for `appendStreamingAsset(name, bytes)` |
| `getStreamingProgress(name)` | UUID, byte counts, completion, and percentage adapter |
| `isStreamingAssetComplete(name)` | `getStreamingProgress(name).complete` |
| `allocateZeroCopyBuffer(name, size, maxBytes)` | `startStreamingAsset` plus returned UUID/address record |
| `getZeroCopyBufferPtr(uuid)` | Retained WASM heap address for active transfer |
| `getZeroCopyBufferPtrAtOffset(uuid, offset)` | Retained address plus checked in-allocation offset |
| `markZeroCopyBytesWritten(uuid, count)` | `markStreamingAssetBytesWritten` by UUID lookup |
| `getZeroCopyProgress(uuid)` | Legacy-shaped record assembled from stream progress |
| `finalizeZeroCopyBuffer(uuid)` | `finalizeStreamingAsset` by UUID lookup |
| `cancelZeroCopyBuffer(uuid)` | `cancelStreamingAsset` by UUID lookup |
| `getActiveZeroCopyBuffers()` | Sorted UUID mapping enumeration and stale-entry pruning |
| `finalizeStreamingAsset(name)` | Native stream finalization (shared implementation) |
| `getMMapZeroCopy()` / `setMMapZeroCopy(enabled)` | Explicit unsupported methods; no mmap-backed WASM storage |

The post-streaming wasm32 Node profile passes all 24 suites with native
validation parity enabled against the rebuilt `build_ninja/lusdcat` (no
skipped suites). The focused combined API regression passes on wasm32 and
memory64. Generated application modules are unchanged; isolated output modules
were selected through `LIGHTUSD_COMBINED_MODULE` and the Node override loader.
The full `npm test` gate was also started; its Menagerie/browser results must
be checked separately before treating that gate as complete.

The quickstart and animation-sampling examples now also use the public C++
facade for error reporting, save-option initialization, string views, prim
traversal, and sampled skeleton queries. Animation traversal retains `Prim`
owners rather than keeping borrowed C handles in its pending stack. All four
native workflow examples now use the C++ facade throughout, and their CMake
link setup no longer contains an unreachable internal-next fallback. The root
workflow integration test passes; the standalone Debug next build completes
and its suite has 43 passes and one optional AOUSD corpus skip.


The combined asset-cache/resolver family now uses typed C for all 28 methods.
The checked 208-method loader inventory classifies 108 methods as typed C and
100 as Embind. Scalar operations use `lightusd_combined_asset_op`; size_t
counts and cache limits use `asset_size_op` with low/high uint32 words,
preserving memory64 BigInt results and exact limits through `UINT64_MAX`.
`asset_info_get` exposes borrowed byte address/length records plus bounded
name/hash/UUID strings. JS builds `getAsset`/`getAssetByUUID` with owned byte
copies and `getAssetCacheDataAsMemoryView` with an explicitly borrowed view.
`asset_strings` supplies hash/UUID lookup, base/search paths, and name/UUID
pairs. `asset_set_raw` preserves the existing return value (whether an entry
was overwritten), while rejecting spans outside the current WASM heap before
reading them. The same raw-ingest bounds check protects the legacy product.

The behavior fixtures were pinned against the pre-migration modules on both
widths. They preserve empty and missing assets, binary/Unicode inputs, copy
ownership after deletion, UUID replacement, name-versus-UUID deletion, duplicate
search paths, parent-relative configuration, streaming UUIDs, and cache clear
semantics (active zero-copy buffers survive). The legacy cache limit remains
an eviction target: new entries evict in name order, overwrites bypass eviction,
and individually oversized entries are admitted. These policies are preserved
explicitly, not presented as a hard memory budget. C regressions cover null or
undersized records, invalid keys/operations, out-of-heap raw spans and stale
JS receivers. No next-only cache counterpart or complete product parity is
claimed by this boundary migration.

The size_t audit also corrected the preceding streaming adapters on memory64:
they now accept BigInts and preserve full-width expected sizes, offsets, written
counts and allocation limits. The baseline BigInt fixture passes against the
original Embind module; native word-based calls preserve it without narrowing
through double. wasm32 still uses its original number/boolean conversion.

An independent SHA-256 digest assertion exposed an existing wasm32 defect in
`src/sha256.cc`: the message bit count used size_t, causing shifts of 32 or
more bits on a 32-bit value and incorrect hashes. The count is now uint64_t
before multiplication and encoding. Full blocks are hashed directly, with
128 bytes of padding scratch, eliminating the asset-sized temporary allocation;
fixed hexadecimal formatting also removes the iostream dependency. Existing
huge-size rejection remains, and null/nonempty input is rejected. Known empty,
ASCII, binary, block-boundary and multi-block vectors exercise native and WASM
hashes. This intentionally changes incorrect wasm32 digest strings to standard
SHA-256 values; UUID allocation and cache lookup semantics remain unchanged.

After the cache and exact-integer migrations plus SHA-256 correction, the
focused combined API fixture passes on rebuilt wasm32 and memory64 modules.
The wasm32 Node profile passes 24/24 suites with native validation parity and
no skips. The full native unit CTest passes; rebuilt focused SHA-256 tests
also pass after a warning-only guard cleanup. The legacy binding's syntax
check passes with `LIGHTUSD_WASM_WITH_NEXT` removed. The earlier full web gate
continues against the preceding streaming checkpoint's isolated module;
its result is not evidence for the later asset-cache changes. Complete final
product and browser parity gates remain open.

The combined loading/diagnostics family now routes all 22 methods through
checked C entry points. The 208-method inventory is now 130 typed-C methods
and 78 Embind methods. `loading_op` covers synchronous loading, cached/layer
loading, lifecycle controls, status/error strings and validation JSON.
`loading_progress_get` returns a 72-byte POD plus five counted strings;
`loading_memory_probe` returns names and exact low/high size words, preserving
memory64 BigInt results. Negative or over-budget memory probes now return a
failure object in both products instead of attempting an unbounded allocation.
The bound is a conservative per-probe admission check, not an accounting claim
for the complete scene or process.

Combined async loading now uses C++17 `loading_async_begin/step/end` tasks.
JS owns scheduling, retains the loader until `end`, and reconstructs the
existing success/error payload. Native phases preserve the original yields
and progress callbacks. JS callback exceptions are captured until the native
call returns, then propagated without retrying a completed call as a pointer
conversion failure. The callback guard supports nested calls and callback
replacement. Cleanup runs on success, pipeline failure and callback rejection;
deleting the original JS receiver during a phase does not destroy its pending
task. Raw C callers must retain the loader through `end` and must not invalidate
an active task from inside its callbacks. The legacy product keeps its
coroutine binding; combined and next bindings select C++17 in CMake.

Pre-migration fixtures passed against the previous asset checkpoint on both
widths. Rebuilt loading fixtures preserve idle/completed progress quirks,
cancellation/reset state, missing/empty cache errors, parse-only `loadTest`,
layer JSON, validation, six async phase notifications and all 19 memory-probe
rows. Additional checks cover null/short C records, invalid operations,
expired task IDs, memory-probe rejection, callback TypeErrors and retained
async ownership. The wasm32 Node profile passes 24/24 suites with native
validation parity; focused loading checks pass on memory64 too. Both legacy
and combined C++17 binding syntax checks pass. These checks cover the combined
boundary over the existing legacy loader backend; next-only loading/render
parity and the final product gates remain unfinished.

The final loading checkpoint was rebuilt after enabling C++17 in CMake on
both pointer widths. Both complete combined builds pass; their command graphs
contain 357 C++ compilation commands, all selecting C++17. The exact rebuilt
wasm32 artifact passes the 24-suite Node profile with no failures, and the
memory64 artifact passes the focused combined inventory/behavior fixture.
The added synchronous debug-callback test also confirms exception propagation
without replay, recovery on a later load, and DataView byte-subview input for
`loadTest`. The public combined header passes strict C11 syntax checking and
the legacy coroutine binding passes its final C++20 syntax check. No build-time
speedup is inferred from these incremental rebuilds. The older streaming
checkpoint's full web gate remains running separately; final browser/product
parity has not been claimed or used to switch product defaults.

All six combined MCP methods now use `lightusd_combined_mcp_op` with counted
UTF-8 inputs and bounded JSON string-table copies. Context creation/selection
return booleans; tool/resource methods preserve their existing JSON strings.
The adapter determines pointer width with a read-only query before dispatch,
so it never retries a potentially mutating MCP operation after a JavaScript
`TypeError`. The JS receiver is retained for the duration of the call. The
208-method inventory now records 136 typed-C methods and 72 Embind methods.
Remaining Embind families are layer export (15), render queries (48), and
schema/image utilities (9). This boundary migration retains the current MCP
backend; a next-only MCP implementation and complete product parity remain
required before the final default switch.

MCP behavioral fixtures were first pinned on the pre-migration wasm32 and
memory64 modules. They cover missing/duplicate/empty contexts, active-context
preservation on failed selection, two-session and two-loader asset isolation,
Unicode and embedded-NUL names, byte-string inputs, resource roundtrips,
invalid JSON and unknown tools/resources. New checks verify dispatch through
all six C operations, null/nonempty input rejection, invalid operation IDs,
wrong JS arguments, stale receivers, and no replay after a dispatch exception.
Both combined C++17 builds, strict C11 header syntax, and legacy C++20 binding
syntax checks pass. The focused memory64 fixture also passes.
The final MCP wasm32 Node profile passes all 24 suites with native validation
parity enabled and no skipped suites. Isolated `artifacts-mcp` outputs keep the
loading and streaming checkpoints intact. The previously started full web
gate is still live against the streaming checkpoint; it does not verify these
newer MCP changes or close the final browser/product parity gate.

Eight more combined layer/export methods now cross typed C:
`layerToString`, `layerToJSON`, `layerToJSONWithOptions`, `exportAsUSDA`,
`flattenLayer`, `layerToRenderScene`, `exportAsUSDC`, and
`exportLayerAsUSDCWithOptions`. Text/scalar operations use `export_op` and the
bounded string table. USDC uses `export_usdc`, which returns an independent
owning byte-result handle and a 24-byte address/length record; callers release
it with `export_release`. Its bytes remain valid through subsequent exports,
loader reset and loader destruction. JS takes an owned copy and releases the
native result in `finally`, including when a progress callback throws after
native serialization completes. The two legacy USDC wrappers now share the
same native serialization helper with the C boundary.

The inventory is now 144 typed-C and 64 Embind methods. Seven layer/export
methods still need migration: the two caller-supplied USDC buffer writers,
four USDZ exporters, and asset-path remapping. Their buffer/view and option
semantics remain part of the scope; they have not been replaced by a narrower
export feature set. Render queries (48) and schema/image utilities (9) also
remain, along with next-only backend parity and the final product gates.

Pre-migration fixtures passed on both widths after recording the actual JSON
behavior: both JSON exporters retain the authored custom string, but the
current JSON loader rejects this custom-property fixture. Unknown array modes
follow the Base64 path. The tests preserve these existing results rather than
claiming new JSON import support. Text, flatten/render conversion, owned USDC
results, comparison with the existing buffer writer, and USDC reopening pass
on rebuilt wasm32 and memory64 modules. C checks cover null/short records,
invalid operations/options, independently retained bytes and stale receivers.
The complete wasm32 Node profile passes 24/24 suites, strict C11 header syntax
and legacy C++20 binding syntax pass, and neither combined build reports an
error. An inspection of active combined registrations confirms that none of
the 144 classified typed-C methods remains registered through Embind. The
isolated outputs are `artifacts-export`; final next-only parity is still open.

The remaining seven combined layer/export methods now use typed C, completing
that 15-method family. `export_usdc_buffer` validates output metadata and
returns an independent byte-result handle; JS invokes the caller's `set`
method and calls `export_buffer_finish` only after a successful copy. Warning
publication therefore retains its previous ordering, and nested exports cannot
invalidate the outer copy. Throwing copy callbacks release the result without
replaying serialization. Both legacy buffer wrappers now share the same native
serialization/validation implementation. The API still materializes the crate
in a native vector before copying; this is not a new streaming writer or a
claim of reduced peak export memory. Out-of-range byte lengths are rejected
before size_t conversion in both products.

USDZ now uses `package_begin/write/end`: stage preparation owns a snapshot
across JS remap/option getters, while layer packaging reads the authored layer
at the existing point in the operation. The output remains a borrowed view of
the loader's package buffer. Counted map inputs preserve Unicode, embedded NULs
and the legacy one-byte-view values. `export_optimize` accepts an explicit
24-byte options record with a presence mask and four integer settings; omitted
fields retain native defaults. Optimization order, aliases, integer coercions,
root-format defaults, ARKit axis/format overrides, media filtering, composition
layer inclusion and direct layer-path remapping remain covered. Invalid masks,
short records, malformed maps and cross-loader preparation handles are rejected.
The JS adapter retains the loader and destroys preparation handles even when a
getter throws.

The baseline fixtures passed on both pre-migration pointer widths, including
custom byteLength/set output objects, error precedence before property access,
small-buffer retries, subview sentinel preservation, nested copies, getter
mutation order and throwing callbacks. Populated optimization fixtures now
also verify actual material deduplication and mesh merge limits. They preserve
the current distinctions: exact dedupe retains the differently named materials
in this fixture; preview/atlas modes deduplicate them; merging adds an aggregate
and deactivates the two source meshes rather than erasing those prims. Atlas
mode's existing preview-dedupe fallback is unchanged; no new texture-atlas
capability is claimed.

The inventory is now 151 typed-C and 57 Embind methods, with only render
queries (48) and schema/image utilities (9) remaining at the combined loader
boundary. Both complete combined C++17 builds pass. The final wasm32 Node
profile passes 24/24 suites with native validation parity, and memory64 passes
the complete focused combined fixture. Strict C11 header checking and legacy
C++20 coroutine-binding syntax checking pass. An active-registration audit
finds no classified typed-C methods still registered through Embind. Isolated
`artifacts-package` outputs preserve all earlier checkpoints. The older full
web gate remains live against the streaming checkpoint, and the final browser,
next-only backend parity, and default-switch requirements remain open.

Twenty-one combined render-query methods now cross typed C: eleven resource
counts, default-root ID, URI/up-axis, the four native optimization getters,
and camera/scene-metadata/texture payloads. The existing typed configuration
getter handles the four booleans. `render_scalar` returns checked numeric or
counted-string results; the aggregate queries use fixed 72-byte camera,
128-byte metadata and 152-byte texture records plus counted strings. Camera
errors, unloaded empty objects, nullable time bounds, exact float promotion,
owned matrix/bias/scale arrays, texture transforms and optional UDIM fields
retain their existing shapes. Combined builds exclude the three migrated
Emval object builders. The inventory is now 172 typed-C and 36 Embind methods:
27 remaining render queries and nine schema/image utilities.

The pre-migration scalar and ordinary camera/metadata/texture fixtures passed
on both widths. Additional checks reject null/short records, invalid query
keys, wrong JS arguments and dead receivers. Populated fixtures exercise
orthographic apertures/FOVs, authored units/time ranges, matrix independence,
texture color adjustments, transforms and UDIM linkage. Testing UDIMs exposed
an existing WASM abort: UDIM discovery and atlas image decoding probed the
filesystem before consulting the configured resolver, although these products
link without filesystem support. Those direct probes are now native-only;
WASM UDIMs use asset resolution. Native filesystem behavior remains unchanged.
The resulting fixture passes for cached sparse tiles and a packaged two-tile
atlas, including its UV transform and index fields. The native unit suite
passes after the change (71.37 seconds).

A remaining cache-resolver limitation was observed while extending that test:
`EMAssetResolutionResolver::Resolve` echoes missing names, so cache-based atlas
discovery may include nonexistent tile coordinates and expand the grid. Sparse
loading skips them at read time; the packaged resolver reports actual misses
and yields the expected atlas extent. This is not counted as closed parity;
a future resolver/existence change needs coverage for missing tiles and atlas
resolution, without introducing unbounded or duplicate whole-asset reads.

Both rebuilt combined C++17 products and the final focused fixture pass on
wasm32 and memory64. Strict C11 header and legacy coroutine-binding syntax
checks pass. The active-registration audit finds no typed-C methods left in
Embind. The outputs are isolated in `artifacts-render`; full next-only parity,
remaining payload migrations and final product/default gates remain open.
The final wasm32 Node profile, rerun with the complete UDIM/atlas fixture,
passes 24/24 suites with native validation parity and no skips. The earlier
full web gate is still live on the preserved streaming checkpoint; it is not
used as evidence for the current render-query or UDIM changes.

Two more combined render queries, `getMhProfileJSON` and
`getShadingGraphJSON`, now use `lightusd_combined_render_json` and bounded
string-table copies. Their existing native JSON producers remain shared with
the legacy bindings. The adapter retains the loader during inspection, chooses
the pointer width before dispatch, and propagates callback/dispatch exceptions
without retrying the native query. The combined inventory now records 174
typed-C methods and 34 Embind methods (25 render queries and nine schema/image
utilities). Combined builds exclude both migrated Embind registrations.

Behavior fixtures passed first on both pre-migration pointer widths. They
cover unloaded errors, reset, retained JS strings, Unicode MetaHuman values,
relationships, shader connections, color values and asset paths. Additional
boundary checks reject null loaders, invalid query IDs, wrong argument counts
and stale JS receivers, and verify that a dispatch TypeError is raised once.
These changes migrate the combined boundary only; next-only backend parity,
remaining payload migrations and final product/default gates remain open.

Both rebuilt combined products pass, with isolated outputs in
`artifacts-inspection`. The final memory64 focused fixture passes, and the
final wasm32 Node regression profile passes all 24 suites. Strict C11 header
checking and the legacy C++20 coroutine-binding syntax check also pass.

`getInstance` and `getInstancesForMesh` now cross typed C. The former returns a
280-byte record with two row-major double matrices, signed IDs and visibility,
plus counted name/path/display strings. The latter publishes matching indices
in scene order through the existing integer table. JS returns owned ordinary
arrays, preserves null for missing instances and retains literal negative mesh
ID matching (including populated native instances with mesh ID `-1`). Combined
builds exclude both Emval builders and registrations; the legacy product keeps
them. The inventory is now 176 typed-C and 32 Embind methods: 23 render queries
and nine schema/image utilities remain.

Pre-migration fixtures passed on wasm32 and memory64 for scaled/translated
point instances, invalid indices, ordered shared-mesh matches and independent
matrix copies. An additional baseline probe verified negative mesh ID matches
for native instances without geometry on both widths. Boundary regressions
cover short/null records, invalid JS arguments, dead receivers and allocation
size overflow. No next-only backend or final product parity is inferred from
this combined-boundary migration.

Both complete combined builds and final relinks pass, using isolated
`artifacts-instances` outputs. The memory64 focused fixture and all 24 wasm32
Node regression suites pass. Strict C11 header and legacy C++20 coroutine
binding syntax checks pass. The older full web gate was polled and remains
live on the streaming checkpoint; its Menagerie progress has advanced, but
it is not evidence for these later instance-boundary changes or final parity.

`getRootNode` and `getDefaultRootNode` now use the typed
`nodes_begin/next/end` cursor and a 288-byte node record. Native preorder
traversal retains only ancestor frames; JS reconstructs the ordered hierarchy
iteratively, copying numeric fields before any string allocation can grow
WASM memory. This avoids both recursive Emval construction and a second native
copy of the entire hierarchy. Raw C cursors borrow scene nodes: the caller
must keep the loader alive and leave its render scene unchanged until `end`.
The JS adapter retains its receiver and closes the cursor on success or error.
The legacy builders remain available only in the legacy product. The combined
inventory now has 178 typed-C and 30 Embind methods (21 render queries and nine
schema/image utilities).

Baseline hierarchy fixtures passed on both pointer widths. They pin missing
roots, default-root selection, sibling order, Unicode display names, reset
transforms, owned arrays and fractional-index conversion. Additional baseline
probes pin mesh/camera/light categories and native-instance flags, including
the existing `-1` prototype/instance IDs in those node records. C/JS boundary
checks cover invalid records without advancing the cursor, repeated traversal
completion, wrong arguments, cleanup after dispatch exceptions and retention
when the original JS receiver is deleted during traversal. Next-only backend
parity, remaining payload migrations and final product gates remain open.

Both complete combined C++17 builds pass with isolated `artifacts-hierarchy`
outputs. The final memory64 fixture and all 24 wasm32 Node regression suites
pass. Strict C11 header checking, legacy C++20 coroutine-binding syntax and
whitespace checks also pass. These results validate the combined hierarchy
boundary; they do not close the final browser or next-only product gates.

`getUDIMTexture` and `extractUnresolvedTexturePaths` now use typed C tables. `getUpAxis()` now aliases the bounded scene up-axis name query, closing another same-name scene-query row in the next-only matrix; its dispatch fixture verifies equality on wasm32 and memory64. `getTexture()` also assembles the legacy texture metadata shape from the retained next texture fields, including sampler names, bias/scale, authored transform, and sparse UDIM linkage; it returns before requesting any decoded image payload. Its sparse-UDIM dispatch fixture checks the image link and four-channel values on both pointer widths.
A separate `NextAssetStore` parity matrix compares the asset-resolution family against the store surface rather than RenderStream. It records matching UUID, hash, cache, memory-asset, working-path, search-path, parent-relative policy, and streaming-UUID APIs across all 28 legacy asset methods. `getAssetCount()` reports the registered identifier count; a cached composition fixture verifies that the imported parent-relative policy controls resolution of a `../` memory asset on both WASM widths.

Sparse UDIM queries publish four counted metadata strings and four integer
words per tile; the adapter returns owned tile objects, preserving signed
fields and the native container's iteration order. Unresolved paths retain
image order and duplicates. Missing/unloaded UDIMs still return `{}` and empty
path lists still return `[]`. Both Emval builders and registrations are now
excluded from combined builds. The inventory records 180 typed-C methods and
28 Embind methods (19 render queries and nine schema/image utilities). Next RenderStream now retains progress events as a legacy-shaped `getProgress()` record and exposes `cancelParsing()`, `isParsingInProgress()`, `wasCancelled()`, and `resetProgress()`; a real USDA dispatch fixture cancels from a progress callback and verifies cancelled, idle, and reset states on wasm32 and memory64. RenderStream `ok()` now reports whether conversion published a scene and `warn()` aliases the bounded warning query; dispatch checks both methods before and after a successful load.

The baseline fixture passed on both widths after pinning the observed tile
order (1012 precedes 1001 in the two-tile fixture). It also verifies Unicode
unresolved paths, image-order correspondence, reset, missing IDs and independent
returned values. New C/JS checks reject null pointers, wrong arguments, dead
receivers and allocator-size overflow. The existing cached-atlas discovery
limitation remains open; this boundary change does not alter asset resolution.

The complete combined builds pass with isolated `artifacts-udim` outputs. The
final memory64 fixture and all 24 wasm32 Node regression suites pass, as do
strict C11 header and legacy C++20 coroutine-binding syntax checks. The older
full web gate remains live on the preserved streaming checkpoint and has
advanced through additional Menagerie models. Remaining payload migrations,
next-only backend parity and final product/default gates are still open.

The three image accessors (`getImage`, `getImagePtr`, `getImageCopy`) now use a
128-byte typed C record with dimensions, flags, buffer ID, byte address/length,
color-transfer parameters and a 3x3 matrix, plus four counted strings. JS
reconstructs the existing metadata and creates either a live heap view, an
address/length descriptor or an owned byte copy. Numeric fields are copied
before string allocations, and heap views are created only after all string
copies. `getImageCopy` retains its existing lazy USDZ byte materialization;
`getImagePtr` remains inspection-only. This is not a new streaming decoder or
an export-memory reduction claim.

The deprecated `getImage` warning now uses native per-loader state and a JS
console call, preserving its wording and once-only behavior across reset and
cloned receivers, including when the console callback throws. The adapter
retains the loader and chooses the pointer ABI before lazy materialization,
so a TypeError cannot replay that operation. The combined build excludes the
old image Emval builders and registrations. The inventory now records 183
typed-C and 25 Embind methods (16 render queries and nine schema/image
utilities).

The image behavior fixture passed first on wasm32 and memory64 baseline
artifacts. Additional tests cover custom color-space transforms with missing
buffers, heap growth during metadata copying, null/short C records, invalid
modes, stale receivers, shared warning state and one-shot failed dispatch.
Remaining resource payloads, next-only backend parity, allocation-budget gaps
and the final product/default gates remain open.

Both complete combined builds pass with isolated `artifacts-images` outputs.
The wasm32 Node regression profile passes all 24 suites, and memory64 passes
the focused fixture. A final baseline probe additionally pinned inspection-
only lazy USDZ behavior (no buffer address, dimensions/channels `-1`, exact
float-promoted gamma/bias); the strengthened fixture passes on both rebuilt
pointer widths. Strict C11 header checking, legacy C++20 coroutine-binding
syntax and whitespace checks pass. Final browser/next-only product parity
has not been claimed.

`getLight`, `getAllLights` and `getLightWithFormat` now use typed C. Structured
queries return a 424-byte record with 49 double components, flags, signed
resource IDs and an optional borrowed float-pair spectral span, plus counted
strings. JS returns independent arrays and nested sample pairs. Formatted
queries share the native serializer with the legacy wrapper and preserve exact
JSON/XML strings, format/error shape and validation precedence. Combined
builds exclude the old light Emval builders and registrations. The inventory
is now 186 typed-C methods and 22 Embind methods (13 render queries and nine
schema/image utilities).

Complete baseline light objects and JSON/XML text were captured in
`web/js/tests/fixtures/combined-light-queries.json`. Both pointer widths produce
identical snapshots. Explicit signed-zero markers avoid losing negative zero
in direction vectors when storing that baseline. The behavior fixture passed
against both original products, including unsupported-format errors, byte-view
format strings, fractional indices, reset and independent arrays. New C/JS
checks cover null/short records, invalid counted inputs, stale receivers and
serialization exceptions without replay.

Baseline probing also exposed limits of these fixture names: the legacy loader
omits `spectralEmission` for both spectral input fixtures and emits only the
ordinary sphere light from `usdlux_mesh_lights_simple.usda`. The GeometryLight
viewer fixture fails conversion with a missing geometry relationship through
both direct loading and layer-to-render conversion. These are not closed
backend parity results. An injected C-record test separately verifies spectral
pair copying (including signed zero), empty samples and invalid span rejection;
it does not claim end-to-end spectral loading. Remaining resource migrations,
backend gaps and final product/default gates stay open.

Both complete combined builds pass with isolated `artifacts-lights` outputs.
The final memory64 fixture, including the explicit spectral-span adapter test,
and all 24 wasm32 Node regression suites pass. Strict C11 header, legacy C++20
coroutine-binding syntax and whitespace checks pass.

The older full web gate on the streaming checkpoint has now terminated: 27
phases passed, including all 67 Menagerie MJCF/USD/MJCF roundtrips; two browser
sweeps failed while awaiting Vite. Local HTTP probes returned `EPERM` inside
the sandbox. An elevated probe started Vite and returned HTTP 200, identifying
sandbox networking as the startup blocker. The elevated GPU probe reports an
NVIDIA discrete GPU. The first browser retry reached Puppeteer but found its
pinned Chrome absent; the installed Chrome is available through
`PUPPETEER_EXECUTABLE_PATH`. Browser verification is being resumed using that
executable and the existing conversion outputs; no browser pass is claimed yet.
These browser harnesses still use the application's packaged modules, not the
isolated typed-boundary artifacts, so they do not replace final product gates.

`getSkeleton`, `getAllSkeletons` and `getSkeletonJointsFlat` now cross typed C
through `skeleton_begin/next/end`. A 16-byte header carries animation ID and a
borrowed cursor; each 272-byte joint record carries joint ID, child count and
two row-major double matrices, with counted name/path strings. Native traversal
retains only ancestor frames. JS constructs either the hierarchy or the flat
arrays directly, preserving preorder parent indices without building a second
native flattened copy. The adapter retains its loader and closes cursors on
success or failure. Raw C callers must keep the loader alive and its scene
unchanged until cursor release.

Both pointer-width baselines pass the branching skin fixture, including the
nontrivial `[0,1,3,2]` joint order, Unicode metadata and owned transform arrays.
The strengthened baseline also passes two separately animated rigs with
independent skeleton and animation IDs. New boundary checks cover invalid
records, non-advancing failed reads, traversal completion, stale receivers,
exception cleanup and retained ownership. The old recursive skeleton Emval
builders and native temporary flat arrays are excluded from combined builds.
The inventory is now 189 typed-C and 19 Embind methods (10 render queries and
nine schema/image utilities); next-only backend and final product gates remain
open.

Both complete combined builds pass with isolated `artifacts-skeletons`
outputs. The final memory64 focused fixture and all 24 wasm32 Node regression
suites pass. Strict C11 header checking, legacy C++20 coroutine-binding syntax
and whitespace checks pass.

The elevated regular browser retry has completed successfully: all five
representative Menagerie models render both source and converted USD views.
The OffscreenCanvas sweep is now running outside the sandbox with the installed
Chrome and the completed run-17 conversion artifacts. These remain packaged-
module browser checks; no claim of final isolated-artifact browser parity is
made while the remaining boundary and backend migrations are open.

The four combined animation queries (`getAnimation`, `getAllAnimations`,
`getAnimationInfo`, `getAllAnimationInfos`) now use typed C records: 64-byte
clip metadata, 40-byte sampler spans and 32-byte channels, with counted strings.
JS reconstructs the existing track names, custom-property types, raw channels
and summaries. Tracks and raw samplers receive independent owned Float32Array
copies; borrowed C spans require an unchanged live loader. Combined builds
exclude the four Emval builders and registrations. The inventory now records
193 typed-C methods and 15 Embind methods (eight render queries and seven
schema/image utilities).

The checked animation snapshot was captured from pre-migration builds for
both pointer widths. It preserves their differing native clip enumeration
order, typed float arrays and signed zero. Fixtures cover transform, skeletal,
blend-shape and custom scalar/vector channels, timing, summaries, invalid track
filtering and ownership across reset. The legacy summary getter also now
rejects negative animation indices before indexing its vector; this is a bounds
fix, not a claim that the previous undefined access was supported behavior.
Boundary tests cover null/short records, missing indices, invalid JS arguments,
unsafe spans, one-shot dispatch exceptions and retained loader ownership.
Injected sampler records test STEP/CUBICSPLINE mapping separately from native
loading coverage. Value-clip backend parity is not established by these tests.
Next-only parity, allocation-budget work and final product/default gates remain
open.

Both complete combined C++17 builds pass with isolated `artifacts-animations`
outputs. The final focused fixture passes on wasm32 and memory64, and the
wasm32 Node profile passes all 24 suites, including native validation parity.
Strict C11 header checking, legacy C++20 coroutine-binding syntax and whitespace
checks pass. The packaged-module OffscreenCanvas sweep remains in progress;
these results do not close final browser or next-only product parity.

`getMeshPrimvarsJSON` and `computeMeshTangents` now use the typed
`lightusd_combined_mesh_operation` entry point. Primvar JSON uses the counted
string table; deferred tangent computation returns a scalar boolean and keeps
its existing mesh-cache invalidation. Native implementations remain shared
with the legacy product. The JS adapter retains its loader and selects the
pointer ABI before dispatch, so callback exceptions cannot replay the mutation.
Combined builds omit both Embind registrations. The inventory now contains
195 typed-C methods and 13 Embind methods (six render payload methods and
seven schema/image utilities).

The behavior fixture passed on both pre-migration builds. It checks indexed
primvar flattening through layer loading, actual deferred tangent generation
with a normal-map material, repeat calls, invalid IDs and retained JSON after
reset. It also pins existing inspection limitations: direct scene loading
returns no authored primvars for this fixture, and the float-array value type
label is `float[][]` on wasm32 versus `float[]` on memory64. These are backend
limitations rather than new bridge semantics or closed parity requirements.
Boundary tests reject unknown operations, null loaders, invalid JS arguments
and stale receivers, and verify retained ownership and one-shot exceptions.

Both combined C++17 builds pass with isolated `artifacts-meshops` outputs.
The final memory64 focused fixture and all 24 wasm32 Node suites pass.
Strict C11 header checking, legacy C++20 coroutine-binding syntax and whitespace
checks pass. The packaged-module OffscreenCanvas sweep is still live; final
browser, next-only backend, allocation and product-default gates remain open.

`generateBoneTexture` now uses `bone_texture_begin/end` and a 72-byte record.
Its result owns both float arrays independently of the loader until explicit
release; JS copies them and releases the native result even when span validation
fails. The shared native generator replaces the loader's retained bone-texture
cache, and the combined product excludes the Emval builder and registration.
The inventory now records 196 typed-C methods and 12 Embind methods: three
mesh payload accessors, two material queries and seven schema/image utilities
remain. The material queries are classified as render queries; earlier
checkpoint summaries incorrectly grouped them with schema/image utilities.

Baseline fixtures verify all standard influence-rounding thresholds, padding,
vertex offsets, owned copies, fractional-index conversion and errors on both
pointer widths. A six-influence fixture pins the existing cap-before-sort rule,
positive-weight filtering and descending-weight order. The memory64 baseline
also exposed incorrect size_t-to-Emval dimensions (`2^63 + dimension` BigInts).
Both products now publish fixed-width numeric dimensions; this is an explicit
bug fix, not baseline parity for that erroneous representation. The shared
generator checks short weight arrays and bounds the vertex/texel multiplication
before allocating. These checks do not close the general allocation-budget
work or establish arbitrary allocation-failure recovery for native vectors.

C-boundary regressions cover null/short records, owned results surviving another
generation and loader reset, invalid arguments, span rejection with cleanup,
one-shot dispatch errors and deleting the original receiver during generation.
Remaining payload/backend migrations and final product/default gates stay open.


Both complete combined C++17 builds pass with isolated `artifacts-bones`
outputs. The final memory64 focused fixture and all 24 wasm32 Node suites pass.
Strict C11 header, legacy C++20 coroutine-binding syntax and whitespace checks
pass. The packaged-module OffscreenCanvas sweep remains live and advancing;
final browser/next-only product parity has not been claimed.

Mesh boundary preparation now shares native tangent decoding, material-group
ordering and tangent expansion between the copy/view producer and descriptor
producer. `getMeshPtr()` prepares and copies only the tangent array it returns,
instead of invoking the entire Emval mesh builder and copying every mesh array
just to retrieve tangents. Pointer descriptors still refer to the original
mesh streams; tangents retain their existing material-group ordering and owned
Float32Array contract. This is preparation for typed mesh records, not a newly
migrated method: the inventory remains 196 typed-C and 12 Embind methods.

Complete pre-change mesh snapshots are checked in at
`web/js/tests/fixtures/combined-mesh-queries.json` for both pointer widths.
They cover six sources, original/expanded mesh streams, skinning, subsets
(including partial/overlapping groups), display color/opacity, multiple UV sets,
packed tangents and descriptor contents. Pointer addresses are replaced by the
pointed-to typed values, while dtype, lengths, component counts and byte sizes
remain checked. Independent copies, reset lifetime, invalid IDs and fractional
index conversion are also covered. Both pre-change products pass the fixture.

The snapshots deliberately use eager tangent generation. Initial deferred
probing of `c-core-mesh-queries.usda` exposed a pre-existing unsafe path:
`ComputeDeferredTangents()` interprets quantized normals as `vec3` floats, and
the legacy Vec3 tangent-to-float4 decoder similarly treats Char3/Short3 normals
as float pointers. Expanded tangent and original normal counts may also differ.
The observed wasm32 tangent components depended on surrounding heap contents.
This is an open backend correctness/memory-safety issue, not a stable behavior
to preserve or a passing parity gate. Fix decoding and attribute indexing with
native and WASM regression coverage before claiming final mesh/backend parity.
The shared-helper extraction preserves the existing arithmetic and does not
claim to fix this deferred path.

Both combined C++17 builds pass with isolated `artifacts-mesh-prepare` outputs.
The final memory64 fixture and all 24 wasm32 Node suites pass, including the
complete eager mesh snapshots. Legacy C++20 coroutine-binding syntax and
whitespace checks pass. The packaged-module OffscreenCanvas sweep is still
live and advancing; neither it nor the eager snapshots close the deferred
normal-decoding issue or final next-only/product gates.

The deferred-normal issue identified above is now fixed in the shared native
pipeline. Checked normal reads support float3, SNorm8, SNorm16 and packed
1010102, respect stride/element width and reject short buffers or invalid
indices. Deferred computation expands normal and UV inputs independently by
their interpolation/indexing, instead of assuming that matching counts imply
matching vertex order. Tangent quantization and the legacy web float4 decoder
use the same normal reader. Deferred packed storage is now honored even when
normals were already quantized. Tangent reordering respects face-corner versus
vertex storage, and reset clears the tangent cache; a mesh without tangents
cannot inherit an old mesh's cached tangent stream.

Native regressions cover all four normal formats and all four tangent
algorithms, mixed vertex-normal/face-varying-UV input with a rotated UV seam,
equal-count index remapping, indexed normals, short buffers and invalid stride
or indices. The full native unit suite passes (88.91 seconds), and the focused
regression passes after the final test-only warning cleanup. The WASM regression
repeats deferred loading/computation/reset and compares all three mesh accessors
against eager generation, including numeric stability and owned-array lifetime.
This closes the reproduced unsafe normal decoding and stale tangent-cache
cases, not the remaining typed mesh boundary, allocation-budget or full backend
parity requirements. The inventory remains 196 typed-C and 12 Embind methods.

Both combined C++17 builds pass with isolated `artifacts-normals` outputs.
The final memory64 fixture and all 24 wasm32 Node suites pass, including the
unchanged eager snapshots and the new repeated deferred regression. The legacy
C++20 coroutine-binding syntax check and whitespace checks pass. No new typed
method migration or final product/default switch is claimed in this safety fix.

`getMeshPtr()` now crosses typed C through `mesh_pointer_begin/attribute/
submesh/end`. The 64-byte metadata record, 40-byte attribute spans and 16-byte
submesh records preserve descriptor dtypes, scalar/component counts, byte
lengths, UV slots, flags, display metadata and contiguous material runs. The
small result owns record vectors; its data spans borrow the loader's scene and
decoded caches. JS retains the loader through copying, returns borrowed pointer
descriptors and copies the float4 tangent stream. UV slot zero and the `uv0`
alias remain distinct descriptor objects. Packed-normal caches are refreshed on
every descriptor query so equal-sized data from a later load cannot reuse an
old decoded normal stream.

The legacy adapter consumes the same native records. Combined builds exclude
its Emval descriptor builder and registration. The existing complete mesh
snapshots and deferred-normal fixture exercise this boundary without changing
their expected payloads. Additional C/JS checks cover invalid/short records,
missing indices, invalid arguments, span rejection with cleanup, one-shot
exceptions and deleting the original receiver during record reads. This raises
the inventory to 197 typed-C methods and 11 Embind methods: two mesh payload
accessors, two material queries and seven schema/image utilities. Next-only
backend parity, allocation-budget work and final product gates remain open.

Both combined C++17 builds pass with isolated `artifacts-mesh-ptr` outputs.
The final memory64 fixture and all 24 wasm32 Node suites pass with the unchanged
mesh snapshots and deferred-normal regression. Strict C11 header checking,
legacy C++20 coroutine-binding syntax and whitespace checks pass. The
packaged-module OffscreenCanvas sweep remains live; final browser/next-only
product parity and the remaining boundary migrations remain open.

`getMesh()` and `getMeshCopy()` now use `mesh_value_begin`, the shared typed
attribute/submesh readers, and an 80-byte metadata record. A native producer
builds final spans for points, indices, normals, UV sets, colors/opacity,
tangents, skinning, bind matrices and area-light data. Material-group expansion
replaces spans before crossing the boundary, avoiding the former intermediate
Emval arrays that were immediately overwritten. The legacy adapter uses the
same producer. Combined builds exclude both Emval entry points and their
registrations. All three mesh accessors now use typed C; the inventory is
199 typed-C methods and nine Embind methods (two material queries and seven
schema/image utilities).

The JS adapter copies all arrays for `getMeshCopy()` and creates heap views for
`getMesh()`, after string copies and native record reads have completed. Native
per-loader warning state preserves the deprecated method's once-only warning
across reset and clones, including a throwing console callback. C-result
metadata is released on success, bad spans and exceptions. The original loader
must remain alive for returned borrowed views; retained copies survive loader
destruction.

The mesh snapshots retain their original pre-migration bytes. Only their test
expectations for memory64 UV `vertexCount` are adjusted: the old size_t-to-Emval
path returned BigInts, while both adapters now return numeric
counts. This is an explicit bug fix, analogous to the bone-texture dimension
fix. Additional tests cover short/null metadata, signed joint indices, double
bind matrices, aliasing versus independent copies, warning exceptions, stale
receivers and cleanup when attribute reads fail. These boundary changes do not
close the known area-light backend gaps, general allocation-budget work or
next-only product parity.

Both combined C++17 builds pass with isolated `artifacts-mesh-values` outputs.
The final memory64 fixture passes after correcting the test's UV-count
expectation to ordinary BigInt-to-number conversion; the original UV baseline
contains `3n`, not the high-bit value observed for bone dimensions. All 24
wasm32 Node suites pass. Strict C11 header checking, legacy C++20
coroutine-binding syntax and whitespace checks pass.

The older packaged-module OffscreenCanvas sweep has now finished: 64/67 models
passed. Three public Menagerie models (`iit_softfoot`, `ms_human_700`, and
`robot_soccer_kit`) timed out at 180 seconds. The softfoot failure screenshot
shows the worker still in the parsing stage, with no recorded console error.
A focused retry of those three models is running with a 600-second limit to
distinguish slow work from a persistent failure. The original default-timeout
gate remains failed, and this packaged-module run is not final validation of
the isolated typed-boundary artifacts.

`getMaterial()` and `getMaterialWithFormat()` now cross typed C through
`material_get`. A 240-byte record carries PreviewSurface values, workflow flags
and texture IDs; counted string-table entries carry MaterialX configuration,
errors and serialized output. The legacy adapter shares the native producer,
while combined builds exclude both Emval getters and their registrations.
The inventory is now 201 typed-C methods and seven Embind schema/image
utilities. This completes the combined render-query boundary migration, not
next-only render backend parity.

Pre-migration snapshots on both WASM widths preserve JSON/XML byte hashes and
complete legacy payloads across textured PreviewSurface, MaterialX configuration
and OpenPBR fixtures. Tests cover both workflows, all property/texture slots,
empty-format legacy output, error precedence, owned arrays, null/short records,
invalid arguments, one-shot exceptions and retained loader ownership. The
custom fixture uses the schema's string source URI and float4 texture fallback.

Both combined C++17 builds pass with isolated `artifacts-materials` outputs.
The final wasm32 and memory64 fixtures and all 24 wasm32 Node suites pass.
Strict C11 header checking, legacy C++20 coroutine-binding syntax, JS syntax
and whitespace checks pass. General allocation budgets, next-only backend
parity and final native/Python/browser product gates remain open.

The extended packaged-module browser retry also failed: all three previously
timed-out models still time out at 600 seconds. The browser gate remains open;
increasing the timeout did not resolve these failures.

The six remaining schema utilities now use typed C: `createSampleScene`,
`createURDFPhysicsScene`, `extractPhysicsSceneJSON`, `clearURDFMeshBuffers`,
`setVisualMesh` and `setCollisionMesh`. Counted text inputs and explicit float/
int32 spans replace their Embind boundary. Legacy mesh upload and typed C share
the same native validation/storage function. Combined builds exclude the old
mesh Emval adapters and all six registrations. The method inventory now has
207 typed-C entries; only `encodeImageNative` remains on Embind.

Mesh uploads snapshot all input bytes before WASM allocations, preserve raw
32-bit interpretation (including Uint32 index storage), and retain the loader
through dispatch. Native uploads copy arrays into the shared visual/collision
registry, reject invalid pointer/count/alignment combinations, and preserve
the previous entry after invalid replacement. Null optional arrays, malformed
mesh diagnostics and empty-name precedence remain compatible. Schema calls
select the pointer ABI before mutation and never replay a thrown TypeError.

The new per-width `combined-schema-utilities.json` baseline records exact USDA
and physics-JSON hashes, return values and diagnostics from the previous
artifact. It covers sample authoring, malformed URDF, uploaded visual/collision
meshes, caller buffer mutation and clearing the registry. Boundary tests add
heap-backed uploads, failed replacement, invalid spans/arguments, one-shot
exceptions and deleting the original receiver during extraction or upload.
This migrates the combined boundary; the native schema implementation still
uses the legacy stage and does not establish next-only authoring parity.

Both combined C++17 schema builds pass with isolated `artifacts-schema`
outputs. The memory64 fixture and all 24 wasm32 Node suites pass, including
the schema snapshots and boundary regressions. Strict C11, legacy C++20
coroutine-binding syntax, JS syntax and whitespace checks pass.

`encodeImageNative()` now uses a counted pixel/format C call and a 24-byte
output record. A shared native encoder returns the original borrowed byte
span, null on encoder/format failure, or the separate invalid-dimensions
result without changing loader diagnostics. JS validates the output span and
retains the loader through dispatch. Returned views still require the loader
and its current image-export buffer to remain alive. Combined builds exclude
the old Emval encoder and registration; all 208 inventoried loader methods
now use typed C, with no remaining Embind method entries in that inventory.

Pre-migration snapshots compare encoded bytes for PNG, BMP, TIFF, DNG and EXR,
one through four channels, unsupported/case-sensitive formats, invalid
dimensions, short input and excess input bytes on both pointer widths.
Boundary checks cover null/short records, invalid arguments, borrowed-span
aliasing, bad output ranges, exceptions without replay, reset-safe explicit
copies and deletion of the original receiver during dispatch. These changes
close this combined method-boundary inventory, not the full refactor's
next-only backend, allocation, consumer or final product gates.

Both combined C++17 builds pass with isolated `artifacts-imageencode` outputs.
The complete memory64 combined fixture and all 24 wasm32 Node suites pass,
including the live 208-method inventory and both new utility baselines.
Strict C11 header, legacy C++20 coroutine-binding syntax, JS syntax and
whitespace checks pass. Packaged browser failures and the remaining full
refactor gates described above are still open.

The `tydra_to_renderscene --next` consumer now loads, converts, queries memory
and reads diagnostics through the installed C++ facade over C/POD handles.
It no longer imports internal next loader or Tydra converter headers, and
links the public render C target. Native instancing, payload/thread options,
time code, triangulation, tangent selection, animation suppression, low-memory
geometry handling and filesystem texture resolution keep their prior mappings.
Its explicit legacy path and the previously unsupported next dump exporters
remain unchanged; this is not a claim that those exporters have next parity.

Six render controls now occupy previously reserved configuration bytes:
triangulation/tangent method, geometry/instance-source discard preferences,
animation suppression and the default filesystem resolver. Zero preserves
the prior C defaults; the 64-byte v4 layout and existing field offsets are
unchanged. One-shot conversion owns its resolver for the operation; persistent
sessions own it through repeated updates. Instancer source arrays remain
available whenever draw expansion needs them, despite a discard preference.

A saved pre-change command and the migrated command match scene counts,
diagnostics and exit status in 72 combinations across mesh, animation,
instancer, native-instance, texture and payload fixtures. Timing and memory
accounting are excluded from that comparison: the C scene memory query also
accounts for its auxiliary storage (24 extra bytes in the small mesh case).
Native C tests exercise fan indices, all tangent selections, animation
suppression, discarded geometry, required instancer arrays, resolver lifetime,
invalid controls and unchanged configuration layout. Full next-product and
standalone Debug CTest gates pass (45 and 44 registered tests respectively,
one optional AOUSD corpus skip each). No whole-product performance improvement
or final default switch is claimed.

The new `tydra-next-c-api-smoke` passes in the native Ninja tree. Strict C11
header and whitespace checks pass. A current direct-include audit of `.cc` and
`.hh` files under `examples` and `web` finds 30 files still importing
`next/` or `tydra/next/` internals; those remain consumer/binding work, alongside
the backend and product gates above.

The viewer's incremental upload planner now consumes public
`lightusd_render_change_set`/`lightusd_prim_change` POD records and an explicit
new revision. It no longer includes next's change-set header or calls its
Path/flag helpers. The existing next-session call site borrows immutable path
and property strings into temporary C records; compile-time assertions guard
the flag mapping. This adapter remains at the viewer call site until its
session ownership is migrated. Property classification reads counted string
views, including non-terminated names, and invalid record sizes/null spans
are rejected before scene processing.

The planner test also uses only public records and no longer links
`lightusd_next`. Existing slot/remap/material/texture tests and new malformed
record, revision, and bounded-property cases pass. The complete viewer builds,
and all five Vulkan/OpenGL incremental topology, layer-reload and texture
regressions pass under Xvfb (11.72 seconds). The elevated environment exposes
NVIDIA RTX 5060 Ti and llvmpipe; no forced Vulkan device override was used.
Whitespace checks pass. This removes two direct internal-header consumers;
the viewer's native next session and snapshot ownership remain to migrate.

Document snapshots now expose their complete change records through
`lightusd_document_snapshot_changes_copy`. A fixed-width metadata record
reports base/new revision, full-resync/stage-metadata flags and required
prim/property counts. A sizing query precedes the copy into caller-owned
arrays. Successful calls allocate no native storage; paths and counted
property text borrow the immutable snapshot. Insufficient capacities return
the required counts without partially overwriting either array. The shared
prim-change and flag declarations now live in the core session header, still
included by the render header, with unchanged names and layouts.

This closes a prerequisite for migrating viewer session ownership: the old
snapshot enumeration omitted property names, so it could not preserve the
planner's distinction between texture-only and material changes. The C++
facade forwards the complete export, and the interactive-session workflow
prints those records and verifies the changed `/World.level` property after a
variant edit. Native C tests cover metadata/property edits, empty change sets,
sizing/short capacities, invalid arguments, unchanged arrays on failure, and
borrowed strings retained across later publication and session destruction.
The viewer session itself still uses native next ownership; this export does
not declare that consumer migration complete.

Both native next builds and full CTest suites pass: 45 registered next-product
tests and 44 standalone Debug tests, with the optional AOUSD corpus test
skipped in each. The interactive workflow and document tests pass, the shared
library exports the new symbol, and the complete viewer rebuild and isolated
planner regression pass. Strict C11 header and whitespace checks pass.

The public document session now supports uncomposed loading and source-layer-only
cache retention through formerly reserved option bytes; zero preserves the
existing defaults and structure offsets. A fixed-width memory query exposes
retained source, transient cache, composed stage and peak estimates. These
estimates exclude snapshots retained separately by callers.

`lightusd_document_session_take_stage` transfers the published stage to an
independent C owner without cloning it, preserving source anchors and warnings.
It refuses transfer while the current stage is retained by a snapshot. Successful
transfer closes the session; snapshot requests then return NOT_FOUND, and the
session can be reopened without affecting the transferred stage. The C++ facade
and interactive workflow exercise this ownership transition. These operations
close further prerequisites for viewer/quicklook migration; those consumers'
native session ownership remains outstanding.

Both full next CTest suites pass (45 product and 44 standalone Debug tests,
one optional AOUSD corpus skip each). Focused C tests verify default/layout
compatibility, invalid controls, reference composition versus uncomposed loading,
reduced transient storage with source layers retained, memory query validation,
retained-snapshot refusal, closed-session behavior and independent stage lifetime.

Document sessions now expose batch variant replacement and payload loading through
`lightusd_document_session_set_variants` and
`lightusd_document_session_load_payloads`. Each valid batch invokes one native
recomposition instead of publishing intermediate single-edit stages. Variant
replacement drops overrides omitted from the new batch; an empty batch restores
authored selections. The boundary validates paths, empty names, duplicate
prim/set pairs and overflowing counts before editing. The C++ interactive
workflow now uses these batch operations.

The new two-sibling regression exposed and fixed a native PCP invalidation bug:
`IsPathAtOrUnder` required an extra separator after the pseudo-root, so a batch
whose common ancestor was `/` could retain stale descendant composition data.
Root invalidation now matches absolute descendants. C tests verify both payloads
appear in one revision, cancelled batches restore override/load-rule state on a
later rebuild, rejected batches preserve publication, and retained snapshots
remain unchanged. Both full next suites pass (45 product and 44 standalone Debug
tests, with one optional AOUSD corpus skip each). This advances the document
boundary; viewer session ownership and final backend/product parity remain open.

The viewer rebuild, isolated upload-planner test, and all five incremental
OpenGL/Vulkan topology, layer-reload and texture tests pass after the cache fix.
Strict C11 header and whitespace checks also pass.

The public C/C++ document boundary now exposes open/composed state, counted
dependency-path copies, transient cache trimming and full composition-cache
release. Release preserves the published stage/revision and dependency paths;
subsequent edits reconstruct the cache with its prior payload rules and variant
overrides. Native cache-control methods now return operation status so reentrant
calls can report BUSY instead of silently claiming a release occurred. Closed
C sessions return NOT_FOUND. The interactive workflow enumerates dependencies,
releases its cache with retained snapshots, and then reloads through the facade.

Focused C tests cover short dependency buffers, invalid indices/arguments,
dependencies retained after release, memory accounting, unchanged publication,
cache restoration, repeated release, and reentrant cache calls during progress.
This supplies the viewer's cache/dependency operations without exposing native
session types; migrating viewer ownership still requires its remaining preview,
geometry-release and snapshot query paths.

Both full next suites pass (45 product and 44 standalone Debug tests, one optional
AOUSD corpus skip each), as do the viewer build, isolated upload-planner test,
strict C11 header and whitespace checks. The shared library exports all six new
state, dependency and cache-control symbols.

The viewer's capacity-based budget and texture-fit planning now uses public
render C records and C++ facade calls. `main.cc` no longer imports the internal
resource-budget header, and `LoadOptions::textureFit` carries a public POD record
instead of a Tydra type. Loader policy logging also uses the C helpers. The
viewer links `lightusd_render_c`; its remaining scene/session implementation
still links next and Tydra directly.

The boundary delegates to the existing budget/policy calculations, preserving
host reserves, VRAM thresholds, texture quality caps, byte-suffix parsing and
overflow handling. C tests check all output budget fields for the ordinary
32/16-GiB case, zero/small/extreme capacities, all quality and texture-fit modes,
numeric suffixes, malformed/overflowing input, record sizes, and unchanged
outputs on failure. Both full next suites pass (45 product and 44 standalone
Debug tests, one optional AOUSD corpus skip each). The viewer builds and its
Vulkan texture-fit regression passes, verifying resize/compression decisions
through the migrated path. Strict C11 and whitespace checks pass. No whole-product
compile-time improvement or final default switch is claimed by this change.

`lightusd_document_snapshot_stage` now returns an independently retained,
read-only C stage backed by the immutable document snapshot, without cloning
scene data. Existing stage traversal, metadata, value/animation queries,
dependencies, serialization and render conversion read the retained backing
stage. Source anchors and warnings are preserved. Authoring is rejected at the
shared mutable-prim/root entry points; callers can explicitly flatten to obtain
an editable stage. The C++ facade exposes the retained view and read-only query.

Views survive session edits/destruction and keep prim handles alive while their
C owner is retained. They also block destructive session stage transfer until
released. Render-session update/prepare now retains these immutable inputs
directly, while still cloning mutable input stages. The interactive workflow
queries a snapshot through the ordinary C++ Stage/Prim facade. This supplies
snapshot consumers with the existing public query/export surface instead of
requiring a duplicate family of snapshot-specific getters.

C tests exercise mutation rejection, editable flattening, USDC export/reload,
retained-handle lifetime across variant edits/session destruction, transfer
refusal, direct render conversion and render-session updates from snapshot
views. Viewer preview callbacks and geometry-release migration remain open.

The final next-product and standalone Debug suites pass (45/44 registered tests,
one optional AOUSD corpus skip each), including existing mutable-stage authoring
coverage. Shared-library exports, strict C11 header and whitespace checks pass.
The viewer also builds against the new stage-handle ownership representation,
and its isolated incremental planner regression passes.

Document sessions now deliver authored-root and composed-spatial previews through
a synchronous public C callback. Each event contains a borrowed read-only C stage,
provisional revision, phase and completeness flags. Callers retain the stage to
keep it after the callback; source anchors remain available to public query and
export operations. Returning zero cancels loading without publishing a new
document. Uncomposed loads emit only the authored-root preview. Registration
does not change the document-options record layout.

The interactive C++ workflow registers this callback and retains both preview
stages. C tests cover read-only access, spatial omission of non-spatial values,
cancellation at either phase preserving the current revision, root reload,
disabled delivery, uncomposed loads, and preview lifetime after session destruction.
Retained provisional stages do not block transfer of the separately published
stage. Viewer callback migration and geometry release remain unfinished.

Both full next suites pass (45 product and 44 standalone Debug tests, one
optional AOUSD corpus skip each), including the preview-enabled workflow.
The viewer rebuild and isolated incremental planner regression pass, along with
shared-symbol, strict C11 and whitespace checks.

The public document API now releases static geometry defaults for one prim path
or the whole composed stage, returning fixed-width property/element/byte counts.
Retained snapshots keep their arrays, declarations and non-geometry/animated
properties remain available, and rebuilding restores authored geometry. Invalid
paths, closed/uncomposed sessions and reentrant mutation return explicit errors;
failed calls leave statistics untouched. Per-prim release omits full-stage byte
scans, preserving the streaming converter's cost model.

The native per-prim operation previously passed an old layer's prim handle into
the stage after copy-on-write, causing release to silently do nothing whenever
a snapshot was retained. It now resolves by path after preserving the snapshot.
Both native and C regressions verify this case, repeated release and restoration;
the C++ workflow compacts consumed static geometry before transferring its stage.
This closes the public geometry-release gap; viewer session ownership migration
and the broader backend/default-product gates remain outstanding.

Both full next suites pass (45 product and 44 standalone Debug registered tests,
one optional AOUSD corpus skip each). The viewer rebuild, isolated planner,
VRAM-budget regression and all five incremental GL/Vulkan regressions pass.
Shared-symbol, strict C11 and whitespace checks pass. Workspace disk exhaustion
during the Debug archive build was resolved by preserving that generated build
under `/tmp/lightusd-c-core-next-debug`, with `build_ninja/next_debug` remaining
the entry point through a symlink; no source or build artifacts were discarded.

The next-backed facial-control reader now consumes public C stage, traversal and
dictionary queries. It no longer imports native next Stage/Value headers. A new
dictionary string-family-array copy operation supplies owned control names and
mappings; the C++ DictionaryView facade forwards it. Defaults and ranges use
typed numeric views. Malformed ranges are rejected before reading pairs, fixing
the old reinterpretation of a scalar array as float2 data.

The viewer and MCP call sites still own native snapshots. A private, non-installed
adapter retains those snapshots as read-only C handles without cloning so this
leaf consumer can migrate now. This adapter is temporary and must be removed
when viewer session/snapshot ownership moves to the public document API; it is
not a new supported C++ interchange boundary. The legacy reader remains separate.

The new headless viewer unit checks traversal order, short mappings/ranges/defaults,
range normalization, malformed types, missing data, owned dictionary-list lifetime
and shared snapshot retention. Both full next suites pass (45 product and 44
standalone Debug registered tests, one optional AOUSD corpus skip each), including
the portable DictionaryView test. The complete viewer builds, and strict C11,
shared-symbol and whitespace checks pass.

Both viewer blendshape regressions (`lusdview-noninstanced-blendshape` and
`lusdview-blendshape-morph`) pass under Xvfb with the rebuilt viewer (40.63 seconds).

The viewer preview cache now owns public C++ Stage handles and reads/writes USDC
through the public C facade. The cache implementation and its unit test no longer
include native next headers. Explicit USDC selection preserves format validation
even for temporary files. Background writers retain immutable preview handles
without cloning scene data; manifest publication and invalidation are unchanged.
The still-native loader uses temporary private adapters to retain generated
snapshots and borrow a cached stage during preview conversion. These adapters
must disappear with the remaining viewer session/converter migration.

Cache tests cover snapshot-owner destruction before serialization, extent
roundtrips, retained hits after replacement, truncated and wrong-format payloads,
failed writes without manifest publication, and existing invalidation behavior.
Both full next suites pass (45 product and 44 standalone Debug registered tests,
one optional AOUSD corpus skip each). The viewer builds and its cache and facial
control tests pass. An interactive Vulkan run under Xvfb verifies a cold cache
store, then a warm hit and bounds-preview upload before mesh upload. This probe
uses `--no-threaded`: the default interactive Vulkan render-thread path bypasses
progressive streaming and therefore cannot validate the composition preview cache.

Public document sessions now expose payload selection and selection-notification
callbacks, with a C++ facade method. This supplies the viewer's whitelist and
payload-progress requirements without native composition callbacks. Returning
zero defers a payload; explicit load/unload rules still take precedence. The
notification reports a decision to load, not successful asset I/O. Borrowed
path strings and potentially concurrent callback delivery are documented.

Replacing or clearing callbacks drops cached composition decisions while keeping
source layers, published snapshots and explicit load rules. Otherwise a rebuild
could silently reuse the old policy. Callback adapters remain installed across
cache restoration and root reload, so replacement does not leave stale userdata
captured in native options. Reentrant replacement returns BUSY. Tests cover both
default load policies, selective composition, callback replacement/clearing,
explicit-rule precedence, cache restoration, root reload and retained snapshots.
The interactive C++ workflow records payload selections with an atomic counter.
Viewer ownership migration still requires resolver/tuning options and initial
variant configuration before its main session can move to the public boundary.

Both full next suites pass (45 product and 44 standalone Debug registered tests,
one optional AOUSD corpus skip each). The viewer build and both migrated leaf
tests pass. The new callback symbol is exported by the shared library; strict
C11 header and whitespace checks pass.

Document sessions now have a separate pre-open configuration record for initial
variant overrides, composition opinion batch size/timing, input policy and
parent-path restrictions. Initial variants are copied and applied during the
first composition, avoiding a second publication with different content. The
authored-root preview remains authored; composed spatial previews use the
configured overrides. Batch edits and initial configuration share validation.
Configuration is rejected while open or during an operation, and can be replaced
after destructive stage transfer closes the session.

The untrusted-input default remains intact: native normalization rejects
absolute/parent asset paths regardless of permissive resolver flags. Trusted
input is an explicit choice using the existing public input-policy enum, and
can still reject parent paths. Tests exercise actual referenced-layer loading
under all three policies, including cache restoration. Initial-variant tests
cover copied strings, invalid/duplicate/overflowing replacements preserving the
previous configuration, root reload, and clearing after close. The C++ workflow
uses the facade's pre-open configuration method. These controls close the
identified initial-configuration gaps; the viewer's native ownership and
converter dependencies still need migration.

Validation: full next product and standalone Debug suites pass (45/44 registered
tests, one optional AOUSD skip each), as do the viewer build and two migrated leaf
tests. Both new configuration symbols are present in the shared library, and
strict C11 header compilation and whitespace checks pass.

The viewer now owns retained render sessions and prepared transactions through
the public C++ facade. Its application header/source no longer name native
RenderSession, PreparedRenderUpdate or SceneUpdateSink types. A public C event
callback performs the GPU transaction at End, before render publication; the
callback is cleared after synchronous commit so stack userdata cannot escape.
Bootstrap commits use the accepting default sink. RAII aborts uncommitted work.

A temporary private document-snapshot adapter retains the native stage and
copies its revision/change records for the still-native document owner. The
public document prepare operation therefore preserves actual document revisions,
including gaps, instead of generating a separate render revision sequence.
The adapter uses an anonymous source anchor, matching the prior default native
render-session configuration, and must be removed when document ownership moves
to the public API. Tests check shared ownership, retained-view lifetime and
revision/change preservation. Native document sessions and converter access are
still outstanding; this change migrates render transaction ownership only.

Validation: both full next suites pass (45 product and 44 standalone Debug
registered tests, one optional AOUSD skip each), the viewer and bridge/planner
unit targets build/pass, and all five GL/Vulkan incremental regressions pass
under Xvfb (10.37 seconds). Those runtime tests require prepared revision 2 and
reject commit-failure fallback. Whitespace checks pass.

The viewer's main persistent document is now owned by the public C++
DocumentSession facade through `ViewerDocument`. Open, payload/variant edits,
layer reload, cache retirement, memory statistics and geometry release all cross
the public C boundary. The application and loader no longer hold a native
StageSession for this document. A temporary viewer-local adapter still accepts
the existing native option shape and returns native read-only stage/change views
for converters and UI queries; these views alias public Stage owners without
cloning and survive document destruction. This adapter is not an installed API.

Progress, preview and payload callbacks translate public POD events at that
boundary. Per-load callbacks are cleared on every exit from the loader so later
recomposition cannot invoke closures referring to an earlier load's locals.
The owning document stays at a stable address. Initial variant overrides,
root-reload reset semantics, explicit payload rules and cancellation retain
their previous behavior. A new viewer document test covers these operations,
geometry release with a retained view, cache retirement, callback cleanup and
snapshot lifetime after owner destruction.

Owned bulk dependency and deferred-payload list queries avoid repeatedly copying
the entire native list for each indexed string request. The viewer uses these
queries, and C tests verify contents, error outputs and lifetime across edits
and session destruction. Converter/UI native query shapes, the one-shot native
value-clip loader, quicklook ownership, and the broader parity/default gates
remain outstanding. Main document ownership migration does not complete those
remaining consumer migrations.

Validation: the new viewer document test and both full next suites pass (45
product/44 standalone Debug registered tests, one optional AOUSD skip each).
The final viewer build passes all eight selected GPU regressions: five GL/Vulkan
incremental tests, VRAM budgeting and both blendshape tests (50.11 seconds).
An interactive non-threaded Vulkan probe stores a cold preview cache and loads
and uploads the cached bounds preview on the warm run. Both bulk-list symbols
are exported by the shared library; strict C11 and whitespace checks pass.

Viewer render preparation now takes the public document snapshot directly.
The private `RetainDocumentSnapshot` adapter and its exported implementation
have been removed. A public prepare-with-changes operation accepts aggregated
POD changes spanning several document edits while obtaining the target revision
and source directory from the immutable snapshot. This preserves revision gaps
and source anchors without converting a public document back through a native
snapshot. The C++ facade owns the resulting prepared transaction.

The application shares one temporary native-change-to-POD view between render
preparation and the incremental GPU planner; only that change-record adapter
remains at this boundary. Change decoding now rejects overflowing prim/property
counts before vector allocation. C tests exercise skipped revisions, explicit
full resync, abort without publication, commit at the snapshot revision and
invalid/overflowing change records. Native stage query adapters elsewhere in
the viewer and the remaining consumer/parity gates are still unfinished.

Validation: both full next suites pass (45 product/44 standalone Debug registered
tests, one optional AOUSD skip each), along with the viewer build and three
focused viewer unit tests. All five incremental GL/Vulkan regressions pass under
Xvfb (10.52 seconds). The new public preparation symbol is exported and the
removed private document-adapter symbol is absent. Strict C11 and whitespace
checks pass.

The viewer render/UI thread now retains its published stage through the public
C++ Stage owner rather than a native shared Stage pointer. Snapshot publication
uses the public document snapshot/revision queries directly. Remaining native
camera, animation, GUI and tracer functions borrow the stage only while this
public owner stays alive; their query interfaces still need migration. Facial
control queries in both the application and MCP now consume public handles
directly, removing their native-to-C snapshot retention round trips.

Validation: the viewer builds, five focused viewer unit tests pass, and all
eleven selected runtime regressions pass under Xvfb (62.43 seconds): five
incremental GL/Vulkan cases, VRAM budgeting, two blendshape cases and camera
motion on CPU, Vulkan and OpenGL raster paths. Whitespace checks pass. This
consumer-only change does not alter the public core ABI or switch defaults.

Viewer timeline metadata and MCP stage-info, prim-list, type-filter and search
queries now use public stage/prim/value handles. Shared viewer query helpers
copy metadata into owned results and traverse in authored depth-first order,
including inactive prims. Traversal uses one frame per ancestor and a false
visitor result stops the whole walk, preserving capped list behavior without
retaining every sibling handle. Attribute/schema MCP queries and native render
converters remain separate migration work.

A dedicated public-stage-query unit covers authored metadata, unchanged outputs
on failure, nested/root traversal order, inactive prims, empty stages and global
early stopping. The GL/Vulkan dependency-reload regressions now check the live
MCP endpoints for metadata, list caps, subtree selection, type filtering and
search before exercising reload transactions.

Validation: the viewer build and dedicated query unit pass. Seven runtime
regressions pass (99.48 seconds), covering blendshapes, skeletal animation,
CPU/Vulkan/OpenGL camera motion and both live MCP/reload cases. After limiting
public snapshot creation to the migrated MCP endpoints, the final build and
both MCP/reload regressions pass again (4.48 seconds). Whitespace checks pass.

The next-backed MCP library-tool branch now uses public C handles throughout,
including prim summaries, attribute lists/default summaries and variant queries.
Resolved property-name lists include inherited and schema-defined properties;
the existing indexed property queries retain authored-slot semantics. Default
inspection preserves schema fallbacks and value blocks, and reports array type
and count without materializing lazy array data. Returned names own their storage;
scalar/text views borrow the retained stage or schema registry.

The live topology regression checks prim summaries, schema fallback values and
array summaries before exercising variant topology and material edits. Composed
snapshots intentionally have no variant definitions after resolution, so MCP
variant enumeration remains empty as before; edit success is verified through
published geometry changes. The C suite separately covers authored variant data.

Validation: viewer build and both full next suites pass (45 product/44 standalone
Debug registered tests, one optional AOUSD skip each). The strengthened C test
also passes with always-active checks. All five incremental GL/Vulkan regressions
pass under Xvfb (10.82 seconds). Both new query symbols are exported; strict C11
and whitespace checks pass. Native queries elsewhere in MCP, viewer camera/UI
and converters, quicklook, and the broader parity/default gates remain pending.

Viewer document payload queries and requests now use owned strings instead of
native Path objects; memory reporting returns the public C stats record directly.
MCP no longer names native next types or includes native next headers. Its
transitive document/application dependencies still contain native options,
change records and converter queries, so this does not complete their migration.
Loader proxy generation, recursive payload expansion and GUI payload lists use
the string interface. The document unit checks exact deferred paths, retained
list contents after loading and the public memory-record layout. The live USDZ
payload regression checks the reported prim/arc before loading both products.

Validation: viewer and document-test builds pass, the document unit passes, and
all six selected runtime regressions pass under Xvfb: USDZ deferred payloads and
five incremental GL/Vulkan cases. Whitespace checks pass.

Viewer geometry release now accepts prim paths and returns public C release
statistics. Loader accumulation uses the C record and preserves 64-bit byte
counters; consumed prototype paths no longer require a native prim lookup before
release. The document's native `GetSnapshot` accessor is removed. The loader
obtains revisions and owning Stage views through public document snapshots, then
borrows the native Stage for the converter's remaining internal queries. Native
preview callback adaptation, options and change records remain outstanding.

The viewer document test now performs stage checks through public handles and
retains a public Stage past document destruction. It verifies geometry release
preserves arrays in an older snapshot, removes defaults from the current
publication, and safely handles repeated release and missing paths. Validation:
viewer build, document unit and all seven selected runtime regressions pass
(16.25 seconds): VRAM budgeting, USDZ deferred payloads and five incremental
GL/Vulkan cases. Whitespace checks pass. Full consumer/parity/default gates
remain open.

Viewer document configuration now uses initialized public C option/open records
and owned variant maps. Progress, preview and payload callbacks receive public
C events/views directly. The adapter no longer includes native StageSession
headers, reconstructs native callback objects, or borrows native stages. Its
remaining native interface is change records. Borrowed preview stages are
explicitly retained by consumers that need them after the callback. The loader
and background cache writer now retain public Stage owners throughout preview
handoff, eliminating their `RetainStageSnapshot` conversion.

The document test retains a public preview beyond document destruction, in
addition to callback cleanup, cancellation, payload and variant coverage.
Viewer build and both document/cache units pass. All seven selected GPU runtime
regressions pass (16.24 seconds): VRAM budgeting, deferred USDZ payloads and the
five incremental GL/Vulkan cases. Native change-record conversion, the value-clip
loader and remaining consumer/parity/default gates are still outstanding.
The isolated interactive Vulkan probe also passes: a renderable composed fixture
stores a cold cache, hits the warm cache and uploads the composition preview on
both runs. The probes are terminated after these checks; they are not full
interactive shutdown tests. Whitespace checks pass.

The viewer document owner no longer depends on native next headers or types.
Owned viewer change records store strings and public C flag bits; the loader
aggregates them directly and render preparation/planning receives borrowed C
records backed by those strings. This removes native StageChangeSet/PrimChange/
Path conversion from the document boundary and the flag-translation assertions
in the renderer adapter. Consecutive edits preserve revision ranges, merge flags
and deduplicate property names; gaps request a full resync.

The document unit covers aggregation, duplicate properties, metadata changes,
revision gaps and ownership after source records are cleared. Its public-only
header compiles in isolation with C++17 and only the C API include directory.
Native converter/UI queries and the value-clip loader remain separate work.
Validation: viewer build and three focused units pass (document, public stage
queries and incremental scene planning). All seven selected GPU regressions pass
under Xvfb (15.92 seconds): VRAM budgeting, USDZ deferred payloads and five
incremental GL/Vulkan cases. Whitespace checks pass. This completes the viewer
document-owner interface migration, not the remaining converter/UI or product
parity/default gates.

Removed the private `RetainStageSnapshot` declaration, implementation and export
now that production consumers retain public document/preview Stage handles.
The facial-control test also uses the public document API, retaining a read-only
view past session destruction. It no longer constructs a native stage or uses
private bridge queries. At this stage of the migration, `BorrowNativeStage`
still served native converter/UI consumers; the viewer loader's final borrow
was removed in the later migration record below.

Validation: the viewer links and three focused units pass (facial controls,
preview cache and document). Both full next suites pass (45 product/44 standalone
Debug registered tests, one optional AOUSD skip each). Source searches and shared
symbol inspection confirm the removed adapter is absent. Whitespace checks pass.

The GUI now retains a public Stage owner instead of borrowing the application's
native Stage pointer. Its scene hierarchy uses public prim handles for root and
child traversal, labels, active state and payload indicators/context actions.
A public `lightusd_prim_has_payload` query reports authored payload arcs in the
current view (composition may consume them); invalid handles return false.
Remaining inspector, payload-panel, stage-metadata and deformation functions
still borrow the native stage while the GUI's public owner stays alive.
The GUI header no longer names native next types. This is not full UI migration.

The C test covers absent/authored payloads and invalid handles. Both full next
suites pass (45 product/44 standalone Debug registered tests, one optional AOUSD
skip each); the new symbol is exported and the C11 header check passes.
Viewer validation: the final build passes all ten selected runtime regressions
under Xvfb (52.05 seconds), covering blendshape morphs, CPU/Vulkan/OpenGL camera
motion, USDZ deferred payloads and five incremental GL/Vulkan cases. A separate
visible-window run draws a nested hierarchy with an inactive prim and selected
mesh for three frames and exits successfully. Whitespace checks pass.

The next inspector now reads prim identity/specifier/active state, shader-ball
material choices and resolved property defaults through public C queries. Its
native Value summary helper is replaced by a public-view formatter; arrays
remain type/count-only, strings/tokens/assets retain their previous display,
and schema defaults/blocks follow resolved inspection semantics. Relationships,
variant/composition metadata and inherited material-binding resolution still
use native queries and remain to be migrated.

A focused query unit covers scalar/string/asset formatting, schema fallback,
blocked defaults and buffer-free array summaries. Validation: viewer build and
both query/document units pass; all five incremental GL/Vulkan regressions pass
(10.38 seconds). A selected-mesh inspector smoke run draws three visible-window
frames under Xvfb and exits successfully. Whitespace checks pass.

The inspector relationship table now uses public relationship-name/target-count
queries. C relationship queries include instance-source relationships when a
local opinion is absent, matching native UsdPrim lookup; local opinions still
win, and targets remain unforwarded paths. The owned name-list query clears its
output on failure. The fixture covers reference target remapping, empty targets,
local overrides, inherited fallback after removing a local opinion, invalid
handles and name-list lifetime after stage destruction.

Validation: viewer build and both full next suites pass (45 product/44 standalone
Debug registered tests, one optional AOUSD skip each). After strengthening the
inherited-fallback assertions, the C unit passes again in both configurations.
Six runtime regressions pass (USDZ payloads and five incremental GL/Vulkan cases),
and a selected-prim visible-window smoke run with a relationship exits normally.
C11 and whitespace checks pass. Composition metadata and material binding in the
inspector, other native consumers, and parity/default gates remain unfinished.

The next prim inspector now uses public queries throughout. Variant controls
retain authored selection keys even for sets without local definitions; the
new owned key-list query supplies these keys and the existing selection query
supplies their values. Public arc counts supply the composition panel. Material
binding preserves the prior helper's first unforwarded relationship-target
behavior; this is not a new full material-binding resolver. The inspector no
longer borrows a native stage or depends on Tydra next scene-access helpers.
Other GUI metadata/payload/deformation borrowers and broader consumer/parity
work remain.

C tests cover all four arc types, unknown types, invalid handles, selections
without local definitions and key-list lifetime after stage destruction.
Both full next suites pass (45 product/44 standalone Debug registered tests,
one optional AOUSD skip each), the viewer builds, both symbols are exported,
and C11/whitespace checks pass. A visible-window root-layer-only smoke run draws
the selected prim with variants, four arc kinds and a material relationship for
three frames and exits successfully; the fixture's shaderless material uses the
expected fallback.
All six selected runtime regressions also pass under Xvfb (13.48 seconds): USDZ
deferred payloads and five incremental GL/Vulkan cases.

The GUI stage-metadata panel now uses public values and the owned sublayer-list
query. Shared stage-info helpers include frame rate, documentation/comment and
authored timing flags. The new authored-metadata query distinguishes absent
values from explicit zero/default/empty opinions, preserving the panel's timing
row visibility without changing ordinary metadata getters. Native GUI stage
queries now remain only in the payload panel and deformation helper; other
consumer and product gates are still outstanding.

C tests cover authored default numbers/empty strings, absent fields, invalid
inputs and metadata setters. Viewer query tests check comments/documentation,
frame rate and explicit zero versus absent time codes. Viewer build, query unit
and both full next suites pass (45 product/44 standalone Debug registered tests,
one optional AOUSD skip each). The new symbol is exported; C11 and whitespace
checks pass.
All eight selected runtime regressions also pass under Xvfb: CPU/Vulkan/OpenGL
camera motion and five incremental GL/Vulkan cases.

The GUI payload panel now obtains stored payload-arc text through the public C
API. It queries only the deferred prim paths being displayed, avoiding the
previous full-stage walk and temporary asset map on each panel draw. The first
payload arc and `(deferred)` fallback preserve existing display behavior.
`lightusd_prim_arc_text` returns borrowed inspection text, not a resolved asset
path; invalid handles/types/indices return an empty view. Tests cover all four
arc kinds and failure cases. The GUI no longer includes native next headers;
its one remaining private stage borrow serves deformation, which still needs
migration with the animation/converter interfaces.
Validation: viewer build, both full next suites (45 product/44 standalone Debug
registered tests, one optional AOUSD skip each), and all six selected runtime
regressions pass. Runtime coverage includes USDZ deferred payloads and five
incremental GL/Vulkan cases. The arc-text symbol is exported; C11 and whitespace
checks pass. Broader consumer/parity/default gates remain open.

The viewer deformation and posed-bounds entry points now take public Stage
handles. GUI and application callers pass their retained C owners directly;
the loader implementation borrows a native stage privately while its existing
bone/morph algorithms run. The GUI has no native next types, headers or private
stage-bridge calls left. This completes that GUI boundary, not native animation
internals: bone/morph evaluation and the other converter interfaces still need
migration, along with the broader consumer/parity/default gates.

Null-stage deformation calls clear the output and return false; posed-bounds
calls reject a null stage without changing bounds. The viewer builds and
whitespace checks pass. Runtime verification follows below.
Runtime validation: blendshape morph, skin-animation and combined morph-skin
regressions pass. RT-skinning skips for a cold Vulkan hardware RT pipeline
cache; instanced-prototype RT skips because its backend probe produces no image.
Those two hardware paths are not verified by this run. The five-test selection completes in 110.06 seconds with no failures.

Per-frame GPU bone-matrix and morph-coefficient entry points now also accept
public Stage handles. Application callers pass their retained owner directly;
RT deformation forwards the same handle when requesting morph coefficients.
The implementation still evaluates native skeleton/morph data privately in the
loader, so this is an interface migration, not completion of native animation
internals. Morph queries reject null outputs and clear output before rejecting
a null stage; skinning rejects a null stage without changing the caller's frame.
Camera/world-update interfaces and remaining consumer/parity gates stay open.
Animated mesh-world updates now perform prim lookup and world-transform
sampling directly through the public C API. Their bounds/picking math is
unchanged, and application callers no longer borrow a native stage for these
updates. This path removes native queries rather than wrapping them; the public
transform API uses the shared core evaluator already covered by C tests.
Validation: the bone/morph interface build passes blendshape morph,
skin-animation and combined morph-skin regressions (114.43 seconds). After the
additional direct world-query migration, the final viewer build passes all five
selected camera/reload regressions: CPU/Vulkan/OpenGL camera motion and both
GL/Vulkan layer reloads. Whitespace checks pass. Native animation internals,
camera interfaces and the broader consolidation gates remain unfinished.

Viewer camera lookup and shutter sampling now take retained public Stage
handles. Camera traversal, identity, scalar defaults, and world transforms use
public C queries; clipping-plane arrays use default-time evaluation without
connection following and copy every stored float component. First-match
authored traversal, name/path/suffix matching, lens fallback values, clipping
clamps, and numeric-time transform sampling remain unchanged. Null stage/output
lookup returns false. Checkpoint previews pass their public owner to camera
lookup while retaining private native bounds extraction.

The application translation unit no longer includes native next headers or
borrows native stages. The loader header likewise exposes no native next Stage
types, including its camera-gathering entry point. Gathering/backplate
conversion and bone/morph algorithms still use native data inside the loader;
this does not complete the remaining consumer, parity, or default-switch gates.
Validation: the final viewer build passes all seven selected camera regressions
under NVIDIA offload/Xvfb (44.81 seconds): record equivalence, clipping planes,
shutter and stereo contracts, and CPU/Vulkan/OpenGL motion. Whitespace checks
pass. No native next references remain in app.cc or next_scene_loader.hh.

Camera record gathering now also uses public root/child traversal, identity,
world transforms, and the same C default readers as interactive camera lookup.
The duplicate native lens/stereo/clipping readers are removed. Gathering keeps
its prior authored order, camera-leaf traversal, identity-transform fallback,
and clipping defaults. The load path passes its retained C Stage directly.
Only selected-camera backplate extraction borrows native data in this path;
ordinary camera gathering no longer does. Backplate schema extraction, native
animation/converter queries and the product parity/default gates remain open.
Validation: viewer build, all seven camera regressions (41.00 seconds), and
the direct GL/Vulkan backplate-display regression pass. The latter verifies
visible/hidden and multiple-plate Vulkan images, and distinct OpenGL pixels.
Whitespace checks pass.

Backplate evaluation now crosses the public C boundary through an owning
`lightusd_backplate` result and a POD `lightusd_backplate_info` record. It calls
the existing core schema evaluator at finite numeric time, retaining instance
validation, interpolation and fallback behavior without decoding images. Info
strings borrow from the result, which remains valid after stage mutation or
destruction. The C++ facade provides a move-only owner. Invalid arguments and
unapplied instances clear the result; info failures clear the record.

The viewer enumerates applied schemas through public metadata and consumes the
public evaluated record. Camera gathering/backplate rendering no longer borrows
a native stage or includes the native camera schema header. Native preview
bounds, animation and the main converter remain migration work, along with the
product parity, allocation and default-switch gates.
Validation: product and standalone Debug builds/suites pass (45 and 44
registered tests, one optional AOUSD skip each). The pure C11 syntax check,
shared-library exports, viewer build and whitespace check pass. GL/Vulkan
backplate-display passes with distinct OpenGL pixels; all seven camera
regressions pass (41.47 seconds).

Checkpoint previews now obtain stage metadata, root/child traversal, active and
visibility state, extents, display colors and world transforms through the
public C API. Startup camera-based mesh ranking uses the same public extent
reader and ancestor transform queries. No native Stage borrow remains in
checkpoint construction. Box limits, inactive/invisible subtree pruning,
first extent-pair selection, ordering and camera-distance ranking are retained.

The new owned `lightusd_attr_copy_default` query shares the inspector's
schema/instance fallback rules without time sampling or connection following.
It materializes a private lazy copy, leaving stage storage unchanged, and
returns a value that survives stage mutation/destruction. Camera clipping-plane
reads also use this direct query. Preview extents accept float/double-backed
arrays; a present but unusable extent does not fall through to extentsHint.
Tests cover schema defaults, blocked/missing/invalid queries, result lifetime,
lazy-copy stage memory, float/double extents, blocked fallback, short/wrong-type
arrays, and time-sampled-only properties.

Validation: product and standalone Debug suites pass (45/44 registered tests,
one optional AOUSD skip each), as do the viewer build, three preview/document
query tests, C11 syntax and symbol-export checks. Cold and warm-cache GPU probes
both upload previews and reach full scene presentation; the warm run reports a
cache hit. Probes terminate the viewer afterward, so they do not verify graceful
shutdown. Native animation/main-converter queries and the broader allocation,
parity, measurement and default-switch gates remain open.
Runtime regression: all eight selected tests pass (45.52 seconds), covering
startup Ptex camera demand and the seven camera-record/clipping/shutter/stereo/
CPU/Vulkan/OpenGL motion checks. Whitespace checks pass.

Blend-weight resolution now uses a shared public-stage-query helper for both
live morph coefficients and load-time morph baking, including instanced
prototypes. It follows the existing first-target ancestor relationship search
and SkelRoot subtree fallback, reads names through owned C values/lists, and
linearly interpolates float-backed sample arrays. Mismatched sample sizes retain
the earlier sample; endpoints clamp. The old native resolver and its dedicated
interpolation helper are removed. Live morph updates no longer borrow a native
Stage. Prototype/bake conversion passes the retained public Stage alongside its
still-native converter context.

Unit tests cover ancestor bindings, invalid-target fallback, nested SkelRoots,
inactive ancestors, midpoint/endpoints, mismatched arrays, string-backed names,
empty sampled-name fallback, and unbound/invalid/wrong-owner handles. Existing
outermost-SkelRoot fallback behavior is retained. At that point skeleton pose
sampling, binding discovery, morph extraction and the main converter remained;
the subsequent migration entries below record the first three moving behind
public queries. The main converter and broader allocation, parity, measurement
and default-switch gates remain open.

Skeleton posing now consumes an owned C skeleton sample rather than native
Skeleton/SkelAnimation schema data. The sample includes joint topology, rest and
bind transforms, and time-evaluated animation channels; its views survive Stage
destruction. Both load-time CPU baking and live raster/ray-tracing bone-row
updates use it. The C smoke test covers topology, channel data, missing
animation, invalid access, and ownership lifetime. At that point native
skin-binding discovery and the main converter remained; the following entry
records the skin-binding migration. The main converter and broader parity,
allocation, measurement, and default-switch gates remain open.

Verification: the product next suite passed 45/45 and the standalone Debug
suite passed 44/44 (one optional AOUSD skip in each). The NVIDIA/Xvfb
`lusdview-deform-skin-xform`, `-skin-animation`, `-morph-skin`, and
`-instanced-proto` regressions all passed. Pure C11 syntax and diff checks pass.

The viewer's remaining skin-binding discovery now uses public C queries for
influence arrays, joint-order tokens, skeleton/animation relationships,
`geomBindTransform`, and mesh/skeleton world transforms. Relationship search
keeps the explicit binding, enclosing SkelRoot lookup, and nested-root animation
fallbacks. CPU baking and GPU skin setup no longer receive native Stage/Prim
arguments. Blendshape baking and GPU morph-channel setup now use public token,
relationship, attribute, and in-between weight metadata queries; sparse offsets
and point indices are copied through owned values. The authored control-cage
reader uses the same public array path. Purpose inheritance, model-root lookup,
and the Unreal `assetInfo` double-sided fallback now use public prim/property
and dictionary queries as well. Native float/token array readers and the
UsdSkel schema include are removed from these deformation paths. All four
deformation regressions pass; the noninstanced blendshape, double-sided, and
proxy-purpose supersede checks pass after the latest query moves. Geometry
conversion and the main converter still use native next internals, so fully
removing native viewer access remains unfinished.

Inherited material binding now has an owned C query backed by the shared
UsdShade resolver, including preview/full purpose fallback, ancestor strength,
and invalid-target handling. Viewer prototype, volume, subset, and pending-mesh
material selection use this query. The pure C test covers ancestor all-purpose
and named-purpose lookup, fallback, unbound purposes and invalid handles.
Material terminal paths and connected shader-port values now use public C
queries too. Volume density/color/emission values and displacement scalar
fallbacks no longer read shader ports through native prims. C tests cover a
surface terminal, scalar port values, wrong prim types and invalid terminal
kind; material inheritance, degraded-material, displacement, and RT geomsubset
viewer regressions pass.

Geometry-light targets and light/shadow collection target enumeration in the
viewer now use the public relationship-target APIs. Collection expansion and
exclude/include matching retain their previous behavior. The viewer builds;
multi-light and geometry-light raster regressions pass. RT light-link coverage
skipped under its runtime capability gate. At that point the native converter,
light evaluation, instance traversal, VDB, point/curve/Gaussian and clip queries
remained in the loader; the subsequent entries record which of those queries
have since moved behind the public API.

VDB volume discovery now enumerates public relationship names and reads field
targets, asset paths and field-name tokens through public C queries as well.
The volume walker now also uses public stage/prim traversal and the public
world-transform query; only VDB asset decoding remains specialized viewer code.
The viewer rebuild and whitespace check pass; no registered VDB viewer runtime
test is available in this build.

PointInstancer prototype target lookup now uses public relationship queries in
both the main instancer pass and nested-prototype expansion. GeomSubset material
binding-presence checks use the same public relationship API, and point
interpolation metadata now comes from public attribute metadata. Instancing and
geomsubset RT material regressions pass; the C API smoke test and viewer build
pass. At this point native-instance prototype metadata and grouping remained
internal; the later entry below records their public/extracted-path migration.

Curve conversion's clip-owner detection now uses a public C query for authored
value-clip dictionary metadata and public parent traversal. The C API test covers
authored clips, missing prims and invalid handles; it passes alongside the
value-clip feature test, viewer build, and whitespace check. At that point
clip-backed curve conversion itself still ran through the native converter;
the later curve-carrier entry records its migration.

Gaussian splat SH budgeting now uses `lightusd_attr_would_materialize_array`,
which checks lazy-array borrowing eligibility without decoding or copying the
payload. The existing 128 MiB threshold and fallback remain unchanged. The C API
smoke test exercises the query over USDC array attributes and passes; the viewer
build passes. The Vulkan Gaussian chunk test skipped because its runtime
capability gate was not met.

The public C API now exposes a borrowed native-instance prototype path; it is
empty for the prototype holder and ordinary prims. A pure C test checks holder,
instance and invalid-handle behavior. Nested-instancer discovery and native
instance grouping both use this public query or the extracted
`RenderPrimRecord::native_prototype` path, removing the loader's direct
`GetPrimSpec` calls. Native-instance skinning, instanced-prototype deformation,
and PointInstancer deformation regressions all pass on NVIDIA Vulkan. The main
native Stage borrow was removed in the later public catalog migration below.

Dome-light texture format/file evaluation and light world transforms now use
public C queries. Stage up-axis and meters-per-unit are also read from public
stage metadata. `BuildNextLights` now discovers lights through public root/child
queries and obtains the converter's typed light parameters through the new
per-prim public render C query. Direct link targets and IES file metadata also
come from public relationship/attribute queries, so the viewer no longer calls
the native light converter or looks up native light prims. The light-link
batching guard checks includes and excludes as relationships. A focused
regression caught that querying these relationship names as attributes merged
differently linked meshes and dropped the blue contribution. The pure C render
API test covers directional-light color, intensity, angle, flags, and wrong
prim type. Multi-light, geometry-light and light-record-equivalence viewer
regressions pass, as does the standalone C render-session test.

The viewer's per-prim curve conversion now goes through a public C render query
instead of `RenderSceneConverter::ConvertCurves` on native prim handles. The
query returns a one-curve render scene, resolves clip assets with public
resolver inputs (stage directory, dependency search paths, and parent-path
policy), and exposes control/tessellated opacity plus tessellated width/color
buffers. The viewer copies the existing tessellated representation into its
compatibility carrier and releases the temporary scene immediately. The pure C
render-session test checks those buffers and a clip asset loaded from disk at a
sampled time. Viewer nonmesh extraction, GL/Vulkan rendering, backend parity,
and value-clip regressions pass.

The render C API exposes bounded per-prim mesh conversion and typed buffers
for opacity, triangulated corner maps, subdivision ancestry, and face-to-triangle
offsets, plus a small extra-info record for UV names, interpolation, color
width, and sidedness. The viewer's ordinary and instanced-prototype mesh
conversions now call this public query and reconstruct their existing carriers
from those buffers, keeping
seam welding, source-face mapping, custom primvars, skin flags, and morph flags.
The C API also provides explicit extent-derived and bounds-derived mesh proxy
queries, and both serial and worker proxy conversion use them. The pure C test
checks proxy geometry plus the public mesh metadata and buffers. Viewer backend parity,
vertex color, UV routing, blendshape, animated skinning, morph-subset, material
inheritance, and geomsubset-material regressions pass. Instanced-prototype
skinning, prototype deformation, PointInstancer deformation, and morph-cull
regressions also pass after the prototype migration. No viewer mesh or proxy
conversion call remains on `RenderSceneConverter`.

A native-instance regression exposed a dangling-record bug in Tydra extraction.
The extractor reserved a `std::vector` using authored `Stage::GetPrimCount()`,
then stored pointers to elements in traversal/category lists. Composed instance
proxies can grow the record count past that estimate, relocating the vector.
A Vulkan render of the instanced-prototype fixture aborted with `std::bad_alloc`
while hierarchy construction read a stale record. Extracted records now use
address-stable deque storage; category/traversal views keep valid pointers as
new proxies arrive. A focused growth test pins pointer lifetime and release.

The instanced-prototype deformation regression now renders and passes in 38.94
seconds on NVIDIA Vulkan raster. Its harness treats allocation aborts and
process signals as failures rather than backend-unavailable skips. The Tydra
unit enables assertions in Release builds so setup performed in assertions is
not silently compiled away; the full direct Tydra unit binary passes.
Validation after the storage fix: both next product (45 registered) and
standalone Debug (44 registered) suites pass with their optional AOUSD skip.
An overlapping first Debug run exceeded the 6-second instance-group timing
limit (7.23 seconds); the isolated rerun passed in 4.14 seconds and the complete
Debug rerun passed in 12.02 seconds. This is timing evidence, not a change to
the instance-group algorithm. Whitespace checks pass.

Geometry-array release tracking no longer inspects native layer pointers or
prim indices in the viewer. A public C query now returns an opaque, stage-local
resource identity for root-layer prim handles; the loader counts pending uses
by this identity and releases arrays by the final record's public path. This
preserves delayed release for multiple instance proxies while removing the
loader's `GetRootLayer`/`GetLayer`/`GetIndex` resource-management dependencies.
The pure C API test checks repeatability and invalid-handle behavior. The
standalone C API test, full native unit suite, instanced-prototype skinning and
VRAM-budget viewer tests pass; the multi-light regression also passes.

Remaining native viewer migration goal:

- [x] Resolve OpenGL software-renderer material AOV parity. The compact GL
  fallback now evaluates normal, coat normal, occlusion, coat weight/color/
  roughness, and specular/IOR F0 semantic modes while retaining bounded
  lighting. OpenGL semantic and degenerate-normal CTests pass under Xvfb.
- [x] Remove `BorrowNativeStage` from `LoadUSDViaNext`. The public render C API
  now owns inherited renderable-prim traversal and returns categorized POD
  records with purpose, material binding, matrices, animation state, and native
  prototype identity. Root/prototype instancers, mesh/proxy conversion, volumes,
  lights, curves, and carrier attributes now use public handles/queries. Float
  array handles preserve lazy backing and bounded-copy access; ParticleField
  schema property selection is also behind the C API. No `BorrowNativeStage`,
  `GetRootLayer`, `GetLayer`, `GetIndex`, or `lightusd_internal` reference
  remains in `examples/lusdview`.
- [x] Audit the loader's clip-backed data paths: curves resolve clip assets
  inside `lightusd_render_convert_curves`; Points/Gaussian arrays use the
  public lazy float-array query; camera/transform samples use public attribute
  and transform evaluation. No loader call reaches native Stage clip helpers.
- [x] Add `lusdview-value-clip-carrier`, which renders clip-backed BasisCurves
  at two time codes and checks the visible carrier changes from wide to tall.
  Both it and `feat-value-clip` pass on the current host.
- [x] Finish native viewer and next/C API regression gates. `ctest -L lusdview`
  passed all 95 registered tests with 22 capability/asset skips. Rebuilt
  `build-next`; its full CTest suite passed all 44 tests with one optional
  AOUSD fixture skip, including both C API session tests.

All checklist gates are verified: the main loader uses the public C API
boundary, clip-backed viewer geometry is tested, OpenGL material AOV parity is
covered, and the full viewer/next suites pass with recorded skips.

Continuation prompt for future maintenance:

> Preserve the public C API boundary in future `lusdview` changes. The
> clip-backed viewer regression, OpenGL material AOV regressions, full viewer
> suite, and full standalone next suite pass; retain their capability-gated
> skips and update this checklist whenever the migration changes.

The viewer's world-transform samples now route through the public C transform
query instead of Tydra's native-stage helper. PointInstancer arrays and
prototype paths now come from a per-prim public render conversion, tested in
the pure C API suite and used by both top-level and nested viewer instancers.
The top-level PointInstancer walk, prototype subtree split, prototype mesh-path
gather and deferred-proxy path lookup now use public prim handles and path
queries instead of `tnext::Stage`/`UsdPrim` traversal.
The inherited render catalog now crosses the same public C boundary. The
loader consumes copied POD catalog records and public float-array views for
Points and Gaussian splats; ParticleField schema property selection is also
exposed by C. `test_lightusd_render_session` checks the catalog, float-array
view, and ParticleField queries. PointInstancer arrays and prototype paths use
per-prim public render conversion for top-level and nested instancers. Mesh,
proxy, volume, light, curve, material, transform, and camera queries use public
handles and C APIs; no `BorrowNativeStage`, direct native Stage traversal, or
root-layer identity lookup remains in `examples/lusdview`.

The compact GL fallback evaluates tangent-space base/coat normals, occlusion,
coat weight/color/roughness, and specular/IOR F0 AOVs while retaining bounded
lighting. The final full native CTest run passed all 311 tests; optional
GPU/backend and supplemental-corpus tests skipped where unavailable. The
focused `ctest -L lusdview` run passed all 95 tests with recorded
capability/asset skips. The standalone `build-next` suite passed all 44 tests
with one optional AOUSD fixture skip. The viewer clip-carrier regression and
`feat-value-clip` also pass. The final `git diff --check` passes.

The next-only WASM `RenderStream` now preserves parser and composition warning
text and exposes it through a bounded `warning()` query alongside `error()`.
Previously, successful in-memory composition could return useful diagnostics
that the product adapter silently discarded. The C dispatch test confirms the
warning query is callable and empty for a clean load; wasm32 and memory64
dispatch suites pass. The one-shot render adapter now retains and exposes the PCP dependency and
typed issue report from in-memory composition, so callers can inspect resolution
outcomes without adopting a persistent `StageSession`.

`NextFlattenSession` now caps retained input as an aggregate: a 512 MiB default
across the root and supplied dependency layers, configurable from 1 byte through
1 GiB. Layer replacement is accounted against the replaced payload and rejected
atomically when it would exceed the cap. The JS adapter preflights dependency
bytes before staging them in WASM memory; bounded queries expose the configured
cap and retained input total. Dispatch tests cover exact accounting, cap
rejection, unchanged retained bytes after a rejected replacement, and refusing
to lower the cap below retained data. The next C dispatch and next-only
converter suites pass on wasm32 and memory64. This bounds retained input bytes;
it does not claim the same cap bounds parsed-layer working memory during
composition, which remains in the allocation-safety track.

The next-only `NextFlattenSession.step()` now returns the flatten pipeline's
structured composition diagnostics when strict composition fails. The fixed
step-info POD reuses its reserved word for a diagnostic count, and bounded
step-buffer kind 3 copies each message; JS includes `compositionErrors` and
`compositionErrorCount` only when present. This preserves existing failure
status and error text while surfacing the full diagnostic list. A malformed
referenced USDA layer verifies a need-layer request followed by a failed step
with retained composition diagnostics. The C dispatch, USDA composition, and
next-only converter suites pass on wasm32 and memory64. Flatten steps now return the source layer identifiers loaded during that
attempt, including partial dependencies with `need-layer` and error results. A USDA root/reference fixture verifies the final `layerDependencies` list, and a two-sublayer case checks partial dependencies on an intermediate `need-layer` response plus the complete final set. The C
dispatch and USDA composition suites pass on wasm32 and memory64. Persistent
Persistent `StageSession` and the native C document session now expose
dependency paths and typed composition issues. The C issue API returns a count
and one counted site/message record per index, without copying the entire
issue list for each query; the values remain available after composition-cache
release. One-shot `RenderStream.compositionReport()` exposes the equivalent
next-only WASM report.

`NextUSDZConverterNative` packaged assets now have a replacement-aware
aggregate payload cap, defaulting to 512 MiB and configurable up to 1 GiB.
`setAsset` preflights the key and byte length before the JS adapter stages the
payload; an over-limit rejection allocates only the key staging block and
preserves the previous entry and total. `assetBytes`, `maxAssetBytes`, and
`setMaxAssetBytes` expose the bound. C dispatch tests cover aggregate rejection,
replacement atomicity, accounting, heap-backed byte views, and refusing to
lower the cap below current use. The dispatch and next-only converter suites
pass on wasm32 and memory64. This caps retained encoded asset bytes, not all
temporary memory used later by USDZ export.

`NextFlattenSession` also exposes the Crate writer's hard output-byte cap,
defaulting to 512 MiB and configurable up to 1 GiB. The cap applies to buffered
and callback-streamed flatten writes before the writer grows its output buffer.
The C dispatch test verifies a 64-byte cap returns the writer's limit error,
raising the cap permits a retry, and lowering the cap after receiving a JS-owned
output copy does not affect that copy. The callback-sink case confirms a failed
stream stops emitting by the configured byte count. Dispatch tests pass on wasm32 and
memory64; the USDA composition and next-only converter suites pass on both
widths as well.

`NextUSDZConverterNative`'s retained URDF mesh store now has the same
replacement-aware aggregate payload bound, defaulting to 512 MiB and
configurable up to 1 GiB. The JS adapter validates mesh counts and geometry
shape through a key/count-only C preflight before staging the float/index
payload; native code checks index range before allocating retained vectors.
Replacing a name accounts against its prior data, rejected replacements leave
the retained entry unchanged, and clearing the mesh store resets accounting.
Dispatch tests cover accounting, cap rejection, replacement atomicity, invalid
geometry, and invalid indices on wasm32 and memory64. This bounds the retained
mesh buffers, not all temporary conversion or export working memory.

The next-only image utility now covers baseline TIFF and DNG output alongside
PNG/BMP. TIFF and DNG use the legacy encoder's same uncompressed 8-bit sample
layout (the DNG selector does not add camera metadata); the next writer
calculates the bounded output size before allocation,
copies pixels once into the final buffer, and writes a little-endian IFD with
strip, channel, orientation, and alpha metadata. Dispatch tests inspect the
header, IFD fields, and exact pixel payload for one through four channels on
wasm32 and memory64; generated files also load through `tiffinfo` and Pillow,
including grayscale-alpha. With `LIGHTUSD_WASM_WITH_EXR` enabled, the next-only
writer also uses TinyEXR v3 for one-, three-, and four-channel 8-bit input,
emitting half-float channels. The EXR query retains one encoded result for the
synchronous JS copy; this avoids a second encode and a second WASM output
allocation. The retained result is released in a `finally` path. A wasm32 and
memory64 dispatch assertion confirms the EXR call allocates only its input
staging block, and `exrheader` plus Python OpenEXR independently validate the
generated RGB file. The codec is optional and can be disabled at configure time.

Next-only `RenderStream.lodVariantCount()` now matches the legacy LOD selector
query by returning the maximum option count of exact-case `LOD` variant sets
across listed prims, or zero when the loaded stage has none. It derives the
value from the bounded variant-list primitives and does not mutate selection.
A nested two-prim fixture verifies the maximum and lowercase-name behavior;
the USDA-composition and RenderStream inventory suites pass on wasm32 and
memory64.

Next-only `NextFlattenSession.addSublayer(path)` provides a bounded authoring
operation for root-layer composition. It parses the retained root through the
next layer reader, appends the authored path, serializes back through
the next USDA writer, and commits only if the aggregate root/dependency input
budget still permits the rewritten root. A two-width USDA fixture adds a
previously absent dependency, completes the need-layer loop, and verifies its
mesh appears in flattened output; a small-budget fixture verifies rejection
leaves the original root usable. The USDA-composition and converter suites pass
on wasm32 and memory64. Sublayer and prim-level reference, payload, inherit, and
specialize authoring now have tested next-only paths; full composed export
parity remains open.

`NextFlattenSession.addPrimArc(kind, primPath, assetPath, targetPath, listOp)`
authors typed reference, payload, inherit, or specialize metadata on an
existing root-layer prim. Arc sites and targets must be absolute prim paths;
reference/payload assets use the authored `@asset@</target>` encoding. The
optional `listOp` applies explicit, add, prepend, append, delete, or reorder
semantics. Edits are serialized into a candidate root and committed only when
parsing succeeds and the aggregate input-byte limit still holds. USDA fixtures
verify all four arc kinds compose geometry; reference and payload arcs request
their external dependency. Missing-site, invalid list-op, and over-budget
cases are rejected without changing the root. The USDA-composition and
next-only converter suites pass on wasm32 and memory64.

Next-only material records now preserve the fallback flag and per-material
conversion diagnostics through bounded C queries. `getMaterialRecord()` and
`getAllMaterials()` include diagnostic kind, material/node paths, shader ID,
and message; unsupported-shader and supported PreviewSurface/OpenPBR fixtures
verify the typed records. The C-dispatch test passes on wasm32 and memory64,
and the next-only converter suite passes on wasm32.

`LayerDocument` now exposes relationship target replacement, inspection, and
removal through the public C relationship API. The JS setter preflights UTF-8
size before encoding, and both JS/native paths cap each target list at 65,536
entries to bound vector overhead. `getRelationshipTargets()` returns owned
strings through bounded per-target copies under a 512 MiB aggregate ceiling.
WASM dispatch fixtures verify replacement, export/reload, removal, and invalid
input on wasm32 and memory64; the next-only converter suite also passes on both
widths.

`LayerDocument.setStageMetadata(key, value)` authors and `getStageMetadata(key)` returns owned typed values plus the authored-opinion flag for the supported scalar
stage metadata keys through `lightusd_stage_set_metadata`: token-valued axis,
default-prim, and color-management fields; string documentation/comment;
asset-valued color configuration; and numeric timing/unit fields. Type checks,
round-trip typed value and authored/unauthored checks, unknown-key rejection, an invalid up-axis case, USDA export, and reload are
covered by the wasm32/memory64 dispatch tests. Those tests exposed and fixed the public C stage-metadata setter failing to mark `defaultPrim`, `doc`, `comment`, `colorConfiguration`, and `colorManagementSystem` authored; the flags now survive USDA export/reload.

`LayerDocument.setPrimMetadata(path, key, value)` now authors the supported
prim metadata keys through the public C setter: `active`, `hidden`, and
`instanceable` booleans; `kind` tokens; `doc`, `comment`, and `displayName`
strings; and the `apiSchemas` token array. `getPrimMetadata()` returns owned
scalar or token-array values through a bounded size-query/copy path, and type
checks the native result before decoding it in JS. The adapters validate types
and payload limits before staging; the native bridge validates packed token
arrays before calling C. Dispatch coverage checks scalar/array authoring and
inspection, authored versus unauthored booleans, authored empty strings,
invalid values/keys, and USDA export on wasm32 and memory64. The native C API
now exposes `lightusd_prim_metadata_is_authored`; its getter and setters use
the model's authored flags for booleans, tokens, strings, and token arrays,
including empty authored values.

The next-only `LayerDocument.setAttributeMetadata(path, name, key, value)` and
`getAttributeMetadata(path, name, key)` expose the public C property-metadata
setter/getter for interpolation and color-space tokens, display/document
strings, hidden booleans, element-size integers, and numeric weights. The JS
setter validates each key's type and a conservative UTF-8 size bound before
encoding; native staging revalidates text and calls the C setter. The getter
uses a bounded size-query/copy and typed scalar decode, returning an error for
unauthored values. wasm32/memory64 dispatch tests verify round-tripped fields,
authored empty strings, unauthored metadata, export, and invalid types.

Next-only `getAnimation()` and `getAnimationView()` now preflight each clip,
and `getAllAnimations()` preflights the complete set, using C size queries for
keyframe times, channel values, array values, joint remaps, and order counts. A
512 MiB weighted materialization estimate rejects excessive aggregate JS
results before allocating channel payload copies; dispatch tests inject an
oversized channel buffer and verify both single and aggregate getters reject
it on wasm32 and memory64. String payloads are charged at twice their UTF-8 byte length plus 64 bytes per
string; residual JS engine object overhead remains part of the broader
allocation audit.

The lighter `getAnimationInfo()` and `getAllAnimationInfos()` summaries now
also preflight animation names and clip-asset paths. The aggregate getter
checks the weighted total for all summaries before constructing any records;
wasm32/memory64 dispatch tests inject an oversized name size and verify both
getters reject before copying it.

Next-only `getRootNode()` now makes a sizing traversal before constructing
the legacy recursive tree. It checks node names, paths, prototype paths,
child lists, and a weighted per-node allowance against a 512 MiB limit, with
a separate 262,144-node guard. A wasm32/memory64 dispatch fixture injects an
oversized root name and confirms rejection occurs before `getNode()` builds
any node object. Direct `getNode()` calls also preflight the combined name,
path, prototype path, child IDs, and object allowance; the fixture verifies
the single-node rejection too. The shared string-copy helper now enforces a
512 MiB per-string cap. Tree traversal remains iterative and cycle-checked.

Next-only `getSkeleton()` now estimates the owned joint arrays, child lists, UTF-8
name/path strings, and legacy aliases before materializing its compatibility
hierarchy. `getAllSkeletons()` preflights the aggregate across all skeletons,
and `getSkeletonJointsFlat()` accounts for the additional flattened matrices.
A 512 MiB weighted budget rejects oversized joint-buffer sizes before payload
copies; wasm32/memory64 dispatch fixtures cover each getter. String size is
charged at twice UTF-8 length plus 64 bytes per string, while residual object
allocation overhead remains an estimate.

Next-only `getMaterialRecord()` and `getAllMaterials()` now query retained
diagnostic string sizes, material names/paths, and parameter buffer lengths
before creating returned records. Both single-record and aggregate queries use
a 512 MiB weighted budget; wasm32/memory64 dispatch tests inject an oversized
diagnostic string and verify rejection before the record copy. The budget
charges UTF-8 strings at twice their byte length plus per-string overhead and
parameter buffers by their returned typed/JS representation estimate.

Next-only `getLight()` now queries light-link strings and resolved mesh-ID
buffer lengths before building its compatibility record, with a 512 MiB
weighted per-light ceiling. `getAllLights()` now performs a full first pass,
preflighting the aggregate weighted estimate across every light before it
builds any compatibility records. The wasm32/memory64 dispatch test injects
an oversized IES path and verifies both single and aggregate getters reject it
before its copy. A two-record synthetic list also verifies the cumulative
limit rejects when each individual estimate remains below the cap. String
charge includes resource names/paths, link strings, UTF-8-to-JS expansion and
per-string overhead; resolved mesh-ID buffers account for the typed copy and
JS array. An injected oversized resource name is checked by both getters.

Material JSON/XML parity now has a standalone wasm32/memory64 regression that
compares the next-only API against the combined legacy loader for PreviewSurface
and OpenPBR fixtures. The OpenPBR fixture also locks the serializer's legacy
aliases and fuzz-over-sheen mapping while retaining distinct authored sheen and
fuzz in the next render record. The renderer's effective sheen slots continue
to receive fuzz values where authored. Both dispatch suites, both exact parity
checks, the next-only API inventory, and the full standalone Debug CTest suite
pass; the AOUSD value-resolution test remains an optional fixture skip.

The parity matrix now maps the four legacy native-render configuration
setters and getters (`set/getNativeMaterialDedup`, `set/getNativeMeshMerge`,
`set/getNativeMeshMergeBakeTransform`, and `set/getNativeFlattenRenderTree`) to
the corresponding next `RenderStream` flags. The next stream stores these same
four settings, and dispatch tests verify each getter tracks both enabling and
clearing its setter. These mappings reduce unmapped combined-loader methods
from 42 to 34; they remain marked for behavior review because the matrix
records API correspondences, not full product parity. The parity-matrix,
next C-dispatch, and next-only converter suites pass on wasm32 and memory64.

Next-only image copies now check the RenderStream's remaining memory budget as
well as the existing 512 MiB payload ceiling. `getAllImages()` preflights its
aggregate estimate before constructing records, and `getImageCopy()` checks its
individual decoded pixel payload before accessing the image buffer. Dispatch
coverage forces the remaining-memory query to reject and verifies neither the
single nor aggregate path reaches the image-data accessor. The rebuilt wasm32
and memory64 C-dispatch suites and parity matrix pass.

The aggregate texture compatibility getter now checks `RenderStream` remaining
memory after its full 512 MiB output estimate and before constructing any
texture records. A dispatch regression injects an exhausted-memory result and
confirms the aggregate rejects before its first record getter. The rebuilt
wasm32/memory64 dispatch suites and the parity matrix pass.

The 512 MiB aggregate getters for animation summaries, full animation payloads,
lights, skeletons, and materials now also compare their preflight estimate with
the stream's remaining memory before building any JS records. Shared
`preflightRenderAggregate()` enforces the same check for each family. Dispatch
regressions inject zero remaining memory and verify the per-record getter is
never called for any of the five aggregates. The rebuilt wasm32/memory64 C
dispatch suites and parity matrix pass.

NextAssetStore's identifier and UUID aggregate getters now cap estimated
JavaScript output at 512 MiB before copying the strings into the returned
array/object. Identifier enumeration also rejects more than 65,536 entries;
search-path enumeration applies the same entry-count and aggregate bounds.
The asset store has no RenderStream-style remaining-memory query, so these
checks enforce a fixed output budget rather than claiming a live heap-headroom
measurement. The C-dispatch and 28-method parity suites pass on wasm32 and
memory64.

The next-only parity matrix now maps legacy `remapLayerAssetPaths` to the
`NextFlattenSession` workflow `begin` → `remapLayerAssetPaths`. This direct
root-layer mutation returns the number of rewritten asset values and is covered
by the USDA composition fixture, including unchanged output after rejected
aggregate-budget growth. The matrix now has 25 NextFlattenSession workflows and
31 methods without a same-name or mapped next surface. The matrix and USDA
composition regressions pass on wasm32 and memory64.


Legacy `exportLayerAsUSDCWithOptions()` now maps to `NextLayerDocument.load()`
plus `exportUSDC()`. Its legacy implementation currently ignores the options
object. A wasm32/memory64 parity regression exports the same USDA layer through
both implementations, reloads both USDC payloads, and compares the re-exported
USDA text byte-for-byte. The layer-export map now covers six workflows, and
the cross-product matrix has 31 methods without a same-name or mapped next
surface; optioned caller-buffer export and stage export remain separate gaps.


`exportLayerAsUSDCToBufferWithOptions(buffer, options)` now maps to
`NextLayerDocument.exportUSDCToBuffer(buffer, options)`. The next adapter
preserves the legacy validation order and error results for unloaded layers,
null/wrong/empty buffers, and insufficient capacity; failed capacity checks
leave the caller buffer unchanged. On success it copies the owned USDC result
into the supplied Uint8Array and reports the written size. The parity fixture
compares both implementations' success bytes, result sizes, untouched tail
bytes, and all invalid-capacity error cases on wasm32 and memory64. The matrix
now has 31 methods without a same-name or mapped next surface.


The parity matrix recognizes `validateFromBinary` as a module-level next API
rather than a missing class method. Its WASM compatibility adapter now disables
authored-stage-presence warnings, matching the legacy loader while native next
validation retains those stricter checks. The combined-vs-next regression
checks USDA valid/invalid fixtures, USDC, and parse failures. USDA results now
match exactly. Next now preserves the legacy USDC threading-warning text; its
warning lacks the legacy source-location prefix. For USDC, the adapter reuses
the bounded next CrateReader result for semantic validation and reports the
crate checked-group only after successful decode. The remaining known
difference is parse-failure wording (plus the warning's source prefix). The matrix marks this row
`known_behavior_gap`; 30 legacy methods still have no next surface or mapped
workflow. The regression runs on wasm32 and memory64.

The missing deferred-tangent configuration pair is now exposed by next
`RenderStream` as `set/getDeferTangentComputation`. It shares the existing
`computeTangents` flag with inverted semantics, so the default remains deferred
and toggling either API is reflected by the other. Dispatch coverage verifies
the default, both directions of the mapping, argument validation, and the
cross-product inventory records both methods. The rebuilt wasm32 and memory64
dispatch suites and parity matrix pass; the matrix now has 28 methods without
a same-name method or mapped workflow.

Next `RenderStream.reset()` now matches the legacy full reset boundary: it
releases the converted scene, provided and streamed assets, imported memory
assets, and their shared payload-budget reservations while preserving the
stream's configuration. It is distinct from `end()`, which only drops the
current scene. Dispatch coverage loads a scene and retains an asset, then checks
that reset clears both and preserves a setting. The rebuilt wasm32/memory64
dispatch suites and parity matrix pass; 27 methods remain without a same-name
method or mapped workflow.

`exportStageAsUSDCToBufferWithOptions` now maps to the `RenderStream` workflow
`begin` → `exportUSDCToBuffer`. The next writer serializes the retained next
Stage into a bounded C-owned Crate buffer, and the JS adapter validates the
caller buffer before writing, leaves it unchanged on insufficient capacity,
and safely rebinds a WASM-heap-backed target if writer allocation grows memory.
The two-width parity fixture compares invalid-buffer errors, untouched tails,
and reloaded USDA text from the legacy Stage export and next Stage export. The
layer-export map now covers seven methods, and 26 matrix entries still lack a
same-name method or mapped workflow.

Next `RenderStream.debugLogMemory(label)` now preserves the legacy debug
contract: it returns `{label, heapBytes}` and emits the same `manual` event
shape through `Module.onLightUSDDebug` when a listener is installed. Callback
exceptions propagate once, and argument/receiver checks are covered. The
wasm32/memory64 C-dispatch suites and parity matrix pass; 25 matrix entries
remain unmapped.

`loadAsLayerFromBinaryWithProgress` now maps to
`NextLayerDocument.loadWithProgress(bytes, callback)`. Parser progress from
USDA and USDC is forwarded with phase-local counts, and returning `false`
cancels the load without replacing the document already held by the object.
Callback exceptions are captured, cancel parsing, and rethrow once after the
native parser returns. The regression checks both formats, cancellation,
exception propagation, and document preservation;
the matrix now has 24 entries without a same-name method or mapped workflow.

`getMhProfileJSON` now maps to `NextLayerDocument.getMhProfileJSON()`. It
traverses retained next Stage data, preserving authored `mh:*` scalar/array
values, relationships, and time-sampled float arrays. A checked size/copy C
query caps traversal and JSON output at 512 MiB; the wasm32/memory64 regression
compares parsed output with the legacy loader and covers sampled curves and
relationships. The parity matrix now has 23 entries without a same-name method
or mapped workflow.

`getShadingGraphJSON` now maps to `NextLayerDocument.getShadingGraphJSON()`.
The next inspector walks authored Stage opinions only (excluding schema
fallback properties) and preserves the legacy top-level schema, authored
connections and relationship targets, asset paths, property color-space
metadata, and `ColorSpaceAPI` values. A bounded C size/copy path enforces a
512 MiB estimate/output limit and one-million-prim traversal cap. A mixed
Material/Shader fixture compares parsed next and legacy JSON on wasm32 and
memory64; 22 matrix entries remain without a same-name method or mapped
workflow.

`exportLayerAsUSDZWithOptions` now maps to `NextLayerDocument.exportUSDZ()`
with an optional `NextAssetStore`. The writer supports USDA and USDC root
layers, bounds package output and copied assets, and reserves memory for the
retained Stage and store before serialization. The parity fixture compares
stored ZIP entries, packaged asset bytes, and reloaded root-layer USDA text
against the legacy export for both root formats. wasm32 and memory64 builds,
layer-export parity, dispatch validation, and the combined parity matrix pass;
21 legacy methods remain without a same-name method or mapped workflow.

`releaseSourceLayer()` is now present on `RenderStream`. The legacy hook frees
the pre-composition layer after composition has completed. Next composition
consumes the source root and replaces it with the composed Stage, so the hook
has no additional allocation to release; it remains a no-op and leaves the
composed Stage and render scene intact. Dispatch coverage exercises it after
an inherit composition and verifies that the composed meshes remain available.
The WASM parity inventory recognizes the same-name method; 20 methods remain
without a same-name method or mapped workflow.

The point-instancer query family `numInstances`, `getInstance`, and
`getInstancesForMesh` now maps to next `RenderPointInstanceDraw` records.
Instance order follows draw order, mesh queries preserve that order, local
matrices come from the instancer-local draw transforms, and global matrices
apply the instancer Stage transform. The next adapter retains legacy integer
argument truncation and null/empty invalid-index behavior, with a one-million
record ceiling before building an aggregate result. The dispatch fixture
checks the same translated/scaled instance matrices as the legacy query tests,
mesh association and ordering, and fractional numeric indices. The matrix now
has 17 methods without a same-name method or mapped workflow.

`setLoadTextureInNative` now controls next conversion's native image decode.
The default remains false, preserving metadata-only images; when enabled, the
converter reads retained `NextAssetStore` views without making another encoded
asset copy, preflights dimensions and a cumulative decoded-payload estimate,
and rejects images that exceed the remaining stream memory budget before
decoding. The regression supplies valid UDIM PNGs, checks lazy default behavior
and decoded pixels when enabled, and validates the setter/getter contract.
wasm32 and memory64 dispatch and parity matrix runs pass; 16 methods remain
without a same-name method or mapped workflow.

`getCombineUDIMTiles` / `setCombineUDIMTiles` now control next conversion's
UDIM atlas packing. Next keeps its sparse-tile default (`false`) for editor
workflows; enabling packing requires native texture loading and combines
same-sized decoded 8-bit tiles into a bounded RGBA atlas. The stream reports
the atlas UV scale/offset alongside `isUDIM` and uses `udimTextureId == -1`
to identify the packed representation. Ineligible or undecoded tiles remain
sparse with a conversion warning. wasm32 and memory64 dispatch fixtures cover
both representations and compare atlas pixels and UV remapping; 14 legacy
methods remain without a same-name method or mapped workflow. The legacy
configuration matrix verifies its default is `true`, while the next dispatch
fixture verifies `false`; the parity matrix now records this as an explicit
known product-default difference rather than treating it as an unchecked
same-name match.

`validateLoadedLayer(optionsJson)` is now available on next `LayerDocument`.
It validates the document's current authored state by exporting USDA into
the document's retained C buffer and passing that buffer directly to next's
C-backed AOUSD validator. Only filename and options are staged in an extra
WASM allocation; the full USDA payload is neither decoded/re-encoded nor copied. The no-layer JSON result matches legacy exactly; authored USDA
validation results match after clearing the legacy loader's prior diagnostics.
The fixture also checks argument errors. wasm32 and memory64 validation and
parity-matrix runs pass; 13 legacy methods remain unmapped.

Legacy `layerToRenderScene()` maps to the next split-object workflow
`RenderStream.beginFromLayerDocument(document)`. A C export validates both
handles, asks the document to retain its USDA output, and passes that buffer
directly to the stream; JS does not create a second USDA string or byte view.
The stream applies its normal input/resident-memory preflight and reports its
usual parse progress. The paired legacy/next fixture compares mesh, material,
texture, image, and light counts for an authored mesh layer; next reports one
additional node because it includes its explicit scene root. Both WASM widths
pass and the parity matrix now has 12 unmapped methods.

`getMemoryStats()` is now available on next `RenderStream`. It reports retained
scene counts and sums size-query results for the ten mesh buffer streams;
when an asset store is attached, its cache count and byte limits come from
`NextAssetStore.memoryStats()`. The next stream does not retain the legacy
reordered-mesh cache, so that count is zero, and buffer count reflects nonempty
next mesh streams. Dispatch checks scene counts, payload totals, argument
validation, and attached-store statistics. The parity matrix maps this method
to RenderStream and now has 11 unmapped methods.

The sublayer, reference, and payload asset-path extraction methods are now
explicitly mapped to next `RenderStream` in the parity matrix. A paired
legacy/next fixture with two sublayers and reference/payload arcs compares all
three ordered path lists from the same authored root layer. The full combined
WASM regression passes on wasm32, and a direct legacy-versus-next comparison
passes on memory64; the matrix still has 11 methods without an explicit next
surface or workflow.

The same paired fixture now verifies `hasSublayers()`, `hasReferences()`,
`hasPayload()`, `hasInherits()`, `hasVariants()`, `lodVariantCount()`, and
`extractVariants()` against legacy results before composition mutates the
layer. It also compares `getURI()` and `getUpAxis()` on the same loaded root.
These nine query methods are explicitly mapped to RenderStream in the matrix.
Combined regression and direct legacy/next checks pass on wasm32 and memory64;
46 methods now have explicit RenderStream mappings.

`extractUnresolvedTexturePaths()` is also explicitly mapped to next
`RenderStream`. The existing Unicode multi-material fixture now loads identical
USDA through the legacy loader and next stream with native image decoding
disabled, then compares the full unresolved URI list, including order and
duplicates. The full combined regression passes on wasm32 and memory64; 25
methods now have explicit RenderStream mappings.

The loader-configuration matrix now records the next memory-limit policy
difference explicitly. Next defaults to an architecture-independent 1 GiB and
accepts limits from 1 through 8192 MiB; legacy defaults to 2 GiB on wasm32 and
8 GiB on memory64 and preserves signed setter values. A paired default fixture
checks the complete shared configuration set on both widths, including the
intentional UDIM difference and the exact memory defaults. The matrix marks
`getMaxMemoryLimitMB()` and `setMaxMemoryLimitMB()` as known behavior gaps
because next applies a stricter untrusted-input budget policy.

Ten additional loader-configuration getters are now explicitly mapped to
RenderStream: defer tangent computation, bone reduction, value clips, round
bone count, sphere subdivisions, target bone count, and the four value-clip
range/sample settings. The paired fixture asserts their complete defaults are
equal to legacy, and direct paired runs pass on wasm32 and memory64. UDIM and
memory-limit differences stay separately marked as known behavior gaps.

The matching configuration setters are now explicitly mapped after canonical
accepted-value comparisons on both WASM widths. The paired checks cover tangent
deferral, bone reduction/rounding and target count, sphere subdivisions, value
clip enable/range/sample settings, and the UDIM combine toggle. The UDIM setter
remains a known behavior gap because the products intentionally have different
defaults and next's typed API rejects legacy Embind coercions; the other setters
remain behavior-review mappings because their invalid-input coercion contracts
differ even though accepted values produce identical state.

The aggregate allocation audit also bounded two RenderStream payload paths.
`providedAssetNames()` caps entries at 65,536, queries every UTF-8 name length,
and checks a 512 MiB weighted aggregate estimate plus remaining stream budget
before allocating the returned strings. `getMeshSubsetOutput()` caps the
combined estimate for its WASM and JS payload copies at 512 MiB, validates
material/submesh counts before building records, and preflights a weighted
object estimate.
Injected excessive counts and payload sizes prove both methods reject before
their first WASM allocation. wasm32/memory64 dispatch, parity matrix and
whitespace checks pass.

The follow-up allocation audit also bounds blend-shape and material-diagnostic
aggregates. Blend-shape and diagnostic counts are capped at 65,536; diagnostic
string byte lengths are queried across all four fields before copying, and the
weighted WASM/JS estimate is preflighted as one aggregate. Each blend-shape
name and every point/normal offset size are queried and included in the weighted
aggregate before any returned name or offset buffer is allocated; inbetween
records share the same 65,536 total-record cap. `compositionReport()` now caps
dependency and issue counts, weighs
each copied string against the 512 MiB aggregate ceiling, and checks remaining
stream budget before allocating. Injected excessive shape, diagnostic, and
composition counts verify rejection before the first WASM allocation; oversized
diagnostic strings and combined blend-shape offsets are rejected before their
returned-data allocations. The wasm32 and
memory64 next dispatch and composition suites, the 208-method parity matrix,
and `git diff --check` pass. Eleven combined loader methods still have
no mapped next surface or workflow; the matrix keeps these explicit rather
than asserting parity without evidence.

`getSceneMetadata()` now includes `StageMeta.comment` and authored `autoPlay`,
while absent start/end time codes remain `null`. The next USDA parser and writer
retain authored `autoPlay`, and stage metadata derives it from the root layer.
The paired viewer fixture authors `autoPlay = false` and compares the full
record against legacy; its layer-document checks cover USDA and USDC export /
reload. Legacy render conversion now reads the payload of `StringData` values
in `customLayerData.copyright`, matching next `StageMeta` and the viewer query.
The paired fixture checks authored copyright alongside autoplay. For unauthored `metersPerUnit`, next render conversion and
`getSceneMetadata()` now use the legacy render-product fallback of 1.0 while
next Stage metadata retains its 0.01 fallback. A paired unauthored fixture and
the authored metadata checks pass on wasm32 and memory64; the matrix no longer
marks this render query as a known default difference. The full combined loader
regression also covers USDA export and the USDC export/reload round trip.

The cross-product matrix now classifies `testValueMemoryUsage` as a test-only
legacy memory-probe helper rather than a missing product API. It is compiled
only into the legacy combined build and is used by the memory-usage CLI and
tests. The matrix still inventories all 208 combined methods, but reports this
one separately from the six MCP methods retained by an explicit product-boundary
decision; the current matrix has no unresolved product methods. `layerToJSON` now maps to
`NextLayerDocument.load()` plus
`exportJSON()`. The next-only serializer emits the legacy envelope, recursively
serializes root/nested prim specs and basic typed attributes, relationships,
connections, property metadata, and time-sample values using the next USD value
printer. Its weighted estimate and final output are capped at 512 MiB, and the
JS adapter decodes a view of retained output without making a second full-size
WASM copy. Paired wasm32 and memory64 tests compare the complete prim-spec tree
and authored layer/property metadata against the legacy serializer for a nested
layer with a typed string attribute, relationship, and sample-only attribute;
both next-only C dispatch tests also pass.

`layerToJSONWithOptions` now maps to `NextLayerDocument.load()` plus
`exportJSONWithOptions(embedBuffers, arrayMode)`. Legacy PrimSpec JSON stores
values as canonical USD text and does not populate buffer/accessor tables, so
`embedBuffers` and `base64`/`buffer` modes produce the same layer records; other
array-mode strings fall back to `base64`. The paired test compares parsed
prim-spec and metadata structures for `base64`, `buffer`, and an unknown mode
against legacy on wasm32 and memory64. The mapping remains under behavior
review for the full property/metadata surface.

The concrete port surface for that family is now audited. Legacy layer JSON
contains the layer name and authored metadata, recursive `primSpecs` with
specifier and list-op qualifiers, property type/variability/interpolation and
authored metadata, connection and blocked-value state, default/time-sample
values, plus optional `buffers`/`bufferViews`/`accessors` tables. Values use
canonical USD value text so arbitrary supported USD types remain lossless.
The current `exportJSON()` and `exportJSONWithOptions()` mappings are behavior
review mappings, not full schema parity: uncommon layer/prim composition
metadata and complete metadata/property handling still need typed
serialization. `NextLayerDocument.loadJSON()` now imports the exporter's
supported subset: basic prim hierarchy/specifiers, typed default and time-sample
values, relationships/connections, selected property metadata (including
`allowedTokens`, `customData`, `assetInfo`, and `sdrMetadata`), common layer metadata, prim `inherits`, `specializes`,
`references`, and `payloads` list operations, plus `variantSets` list operations
and the `variants` selection map. The reference/payload JSON
records retain asset and prim paths plus layer offset/scale; bare operations
use the legacy empty `op` string and asset-only targets use the legacy
`#INVALID#` prim-path marker. JSON paths convert to the internal arc spelling
on import so authored opinions write valid USDA. Paired legacy fixtures check
all four arc fields, variant metadata, and the resulting USDA on both WASM
widths. Variant body content is outside the legacy PrimSpec JSON fields.
Reference `customData` is now retained by the next arc model, including nested
values through USDA, USDC, and JSON roundtrips. Dictionary-valued
`customLayerData` and `expressionVariables` now import and export recursively
for JSON booleans, numbers, strings, nested dictionaries, and typed-value
envelopes for supported USD arrays/compound values. Typed arrays retain their
USD type name rather than being inferred from untyped JSON. Raw JSON arrays
remain unsupported; typed asset and dictionary payloads are handled as
described below, while other special payloads still need coverage. Paired JSON tests verify
nested dictionaries and a legacy-serialized `float[]` round trip. Layer JSON
also preserves `hasOwnedSubLayers`, root `primChildren` ordering, sublayer asset
paths and offsets, relocates, and unregistered string metadata. Paired import
checks compare all of these layer metadata fields against the legacy JSON shape.
It parses into a temporary layer and installs the result only after
validation succeeds; malformed JSON clears any previously loaded document to
match the legacy failure transition. JSON input and estimated working set have
explicit caps. `Layer::memory_usage()` now includes layer metadata strings,
ordering and sublayer vectors, relocates, recursive dictionary and numeric
array payloads, and unknown extension fields; the JSON import working-set
check uses this estimate. `PrimSpec::memory_usage()` now also charges retained
property metadata dictionaries and numeric arrays, prim cold strings and list
edits, relationship/connection opinions, extension fields, and variant-option
records. Native regressions verify that large layer and prim/property metadata
raise the layer estimate. The layer estimate now walks nested variant content
Layers iteratively and charges each shared Layer once; a native test attaches
one content Layer to two options and verifies the second owner does not double
the charge. This remains an estimate rather than exact allocator accounting.
Paired tests
cover export -> import, malformed-input state, and both WASM widths. A private
C-stage bridge installs the validated Layer directly
into a freshly-created stage handle, avoiding a USDA serialization/reparse pass;
the legacy paired import now matches the full tested prim-spec tree, including
custom-property flags. The failure transition now also matches legacy:
malformed input clears the loaded layer and retains an error. The importer
decodes the exporter's canonical quoted string metadata before authoring it.
Paired legacy import confirms that ordinary and blocked relationships,
attributes, nested prims, sample-only attributes, values, custom flags, and
metadata survive. Paired legacy-authored property dictionaries cover nested
`customData` and `sdrMetadata`; an injected Layer JSON fixture checks
`assetInfo` import/export, USDA export/reload, and USDC export/reload because
the legacy USDA parser does not accept that fixture's property spelling. The
USDC round trip also checks root `doc`, `comment`, `owner`, and
`renderSettingsPrimPath`. Layer String ValueReps now use the inline-index bit,
as prim and property strings already did; omitting it made strict Crate reload
reject an authored root `comment`. The layer JSON exporter now includes
authored `renderSettingsPrimPath`, matching the importer and legacy JSON.
Authored `colorConfiguration` and `colorManagementSystem` now export as
legacy-shaped layer metadata; paired USDA and USDC checks cover both fields.
`playbackMode` is now an authored next layer/stage field instead of relying on
unknown-metadata preservation. USDA, USDC, and Layer JSON carry its legacy
`loop`/`none` spelling; the paired legacy fixture authors `loop`, and the
USDC reload fixture verifies it with the other root metadata fields.
These dictionaries share the recursive JSON metadata parser and reject
unsupported raw arrays. Declared-only attributes use legacy `emptyAttribute`
records; bare relationship declarations use `noTargetsRelationship`.
Relationship `= None` retains the legacy `valueBlock` marker, while `= []`
retains an authored empty `pathVector` with `targetCount: 0`. The Next USDA
writer now keeps those three spellings distinct. A sampled Pixar USDC encodes
`None` and `[]` with the same `targetPaths` fieldset, so Crate reload still
normalizes that specific distinction.
`loadLayerFromJSON`
now maps to `NextLayerDocument.loadJSON()` in the checked matrix; authored
blocked attribute defaults and time samples now round-trip with `None` and
blocked markers. Single-operation relationship list edits (`add`, `append`,
`prepend`, and `delete`) retain their authored qualifier and operand targets in
JSON export/import; paired tests compare them to legacy on wasm32 and memory64.
Delete-only opinions now remain in `relationship_names()` even when removing
the last effective target empties the local target vector; both the native
Crate round-trip test and JSON export/import cover that case. `order` is parsed
from JSON but the legacy USDA JSON path has no paired authored relationship
fixture for it. Combinations of multiple authored sublists on one relationship
or prim arc still lack preserved operation ordering in the next layer model.
The legacy Layer PrimSpec exporter emits property arrays as canonical USD text
in both `base64` and `buffer` modes, including the paired `float[]` fixture;
it does not emit accessor tables on this path. Importing externally supplied
buffer bytes is not part of PrimSpec import in either product. Next now
validates optional `buffers`, `bufferViews`, and `accessors` transport tables
before accepting Layer JSON, including embedded base64 size and syntax limits;
the validation avoids a second decoded copy because no property reads the
table. Paired legacy/next tests cover valid tables and malformed lengths,
external URIs, offsets, counts, and null table values. Typed asset metadata
objects now import their authored `assetPath` through the next Layer JSON path;
a paired legacy fixture verifies JSON normalization and USDA asset spelling on
wasm32 and memory64. Typed dictionary wrappers in layer metadata now retain their wrapper and
nested typed values through next Layer JSON import/export, matching legacy on
paired wasm32 and memory64 fixtures. A dictionary serialization hint survives Value copy-on-write
without changing USDA/USDC dictionary semantics. Typed compound payloads
now preserve vector-array elements and 2x2/3x3/4x4 matrix rows instead of
emitting flat arrays or null; paired tests cover scalar matrices and color
arrays on both WASM widths. Next-only matrix-array checks cover nested
dimensions; the legacy importer faults on that input, so it is not counted as
paired parity. Scalar `int2`/`int3`/`int4` metadata now exports typed component
arrays instead of null; paired layer JSON checks pass on wasm32 and memory64.
Scalar `timecode` layer metadata now retains its typed JSON envelope rather
than losing the type to a plain number; the same paired fixture verifies it.
Typed `bool[]` metadata now exports boolean elements instead of null through
the shared next web value encoder; paired layer JSON checks pass on both WASM
widths.
Half-precision scalar, vector, role, quaternion, and array layer metadata now
round-trips through next Layer JSON instead of exporting null; this is a
next-only check on both WASM widths because the focused legacy half import
probe did not complete. Other
special typed payload objects and full legacy field
coverage remain open before parity can be considered complete. Follow-up
paired tests must cover the complete metadata/property surface, legacy JSON
import, and output-budget rejection on wasm32 and memory64 before these methods
can be marked parity-complete.

Property `customData`, `assetInfo`, and `sdrMetadata` now use the layer-aware
typed metadata encoder. A paired authored USDA fixture verifies nested
`bool[]` and `color3f[]` records retain their type and element grouping on
wasm32 and memory64. The property metadata output budget now counts nested
dictionary entries and array elements before serializing them, rather than
charging only a top-level array. Layer JSON export now also preflights the
minimum decoded storage for crate-backed lazy arrays at a 16x working-set
weight before metadata JSON conversion or property-value printing. A generated
270,000-element `matrix4d[]` USDC fixture is rejected before JSON
materialization on wasm32 and memory64 while the layer remains usable for USDC
export. The suspected repeated string-table expansion is not a lazy-array
case: the crate reader eagerly decodes `string[]`, `token[]`, and `asset[]`.
Their expanded
storage is included in the retained Layer estimate before Layer JSON export.
The native codec-matrix round-trip now checks that all three remain non-lazy.

A sampled comparison of the first 100 small top-level USDA fixtures found 99
that both products load; complete parsed Layer JSON differed on 29 before the
property-metadata audit and 14 afterward. Next now matches the legacy
`isEmpty` flag for connected attributes and the canonical metadata strings for
interpolation, color space, render type, numeric flags/indices, and simple or
multiline documentation; next JSON import accepts both the legacy canonical
spellings and its earlier scalar spellings. Paired authored metadata and
multiline fixtures pass on wasm32 and memory64. The USDA parser now retains
`custom` and authored variability flags on time-sample-only attributes;
native parser and paired Layer JSON tests cover that correction.
Layer JSON export now emits an attribute's retained raw default text, so
`edit [...]` array edits export as legacy's canonical value instead of an
`emptyAttribute`; JSON import parses `edit [...]` through the USDA parser's
now-shared `ParseArrayEditText`, validating literals against the element type
and restoring both the canonical text and the structured op list. Matrix values
and time samples use legacy's pxr-style `( (...) )` spelling in Layer JSON only;
the next USDA writer keeps its compact spelling so flatten baselines are
unchanged. A paired fixture covers `int[]`/`string[]` edits, scalar, array and
time-sampled matrices, legacy-JSON re-import, and wrong-type edit rejection on
wasm32 and memory64. The same 99-file sample now differs on 3 files, each an
intentional next behavior: `__AnyType__` is cleared at parse time to match pxr,
root `reorder rootPrims` is preserved as `primChildren` where legacy drops it,
and an attribute authoring both a default and `.connect` keeps both where
legacy drops the connection. Next imports legacy's resulting targetless
`connection` record as a value-only attribute (legacy cannot re-import that
record itself); a targetless connection without a value is still rejected.
The 99-file sample is an audit aid, not a complete
parity gate.

The parity matrix now maps legacy `loadTest(filename, byteView)` to the
`NextLayerDocument.load(byteView)` loading workflow. Both APIs accept the same
DataView slice over a padded backing buffer; the paired regression verifies
successful loading of the authored prim through next's typed layer interface.
The full combined runtime suite exercises this check on wasm32 and memory64.

The six combined MCP context/tool/resource methods are now represented in the
parity matrix as `product_decision`, matching the product-scope table above:
they remain in the dedicated MCP server integration and are not APIs of the
next-only scene-core WASM module. The verifier checks that each entry belongs to
the MCP family, has no accidental RenderStream match, and carries the explicit
decision. They are reported separately from both parity mappings and unresolved
product gaps; this is a scope decision, not a claim of method-level parity.
