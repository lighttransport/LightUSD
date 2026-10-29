# Large-scene previews

`lusdpreview` composes a source USD scene in batches on the CPU, bakes bounded
texture samples, and writes reusable USD preview sidecars. Gaussian splats are
the default. Point previews use `ParticleField` with a constant surflet kernel;
volume previews use `UsdVol` / `OpenVDBAsset` with surface and albedo grids.

```sh
cmake -S . -B build_ninja -G Ninja -DLIGHTUSD_BUILD_TESTS=ON
cmake --build build_ninja --target lusdpreview
build_ninja/lusdpreview scene.usda preview-export --kind gaussian --progress
```

The export directory must be new. Copy the whole directory to move a preview;
`root.usdc` references its relative sidecars. Volume exports additionally contain
`render.usdc`, a cached colored isosurface used by the existing surface renderers.
This is a surface approximation of the scene, not participating-media rendering.

The same switches work before loading in both applications:

```sh
lusdview scene.usda --scene-preview --preview-kind gaussian
lusdrender scene.usda out.png --scene-preview --preview-kind gaussian -cuda
lusdrender scene.usda out.png --scene-preview --preview-kind volume -vkr
```

`--preview-budget-mib` is the total renderer budget, including a runtime reserve,
upload copies, geometry and acceleration/sort scratch. By default the CLI uses
one quarter of installed NVIDIA VRAM, capped by available memory minus 512 MiB;
without a device query it uses 2048 MiB. An 8 GiB GPU therefore defaults to
2048 MiB. GPU conversion kernels are not required. CPU rendering uses the same
cached scene; the viewer's CPU tracer uses bounded triangle surflets.

Automatic sampling coarsens to fit the budget. `--preview-resolution N` requests
an explicit longest-axis resolution and fails if it exceeds the budget. To
refine, choose a larger resolution/budget and regenerate with `--preview-refresh`
(`--refresh` in `lusdpreview`). Both applications automatically select among four
cached USD variant levels within the geometry budget. The viewer refines as the
camera approaches and coarsens as it recedes, with hysteresis and a 250 ms pause
after interactive movement. `--preview-lod-pixels N` sets the projected error
(default 2 pixels); `--preview-no-lod` uses the finest cached representation.
The experimental render-thread path does not perform automatic camera updates.
Refinement stops at the finest samples captured during conversion.

`lusdpreview --lod` also builds the portable companion `lod-v3/root.usdc` and
relative tile sidecars. Point and volume conversions reuse the canonical sampled
Gaussian cache, avoiding another composition of the original scene. Volume
bricks adapt their voxel size to local surface scale so distant environment
geometry does not erase nearby detail. Their render LODs use the cached colored
isosurface. Cache version 3 regenerates older sampled caches.

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
the selected time. Cameras and untextured lights are retained; light asset
textures are omitted with a warning to keep exports independent of the source
asset tree. Volume shells approximate opacity as surface coverage.

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
