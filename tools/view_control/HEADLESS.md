# Headless rendering for coding agents

`control.py headless` compares **lusdrender and usdrecord** on one resolved
camera, image size, time code and diagnostic USD wrapper. It runs each renderer
in a separate sequential batch process and writes original images, pixel metrics,
a difference image, heatmap, HTML report, logs, argv and a JSON manifest. Source
USD layers remain read-only. This also provides a line-delimited JSON-RPC MCP
server for coding agents.

The reference uses the fork's `usdrecord --egl`: a surfaceless OpenGL context
with no Qt, desktop windows, X11, Wayland or Xvfb. Linux EGL/OpenGL 4.5 compatibility
support is required. The graphics vendor, renderer and version appear in the
manifest; check these before treating a run as hardware-backed. `lusdrender`
can use CPU, Vulkan ray query (`vkr`), HIP or CUDA. Its native backend summary
and complete log identify any runtime fallback.

Build/install the sibling OpenUSD fork containing the added `usdrecord` options,
`UsdAppUtils.headlessContext`, and FrameRecorder background/bone controls. Build
lusdrender with the preferred native Ninja tree:

```sh
cmake --build build_ninja --target lusdrender -j8
cmake --build ../OpenUSD/build_ninja --target install -j16
```

A one-shot render needs no display environment:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY python3 tools/view_control/control.py headless \
  tests/usda/view-control-skeleton.usda --out /tmp/agent-render \
  --backend vkr --size 640x480 --time 6
```

The default executables are
`build_ninja/lusdview_lightrt_c_build/lusdrender`,
`../OpenUSD/dist-v26.11/bin/usdrecord`, and
`../OpenUSD/.venv/bin/python3`. Override them with `--lusdrender`, `--usdrecord`
and `--usd-python`. The controller discovers the SDK's installed Python package
and libraries; `--usd-env NAME=VALUE` overrides its runtime environment. Pillow
is required by the controller for comparison and MCP image previews.
Each subprocess has a configurable `--timeout` (default 600 seconds), and timed
out/interrupted owned processes are terminated with their process groups.

Use an empty output directory, or pass `--session-subdir` to create a private
fresh session below a reusable output root. No existing job is overwritten.
`--camera /World/Camera` resolves an authored camera at the requested time and
conforms its vertical filmback to the output size. Without a camera, the
preparation step computes a deterministic frame from the selected purposes.
Both engines then read the same explicitly authored camera; they do not fit
independently. Automatic framing is recomputed per job. To hold the view across
edits or frames, configure a resolved camera from a previous render result.

## Import a viewport from the interactive pair

```sh
python3 tools/view_control/control.py headless scene.usda \
  --out /tmp/agent-same-viewport --backend vkr \
  --state /tmp/linked-view-session/capture-123/capture.json
```

`--state` accepts a native `view_state`, paired capture manifest, headless
`render.json`, session `state.json`, or raw `render-state.json`. It preserves
roll, perspective/orthographic projection, lens shift, clip planes, physical size,
purposes, background, time and selection. `--size` explicitly conforms the
imported camera to another aspect ratio. The scene positional argument selects
the source asset; it need not be the same asset used by the original viewport.

The batch profile supports shaded mode, black/white background, default/render/
proxy/guide purposes, bones and selection bounds. Nonzero exposure, HUDs and
unsupported shading modes fail validation. Bone and selection annotations are
shared camera-facing emissive meshes in the generated diagnostic layer, with
readable screen-space strokes and source-depth clipping. Native generated bones
are disabled in usdrecord so guide-purpose geometry remains independent. These
are diagnostic annotations, rather than native renderer selection effects.

## MCP configuration and tools

An MCP host can launch a reusable agent configuration such as:

```json
{
  "mcpServers": {
    "usd-render-control": {
      "command": "python3",
      "args": [
        "tools/view_control/control.py", "headless",
        "tests/usda/view-control-skeleton.usda",
        "--out", "/tmp/usd-agent-jobs", "--session-subdir",
        "--backend", "vkr", "--mcp-stdio"
      ]
    }
  }
}
```

Set the host's working directory to the repository root, or use absolute paths.
The channel uses one JSON-RPC message per line, matching the existing controller's
stdio transport. Renderer output is redirected to job logs, keeping stdout for
protocol messages. EOF, `stop`, SIGINT or SIGTERM cleans up owned work.

Tools returned by `tools/list`:

- `status`: current revision, state, output directory and explicit capabilities.
- `configure {state:{...}}`: update shared camera, camera path, size, time,
  display or selection. Top-level fields replace values; display fields merge.
  A camera is a complete resolved camera object. All updates are validated
  before acceptance, including USD camera/selection paths.
- `render {}`: render both tools and return images, metrics, report, native
  diagnostics, reproducible state SHA-256 and process wall times.
- `inspect_prim {path,offset?,limit?}`: properties, relationships, connections,
  variants and provenance from the composed OpenUSD stage. Values and pages are
  bounded (default 64, maximum 256 properties). This is explicitly OpenUSD
  inspection; it does not claim independent LightUSD property evaluation.
- `overlay {action,edits?}`: sparse typed edits and undo/redo/clear. Edit syntax
  matches the interactive controller's [shared debug overlay](README.md#control-window-and-debug-editing).
  At most 128 accumulated operations and 1 MiB of configuration are accepted.
  Failed edits leave state/history unchanged. Each job retains its diagnostic
  layer and sparse source-referencing overlay for inspection/export.
- `image {name?}`: a bounded MCP image preview of `usdrecord`, `lusdrender`,
  `diff` (default), or `heatmap`. Original-size files remain on disk. Metadata
  includes captured/current revisions so an old result is identifiable after
  configuration changes.
- `stop {}`: end the agent session.

For example, send `tools/call` requests with these names/arguments:

```json
{"name":"configure","arguments":{"state":{"size":[640,480],"time":6,"display":{"skeleton":true},"selection":["/Rig/Skeleton/Skin"]}}}
{"name":"render","arguments":{}}
{"name":"image","arguments":{"name":"diff"}}
```

## Artifacts and timing policy

Every job records its resolved camera as default-valued USD attributes, preventing
lazy/default-time readers from selecting a different lens. Loaded layer file
sizes/mtimes are checked before, between and after the two native processes;
USDZ dependencies track the outer archive. A changed loaded dependency rejects
the paired result. MCP render replies include at most 128 dependency entries;
the full inventory is kept in the manifest. Textures and externally modified assets are not copied into a
complete immutable asset snapshot. `render-state.json`, the generated `scene.usda`
and `render.json` describe the exact requested job, with native command lines and
logs beside them. `failure.json` records a failed render; failed native output is
not accepted as a new successful pair.

Images are compared as emitted: no alignment, resizing or exposure fitting.
Shading, lighting, tone mapping, transparency and material support can differ
between Storm and lusdrender. Metrics describe those differences and do not
assert pixel equivalence. `process_wall_seconds` measures the same external
scope for each sequential process: startup, USD loading, geometry/BVH building,
rendering and writing. This is fresh-process batch latency (filesystem/driver caches may be warm), not GPU-only or resident
interactive throughput. Preparation and image comparison are outside that timer.

## Regression checks

```sh
python3 -m unittest discover -s tools/view_control -p 'test_headless*.py'
# Run the USD-only tests with the SDK's Python/environment:
python3 -m unittest discover -s tools/view_control -p 'test_headless_usd.py'
python3 tools/view_control/test_headless_live.py --out /tmp/headless-cpu --backend cpu
python3 tools/view_control/test_headless_live.py --out /tmp/headless-vkr --backend vkr --require-amd
python3 tools/view_control/test_headless_live.py --out /tmp/headless-hip --backend hip --require-amd
```

The live test explicitly removes display variables. It checks rolled/shifted
orthographic silhouettes, clipping, failed-update atomicity, typed edits/history,
inspection, animated bone/selection annotations in rendered pixels, MCP image
previews, and unchanged source bytes. Use the documented source-built Vulkan
validation layers for the Vulkan run. CUDA requires suitable hardware; a missing
backend is reported as a failed job rather than silently treated as a CPU run.

## JSON validation jobs: assets, animation times and shading AOVs

```sh
env -u DISPLAY -u WAYLAND_DISPLAY LUSDR_GPU_TRIANGLE_CHUNK=4000000 \
  python3 tools/view_control/control.py validate \
  --config tools/view_control/jobs/usd-assets.json \
  --assets-root ../usd-assets --out /tmp/usd-assets-validation
```

`validate` accepts an empty output directory, writes `validation.json`, an HTML
index, a replay configuration and all per-frame native commands/logs/images.
Exit status is zero only when every requested comparison passes; unsupported
AOVs are incomplete, never successful comparisons. Individual failed AOVs/frames
are retained and the runner continues. Native capture failures are `ERROR`,
measured differences are `FAIL`. No automatic pose bake hides skinning defects.
The sibling fork must include `FrameRecorder.SetCaptureAov` and
`usdrecord --captureAov` in addition to the EGL features above.

The version 1 JSON example in [jobs/usd-assets.json](jobs/usd-assets.json) includes:

- `assets_root`: root of the existing checkout, overridable on the command line.
  Scene paths must resolve within this root. No network download is performed.
- `size`, `backend`, `aovs`, `thresholds`: common render and acceptance settings.
- `time_sampling`: `{"mode":"uniform","count":3}` includes the endpoints;
  a single uniform sample selects the midpoint. `{"mode":"random","count":3,
  "seed":20261102}` samples reproducible, sorted floating-point time codes.
  `{"mode":"explicit","values":[1,12.5,24]}` replays chosen frames. Optional
  `start`/`end` override the stage range. Degenerate ranges produce one frame.
  Missing/degenerate authored ranges fall back to the range of authored attribute
  samples, including animation properties; these are USD time codes, not seconds.
- `camera_policy`: `fixed_first` (default) freezes the first resolved camera over
  the sequence. `per_frame` fits each frame independently while keeping each
  renderer pair identical. A fixed view can clip later animation; inspect images.
- `beauty_lighting`: optional opt-in shared beauty contract, for example
  `{"key":1.0,"fill":0.25}` (optional `dome`, `key_rotate`, `fill_rotate`,
  `force`). A wrapper layer sublayers the source unchanged, copies its stage
  metadata and authors two `DistantLight`s (plus an optional `DomeLight`) so
  both tools evaluate the same rig. Scenes that author their own `UsdLux`
  lights keep their authored rig and the wrapper is recorded as not applied
  unless `force` is true. It is a diagnostic contract for unlit assets, not a
  claim about author intent. See
  [jobs/usd-assets-beauty.json](jobs/usd-assets-beauty.json).
- `scenes`: objects with `path`, optional per-scene `time_sampling`, `aovs`,
  `camera_path`, `beauty_lighting`, or an inline headless `state` for a
  hand-picked view.

Every frame records the exact time and resolved camera. The animation probe
records skeleton-root and animated-attribute counts. Both tools receive the
identical time code, including fractional values. Sources remain read-only.

Available AOV comparisons:

| AOV | Capture and comparison semantics |
|---|---|
| `color` | Emitted beauty RGB, normalized MAE and SSIM; requested native backend versus Storm EGL. Lighting/BRDF differences can legitimately fail. Use `beauty_lighting` when comparing unlit assets, so both renderers share one authored rig instead of two different default rigs. |
| `depth` | Raw Float32 PFM: SDK OpenGL window depth converted using the frozen lens and clipping planes to native primary-ray distance. Coverage IoU plus 95th percentile relative depth error on common coverage eroded one pixel. |
| `worldNormal` | SDK signed Float32 `Neye` eye normals, rotated into world space; compared with native geometric triangle world normals. Coverage IoU and 95th percentile angular error. This is geometric/interpolated normal validation, not a normal-map shading AOV. |
| `baseColor` | Shared UsdPreviewSurface emissive diagnostic connected to the original diffuse color/texture network; sRGB images. |
| `roughness`, `metallic` | Shared emissive grayscale diagnostics for constant/evaluated scalar UsdPreviewSurface inputs. Connected scalar inputs are explicitly unsupported. |

Native raw depth/normal capture currently requires the **CPU next renderer**,
regardless of the beauty backend; this is recorded explicitly and is not AMD GPU
performance measurement. SDK raw AOVs use Storm EGL on the selected GPU. Raw PFM
files preserve float values without color correction. Preview PNGs and error
heatmaps are separate diagnostic artifacts. Empty common coverage cannot pass. An empty reference AOV is marked incomplete, with reference/candidate coverage counts retained, rather than treated as proof of missing candidate geometry.
The raw normal comparison intentionally exposes smooth/flat normal differences;
material normal-map effects remain visible in beauty RGB comparisons.

Material diagnostics preserve binding, opacity, source geometry and animation.
They replace diffuse/specular response with emissive visualization and disable
material normal input in the temporary layer. They are shared diagnostic AOVs,
not native renderer albedo/roughness buffers or BRDF correctness proofs. Unsupported
surface shader types and connected scalar inputs prevent a complete material
AOV from being scored. RGB error over the full frame can also include geometry
mismatch; examine depth/normal results before attributing it to materials.
Thresholds are configurable, recorded and never fitted after rendering.

```sh
python3 -m unittest discover -s tools/view_control -p 'test_validation.py'
```

## ALab, Island and Menagerie jobs

The additional configurations in `jobs/alab.json`, `jobs/island.json`,
`jobs/island-subsets.json` and `jobs/menagerie.json` use the same runner and gates.
Override `--assets-root` for another dataset mount. ALab uses the complete merged
package and samples its shot and Remi character at 1004, 1030.5 and 1057. The
original package entry that contains no composed meshes is not used as an empty
rendering success. Island's full-scene job uses two authored cameras; the subset
job uses explicit oblique camera states. Menagerie cameras frame robot links
rather than the ground-plane bounds.

Per-scene `isolate: ["/root/remi"]` writes a temporary source-referencing layer
that deactivates other branches under the selected root. Other scene roots remain
available. This is a diagnostic subset, not a validation of the full scene or its
original lighting: unrelated lights/cameras/material branches under that root
can also be deactivated. The manifest lists every deactivated branch.

`validation_stage.evaluated_snapshot()` is an explicit SDK control: it evaluates
authored attributes at one USD time into a separate stage, resolves composition
and texture asset paths, and removes time sampling. This helps separate native
loading from rendering. A successful snapshot comparison must never be counted
as a successful native composition/cache evaluation of the original source.

Both beauty processes are attempted independently even when one fails. A failed
renderer prevents accepting a paired diff; `failure.json` retains both commands,
individual process errors and successful-process timings. Loaded-layer changes
still stop the pair immediately. Material diagnostic scoring additionally checks
visible geometry bindings: unsupported or unconverted bound surfaces, absent
materials and connected scalar inputs prevent a complete material-channel pass.

### Menagerie conversion and independent physics loading

Install the current LightUSD Python package and `mujoco==3.16.0` in the calling
interpreter. The SDK worker uses its separate Python interpreter; it needs no
LightUSD or MuJoCo installation.

```sh
python3 tools/view_control/menagerie_validate.py \
  --root /path/to/mujoco_menagerie --out /tmp/menagerie-converted \
  --model unitree_go2/scene.xml \
  --model franka_emika_panda/scene.xml \
  --model anybotics_anymal_c/scene.xml \
  --profile newton
python3 tools/view_control/control.py validate \
  --config tools/view_control/jobs/menagerie.json \
  --assets-root /tmp/menagerie-converted --out /tmp/menagerie-render
```

The converter retains USDA/USDC/USDZ files, native semantic reports, MuJoCo
rest-frame/joint/count and SI mass/center-of-mass/inertia checks, and independent
OpenUSD/LightUSD snapshots after loading each format. Missing expected reference
checks are failures, not silent skips. Inertia is compared as a body-space tensor
so principal-axis quaternion signs or eigenvalue permutations do not create false
physical mismatches. `openusd` can be requested as an additional profile; its
intentional extension omissions do not qualify as full MuJoCo fidelity.

`physics_snapshot.py --engine openusd|lightusd --scene ... --out ... --times 0 .5 1`
can also audit any existing physics asset. Run each engine with its own Python
environment and compare JSON using `physics_compare.py REFERENCE CANDIDATE --out
DIFF`. Snapshots cover authored physics, MJC, Newton and PhysX properties,
multi-apply drive/limit names, physics-material bindings and collider collections.
They include types, variability, values at explicit times, schemas, connections
and composed relationship targets. Unknown extension schema tokens are read from
the composed authored list-op, rather than dropped by SDK schema registration.
Empty reference physics and truncated comparisons cannot pass. Full fallback
resolution and solver/dynamics equivalence require separate checks; see
[physics validation](../../doc/physics-validation.md).

Material diagnostic revision 3 authors overrides in a detached layer, then attaches
the source once. It uses explicit empty connection lists to block weaker source
connections when disabling diffuse/specular/normal inputs. Coverage follows the
requested purposes, so excluded guide collision meshes do not invalidate visible
material channels. Each preparation manifest records its diagnostic revision.
Older reports remain historical measurements of their recorded preparation method.

Physics snapshot revision 2 also checks authored collider shape dimensions,
point/topology digests and evaluated collider world transforms. Geometry digests
use canonical little-endian float32 positions and int64 topology, with signed zero
normalized; they detect equal-count coordinate/index corruption. They are exact
file-load fidelity checks, not an approximation-aware comparison of source
collision hulls or solver behavior.

For an independent **original MJCF** image/depth oracle, use the same installed
MuJoCo interpreter with Pillow and a resolved camera from the paired USD job:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY python3 tools/view_control/mujoco_record.py \
  /path/to/mujoco_menagerie/unitree_go2/scene.xml \
  --state /tmp/paired-job/render.json --out /tmp/mjcf-source --require-amd
```

This capture currently requires a Z-up meter camera, zero lens shift and rest time
zero, matching the tested default conversion frame. It sets MuJoCo's actual GL
camera extrinsics/frustum directly and records the EGL vendor/renderer. Source
geometry groups 0..2 are visible; it runs no trajectory. Depth PFM contains
primary-ray distance with zero background. Beauty includes MuJoCo lighting,
reflections and skybox differences. In particular, an infinite source ground
plane and finite USD preview can fail full-frame coverage even while robot depths
agree; retain that discrepancy instead of hiding it in a background-dominated
RGB score or declaring conversion equivalence.

For material-channel jobs, captured `state.edits` must first be exported to a
source-referencing overlay and selected as the scene, with `edits` cleared. The
runner rejects material AOV requests with accumulated state edits so diagnostic
values cannot silently come from an unedited material network. Camera/display
state imports and edits used with beauty/depth/normal-only jobs remain supported.

## Bounded Vulkan geometry diagnostics

`vkr-instanced` retains shared prototypes and placements; `vkr-streamed` also
bounds resident BLAS batches and builds/traces one TLAS slice at a time. Both
preserve nearest-hit IDs across batches. `--curve-segments 1..64` (job JSON
`native_curve_segments`) explicitly selects spline samples per span; default 8.
Native curve tubes use six sides, and ribbons use authored evaluated normals.
This is tessellated diagnostic geometry, not analytic curve or material parity.

Raw depth and normal comparisons use an opaque emissive diagnostic material
wrapper for both engines, preserving source points, topology, skeleton and time.
Opacity maps, shader displacement and material normal maps are outside that gate.
Native raw normals are signed and unoriented. The SDK normal wrapper removes
editable Mesh double-sided flips and uses no culling; uneditable instance-proxy
or curve cases report unsupported rather than silently comparing a subset.
The legacy `normals` attribute loses to `primvars:normals` when both are authored.
Normal metrics retain missing/invalid interior normals as failures. A successful
beauty render does not imply that geometry, normal or material-channel gates pass.

For `worldNormal`, validation explicitly sets
`USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS=1` on the SDK subprocess and records
it. OpenUSD 26.11 defaults this setting to false, so an unconfigured Storm
capture is not a reliable reference for authored normals on animated skin.
Beauty remains the requested viewer shading configuration; its result is a
separate gate. Older runs without the recorded override remain distinct evidence.

Raw depth and normal AOVs explicitly set `HDX_MSAA_SAMPLE_COUNT=1` for Storm to
match native pixel-center rays. Storm's default four-sample resolved coverage
cannot be treated as the same work. No edge dilation or image registration is
used. Missing authored normals use incident-polygon smooth normals when native
smooth shading is requested; raw geometric normals remain face normals.

SDK normal diagnostics may disable necessary native-USD instanceability in their
detached override layer to edit doubleSided. They preserve source-referenced
points, topology, clips and skeletons and never bake arrays. These edits are
listed in `deinstanced_for_display_override`; such a capture cannot establish
performance parity with the original shared-instance representation. Curves whose
normal conventions are uncalibrated still report unsupported.

Sessions pin hashes of SDK libraries, pxr Python modules and plugin/schema/shader
resources in addition to executable hashes. Normal filesystem edits invalidate
the fingerprint even if size and mtime are preserved. System libraries and GPU
driver dependencies are outside this SDK-install fingerprint and must be recorded
in the hardware/toolchain profile.

Material-channel wrappers use native `-diagnosticEmission`: evaluate only the
wrapper's emissive channel, apply texture color-space conversion and RGB
scale/bias, and encode standard sRGB without a photographic tone map. Storm's
MSAA is disabled for these captures too. This is calibrated on CPU and flat
Vulkan using analytic pixel values; other native backends currently report
unsupported material-channel output. The manifest records this mode. Original
beauty is a separate capture with its ordinary shading model.

The emissive diagnostic follows PreviewSurface `opacityMode`: the default
`transparent` keeps emission at full weight and attenuates the background;
`presence` attenuates emission too. A positive `opacityThreshold` masks both
modes. Independent two-layer CPU/Vulkan tests cover both modes, custom UV sets,
shared-material caching, and texture alpha scale/bias. This diagnostic support
does not imply full beauty-shader opacity-mode support on every backend.
See the [PreviewSurface specification](https://openusd.org/release/spec_usdpreviewsurface.html).

The native command now requests smooth shading to match FrameRecorder's smooth
display mode. This is an explicit display override: USD's default polygonal
normal policy remains faceted when neither normal attribute is authored. Raw
geometric normals are always face normals. Subdivision meshes define their own
normals and do not use authored polygonal normals.

Prepared sessions hash their used USD layers/packages and recheck them across
captures. External texture payloads are outside that layer fingerprint; archive
or repository identities and channel-specific texture provenance must be pinned
separately for an immutable material baseline.

On Linux every owned probe/render/AOV subprocess now runs under the shared
process supervisor. Defaults are30GiB sampled process-tree RSS and4GiB available
reserve, in addition to the wall timeout. Per-command `.resources.json` files
record the actual status, budget, peak RSS and CPU/wall times; `.process/` keeps
separate stdout/stderr and exact `/usr/bin/time` RSS. Cancellation still stops
the owned process group. Other platforms retain timeout supervision; requesting
explicit resource supervision there fails instead of claiming unsupported checks.

Validation JSON accepts `process_limits` globally or per scene: `max_rss_mib`,
`reserve_mib`, and optionally both `gpu_vram_path` and `gpu_budget_mib`. The GPU
counter must be an explicit absolute readable device-memory counter, for example
the appropriate AMD sysfs device's `mem_info_vram_used`. Its budget covers the
whole device, including other clients. GPU jobs remain serial. A scene object
replaces the global object; no unrecorded hardware index is guessed.

`native_load_limits` separately supplies `max_memory_gib` and
`max_array_elements` to the renderer. Original Island jobs explicitly use30GiB
and67,108,864 elements. The CLI now fails on unreadable/rejected references;
rendering a partial stage no longer silently establishes full-scene coverage.

Direct `UsdUVTexture.outputs:r/g/b/a` scalar material inputs can use the
revision6 shared grayscale adapter when SDK Python has Pillow installed. It
replicates the selected encoded channel into RGB, preserves UV connections,
wrap modes, selected scale/bias/fallback, and RGB transfer functions; alpha
channels remain linear. Source opacity stays on its original graph. Both
renderers sample the same derived image. The manifest records input/output
SHA-256, channel, dimensions, Pillow version, and decoder module/binary hashes.
This is an image-channel transformation, with no SDK point or material-value
bake. Encoded inputs are limited to64MiB and decoded images to16M pixels;
unsupported graph nodes, UDIMs, high-bit-depth images and connected scale/bias
remain explicit coverage failures. Missing Pillow reports unsupported coverage.

### Reference quality and visible coverage

Validation job JSON accepts `reference_complexity` (`low`, `medium`, `high`,
`veryhigh`), globally or per scene; default is `low`. SDK command/manifests retain
the chosen quality. This is a controlled approximation setting, not a golden
reference: the independent unit-sphere control shows higher SDK complexity can
increase geometric error.

Beauty/material jobs automatically add a depth AOV when absent. PASS requires
foreground in both views with common depth pixels; `implicit_aovs` records the
witness. Empty or failed witnesses make otherwise matching images INCOMPLETE. This
opaque geometry witness does not prove complete opacity-aware material visibility.
The [extended USD-assets job](jobs/usd-assets-extended-materials.json) covers
texture transforms, alpha, color spaces, MaterialX and two more sampled characters.
[Measured findings](../../doc/validation/usd-assets-extended-2026-11-03.md) retain
unsupported channels, partial references and failed beauty comparisons.

Validation manifests and summary HTML update atomically, retaining the prior valid
file if storage fills during an update. Partial temporary files are removed.
