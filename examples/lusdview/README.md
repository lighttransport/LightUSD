# lusdview

An interactive USD viewer example for lightusd, rendering with **OpenGL 3.3** and
**Vulkan** behind a common backend interface. It loads a USD file
(`.usd/.usda/.usdc/.usdz`), converts it with the Tydra `RenderScene` API and
displays it with an ImGui docking UI.

Authored stereo pairs can be captured natively side by side from GL or Vulkan:

```sh
./build_ninja/lusdview --headless --backend vk --frames 1 --stereo \
  --camera Left --size 640x480 --screenshot stereo.ppm scene.usda
```

In a window, `--stereo` composes both authored eyes into the live viewport at
display resolution. The same compositor works with the legacy and next loaders.

The companion tool supplies the same workflow for external tracing modes
(`--size` is the per-eye resolution):

```sh
python3 examples/lusdview/render-stereo.py \
  --lusdview ./build_ninja/lusdview --next --size 640x480 \
  --output stereo.ppm scene.usda
```

Authored camera shutters can likewise drive deterministic raster motion exports:

```sh
./build_ninja/lusdview --next --backend vk --camera Shot --time 24 \
  --pt-motion-segments 8 scene.usda
```

When paused, native GL/Vulkan raster evaluates one midpoint shutter pose per
frame and holds the completed linear-light average. `--no-raster-motion` keeps
the selected timeline pose unchanged for external sampling or debugging. The
companion tool performs the same schedule in separate viewer processes:

```sh
python3 examples/lusdview/render-motion.py \
  --lusdview ./build_ninja/lusdview --next --backend vk --camera Shot \
  --time 24 --segments 8 --size 1280x720 --output motion.ppm scene.usda
```

## Features

- **USD composition with optional lazy payloads** — `.usd/.usda/.usdc` files are composed
  on load (subLayers / references / payload / inherits / variants / specializes,
  LIVRPS order). Payloads load by default so the initial scene is complete. With
  `--defer-payloads`, the scene opens showing non-payload content and the **Payloads**
  panel lists deferred payloads with per-prim **Load** buttons (plus
  **File ▸ Load All Payloads**); loading recomposes asynchronously on the worker
  thread. Headless/`--frames` runs load payloads eagerly so screenshots are
  complete. `--load-payloads` explicitly selects the default eager behavior;
  `--no-composition` loads the root layer only (legacy fast path, also used for
  `.usdz` archives for now). `--defer-references` extends the same lazy machinery
  to `references` arcs (the **Payloads** panel's **Arc** column shows
  `payload`/`reference`); it is **opt-in only and non-standard** — USD assumes
  references always resolve, so most scene content stays unloaded until you
  request it. Deferred references load on demand exactly like payloads (one
  whitelist drives both: loading a prim resolves its payloads and references).

- **Animation playback (Timeline panel)** — a **Timeline** panel docked along the
  bottom of the viewport plays time-sampled scenes. It reads `startTimeCode` /
  `endTimeCode` / `timeCodesPerSecond` from the stage and offers **Play/Pause**,
  **Stop** (reset to start), a frame **scrubber**, **Loop** and **Speed**. The
  viewer owns the playback clock; each new time re-evaluates the scene
  **asynchronously** (Tydra at `env.timecode`) on the worker thread and the
  request is **coalesced** (only one re-eval in flight — playback runs as fast as
  conversion sustains and never blocks the UI). Re-evaluation skips texture decode
  (`load_texture_assets=false`) and reuses the initial load's materials/textures,
  so only geometry/transforms are rebuilt. This animates **time-sampled
  transforms** (`xformOp` timeSamples), **deforming meshes** (time-sampled
  `points`), **value clips**, and **skeletal + blendshape animation**.
  Skinning (`skinning.cc`) runs on the **GPU by default** (`--skinning
  auto|cpu|gpu`): the rest mesh is uploaded once with per-vertex joint
  indices/weights, and each posed frame the viewer computes the skinning
  matrices (`inverse(bind)·posedWorld`, joint poses from the animation /
  blendshape weights from the stage's `SkelAnimation`, which Tydra does not
  emit), uploads them as an RGBA32F **bone-matrix texture**, and the vertex
  shader does linear-blend skinning. **Blendshapes** are applied in the **GPU
  vertex shader** too (before skinning): each target's primary + in-between
  samples become sparse per-vertex "channels", and a weight change uploads only a
  tiny per-channel coefficient array — no VBO re-upload. Deltas are stored
  half-precision; the shader skips the wide fetch for inactive channels (facial
  rigs: only a handful of 64+ targets active per frame). With displacement the
  morph + skin also run in the **GPU-tessellation** vertex stage, so deformed
  meshes tessellate the posed surface. The ray-tracing backends keep a CPU morph
  (baked into the traced geometry). **Animated node transforms** (a moving SkelRoot /
  parent Xform) are posed on the GPU path too: the affected mesh world matrices
  are re-evaluated from the stage each frame (`BuildXformNodeFromStage`, no
  geometry re-pack). The **CPU** path (`SkinPointsLBS`, full re-pose + upload) is
  the fallback, used when the renderer lacks GPU skinning or for the
  **ray-tracing** technique (whose BLAS needs baked vertices). For a static frame
  at a specific time (e.g. a headless screenshot), use `--time CODE` (alias
  `--frame`), which bakes the posed geometry too.

- **Dockable Unity-like layout** (Dear ImGui docking branch):
  - **Hierarchy** browser — the Stage prim tree (name + type), with an optional
    RenderScene-node view. The selected prim path is shown at the top, and
    double-clicking a hierarchy item frames that prim/subtree in the viewport.
    Selected paths are also shown as clickable breadcrumbs so you can jump back
    to ancestor prims quickly.
  - **Inspector** — a 2-column table of the selected prim's properties/values
    (via `tydra::GetPropertyNames` / `GetProperty`).
  - **Camera** — live eye/target/yaw/pitch/distance readout, preset view
    buttons, bookmark save/load controls, and interactive navigation tuning.
  - **Viewport** — the 3D scene rendered to an offscreen target and shown as an
    `ImGui::Image`.
  - **Stats** — fps, mesh/triangle/material/texture counts, scene bounds, and a
    list of anything skipped (UDIM/undecoded textures, empty meshes).
  - **Search boxes** in the Hierarchy (filters prims by name/type/path, keeping
    matches' ancestors and auto-expanding), the Inspector (filters properties by
    name/value), and the Stage metadata panel (filters by key/value).
- **Maya-style navigation & selection** (only when the viewport is hovered):
  - **Persistent layout** — docking plus native window position/size are saved
    to `imgui.ini` and restored on the next run. A clean 4K first run starts at
    2560×1600; `--size` and configured `window_size` remain explicit overrides.
  - `Alt + LMB` orbit, `Alt + MMB` pan, `Alt + RMB` / **mouse wheel** dolly.
  - `W` / `S` moves both camera eye and orbit pivot forward/backward for
    right-handed mouse navigation; `Up` / `Down` are aliases and `Shift`
    increases the step.
  - `V` cycles shaded, wireframe-only, and wireframe-with-shading views.
  - `N` cycles the master navigation sensitivity through `0.25x`, `0.5x`,
    `1x`, and `1.5x`; the Camera panel also accepts a precise numeric scale.
    Mouse orbit, pan, and dolly use a calmer 65% base response.
  - **Navigation HUD** — an in-viewport cheatsheet/status overlay is shown by
    default and can be toggled with `F1`.
  - **Click to pick** — a plain left click selects the nearest mesh under the
    cursor (world-space ray vs. per-mesh AABB then ray/triangle), updating the
    hierarchy highlight, inspector and selection bbox.
  - **Hierarchy sync** — viewport or MCP selection auto-expands and scrolls the
    hierarchy to the focused prim when possible.
  - **Selection stepping** — `[` / `]` move to the previous/next visible
    renderable selection, making scene review faster without clicking.
  - **Selection history** — `Alt+Left` / `Alt+Right` move backward and forward
    through previously focused prims.
  - `F` frames (fits) the **selected** mesh (falls back to the whole scene);
    `A` frames the whole scene.
  - **Preset views** — `0` home, `5` isometric, `1 / Shift+1` front/back,
    `3 / Shift+3` right/left, `7 / Shift+7` top/bottom.
  - **Camera bookmarks** — `Ctrl+1..3` recall a saved view and
    `Ctrl+Shift+1..3` save the current camera/selection to a bookmark slot.
  - **Safe quit shortcuts** — `Ctrl+Q` exits immediately; Escape must be
    pressed twice within 600 ms, so a single Escape cannot close the viewer.
  - **Hide family** (Maya 2016+): `H` toggle-hide the selection, `Ctrl+H` hide,
    `Shift+H` show, `Alt+H` isolate (hide everything else). **View ▸ Unhide all**
    restores. Visibility is viewer-side (raster paths; the RT path traces all
    meshes) and is not written back to the USD `visibility` attribute.
  - **Selection overlay controls** — `Shift+K` cycles off, bounds only,
    depth-tested wireframe, and X-ray wireframe. `K` quickly hides or restores
    the last visible mode without clearing the selection. X-ray deliberately
    shows hidden edges for topology debugging.
- **Skeleton (UsdSkel) display** — the joint hierarchy is drawn as world-space
  bone line segments (parent→child) plus a small cross per joint, as an **X-ray
  overlay** (depth-test disabled, so bones stay visible through solid geometry).
  Joint bind poses are placed by the skinned mesh's world matrix (so bones align
  with the mesh even across an up-axis conversion). Toggle from **View ▸ Skeleton**.
- **Material textures** on both raster backends: base color, metal/rough,
  normal, emissive, and separate scalar opacity masks. Opacity honors selected
  channels, scale/bias, UV transforms and UV sets; ordinary and UDIM masks are
  supported. Varying `primvars:displayOpacity` modulates raster and RT output.
- **Native Ptex materials** — rectangular, bounded atlases preserve texture-
  local face IDs and intrinsic face UVs. Base color, metal/rough, normal,
  emissive, opacity, occlusion, specular color, and coat weight/color/roughness
  are sampled on OpenGL, Vulkan raster, Vulkan ray-query, and CUDA/HIP paths;
  displacement is baked once into the traced/raster geometry. Atlas residency
  is capped by the configured GPU budget, with representative-face fallback
  and render-report counters for downsampling, truncation, and degradation.
- OpenPBR surface lobes (transmission, subsurface, sheen/fuzz, anisotropy,
  thin-film and dispersion) are preserved in the canonical material record and
  evaluated by the real-time preview backends. Volume output is preserved and
  evaluated through a homogeneous realtime fallback, with the same packed
  material values available to raster and ray-tracing paths.
- **Live OpenPBR material editor** — the dedicated dock panel follows the
  selected mesh, exposes constant base/specular/transmission/subsurface/coat/
  fuzz/film/emission/opacity inputs, and rerenders on every adjustment. Vulkan,
  CUDA, and HIP update material buffers without rebuilding geometry or BVHs;
  texture- and nodegraph-connected controls are disabled and identified. The
  material combo also exposes dual-authored PreviewSurface/OpenPBR materials;
  **Use constant OpenPBR** creates a session-local, connection-free OpenPBR
  preview for parameter studies such as the StandardShaderBall neutral shell.
  Overrides do not modify the source USD.
- **Authored USD lighting** on both raster backends: up to 16 supported direct
  lights are evaluated in stage order with diffuse/specular multipliers,
  shaping cones, and per-mesh `collection:lightLink` masks, alongside
  DomeLight IBL. DistantLight and finite sphere/point, rect, disk, and cylinder
  records share the same packed GL/Vulkan interface; finite area shapes use
  deterministic eight-point rectangle/disk/cylinder samples in raster and
  stochastic samples in RT. GeometryLight targets are resolved to emissive
  triangle ranges in the software-BVH path, IES profiles are parsed and packed
  into the shared light record, and PortalLight uses deterministic rectangular
  opening samples. Raster GeometryLight uses eight area-stratified samples from
  its resolved emissive mesh and reports unresolved sources. Vulkan RT
  scenes carrying these extended payloads use the hardware shader when
  available, with compute-BVH as the capability fallback. Native Points/Curves currently receive all packed direct lights
  because their light-link carrier mapping has not landed yet.
- **UsdGeomPoints and curves (`--next`)** — Points plus BasisCurves,
  HermiteCurves, and NurbsCurves retain evaluated widths, displayColor,
  displayOpacity, material binding, purpose, animation time, and transforms.
  OpenGL and Vulkan raster draw world-sized camera-facing point discs and
  tessellated curve ribbons; Vulkan ray query and CUDA/HIP trace width-aware
  solid octahedron/tube proxies. Under a bounded `--max-tris` RT profile,
  legacy carriers use compact analytic splats (`max-tris / 8` samples) instead
  of materializing all triangle proxies. Viewport click-picking for these carriers
  remains a roadmap item; mesh picking is unchanged.
- Gaussian splat point carriers use analytic ellipses on Vulkan ray query and
  CUDA/HIP. Their BVH metadata is chunked; when point records exceed
  the configured GPU budget, one point/BVH chunk is uploaded and traced at a
  time, with nearest-depth/color reduction on CUDA/HIP. Vulkan ray query keeps
  analytic records resident; if its point-buffer ceiling is exceeded, it skips
  the temporary analytic descriptor/BVH build and uses the raster splat
  fallback, which rejects off-screen samples before expanding camera-facing
  quads. `LUSDVIEW_GAUSSIAN_GPU_BUDGET_MB` can rehearse a smaller device;
  otherwise the backend derives a ceiling from free VRAM with headroom.
  OpenUSD's float and half attribute families (`positions`/`positionsh`,
  `scales`/`scalesh`, orientations, opacities, and spherical harmonics) share
  the same bounded path, with float-over-half precedence. Points/Curves-only
  Vulkan RT scenes render their carrier fallback even when no triangle TLAS is
  present.
- **Surface displacement** (`UsdPreviewSurface inputs:displacement` — constant or
  a height texture, honoring the `UsdUVTexture` `scale`/`bias`). The **raster**
  paths displace in the vertex shader (coarse, no extra geometry), with an opt-in
  **GPU-tessellation** path (GL 4.1 / Vulkan tessellation shaders) for adaptive
  sub-triangle detail; **View ▸ Displacement** toggles it and exposes live scale +
  max-tess sliders (both live on GL and Vulkan). The tessellation vertex stage
  applies blendshape morph **and** skinning, so morphed/skinned displaced meshes
  tessellate the deformed surface (not the rest pose). The **ray-tracing** backends
  (Vulkan ray query, CUDA) intersect real triangles, so the displacement is baked
  into the geometry before the BLAS/TLAS is built. Geometric normals are used on
  displaced surfaces so the new detail shades correctly.
- **Subdivision-surface pre-tessellation** — explicitly authored Catmull-Clark, Loop and
  bilinear meshes are uniformly refined with `tinysubdiv` before raster or RT
  geometry is built. The default level is 2; use `--subdivision-level 0` to
  disable it or change **Scene ▸ Subdivision surfaces ▸ Pre-tessellation level**
  to rebuild the live scene. The default next loader refines face-varying UVs
  and recomputes normals along with the topology; the legacy loader additionally
  supports refined skin and blend-shape primvars.
- **Production path tracing** — `--path-trace` selects a separate multi-bounce
  integrator while preserving the fast `--rt` preview. Vulkan uses hardware ray
  query when the selected adapter exposes it; `--cuda` and `--hip` run the same
  software-BVH transport kernel through their runtime-loaded GPU APIs. The
  integrator evaluates the canonical OpenPBR/MaterialX graph used by
  OpenChessSet, including textured base/metal/roughness/emission, transmission,
  emissive hits, Russian roulette, IOR-aware GGX direct-light visibility, and a
  radius-aware subsurface interior random walk. Direct-light reservoir sampling
  skips inactive, unlinked, and dome lights instead of spending shadow rays on
  them. USD camera optics and
  `--f-stop`/`--focus-distance` drive deterministic thin-lens depth of field.
  Shaded GL and Vulkan raster views apply the same lens values as a deterministic
  screen-space depth-of-field pass; pinhole cameras and diagnostic AOVs bypass it.
  Named USD cameras also apply up to eight authored `clippingPlanes` equations
  to ordinary, tessellated, and instanced meshes in both GL and Vulkan raster.
  After picking an object, **Camera ▸ Depth of field ▸ Focus selected** moves
  the focal plane to the clicked surface (or to the bounds center for hierarchy
  and marquee selections). The persistent MCP `viewport` operation exposes the
  same action as `op=focus_dof` after `pick` or `set_focus`. The Camera panel
  also exposes live enable, focus-distance, 50 mm-equivalent F-stop, and exact
  stage-unit aperture-radius controls.
  Interactive and final presets expose sample/depth/seed controls; final uses a
  built-in variance-guided, row-parallel à-trous filter. Robust display-side
  firefly rejection also works with denoising disabled and is adjustable with
  `--pt-firefly-clamp` (lower is stronger, `0` disables it). All three GPU
  backends can stop at coarse checkpoints after
  scene-linear convergence; `--pt-variance 0` keeps the maximum-throughput exact
  sample-count path. `--linear-output image.exr` writes the
  unfiltered scene-linear half-float accumulation independently from the
  tone-mapped PNG/PPM display.
- **Runtime backend switch** — nine render techniques, switchable **live, without
  restarting**, from **View ▸ Render Technique** (or their startup CLI flags):
  GL Raster, Vulkan Raster, Vulkan RT, CUDA RT, HIP RT, CPU RT, plus Vulkan,
  CUDA, and HIP Path Trace. GL raster and
  Vulkan raster are window owners (switching between them tears down and rebuilds
  the renderer, then re-uploads the scene); Vulkan RT is a same-instance toggle on
  the Vulkan renderer (no reupload); CUDA RT, HIP RT, and CPU RT trace externally
  and composite the result over whichever window owner is active. A failed switch
  (e.g. CUDA RT with no NVIDIA GPU present) logs a warning and reverts to the
  previous technique instead of crashing; CUDA/HIP entries gray out in the menu
  once a switch attempt has proven the device unavailable. **CPU RT** has its own
  dedicated keybinding, **`R`** (viewport hovered), which toggles it on/off and
  restores whichever technique was active before.
  - **Vulkan ray tracing (ray query)** is enabled only when the GPU exposes
    `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+ `bufferDeviceAddress`)
    and the project was built with an RT-capable `glslangValidator`; otherwise the
    viewer transparently falls back to Vulkan rasterization. The RT path builds a
    BLAS per mesh + a TLAS, then a compute shader (`vk/shaders/raytrace.comp`)
    traces primary + shadow rays with `rayQueryEXT` into a storage image that is
    copied into the displayed target. Shading evaluates the shared GGX/coat
    material constants, authored USD lights, DomeLight IBL, and per-light
    visibility rays. Start in RT with `--rt`. RT uses a ray-tracer-friendly
    conversion (no single-index dedup; `build_vertex_indices=false`); displacement
    is baked into the traced geometry. RT samples the six semantic material
    textures, including separate opacity, UV1/transforms, compressed-only
    sources, and sparse UDIMs; full ordinary, compressed, and UDIM mip chains use
    ray-footprint/projected-triangle LOD with trilinear filtering. Alpha-mask
    texels are rejected during traversal. Varying displayOpacity and material
    constants are also supported, including per-face materials bound through
    GeomSubsets. Helper lines, grid/axes, skeletons, volumes, and selection
    highlights are composited over the traced image without recreating the
    renderer.
  - **CUDA RT** / **HIP RT** flatten the scene to world-space triangles (instances
    expanded) and path-trace it (primary + one shadow ray) on the CUDA driver API
    (via cuew, NVRTC-compiled at runtime) or HIP/ROCm (via hipew, hiprtc). `--cuda`
    / `--hip` without `--headless` now drive the viewport **interactively** (build
    once, retrace on the orbit camera) instead of only writing a one-shot
    screenshot — that one-shot path is still used for `--headless --screenshot`.
  - **CPU RT** (`--cpu-rt`) flattens the scene the same way (reusing the CUDA/HIP
    tracers' `BuildHostScene` flattener) and traces it on CPU worker threads with
    the vendored `lightrt_c` BVH library (also used by `tools/lusdrender`), one
    sample per pixel with flat Lambertian + hard-shadow shading. Headless
    `--cpu-rt --screenshot` renders a one-shot CPU image, matching the CUDA/HIP
    capture workflow and making the CPU path usable in display-free tests.
- **Responsive, non-freezing UI**: USD parse → Tydra convert → DrawScene build
  run on a **worker thread** with a live progress modal; the GPU upload happens
  on the render thread when the worker finishes. The window stays interactive
  while a large file loads.
- **mmap + streaming/progressive loading** for interactivity:
  - The USDC file is **memory-mapped** (`io::MMapFile` + `LoadUSDFromMemory`,
    mapping kept alive for the Stage); uncompressed arrays stay zero-copy.
  - The Tydra conversion runs through the **streaming API**
    (`RenderSceneConverter::ConvertToRenderSceneStreaming`): the `DrawScene` is
    built **incrementally as each element is produced** — mesh geometry is
    interleaved as each mesh converts, world placement is applied when the node
    hierarchy is built, and textures/materials are decoded on completion (see
    `mesh_build.cc: BuildDrawSceneStreaming`). It produces a `DrawScene` identical
    to the monolithic `ConvertToRenderScene` + `BuildDrawScene` path.
  - Non-threaded OpenGL and Vulkan raster loads use a bounded producer/consumer
    stream: meshes are **streamed to the GPU progressively** (time-budgeted per
    frame) so geometry pops in incrementally, then **textures stream in lazily**
    (surfaces show base color until their texture lands). The render loop never
    stalls on one big upload. RT/CUDA/HIP paths retain their scene-owned upload
    paths, and headless `--frames` drains the stream synchronously for
    deterministic capture. Later payload, variant, and retained-layer edits
    prepare a `tydra::next::RenderSession` candidate on the loader worker and
    publish it through the same atomic GL/Vulkan transaction on the render
    thread. `LUSDVIEW_RENDER_SESSION_MAX_MB` bounds this prepared scene (256 MiB
    by default; `0` disables it for oversized scenes). PointInstancer transform,
    displayColor, displayOpacity, and count edits retain stable mesh slots; layout
    and opacity-class changes replace only the affected slot.
  - With an authored `--camera` (including the Caldera profile default), mesh
    admission inherits model/district extents and ranks conservative in-view
    bounds first. Static material batches are capped at 512K vertices during
    interactive streaming, so one scene-wide batch cannot delay the first real
    raster frame; exact frustum and projected-size LOD still run per frame.
  - The interactive **ALab** profile treats baked procedurals as refinement:
    loaded Curves use one tessellation sample per span and retain at most 64
    curve prims / 100,000 complete strands. Textures start at a 512px edge cap,
    use online device-native block compression, and skip eager mip generation.
    Explicit texture CLI choices and headless captures retain their requested
    quality.
    Ordinary filesystem textures reserve stable material slots without reading
    pixels; after geometry publication, up to four workers decode, resize, and
    CPU-encode GPU-native blocks while the viewport remains interactive. Each
    completed texture uploads independently. USDZ, UDIM, Ptex, and
    kept-compressed KTX2 retain their specialized synchronous readers for now.
    Native RT texture tables apply the same decoded-byte budget (normally 25%
    of probed device VRAM), deduplicate identical used images, and leave
    over-budget textures on the backend fallback image instead of risking an
    unbounded RT staging allocation. CUDA and HIP receive the same cap when
    building their host-side RT scene tables.
- **Interruptible / budgeted loading** so huge scenes can't freeze the app or
  thrash VRAM:
  - **Cancel** button in the loading modal (and an automatic conversion
    **time budget**) aborts mid-conversion.
  - **Triangle / vertex-memory budget** stops building once the cap is hit and
    marks the scene *truncated* (editable in the Stats > Render budget panel).
  - The probed host/VRAM resource budget also applies to ordinary `--next`
    loads, not only named large-scene profiles: composition retention, GPU
    geometry, upload staging, and decoded textures are capped before
    materialization. If authored composition exceeds the host cap, lusdview
    reports a bounded load error instead of allowing an OOM-sized allocation.
- **HiDPI-ready appearance**: Maya-like dark theme, **Cascadia Mono** font
  (embedded, `examples/common/cascadia_mono.h`), and a UI scale that defaults to
  **2×** for 4K panels (font 16 → 32 px, crisp — the scale is baked into the font
  size, not `FontGlobalScale`). Tune with `--ui-scale S`, or load `font_size`,
  `window_scale`, and `window_size` from a JSON config file.
- **Rendering foundation** copied from
  [light3d](https://github.com/lighttransport/light3d) into
  `examples/common/light3d` (math, camera, material incl. GLSL shaders, texture).
- Gracefully **skips** what the GPU path can't show directly (e.g. raw UDIM,
  undecoded textures) instead of failing.

## Build

GUI deps (OpenGL/GLFW, optionally Vulkan) are opt-in so headless CI is
unaffected:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_WITH_TYDRA=ON \
  -DLIGHTUSD_BUILD_GUI_VIEWER=ON
cmake --build build -j16 --target lusdview
```

- **Vulkan** is enabled automatically when `find_package(Vulkan)` succeeds (needs
  `glslangValidator` to compile the shaders). Force a GL-only build with
  `-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`.
- **Vulkan ray tracing** additionally needs a `glslangValidator` that supports
  `GL_EXT_ray_query` (≈ glslang ≥ 11 / a Vulkan SDK build). CMake probes the
  compiler at configure time and only enables RT if it can compile
  `vk/shaders/raytrace.comp`; otherwise it prints a notice and the Vulkan backend
  uses rasterization. If your system glslang is too old, build a local one once
  (Linux):

  ```bash
  examples/common/build-glslang.sh      # clones + installs to examples/common/glslang
  cmake -S . -B build                    # auto-detects it (see the configure log)
  ```

  CMake prefers, in order: `-DLUSDVIEW_GLSLANG=/path`, `examples/common/glslang`,
  then the system compiler.
- The **native file dialog** (File > Open) is enabled when GTK3 is available on
  Linux (always on Windows/macOS); otherwise pass a file on the command line.

## Run

```bash
./build/lusdview models/suzanne-pbr.usda
./build/lusdview --backend vk models/suzanne-pbr.usda
./build/lusdview --rt models/suzanne-pbr.usda          # Vulkan ray tracing (implies --backend vk)

# Headless smoke test / screenshot (renders N frames then exits):
./build/lusdview --frames 5 --screenshot out.ppm models/suzanne-pbr.usda
./build/lusdview --backend vk --rt --frames 8 --screenshot rt.ppm models/suzanne-pbr.usda

# HiDPI: 2x is the default; tune for your display:
./build/lusdview --ui-scale 1.5 model.usdz

# Load startup settings from JSON:
./build/lusdview --config /path/to/lusdview.json model.usdz

# Budget controls (also useful for scripting/CI):
./build/lusdview --max-tris 5000000 model.usdz     # cap geometry (triangles)
./build/lusdview --time-budget 10 model.usdz        # abort conversion after 10s

# USD composition / lazy payloads:
./build/lusdview --load-payloads scene.usda         # compose payloads eagerly
./build/lusdview --defer-payloads scene.usda        # explicitly request lazy payloads
./build/lusdview --defer-references scene.usda       # also defer references (opt-in, non-standard)
./build/lusdview --no-composition scene.usda        # root layer only (no arcs)

# Large-scene startup preview cache (interactive large-scene profiles):
./build/lusdview --large-scene-profile instance-heavy scene.usda
./build/lusdview --large-scene-profile instance-heavy --preview-cache refresh scene.usda
./build/lusdview --preview-cache off scene.usda
./build/lusdview --preview-cache-dir /fast/cache --preview-cache-max-gb 4 scene.usda

# Animation: scrub/play interactively from the Timeline panel, or render a
# static frame at a specific USD time code (headless screenshot of frame 12):
./build/lusdview --frames 4 --time 12 --screenshot frame12.ppm anim.usda

# Headless full-window screenshot (UI + viewport, GL backend):
./build/lusdview --frames 8 --window-shot ui.ppm model.usdz
```

Note: `--frames` loads synchronously (deterministic screenshots); interactive
runs load on the worker thread.

Interactive large-scene profiles cache their compact spatial preview as a
versioned USDC file. A warm run can display that preview while authoritative
composition continues in the background. Cache entries are invalidated when the
root or any physical composition dependency changes; composition policy,
payload/reference choices, variants, parent paths, and time code are part of the
cache key. `auto` is the large-profile default, `refresh` rebuilds the entry,
and `off` disables both reads and writes. The default cache limit is 8 GiB with
oldest-entry eviction.

### Startup config

lusdview can load a JSON startup config automatically from the platform default
location, or from `--config /path/to/config.json`.

- **Linux:** `$XDG_CONFIG_HOME/lusdview/config.json`, else `~/.config/lusdview/config.json`
- **macOS:** `~/Library/Application Support/lusdview/config.json`
- **Windows:** `%APPDATA%\\lusdview\\config.json` (falls back to `%LOCALAPPDATA%`)

The ImGui docking layout is loaded and saved as `imgui.ini` in the same
platform `lusdview` config directory. If an `imgui.ini` exists beside the
lusdview executable, that portable/local layout takes precedence and is also
the save target for that run.

Supported keys (snake_case and kebab-case are both accepted):

```json
{
  "font_size": 24,
  "window_scale": 1.25,
  "window_size": {
    "width": 1600,
    "height": 900
  },
  "navigation": {
    "orbit_sensitivity": 1.0,
    "pan_sensitivity": 1.0,
    "dolly_sensitivity": 1.0,
    "invert_dolly": false
  },
  "composition": true,
  "payload_policy": "defer"
}
```

- `font_size` sets the ImGui font size in pixels and scales widget metrics to
  match.
- `window_scale` scales the default `1280x800` window/composite size.
- `window_size` sets an explicit logical window size and overrides
  `window_scale`.
- `navigation.orbit_sensitivity`, `navigation.pan_sensitivity`, and
  `navigation.dolly_sensitivity` tune camera response.
- `navigation.invert_dolly` flips wheel / Alt+RMB dolly direction.
- `composition` enables/disables USD composition on load (default `true`;
  `--no-composition` overrides).
- `payload_policy` is `"defer"` (lazy) or `"load"` (eager, default);
  `--defer-payloads` / `--load-payloads` override.
- `--ui-scale` still works and overrides the config-derived font/window scaling
  for that run.

### Headless / CI (no physical display)

**Truly windowless (Vulkan, no X server at all)** — `--headless` skips GLFW, the
surface and the swapchain entirely: it renders the frame (and the full ImGui UI)
into an offscreen image on the GPU and reads it back. Nothing touches a display,
so it runs on a bare host with `DISPLAY` unset.

```bash
# 3D viewport screenshot, no display:
./build/lusdview --headless --frames 8 --screenshot out.ppm model.usdz

# Full UI composite (Hierarchy / Viewport / Inspector / Stage), no display:
./build/lusdview --headless --frames 8 --window-shot ui.ppm model.usdz

# Ray tracing works headless too:
./build/lusdview --headless --rt --frames 8 --screenshot rt.ppm model.usdz
```

`--headless` is Vulkan-only (OpenGL needs a window/context) and implies
`--backend vk`; it needs `--frames N` (windowless runs are bounded by frame
count). The composite size comes from `window_size`, or from `1280×800 ×
window_scale` / `--ui-scale` when no explicit size is configured. Unlike the GL
`--window-shot`, the headless `--window-shot` captures the **full UI** (it reads
the Vulkan composite image, not a GL back buffer).

**xvfb-run (either backend, in-memory X server)** — for the OpenGL backend (or
the windowed Vulkan path) on a machine with no display, wrap the run in
`xvfb-run`, which spins up a virtual X server. The GPU still does the rendering;
only window-system integration goes through the virtual display.

```bash
# No X server at all (Vulkan) — preferred when available:
./build/lusdview --headless --frames 8 --screenshot out.ppm model.usdz

# OpenGL offscreen render on a headless host (uses the real GPU, no monitor):
xvfb-run -a -s "-screen 0 1280x800x24" \
  ./build/lusdview --backend gl --frames 8 --screenshot out.ppm model.usdz

# Vulkan via xvfb also works (windowed path):
xvfb-run -a -s "-screen 0 1280x800x24" \
  ./build/lusdview --backend vk --frames 8 --screenshot out.ppm model.usdz
```

- **No-X option:** `--headless` (Vulkan, above) needs no X server at all — prefer
  it on bare hosts. Use `xvfb-run` for the **OpenGL** backend or when you need the
  windowed (swapchain) Vulkan path.
- Use a **24-bit** screen depth (`x24`); the `xvfb-run` default (8-bit) can't
  give GL/Vulkan a usable visual.
- The screenshot PPM is the **3D viewport** at the (clamped) window size — pass a
  smaller `--ui-scale` to make it deterministic regardless of the screen size.
- The GL `--window-shot` grabs the GL back buffer (full UI on the GL backend);
  for windowless Vulkan UI capture use `--headless --window-shot` (above).

## MCP server (drive the viewer from an LLM/agent)

lusdview embeds an **MCP (Model Context Protocol)** server so an LLM/agent can
inspect and control the running viewer. It speaks JSON-RPC 2.0 over two
transports:

For a viewer launched without MCP command-line options, open **Tools > MCP
Server**, edit the hostname and port (default `127.0.0.1:8080`), and choose
**Start server**. The panel can stop and restart the HTTP endpoint without
reloading the scene.

```bash
./build/lusdview --mcp-http=8080 model.usdz   # HTTP: POST JSON-RPC to http://localhost:8080/mcp
./build/lusdview --mcp-stdio    model.usdz     # stdio: JSON-RPC on stdin/stdout (logs on stderr)
./build/lusdview --mcp          model.usdz     # both (HTTP on 8080)
```

Tools (`tools/list` for schemas):

| tool | does |
|---|---|
| `load_usd {path}` / `load_usd {usda}` | load a USD path or inline USDA text (async; poll `get_scene_info`) |
| `get_scene_info` / `get_scene_bbox` | loading state/path, scene generation, filepath, stage revision, retained layer dependencies, mesh/triangle/material/texture counts, up axis, world-space AABB |
| `get_focused_prim` | selected prim: path, vertex/triangle counts, bbox, world matrix, material (id/name/base color/metal/rough) |
| `set_focus {path}` | select a prim by absolute path; returns the same info |
| `viewport {op}` | `orbit`/`pan {dx,dy}`, `dolly`/`forward`/`backward {amount}`, `fit`, `home`, `isometric`, `front`, `back`, `right`, `left`, `top`, `bottom`, `bookmark_save {slot}`, `bookmark_load {slot}`, `set {target,yaw,pitch,distance}`; returns the camera state |
| `list_prims {max?}` | renderable mesh prim paths |
| `load_payloads {paths?}` | load deferred USD payloads (and deferred references under `--defer-references`); omit `paths` = all; async, poll `get_scene_info`, which reports `deferred_payloads` (each with an `arc` field) |
| `reload_layer {path?}` | reload the retained root layer or an exact resolved dependency reported by `get_scene_info`; async and transactionally retains compatible GPU resource slots |
| `timeline {op, time?}` | animation playback: `op` = `play`/`pause`/`stop`/`seek {time}`; async re-eval, poll `get_scene_info` (reports `has_animation`/`time`/`start_time`/`end_time`/`fps`/`playing`) |
| `render_settings {mode?, grid?, adaptive_quality?, target_render_fps?}` | change resettable render state between captures without restarting the viewer; `mode:"picking"` displays the depth-tested mesh/curve/points picking-ID buffer |
| `shader_reload {action, backend?, source?, watch?}` | inspect, explicitly recompile, or watch the active Vulkan GLSL / CUDA NVRTC / HIP hiprtc source while keeping the viewer and GPU scene alive |
| `get_render_stats {}` | report UI/render FPS, threaded-render state, adaptive tier, render scale, and target FPS |
| `screenshot {path}` | capture the current viewport as PNG, JPEG, or PPM (selected by extension) |
| `mouse {type,x,y,...}` | inject viewport-local mouse movement or button events; `type` is `move` or `button`, with `button=0/1/2` and `down=true/false` |
| `pick {x,y,x1?,y1?,select?}` | query one pixel, or pass the opposite `x1,y1` corner to select the prim covering the most depth-tested pixels in a region; returns USD path, carrier kind, and `covered_pixels` |

Example:

```bash
curl -s -XPOST localhost:8080/mcp \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"viewport","arguments":{"op":"fit"}}}'
```

Mouse coordinates are pixels relative to the captured 3D viewport, not the
full application window. A click can be synthesized with a move followed by
separate left-button press and release calls. For deterministic inspection,
`pick` directly reports the carrier at a pixel and can update selection with
`select:true` (the default):

The View menu's **Show selected only** command (`Alt+H`) isolates the selected
prim, including all renderable descendants when an Xform or Scope is selected.
**Show all** restores meshes, curves, and points together.

```bash
curl -s -XPOST localhost:8080/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
       "params":{"name":"pick","arguments":{"x":320,"y":240}}}'
```

In addition to the viewer tools above, the server also exposes the **lightusd
library's own USD tools** (`stage_info`, `prim_list`, `prim_get`, `attr_get`,
`query_prims_by_type`, `search`, `run_script`, composition tools, …) — any tool
name that isn't a viewer tool is forwarded to `tydra::mcp::CallTool` and run
against a snapshot of the currently-loaded Stage. (Query tools see a per-load
snapshot; tool-side edits aren't reflected in the 3D viewport.)

Tool calls are marshalled onto the render thread (GL/VK/camera are
main-thread-only), so they take effect on the next frame. The server is built by
default (deps — `nlohmann/json` + civetweb — are vendored in `src/external`);
disable with `-DLUSDVIEW_ENABLE_MCP=OFF`. The HTTP transport is the minimal
"Streamable HTTP" subset (one JSON response per POST; no SSE).

### Live GPU shader and kernel iteration

Start one long-lived RT/path-trace viewer and let it watch the active backend's
development source:

```bash
./build_ninja/lusdview --backend vk --rt --path-trace \
  --live-shader-reload --mcp-http=8080 model.usdz
```

The default Vulkan source follows the active RT implementation:
`vk/shaders/raytrace.comp` for hardware ray query and
`vk/shaders/raytrace_swbvh.comp` for compute-BVH. CUDA/NVRTC and HIP/hiprtc
share `raytracer_kernel_src.txt`. Packaged builds or alternate working copies
can set an explicit path over MCP. Vulkan invokes `glslc`; set
`LUSDVIEW_GLSLC=/absolute/path/to/glslc` when it is not on `PATH`.

```bash
# Inspect generation, compile time, errors, watch state, and pending work.
curl -s -XPOST localhost:8080/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{
        "name":"shader_reload","arguments":{"action":"status"}}}'

# Compile once. backend may be active, vulkan, cuda, hip, or all.
curl -s -XPOST localhost:8080/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
        "name":"shader_reload","arguments":{"action":"reload",
        "backend":"active"}}}'

# Watch a different source. Saving it triggers a compile within 250 ms.
curl -s -XPOST localhost:8080/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
        "name":"shader_reload","arguments":{"action":"watch",
        "backend":"vulkan","source":"/tmp/raytrace.comp","watch":true}}}'
```

Reload is transactional: lusdview creates and validates a replacement module
and entry point before swapping it. Compilation or pipeline-creation errors are
reported in `last_error`, while the previous generation keeps rendering and all
scene buffers, textures, acceleration structures, camera state, and accumulated
UI state remain resident. Windowed threaded Vulkan reports `pending:true` while
the render thread completes the swap; poll `status` before taking a comparison
`screenshot`. Vulkan's compact variant includes OpenPBR constants, all semantic
textures, and direct MaterialX image/tiledimage routes. Arithmetic or procedural
MaterialX networks promote transactionally to the full interpreter.
Both hardware ray query and compute-BVH support transactional live replacement;
an explicit MCP source remains selected until the client changes it.

CUDA/HIP source must preserve the exported `trace` entry point and its argument
ABI; Vulkan source must preserve the descriptor/push-constant ABI of the active
pipeline. The MCP server is intentionally unauthenticated and can read local
source paths, so expose it only to trusted local clients.

For render regressions, `tests/lusdview/mcp_render_batch.py` consumes a JSON
manifest and keeps one lusdview process alive for all compatible cases. Each
case may supply `path` or inline `usda`, expected scene-info fields,
`render_settings`, a `viewport` operation, and a screenshot name. Backend,
loader, RT implementation, startup environment, and other process-wide options
belong in separate batches. The `lusdview-mcp-render-batch` CTest is the compact
reference: it replaces three inline scenes and captures three render modes from
one Vulkan process. `lusdview-mcp-render-batch-gl-window` runs the same cases
through one real GLFW/OpenGL window under Xvfb. Both record the window and
renderer generations and fail if either resource is recreated within a batch.
The back-face material regression uses the same mechanism to keep one window or
offscreen renderer alive across its four fixture/camera combinations for GL,
Vulkan raster, and Vulkan RT. CUDA/HIP remain isolated because those backends
own and write their test screenshot during process shutdown.

## Architecture

```
USD ──LoadUSDFromFile──▶ Stage ──RenderSceneConverter──▶ tydra::RenderScene
                                                              │
                                            mesh_build.cc: RenderScene ▶ DrawScene
                                            (interleaved verts, submeshes, world
                                             matrices, decoded RGBA8 textures, AABB)
                                                              │
                                  ┌───────────────────────────┴──────────────┐
                            GLRenderer (GL330 uniforms)        VulkanRenderer (push-constants)
                                  └───────────── offscreen target ────────────┘
                                                              │
                                            ImGui::Image in the "Viewport" dock
```

`renderer.hh` is the GL/Vulkan boundary; everything else (loader, mesh build,
camera, GUI, `DrawScene`) is backend-neutral.

Camera `BackPlateAPI:<instance>` schemas are decoded by the next loader when
that camera is selected with `--camera`. Every visible instance becomes a
camera-space textured grid in authored order. Optional alpha is merged into the
plate texture and optional depth drives the grid vertices, so the same ordinary
depth-tested draw stream displays the stack in both OpenGL and Vulkan raster.
Use `tests/run-backplate-display.sh <lusdview> <repo-root>` for the focused
headless Vulkan and Xvfb OpenGL regression.

## Threading model

USD parse, composition, Tydra conversion, and DrawScene construction run on a
CPU worker. Builds include the dedicated render-thread path by default
(`LUSDVIEW_ENABLE_GL_THREAD=ON`). Interactive Vulkan uses it by default so the
render thread owns the Vulkan queue while GLFW events and ImGui frame
construction remain responsive on the main thread. Use `--no-threaded` to opt
out, or `--threaded` to opt in for OpenGL. Disable the CMake option to omit this
path.

For dense BasisCurves, Vulkan raster retains every loaded prim and strand in a
persistent device-local carrier buffer. The vertex shader expands sampled
segments into camera-facing ribbons, avoiding per-frame CPU tessellation and
uploads. Quality defaults to 8 evenly distributed segments per strand and drops
temporarily to 2 while the camera moves. Change it live under **View > Curve
quality** (8/16/32/64), or choose full tessellation for comparison. Set
`LUSDVIEW_VK_CPU_CARRIERS=1` to exercise the compatibility fallback.

## Known limitations

- Vulkan/CUDA/HIP ray tracing samples base color, metallic, roughness, normal,
  emissive, and opacity images through the shared semantic table. The RT
  software texture path uses trilinear footprint/projected-triangle mip LOD,
  without raster anisotropy.
- Vulkan raster uses four bound descriptor sets and runs on low-limit software
  devices as well as desktop GPUs.
- Native point and curve carriers honor authored direct-light and shadow-light
  masks on both raster backends. Their carrier geometry participates in the
  corresponding planar or point shadow-map pass, subject to the same path and
  visibility filters as mesh geometry.
- Hardware and compute-BVH Vulkan rays traverse a bounded stack of
  alpha-blended layers (eight layers, with a conservative opaque tail).
  The nearest primary layer receives full shading; deeper primary layers use
  a bounded material/light response and inherit the nearest layer's refracted
  walk, so very deep stacks and multi-bounce lighting remain an approximation.
- CUDA/HIP primary rays use the same bounded front-to-back layer policy: the
  nearest layer is fully shaded and up to seven deeper accepted layers receive
  bounded material/light underlay shading along the carried refracted ray.
  Mask layers are rejected during traversal and do not occlude later layers.
- MaterialX image/procedural graphs are evaluated at asset-relative UV samples
  and baked into bounded 16x16 semantic maps when no extracted slot exists.
  Vulkan hardware ray-query and compute-BVH paths additionally evaluate the
  packed graph IR per hit for constants, resident image/tiled-image nodes,
  arithmetic, mix, clamp, dot, and normalize operations. CUDA/HIP consume the
  same packed IR, including resident UDIM/Ptex image sampling; the CPU fallback
  evaluates the same graph lanes needed by its preview path. Vulkan and OpenGL
  raster evaluate the resident graph lanes as well, including tangent-space
  normal maps and OpenPBR subsurface weight/color, with bounded preview lobes.
  Vulkan raster also carries OpenPBR transmission weight/color into a bounded
  colored back-light approximation; full secondary-ray refraction remains an RT
  feature. Transparent intermediate layers now carry bounded refraction in the
  CPU, hardware ray-query, and compute-BVH walks; very deep stacks still use a
  conservative layer budget. True MaterialX tile-placement semantics and full
  procedural-node parity remain open. Simple untextured Preview materials use
  a compact graph-free Vulkan RT shader; textured/OpenPBR materials retain the
  full-fidelity shader variant.
