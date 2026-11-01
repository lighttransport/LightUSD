# ld3d12view

A native Windows **Direct3D 12** USD viewer: the Windows counterpart of
[`lmetalview`](../lmetalview/README.md). Drag a `.usd` / `.usda` / `.usdc` /
`.usdz` file onto the window (or pass it on the command line) and get a shaded,
framed preview with a timeline, shadows, image-based lighting, skinning and
playback.

It reuses lmetalview's portable C++ core (USD load, conversion to a flat preview
scene, orbit camera) and replaces the Swift/Metal side with a small C++
Direct3D 12 renderer. The window, input and HUD come from the vendored
[lightui](../common/lightui/README.lightusd.md); D3D12 renders straight onto
lightui's `HWND`.

```bat
:: from a "x64 Native Tools Command Prompt for VS 2022" (or any shell where
:: cl.exe and the Windows SDK are on the path)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DLIGHTUSD_NATIVE_PRODUCT=next ^
      -DLIGHTUSD_NEXT_BUILD_EXAMPLES=ON ^
      -DLIGHTUSD_NEXT_BUILD_LD3D12VIEW=ON
cmake --build build --target ld3d12view

build\src\next\ld3d12view_example\ld3d12view.exe models\suzanne-pbr.usda
```

`LIGHTUSD_NEXT_BUILD_LD3D12VIEW` defaults to `LIGHTUSD_NEXT_BUILD_EXAMPLES`, is
only available on Windows and only in the `next` product. Enabling it also
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
* `--debug` enables the D3D12 debug layer. That needs the "Graphics Tools"
  optional Windows feature; without it the flag is silently ignored.

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
| `C` | Switch to the scene's authored camera (any mouse navigation switches back) |
| Esc | Cancel a running load; otherwise quit |
| Drag and drop | Open the dropped file (the first one if several) |

The HUD's bottom bar (animated scenes only) has a play/pause button, a timeline
you can click or drag to scrub, and a time readout. The top-left panel shows the
file name, mesh count, time codes per second and frame rate. While a file loads,
a centred card shows its name, the current phase and a progress bar.

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
| `--authored-camera` | Start on the scene's first authored camera |

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

## How it works

| Layer | Source | Notes |
| --- | --- | --- |
| USD load + conversion | `lmetalview/core` (`lmv.h`) over `lightusd_next` + `tydra_next` | Shared with the Metal viewer; the same flat scene, packed OpenPBR materials, decoded images, lights and cameras |
| Camera | `examples/common/light3d/camera-nav.*` through `lmv_camera_*` | The `OrbitCamera` lusdview and lmetalview use; 0..1 depth projection |
| Window, input, HUD | vendored `examples/common/lightui` | `lui_window_native_handle()` gives the `HWND`; `lui_window_set_external_render()` stops lightui painting over D3D12 |
| Device + swapchain | `d3d12/device.*` | One direct queue, flip-model swapchain, 3 buffers, per-buffer allocators and fences, resize, synchronous capture |
| Scene upload | `d3d12/scene_gpu.*` | Vertex/index buffers, materials (structured buffer), lights, textures, shadow map, skin/morph data |
| Mesh shading | `d3d12/mesh_renderer.*` | One HLSL source compiled at startup: lit pass, shadow pass, blend pass |
| Points and curves | `d3d12/points_lines.*` | Camera-facing splats and line ribbons |
| Background | `d3d12/background.*` | Dome environment or a gradient |
| HUD | `hud.*`, `d3d12/hud_renderer.*` | lightui widgets drawn into a CPU surface, composited by one fullscreen pass |
| Loading | `loader.*` | Worker thread around `lmv_load` with progress and cancel |

**Frame order:** background → shadow pass (one directional caster) → opaque and
masked meshes → points/curves → blended meshes sorted back to front → grid → HUD.

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
| Textures: 8-bit sRGB/linear, float EXR, KTX2 BC1/BC3/BC5/BC6H/BC7 | yes, with a full mip chain (generated on the CPU for uncompressed images) |
| UDIM | yes: one texture array per UDIM texture plus a 10x10 tile table; missing tiles render magenta |
| Ptex | yes: atlas image plus per-face rectangles and per-vertex face ids |
| Wrap modes (repeat, clamp, mirror, black), UV offset/scale/rotation, channel select, value scale/bias | yes |
| Authored lights (distant, spot, point; rect/disk/sphere as point lights) or a three-point studio rig | yes |
| Dome light: SH9 diffuse + roughness-blurred reflections + background | yes |
| Directional shadow map (2048², 3×3 PCF), alpha-tested for cutouts | one caster (first directional light) |
| Alpha: opaque, mask (cutout), blend | yes; blended draws are sorted per draw, not per triangle |
| Skinning (4 influences) and blend shapes | yes, in the vertex shader |
| PointInstancer / native instances | yes (expanded by the core) |
| UsdGeomPoints / particles, curves | yes (discs and ribbons with a 1 px minimum size) |

A connected texture **replaces** the material's constant colour (base and
emission). The packed constant is only a fallback — UsdPreviewSurface leaves its
0.18 default there when `diffuseColor` is textured — so multiplying by it would
darken every textured material.

## Limitations

* One shadow caster, no cascades; blended surfaces do not cast shadows.
* Blending is in encoded (sRGB) space and sorted per draw by the bounding-box
  centre, so overlapping or very large blended meshes can order wrongly. There
  is no order-independent transparency.
* Transmission refracts only the environment, never other objects.
* Float textures are uploaded as 32-bit floats and mips are box-filtered in
  encoded space.
* BC textures whose size is not a multiple of 4 are not uploaded (treated as
  unbound).
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

All registered by CTest when the next tests are enabled, all on the WARP adapter
so they run without a GPU. The input and loading tests post real window messages
(`WM_DROPFILES`, `WM_KEYDOWN`, mouse clicks, `WM_SIZE`), so they exercise the
window procedure, not just the viewer logic.

```bat
ctest --test-dir build -R "ld3d12view|lmetalview" --output-on-failure
```

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
`lmv_scene_set_time`, so `progress_user` must outlive the scene.
