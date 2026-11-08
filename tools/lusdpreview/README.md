# Large-scene previews

`lusdpreview` composes a source USD scene in batches on the CPU, bakes bounded
texture samples, and writes reusable USD preview sidecars. Gaussian splats are
the default. Point previews use `ParticleField` with a constant surflet kernel;
volume previews use `UsdVol` / `OpenVDBAsset` with surface and albedo grids.
Both applications render these grids directly by ray marching their signed
distance fields and sampling the albedo field.

```sh
cmake -S . -B build_ninja -G Ninja -DLIGHTUSD_BUILD_TESTS=ON
cmake --build build_ninja --target lusdpreview
build_ninja/lusdpreview scene.usda preview-export --kind gaussian --progress
```

The export directory must be new. Copy the whole directory to move a preview;
`root.usdc` references its relative sidecars. Standalone volume exports normally
include `render.usdc`, a cached colored isosurface for other surface renderers.
`lusdpreview --volume-no-surface` omits surface extraction; its `render.usdc`
contains only bounds/camera metadata. Application volume previews use this
mode by default. `--preview-volume-carrier` opts into the previous mesh path.
The default mode is a surface approximation; participating-media options are
described below.

The same switches work before loading in both applications:

```sh
lusdview scene.usda --scene-preview --preview-kind gaussian
lusdrender scene.usda out.png --scene-preview --preview-kind gaussian -cuda
lusdrender scene.usda out.png --scene-preview --preview-kind volume -vkr
```

`--preview-budget-mib` is the total renderer budget, including a runtime reserve,
upload copies, geometry and acceleration/sort scratch. The CLI defaults to
2048 MiB without querying device memory. Applications can provide GPU capacity
explicitly through the public API to derive a quarter-capacity budget.
`--preview-compute auto|cpu|vulkan|cuda` selects conversion compute. Auto tries
only backends with an explicit host provider and library filenames, then CPU.
An explicitly requested unavailable GPU backend fails. Configure
`-DLIGHTUSD_PREVIEW_GPU_COMPUTE=OFF` for a CPU-only preview build.

Native GPU preview compute requires `--preview-native-host` and absolute loader
filenames: `--preview-vulkan-library FILE` for Vulkan, or
`--preview-cuda-driver-library FILE --preview-cuda-compiler-library FILE` for
CUDA. The library does not search for drivers automatically. The qualification
scripts below accept the same library filename arguments and supply the native
provider when any is present. Supply these paths for GPU conversion and native
volume qualification. See [explicit host capabilities](../../doc/testing-cpp.md#explicit-host-capabilities-and-builds-without-native-io)
for the callback API and `LIGHTUSD_NATIVE_IO=OFF` builds.

Bounded kernels interpolate geometry/attributes for splats, points and volumes;
CPU baking evaluates the sampled material/texture inputs. Source composition,
geometry discovery and cache writing remain on the CPU. CPU rendering uses the same
cached scene; the viewer's CPU tracer uses bounded triangle surflets.

Automatic sampling coarsens to fit the budget. `--preview-resolution N` requests
an explicit longest-axis resolution and fails if it exceeds the budget. To
refine, choose a larger resolution/budget and regenerate with `--preview-refresh`
(`--refresh` in `lusdpreview`). For splats, points and volume surface carriers,
both applications automatically select among four cached USD variant levels
within the geometry budget. The viewer refines as the
camera approaches and coarsens as it recedes, with hysteresis and a 250 ms pause
after interactive movement. `--preview-lod-pixels N` sets the projected error
(default 2 pixels); `--preview-no-lod` uses the finest cached representation.
Camera refinement also works with the experimental GL/Vulkan `--threaded`
render owner. GPU work stays on its owner thread and a scene-swap handshake
protects the outgoing scene until replacement uploads finish.

When the camera needs detail beyond the captured samples, the viewer resamples
an automatically chosen visible source region on a background worker. It clips
source triangles to the sampling window, prunes distant instance placements
and keeps a bounded sampled background. Native/point-instanced source triangles
and placement links use a persistent disk index, preserving prototype reuse
without duplicating all placed source meshes in memory. Budget pressure coarsens
that background to leave room for visible detail. Source refinements are cached,
cancelled when the camera moves, and presented after complete uploads. The
renderer resolves the equivalent camera region before its single-frame render.
`--preview-no-source-refine` keeps the original cached samples for comparisons.

A cold conversion builds a reusable `source-index-v1-*` sibling in the cache.
Older preview caches build it on their first source refinement. That one-time
composition can still take minutes. Later camera regions validate the source
layer stamps, seek intersecting mesh chunks and replay only nearby placements;
they do not compose the original scene. The index stores original triangles,
material graphs and placement links, and is separate from portable exports.
Time, variant selection and source dependency changes select or invalidate it.
Interactive rendering retains the old preview while a region is prepared.

PointInstancer `inactiveIds` metadata participates in visibility using exact
signed 64-bit IDs. Preview cache version 4 and source geometry index version 2
invalidate older samples that could include inactive instances. Conversion
rebuilds these caches automatically.

Direct volume rendering uses the authored UsdLux lights, including color,
exposure, temperature, transforms, diffuse strength, normalization and spot
cones. Distant and local lights cast bounded SDF shadows; dome lights provide
constant ambient illumination. Area lights use a finite-area approximation.
With no scene lights it retains the headlight fallback. Camera exposure and a
Reinhard display curve preserve HDR lighting detail. Use `--preview-volume-grids` in either application to open an existing UsdVol
scene directly, without surface conversion or isosurface extraction. Density-only
inputs automatically use medium rendering; missing albedo is white. Grids must
have axis-aligned uniform transforms (including translation and uniform Volume
scale). Other transforms fail explicitly. Mixed mesh/volume shading is outside
this direct preview path. Each dense grid is limited to one million voxels and
the total renderer budget; larger native grids need coarser input.

For example:

```sh
lusdview fog.usda --preview-volume-grids --backend vk
lusdrender fog.usda fog.png --preview-volume-grids -cuda
```

Use `--preview-volume-mode medium` for Beer-Lambert absorption, single scattering
and emission. For generated previews the finite SDF shell defines density;
this does not reconstruct smoke or other media absent from the source sampling.
`--preview-volume-density F` scales extinction, `--preview-volume-scattering F`
sets scattering albedo (0–1), `--preview-volume-emission F` adds colored emission,
and `--preview-volume-anisotropy F` sets the Henyey-Greenstein phase parameter
(-0.95–0.95). Independent native volumes add extinction and extinction-weighted
color where they overlap. Generated preview tiles carry an overlap-group marker
so their coarse/fine halos select the finest field within each preview rather
than doubling density. Explicit native tiles can opt into the same behavior
with a `lightusd:preview:overlapGroup` token on their Volume prims.

Rays split at field entry/exit boundaries and integrate density/color using
adaptive two-point Gauss quadrature. `--preview-volume-step-size F` controls the
maximum step in voxel units (default .5, range (0,2]);
`--preview-volume-error F` sets relative quadrature error (default .001, range
[0,1], with 0 disabling refinement). `--preview-volume-cutoff F` controls early
termination of negligible transmission (default 1e-7, with 0 disabling it).
Light is evaluated inside each segment at its absorption-weighted centroid,
with extra subdivisions for optically thick emission/scattering segments.
Shadow rays use the same field boundaries and quadrature; density grids also
attenuate surface shadows. Step exhaustion or unmet quadrature tolerance fails
the frame explicitly, instead of silently treating unvisited media as air.

`--preview-volume-steps N` and `--preview-shadow-steps N` select budgets up to
65,536 steps (defaults 4096 and 1024). Finite rect, disk, sphere/ellipsoid and
cylinder lights sample their emitting surfaces with transformed area Jacobians,
projected area and inverse-square geometry. `normalize` divides by emitting
area; sampled directional cones use the authored angular diameter. Surface
reflection uses the Lambertian 1/pi factor; local/directional medium lighting
uses a normalized Henyey-Greenstein phase function. Untextured environments
sample the surface/phase distribution directly and cast volumetric shadows.
These controls converge the existing sampled fields, rather than reconstructing
source geometry absent from the preview.

Vulkan submits work in bounded batches and specializes conversion/surface/
single-scattering/multiple-scattering pipelines. Field bounds use a stackless
spatial index; CPU and software Vulkan share the same kernels. Use
`--preview-volume-headlight` or `--preview-volume-no-shadows` for comparisons.
Lights are frozen to the cached scene. Dome textures support latlong, angular and
mirrored-ball maps with authored orientation.
Emission maps are bounded to 256 pixels on the longest edge and baked as relative
linear HDR sidecars for portable export. Latlong domes use luminance-weighted
texel sampling with exact texel solid angles; rect lights use luminance-weighted
area sampling. This preserves small bright emitters that sparse axis/area
quadrature can miss. `--preview-light-samples N` selects 1–64 samples per textured
light (default 8), independently of volume path samples. Sampling tables are
shared by identical emission maps and charged against the renderer budget;
one 256×256 table uses about 256 KiB. Angular and mirrored-ball domes sample their original projection disks with
spherical Jacobians, preserving bright edge emitters without a latlong resample. Sampled emission is constant within each cached texel;
low counts can produce noise, especially for surface lighting and shadows.

Stable-camera frames progressively average linear radiance before exposure,
tonemapping and display conversion. `--preview-accumulation-frames N` sets the
limit (1–4096, default 64); use 1 for single-frame comparisons. Camera, clip,
viewport, exposure and shading changes reset the average, as does a scene or
light reload. `lusdview` advances once per rendered frame; saving the final
screenshot reuses the existing frame. The experimental render-thread path uses
the same accumulation. `lusdrender -samples N` accumulates N frames up to the
configured limit. One additional float RGBA image resides in host memory, while
GPU scene buffers are reused. The frame-size guard includes this working set.

The importance sampler passed analytic single-emitter energy checks and 18
headless captures: sparse bright fog, generated SDF surfaces and cached Caldera,
each in both applications across CPU/Vulkan/CUDA. Vulkan used GPU-assisted
validation. At 96×72, peak measured process VRAM was 539 MiB across these runs.
HDR emission sidecars use explicit Radiance scanline packets; affected older
texture caches receive a lighting upgrade, while caches without emission
sidecars remain valid. IES profiles, light linking and custom light
filters remain unsupported.

Use `--preview-volume-bounces 4 --preview-volume-samples 8` with medium mode for
Monte Carlo multiple scattering. Bounce counts are bounded to 1–256 and paths per
pixel to 1–64; defaults are one scattering order and two paths. One order keeps
the deterministic integration above. Higher orders sample free-flight distances
and Henyey-Greenstein directions, with direct lighting at each collision and Russian roulette after the fourth bounce. They
retain fixed seeds for repeatable frames and add noise at low sample counts.
Shadow quadrature, bounded ray steps and finite scattering orders remain preview
approximations. These controls do not allocate additional scene-sized buffers.

Qualification on the 8 GiB RTX 3070 and 32 GiB host passed both applications on
CPU, hardware Vulkan (GPU-assisted validation) and CUDA. The textured native fog
fixture used four orders and eight paths at 96×72, peaking at 433 MiB process
VRAM. Caldera used four orders and two paths at 96×72, peaking at 539 MiB VRAM
and 692 MiB RSS after cache conversion; backend captures matched. These are
small-frame preview checks, rather than an image-convergence benchmark. A
lighting-only upgrade of the older Caldera cache took 215 seconds and reused
the geometry index; subsequent conversions validate the updated cache.

`lusdpreview --lod` also builds the portable companion `lod-v3/root.usdc` and
relative tile sidecars. Point and volume conversions reuse the canonical sampled
Gaussian cache, avoiding another composition of the original scene. Volume
bricks adapt their voxel size to local surface scale so distant environment
geometry does not erase nearby detail. The optional carrier path uses cached colored
isosurface LODs; the direct path marches the original brick fields. Cache version 3 regenerates older sampled caches.

`--preview-cache-dir DIR` selects a cache on local disk. A warm cache checks source
layer and sampled texture sizes/timestamps without composing the original
geometry. The key includes representation, sampling settings, time and variant
selection. Source files must remain available for dependency validation. An
exported preview can be opened directly without the source scene.

Conversion checks a 16 GiB host RSS budget and a 4 GiB available-memory
reserve at composition/sampling boundaries on Linux. Use a disk-backed cache for large scenes. Native instances
and PointInstancers reuse disk-backed prototype samples. Materials bake
base color, opacity, normals and supported texture coordinates; complex shading,
specular/transmission, displacement and animation are approximate or frozen at
the selected time. Cameras and lights are retained; supported light emission textures are baked
into portable sidecars. Missing textures fall back to constant emission with a warning. Volume shells approximate opacity as surface coverage.

### Light filters

Lights may bind portable filters with `rel light:filters` (or `rel filters`).
A filter prim carries `uniform token lightusd:filter:kind` plus its parameters:

| Kind | Parameters | Effect |
|---|---|---|
| `intensity` | `float lightusd:filter:intensity` (default 1, clamped 0..64) | multiplies the light intensity |
| `color` | `color3f lightusd:filter:color` (default 1,1,1, non-negative, clamped 16) | multiplies the light color |

The converter resolves each target, reports a warning for a missing prim, an
unknown kind or malformed parameters, and folds the receiver-independent result
into the render-ready intensity/color. Every backend (raster, RT, preview
kernels, C API consumers) therefore sees the same light without per-kernel
evaluation, and a cached/exported preview bakes the folded values into the
light it keeps, so an exported cache lights the same way without the filter
prims. Unknown kinds leave the light unchanged instead of silently brightening
or darkening it.

Additional kinds register a parser without core changes:

```cpp
lightusd::tydra::next::RegisterLightFilter(
    "gain", [](const lightusd::next::Stage&, const lightusd::next::UsdPrim&,
               double, lightusd::tydra::next::LightFilter* out, std::string*) {
      out->intensity = 0.5f;   // receiver-independent multiplier
      return true;
    });
```

Receiver-dependent kinds (light shapers, blockers) need per-receiver evaluation
in each backend and are not folded by this path.

The same registry is reachable from C and Python. A custom kind reads up to
eight authored scalars from `lightusd:filter:value0..7` plus `:intensity` and
`:color`, and returns the multiplier:

```c
static int gain(const lightusd_light_filter_params* p, void* user,
                float* intensity, float* color) {
  *intensity = p->values[0];           /* lightusd:filter:value0 */
  color[0] = p->color[0];
  color[1] = p->color[1];
  color[2] = p->color[2];
  return 1;                             /* 0 rejects the filter prim */
}
lightusd_register_light_filter("gain", gain, NULL);
```

```python
def gain(params):
    return params["values"][0], params["color"]

lightusd.register_light_filter("gain", gain)
intensity, color = lightusd.light_filter_effective(stage, "/World/Gain")
lightusd.unregister_light_filter("gain")
```

### Light and shadow linking

Lights may restrict their receivers with the usual USD relationships:

```
def SphereLight "Key" {
  rel light:link = [</Fog>]
  rel shadow:link = [</Fog>]
}
```

The cached preview keeps these scene-relative relationships. On load, each
volume prim gets a dense receiver id and each linked light gets a compiled bit
mask (`ceil(receivers/32)` words), tested once before emission and once before
the shadow ray, so light and shadow linking stay independent. A light without a
relationship links everything; isosurface shading has no receiver id and is
unfiltered. The preview applies shadow links receiver-side: a receiver excluded
from a light's shadow set is lit unshadowed rather than filtering individual
blockers. Targets that do not resolve to a cached volume contribute no receivers.

Photometric profiles (`inputs:shaping:ies:file`) are anchor-resolved, baked to a
portable `light-<hash>.ies` sidecar in the cache, and resampled on load into a
32x16 candela table in the light's local frame. The shared volume kernel
interpolates that table (bilinear in vertical angle and wrapped azimuth, with
`angleScale`/`normalize` already applied), so CPU, Vulkan and CUDA produce the
same shaped emission. A profile that cannot be read leaves the light unshaped
with a warning instead of failing the conversion.

For development without a hardware GPU, use the installed Mesa software stack:

```sh
scripts/with-software-rendering.sh \
  build_ninja/src/next/tydra_next_build/test_scene_preview_compute vulkan
python3 tools/lusdpreview/check-renderers.py scene.usda --software \
  --kinds volume --viewer build_ninja/preview-render/lusdview \
  --renderer build_ninja/preview-render/lusdview_lightrt_c_build/lusdrender \
  --cache build_ninja/software-cache --output build_ninja/software-check
```

The wrapper pins Vulkan to lavapipe, selects llvmpipe for OpenGL, limits software
worker threads to four by default, and disables NVIDIA offload. The explicit
`--preview-software` option selects only CPU Vulkan devices. Pass `--preview-software` to preview applications run through the wrapper.
`--software` applies the wrapper and explicit preview option to each application,
skips CUDA/NVML and labels its results as software checks. These runs do not
qualify hardware performance or hardware VRAM usage. Set
`LIGHTUSD_SOFTWARE_VULKAN_ICD` if Mesa's ICD lives outside the standard paths.

On memory-constrained development machines, build and run one workload at a
time. The Linux guard can wrap builds or tests, includes worker processes in
its RSS total, and checks host and cgroup-v2 ancestor memory headroom:

```sh
python3 tools/lusdpreview/memory_guard.py --max-rss-mib 8192 --reserve-mib 4096 -- \
  scripts/with-software-rendering.sh cmake --build build_ninja -j1
```

For CPU/compose qualification, configure with
`-DLIGHTUSD_PREVIEW_GPU_COMPUTE=OFF` to omit preview shader generation. Selecting
Mesa at runtime does not limit the shader compiler's build-time memory use.

`check-renderers.py` uses the same guard with an 8192 MiB RSS threshold and a
4096 MiB available-memory reserve. Set `--max-rss-mib`, `--reserve-mib` and
`--preview-host-mib` (default 6144) for the host; the preview host budget is a
converter setting, while the external guard observes the entire process group.
Timeout or memory-limit termination escalates to SIGKILL if needed and fails
the check. Sampling every 50 ms reduces OOM risk but is not a hard allocation
limit. Image blank checks use at most a 320×240 thumbnail and validation logs
are scanned incrementally. Run `python3 tools/lusdpreview/test_memory_guard.py`
to check ancestor limits, worker accounting and bounded termination.

For GPU qualification, build both applications with Vulkan and CUDA support,
then run on a host with NVIDIA NVML, Python Pillow (`python3-pil` on
Debian/Ubuntu), and hardware device access:

```sh
python3 tools/lusdpreview/check-renderers.py scene.usda \
  --viewer build_ninja/lusdview --renderer build_ninja/tools/lusdrender/lusdrender \
  --cache build_ninja/preview-cache --output build_ninja/preview-check
```

The runner renders all three representations in both applications on CPU,
Vulkan and CUDA, records wall time / RSS / per-process VRAM, requires measurable
hardware allocations for GPU runs, and stops its own process at the 2 GiB GPU
or 16 GiB host limit. For Vulkan shader validation, also set `VK_LAYER_PATH`,
`VK_INSTANCE_LAYERS` and `VK_LAYER_SETTINGS_PATH` as described in
[the viewer documentation](../../doc/lusdview.md#gpu-assisted-validation-shader-faults--device-loss).

Camera refinement has a separate 27-capture regression:

```sh
python3 tools/lusdpreview/check-lod.py --viewer build_ninja/lusdview \
  --output build_ninja/preview-lod-check
```

It checks near/far automatic selection and budget accounting on CPU, Vulkan and
CUDA for every representation, and compares refined finest captures against
captures with LOD disabled.

## Qualification result

A full 45-district Caldera source was qualified on a nominal 32 GB host with an
8 GiB RTX 3070, using the default 2048 MiB renderer ceiling. All formats sampled
10,224,245 composed prims, 125,152 source meshes and 1,057,296 placements into
514,135 retained surface samples. Cold conversion timings include composition,
texture/material discovery, sampling and sidecar writing; the first Gaussian
run predates the transform/block-I/O optimization used by subsequent runs.

| Representation | Cold conversion | Peak converter RSS | Warm GPU render | Peak measured renderer VRAM |
|---|---:|---:|---:|---:|
| Gaussian | 13 min 52 s | 9.30 GiB | 1.59–2.86 s | 653 MiB |
| Points | 10 min 50 s | 9.30 GiB | 1.53–2.80 s | 653 MiB |
| Volume + surface carrier | 11 min 35 s | 9.30 GiB | 1.73–3.00 s | 779 MiB |

Warm timings are complete 320-pixel-wide single-frame process runs, including cache
validation and loading; they are not steady-state FPS. The 18-case full-scene
matrix passed on CPU / Vulkan / CUDA in both applications with Khronos
GPU-assisted validation and nonblank-image checks. NVML measurements cover only
the renderer process, so unrelated GPU workloads are excluded. Converter limits
are independent of the much smaller warm-render working set.

Release and Debug native suites passed, with the optional AOUSD value-resolution
case skipped. All 5,278 existing composition/rewrite comparisons matched the previous
binary: 5,276 successful outputs were byte identical and two expected failures
remained unchanged. Full source composition/write finished separately in 83.9 s at 9.27 GiB
peak sampled RSS with no process swap, retaining its prior SHA-256 and
3,014,654,530-byte output. The
existing Gaussian dispatch/schema tests passed. The follow-up hardware-RT
chunk test also passed on the RTX 3070 with GPU-assisted validation after enabling
the sampled-image nonuniform-indexing feature required by its shaders. Headless qualification used the
viewer with MCP disabled because this host's MCP link requires an unavailable
C++ runtime symbol; this does not affect the rendering backends.

Follow-up qualification also covers Island and ALab (using its authored
`renderCam` interior camera), with the same 18-case matrix per scene. A fresh
canonical conversion took 10 min 57 s for Caldera (9.44 GiB peak RSS), 20 min 57 s
for Island (2.10 GiB), and 47.5 s for ALab. Reusing those samples reduced point
conversion to about 1–3 s and adaptive volume conversion to 14–28 s. Unsupported
source shading and unavailable textures still use the reported material fallback.

All 54 follow-up captures passed with GPU-assisted Vulkan validation and
nonblank-image checks. Peak measured per-process VRAM was 758 MiB for Caldera,
812 MiB for Island, and 1,356 MiB for ALab, below the 2,048 MiB ceiling. All 27
camera-regression captures passed; automatic finest selection matched disabled
LOD exactly in each backend and representation. The native Release / Debug
suites and seven focused renderer regressions also passed (one optional native
corpus check skipped per suite).

## Extended preview qualification

The direct-grid Caldera matrix passes all six CPU/Vulkan/CUDA combinations in
both applications under Vulkan GPU-assisted validation. Peak sampled renderer
VRAM is 540 MiB and peak RSS is 694 MiB. With its canonical samples already
cached, direct-grid conversion took 12.0 s on CPU, 7.2 s on Vulkan and 4.6 s on
CUDA in this run (concurrent source-composition work was active). These timings
include grid construction and VDB writing, but exclude original sampling.

The shared kernel test compares CPU/Vulkan/CUDA geometry sampling, conversion
and direct SDF/albedo rendering, including persistent-grid camera updates and
native-instance batch/source replay. Vulkan scalar differences
were below `1e-6`; the focused rendered pixels agreed within one 8-bit step.
Source refinement passes 18 headless captures across all formats/backends and six captures each
with actual GL and Vulkan render threads. The original 27-capture cached-LOD
check remains available with source refinement and direct grids disabled.

```sh
python3 tools/lusdpreview/check-source-refinement.py --viewer build_ninja/lusdview \
  --output build_ninja/source-refinement-check
xvfb-run -a -s "-screen 0 1280x800x24" \
  python3 tools/lusdpreview/check-source-refinement.py --viewer build_ninja/lusdview \
  --output build_ninja/threaded-refinement-check --threaded --owner vk
```

Use `--owner gl` for the other experimental render owner. GPU device/offload and
validation-layer setup follow the same qualification procedure above.

A close-view full Caldera original-triangle refinement completed in 17 min 35 s
at 11.44 GiB peak RSS, including CUDA rendering. It increased retained samples
from 514,135 to 689,600 and reduced the world-space sampling cell from 226.77 to
9.26 scene units. Source composition and texture/material discovery still
dominate that cold path; geometry outside the sampling window stays coarse.

## Persistent-index and lighting checks

The measured index/query results on the 32 GB host were:

| Scene | One-time cold index build | Index on disk | New indexed region | Peak region RSS |
|---|---:|---:|---:|---:|
| Caldera | about 12 min | 9.7 GiB | 18.9 s | 475 MiB |
| Island | 46 min 50 s | 13 GiB | 22.1 s | 1.70 GiB |

Both region checks forbid source composition. Caldera retained 514,135 → 566,662
samples at a 0.231-unit cell. Island retained 1,120,081 → 1,251,147 samples,
including 131,066 regional samples at a 0.226-unit cell. The camera windows
are different from the earlier 17-minute qualification; these are indexed-query
measurements rather than a controlled same-view speedup comparison. Regional
cache hits avoid sampling as well.

Island's complete cold pass now includes 25,852,934 placements and 119,215
meshes. The preview CLIs derive the permitted array count from the host budget,
allowing the large arrays that exceeded their previous 16-million-element
reader default. Peak cold RSS was 5.53 GiB with no swap. Derived point/volume
caches track the canonical manifest so a canonical rebuild invalidates them.

All 36 Island/ALab representation captures, 12 additional medium captures and
six native-density captures passed across CPU/Vulkan/CUDA in both applications,
with Vulkan GPU-assisted validation. ALab uses its authored `renderCam`.
Measured per-process VRAM stayed below 2,048 MiB (878 MiB maximum in the
representation matrices). Source geometry, indices, images and logs remain
outside Git. The native Release/Debug suites pass with one optional corpus
case skipped per suite.

The compute regression checks analytic homogeneous-density absorption and
single scattering, emission, phase changes, authored surface lights and indexed
native-instance resampling on CPU/Vulkan/CUDA. Medium pixels allow up to four
8-bit steps across floating-point backends near brick boundaries.

```sh
python3 tools/lusdpreview/check-particle-radiance.py --viewer build_ninja/lusdview \
  --output build_ninja/particle-radiance-check
python3 tools/lusdpreview/check-renderers.py scene.usda \
  --viewer build_ninja/lusdview --renderer build_ninja/tools/lusdrender/lusdrender \
  --cache build_ninja/preview-cache --output build_ninja/medium-check \
  --kinds volume --volume-mode medium
```

The radiance check verifies that a millionfold light intensity change does not
relight converted point colors in Vulkan raster rendering. Converted point
previews use their stored normals for a bounded camera light, so uniformly
colored interiors retain visible shape; ordinary authored SH carriers are unlit.
