# ld3d12view

A native Windows **Direct3D 12** USD viewer: the Windows counterpart of
[`lmetalview`](../lmetalview/README.md). Drag a `.usd` / `.usda` / `.usdc` /
`.usdz` file onto the window (or pass it on the command line) and get a shaded,
framed preview with a timeline, cascaded shadows, image-based lighting,
skinning and playback, plus click-to-inspect, an outliner with variant
switching, camera bookmarks and screenshot / turntable capture.

It reuses lmetalview's portable C++ core (USD load, conversion to a flat preview
scene, orbit camera) and replaces the Swift/Metal side with a small C++
Direct3D 12 renderer. The window, input and HUD come from the vendored
[lightui](../common/lightui/README.lightusd.md); D3D12 renders straight onto
lightui's `HWND`.

```bat
:: from a "x64 Native Tools Command Prompt for VS 2022" (or any shell where
:: cl.exe and the Windows SDK are on the path)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DLIGHTUSD_BUILD_EXAMPLES=ON ^
      -DLIGHTUSD_BUILD_LD3D12VIEW=ON
cmake --build build --target ld3d12view

build\src\next\ld3d12view_example\ld3d12view.exe models\suzanne-pbr.usda
```

`LIGHTUSD_BUILD_LD3D12VIEW` defaults to `LIGHTUSD_BUILD_EXAMPLES`, is
only available on Windows and only in the `core` product. Enabling it also
builds `lmetalview_core` (the portable half of lmetalview), but not the Swift
app. Nothing needs installing beyond Visual Studio's C++ workload and the
Windows SDK: `d3d12`, `dxgi` and `d3dcompiler` ship with the SDK, and all
shaders are compiled at startup with `D3DCompile` (no `dxc`/`fxc` step).

## Requirements

* A GPU and driver with **Direct3D 12 feature level 11_0** and **resource
  binding tier 2 or higher** (the bindless texture table is unbounded). Every
  desktop GPU from the last decade qualifies. `--warp` selects the WARP software
  adapter, which also qualifies and needs no GPU at all (this is what CTest
  uses).
* `--debug` enables the D3D12 debug layer (needs the "Graphics Tools" optional
  Windows feature; the log says "debug layer unavailable" without it). Warnings
  and errors are printed, and the process exits with status 1 if any validation
  *error* was reported. Setting `LD3D12VIEW_DEBUG=1` does the same without the
  flag; every CTest test runs that way.

## Controls

| Input | Action |
| --- | --- |
| Left drag | Orbit (Maya-style, shared with lusdview and lmetalview) |
| Right / middle drag, shift + left drag | Pan |
| Wheel | Dolly |
| `F` | Frame the scene |
| `1`–`7` | Camera presets (iso, front, back, right, left, top, bottom) |
| `W` / `G` / `S` / `B` | Wireframe / ground grid / shadows / background |
| `H` | Show or hide the HUD |
| Space | Play / pause (animated scenes start playing) |
| `L` | Toggle looping |
| ← → / Home | Step one frame / back to the start |
| `C` | Step through the scene's authored cameras (the status panel shows `cam i/n`), then back to the free view you left; mouse navigation also returns to the free view |
| Left click (no drag) | Pick: outlines the object's bounding box and shows an inspector card with prim path, mesh, triangle, material and OpenPBR values, and the hit point |
| `O` | Show or hide the outliner panel (prim tree and variant sets) |
| `Ctrl`+`1`–`9` / `Shift`+`1`–`9` | Store / recall a camera bookmark (kept per scene file across restarts) |
| `P` / `Shift`+`P` | Save a PNG screenshot without / with the HUD into `--output-dir` |
| `T` | Record a 360° turntable as a PNG sequence in a new `turntable-<time>` folder under `--output-dir`; playback pauses meanwhile |
| Esc | Cancel a running load or turntable; otherwise clear the selection; otherwise quit |
| Drag and drop | Open the dropped file (the first one if several) |

The HUD's bottom bar (animated scenes only) has a play/pause button, a timeline
you can click or drag to scrub, and a time readout. The top-left panel shows the
file name, mesh count, time codes per second and frame rate. While a file loads,
a centred card shows its name, the current phase and a progress bar.

**Outliner (`O`).** A panel on the right lists the stage's *variant sets* (from
the root layer; click a row to reload with the next variant, the camera stays
where it is) and the prim tree built from the drawable meshes' paths (click a
group to expand it, a leaf to select it; the mouse wheel scrolls). Picking in the
viewport highlights the matching row. Variant overrides are applied to every
prim that defines a set of that name; sets that exist only in referenced or
payloaded layers are not listed, because composition resolves them away.

## Command line

```text
ld3d12view [options] [file.usd]
```

| Option | Meaning |
| --- | --- |
| `--size WxH` | Initial window size (default 1280x720) |
| `--warp` | Use the WARP software adapter |
| `--debug` | Enable the D3D12 debug layer |
| `--hidden` | Do not show the window (for scripted runs) |
| `--frames N` | Render N frames **after loading finishes**, then exit |
| `--screenshot out.png` | Write the last frame (HUD included) as a PNG; use with `--frames` |
| `--preset 0-6` | Start from a camera preset |
| `--wireframe`, `--no-grid`, `--no-shadows`, `--no-hud`, `--no-background` | Start with that feature off |
| `--time T`, `--animate-from T0` | Show the scene at time T (optionally after first evaluating T0) instead of playing |
| `--play`, `--paused`, `--no-loop`, `--speed X` | Playback control (speed multiplies real time) |
| `--authored-camera`, `--camera N` | Start on the scene's first (or N-th, from 0) authored camera |
| `--no-compress-textures` | Keep large 8-bit textures uncompressed (see below) |
| `--outliner` | Start with the outliner open |
| `--variant set=name` | Start with a variant override (repeatable) |
| `--output-dir DIR` | Where `P` and `T` write (default: the current directory) |
| `--turntable N` | Record an N-frame turntable headlessly into `--output-dir`, then exit |
| `--turntable-frames N` | Frames per 360° for the `T` key (default 120) |

Playback runs in real time: one second of wall clock advances the scene by
`timeCodesPerSecond` time codes (24 for most files), times `--speed`.

A file that cannot be loaded from the command line exits with status 1. A file
that fails to load after startup (a bad drop) leaves the current scene in place
and says "failed to load …" in the status panel for a few seconds.

### Test hooks

These drive the viewer through the real window procedure so CTest can cover
input and loading without a person at the keyboard. They are not meant for
interactive use.

| Option | Effect |
| --- | --- |
| `--simulate-drop FILE` | Posts a `WM_DROPFILES` message for FILE (as Explorer would) |
| `--simulate-space` | Posts a Space key press |
| `--simulate-click play`, `--simulate-click timeline=F` | Posts a left click on the play button / at fraction F of the timeline |
| `--simulate-resize WxH` | Resizes the window (swapchain, depth buffer and HUD texture are recreated) |
| `--load-delay-ms N` | Holds each load in flight for N ms so progress and cancellation can be observed |
| `--simulate-cancel` | Posts Esc while a load is running |
| `--capture-loading` | With `--screenshot`: capture while the load is still running |
| `--simulate-pick fx,fy` | Posts a left click at fractions (fx, fy) of the window |
| `--simulate-variant-click N`, `--simulate-outline-click N` | Clicks variant row N / visible prim row N of the outliner |
| `--simulate-camera-cycle N` | Presses `C` N times |
| `--simulate-bookmark N`, `--simulate-bookmark-load N` | Stores slot N, disturbs the view and recalls it (checks the round trip) / recalls a slot persisted by an earlier run |
| `--simulate-shot` | Presses `P` |

Environment: `LD3D12VIEW_DEBUG=1` (debug layer, see above),
`LD3D12VIEW_BOOKMARKS=file` (bookmark file; default
`%LOCALAPPDATA%\ld3d12view\bookmarks.txt`), `LD3D12VIEW_NO_PREFILTER=1` (skip the
specular environment prefilter, for A/B comparisons).

## How it works

| Layer | Source | Notes |
| --- | --- | --- |
| USD load + conversion | `lmetalview/core` (`lmv.h`) over `lightusd_core` + `tydra` | Shared with the Metal viewer; the same flat scene, packed OpenPBR materials, decoded images, lights and cameras |
| Camera | `examples/common/light3d/camera-nav.*` through `lmv_camera_*` | The `OrbitCamera` lusdview and lmetalview use; 0..1 depth projection |
| Window, input, HUD | vendored `examples/common/lightui` | `lui_window_native_handle()` gives the `HWND`; `lui_window_set_external_render()` stops lightui painting over D3D12 |
| Device + swapchain | `d3d12/device.*` | One direct queue, flip-model swapchain, 3 buffers, per-buffer allocators and fences, resize, synchronous capture |
| Scene upload | `d3d12/scene_gpu.*` | Vertex/index buffers, materials (structured buffer), lights, textures, shadow map, skin/morph data |
| Mesh shading | `d3d12/mesh_renderer.*` | One HLSL source compiled at startup: lit pass, cascaded shadow passes, blend pass |
| Lighting setup | `d3d12/lighting.*` | Authored lights or the studio rig, SH9 irradiance, GGX-prefiltered environment chain, shadow cascade fitting |
| Picking and inspector | `pick.*` | CPU ray cast (bounding box slab, then Möller–Trumbore) and the material/prim description |
| Points and curves | `d3d12/points_lines.*` | Camera-facing splats and line ribbons |
| Background | `d3d12/background.*` | Dome environment or a gradient |
| HUD | `hud.*`, `hud_outline.cc`, `d3d12/hud_renderer.*` | lightui widgets, the inspector card and the outliner drawn into a CPU surface, composited by one fullscreen pass |
| Loading | `loader.*` | Worker thread around `lmv_load` with progress and cancel |

**Frame order:** background → shadow passes (3 cascades of the one directional
caster) → opaque and masked meshes → points/curves → blended meshes sorted back
to front → grid → selection box → HUD.

**Resources are bindless.** Every texture lives in one shader-visible descriptor
heap and a material refers to it by index (`NonUniformResourceIndex`). Materials
and lights are structured buffers read through root descriptors. Per-draw state
is 20 root constants (world matrix plus material index and flags); per-frame
data is a constant buffer in a small upload ring (one slot per swapchain
buffer). Lights, skin palettes and blend-shape weights are rewritten each frame
so animation never re-uploads geometry.

**Animation** first tries `lmv_scene_update_animation` (node, light, camera,
visibility, PointInstancer, skeleton and blend-shape changes in place) and falls
back to `lmv_scene_set_time` plus a GPU rebuild for animated materials or
topology, exactly like lmetalview.

**Loading** runs `lmv_load` on a worker thread and uploads to the GPU on the main
thread when it finishes, so the window stays responsive while a file parses. A
new request cancels the running one. The GPU upload itself (textures, buffers)
still happens on the main thread and can pause the window briefly for a large
scene.

## What is rendered

| Feature | Supported |
| --- | --- |
| Base colour/weight, metalness, roughness, emission, opacity, normal maps | yes |
| Specular colour/weight, coat (own normal map), sheen, subsurface, transmission, thin film, anisotropy | yes, approximated (same model as lmetalview; no volumetric absorption or scattering) |
| Textures: 8-bit sRGB/linear, float EXR, KTX2 BC1/BC3/BC5/BC6H/BC7 | yes, with a full mip chain (generated on the CPU for uncompressed images). 8-bit images of at least 512×512 (multiples of 4) are **BC7-compressed at load** (4× less video memory, ~58 dB PSNR on a photo texture; the load log reports the saving). Ptex atlases and float images are never compressed. `--no-compress-textures` opts out |
| UDIM | yes: one texture array per UDIM texture plus a 10x10 tile table; missing tiles render magenta |
| Ptex | yes: atlas image plus per-face rectangles and per-vertex face ids |
| Wrap modes (repeat, clamp, mirror, black), UV offset/scale/rotation, channel select, value scale/bias | yes |
| Authored lights (distant, spot, point; rect/disk/sphere as point lights) or a three-point studio rig | yes |
| Dome light: SH9 diffuse + GGX-prefiltered specular + background | yes. The environment is resampled to 512×256 and convolved on the CPU into a 6-mip chain (mip *i* = roughness *i*/5), then combined with the split-sum environment BRDF and multiple-scattering compensation (also applied to direct lights) |
| Dielectric reflectance and diffuse | F0 from `specular_ior`; Oren–Nayar diffuse from `base_diffuse_roughness` |
| Directional shadows: 3 cascades of 2048², texel-snapped, 3×3 PCF, normal-offset and slope bias, alpha-tested for cutouts | one caster (first directional light); cascades are fitted to the view frustum clipped to the scene bounds |
| Alpha: opaque, mask (cutout), blend | yes; blended draws are sorted per draw, not per triangle |
| Skinning (4 influences) and blend shapes | yes, in the vertex shader |
| PointInstancer / native instances | yes (expanded by the core) |
| UsdGeomPoints / particles, curves | yes (discs and ribbons with a 1 px minimum size) |

A connected texture **replaces** the material's constant colour (base and
emission). The packed constant is only a fallback — UsdPreviewSurface leaves its
0.18 default there when `diffuseColor` is textured — so multiplying by it would
darken every textured material.

## Limitations

* One shadow caster (the first directional light); spot and point lights cast no
  shadows, and blended surfaces do not cast shadows either.
* Picking tests the rest pose of skinned meshes and the bounding box is not
  refit to blend shapes. Selecting from the outliner picks a mesh's first draw
  (instances are not listed separately).
* The outliner lists drawable meshes only (no lights, cameras or empty prims),
  and variant sets come from the root layer.
* Blending is in encoded (sRGB) space and sorted per draw by the bounding-box
  centre, so overlapping or very large blended meshes can order wrongly. There
  is no order-independent transparency.
* Transmission refracts only the environment, never other objects.
* Float textures are uploaded as 32-bit floats and mips are box-filtered in
  encoded space.
* BC textures whose size is not a multiple of 4 are not uploaded (treated as
  unbound), and images that are not multiples of 4 are never compressed.
* HDR environment images are mapped as Y-up equirectangular regardless of the
  scene's up axis (same as lmetalview).
* The HUD does not scale with display DPI and renders Latin text only (the
  embedded Hack font).
* The GPU upload after a load, and the rebuild after an animated-material time
  change, run on the main thread and can stall a frame.
* Windows only. lightui implements file drop on Win32 only.

## Troubleshooting

* **"CreateGraphicsPipelineState … failed" or a root-signature error at
  startup** — the GPU/driver is below binding tier 2. Run with `--warp`.
* **A black window / device removed** — run with `--debug` (needs Graphics
  Tools) and read the debug output, or try `--warp` to rule out the driver.
* **Drops do nothing** — a viewer started as Administrator does not receive
  drops from a normal Explorer window (a Windows rule, not a viewer setting).
* **A texture shows magenta** — a UDIM tile or Ptex face is missing/undecoded.
  The load log also counts textures it could not use ("N unsupported").

## Tests

All registered by CTest when the core tests are enabled, all on the WARP adapter
so they run without a GPU. The input and loading tests post real window messages
(`WM_DROPFILES`, `WM_KEYDOWN`, mouse clicks, `WM_SIZE`), so they exercise the
window procedure, not just the viewer logic.

```bat
ctest --test-dir build -R "ld3d12view|lmetalview" --output-on-failure
```

Every test runs with the D3D12 debug layer on (when Graphics Tools is installed)
and fails on any `D3D12 ERROR` line. To also run a few scenes on the machine's
real GPU, configure with `-DLD3D12VIEW_HW_TESTS=ON`: this adds `ld3d12view-hw-*`
tests (label `hardware`) that fail if they land on WARP.

See [doc/testing-cpp.md](../../doc/testing-cpp.md) for the list.

## Relationship to lmetalview

The scene description, materials model, orbit camera and animation strategy are
shared with lmetalview on purpose, so the two viewers show the same scene the
same way. Where they differ:

* the renderer is C++/HLSL/D3D12 instead of Swift/MSL/Metal;
* the background shows the real dome image when there is one (lmetalview shows a
  dimmed studio gradient);
* a connected texture replaces the constant colour instead of multiplying it;
* real-time playback uses `timeCodesPerSecond` (lmetalview advances one time code
  per second at 1×).

Changes made to the shared core while building this viewer: it now also builds on
Windows (`lmetalview_core`), `lmv_light_view` gained `dome_image_id` (tydra
stores an image id in the dome light, not a texture index), and `lmv.h` documents
that a scene calls the load-time progress callback again from
`lmv_scene_set_time`, so `progress_user` must outlive the scene. Later additions:
`lmv_load_options.extra_variant_*` (several variant overrides at once) and
`lmv_scene_variant_set_count/_variant_set` (the root layer's variant sets), and a
fix that ignores UsdPreviewSurface's default black `specularColor` (it zeroed F0
and made default metals render black in both viewers).
