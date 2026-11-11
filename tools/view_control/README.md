# Linked lusdview and usdview control

The controller launches and owns both viewers, serves a loopback MCP endpoint,
links resolved camera poses/frusta, purpose/shading/background settings, prim
selection and paused timeline changes in either direction, and captures matched
physical viewport sizes. Both native windows remain open. lusdview uses eager payload loading with geometry
admission caps disabled and adaptive quality off for comparison. The controller
currently forces the non-threaded lusdview path because completed-frame camera/time
acknowledgments are implemented there. Camera exchange includes
roll, orthographic scale, lens shift and clipping planes. Unsupported settings
produce errors and suspend synchronization; inspect `status`, then resynchronize
with the controller `sync` tool and an authoritative `source` viewer.

Build lusdview with MCP support and build/install the sibling OpenUSD fork containing
`usdviewq/remoteControl.py`. The bridge is opt-in; normal usdview startup is unchanged.

```sh
python3 tools/view_control/control.py launch scene.usda \
  --lusdview build_ninja/lusdview --usdview ../OpenUSD/dist/bin/usdview \
  --out /tmp/linked-view-session --size 1280x720
```

The launch command stays running until Ctrl-C or `stop`; it cleans up its owned
process groups. Each session uses a private token file and authenticated loopback
endpoints. Logs, argv and endpoint manifests are stored in the session directory.
Use a fresh directory for each session. `--usdview-env NAME=VALUE` configures the
usdview runtime without changing lusdview's environment. On X11, the default Qt
and PyOpenGL platforms are xcb and glx. `--headless-lusdview` is optional.

```sh
python3 tools/view_control/control.py status --session /tmp/linked-view-session
python3 tools/view_control/control.py capture --session /tmp/linked-view-session
python3 tools/view_control/control.py benchmark --session /tmp/linked-view-session --samples 30 --warmup 10
python3 tools/view_control/control.py stop --session /tmp/linked-view-session
```

Capture writes both original PNGs, `capture.json`, image metrics, a difference
image, a heatmap and `index.html`. Open that file for side-by-side, overlay and wipe
comparison. Captures require synchronized states, paused timelines and newer
completed frames with matching resolved cameras and times. Each renderer retains
the submitted frame pixels and its complete supported camera, display, selection,
focus, scene revision and viewport-size snapshot under an opaque barrier token.
Saving a screenshot reads those retained pixels, even after later native changes;
the controller rejects state changes during paired capture and releases both caches.
This protocol currently requires mono raster rendering and the updated fork;
the legacy development adapter cannot provide full paired-capture barriers. Pixels are compared
as emitted, without geometric alignment or exposure fitting. Renderers can differ
in shading even when view geometry matches. This is visual diagnostics, not a
claim of renderer pixel equivalence.

Add `--mcp-stdio` to launch for an MCP host. The controller exposes `status`, `sync`,
`viewer_tools`, `viewer_call`, `playback`, `overlay`, `capture`, `benchmark` and `stop`. `viewer_call` takes `viewer` (`lusdview` or
`usdview`), `tool` and `arguments`, forwarding to that viewer's own implementation.
Paired `viewer_call` permits read/navigation/display tools; scene edits use the
shared `overlay` tool. `viewer_tools` lists the permitted native schemas.
The usdview bridge provides `view_state`, `viewport`, `render_settings`, `select`,
`inspect_prim`, `timeline`, `screenshot`, `get_scene_info` and `get_render_stats`.
`inspect_prim` accepts `path`, `offset` and `limit` (default 64, maximum 256), and
returns bounded attribute previews, relationships, connections and variant selections.
usdview also reports authored property-stack provenance and diagnostic metadata;
lusdview explicitly reports that provenance is unavailable through its public C API.
lusdview's `inspect_prim` reads its native next stage, returns relationships and
connections, and reports array default type/count without materializing buffers.
Its value policy is resolved defaults; usdview inspects the current time. Existing
`attr_get`, `prim_get` and other LightUSD MCP tools remain available.

## Shared-clock playback

Use the control window's linked play/pause buttons or:

```sh
python3 tools/view_control/control.py playback --session /tmp/linked-view-session --action play --fps 30
python3 tools/view_control/control.py playback --session /tmp/linked-view-session --action pause
```

The controller uses usdview's current time as the initial shared time and requires
matching stage ranges/time-code rates. Native playback stays paused: each next
time code is sent only after both renderers acknowledge a newer completed frame
at the preceding code. This prevents independent playback clocks from drifting.
The requested FPS sets the code step and maximum scheduling rate; slow renders
and RPC overhead reduce actual playback speed. Add `--no-loop` to stop after the
end frame has rendered. Camera/display/selection changes remain linked during
playback. Native play buttons still select independent application clocks and
suspend synchronization; use linked controls instead. Pause linked playback before
capture, editing or benchmarking.

## Native lusdview comparison panel

In lusdview choose **Tools > Compare viewers**. The panel provides paired capture,
authoritative resynchronization, linked playback, picked-prim inspection, shared
JSON edits, undo/redo/clear and overlay export. Preview modes are side by side,
alpha overlay, wipe, difference and heatmap. Images decode on a worker and use
independent GL/Vulkan preview textures. Preview dimensions are limited to 4096 per
axis; larger original captures remain available in the saved report. Native UI
commands use an atomic private session queue and leave rendering responsive.
The native `compare_panel` MCP tool opens the panel, chooses mode 0..4 and can
save its composited window to `lusdview-compare-window.png` in the session directory.

Bone display synchronizes independently of the guide purpose using
`render_settings {"skeleton": true}` or either native menu. lusdview draws joint
lines from the animated skinning pose. usdview uses an opt-in scene-index filter
that exposes only generated bone meshes, preserves scene visibility and leaves
skinned meshes, descendant prims and ordinary guides intact. Standard usdview
startup retains its native behavior. Legacy/non-scene-index SDKs report bone
control as unsupported.

## Control window and debug editing

Open `control_url` from the launch output in a browser and enter the session token
from `token_file`. The local control window provides paired picked-prim inspection,
JSON edit batches, undo/redo, exports, benchmarking and side-by-side/overlay/wipe
displays for the most recent capture. The token stays in tab memory. Captured
images require bearer authentication; the page includes no external resources.
Browser visual verification requires an available browser; HTTP/authentication
and JavaScript syntax can be checked independently.

Unsaved native USD layer opinions must be saved or exported before enabling the
shared wrapper. The first edit creates a private debug directory with `overlay.usda` and
`scene.usda`. Both viewers switch to this wrapper, preserving their view state.
The wrapper composes the sparse overlay over the original root layer and copies
stage metadata such as up axis, units and time range. Relative source assets
continue resolving from their original layer. New asset-valued edits are anchored
to the original root. Existing USD files are never edit targets.

```sh
python3 tools/view_control/control.py overlay --session /tmp/linked-view-session --action edit --edits edits.json
python3 tools/view_control/control.py overlay --session /tmp/linked-view-session --action undo
python3 tools/view_control/control.py overlay --session /tmp/linked-view-session --action redo
python3 tools/view_control/control.py overlay --session /tmp/linked-view-session --action export
```

`edits.json` is an array, for example:

```json
[
  {"op":"set_attribute","path":"/World/mesh","property":"visibility","value":"invisible"},
  {"op":"set_attribute","path":"/World/mesh","property":"debug:scale","type":"float3","value":[1,2,1]},
  {"op":"relationship","path":"/World/mesh","property":"debug:related","targets":["/World/other"]}
]
```

Operations: `set_attribute` (optional numeric `time`; explicit `type` for new
attributes), `block_attribute`, `clear_property` (remove overlay opinion),
`relationship` / `connections` (`targets` array), `variant` (`set`, `value`),
`metadata` / `property_metadata` (`key`, `value`). Metadata is restricted to
supported diagnostic fields; prim creation/removal is excluded. Scalar numeric,
string/token/asset, vector/matrix/quaternion and array values are converted to
native USD types. Unsupported types fail explicitly. Edits require paused playback.

A batch is validated on a temporary composed stage, then published atomically.
The controller reloads lusdview and restores the view state before accepting the
history entry. On failure it restores the previous overlay and reloads the peer;
recovery failures suspend synchronization and remain visible in status. Up to 32
states / 64 MiB are retained for undo. Overlay size is capped at 8 MiB, a batch at
128 operations, and a supplied array at 65,536 elements. `clear` removes all debug
opinions and can be undone. Export writes a sparse layer plus a wrapper referencing
the original source; it does not bundle source assets. A controller crash during
publication can leave its last wrapper on disk; session recovery after process
failure is not implemented.

Inspect endpoints or compare existing captures:

```sh
python3 tools/view_control/control.py tools --endpoint http://127.0.0.1:8080/mcp --token-file /path/to/token
python3 tools/view_control/control.py call --endpoint http://127.0.0.1:8080/mcp --token-file /path/to/token get_scene_info
python3 tools/view_control/control.py compare --reference usdview.png --candidate lusdview.png --out /tmp/comparison
```

`call --arguments request.json` reads a JSON object. The client bounds response
size, rejects redirects and remote endpoints, bypasses HTTP proxies, and reports
JSON-RPC/tool errors as failures.

On POSIX, `benchmark` suspends the owned peer process group with SIGSTOP during
warmup and sampling, then resumes it with SIGCONT in a finally block. Both windows
and GPU allocations remain present. It records unique completed frames, raw native
CPU timing samples and min/median/max in `benchmark-*.json`. Each viewer's timer
scope is recorded: lusdview includes its UI frame through present, while usdview
measures paintGL. These scopes differ; results are shared-session diagnostics,
not isolated GPU throughput comparisons. Native input must remain still during a
batch. Peer suspension also temporarily pauses that window's input processing.

## Validation and remaining work

```sh
python3 -m unittest discover -s tools/view_control -p 'test_control.py'
python3 tools/view_control/test_live.py --session /tmp/linked-view-session --prim /hello
# For a session launched with tests/usda/view-control-skeleton.usda:
python3 tools/view_control/test_frames_live.py --session /tmp/skeleton-view-session
python3 tools/view_control/test_panel_live.py --session /tmp/skeleton-view-session --prim /Rig/Skeleton/Skin
# Use the USD-enabled Python/environment used by usdview:
python3 -m unittest discover -s tools/view_control -p 'test_overlay.py'
python3 tools/view_control/test_overlay_live.py --out /tmp/overlay-regression --usdview ../OpenUSD/dist/bin/usdview
```

The live test changes cameras, display and selection, checks invalid-pose rejection,
authentication, echo suppression and fresh equal-sized captures. Run it against a
small fixture with the specified prim. The bridge has also been exercised using
installed USD 26.05 with the explicit development adapter:
`--usdview-python ../OpenUSD/.venv/bin/python3 --bridge-source ../OpenUSD/pxr/usdImaging/usdviewq/remoteControl.py`.
This adapter is for compatibility testing, not a substitute for validating a new
fork build. The separate `../OpenUSD/build_ninja` / `dist-v26.11` build has now been
built with `pxr_lte` / `pxr_lteInternal_v0_26_11` namespaces and its installed bridge
has passed the live camera/display/selection/timeline/playback suite on AMD.
Normal startup without remote control also passed, as did the full overlay
recovery/export/inspector suite, independent bone/retained-pixel checks, the native
Compare panel through Vulkan/OpenGL/Vulkan switching, and a 30-sample-per-viewer
timing batch. Windowed Vulkan composite dumps and headless resize rejection were
also verified. Vulkan API validation reported no errors in the final runs;
existing allocation and layout best-practice warnings remain. For that installation use:

```sh
python3 tools/view_control/control.py launch tests/usda/view-control-animation.usda \
  --out /tmp/linked-v2611 --usdview ../OpenUSD/dist-v26.11/bin/usdview \
  --usdview-env PYTHONPATH="$(realpath ../OpenUSD/dist-v26.11/lib/python3.11/site-packages)" \
  --usdview-env LD_LIBRARY_PATH="$(realpath ../OpenUSD/dist-v26.11/lib):$(realpath ../OpenUSD/deps/inst/lib)"
```

The panel live suite captures every preview mode on Vulkan, OpenGL and Vulkan
again, then exercises queued inspection and capture. Run it under Vulkan
validation and review the saved `native-*-mode-*.png` window screenshots.

The overlay live suite checks both native values and rendered bounds, source
file hashes, undo/redo, failed batches and an injected peer-reload failure.

Remaining work: matched GPU timing scopes, richer typed property/provenance
diagnostics, and browser visual verification when a browser is available. Current usdview exposure is fixed at zero; unsupported
shading modes suspend synchronization. Existing source assets are never edited.

## Headless coding-agent control

Use `control.py headless` for paired **lusdrender / usdrecord** jobs and MCP
control without either viewer window. The fork's surfaceless `usdrecord --egl`
path needs no desktop, Qt, X11 or Xvfb. It supports imported viewer camera states,
shared purposes/time/diagnostic overlays, bounded inspection, image previews,
comparison artifacts and sequential batch timings. See [HEADLESS.md](HEADLESS.md)
for setup, CLI/MCP examples, supported state and regression commands.

Bounded physics trajectories can be checked independently of renderer agreement:

```sh
PYTHONPATH=/path/to/native/python python3 tools/view_control/dynamics.py \
  --config tools/view_control/jobs/physics-dynamics.json --out /tmp/dynamics \
  --engine newton-mjcf --engine newton-usd --python /path/to/sdk/python \
  --env PYTHONPATH=/path/to/newton:/path/to/sdk/python-packages
```

`--engine mujoco-mjcf --engine mujoco-usd` additionally requires
`--mujoco-worker` built from `tests/next/mujoco-bounded-dynamics.cc` and the
MuJoCo USD decoder `--mujoco-plugin`. `setup_mujoco_usd.sh` builds both
reproducibly (USD-enabled libmujoco, decoder plugin, worker) and applies the two
OpenUSD-compatibility fixes (a `namespace pxr = PXR_NS` forced include for
OpenUSD dists whose `PXR_NS` is not `pxr`, and the missing
`mjcPhysics/generatedSchema.usda`):

```sh
MUJOCO_SRC=/path/to/mujoco OPENUSD_DIST=/path/to/OpenUSD/dist \
  tools/view_control/setup_mujoco_usd.sh          # prints the two exports below
export LIGHTUSD_MUJOCO_WORKER=... LIGHTUSD_MUJOCO_USD_PLUGIN=...
python3 tools/view_control/check_mujoco_usd_matrix.py   # or: ctest -R tool-mujoco-usd-matrix
```

`check_mujoco_usd_matrix.py` asserts the documented verdicts on both the MJCF
control and the converted USD (default-`solref` contact FAILS identically on
both; the stiff-contact control passes) and exits 77 (ctest SKIP) when the
consumer is not built. Each process is bounded by `--timeout`.
The output retains source/conversion hashes, exact commands, trajectories and
per-contract failures. A matching failed trajectory remains a failure. Contact
must reach impact and settle; incomplete dynamics cannot certify whole robots.

`jobs/physics-contact-stiff.json` is an additional compliance control with the
same quality gates and denser impact sampling. Optional `contact_solref` accepts
a finite positive `[time_constant, damping_ratio]` pair for contact cases, with
time constant at least twice the timestep. Omission keeps the MJCF default;
it does not silently harden the original default-contact case. See the
[measured source/consumer controls](../../doc/validation/physics-contact-response-2026-11-03.md).

Validation job JSON may set `"data_backend": "vkr"` to capture depth and signed
interpolated normals through native Vulkan traversal. The default remains
`"cpu"`; each AOV records its actual requested backend. Run with hardware device
passthrough and inspect the retained native log's device/backend lines. GPU
triangle AOVs omit opacity filtering, which must be considered when diagnosing
coverage differences against Storm.

Scene job JSON can set `reference_population_mask: true` with nonempty `isolate`
roots. The SDK reference then uses a separate population mask expanded across
relationship/connection dependencies, including instance proxies and generated
camera/material helpers. Limits are 4096 paths and 65536 path bytes. Native input
and source files are unchanged. This controls subset working sets; it does not
establish full-scene parity or permit speed ratios across different working sets.

Reference logs are checked even after exit zero. Known shader/link/GL failures,
failed resource loads and missing field plugins make the affected requested
capture INCOMPLETE. Truncated Storm lighting also invalidates beauty, while unlit
material and opaque data AOVs do not depend on the light count. Each AOV checks
its own reference log; a partial beauty reference does not invalidate a separate
clean opaque diagnostic. Reports retain bounded findings and `reference_complete`.

Geometry normal AOVs report unperturbed interpolated normals. They cannot certify
normal-map sampling or tangent frames; see the generated
[interpretation control](../../doc/validation/normal-aov-contract-2026-11-03.md).

### Paired library load/readback measurements

`load_benchmark.py` measures original-source LoadAll composition and evaluated
Mesh points/counts/indices at an explicit numeric time. Both engines retain native
in-memory instances and traverse logical Mesh prims including instance proxies.
The exact hash includes each logical path and decoded arrays, with multiplicity;
generated prototype names, PointInstancer placement expansion and world
transforms are outside this array-read workload. The repaired native proxy handles
make the same logical work accessible through Python.

It uses one Python executable for both bindings, alternates engine order, and
requires at least one warmup and five measured fresh processes. Limits are
30 GiB RSS, 4 GiB reserve and 1800 seconds per process. Ratios are emitted only
when every run has the same exact work hash. Operation time excludes imports;
process RSS includes imports and bindings. This is neither renderer throughput
nor a library-only memory estimate.

```sh
python3 tools/view_control/load_benchmark.py --config /tmp/load-job.json \
  --out /tmp/load-results --python ../OpenUSD/.venv/bin/python3 \
  --usdrecord ../OpenUSD/dist-v26.11/bin/usdrecord \
  --native-library build_ninja/liblightusd_c.so \
  --native-extension /tmp/staged-python/lightusd/_core.abi3.so
```

The config contains `version: 1`, `warmups: 1`, `repetitions: 5`, a `sdk_env`
object with the SDK PYTHONPATH/library settings, and `cases` with unique `id`,
local source `path`, and numeric `time`. Run it separately from builds and render
jobs. Executable, native library/extension, SDK runtime and worker hashes are
pinned; a change during measurement rejects the result.

Validation adds a depth witness when beauty/material AOVs omit it. Passing image
metrics require foreground in both views with common depth pixels, preventing
matching empty geometry from certifying materials. The opaque witness is a
necessary check, not complete opacity-aware material-visibility coverage. `implicit_aovs` records the
extra capture. `reference_complexity` accepts `low`, `medium`, `high`, `veryhigh`
globally or per scene. It controls SDK tessellation and is recorded in commands
and manifests; higher complexity does not guarantee closer analytic geometry.
See the [extended corpus and independent sphere control](../../doc/validation/usd-assets-extended-2026-11-03.md).
