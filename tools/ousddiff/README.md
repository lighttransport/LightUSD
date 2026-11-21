# OpenUSD / lusdrender comparison runner

`ousddiff` renders the same prepared USD through OpenUSD `usdrecord` and
LightUSD `lusdrender`, measures each process, compares images, and writes JSON
and an offline HTML report. Requires Linux, Python 3.11+, GNU time, and Pillow.
The runner does not require NumPy, the LightUSD Python extension, or MuJoCo.
OpenUSD preparation runs in a separate configured Python interpreter.

## Quick start

The supplied `presets.json` uses the local OpenUSD, USD WG assets, Menagerie,
Caldera, Island, and merged ALab locations. Copy it to customize paths and limits.
Renderer paths depend on the native build configuration: the current legacy
viewer build places `lusdrender` in `build_ninja/lusdview_lightrt_c_build/` and
the converter in `build_ninja/examples/urdf-to-usd/`. A next converter build
places `urdf-to-usd` directly in `build_ninja/`. Override these paths as needed.

```sh
# Inventory only: opens MJCF XML for entrypoint discovery, but does not compose USD.
python3 tools/ousddiff/runner.py list --out /tmp/ousddiff-inventory.json

# Small CPU probe; exclude complete large scene roots before sampling.
python3 tools/ousddiff/runner.py run --preset usd-assets \
  --mode probe --sample-percent 1 --seed 0 --no-full-scenes \
  --renderers embree,cpu --max-input-mib 16 --out /tmp/ousddiff-probe

# Reuse completed cases when inputs, dependencies, code, binaries and settings match.
python3 tools/ousddiff/runner.py run --preset usd-assets \
  --mode probe --sample-percent 1 --seed 0 --no-full-scenes \
  --renderers embree,cpu --max-input-mib 16 --resume --out /tmp/ousddiff-probe

# Compare against a saved run; the baseline is never modified automatically.
python3 tools/ousddiff/runner.py run --config /path/to/config.json \
  --mode probe --sample-percent 1 --no-full-scenes \
  --baseline /path/to/baseline/results.json --out /tmp/ousddiff-current

python3 tools/ousddiff/runner.py report \
  --input /tmp/ousddiff-current/results.json --out /tmp/ousddiff-current/report.html

python3 tools/ousddiff/runner.py compare \
  --reference reference.png --candidate candidate.png --out /tmp/ousddiff-images
```

Open `report.html` locally: no server, remote resources, or JavaScript fetches
are required. Keep its artifact directory beside it. Reports have filters,
sortable timing/RSS columns, pagination, lazy image comparisons, reproduction
arguments, backend diagnostics, and log links.

`--preset` accepts comma-separated dataset names or `all`. `--mode full`
processes every eligible candidate; full mode is independent of geometry LOD.
**Complete scene roots can be extremely expensive.** `--no-full-scenes` excludes
the preset's named entry roots and their full-geometry variants in either mode.
It does not prove that arbitrary user-added assemblies are inexpensive.
Use dataset include/exclude patterns and resource limits to bound them.

## Configuration

`schema_version` is 1. Paths in datasets, explicit cases, executable settings,
and OpenUSD settings resolve relative to the JSON file. `~`, environment
variables, and `{repo}` in configured executable paths are expanded. Missing
variables in paths are errors. Commands are argument arrays; no shell executes
configuration text.

```json
{
  "schema_version": 1,
  "openusd": {"dist": "/path/to/OpenUSD/dist", "python": "/path/to/OpenUSD/.venv/bin/python3"},
  "converter": "/path/to/urdf-to-usd",
  "resources": {"timeout_s": 1800, "max_rss_mib": 30720, "reserve_mib": 4096},
  "render": {
    "width": 512, "height": 512, "purposes": ["default", "render"],
    "complexity": "low", "repeat": 1, "warmups": 0
  },
  "datasets": [{
    "name": "example", "root": "/path/to/assets",
    "include": ["**/*.usd", "**/*.usda", "**/*.usdc", "**/*.usdz"],
    "exclude": ["captures/*"], "full_scene_roots": ["entry.usda"]
  }],
  "cases": [{
    "id": "explicit/example", "path": "/path/to/example.usda",
    "render": {"frame": 12},
    "variant_rules": [{"path": "/World/Asset", "set": "lod", "selection": "full"}]
  }],
  "renderers": [
    {"name": "embree", "kind": "usdrecord", "executable": "/path/to/usdrecord", "backend": "Embree", "required": true},
    {"name": "cpu", "kind": "lusdrender", "executable": "/path/to/lusdrender", "backend": "cpu", "required": true}
  ],
  "pairs": [{"renderers": ["embree", "cpu"]}]
}
```

Each renderer can specify `args` and an `env` mapping. Backends include Storm,
Embree, cpu, vk, vkr, cuda, and hip as supported by the installed binaries.
GPU presets require hardware: software devices, fallback, or unverifiable
identity fail the capability check. Storm uses the existing display or one
batch-owned Xvfb, outside timed renders. `glxinfo` verifies its hardware identity.
A failed capability is recorded once; remaining usable backends still execute.
Set a renderer's `required` to false to allow its absence. `--renderers` selects
a subset and removes comparison pairs whose members were not selected.

For other renderers, use `kind: "custom"` with `argv` containing `{scene}`,
`{image}`, `{camera}`, `{width}`, `{height}`, and/or `{frame}` placeholders.
Custom capability checks verify executable availability only. `prepare: false`
explicitly disables shared preparation for custom workflows; then matching
camera, time and dimensions are the caller's responsibility.

The Menagerie preset discovers model and scene XML with a `worldbody` or
`include`, excluding include-only actuator/asset fragments. Conversion uses
native `urdf-to-usd` with real meshes and retains its USD and generated assets.
Both renderers receive the same output. Conversion failures are distinct from
preparation/render failures. Existing USD-family inputs remain separate cases.

## Selection and shared preparation

Probe selection ranks stable dataset-relative IDs by SHA-256 of seed and ID.
It samples 1–10%, rounded up within each dataset, geometry/composition/support
candidate group and decimal file-size band. Small strata receive at least one
case: the effective percentage can exceed the requested percentage. Both are
reported in `selection.json`, together with eligibility filters and selected IDs.
File size does **not** bound the cost of loading its dependencies.

`--group geometry` and `--max-input-mib N` filter before sampling.
`--cases-from selection.json` or a JSON list of IDs reruns explicit cases instead
of sampling. `--frames 1 2.5 default` expands each selected input into separate
frame cases/processes. Otherwise `render.frame` defaults to the authored start
time; `"default"` selects USD's non-animated Default time. Per-case `render`
settings override global settings.

Preparation preserves composition using a stronger wrapper layer, copies root
axis/unit/time metadata, applies variant rules, loads payloads, and records
resolved dependency fingerprints. Full Caldera roots apply `districtLod=full`
per prim; ALab full roots select available `geo=render_high` variants. Already
selected variants remain unchanged. Island loads its authored geometry payloads.

The worker counts renderable geometry including instance proxies. Valid stages
without renderable geometry, or with empty bounds for selected purposes, are
reported explicitly rather than counted as render failures. Malformed stages,
composition errors, and unresolved dependencies are preparation errors.

A configured `render.camera` must be an absolute camera path. Otherwise a
common camera frames the reference bounds, with planar scenes viewed face-on.
Authored camera aspect is authoritative; generated cameras use the configured
width/height aspect. Height follows usdrecord's float division/truncation and is
passed explicitly to lusdrender. Purposes include `default`; use `render` or
`proxy` deliberately. Authored render-settings metadata is disabled in the
wrapper so captures use the comparison camera and output location.

## Measurements and pass/fail

Processes run serially and freshly for every scene/frame/backend/repetition.
Warmups are excluded from medians; raw attempts and measured sample counts are
retained. Wall time uses a completion notification, avoiding the RSS sampler's
50 ms polling delay. GNU time supplies CPU times and process peak RSS, converted
from KiB to bytes. Preparation/conversion have their own measurements.

`peak_rss_bytes` is GNU time's maximum RSS, **not** simultaneous summed tree RSS.
`peak_tree_rss_bytes` samples aggregate descendant RSS every 50 ms, may miss
brief spikes, and counts shared pages more than once. Neither measures VRAM.
The guard uses host/cgroup headroom, a memory reserve and sampled tree RSS. This
is a watchdog, not a kernel-enforced allocation limit. Timeouts/resource limits
terminate process groups and observed descendants; the batch continues.

Images are decoded and validated, including exact dimensions. Original images
are retained. Metrics compare emitted RGB values composited over black, with
alpha error separate; no registration, resizing, gamma or exposure fitting is
applied. SSIM uses uniform non-overlapping 8×8 luminance windows, population
moments and standard C1/C2 constants. Its method is included in JSON. Perfect
PSNR is `null` plus `psnr_infinite: true`, keeping JSON finite. Changed-pixel
fraction uses maximum channel error greater than `pixel_tolerance` (default 8/255).

Cross-engine differences are `uncalibrated` until thresholds are provided.
This matters because Embree CPU color correction is disabled while lusdrender
uses tone mapping and sRGB encoding. Successful execution alone is not a visual
fidelity pass. Calibrate thresholds against reviewed images for each pair/case:

```json
{
  "thresholds": {"normalized_rmse": {"max": 0.15}},
  "pairs": [{"renderers": ["embree", "cpu"], "thresholds": {"ssim": {"min": 0.85}}}],
  "historical_thresholds": {"normalized_rmse": {"max": 0.01}, "ssim": {"min": 0.99}},
  "performance_thresholds": {
    "wall_s": {"relative": 0.15, "absolute": 0.1},
    "peak_rss_bytes": {"relative": 0.1, "absolute": 1048576}
  }
}
```

Global thresholds are overridden by pair thresholds, then per-case thresholds.
Available metrics are normalized MAE/RMSE, SSIM, PSNR, changed-pixel fraction,
and alpha MAE; each supports `min` and `max`. Historical image defaults are
NRMSE ≤0.01 and SSIM ≥0.99. Performance changes are informational unless a policy
is configured; both relative and absolute increases must exceed it to fail.
Use multiple repetitions for meaningful timing gates. Embree's threaded sampling
can still vary despite a fixed seed; calibrate historical tolerances accordingly.

Historical runs require matching IDs, rendering settings, input/dependency
fingerprints, CPU/platform, relevant environment, and backend identity.
Renderer binary hashes are recorded but may change between historical runs.
Missing/incompatible baseline cases fail explicitly; removed IDs are listed.
Keep baseline inputs/artifacts available. Resume additionally checks executable
and runner hashes, dependencies, output images, settings and thresholds. Failed
cases are rerun. Changing rendering settings requires a new compatible baseline.

Expected failures are explicit, for example:

```json
{"expected_failures": {"example/bad.usda": {"status": "preparation_error", "reason": "intentionally malformed fixture"}}}
```

The expectation must match the actual case status; an unexpected pass fails.
No expectations are inferred from filenames or added automatically after a probe.

Exit codes: 0 for passing enabled checks; 1 for execution, resource, capability,
comparison or baseline failures; 2 for invalid invocation/configuration; 77 for
an explicitly optional external suite whose dataset is absent. Interruptions
write partial reports and exit 130. JSON updates atomically after each case;
HTML updates periodically and at completion.

## Tests

```sh
python3 -m unittest discover -s tools/ousddiff -v
OUSDDIFF_RUN_INTEGRATION=1 python3 -m unittest discover -s tools/ousddiff -v
ctest --test-dir build_ninja -R '^tool-ousddiff-harness$' --output-on-failure
```

Both native product configurations register the harness when tests are enabled
and the configured Python has Pillow. Select that interpreter using
`-DPython3_EXECUTABLE=/path/to/python3`. Integration tests exercise tiny local
fixtures, shared cameras, instance proxies, variants, frames, resume and history.
The optional external CTest is enabled with
`-DLIGHTUSD_OUSDDIFF_EXTERNAL_TESTS=ON`; it performs a 1% CPU assets probe.
No assets, rendered images, generated USD or local reports belong in source control.

## AMD hardware execution

On a managed coding-agent sandbox, GPU enumeration may list the host's RX 9070
XT while `/dev/dri/renderD128` and `/dev/kfd` are hidden. Run GPU validation in
the authorized host/elevated execution context. Changing file permissions inside
the sandbox does not expose the missing devices. See the repository's
[GPU device access procedure](../../doc/lusdview.md#gpu-device-passthrough-and-elevated-execution).

The verified host has Radeon RX 9070 XT (RADV GFX1201) for Vulkan and AMD
radeonsi for OpenGL. Select the installed Radeon ICD explicitly when desired:

```sh
env VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json \
  python3 tools/ousddiff/runner.py run --preset usd-assets \
    --mode probe --sample-percent 1 --no-full-scenes --max-input-mib 16 \
    --renderers storm,vk --out /tmp/ousddiff-amd-probe
```

`lusdrender -vk` and `-vkr` need no X server. The installed OpenUSD Storm build
uses an offscreen GLX/Qt context: its host `DISPLAY` must be accessible and
`glxinfo -B` must identify accelerated AMD hardware. No visible window is opened.
Xvfb commonly supplies llvmpipe on AMD, so it is not a substitute for hardware GL
in this setup. The runner rejects that fallback. Set `require_hardware: true`
and configure `backend: "vkr"` for a hardware ray-query check; its log must show
`ray_query` without a fallback. Renderer `args` can specify `-vkDevice N` after
confirming the physical-device index with `vulkaninfo --summary`.

The hardware 1% probes completed without complete large scene roots. The
restricted large-dataset leaf/Menagerie probe rendered 15/15 cases through both
Storm and Vulkan. The USD-assets probe retained a Storm MaterialX nodedef error
and two lusdrender inputs without supported Mesh geometry as failures. An
explicit MaterialX standard-library search path did not resolve the Storm
fixture error. These are separate from GPU device availability.

### Named profiles and failure reruns

`--profile amd` selects the checked-in hardware matrix: Storm, Vulkan compute,
and Vulkan ray queries. It requires an AMD Radeon device, selects the Radeon
Vulkan ICD for native GPU jobs, and requires `ray_query` in the vkr backend's
reported identity. `--renderers storm,vk` optionally narrows that matrix.
Dataset selection (`--preset`) and full-scene exclusion still apply separately.
Run the profile in the authorized host context with GPU device access:

```sh
python3 tools/ousddiff/runner.py run --profile amd --preset usd-assets \
  --mode probe --sample-percent 1 --no-full-scenes --max-input-mib 16 \
  --out /tmp/ousddiff-amd

# Retry only failed inputs, preserving individual failed frame IDs.
python3 tools/ousddiff/runner.py run --profile amd --preset usd-assets \
  --mode probe --sample-percent 1 --no-full-scenes --max-input-mib 16 \
  --failed-from /tmp/ousddiff-amd/results.json --out /tmp/ousddiff-amd-retry
```

Named profiles live under the config's `profiles` object. A profile replaces
list sections such as `renderers` and `pairs`; `render`, `resources`, and
`openusd` object values merge with the base settings. Paths are still resolved
relative to the configuration file. Renderer `device_pattern` and
`backend_pattern` are case-insensitive regular-expression searches. For an
explicit RX 9070 XT requirement, set `device_pattern` to `Radeon RX 9070 XT`.
The recorded identity always identifies the device actually used.

Results now contain per-attempt `diagnostics`, per-case `failure_codes`, and
aggregate `summary.failure_code_counts`. Nonfatal shader warnings stay
informational. Important codes include:

| Code | Meaning |
|---|---|
| `no_renderable_geometry` | The candidate found no supported geometry, including successful exits that wrote blank fallback images. |
| `materialx_nodedef_missing` | Shader generation could not find a MaterialX node definition. |
| `materialx_interface_mismatch` | The generated material inputs do not match their declaration. |
| `graphics_context_unavailable` | The offscreen OpenGL/Qt context or display could not initialize. |
| `device_mismatch` / `backend_mismatch` | Reported identity did not match the configured requirement. |
| `software_device` / `backend_fallback` | Hardware was required but a software device or fallback was selected. |

Failed render summaries retain their last attempt's wall time and RSS even
when no repetition completed. `measurement_kind` distinguishes `failed_attempt`
from `median_successful`; `total_wall_s` includes every attempted repetition and
warmup. `image_attempt` identifies which attempt supplied a retained image.
Successful timing/RSS medians continue to exclude warmups. Renderer summary
`total_attempt_wall_s` includes failed attempts and warmups as well.

The complete logs remain on disk; excerpts are bounded and classification reads
only the last 256 KiB of each log to bound parent memory. New failure codes do
not change visual thresholds or turn warnings into failures automatically.

`failed_ids.json` and `results.json.failed_ids` contain unexpected failures only;
expected failures and support/empty inputs are excluded. The HTML report can
filter by failure code and download the failing ID list. Pass that list to
`--cases-from`, or pass the original results to `--failed-from`. IDs with
`#frame=...` retain their original frame; combining them with a new `--frames`
range is rejected. Eligibility filters remain enforced on reruns. A clean
`--failed-from` report exits successfully without launching any renderers.

The dedicated tiny AMD integration test is opt-in:

```sh
OUSDDIFF_RUN_AMD_INTEGRATION=1 python3 -m unittest discover \
  -s tools/ousddiff -p test_gpu_integration.py -v
```

CMake's `LIGHTUSD_OUSDDIFF_AMD_TESTS=ON` registers the same hardware test as
`tool-ousddiff-amd-hardware`; run it in a host context with render-node access.
It verifies actual device identity and ray-query operation without loading a
complete large scene.
