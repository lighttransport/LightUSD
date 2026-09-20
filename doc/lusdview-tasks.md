# lusdview / lusdrender active tasks

This file tracks unfinished renderer work only. Completed implementation history,
old audit notes, machine-specific run logs, and superseded plans belong in Git
history and are intentionally omitted here.

Updated 2026-09-20.

## Build performance

### Reduce cold MaterialX graph compilation time

The graph compiler now has independently compiled lowering and packing phases:
`lightrt_mtlx_graph_compile.cc` lowers the MaterialX JSON/closure graph, while
`lightrt_mtlx_graph_phases.cc` packs runtime nodes, routes OpenPBR outputs, and
performs dependency ordering. The lowering-only TU uses source-local `-O0` in
Release and RelWithDebInfo builds to avoid pathological GCC IPA/PTA time; the
split packing phase now uses the target's normal optimization level. The
packing phase also reserves expanded JSON node storage and uses reserved hash
tables for node IDs and duplicate suppression.

Measured on Linux with `CCACHE_DISABLE=1`, `/usr/bin/time`, and the existing
source-local `-O1` override (wall seconds / peak RSS in KiB):

| Compiler/configuration | Lowering phase | Packing phase | Combined objects |
| --- | ---: | ---: | ---: |
| GCC Release | 82.4 / 873,868 | 3.1 / 299,236 | 85.5 s |
| GCC RelWithDebInfo | 107.7 / 1,418,520 | 4.3 / 344,776 | 112.0 s |
| Clang Release | 39.2 / 565,532 | 4.1 / 215,036 | 43.3 s |
| Clang RelWithDebInfo | 73.6 / 1,579,000 | 5.0 / 234,448 | 78.6 s |

The measurements show that closure lowering remains the cold-build bottleneck;
edits to output packing/topological ordering now rebuild only the 3–5 second
phase. Linking remains below one second in the existing lusdview test target.

After applying the phase-specific optimization policy, the new cold Release
measurements are:

| Compiler | Lowering (`-O0`) | Packing (target `-O3`) | Combined objects |
| --- | ---: | ---: | ---: |
| GCC | 12.7 / 644,252 | 4.7 / 329,692 | 17.3 s |
| Clang | 7.1 / 322,408 | 4.9 / 217,856 | 12.0 s |

These are direct compiler invocations with ccache disabled; linking is reported
separately and remains below one second. The five CPU-only MaterialX bridge,
evaluation, connection, and `lusdrender` graph tests complete together in 0.27
seconds with a 121,856 KiB peak RSS, so the lowering optimization change does
not create a measurable test-time regression.

Future work:

- [x] Reduce a cold graph-compiler build substantially below the current
  approximately 71-second baseline without changing graph behavior. GCC
  Release lowering now compiles in 12.7 seconds at 644,252 KiB peak RSS; the
  normally optimized packing phase compiles in 4.7 seconds at 329,692 KiB.
- [x] Break `CompileMaterialXGraphRuntime` into independently compiled,
  testable lowering and output-packing phases. Closure lowering remains in the
  first phase so the next optimization can be isolated without changing the
  graph ABI.
- [x] Replace avoidable allocation churn in the packed ABI phase with reserved
  JSON storage and reserved hash tables. JSON parsing and C++ scene ownership
  stay at the C++ boundary.
- [x] Re-measure GCC and Clang Release and RelWithDebInfo builds with ccache
  disabled, recording per-object wall time and peak memory above.
- [x] Re-evaluate the source-local `-O1` override after decomposition. Packing
  returned to the normal target optimization level. Lowering moves to `-O0`:
  its measured `-O1` cost remained 82.4 seconds and 873,868 KiB, while `-O0`
  reduces it to 12.7 seconds and 644,252 KiB.

Acceptance criteria:

- MaterialX bridge, graph-connection, graph-evaluation, and lusdrender CPU graph
  tests remain green.
- Generated graph records and rendered reference pixels remain unchanged.
- Ordinary edits to Vulkan rendering, bridge orchestration, or GPU packing do
  not rebuild the graph compiler.
- Documentation reports compilation and linking separately; abbreviated Ninja
  progress output is not treated as timing evidence.

## Incremental scene updates

- [x] Add a transactional DrawScene/GPU update path for revision-contiguous
  transform and mesh-topology edits. Compatible replacements preserve renderer
  mesh slots and upload changed vertex streams, world matrices, or fully staged
  replacement meshes; all validation
  completes before the first renderer mutation. GL validates the complete slot
  batch before committing; Vulkan maps every affected allocation before any
  write, so a failure leaves the displayed revision intact. Retain the streaming
  converter for initial loads and very large geometry.
- [x] Preserve stable renderer resource IDs across compatible payload, variant,
  and dependency-layer edits, with transactional rollback on failed updates.
- [x] Aggregate consecutive variant and payload edits into a single change set
  rooted at the displayed revision, so a multi-edit UI action can use the
  incremental path when its combined changes are otherwise compatible.
- [x] Add focused tests proving unchanged geometry, materials, and textures are
  neither reconverted nor re-uploaded. The RenderSession test checks the
  converted/upsert counts, retained copy-on-write mesh allocation, and zero
  material, texture, and image upserts for a geometry-only edit; planner and
  GL/Vulkan tests verify the matching backend slots are omitted.
- [x] Update pure material edits in place when material and texture slot catalogs
  remain stable. Only material slots whose prim path contains the reported
  shader/material change are repacked; mesh and texture allocations are retained.
- [x] Connect the next viewer loader to `tydra::next::RenderSession` and commit
  prepared revisions through a `SceneUpdateSink` whose final step is the atomic
  DrawScene/GPU transaction. Preparation runs on the loader worker; a rejected
  sink leaves both the RenderSession and displayed GPU revision unchanged.
- [x] Provide explicit `Prepare`, `Commit`, and `Abort` semantics for retained
  render updates. Prepared candidates are retryable after sink rejection and
  rejected as stale after a competing revision commits.

The retained `tydra::next::RenderSession` foundation selectively converts
geometry-only topology/primvar edits and has a focused copy-on-write test that
proves an unchanged mesh keeps its allocation and emits no upsert. Lusdview now
uses the published `StageChangeSet` to retain compatible GPU mesh slots, with a
focused planner test covering unchanged slots, changed vertex/world streams,
stale revisions, structural flags, and topology replacement. A headless Vulkan
MCP regression switches variants across different topology, mesh addition and
removal, and a material color edit, requiring the real Vulkan transactions and
an unchanged mesh slot throughout. A second MCP regression edits a referenced
sublayer on disk, reloads only that retained layer, and requires its compatible
vertex change to commit without rebuilding the GPU scene. Ordinary 2D texture replacements now stage
every GL/Vulkan allocation before publishing any changed slot and retain a
payload fingerprint after CPU pixels are released. Deformable meshes also use
the vertex/transform transaction while their joint, influence, and morph layout
is unchanged. Same-count instance-transform edits update only their mapped GL/Vulkan
instance slice and retain every resource ID. Same-count displayColor and
displayOpacity edits share that transaction on GL and Vulkan. Instance-count and
color/opacity presence-layout changes atomically replace only the affected stable
mesh slot; Vulkan retires the old immutable MDI commands and moves the replacement
to owned per-mesh buffers. Opacity edits crossing the opaque/translucent boundary
use the same slot replacement so the draw pipeline remains correct. UDIM/Ptex and
deform-layout replacements still take the full-scene fallback.

Every accepted incremental commit logs backend transaction time and estimated
uploaded MiB, making retained-slot regressions visible in ordinary viewer test
logs without enabling a profiler. The viewer bounds the additional prepared
RenderScene to 256 MiB of estimated DrawScene geometry by default; set
`LUSDVIEW_RENDER_SESSION_MAX_MB` to tune the bound or `0` to keep the direct
DrawScene planner for oversized scenes.

## Camera completeness

- [x] Add thin-lens depth of field to GL and Vulkan raster. Vulkan ray query and
  the shared CUDA/HIP tracer already support it.
- [x] Implement shutter/motion blur consistently across raster and RT backends.
  The shared authored-shutter contract and deterministic midpoint schedule are
  in place. Headless CPU, CUDA, and HIP evaluate next-stage camera transforms,
  object transforms, and deformation at every shutter sample, rebuild their
  acceleration structures, and average in linear light. Their regressions cover
  independent geometry and camera motion. Native paused GL/Vulkan raster now
  consumes the same midpoint schedule one pose per frame, averages in linear
  light, holds the completed image, and reports segment coverage for both
  loaders; legacy camera xforms are evaluated directly from the retained Stage.
  The dependency-free `render-motion.py` companion provides the equivalent export
  workflow and disables native sampling in its per-pose subprocesses. Vulkan RT
  now keeps one progressive accumulation while cycling the midpoint poses and
  reports segment coverage; the focused Vulkan regression verifies the
  accumulated sample count and deformation path. Interactive CUDA/HIP accumulation now
  cycles authored camera transforms, optics, object transforms, and deformation
  across shutter segments using in-place BVH refits; it resets when its base
  view or shutter contract changes and reports camera/scene segment coverage.
- [x] Implement stereo rendering. `--stereo` now provides loader-parity pair
  resolution, same-parent preference, ambiguity diagnostics, automatic left-eye
  preview selection, and report records. Native headless GL/Vulkan screenshots
  render and compose both eyes side by side while preserving per-eye pose, lens,
  shutter, and clipping state. Windowed GL/Vulkan live viewports compose both
  eyes at display resolution, with legacy/next coverage. The dependency-free
  `render-stereo.py` companion extends that workflow to external CPU/CUDA/HIP
  tracing modes.
- [x] Apply authored camera clipping planes in GL and Vulkan raster. Both
  backends consume the first eight world-space plane equations, including the
  ordinary, tessellated, and PointInstancer mesh paths; the viewer warns when
  an authored camera exceeds that portable bound. The GL/Vulkan screenshot
  regression checks that the same plane retains the same half-space.
- [x] Keep default and legacy loader camera records equivalent and extend image
  regressions for every newly supported path. The record test covers perspective,
  orthographic, lens, shutter, exposure, window policy, and clipping data; the
  clipping screenshot test requires equivalent default/legacy output on GL and
  Vulkan.

## Lighting completeness

- [x] Implement finite Sphere, Disk, Rect, and Cylinder area-light sampling in
  GL and Vulkan raster. Both backends use the shared packed light record and a
  deterministic finite-sample pattern; shadow coverage and light-link masks are
  tested alongside the legacy/next loader parity path. The Vulkan RT area-light
  gate now probes for a hardware-capable adapter before entering the expensive
  ray-query path, so software Vulkan runs skip instead of timing out.
- [x] Implement dedicated GeometryLight and emissive-mesh raster sampling.
  Resolved source meshes now provide eight deterministic, area-stratified
  world-space triangle samples to both raster backends. Unresolved or empty
  sources are omitted with an explicit diagnostic instead of degrading to a
  point emitter. RT continues to sample the complete triangle range.
  `lusdview-raster-geometry-light` locks the GL/Vulkan image response and the
  focused lighting test covers sample preparation and packing.
- [x] Implement PortalLight rectangle sampling and IES evaluation in raster.
  PortalLight now uses the authored width, height, transform axes, and the same
  deterministic eight-point rectangle pattern in every GL/Vulkan raster draw
  path. The shared 6x4 IES payload is evaluated by both raster backends.
- [x] Keep structured path-qualified diagnostics for those light features.
  Viewer reports now include authored GeometryLight/PortalLight/IES and
  resolved emissive-mesh counts while unresolved targets and profiles retain
  their original path-qualified skipped messages.
- [x] Extend deterministic cross-backend image tests for the finite-light and
  collection paths. `lusdview-raster-multilight`, `lusdview-raster-shadow-map`,
  and the area-light RT gates cover GL/Vulkan images, shadows, and authored
  collection membership; hardware-dependent RT cases remain explicit skips.

## Verification

Prefer the Ninja build tree and run the focused renderer gates before the full
native suite:

```sh
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_GUI_VIEWER=ON
cmake --build build_ninja -j16
ctest --test-dir build_ninja -R 'lusdview|tool-lusdrender' --output-on-failure
```

GPU-dependent tests require a usable hardware device and the documented
environment in `doc/lusdview.md`. A missing backend or external corpus is an
explicit skip, not proof of rendering parity.
