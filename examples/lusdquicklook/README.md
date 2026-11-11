# lusdquicklook

A minimal, portable USD Quick Look viewer: point it at a folder, click a
`.usd*` file, get a shaded preview.

Where `examples/lusdview` is the full-featured viewer (GL + Vulkan raster,
Vulkan/CUDA/HIP ray tracing, ImGui, GLFW, gigabyte scenes), this is the opposite
end of the scale. It answers "what is in this file?" It runs with **no GPU**, it
holds a **bounded memory budget**, and it degrades — rather than dies — on files
too large to preview.

```
cmake -B build -DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_WITH_TYDRA=ON \
               -DLIGHTUSD_BUILD_QUICKLOOK=ON
cmake --build build --target lusdquicklook -j16

./build/lusdquicklook ~/assets            # browse a folder
./build/lusdquicklook model.usdz          # open one file
./build/lusdquicklook model.usdz --screenshot out.png   # headless

./build/lusdquicklook model.usdz --shading-mode normal   # a debug AOV
./build/lusdquicklook model.usdz --env studio.png        # light it with an HDRI
./build/lusdquicklook model.usdz --max-mem 256 --max-gpu-mem 512
```

Interactive: drag to orbit, middle/right-drag to pan, wheel to dolly,
**double-click to re-centre the orbit on a surface**. `f` frames everything,
`shift+F` frames the last picked mesh, `.` re-centres on it again. `1`–`7`
switch shading mode, `g` cycles the backend, `t` toggles the image browser,
`r` refreshes the folder. The toolbar carries the same shading-mode and backend
choices as dropdowns, and the status bar always names the backend that is
actually live.

Camera motion is damped, and picking re-aims without moving the eye — the image
does not lurch, but everything after it orbits around what you clicked. The
damper *snaps* to its goal below an epsilon rather than merely approaching it,
because the event loop treats a moving camera as work and would otherwise never
go idle. Smoothing is forced off under `--screenshot` and by `--no-smoothing`,
so headless output can never depend on how many frames happened to elapse.

`LIGHTUSD_BUILD_QUICKLOOK` is independent of `LIGHTUSD_BUILD_GUI_VIEWER`: this
target shares no code with lusdview and pulls in no window-system or GPU
dependency.

**One caveat to that independence.** `LIGHTUSD_BUILD_QUICKLOOK=ON` force-sets
`LIGHTUSD_WITH_TEXTOOLS=OFF` (top-level `CMakeLists.txt`) to keep this target
lean — but textools also supplies lusdview's envmap/IBL path, so enabling
quicklook silently costs *lusdview* its dome lighting, and
`lusdview-dome-orientation` fails. Build both with
`-DLIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS=ON`. Note that the `FORCE` writes the
cache, so an existing build directory also needs `-DLIGHTUSD_WITH_TEXTOOLS=ON`
to clear the stale value.

## What it is built on

| Piece | Why |
| --- | --- |
| **lightui** (`examples/common/lightui`) | Software-rendered C99 UI. Its X11 backend `dlopen`s libX11 with its own minimal ABI declarations, so there is no build-time or link-time X dependency — only `libdl`. That property is what makes the app portable and minimal; preserve it. |
| **`src/`** (`lightusd::StageSession`) | Lazy USD loading with progress/preview callbacks that can **abort mid-composition**. The budget is enforced from inside those callbacks, so a stage that would bust the cap is never fully built first. |
| **`src/tydra/`** (`ConvertToSink`) | Streaming conversion keeps one geometry prim live at a time, and `SelectGeometry` lets us decide `Full`/`Proxy`/`Skip` **per mesh** against the running budget. |
| **lightrt** (`src/external/lightrt`) | CPU BVH kernel. Uses the lightusd-local `lrt_tri_scene_build_indexed`, which keeps indexed data instead of materializing a vertex soup. |
| **`src/tydra/` (mem-budget.hh)** | Tracking allocator + process cap, shared with `tools/lusdrender`. |

## Memory budget

`--max-mem <MB>` (default **512**), clamped at startup against what the machine
can actually back. It is split into advisory shares — stage/compose 55%,
geometry 25%, textures 10%, framebuffers + BVH 10% — over a single hard cap that
counts every `PoolAlloc` byte. Allocations past the cap throw `std::bad_alloc`
on the worker, which the loader catches and turns into a degraded preview.

Degradation ladder, each rung announced in the status bar:

1. Full composed preview with textures.
2. Textures dropped (count or byte cap reached) → flat base colours.
3. Proxy/bbox geometry for meshes that would not fit.
4. Uncomposed root layer, when composition itself busts the cap.
5. Skipped, with a "too large for quick look (projected X, budget Y)" card.

Rung 5 also has a pre-open form: file size × a per-format expansion factor gives
a projection *before* anything is allocated, so hopeless files never open. That
is what colours a row amber in the file list.

Measured: 54 MB peak RSS at `--max-mem 128` on a textured USDZ; the ctest
asserts it.

## Renderers

**CPU ray tracer** (default, always available). One BLAS per mesh plus a TLAS
with identity transforms — positions are baked to world space by the loader, so
the TLAS exists to key a hit back to its mesh, and geometry arriving during a
progressive load only costs a TLAS rebuild. Progression is an eighth-resolution
pass first (the "first pixel"), then full-resolution 32×32 tiles accumulating
one sample at a time; any camera or scene change resets the accumulator.
Shading is direct lighting only: Lambert + GGX from `UsdPreviewSurface` /
OpenPBR, shadow rays, and a camera-locked three-point rig when the stage
authors no lights (which is most asset files).

**GL raster** (optional, `--backend gl`, or `auto` when the budget allows).
Deliberately **offscreen**: lightui has no GL window path and exposes no native
handle, so instead of patching the vendored UI, this creates a headless context
(EGL surfaceless / WGL / CGL, all `dlopen`ed at runtime), renders into an FBO and
reads back into the same blit-ready buffer. One UI code path, zero changes to
lightui, and it works with no compositor. The fragment shader mirrors
`render/shade.cc` — the ctest asserts the two backends agree within 10% of the
viewport; they currently differ by ~1%.

GL setup is lazy: the process does not create a context, compile shaders, or
allocate driver resources until GL is actually selected. `--max-gpu-mem <MB>`
(default **512**) caps the estimated scene, framebuffer and texture residency;
when the driver reports free VRAM, quicklook also keeps a 256 MB driver reserve.
The estimate is rechecked as streamed meshes arrive and on viewport resize.
Exceeding the cap demotes to CPU with the reason in the status bar; a later
smaller asset can retry GL. `auto` stays on the CPU below a 256 MB host budget
because a GL driver costs ~100 MB of RSS that the shared allocator cannot track.

**Switching backends is a runtime operation**, not a startup decision. The
first GL selection reports the device, version, free VRAM, estimate and cap;
the `g` key and toolbar dropdown switch at any time. A GL context lost
mid-session — unusable framebuffer, `GL_OUT_OF_MEMORY`, `GL_CONTEXT_LOST` —
demotes to the CPU tracer with the reason carried into the status bar. The rule
is that a demotion must never be mistakable for a deliberate choice, so
`cpu · 8 threads (gl: no EGL display)` reads differently from a plain `cpu`.

## Materials and lighting

Both backends resolve the same `EvaluatedMaterial` (`render/shade.cc`), and the
GL fragment shader is a transcription of it rather than an independent
implementation. That is the whole basis for the parity the tests enforce.

- **Maps**: base colour, normal, roughness, metallic, emissive and opacity. The
  packed channel comes from tydra's `output_channel`, not an assumed ORM
  layout. Data maps are forced to linear regardless of what the asset's
  metadata claims — assets routinely mistag them, and decoding a roughness map
  through the sRGB curve is silently wrong.
- **Tangents** are generated only for meshes whose material has a normal map.
  At 16 B/vertex, carrying them everywhere would regress a tight `--max-mem`.
  Normal mapping uses that interpolated frame, never screen derivatives, which
  the tracer has no equivalent of.
- **Transparency**: cutout (`opacityThreshold`) discards in GL and skips the hit
  on the CPU — including for shadow rays, since a surface that is not there
  cannot cast one. Blend draws opaque first then sorts back-to-front in GL,
  while the tracer accumulates layers along the ray; overlapping translucent
  shells therefore do *not* match pixel-for-pixel, and the test gives them
  their own tolerance.
- **IBL**: an authored `DomeLight` texture, or `--env <image>`, lights the scene
  and becomes the background. The environment is projected to 9 SH coefficients
  and prefiltered into a 4-level chain **on the CPU**; GL receives the
  coefficients as uniforms and the levels as ordinary textures. It never
  integrates or filters the environment itself, which is what keeps the two
  backends in agreement. `--no-ibl` falls back to the flat hemispheric ambient.

## Shading modes

`--shading-mode`, the toolbar dropdown, or `1`–`7`: `shaded`, `albedo`,
`normal`, `uv`, `roughness`, `metallic`, `depth`. Everything but `shaded`
bypasses lighting and shows one input to the shading model directly, on a black
background.

These are defined once in `ShadeAov()` and mirrored in GLSL. Every term is
plain arithmetic on values both sides already have — depth comes from the
eye-to-hit distance and the camera clip range, never `gl_FragCoord.z`, because
the tracer has no depth buffer to match. They carry no lighting, which makes
them a *stronger* parity signal than the shaded image, and the test holds them
to a tighter tolerance accordingly.

## Known limits

- **No shadow maps in the GL path.** That is the main visible difference from
  the CPU renderer, and the reason the shaded parity bar is looser than the
  debug-AOV one.
- **Overlapping blended surfaces do not match between backends** (sorted vs
  layered). Non-overlapping alpha modes do.
- **Meshes only.** Points, curves and PointInstancers are skipped. The
  `lrt_tlas` instancing needed for PointInstancer prototypes is already in place,
  so this is wiring, not architecture.
- **No animation, skinning or blend shapes.** One time sample.
- **Textures are downsampled to ≤512²** at decode time (environments to ≤512
  wide). No UDIM, no Ptex, no clearcoat or sheen.
- **No wireframe / triangle-density AOV.** GL 3.3 needs a geometry shader or
  de-indexed meshes for barycentrics; poor value for a previewer.
- **No thumbnails in the file list.** The list shows names and sizes.
- **macOS is untested.** The Cocoa platform backend and the CGL context are
  wired and compiled, but nobody has run them.

## Testing

```
ctest -R lusdquicklook --output-on-failure
```

When MCP is enabled, `example-lusdquicklook-mcp-protocol` checks the JSON-RPC
surface without a display, while `example-lusdquicklook-mcp-smoke` drives the
live stdio queue under Xvfb and is skipped when no usable Xvfb display exists.

`tests/run-quicklook-smoke.sh` runs entirely headless (no display, no GPU) and
checks: geometry actually draws (each frame is compared against an empty-folder
render — a colour histogram would not catch a blank viewport, since the gradient
alone yields hundreds of colours), the budget bounds real RSS, `--threads 1` and
`--threads 8` are pixel-identical, the three `--backend` modes all produce an
image, a 1 MiB GPU cap refuses before GL allocation and falls back to CPU,
`--verbose` always names the live renderer, and a missing file exits non-zero
with a diagnostic.

Most of it is CPU-vs-GL parity, at deliberately different bars:

| Case | Bar | Why |
| --- | --- | --- |
| debug AOVs, per mode | 0.05 | no lighting to disagree about |
| textured albedo | 0.05 | catches UV orientation and filtering drift |
| alpha modes (non-overlapping) | 0.05 | pure mode correctness |
| IBL | 0.10 | environment built on CPU, uploaded to GL |
| shaded | 0.10 | GL has no shadow maps |
| overlapping blend | 0.20 | sorted vs layered; cannot match exactly |

## MCP control

The interactive viewer can expose a small local Model Context Protocol server.
It keeps protocol threads away from the scene and renderer: tool calls are
queued and executed by the normal UI loop. Start stdio, HTTP, or both:

```
./build_ninja/lusdquicklook model.usdz --mcp-stdio
./build_ninja/lusdquicklook model.usdz --mcp-http=8765
./build_ninja/lusdquicklook model.usdz --mcp
```

The HTTP endpoint is `http://localhost:8765/mcp`. It accepts one JSON-RPC POST
per request. Available tools are `load_usd`, `get_scene_info`, `list_prims`,
`viewport`, `screenshot`, `render_settings`, and `quit`. A screenshot is a PNG
of the quicklook window; `width` and `height` can request a temporary offscreen
size. MCP is intended for a trusted local client and is not authenticated.
It is an interactive mode and cannot be combined with `--screenshot`.

Two checks are structural rather than tolerance-based, because a threshold
cannot express them: every shading mode must differ from every other (a mode
that silently renders nothing would otherwise pass), and a cutout below its
threshold must render **identically to the same scene with that mesh deleted**
— which is what caught cut-away surfaces still casting shadows.

`tests/run-quicklook-gui-smoke.sh` drives the **real GUI** under Xvfb, because
the headless path never touches `lui_window_create`, the event loop or
`lui_window_present`. Both bugs found in that area during development were
invisible to `--screenshot`:

- `lui_window_create` hung forever with no window manager (upstream lightui
  waited for a `ConfigureNotify` that a bare X server never sends). Patched in
  the vendored copy — see `examples/common/lightui/README.lightusd.md`.
- the event loop blocked in `wait_event` before painting its first frame, so
  the window stayed blank until the user moved the mouse.
- `lui_event_t::clicks` was hardcoded to 1 in the X11 backend, so no
  double-click could ever be observed on Linux — which had been silently
  breaking descend-into-directory in the file list. Also patched in the
  vendored copy; that file now documents three local patches, all
  behaviour-preserving and upstreamable.

It also asserts the app actually goes idle when converged (0 CPU ticks over 3s
in practice), which is the "less cpu" half of the brief.

On an NVIDIA host, use the same offscreen wrapper as the main viewer's GPU
debugging guide. This keeps the X server virtual while routing EGL/GLX through
the NVIDIA vendor library:

```
xvfb-run -a -s "-screen 0 800x600x24" env \
  __NV_PRIME_RENDER_OFFLOAD=1 \
  __GLX_VENDOR_LIBRARY_NAME=nvidia \
  __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
  ./build_ninja/lusdquicklook model.usdz --backend gl --frames 4 \
    --size 640x480 --screenshot /tmp/lusdquicklook-nvidia.png --verbose
```

The optional `example-lusdquicklook-nvidia-smoke` test runs this command and
skips with CTest's normal code 77 when Xvfb or a live NVIDIA driver is absent.

Two things worth knowing if you extend the tests:

- Compare the **viewport only** (`tests/pngdiff.py` does). It excludes the file
  list, the toolbar and the status bar, all three of which legitimately differ
  between runs — the status bar prints live process RSS, and the toolbar shows
  the current directory, so two renders of files in *different folders* differ
  by the caption alone. That last one cost a debugging session.
- Use a window at least ~480px wide. The file-list pane has a 160px minimum, so
  a small window leaves almost no 3D area to compare.

`tests/build-and-run-windows.sh` cross-builds with llvm-mingw (or system
mingw-w64) and smoke-tests the result under Wine. It skips cleanly when no cross
toolchain is present, and distinguishes "Wine cannot run any Windows binary
here" from "our binary failed" — an incomplete Wine install fails every `.exe`
identically, which would otherwise look like our bug.

**Status of the Windows port: verified.** The cross-build produces a PE32+
x86-64 `lusdquicklook.exe` (Win32 platform backend, WGL context, CPU tracer),
and running it under Wine loads a USD file, renders and writes a PNG identical
in content to the Linux build — Windows path handling included.

Two things the script handles automatically for the GCC path, both worth knowing
if you build by hand:

- `-Wno-error=maybe-uninitialized`, for a false positive in `src/usdMtlx.cc`
  (pre-existing library code, not this app; the library builds with `-Werror`).
- `-static-libgcc -static-libstdc++ -static`. Without it the `.exe` imports
  `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` and will not
  start unless those sit beside it. A self-contained binary is what "portable"
  should mean here.

**macOS** is written but entirely untested: `platform_cocoa.m` is wired into the
lightui build and `render/gl_context_egl.cc` carries a CGL path, but nobody has
compiled or run either.
