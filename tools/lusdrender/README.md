# lusdrender

CPU preview ray-tracer for USD scenes (LightRT BVH + the `next` lazy USDC
loader). Renders large, reference- and instance-composed scenes directly, within
a bounded memory budget.

## Quick start

```sh
lusdrender scene.usd out.png -rtPreview -w 960 -height 540 -autoframe
```

### Build and link time

On Linux, `lusdrender` automatically uses the first available fast linker in
the order mold, LLD, then gold. This is target-local—the rest of LightUSD keeps
the linker selected by the parent build. CMake prints the selected linker while
configuring. Versioned LLD installations are supported even when the system's
unversioned alternative is absent.

Do not infer the active phase from Ninja's abbreviated progress line. A measured
three-minute rebuild was GCC optimizing `lightrt_mtlx_bridge.cc`, while the
actual GNU BFD link took less than a second. The MaterialX graph compiler and
flat GPU ABI packing now use separate translation units, and the graph compiler
uses source-local `-O0` in Release/RelWithDebInfo to avoid GCC's pathological
IPA/PTA cost. On the reference Linux build this reduced GCC Release lowering
from 82.4 to 12.7 seconds, while the independently compiled packing phase uses
the target's normal optimization level and takes 4.7 seconds. Use
`/usr/bin/time -v cmake --build ...` plus Ninja's `.ninja_log` when diagnosing
a regression.

Disable the target-local selection when diagnosing a linker/toolchain issue:

```sh
cmake -S . -B build_ninja -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUSDRENDER_USE_FAST_LINKER=OFF
```

Common flags:

| flag | meaning |
|------|---------|
| `-rtPreview` | ray-traced preview (the `next` loader; default for USDC) |
| `-vk` / `-vkr` / `-d3d` | GPU backends — Vulkan compute / Vulkan ray query / Direct3D 11 compute — see [`doc/lusdrender.md`](../../doc/lusdrender.md) for status + testing |
| `-vkDevice N` | Select a Vulkan physical-device index for explicit multi-GPU validation (`-1`/omitted means automatic selection) |
| `-cuda` / `-hip` | Headless RT using the GUI-free lusdview core, including packed MaterialX/OpenPBR preview shading |
| `--path-trace` | Production path tracing on `-vkr`/`-cuda`/`-hip`; tune with `--pt-quality`, `--pt-samples`, `--pt-max-depth`, and `--pt-rr-depth` |
| `-largeSceneProfile caldera\|island\|alab` | Vulkan large-scene preset over backend/LOD/shared memory policy; explicit flags win |
| `-w N -height N` | image size (`-height` omitted → from camera aspect) |
| `-autoframe` | usdrecord-style auto camera framing |
| `-camera <path>` | render through a named `UsdGeomCamera` |
| `-mask <prim,...>` | restrict to these prim subtrees; with the `next` loader, defer payloads that cannot intersect the selected subtrees |

When `-mask` is used with the default `next` loader, payloads whose authored
prim path is outside (and does not contain) the requested subtree are left
deferred during composition. This keeps a per-element render from first
materializing the whole payload set; a selected payload may still contain its
own nested payloads. The loader reports the number of deferred payloads.
| `-complexity low\|med\|high\|veryhigh` | subdivision preset |
| `-smooth` | interpolate authored normals (smooth shading) |
| `-threads N` | cap build and CPU shade-after-hit worker threads (`0` = auto) |
| `-displaceScale <f>` | `UsdPreviewSurface` displacement multiplier (default 1.0; `-noDisplace` disables) |
| `-materialResolver legacy\|tydra-next\|compare` | next-loader material resolver. `tydra-next` is the default shared path; `legacy` keeps the hand-rolled compatibility path; `compare` renders legacy and reports resolver field differences. |
| `-materialShading legacy\|lightrt-bsdf` | CPU shading path. `lightrt-bsdf` is experimental and evaluates direct-light/headlight response through the shared LightRT OpenPBR BSDF; `legacy` remains the default. |
| `-maxMem <GiB>` | memory cap override (automatic policy reserves 2 GiB on the 32 GiB target) |
| `-texMaxSize <N>` | longest texture edge cap; derived automatically from the host/VRAM budget unless explicitly set |
| `-texBudgetMb <N>` | decoded texture residency budget; derived automatically from the host/VRAM budget unless explicitly set |
| `-stats` | print mesh/triangle/memory/timing stats |

The headless MaterialX/OpenPBR parity regression runs the production Vulkan
ray-query, CUDA, and HIP paths when they are available:

```sh
ctest --test-dir build_ninja \
  -R '^tool-lusdrender-materialx-openpbr-parity$' --output-on-failure
```

It checks PreviewSurface/OpenPBR/standard_surface color semantics, executable
MaterialX graph retention, a typed `color4` to `color3` swizzle result, tight
CUDA/HIP image parity, `atan` plus spec-matched `screen`, `overlay`, `burn`, and
`dodge` blend execution, and
exposure-independent Vulkan/shared-backend chromaticity parity. A committed
six-panel OpenPBR visual reference separately locks base, coat, transmission,
subsurface, emission, and opacity output for the Vulkan and shared CUDA/HIP
integrators. To turn missing hardware into a failure for a GPU validation host,
set `LUSDR_PARITY_REQUIRE_BACKENDS=vkr,cuda,hip`. Set
`LUSDR_PARITY_REQUIRE_HARDWARE=1` to require NVIDIA identity on CUDA and AMD
identity on HIP. Multi-GPU Vulkan validation accepts an explicit index/vendor
matrix such as `LUSDR_PARITY_VULKAN_DEVICES=0:NVIDIA,1:AMD`; every listed
device must expose hardware ray query and pass the semantic grid.

Spatial graph coverage includes standard `ramplr`, `ramptb`, `splitlr`, and
`splittb` nodes. The ramp fixture checks red-to-blue horizontal variation and an
independent green vertical gradient, including implicit hit UV when no texcoord
input is connected. The split fixture drives both axes through a connected
MaterialX texcoord node and verifies the resulting image quadrants.

Color-adjust coverage follows MaterialX semantics for `saturate`: `amount`
mixes the input away from or toward its luminance, rather than acting as a
shader-language shorthand for clamping values to `[0, 1]`. The headless
blend-chain fixture executes this operation after screen, overlay, burn, and
dodge on every requested backend.

High-arity `ramp4` and `switch` nodes are lowered at graph compile time into
bounded extract/mix and equality-select primitives. This retains connected
inputs without expanding the fixed GPU material ABI. The spatial fixture uses
a connected texcoord-driven `ramp4`, while the color fixture selects through a
lowered switch on all hardware backends. Default CPU lusdrender separately runs
a deep 16-node graph regression; its evaluator and NodeGraph traversal share a
64-level safety bound with the runtime node limit.

CUDA cache transitions can be made part of the same headless gate. Point
`XDG_CACHE_HOME` at an empty temporary directory and set
`LUSDR_PARITY_CUDA_CACHE_EXPECT=cold` for the first run; repeat with `warm` and
the same directory. The cold run requires an NVRTC cache miss followed by a
persisted PTX entry, while the warm run requires an actual cache hit. Both also
run the typed graph-output checks. Include `cuda` in
`LUSDR_PARITY_REQUIRE_BACKENDS` so an unavailable device cannot turn this gate
into a skip.

The strict hardware wrapper can perform both passes automatically:

```sh
LUSDR_CI_CUDA_CACHE_CYCLE=1 \
  scripts/run-lusdrender-materialx-hardware-matrix.sh build_ninja
```

Texture decode is bounded for ordinary `-rtPreview` runs as well as the named
large-scene profiles. The derived cap applies before material conversion, and
the texture cache releases decoded pixels after upload; explicit texture flags
remain authoritative, including an explicit zero.

Large `ParticleField3DGaussianSplat` fields are built as bounded native ellipse
BVH chunks instead of one GPU allocation. Pure Gaussian scenes use the native
Vulkan ellipse path; stages that also contain meshes, ordinary Points, or
curves use the bounded tessellated
fallback so the splats are not lost when all geometry shares one flat trace. The
native Vulkan BVH is deferred until the scene is known to be pure Gaussian, so
mixed scenes do not build and then discard a second full splat representation.
The default chunk size is 262,144
splats; `LUSDR_GAUSSIAN_CHUNK=N` tunes it for a smaller VRAM budget. CPU RT
tests all chunks directly, while the Vulkan and HIP paths upload/trace them
sequentially, keeps the closest hit per ray, and releases each LightRT BVH as
soon as that reduction completes; only compact per-splat shading metadata stays
resident until image output. Statistics report the retained sample count and
chunk count; a failed chunk reports its range and the tuning knob to reduce it.
For Vulkan/HIP, transformed ellipse inputs are retained as compact per-chunk
arrays and each LightRT BVH is materialized only immediately before its trace;
the input arrays and BVH are then released before the next chunk. This avoids
retaining a CPU BVH for every chunk during load and bounds the acceleration
structure working set to one chunk. The CPU path still builds its chunks during
extraction. Pure Gaussian scenes use the native ellipse path on HIP/ROCm as
well; mixed mesh+splat scenes (and D3D11) use the bounded tessellated fallback,
with SH/color buckets sharing one global `LUSDR_GPU_TRIANGLE_CHUNK` pending
budget rather than each of the 64 quantized buckets becoming a separate
multi-million-triangle allocation.
The OpenUSD float/half property pairs share float-over-half selection and
array-length validation across native and fallback paths.

For GPU mesh previews, next-loader traversal records are also collected and
converted one stage root at a time. This keeps large instancer/job lists from
being retained alongside the world-space geometry waiting for the GPU chunk
upload; the final nearest-hit reduction and material order are unchanged.
After a base BLAS group or mesh prototype is streamed, its consumed job records
and GeomSubset face masks are released before the next prototype build. Curve
job records are released immediately after native curve extraction as well, so
subset-heavy instanced scenes do not retain authored job metadata throughout
the entire BLAS/TLAS build.

The flat next-loader mesh path also splits its native LightRT triangle BVH when
needed. `LUSDR_TRIANGLE_CHUNK=N` sets the triangle limit (default 262,144);
the integrator preserves global material/UV indices while checking every chunk,
and `-stats` reports aggregate chunk node/memory totals.
The Vulkan flat GPU path uses the same sequential strategy for oversized uploads;
`LUSDR_GPU_TRIANGLE_CHUNK=N` (default 262,144) bounds each GPU BLAS/AS and
reduces the nearest hit across chunks before shading. This avoids requiring the
whole flat scene to fit in an 8-GiB device allocation.
The Direct3D 11 path uses the same chunk limit and nearest-hit reduction, rather
than flattening all source meshes into one long-lived BVH. Consumed source
geometry is released after each chunk is built.
Ordinary `UsdGeomPoints` are read through lazy array views and accumulated into
the same bounded GPU geometry chunks (disc primitives when normals are
authored, sphere primitives otherwise), so large point clouds do not create
full temporary array copies or one mesh/descriptors per point.
HIP/ROCm `-stats` reports the corresponding chunk count, flatten/BVH time, and
trace time to make AMD memory/performance tuning comparable to Vulkan.

UsdVol density grids are capped to a conservative share of the `-maxMem` budget
(64 MiB minimum, 512 MiB maximum). Oversized fields are nearest-sampled to the
bounded voxel count and later fields are skipped once the total is exhausted;
the loader reports the resident/budget MiB. VDB grid data is moved into the
raymarch carrier instead of copied, preventing a second full dense allocation
during extraction.
Multi-field volumes are composited once: `density` controls extinction, while
`temperature` and `emission`/`flame`/`heat` drive spatial blackbody/emissive
shading. CPU rendering samples independently transformed/resolved VDB fields;
the interactive GL/Vulkan paths resample auxiliary fields onto the density
lattice before uploading packed 3D textures.
Inherited UsdShade material bindings are followed through `outputs:volume`.
Constant MaterialX/OpenPBR volume inputs for density, scattering color,
emission color, and emission intensity are honored; connected procedural
inputs resolve through Material/NodeGraph interfaces and MaterialX constant
nodes. Constant add/subtract/multiply/divide/min/max, clamp, and mix graphs are
evaluated without baking. Texture, noise, and other spatial procedural volume
graphs remain unsupported.

For a strict GPU CI gate covering Vulkan ray query on both configured physical
devices, CUDA on NVIDIA, and HIP on AMD, run:

```sh
scripts/run-lusdrender-materialx-hardware-matrix.sh build_ninja
```

The default Vulkan mapping is `0:NVIDIA,1:AMD`; heterogeneous workers can set
`LUSDR_CI_VULKAN_DEVICES` to the detected `INDEX:VENDOR` pairs. The gate rejects
software fallbacks and missing required backends rather than silently skipping.
The vendored TinyVDB reader accepts OpenVDB 1.x through 13.x scalar grids
(`bool`, integer, half, float, and double). Vec3 grids are reduced to vector
magnitude for the single-channel density renderer. ZIP, active-mask, BLOSC/LZ4,
and BLOSC/zstd payloads use lightusd's existing vendored codecs. PointDataGrid
assets are rejected explicitly because they are point payloads, not voxel-valued
UsdVol fields.

Curve point arrays use the same lazy view reader before conversion to the
native LightRT strand representation, so ordinary uncompressed USDC curves do
not hold both a source float array and a second temporary float copy.
Non-instanced round/flat curve strands are also split into bounded native BVHs;
`LUSDR_CURVE_CHUNK=N` sets the segment limit (default 262,144). The CPU
integrator checks every chunk, and Vulkan tessellates each chunk independently
when it needs its triangle upload fallback.
HermiteCurves use their authored `tangents` for cubic tessellation in both the
direct scene and instanced curve-BLAS paths; `LUSDR_HERMITE_SEGMENTS=N` selects
the samples per span (default 8, capped at 1024). Invalid or missing tangents
fall back to the authored control polygon with a diagnostic warning.
The round-curve triangle fallback is shared by Vulkan, HIP/ROCm, and D3D11 and
is paged through the LightRT tessellator in the same bounded triangle chunks;
HIP-only and D3D11-only builds therefore retain curve coverage even when no
analytic curve API is available.

## Composition, instancing, memory

* **Direct composition** — reference/payload/sublayer/variant/instancing arcs are
  resolved in place (full PCP engine), so reference-composed scenes (e.g. Caldera
  prefab stubs) render directly with no external `usdcat --flatten` step.
  Self-contained / pre-flattened inputs skip composition and render
  byte-identically to before.
* **Two-level (instanced) BVH** — native instances are placed by a LightRT TLAS
  with each prototype's geometry built once (BLAS), so a scene with millions of
  *visible* triangles only stores the *unique* prototype geometry.
* **Per-instance view-dependent LOD (`-rtLod`)** — at TLAS build time each mesh
  placement is classified from the resolved camera: distant prototypes collapse to
  a shared unit-box BLAS (box-fit onto the prototype AABB), sub-pixel placements are
  dropped, near ones keep the real BLAS. Parity with the interactive viewer's
  `--rt-lod` (`lusdr_rt_lod.{hh,cc}`). Tunables: `-rtLodFullPx` (promote-to-full
  radius, def 64), `-rtLodCullPx` (drop radius, def 2), `-rtLodNoProxy` (Full-or-Cull
  only). Off by default. **Offline caveat:** a path tracer needs off-screen geometry
  for shadows/reflections/GI, so frustum culling is a separate opt-in
  (`-rtLodFrustumCull`, faster but changes lighting); proxy/sub-pixel are softer
  approximations. Works on the CPU two-level TLAS path (`-rtPreview`) **and** the
  `-vk`/`-vkr`/`-d3d`/`-hip` GPU backends — the latter apply it *flatten-side*
  (classify the world-space placements once, Cull→drop, Proxy→box, Full→keep, before
  building the flat BLAS). The GPU collector now expands both `PointInstancer` and
  scenegraph (`instanceable`) native instances in place (world-space placements),
  so instanced geometry renders and LODs on `-vk`/`-vkr` too (flattened: a
  prototype's geometry is duplicated per instance). For instanced scenes,
  **`-vkInstanced`** (implies `-vkr`) builds a true two-level GPU TLAS — one BLAS
  per prototype shared across all instances — storing instanced geometry once
  (e.g. 800 tris vs 160 000 for a 200× scatter), pixel-identical to the flat path.
  See `doc/lusdrender.md`.
* **Shading** — bound `UsdPreviewSurface` (diffuse/normal/roughness/metallic/
  emissive/occlusion textures); for unmaterialed geometry, `primvars:displayColor`
  / `displayOpacity` are honored — constant (per-mesh) and per-vertex/faceVarying/
  uniform (barycentrically interpolated per hit), with opacity blended see-through.
  Geom-only assets (e.g. Animal Logic ALab) render in color with transparent
  glass. UsdGeomBasisCurves, HermiteCurves, and NurbsCurves render as LightRT hair. `-smooth`
  interpolates authored `normals` for smooth shading (default is per-face
  geometric normals, which keeps the lean 4 B/triangle instanced footprint).
  The default `-materialResolver tydra-next` path uses the shared material
  converter. The legacy and compare modes remain migration aids for the
  shared material-eval layer; measure coverage on usd-assets with:
  `USD_ASSETS_ROOT=/path/to/usd-assets LUSDR_RUN_MATERIAL_RESOLVER=1 ctest --test-dir build -R tool-lusdrender-material-resolver --output-on-failure`.
  Add `LUSDR_MATERIAL_SHADING=lightrt-bsdf` to smoke the experimental BSDF
  shading mode through the same curated loop.
  `-materialShading lightrt-bsdf` is the next experimental step: with
  `-materialResolver tydra-next`, lusdrender carries the shared OpenPBR block in
  per-material side tables, then evaluates direct-light and headlight response
  through LightRT's OpenPBR BSDF. The diffuse irradiance and prefiltered
  reflection IBL terms also use `bsdf_eval` in this mode, and opaque hits trace
  a bounded `bsdf_sample` continuation bounce for indirect reflection/
  transmission. Transmission uses Beer-Lambert extinction over the measured
  interior ray distance; subsurface uses a radius/scale-aware normalized
  diffusion response, fuzz uses the Charlie distribution, and thin film uses
  wavelength-sensitive Airy interference. Legacy material resolution still
  falls back to the slim material fields.
* **Back plates** — the next CPU backend composites the selected camera's first
  visible `BackPlateAPI` instance. Color and optional alpha/depth images,
  scale/translate/Z-rotation tweaks, luma gain/lift/gamma, and camera-space
  depth normalization are honored. GPU backends currently omit the plate.
* **Displacement** — `UsdPreviewSurface inputs:displacement` (constant or a
  height texture, honoring the `UsdUVTexture` `scale`/`bias`) offsets each vertex
  along its normal before the BVH build — coarse (no extra geometry), so it works
  on the CPU path tracer and the `-vk`/`-vkr`/`-vkInstanced` backends (the latter
  displaces once per prototype in object space). `-displaceScale <f>` tunes
  the amount; `-noDisplace` turns it off (byte-identical to before).
* **Lighting** — UsdLux finite lights (Rect/Sphere/Disk/Cylinder/Distant) are
  collected from the composed stage and shaded with soft area falloff; DomeLight
  is image-based lighting (`--env` overrides it). Scenes with no lights fall back
  to a camera headlight. This lights interiors a dome can't reach (e.g. ALab's
  shot lighting rig, which renders the full lit shot directly from `entry.usda`).
* **Memory cap** — the shared automatic host policy caps the target at 32 GiB
  and reserves 2 GiB for the OS/driver (30 GiB process cap; override with
  `-maxMem`). StageSession composition receives 55% of that cap, leaving room
  for streamed geometry and BVH construction. When a scene would exceed it,
  lusdrender aborts
  cleanly with an actionable message (raise `-maxMem`, narrow with `-mask`, or
  lower `-complexity`) instead of being OOM-killed.

## Validated benchmark — Activision Caldera maps

All six maps rendered directly from their raw `.usd` (no pre-flatten), 320×180,
`-autoframe`, on a host with ~49 GiB available (auto cap 24.5 GiB). "visible" is
the instance-expanded triangle count actually traced; "unique" is the prototype
geometry stored once.

| scene          | visible tris | unique stored | dedup | instances | peak RSS | wall  |
|----------------|--------------|---------------|-------|-----------|----------|-------|
| restaurant     | 2.4 M        | 764 K         | 3.2×  | 1.8 K     | 0.4 GB   | 1.2 s |
| power_station  | 19.8 M       | 5.7 M         | 3.5×  | 11 K      | 2.4 GB   | 6.9 s |
| beachhead      | 160 M        | 19.9 M        | 8.0×  | 105 K     | 9.9 GB   | 39 s  |
| phosphate_mine | 126 M        | 28.2 M        | 4.5×  | 73 K      | 11.7 GB  | 41 s  |
| airfield       | 163 M        | 35.3 M        | 4.6×  | 104 K     | 17.1 GB  | 64 s  |
| capital        | **195.7 M**  | 43.1 M        | 4.5×  | 136 K     | 21.3 GB  | 81 s  |

Notes:

* Instancing dedup is what keeps these in memory: capital's 195.7 M visible
  triangles would need ~40 GB as a flat array + BVH (which overflows the LightRT
  builder), but only 43.1 M unique are stored → 21.3 GB.
* These RSS figures scale with the host. On a 32 GiB machine the cap is ~16 GiB,
  so airfield/capital abort gracefully (use `-maxMem`/`-mask`) rather than
  OOM-killing.

## Moana Island per-element benchmark

A head-to-head against Pixar `usdrecord` (CPU **hdEmbree**) on the Disney Moana
Island per-element geometry files lives in
[`doc/benchmarks.md`](../../doc/benchmarks.md) (Part 1 — Island), with the reproducible
harness [`bench_island.py`](bench_island.py). Summary: lusdrender is 2.9–14×
faster with 2–9× lower RSS on light/medium elements. UsdGeomPointInstancer
geometry (the XGen ground cover / foliage) is expanded through the two-level
BLAS/TLAS path, so isBeach renders its 4.09 B effective triangles (22 M instances,
63 K unique) in 32 s / 11.7 GB — **2.2× faster and lower RAM than usdrecord**
(73 s / 15.2 GB). UsdGeomBasisCurves, HermiteCurves, and NurbsCurves are ray-traced as LightRT hair
strands; curve-prototype PointInstancers store the curve geometry once (a curve
BLAS) and instance it through the same TLAS as meshes, so isIronwoodA1's bonsai
foliage renders. The instanced (TLAS) path is memory-lean: the per-triangle
record is a 4 B material id (material in a per-BLAS table; positions read from the
LightRT-aliased vertex soup), and instance placements are a compact 3×4 float.
This cut isCoral 6.5 → 4.85 GB and isBeach 11.7 → 7.5 GB (both byte-identical).
The one genuinely geometry-bound element (isCoral, 87.5 M tris, no instancing
shortcut) is still ~3× slower than Embree at ~1.5× the RAM (memory-bandwidth
bound; the gap is architectural). The full
assembled `island.usda` (all 20 elements + XGen + curves + DomeLight) renders
directly from raw USD: **5.69 B effective triangles** (22.8 M instances, 56.6 M
unique) in **1 m 33 s / 25.9 GB** (`-maxMem`). See the doc's analysis and
follow-ups.
