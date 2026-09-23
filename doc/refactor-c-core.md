# C-style core consolidation

## Consolidation scope

The renewed consolidation work prioritizes native and WASM compile time and
allows public API breakage. The former 512 KiB WASM goal is not an acceptance
criterion. Feature parity remains required before switching defaults or
removing the legacy product.

The target is one default implementation based on `src/next`, compiled as
C++17 with explicit storage and non-template algorithms. Legacy remains an
explicit compatibility product. The native and WASM defaults must not switch
until the current default products' supported capabilities have regression
coverage on next. API changes are allowed; removing capabilities is not an
optimization.

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

This is not yet the final consumer migration. Remaining requirements are the
method-level WASM-to-C inventory and POD bridge, completion of aggregate WASM
getters and resource payload coverage, migration of native viewer/tool
consumers, then parity validation and legacy removal. Existing
WASM emval dispatch and native internal C++ consumers remain supported during
that migration. Root native and WASM defaults have not changed.

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
An include audit currently finds 52 example and web translation units or headers that
still import next/Tydra internals. `lusdcat --explain` uses the public C API;
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
| Shared binding boundary | Partial: next WASM class registration is replaced by generation-checked C exports and JS wrappers. Existing method coverage is retained. Native C now has a persistent render session with POD change input, owning snapshots, transactional resource events with tested begin/resource/end ordering, stable resource lookup, allocation-free scene-record enumeration, scene render-settings/color-space strings, and fixed-width scene stats matching the WASM boundary. Native C now also exposes typed animation clip/channel metadata, key/value/remap buffers, bounded order/clip-asset strings, material diagnostics, fallback/displacement/volume flags, terminal paths, and retained OpenPBR, volume, and PreviewSurface-utility node-graph JSON copies. The installed C++ facade now forwards every published render C export, including MaterialX configuration, alongside the common node, mesh, material, texture, image, light, camera, skeleton, instancer, point-draw, and unsupported-record info/buffer calls without requiring consumers to spell the C handles. WASM now has typed RenderStream counts/stats/animation-channel/skeleton summaries and resource/scene-record paths, node metadata/transforms, mesh fields (including optional tangents, secondary UVs, colors, skin, bounds, skeleton association, material-subset ranges, and primvar names/metadata plus raw data/indices buffers when retained), mesh/point-cloud/curves/point-instancer and point-draw fields, curve topology/interpolation metadata, point-instancer prototype paths and binding buffers, animation target/order strings, light, camera, material scalar fields (including clearcoat), texture fields (including wrap modes, output channel, UV rotation, and a fixed-layout sampling payload), scene scalar metadata and strings, plus bounded geometry, animation, skeleton, decoded-image, and USD Physics payload copies and bounded-error exports; remaining aggregate getters outside the published inventory and other resource payloads remain pending. The next-only WASM module no longer links embind or uses emval; the combined legacy module retains its embind API. |
| Tydra chunk ownership | Implemented: one compiled, element-size/alignment-aware storage engine replaces per-type vector/shared-pointer chunk ownership. The thin typed facade provides read-only indexing and explicit mutation. |
| Product selection and defaults | Root CMake accepts `LIGHTUSD_NATIVE_PRODUCT=next` and returns before configuring legacy dependencies; `legacy` remains the default. With `LIGHTUSD_BUILD_EXAMPLES=ON`, the next product now automatically builds and registers the public `next_c_api_example` smoke test; Tydra-enabled builds also register `next_render_session_example`, which exercises persistent C render-session revision, event-sink streaming, and record enumeration. The core-only `LIGHTUSD_WITH_TYDRA=OFF` configuration remains supported. Full parity and the default switch remain pending. No implicit legacy fallback is permitted in the final next product. |

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
| material diagnostic/node-graph payloads | diagnostic POD records and bounded retained OpenPBR, volume, and PreviewSurface utility-graph copies; next WASM exposes preferred surface, PreviewSurface utility, and volume graphs through bounded copies | no remaining next RenderStream graph payload gap; `LightUSDLoaderNative.getMaterialWithFormat` parity remains in the combined-module audit |

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
primvar catalog and bounded copies. Mesh merge leaves sources with custom
primvars separate, preserving their indexed value domains; ordinary meshes
still merge. Combining custom streams into one merged output remains a future
optimization, not a prerequisite for retaining their data.
In the same next-only wasm32 build tree, the mesh-output object changed from
59,618 to 47,530 bytes (20.3% smaller); the linked `.wasm` stayed effectively
flat at 1,665,684 to 1,665,666 bytes. These are object/file sizes, not a
measured compile-time gain or whole-product size claim. The post-split Node
profile passes all 24 suites.

This inventory is the parity checklist for removing aggregate emval consumers;
each remaining gap must gain a bounded C/POD export before the next product can
become the default.
The exact next-only `RenderStream.prototype` surface is captured in
`web/js/tests/renderstream-api-inventory.json` and checked against the live
module by `usdzconvert-next-only.test.mjs` (226 methods, including compatibility
getters and aliases). Typed operations map by family to the
`lightusd_next_render_*` POD/checked-copy exports in `web/binding-next-api.h`;
the corresponding native resource-info, buffer, and string-copy functions are
the `lightusd_render_*` exports in `lightusd-render-c.h`. Compatibility getters
are assembled in JS from those primitives and intentionally have no aggregate
native C object equivalent. The combined module now has a parallel runtime
inventory in `web/js/tests/lightusd-loader-api-inventory.json` for all 208
`LightUSDLoaderNative` methods, checked by the combined-WASM variant-overload
test. Only 16 method names overlap exactly with next RenderStream, which does
not establish signature or behavior parity. Matching scene/render families
(mesh/node, animation, camera, light, skeleton, metadata, stats) have typed
next adapters and native resource APIs. Other legacy methods group into
composition/flatten and export operations; asset cache, UUID, and zero-copy
streaming management; loader configuration and progress; MCP; and URDF/image
utilities. Some have next-core C APIs or `NextFlattenSession`/
`NextUSDZConverterNative` counterparts, while cache/MCP and several loader
controls have no next-only counterpart identified yet. The checked
`lightusd-loader-api-classification.json` now assigns each method to one
semantic family, and the combined-WASM inventory test verifies exact coverage.
This completes family classification; per-method signature, behavior, and
replacement decisions remain open before any legacy method is removed.

| Family | Methods | Initial migration direction |
|---|---:|---|
| Render scene queries | 47 | Compare payload and format against typed RenderStream/native render C exports; names alone do not establish parity. |
| Next flatten | 17 | `nextFlattenUSDC` now uses counted input, POD stats, and bounded result/error copies; 16 Embind methods remain. Map the session-based overloads and preserve fetch, remap, variant, and sink behavior separately. |
| Composition and variants | 16 | Compare with next composition/session APIs and preserve arc ordering and selection semantics. |
| Layer export and validation | 15 | Compare against next readers, writers, converter exports, and validation results. |
| Asset resolution and cache | 28 | Reuse next C resolver APIs where equivalent; UUID and cache policy need explicit coverage. |
| Streaming buffers | 15 | No next-only counterpart is identified for the zero-copy and streaming-buffer lifecycle. |
| Loading and diagnostics | 22 | Separate parser/load parity from legacy progress and cancellation behavior. |
| Loader configuration | 33 | Audit each setting against next load options; legacy render-conversion settings may be product-specific. |
| Schema and image utilities | 9 | Check available next schema and image operations individually; no blanket replacement is assumed. |
| MCP | 6 | No next-only counterpart is identified. |

The combined `nextFlattenUSDC(data, lazyArrays)` compatibility method now uses
a counted byte input, a fixed-layout stats record, and bounded output/error
copies. Its JS wrapper keeps the existing result object, handles subarray
offsets and memory64 pointers, and copies the output before releasing native
buffers. This is the first of the 17 flatten-family methods moved off Embind;
the checked classification records the remaining 16 as pending. The combined
flatten regression passes on wasm32 and memory64, including reloading the
output and reporting invalid input.
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
