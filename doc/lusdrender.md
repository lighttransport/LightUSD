# lusdrender — GPU backend testing (Vulkan / Direct3D 11 / HIP)

`lusdrender` (CPU preview ray-tracer, see
[`tools/lusdrender/README.md`](../tools/lusdrender/README.md)) has several optional
**GPU compute backends** that traverse the same LightRT BVH:

- **Vulkan** (`-vk` / `-vkr`) — `src/external/lightrt/lightrt_c_vk.c` +
  `lightrt_vkew.c`, driven by `lusdr_vulkan.cc`. Runtime-loaded via vkew (no
  Vulkan SDK to build), on by default (`-DLUSDRENDER_WITH_VULKAN=ON`). `-vkr`
  uses hardware ray tracing when present, else falls back to compute trace.
- **HIP/ROCm** (`-hip`) — `src/external/lightrt/lightrt_c_hip.c` +
  `src/external/hipew/hipew.c`, driven by `lusdr_hip.cc`. Runtime-loaded via
  hipew (opens `libamdhip64` + `libhiprtc`; no ROCm SDK to build), on by default
  (`-DLUSDRENDER_WITH_HIP=ON`, non-Windows). The trace kernel
  (`src/external/lightrt/hip/trace_bvh_hip.h`) is a **direct port of
  `trace_bvh.comp`** compiled at runtime with hiprtc, so it traverses the
  serialized LRTS BVH bit-for-bit identically to the Vulkan compute path
  (`-hip` ≈ `-vk`). Verified on AMD Radeon RX 9070 XT (gfx1201).

## Analytic geometry regression

The next-loader CPU and GPU paths tessellate Cube, Sphere, Cone, Cylinder,
Capsule, and Plane primitives through the shared Tydra converter. Schema defaults,
time-sampled dimensions, generated normals/UVs, payload transforms, and native
and point instancing are preserved. Animated dimensions invalidate CPU geometry
between frames.

Run the bounded regression without loading external large scenes:

```sh
ctest --test-dir build_ninja -R '^tool-lusdrender-analytic-primitives$' --output-on-failure
python3 tools/lusdrender/check_analytic_primitives.py \
  build_ninja/lusdview_lightrt_c_build/lusdrender /tmp/analytic-gpu \
  --backends vk,vkr --require-hardware
```

The hardware check requires GPU device access and runs without a display.
Generated UV texture sampling is checked on CPU and compute. The descriptor
ray-query path currently drops PreviewSurface texture connections, including on
authored Meshes; its analytic regression checks geometry and constant materials
but does not assert textured color parity.

## Shared lusdview RT materials (Vulkan, CUDA, and HIP)

`-cuda`, `-vkr`, and the default `-hip` mode use the GUI-free RT core shared with
`lusdview`. The streamed lusdrender scene is adapted directly to `DrawScene`,
including semantic textures, UV transforms, alpha modes, the canonical OpenPBR
parameter block, and retained MaterialX node-graph JSON. Arbitrary MaterialX
image nodes are resolved relative to the input layer and baked into the shared
texture table. Authored point, sphere, rect, disk, cylinder, distant, and dome
lights are converted to the same shared RT light records; a neutral distant key
is used only when the stage has no authored light. Use `-gpuShade cpu` with `-hip` to select the older LightRT
shade-after-hit path explicitly.

The same CLI can exercise preview and production modes without a display:

```sh
lusdrender scene.usda preview.png -cuda -gpuShade preview -autoframe
lusdrender scene.usda vk-final.png -vkr --path-trace --pt-samples 256 -autoframe
lusdrender scene.usda final.png -cuda --path-trace --pt-quality final \
  --pt-samples 1024 --pt-max-depth 12 -autoframe
lusdrender scene.usda hip.png -hip --path-trace --pt-samples 256 -autoframe
```

CUDA, HIP, and Vulkan runtimes are loaded dynamically. The CTest GPU regression exits
with the standard skip code when no suitable device is present. Vulkan
ray-query uses descriptor-backed geometry, canonical MaterialX graph IR,
texture data, OpenPBR constants, and lights for both `-gpuShade preview` and
`--path-trace`; it needs no window system. `-vk` and `-gpuShade cpu` retain the
older LightRT compute/shade-after-hit reference paths.

The default `next` material converter retains connected MaterialX NodeGraphs
as executable IR rather than reducing them to constant/image fallbacks. The
Vulkan path evaluates the graph once per surface hit, routes 48 canonical and
advanced OpenPBR inputs (including live anisotropy, thin-film, dispersion,
transmission-medium, coat-IOR, subsurface-scale, and volume controls), and
supports packed multi-tile UDIM textures. Its production
integrator includes rotated anisotropic GGX, coat/sheen/subsurface and
transmission events, thin-film response, hero-wavelength dispersion, bounded
homogeneous volume attenuation/emission plus Henyey-Greenstein scattering,
analytic and emissive-triangle lights, and dome-environment MIS. Run the focused
headless gate with `ctest --test-dir build_ninja -R
tool-lusdrender-gpu-render --output-on-failure`; its graph fixture asserts that
non-empty executable graph topology reached the descriptor renderer.
The shared CUDA and HIP production path consumes the same 48-route graph ABI,
including the advanced and volume controls, and performs homogeneous
free-flight/Henyey-Greenstein scattering. The same headless gate renders the
MaterialX graph fixture through each available shared backend and checks that
non-empty executable graph topology reached it.
- **Direct3D 11** (`-d3d`) — `src/external/lightrt/lightrt_c_d3d11.cpp`, driven
  by `lusdr_d3d.cc`. Windows-only, on by default
  (`-DLUSDRENDER_WITH_D3D11=ON`); d3d11/dxgi/d3dcompiler ship with the OS, so no
  SDK is needed. Its compute shader (`d3d/shaders/trace_bvh.hlsl`) is
  **decompiled from the Vulkan SPIR-V with SPIRV-Cross**, so it traverses the
  BVH bit-for-bit identically; it just runs through the (much more mature on
  older AMD cards) D3D driver. Unlike the Vulkan helper it batches the whole
  frame's rays into **one** dispatch.

```sh
# CPU reference (always correct):
lusdrender models/suzanne-pbr.usda cpu.png -rtPreview -w 320 -height 240 -autoframe
# Vulkan compute trace:
lusdrender models/suzanne-pbr.usda vk.png  -vk  -w 320 -height 240 -autoframe
# Vulkan hardware ray query (RDNA2+; else falls back to compute trace):
lusdrender models/suzanne-pbr.usda vkr.png -vkr -w 320 -height 240 -autoframe
# GPU material preview (the default); use `-gpuShade cpu` for flat reference.
lusdrender models/suzanne-pbr.usda vk-preview.png -vkr -gpuShade preview \
  -w 320 -height 240 -autoframe
# Large-scene preset for Vulkan batch/validation runs. Explicit backend, LOD,
# camera, and memory flags still win; texture resize/compression flags are not
# changed by the profile.
lusdrender /path/to/island.usda island.png -largeSceneProfile island \
  -w 960 -height 540
# HIP/ROCm compute trace (AMD; kernel compiled at runtime via hiprtc):
lusdrender models/suzanne-pbr.usda hip.png -hip -w 320 -height 240 -autoframe
# Direct3D 11 compute trace (Windows):
lusdrender models/suzanne-pbr.usda d3d.png -d3d -w 320 -height 240 -autoframe
```

A correct GPU image must match the `-rtPreview` reference (same framed mesh).
The startup log prints the chosen device + path, e.g. `backend: LightRT VK
(compute trace)` / `LightRT D3D11 (compute trace, N rays in 1 dispatch)`.
GPU traversal is batched; the CPU-side BVH build and shade-after-hit stage honor
`-threads N` (`0` = hardware concurrency). On Vulkan hardware ray query (`-vkr`
with ray-query support), lusdrender skips the CPU LightRT BVH build and builds
the Vulkan acceleration structure directly from the flattened indexed geometry.
Successful `-vkr` ray-query renders log `ray_query, indexed Vulkan AS, CPU BVH
skipped`, which the GPU regression script checks. If the driver reports ray-query
support but AS build/trace fails, `-vkr` logs `compute trace, ray_query fallback`
and renders through the Vulkan compute path instead of returning a blank/failure.
Set `LUSDR_FORCE_VKR_FALLBACK=1` to exercise that fallback path in local tests.

## Fixed (was: "GPU backends mis-render") — 2026-06-28

The `-vk` / `-vkr` (and `-d3d`) backends previously mis-rendered — `-vk` a
**sparse, holey silhouette**, `-vkr` **fully blank** — while the CPU `-rtPreview`
was correct. This was first suspected to be an AMD **Radeon RX 570** / amdvlk
GCN/Polaris driver bug. Retesting on an NVIDIA **GeForce RTX 5060 Ti** (Linux,
driver `610.43.02`, Vulkan 1.4) reproduced the *exact same* symptoms — which
ruled out a vendor driver and pinned the fault to **CPU-side geometry/setup in
`lusdrender` itself**, not the GPU trace. Root causes (all now fixed):

1. **Polygons were not triangulated.** The GPU geometry collector in
   `lusdrender.cc` copied `faceVertexIndices` verbatim and the backends chunked
   it into groups of three. Suzanne is **468 quads + 32 tris**, so chunking the
   1968 indices by 3 produced 656 scrambled triangles instead of the 968 a proper
   fan-triangulation yields — hence the holes/sparseness. Fixed by triangulating
   with `faceVertexCounts` once, at collection (so `-vk`, `-vkr` **and** `-d3d`
   all get correct geometry).
2. **`-vkr` was fed the wrong vertices.** The ray-query BLAS expects a de-indexed
   triangle soup (9 floats/tri, `VK_INDEX_TYPE_NONE`), but `lusdr_vulkan.cc`
   passed the *indexed* unique-vertex array — over-reading and building garbage
   geometry, so every ray missed → blank. Fixed by expanding the indexed mesh to
   a soup before `lrt_vk_rtx_scene_build`.
3. **One GPU round-trip per pixel** (and, on `-vkr`, an acceleration-structure
   rebuild per pixel) made it minutes-slow at 320×240. Fixed by generating all
   primary rays and tracing the whole frame in **one batched dispatch** (build the
   AS once); 320×240 now finishes in well under a second.
4. **The GPU dispatch used a different camera** (the tilted auto-fit
   `MakeCameraFrame`) than `-rtPreview` (the `MakeUsdRecordCamera` record camera),
   so the same scene framed differently. Fixed by resolving the camera through the
   same path as `ResolveCameraNext`.

Verified on the NVIDIA RTX 5060 Ti: `-vk` and `-vkr` now render the solid,
correctly-framed Suzanne (968 triangles, same dimensions as `-rtPreview`), and
the two paths agree to the byte (compute trace vs hardware ray query). The
`tool-lusdrender-smoke` ctest now asserts this — matching triangle count vs the
CPU reference, non-blank, matching dimensions, and `-vk` ≈ `-vkr` — see
`tools/lusdrender/check_lusdrender_smoke.py` (`check_vulkan`). The GPU path
flat-shades with geometric normals, so it is darker than the CPU image; that is a
shading-model difference, not a geometry error.

```sh
# A correct GPU image now matches the -rtPreview framing/silhouette:
lusdrender scene.usda cpu.png -rtPreview -w 320 -height 240 -autoframe
lusdrender scene.usda vk.png  -vk        -w 320 -height 240 -autoframe
lusdrender scene.usda vkr.png -vkr       -w 320 -height 240 -autoframe
```

### Debugging Vulkan with the Khronos validation layers

If a future change regresses the Vulkan path, the **Khronos validation layers**
give a definitive reason. Build/run them with the helper added for lusdview
(`scripts/setup-vulkan-validation.ps1 -FromSource` on Windows, or build from
source on Linux — see [`doc/lusdview.md`](lusdview.md)); the layer works for any
Vulkan app:

```sh
VK_LAYER_PATH=/path/to/Vulkan-ValidationLayers/build/layers \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  lusdrender scene.usda vk.png -vk -w 320 -height 240 -autoframe 2>&1 |
  grep -E 'VUID|Validation'
```

Note the layer output (and lusdrender's own diagnostics) goes to stderr/stdout;
capture both streams.

## Per-instance LOD on the `-vk` / `-vkr` GPU backends (flatten-side)

`-rtLod` works on the `-vk` / `-vkr` / `-d3d` / `-hip` GPU backends too, applied
**flatten-side**: the same `lusdr_rt_lod` classifier runs once over the already
world-space-flattened `geos` (in `lusdrender.cc`, right after the camera resolves,
before the GPU dispatch). Each `geos[i]` is one world-space mesh placement, so its
world AABB is the classifier input (identity `o2w` + the world AABB as the
prototype bounds):

- **Cull** → drop the placement from the flat soup (fewer triangles to trace).
- **Proxy** → replace its triangles with an axis-aligned box on its world AABB
  (`BoxFitO2W`, a 12-triangle cube) — collapses a dense distant mesh to 12 tris.
- **Full** → keep the real triangles.

Opt-in via `-rtLod` (byte-identical when off); same tunables as the CPU path
(`-rtLodFullPx` / `-rtLodCullPx` / `-rtLodNoProxy` / `-rtLodFrustumCull`). With
`-stats` it logs `[rt-lod] flatten-side: full=… proxy=… culled=…`. Verified on a
120×(10×10-grid) receding scene: 24000 tris → 1440 (all-Proxy) → 624 (Proxy+Cull);
proxy renders the same silhouette coverage as Full.

**Limitation vs the CPU two-level path.** This is flatten-side, not a GPU TLAS, so:

- No per-prototype geometry/memory sharing — Full placements still cost their full
  triangle count in one flat BLAS (the win is *fewer/cheaper* placements, not
  shared BLAS memory).
- It only sees what the GPU collector emits. `CollectRTPreviewMeshesNext` now
  **expands both `PointInstancer` and scenegraph (`instanceable`) native
  instances** in place (`expand_instancers=true` on the GPU flatten caller; see
  *Instancing on the GPU backends* below), so instanced geometry renders and LODs
  here. Flatten LOD therefore covers many-separate-`Mesh` scenes (Caldera's 10k+
  districts), PointInstancer scatters, and native-instanced subtrees alike.

### Instancing on the GPU backends (flatten-side expansion)

`CollectRTPreviewMeshesNext(..., expand_instancers=true)` — set by the GPU flatten
caller in `lusdrender.cc` — expands instancing into world-space `MeshJobNext`
placements instead of stopping at it:

- **`UsdGeomPointInstancer`** (`ExpandPointInstancerJobsNext`): for every visible
  instance (`invisibleIds` skipped) compose `prototype-local · InstanceTRS(pos,
  orient,scale) · instancer-world` and bake the prototype's meshes at that
  transform. Mirrors the CPU two-level `CollectPointInstancer` math (`InstanceTRS`,
  same prototype resolution by child-name then stage path).
- **Scenegraph native instances** (`ExpandNativeInstanceJobsNext`): a prim with
  `instance_prototype` meta is a proxy; bake the prototype's geometry
  (`CollectExpandedProtoJobsNext`, prototype-local) at the proxy's world transform.
  Mirrors the CPU `CollectSceneSplit` native-instance branch.

Both share `CollectExpandedProtoJobsNext` (prototype-local collection with nested
instancers expanded recursively). By default these expansions **flatten** to the
single GPU BLAS (`expand_instancers` defaults to **false**, so the two-level proto
collectors `CollectProtoJobs` stay byte-identical), which duplicates a prototype's
geometry per instance. For instanced scenes the `-vkInstanced` two-level path below
shares that geometry instead.

### `-vkInstanced`: true two-level GPU TLAS (per-prototype BLAS sharing)

`-vkInstanced` (implies `-vkr`) builds a genuine two-level acceleration structure:
**one BLAS per prototype** (geometry stored on the device ONCE) and **one TLAS
instance per placement** (the same prototype BLAS referenced under a per-instance
transform). Instanced geometry costs one BLAS, not one-per-instance — the memory
the flat path could not save.

How it reuses the existing trace **with no shader/SPIR-V change**: LightRT's
ray-query backend already builds a TOP_LEVEL TLAS over BLAS and the shader already
recovers the hit id as `instanceId*tri_chunk + primitiveIndex`. The new C API
`lrt_vk_rtx_scene_build_instanced` (`src/external/lightrt/lightrt_c_vk.{h,c}`) sets
`tri_chunk` to the max prototype triangle count and builds one BLAS per prototype +
a TLAS instance per placement (real transforms). The returned `prim_id` then
decodes as `instance = prim_id / stride`, `prototypeLocalTri = prim_id % stride`.
lusdrender groups the expanded `mesh_jobs` by source prim path into shared
prototypes + placements (`TryRunInstancedVk` in `lusdrender.cc`), and the CPU hit
resolver (`ShadeAndWriteImageInstanced` in `lusdr_gpu_common.cc`) decodes the
instance, looks up the prototype-local triangle, and transforms the object-space
normal by that instance's normal matrix (cofactor of the 3×4, so non-uniform scale
is exact).

A prototype larger than the per-BLAS chunk limit (8M triangles; overridable via
`LUSDR_INST_CHUNK_TRIS` for testing) is **chunk-split** into sub-prototypes of ≤
chunk triangles (vertices remapped per chunk, shading slices carried along), and a
placement of it is emitted once per sub-proto. Each sub-proto is just a smaller
prototype, so the `pid` encoding and hit decode are unchanged; this works around
drivers (RADV) whose AS build-size query overflows for very large single-BLAS
builds. Verified pixel-identical (MAD 0.000) to the unsplit render by forcing a
small chunk on the demo prototype (1200 tris → 24 sub-protos, 7200 instances, still
1200 unique tris stored).

Opt-in (`-vkInstanced`), falls back to the flat path when ray query is unavailable,
there are no shareable instances, or `ninsts*stride` would overflow the 32-bit
`prim_id` encoding (`stride` = max sub-prototype triangle count, so a huge prototype
chunked small still caps total instances). Validated **pixel-identical** to the flat
`-vkr` path (luminance MAD 0.000/255, silhouette IoU 1.000) on a 200×(800-tri)
PointInstancer (BLAS stores 800 tris vs 160 000 flattened — 200× smaller), a
4-instance orient/non-uniform-scale PointInstancer, and a 3-tree `instanceable`
scene. `UsdPreviewSurface inputs:displacement` IS applied (per prototype, in object
space — `ExtractProtoGeo` resolves the bound material's constant + scalar texture
and offsets along the area-weighted smooth normal, like the flat extractor);
verified pixel-identical to the flat path with displacement on, and the displaced
silhouette grows as expected.

#### `-vkInstanced` + `-rtLod`: per-instance LOD on the two-level TLAS

`-rtLod` composes with `-vkInstanced` and applies the `lusdr_rt_lod` Full/Proxy/Cull
selection at the two-level build — the structure LOD is actually designed for (real
per-instance TLAS selection, not the flatten-side approximation). After the camera
resolves, `TryRunInstancedVk` classifies each placement from its prototype's local
AABB + transform: **Cull** drops the instance (absent from the TLAS), **Proxy**
points it at a shared unit-box prototype (built once, 12 tris) via a `BoxFitO2W`
transform onto the prototype's local AABB (a near-zero AABB axis is padded so a
planar/linear prototype still yields a box that traverses — a zero-thickness box
collapses to degenerate triangles and a per-proxy BLAS, unlike a flat soup, then
returns no hits), **Full** keeps the real prototype BLAS. Same knobs as the CPU /
flatten paths (`-rtLodFullPx`/`CullPx`/`NoProxy`/`FrustumCull`); `-stats` logs
`[rt-lod] two-level: full/proxy/culled`. The box BLAS is stored ONCE and shared by
every proxy, so LOD keeps the memory win. Validated on a 200-instance scene: each of
Full-only, Proxy-only (proxy box pixel-covers the mesh, IoU 1.000), Cull-only, and a
Proxy+Cull mix; the 3D-cube proxy matches the Full silhouette exactly. If every
instance culls, it falls back to the flat path.

The GPU-instancing path is now feature-complete: render PointInstancer + native
instances, per-prototype BLAS sharing, per-instance LOD (flatten-side and
two-level), displacement, and >8M-triangle prototype chunking — all reusing
LightRT's existing ray-query shader (no SPIR-V change).
