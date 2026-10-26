# Loading large USD scenes in LightUSD

Production USD scenes are 10–20 GB on disk, spread across thousands of payloaded
files. This document analyzes three publicly-available scenes — **reference scene**
(Disney), **ALab** (Animal Logic), and **benchmark scene** (Activision) — and describes
how LightUSD loads them within a bounded RAM budget (target: **fit a parse into
16 GB**, geometry deferred), plus the remaining implementation work.

See also [instancing.md](instancing.md) for the reference scene instancing analysis, and
[benchmarks.md](benchmarks.md) for middle-scale public scenes
(Pixar Kitchen_set, Intel Moore Lane) that render interactively *without* the
large-scene budgeting flags described here.

## Why it is hard

| Challenge | Consequence |
|---|---|
| Tens of thousands of files | per-file open/parse cost; must parse each once |
| Geometry behind `payload` arcs | must be deferred, not loaded eagerly |
| Deep reference/variant nesting | composition must be lazy and bounded |
| Cross-directory `..`-relative asset paths | resolver must anchor each arc to its layer's directory |
| Multi-GB single files (splats, point caches) | must stream / mmap, not buffer wholesale |

The lever that makes these scenes fit is **payload deferral** (load the
composition structure, leave geometry on disk) combined with **parse-once layer
sharing** and **LOD/proxy variant selection** — *not* mmap (see §Roadmap).

---

## 1. Scene structures

### 1.1 reference scene (`<asset-root>/reference_scene`, ~17 GB)

Root `usd/reference_scene.usda` (~22 KB) — a single `Xform "reference_scene"` (kind `assembly`)
with 20 element children via `prepend references`. Each element uses a
three-file pattern (`element.usda` → `instance.usda` → `geometry.usda`) with
`prepend payload` to binary `model.usd` + XGen archives.

| Metric | Value |
|---|---|
| Composition style | scenegraph instancing + payloads |
| `instanceable = true` prototypes | 60 |
| component instances | 101 |
| `references` / `payload` | 3,924 / 214 |
| top-level `PointInstancer` | 0 (XGen instancing baked in binary `.usd`) |
| largest file | `vegetation_instancer.usd` 652 MB |

### 1.2 ALab v2.3.0 (`<asset-root>/alab/v2.3.0.zip`, ~4 GB USD in-zip)

Root `ALab/entry.usda` (309 B) composes via **subLayers**. Entities use
subLayers for department contributions (`modelling`/`surfacing`/`preview`) +
`references` into `fragment/` + **variant-selected payloads** for geometry LOD
(`geo` = base / render_high / display_high / collision / …), `instanceable`
assembly references, and `inherits`/`specializes` against `__class__`.

| Metric | Value |
|---|---|
| Composition style | subLayers + references + variants + payloads |
| USD files | ~4,298 (mostly tiny `.usda` metadata; geometry in `.usdc` payloads) |
| unique entities | 380 |
| `PointInstancer` | 0 (reference-based instancing) |
| largest files | `alab_sdr_splat.usdc` 537 MB, `alab_hdr_splat.usdc` 382 MB |

ALab is the canonical "payload everything" scene: a typical working set loads
only the chosen geometry LOD per asset (~80 % of geometry stays deferred).

### 1.3 benchmark scene (`<asset-root>/benchmark`, 9.7 GB)

Root `benchmark.usda` (text) → 4 subLayers
(`map_source/mp_wz_reference_scene.usd` + `cameras`/`breadcrumbs`/`endpoints`). The root
also authors `over` prims selecting the `districtLod = "proxy"` variant for each
map district. **63,684 files, all binary crate except the text root.**

| Metric | Value |
|---|---|
| Composition style | subLayers + variant-driven LOD + payload-deferred geometry |
| `payload` | 17,299 |
| `variantSet` (LOD `districtLod = proxy\|full`) | 8,066 |
| `references` | 4,252 |
| `PointInstancer` | 14 (clutter) |
| geometry | `assets/xmodel/generated_splined/` 4.9 GB (25.6k crate files) behind payloads |
| proxies | `assets/xmodel/generated_proxies/` 177 MB (loaded for `districtLod=proxy`) |
| player data | `layers/breadcrumbs.usd` 43 MB + `endpoints.usd` 27 MB (`Points`) |

References are `..`-relative across directories (e.g.
`@../../xmodel/.../model.geo.usd@`), which the loader must allow.

### Memory-fit summary

| Scene | LoadNone / proxy working set | Full |
|---|---|---|
| reference scene | structure only, << 1 GB | ~17 GB |
| ALab | metadata ~0.9 GB | textures 7.3 GB + geometry |
| benchmark scene | structure ~1.4 GB (measured) | ~10 GB |

All three fit a structural parse well under 16 GB when payloads are deferred.

---

## 2. The LightUSD loading strategy

### 2.1 `LargeSceneLoader` (`src/large-scene-loader.{hh,cc}`)

A one-call, memory-bounded loader that wires the existing primitives together
and **keeps the composition graph alive** so geometry can be streamed in later:

```cpp
lightusd::LargeSceneLoadOptions opts;
opts.payload_mode = PayloadMode::LoadNone;       // defer all geometry payloads
opts.allow_parent_relative_paths = true;         // benchmark scene '..' paths
opts.dedup_layers = true;                        // parse each file once

lightusd::LargeSceneLoader loader;
loader.Load("benchmark.usda", opts, &warn, &err);

loader.stage();                                  // composed structure
auto snapshot = loader.stage_snapshot();         // immutable, worker-safe view
loader.deferred_payload_paths();                 // geometry not yet loaded
loader.load_payload(path, &warn, &err);          // may run on a worker; rebuilds
```

Pipeline: `LoadLayerFromFile` (root) → `CompositeSublayers` (L phase) →
`CompositionGraph::Compose` with a payload policy → `BuildStage`. The loader
owns the resolver, `pcp::LayerRegistry`, flattened root, and `CompositionGraph`
together so `load_payload`/`unload_payload`/`deferred_payload_paths` work after
the initial load. Loading or unloading a payload now refreshes the payload
node's own arcs plus the composed descendant PrimIndices under that prim before
Stage publication, so children that exist only inside streamed geometry appear
on load and disappear again on unload; nested relative arcs inside the streamed
payload resolve against the payload file's directory; deferred-payload queries
report only payloads still in the deferred state.

Payload graph/cache mutations are serialized. `load_payload` and
`unload_payload` build a replacement Stage and publish it only after the build
succeeds, so they may run on a worker thread. Render/traversal code that overlaps
streaming must hold `stage_snapshot()`; its immutable Stage remains alive while
the loader publishes a newer snapshot. The legacy `stage()` reference remains
for single-threaded callers.

### 2.2 Payload modes

| Mode | Behavior |
|---|---|
| `LoadNone` | defer every payload (lightest; geometry streamed later) — default |
| `LoadAll` | eager (may exceed RAM on huge scenes) |
| `Budget(mb)` | load payloads until a byte budget of asset file sizes is reached, then defer the rest; an optional `payload_extent_budget` adds a projected `extentsHint`-area cap |

The policy is passed to `CompositionGraphOptions`; the extended
`payload_policy_with_prim` callback also receives the authoring `PrimSpec`, so it
can inspect `extentsHint` without breaking existing two-argument policies.
Deferred payloads are recorded in the graph and retrievable via
`GetDeferredPayloadPaths()`. `LargeSceneLoadOptions::payload_extent_budget` is
opt-in (`0` disables it) and caps the cumulative view-independent projected AABB
area (`max(XY, XZ, YZ)`, squared stage units) alongside the byte cap. Payloads
without a valid hint retain byte-only behavior.

### 2.3 Parse-once layer sharing

With `dedup_layers`, the loader installs `pcp::LayerRegistry`
(`src/pcp/layer-registry.hh`) through the new `CompositionGraphOptions`
layer-loading seam (`load_layer_fn` / `load_layer_userdata`). Each referenced
file is resolved + parsed exactly once and shared across every prim that
references it — essential for the tens of thousands of repeated prefab
references in benchmark scene.

For cold-load profiling across benchmark scene districts, use
`examples/lusdview/tests/run-benchmark-matrix.sh`. It defaults to the main
super-terrain, two season sub-scenes, chem factory, hotel, capital cliffs, and
phosphate-mine cliffs, and writes a TSV summary while keeping the dataset
outside the repository:

```sh
BENCHMARK_ROOT=$ASSET_ROOT \
  TIMEOUT_SECS=180 \
  examples/lusdview/tests/run-benchmark-matrix.sh
```

Set `SCENES_FILE` to a newline-delimited custom scene list. The harness forces
payload deferral and disables the preview cache, and reports early preview,
composition, render-prim collection, geometry estimation, first useful frame,
and full-upload timings separately.

To inventory every USD layer, including generated geometry and support layers,
run `examples/lusdview/tests/run-benchmark-inventory.sh`. It performs bounded,
parallel parse-only probes and writes one TSV row per file with its class,
parse time, peak RSS, and parsed stage bytes. Use the inventory to select
composed-root candidates for the full viewer matrix; leaf geometry files are
measured for parser cost but are not misleadingly treated as standalone scenes.

### 2.4 `..`-relative paths

`CompositionGraphOptions::allow_parent_relative_paths` (added in this work)
threads through the reference/payload/`LoadPayload` asset-path validation so
`..` segments are permitted and delegated to the resolver. Without it the DAG
engine rejected such arcs (benchmark scene composes to almost nothing).

### 2.5 Example / benchmark

`examples/large-scene-load/` loads a scene and reports process RSS (from
`/proc/self/status` VmRSS — the trustworthy peak gauge), deferred-payload count,
unique files parsed, and estimated Stage memory; with `--load-some=N` it streams
in deferred payloads to demonstrate on-demand geometry.

```
large-scene-load <scene.usd[a]> [--mode=none|all|budget] [--budget-mb=N]
                                [--no-dedup] [--load-some=N] [--unload]
```

### 2.5.1 ALab per-element geometry, textures, and procedurals

Before loading the complete ALab set, representative elements were measured
individually from `<asset-root>/alab/_merged_ALab` with `lusdview --next`,
`--load-payloads`, the `alab` large-scene profile, one headless Vulkan frame at
time 1004, and `--timing`. Geometry-only cases use the modelling fragment;
full cases use the entity layer, which adds surfacing/lighting and filesystem
textures. `lab_workbench01` is the closest bounded set element to a kitchen
work area; the extracted entity tree has no element literally named Kitchen.

| Case | Compose s | Convert s | Load total s | Meshes | ntris | Materials/textures | Peak RSS MiB | Upload / present | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Portable TV geometry-only | 0.010 | 0.022 | 0.033 | 5 | 64,693 | 4/0 | 250 | 0.01 s / 0.9 ms | pass |
| Portable TV full textured entity | 0.006 | 0.107 | 0.114 | 2 | 51,997 | 3/2 | 259 | 0.00 s / 0.5 ms | pass |
| Stoat body geometry-only | 0.013 | 0.057 | 0.072 | 3 | 80,856 | 3/0 | 243 | 0.01 s / 1.0 ms | pass |
| Stoat full character entity | 0.080 | 0.387 | 0.520 | 12 | 566,344 | 13/5 | 454 | 0.05 s / 0.6 ms | pass |
| `lab_workbench01` full textured set element | 0.902 | 6.694 | 16.250 | 538 | 3,586,150 | 336/276 | 2,114 | 0.34 s / 63.6 ms | pass |
| Baked procedurals `main.usda` (no variant override) | 0.230 | — | — | 0 | 0 | 1/0 | 376 | — / 1.1 ms | no renderable geometry |
| Baked procedurals, `alfro=render` selected | 0.038 | 0.000 | 0.630 | 0 | 0 | 3/0 | 554 | — / 541.0 ms | Curves conversion failed |

The procedural package is present at `<asset-root>/alab/usd/baked_procedurals`;
the merged `baked_procedurals/main.usda` is only a placeholder layer. Loading
the package's `main.usda` without selecting the `alfro=render` variant produces
no geometry. With the variant selected, composition succeeds and retains the
procedural stage, but `next` reports conversion failures for the Stoat body
and Remi hair Curves, yielding zero mesh draws. The next investigation should
therefore target ALab procedural Curves conversion separately from ordinary
mesh/textured elements.

Example variant wrapper:

```usda
#usda 1.0
(
    subLayers = [ @<asset-root>/alab/usd/baked_procedurals/main.usda@ ]
    upAxis = "Y"
)
over "root" {
    over "stoat" {
        over "body_M_hrc" (
            variants = { string alfro = "render" }
        ) {}
    }
}
```

### 2.6 Measured result (benchmark scene)

`large-scene-load benchmark.usda --mode=none` composes the full proxy-mode
structure — **32,811 prims, 122 files parsed, 373 deferred payloads** — in
**~1.7 GiB / ~3 s**, well within the 16 GB budget. `--load-some=N` streams the
deferred proxy geometry on demand.

**ALab** should be run from the extracted/merged tree when validating full-scene
composition locally:

```
<asset-root>/alab/_merged_ALab/entry.usda
<asset-root>/alab/_merged_ALab/entity/alab_set01/alab_set01.usda
```

The packaged `ALab/entry.usda` path is valid, but in the local extracted
directory it resolves to the small package layout rather than the full merged
shot used by the large-scene measurements. The `mk020_0281` shot composes
**3,293 prims / 1,271 deferred geometry payloads in ~120 MiB** in the legacy
large-scene loader; the set asset alone composes **3,251 prims / 1,264
payloads**.
ALab's asset-centric structure needed three further composition fixes (§3.4):
(a) composing a *referenced/payload* layer's own subLayers — ALab entity files
just sublayer their department layers, so without this the referenced content is
empty; (b) accumulating `references`/`payload` **list-ops** across a sublayer
stack — a shot's department layers each `prepend references` to `/root`, and only
the strongest survived before; (c) marking a referenced prim as having specs when
it has children (so a reference target that is just a parent of geometry expands).

The optional **`baked_procedurals`** package (value-clip + multi-payload baked
character hair/cloth; `main.usda`) composes its full structure — multi-payload
prims and the value-clip `GEO` behind the `alfro=render` variant — in a few MiB,
with the multi-GB binaries and the value-clip layer deferred. This needed the
variant-content fix (§3.5).

The `next` curve converter now accepts semantic USD vector arrays such as
`point3f[]` (the package's authored type) in addition to flattened `float[]`.
It also resolves curve points from clip metadata found on a geometry ancestor
when the composed stage retains that metadata. Direct loading of
`baked_procedurals/clip.usd` was verified with NVIDIA Vulkan at time 1004:
**1 curve, 63,957,007 tessellated samples, no curve-conversion warning**.
The `main.usda` wrapper now resolves the variant-content payload relative to
the layer that authored it; eager payload loading retains the clip-backed body
curve (**5 curves, 68,293,393 tessellated samples**). Curves that are only
procedural placeholders (authored counts but no authored points or clip owner)
are omitted without a false conversion warning.

The same composed wrapper was also exercised through `lusdrender`'s next
loader with CPU `-rtPreview`: it resolved the clip-backed data and built **10
curve strands**, with no missing-asset or curve-conversion diagnostics. The
Vulkan LightRT bridge does not expose the CUDA analytic curve primitive types,
so `lusdrender -vk/-vkr` now builds the same round-linear LightRT curve scene
and tessellates it at the Vulkan upload boundary. The Vulkan curve-only smoke
run rendered the same **10 curve strands** through the RTX GPU triangle path.
This preserves GPU rendering for mixed triangle/round-curve scenes too;
flat/ribbon curves remain on the CPU direct path until a view-dependent Vulkan
ribbon primitive is available. The very large ALab procedural wrapper still
exceeds the current Vulkan upload/engine budget after tessellation.

The same GPU upload bridge now covers `UsdGeomPoints`: point widths become
low-cost round point spheres, while authored point normals become double-sided
disc geometry. This is used by the Vulkan, HIP, and D3D triangle backends. The
minimal public regression fixture `tests/usda/lusdrender-points.usda` renders
on the NVIDIA Vulkan path as **160 GPU triangles**. In the LightRT path,
authored-normal points are retained as analytic disk/oval records for CUDA,
HIP, and Vulkan; points without normals retain the bounded sphere proxy. The
GPU collector reads point arrays lazily, avoiding a second full
positions/widths/normals copy before bounded upload chunks are formed.

ALab's `extras/alab_sdr_splat.usdc` is a
`ParticleField3DGaussianSplat` with **2,274,589** particles. The GPU collector
now reads positions, scales, orientations, opacity, and DC spherical-harmonic
radiance, mapping each covariance ellipsoid to a batched oriented ellipse
upload. A full triangleized upload reaches **15,314,240 triangles** and exceeds
the current LightRT Vulkan engine/upload budget. An explicit
`LUSDR_GAUSSIAN_MAX=N` budget is available for diagnosis; even a 10,000-splat
ALab load still failed Vulkan engine creation in this run. The default does not
silently truncate the asset.

### 2.6.1 Rendering benchmark scene (lusdrender + lusdview, NVIDIA RTX 5060 Ti)

Both viewers compose benchmark scene through the `next` loader (`LoadUSDComposed`,
payloads eager), which handles its `..`-relative payload paths and variant LOD.
The **default Tydra loader rejects benchmark scene** ("Unsafe asset path" on the
`..`-relative payloads), so lusdview must use `--next`.

**Scene facts.** The default (root-authored) LOD is `districtLod = proxy`
(12.67 M default-purpose triangles + ~26 M `purpose = guide` breadcrumb/endpoint
Points). The map is a flat ~8 km reference_scene, so **auto-framing the whole bounds is
useless** — frame a scene camera instead (`layers/cameras.usd`:
`phospate_mine_overview`, `map_capital_square`, `map_capital_overview`,
`map_airfield_overview`, `tile_p_beach`, …). **Use the exact prim name** —
most cameras are `map_`-prefixed (`map_capital_square`, not `capital_square`);
a name that doesn't match **silently falls back to auto-fit** (the useless
whole-reference_scene shot). List them with
`build/lusdcat layers/cameras.usd | grep 'def Camera'`. The guide Points
triangulate into huge planes that engulf the camera, so they must be hidden
(both viewers hide `guide` by default).

**lusdrender** (ray tracing) — renders the proxy correctly on every backend at
1280×720, `~4–5 GiB` RSS:

```sh
T=tests; M=<asset-root>/benchmark/benchmark.usda
./build/tools/lusdrender/lusdrender $M cald_cpu.png -rtPreview -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14
./build/tools/lusdrender/lusdrender $M cald_vk.png  -vk  -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14  # GPU compute trace
./build/tools/lusdrender/lusdrender $M cald_vkr.png -vkr -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14  # GPU ray query
# full LOD: NOT  -variant districtLod=full  (no-op — root authors per-district proxy). See §2.6.2 (wrapper layer).
```

This needed the GPU-collector fix: `-vk/-vkr` previously emitted **raw local**
points for every Mesh (no world transform / purpose / mask), piling all
districts at the origin; they now reuse the CPU path's world-space,
purpose-filtered, masked collection. (Native instances are not yet emitted on
the GPU path — negligible for benchmark scene: ~29 K of 12.70 M tris.)

**lusdview** (`--next`) — frame a scene camera with the new `--camera` flag:

```sh
./build/lusdview --headless --next --backend gl --camera phospate_mine_overview --frames 6 --screenshot out.ppm $M   # best shading
./build/lusdview --headless --next --backend vk --camera phospate_mine_overview --frames 6 --screenshot out.ppm $M   # raster
```

- `--camera <name>` resolves the named USD Camera's world pose from the `--next`
  stage and drives the orbit rig (lusdview otherwise auto-fits the whole scene
  and ignores USD cameras).
- The **GL** backend renders the district cleanly (buildings, terrain, the mine
  tank). The **VK rasterizer** renders the geometry correctly but with flat
  geometric-normal shading (dark, low contrast — a *shading* limitation, not a
  geometry one). This path was unblocked by fixing a latent
  `normalize(vec3(0))` NaN in `mesh.vert` that clipped every zero-normal mesh
  (the `--next` flat preview stores zero normals); before the fix `--next`
  rasterized **nothing** on VK (even plain suzanne).
- lusdview's 30 M default `--max-tris` budget **truncates** benchmark scene; raise it
  (`--max-tris 40000000`) to load the whole proxy (the log prints `truncated`).

**Known limitations (follow-ups).** ~~lusdview's VK ray-query (`--rt`) is
blank on benchmark scene~~ — **fixed**: the RT TLAS included *every* USD purpose, so
benchmark scene's ~26 M-tri `guide` breadcrumb planes (hidden in raster) engulfed the
camera; the TLAS now skips purposes hidden in the UI and the mine district
ray-traces correctly (the July 2026 recipe is in the §2.8 historical note
and in git history). The VK-raster flat shading still wants the smooth-normal +
headlight treatment the lusdrender preview already uses. For full LOD, see §2.6.2.

### 2.6.1-a reference scene element composition and lusdview timings

The following table records each partial element under
`<asset-root>/reference_scene/usd/elements/*/element.usda`. Each run used the `next`
loader, eager payload resolution (`--load-payloads`), the reference scene large-scene
profile, one headless Vulkan frame at time 1, and `--timing`.

`USD tree MiB` is the sum of non-AppleDouble `*.usd`, `*.usda`, `*.usdc`, and
`*.usdz` files below that element directory. It is a conservative whole-file
footprint, including referenced/payloaded files and files available to
variants. `Parse/source ms` is the next-core source-discovery and layer-load
phase; `Compose s` is the composed-stage rebuild. `Extract s` combines point-
instancer, traversal, points/curves, native-instance, render-prim, and
post-extraction phases. `Convert s` is mesh flatten/batch conversion, and
`Load total s` is lusdview's finalize total. `GPU upload s` and `Present ms`
come from `LUSDVIEW_TIME_UPLOAD=1 LUSDVIEW_TIME_FRAME=1`.

The initial smoke pass selected the NVIDIA RTX 5060 Ti for every element. The
detailed timing pass below ran on Vulkan llvmpipe after the NVIDIA kernel driver
became unavailable; upload/present values must therefore be treated as
software-renderer measurements, not NVIDIA performance numbers. This does not
change the composition or conversion statistics.

| Element | USD tree MiB | Meshes | ntris | Materials/textures | Parse/source ms | Compose s | Extract s | Convert s | Load total s | GPU upload s | Present ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| isBayCedarA1 | 100.4 | 15004 | 4067550 | 6/0 | 207.6 | 4.965 | 3.999 | 1.437 | 10.862 | 1.29 | 203.3 |
| isBeach | 712.4 | 201 | 158676 | 203/0 | 532.0 | 0.666 | 5.009 | 0.079 | 5.756 | 4.23 | 3783.8 |
| isCoastline | 89.1 | 1 | 115400 | 2/0 | 2.9 | 0.006 | 0.003 | 0.069 | 0.079 | 0.01 | 2.0 |
| isCoral | 415.0 | 13812 | 17423268 | 13668/0 | 321.1 | 8.597 | 2.925 | 14.473 | 26.149 | 2.53 | 584.1 |
| isDunesA | 24.8 | 835 | 244182 | 836/2 | 105.3 | 0.626 | 0.245 | 1.031 | 1.911 | 0.09 | 3.7 |
| isDunesB | 157.6 | 20001 | 4063416 | 8/0 | 2143.6 | 9.453 | 14.467 | 0.318 | 24.334 | 5.12 | 3512.0 |
| isGardeniaA | 2.3 | 1 | 12344 | 2/0 | 233.9 | 0.257 | 0.092 | 0.001 | 0.386 | 0.00 | 2.9 |
| isHibiscus | 5.5 | 3324 | 255756 | 3/1 | 19.0 | 0.560 | 0.647 | 0.017 | 1.266 | 0.18 | 30.3 |
| isHibiscusYoung | 0.4 | 43 | 14012 | 3/0 | 4.2 | 0.013 | 0.026 | 0.000 | 0.043 | 0.00 | 2.5 |
| isIronwoodA1 | 102.4 | 1 | 486526 | 4/0 | 2.7 | 0.007 | 0.795 | 0.000 | 0.802 | 0.04 | 547.3 |
| isIronwoodB | 100.9 | 2 | 486594 | 5/0 | 86.9 | 0.091 | 0.500 | 0.275 | 0.867 | 0.03 | 478.0 |
| isKava | 2.1 | 539 | 122416 | 105/0 | 38.0 | 0.094 | 0.234 | 0.002 | 0.336 | 0.05 | 12.5 |
| isLavaRocks | 1.8 | 20 | 115264 | 21/0 | 0.8 | 0.012 | 0.004 | 0.045 | 0.062 | 0.01 | 0.9 |
| isMountainA | 64.2 | 1 | 67006 | 2/0 | 93.9 | 0.253 | 0.049 | 0.046 | 0.350 | 0.01 | 0.8 |
| isMountainB | 866.9 | 10 | 1747604 | 9/6 | 88.9 | 0.243 | 5.210 | 0.847 | 6.303 | 0.40 | 2816.7 |
| isNaupakaA | 0.3 | 24 | 5904 | 25/0 | 0.7 | 0.019 | 0.033 | 0.000 | 0.053 | 0.00 | 6.4 |
| isPalmDead | 3.6 | 2 | 309038 | 3/0 | 0.7 | 0.002 | 0.000 | 0.262 | 0.267 | 0.07 | 0.9 |
| isPalmRig | 283.7 | 1980 | 4366560 | 2047/0 | 9.7 | 1.224 | 1.055 | 2.636 | 4.941 | 0.43 | 912.9 |
| isPandanusA | 16.2 | 59 | 15964 | 62/0 | 10.0 | 0.030 | 0.161 | 0.000 | 0.196 | 0.01 | 70.2 |
| osOcean | 104.1 | 1 | 15592264 | 2/0 | 0.3 | 0.002 | 0.000 | 12.639 | 14.267 | 1.16 | 0.5 |

The largest composed element trees are `isMountainB` (866.9 MiB), `isBeach`
(712.4 MiB), and `isCoral` (415.0 MiB). Composition is dominated by source
discovery for `isDunesB` (2.14 s) and by extraction/conversion for the dense
`isCoral` and `osOcean` payloads. The `ntris` column is the post-composition
loaded mesh total, while the last two columns measure the separate raster
upload/present path and can vary substantially with the software renderer.

### 2.6.1-b Next-core benchmark recipes (`lusdcat` + `tydra_to_renderscene`)

For reproducible next-core timing and memory checks on public large scenes, use the
following commands from a `build_ninja` tree where both tools are built
(`lusdcat` is the next CLI; the former `next_usdcat` name is kept as a CMake
target alias):

```sh
BUILD_DIR="${BUILD_DIR:-./build_ninja}"
NEXT_TUSD="${NEXT_TUSD:-${BUILD_DIR}/lusdcat}"
NEXT_RENDER="${NEXT_RENDER:-${BUILD_DIR}/tydra_to_renderscene}"
```

1) `lusdcat`: `compose` and `timing` with native payload policy:

```sh
[ -x "$NEXT_TUSD" ] || { echo "SKIP: lusdcat not found at $NEXT_TUSD"; exit 0; }
if [ ! -f "$1" ]; then
  echo "SKIP: input missing: $1"
  exit 0
fi

# Structural compose profile (deferred geometry payloads)
LIGHTUSD_NEXT_TIMING=1 "$NEXT_TUSD" -l \
  --compose-threads-auto --defer-payloads "$1"

# Flattened USDA output check (native instance flattening behavior)
LIGHTUSD_NEXT_TIMING=1 "$NEXT_TUSD" -f \
  --compose-threads-auto --defer-payloads -o "/tmp/$(basename "$1").usda" "$1"

# Flattened USDC output profile
LIGHTUSD_NEXT_TIMING=1 "$NEXT_TUSD" -f \
  --compose-threads-auto --load-payloads -o "/tmp/$(basename "$1").usdc" "$1"
```

2) `tydra_to_renderscene`: `--next` conversion and low-memory release path:

```sh
[ -x "$NEXT_RENDER" ] || { echo "SKIP: tydra_to_renderscene not found at $NEXT_RENDER"; exit 0; }
if [ ! -f "$1" ]; then
  echo "SKIP: input missing: $1"
  exit 0
fi

"$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --memstat --timecode 0 "$1"

# Static-scene benchmark (skip time-sample animation extraction)
"$NEXT_RENDER" --next --compose-threads-auto --defer-payloads --no-animation \
  --memstat --timecode 0 "$1"

# Low-memory mode (releases next Stage static geometry arrays after conversion)
"$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --lowmem --memstat --timecode 0 "$1"
```

3) Scene-specific invocation examples used in this document:

```sh
# benchmark scene (proxy composition baseline)
BENCHMARK=<asset-root>/benchmark/benchmark.usda
[ -f "$BENCHMARK" ] && LIGHTUSD_NEXT_TIMING=1 "$NEXT_TUSD" -l \
  --compose-threads-auto --defer-payloads "$BENCHMARK"

# ALab merged scene proxy mode
ALAB=<asset-root>/alab/_merged_ALab/entry.usda
[ -f "$ALAB" ] && "$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --lowmem --memstat "$ALAB"

# reference scene payload-heavy scene
REFERENCE_SCENE=<asset-root>/reference_scene/usd/reference_scene.usda
[ -f "$REFERENCE_SCENE" ] && "$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --lowmem --memstat "$REFERENCE_SCENE"
```

For interactive prefab timing, use
`examples/lusdview/tests/run-large-scene-prefab-benchmark.sh`. It reports both
the first useful frame and the full upload/presentation marker from `--timing`.
The benchmark is opt-in because production scene assets are external.
Set `PAYLOAD_MODE=defer PREVIEW_CACHE=off` for cold-load measurements; override
`COMPOSE_THREADS` and `CONVERT_THREADS` when comparing CPU scaling across
district sub-scenes.

For comparisons between commits/branches:

- Keep both runs on the same payload mode (`--defer-payloads` vs `--load-payloads`).
- Pin thread policy (`--compose-threads-auto` or a fixed value) for less run-to-run
  variance.
- Capture `/proc/self/status` `VmRSS` and tool prints around `--memstat` to
  separate conversion cost from stage materialization cost.
- Use `--no-animation` for static-scene conversion comparisons (without
  per-prim time-sample scans) when timing geometry + scene traversal only.

### 2.6.2 Full LOD (`districtLod = full`) — per-shot, not whole-reference_scene

**Whole-reference_scene full LOD does not fit on a 62 GiB host, and the wall is host
RAM, not VRAM** (a 125 GiB host does complete it — see §2.6.8).
Both viewers (and every lusdrender backend, including `-vk`/`-vkr`) *eagerly
compose all 45 districts' full payloads into host-RAM arrays and build a single
in-memory triangle soup before anything is uploaded to the GPU.* On a 62 GiB
host, promoting all districts to `full` climbs steadily during composition and
exceeds **50 GiB while still composing** — it never reaches the triangle count
or BVH build, let alone the GPU. So the 16 GiB VRAM budget is irrelevant: the
host-side working set is the limit, and there is no out-of-core / chunked-BVH
path today (one soup, one BLAS).

Two gotchas when forcing full LOD:

- **A global `-variant districtLod=full` has no effect.** The root `benchmark.usda`
  authors an explicit per-district `over … (variants = {string districtLod =
  "proxy"})` for all 45 districts, and an authored selection beats a load-time
  override. You must re-author the selection in a **stronger wrapper layer**.
- **`-maxMem` does not bound composition.** It guards the triangle-stream / BVH
  allocations (throwing `bad_alloc` → clean abort), but the `next` compose phase
  balloons freely. Past the host's physical RAM the OS OOM-killer fires before
  any clean abort — run heavy full-LOD jobs under a memory cgroup
  (`systemd-run --user --scope -p MemoryMax=50G -p MemorySwapMax=0 …`) so only
  the job dies, never the machine.

**Per-shot full LOD is the production-correct approach and fits comfortably.**
Promote only the district the shot frames; leave the rest proxy. Write a wrapper
that sublayers the scene and re-authors *just that district* to `full`:

```usda
#usda 1.0
(
    subLayers = [ @<asset-root>/benchmark/benchmark.usda@ ]
    upAxis = "Z"
)
over "world" { over "mp_wz_reference_scene" { over "mp_wz_reference_scene_paths" {
    over "mp_wz_reference_scene_geo" {
        over "map_phosphate_mine" ( variants = { string districtLod = "full" } ) {}
    }
}}}
```

Render the wrapper exactly like the scene, framing the matching camera:

```sh
W=benchmark_mine_full.usda   # the wrapper above
# lusdrender CPU ray tracing:
./build/tools/lusdrender/lusdrender $W mine_cpu.png -rtPreview \
    -camera phospate_mine_overview -w 1280 -height 720 -maxMem 45 -stats
# lusdrender GPU ray query (hide guide: 'default' must be in the visible list):
./build/tools/lusdrender/lusdrender $W mine_vkr.png -vkr \
    -camera phospate_mine_overview -purpose default,render,proxy -w 1280 -height 720 -maxMem 45
# lusdview VK rasterizer (raise --max-tris; full mine is ~88 M unique / ~184 M effective tris):
./build/lusdview --headless --next --backend vk --camera phospate_mine_overview \
    --max-tris 60000000 --frames 4 --screenshot mine_vk.ppm $W
```

Measured on the RTX 5060 Ti / 62 GiB host (phosphate-mine shot, mine at full,
rest proxy):

| Path | Result | Peak host RSS | Notes |
|------|--------|--------------|-------|
| lusdrender `-rtPreview` (CPU) | ✅ full detail | 11.9 GiB | 29.8 M default-purpose tris (vs 12.67 M proxy), 35 M unique |
| lusdrender `-vkr` (GPU ray query) | ✅ full detail | 9.1 GiB | 35.3 M tris traced on the GPU; BLAS fits 16 GiB VRAM |
| lusdview `--backend vk` (raster) | ✅ full detail | 15.3 GiB | 162.8 M tris drawn, 57 k instances; richest result |
| lusdview `--rt` (VK ray query) | ⚠️ blank | 11.4 GiB | 30 M-tri cap truncates away the hero geometry (only guide lines remain) — the §2.6.1 large-scene VK-RT limitation |

The mine at full LOD reveals geometry the proxy lacks — lattice crane towers,
domed silos, palm trees, detailed buildings, the circular tank, rocks and
pipework. Note lusdview counts ~88 M unique tris for the full mine (more than
lusdrender's 35 M) because the `--next` converter expands instancing differently;
its 30 M `--max-tris` default truncates heavily, so raise it for raster (which
scales) but expect `--rt`/`--cuda` to stay capped on 16 GiB VRAM.

**Per-district full-LOD gallery.** The same per-shot recipe (one district to
`full`, frame its camera, `lusdrender -rtPreview`) renders any district at full
detail within the host budget. Every district measured so far fits comfortably
as a single promotion (18–50 M default-purpose tris, ≤21 GiB host RSS) — it is
only the whole *reference_scene* at full that does not fit (see top of this section).

| District (camera) | Default tris @ full | View |
|---|---:|---|
| `map_phosphate_mine` (`phospate_mine_overview`) | 29.8 M | ![phosphate mine](images/benchmark/benchmark-phosphate-mine-full.jpg) |
| `map_capital` (`map_capital_square`) | 44.3 M | ![capital square](images/benchmark/benchmark-capital-square-full.jpg) |
| `map_beachhead` (`map_beachhead_village`) | 23.6 M | ![beachhead village](images/benchmark/benchmark-beachhead-village-full.jpg) |
| `map_tile_p` (`tile_p_beach`) | 27.8 M | ![tile_p beach](images/benchmark/benchmark-tile-p-beach-full.jpg) |
| `map_airfield` (`map_airfield_overview`) | 36.9 M | ![airfield](images/benchmark/benchmark-airfield-full.jpg) |
| `map_arsenal` (`map_arsenal_ship`) | 24.1 M | ![arsenal shipyard](images/benchmark/benchmark-arsenal-shipyard-full.jpg) |
| `map_agricultural_center` (`map_agricultural_center_bridge`) | 18.1 M | ![agricultural center](images/benchmark/benchmark-agricultural-center-full.jpg) |

Every other authored camera frames one of these same districts from a different
pose — the district is promoted to `full` exactly as above, only the `-camera`
changes:

| District (camera) | Default tris @ full | View |
|---|---:|---|
| `map_phosphate_mine` (`phospate_mine_bridge`) | 29.8 M | ![mine bridge](images/benchmark/benchmark-phosphate-mine-bridge-full.jpg) |
| `map_capital` (`map_capital_overview`) | 44.3 M | ![capital overview](images/benchmark/benchmark-capital-overview-full.jpg) |
| `map_beachhead` (`map_beachhead_overview`) | 23.6 M | ![beachhead overview](images/benchmark/benchmark-beachhead-overview-full.jpg) |
| `map_tile_p` (`tile_p_road`) | 27.8 M | ![tile_p road](images/benchmark/benchmark-tile-p-road-full.jpg) |
| `map_airfield` (`map_airfield_waiting_room`) | 36.9 M | ![airfield waiting room](images/benchmark/benchmark-airfield-waiting-room-full.jpg) |
| `map_arsenal` (`map_arsenal_overview`) | 24.1 M | ![arsenal overview](images/benchmark/benchmark-arsenal-overview-full.jpg) |

(1280×720 `lusdrender -rtPreview -purpose default,render,proxy` shots, downscaled
to 512 px JPEG for the repo. The colored speckles in some frames are neighboring
districts still at `proxy`, drawn with their `displayColor`. The default-tri
count is per *district*, so it is identical across that district's cameras.)

#### Per-district `lusdview --next` composition measurements

These measurements promote one district to `districtLod = "full"` in a stronger
wrapper layer and leave the other 44 districts at their authored `proxy` LOD.
The input tree is `<asset-root>/benchmark` (10,496,782,370 bytes, about 10.50
GB, including all USD and texture assets). Each run used `--load-payloads`,
`--large-scene-profile balanced`, one headless Vulkan frame at time 1, and
`--timing`. `Extract` is the sum of next-core extraction/collection phases;
`Load total` is the finalize total; `Upload` and `Present` are from
`LUSDVIEW_TIME_UPLOAD=1 LUSDVIEW_TIME_FRAME=1`.

The NVIDIA RTX 5060 Ti smoke backend was unavailable during this run
(`nvidia-smi` could not communicate with the driver), so the detailed timing
pass used Vulkan llvmpipe. Composition and conversion timings remain useful;
upload/present timings are software-renderer measurements. `capital` reached
finalization but exceeded the 180 s per-run limit before GPU upload, so its
upload/present fields are intentionally blank.

| District promoted to full | Meshes | Unique/effective tris | Instances | Parse/source ms | Compose s | Extract s | Convert s | Load total s | Peak RSS MiB | Upload s | Present ms | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| map_agricultural_center | 1648 | 42,880,177 / 43,900,288 | 38,783 | 186.9 | 29.539 | 13.755 | 19.754 | 63.794 | 11,134 | 5.22 | 397.6 | pass |
| map_airfield | 3259 | 45,325,419 / 139,405,185 | 135,885 | 587.8 | 80.025 | 35.560 | 29.673 | 147.466 | 21,945 | 5.80 | 717.7 | pass |
| map_arsenal | 2347 | 43,621,773 / 68,327,795 | 66,493 | 361.7 | 51.887 | 22.594 | 22.034 | 97.766 | 15,931 | 5.06 | 489.6 | pass |
| map_beachhead | 2453 | 44,248,206 / 149,017,863 | 120,853 | 376.0 | 53.967 | 23.745 | 20.860 | 100.218 | 16,422 | 5.20 | 519.3 | pass |
| map_capital | 3623 | 45,498,738 / 164,146,391 | 162,769 | — | 102.006 | 43.450 | 30.525 | 178.830 | 26,134 | — | — | timeout during upload |
| map_phosphate_mine | 7719 | 45,775,280 / 108,414,347 | 90,478 | 365.9 | 50.590 | 24.133 | 17.562 | 93.693 | 15,411 | 4.74 | 634.4 | pass |
| map_tile_p | 5848 | 42,998,848 / 57,564,635 | 41,333 | 345.6 | 49.055 | 20.894 | 24.837 | 95.860 | 14,952 | 3.70 | 407.3 | pass |

The full-promotion cost is not proportional to the selected district alone:
every row still composes the complete proxy reference_scene and its shared layers. The
capital and airfield rows are the heaviest host-memory cases; the phosphate
mine has the most converted mesh nodes because its full payload is highly
fragmented. This is why per-shot full LOD remains preferable to promoting all
45 districts at once.

#### Splitting `map_capital` into smaller load units

The `map_capital` full payload is itself a 42,470-byte crate, but its composed
content contains 114 top-level children below `/world/map_capital`. Loading
that crate directly is much cheaper than composing the complete reference_scene wrapper,
but it still produces a large single conversion batch. A stronger decomposition
is to author one small wrapper per top-level child and reference the child path
inside the capital crate:

```usda
#usda 1.0
(
    upAxis = "Z"
    defaultPrim = "world"
)
def Xform "world" {
    def Xform "map_capital" {
        def Xform "cityhall_01" (
            prepend references = @.../season_4/map_capital.usd@
                </world/map_capital/cityhall_01>
        ) {}
    }
}
```

This preserves the authored world transform while allowing the viewer or a
streaming scheduler to load/convert each child independently. The following
comparison used `--next --load-payloads --large-scene-profile balanced`, one
headless Vulkan frame, and the same timing flags as the table above. The
software Vulkan backend was used because the NVIDIA driver was unavailable.

| Input decomposition | Compose s | Convert s | Load total s | Meshes | ntris | Peak RSS MiB | GPU upload s | Present ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Full-reference_scene wrapper, `map_capital = full` | 102.006 | 30.525 | 178.830 | 3623 | 45,498,738 | 26,134 | — | — |
| Direct `map_capital.usd` crate | 49.396 | 41.780 | 125.879 | 3614 | 39,402,788 | 17,712 | 7.33 | 556.1 |
| One child wrapper: `cityhall_01` | 2.163 | 1.298 | 4.724 | 337 | 2,670,852 | 1,108 | 0.39 | 60.2 |

The direct crate finishes under the previous 180-second limit, but the
top-level-child split is the robust approach: each unit is small enough for
bounded conversion and can be scheduled independently. The remaining work for
production streaming is generating these child wrappers from the 114 authored
children and retaining the parent capital transform when assembling the
results.

### 2.6.3 `-lodStream`: automatic view-dependent district LOD (lusdrender)

The per-shot wrappers above pick the `full` district by hand. `lusdrender
-lodStream` does it automatically: a cheap **proxy pass** composes the scene,
scores each district by **screen-space importance**, then promotes the
top-scoring districts to `full` (via a generated wrapper layer) until a host-RSS
/ GPU-VRAM budget is hit; the rest stay `proxy`. This is the load-time form of
streamed LOD — only the selected `full` payloads get composed and made resident
— and the designed use of the `districtLod` variant.

The importance score is **projected coverage × view-alignment**:
`proxyVerts / distance² × max(0, dot(dir-to-district, camera-forward))²`. Pure
camera distance is not enough — for an *overview* camera it rewards small
gameplay overlays sitting near the eye over the dense district the shot actually
frames. Coverage adds geometric weight; the alignment term adds "what the camera
looks at". (E.g. `phospate_mine_overview`: distance ranks `map_loot_zones`
first and `map_phosphate_mine` 8th; the coverage+alignment score ranks the mine
first at `align=0.98`.)

```sh
M=<asset-root>/benchmark/benchmark.usda
# CPU ray tracing, default budgets (host = 50% of MemAvailable):
./build/tools/lusdrender/lusdrender $M out.png -lodStream -rtPreview \
    -camera map_capital_square -purpose default,render,proxy -w 1280 -height 720 -stats
# GPU ray query also caps by VRAM (default = 50% of the device-local heap):
./build/tools/lusdrender/lusdrender $M out.png -lodStream -vkr -camera map_capital_square ...
```

Flags: `-maxMem <GiB>` (host budget, else 50% MemAvailable), `-maxVram <GiB>`
(GPU budget, else 50% of the device-local heap — queried via a new
`lrt_vk_device_local_bytes()`), `-lodDistrictMem`/`-lodDistrictVram` (the flat
per-district cost charge; default 10 / 3 GiB), `-lodMinVerts`/`-lodMaxVerts`
(proxy-vert band; skip tiny trigger/volume children and sprawling non-district
overlays that author no `full` geometry — e.g. a 14.6 M-vert spawn-marker set
that would otherwise hijack the ranking; defaults 1000 / 2000000),
`-lodContainer` (the namespace prim whose children are districts; default
`mp_wz_reference_scene_geo`).

Measured (`phospate_mine_overview`, RTX 5060 Ti / 62 GiB host, default budgets):

```
[lodStream] budgets: host 24.7 GiB (avail 49.4, proxy 2.1)
[lodStream] 36 districts (10 sub-threshold skipped), promoting 2 to full ...
[lodStream]   FULL map_phosphate_mine score=1.67e-3 align=0.98   FULL map_tile_f align=0.92   proxy (rest) ...
```

It auto-promoted `map_phosphate_mine` (what the camera frames) + the in-view
`map_tile_f`, leaving the other 34 districts proxy. CPU peak RSS **13.7 GiB**.
Across `map_capital_square` / `phospate_mine_overview` / `map_beachhead_village`
the score always promotes the framed district first (capital, mine, beachhead
respectively). On the capital shot at default budgets, GPU built the 50 M-tri
BLAS within the 16 GiB card with VRAM the binding budget (7.96 GiB → 2
districts); CPU peak RSS there was 19.5 GiB (est 22.1 — the estimate runs
slightly high, i.e. safe).

**Cost model is intentionally simple.** Proxy geometry does not predict `full`
cost (a low-poly stand-in's vertex count is uncorrelated with the heavy geometry
behind its `full` variant — a 0.25 M-vert capital proxy explodes to 44 M `full`
tris), so the charge is a **flat per-district constant** rather than a
proxy-scaled estimate. Proxy-vert count is used only as a coarse
importance/footprint signal, and only within the `-lodMinVerts`..`-lodMaxVerts`
band — outside it lie trigger/volume prims (tiny) and sprawling non-district
overlays (e.g. the 14.6 M-vert `map_vehicle_spawns` spawn-marker set) that
author no `full` geometry and would otherwise hijack the ranking. Tune
`-lodDistrictMem` up to promote fewer districts / down to promote more.
When `-maxMem` is not set, the hard memory-abort cap is raised to 90% of
MemAvailable — *above* the 50%-of-avail selection budget — so a modest
underestimate on the heaviest district pair completes instead of aborting (the
abort cap stays an OOM safety net, not a tight selection bound). **Limitations**
(true streaming follow-ups): the selection is computed once at load (not per
frame), the chosen set is composed in a single soup/BLAS (no eviction), and the
cost charge ignores per-district size variation — so on a tight machine lower
`-lodDistrictMem` (fewer promotions) or set `-maxMem` to a higher explicit cap.

### 2.6.4 Realtime reference_scene preview: GPU-budget LOD + robust auto-framing (lusdview)

`-lodStream` (above) is a lusdrender, ray-tracing, load-time selection. The
interactive **lusdview** rasterizer hits a different wall on a fully assembled
scene. The Moana reference_scene (`<asset-root>/reference_scene`) composes via `--next` to
**83,801 draws / 42.9 M instances / 56.5 M unique tris**, and after instance
dedup the geometry itself fits comfortably in 16 GiB VRAM (instance transforms
~2.06 GiB). The problem is **draw/buffer count**: the per-mesh raster path
creates ~84 k host-visible Vulkan buffers and stalls for **7+ minutes** before
the first frame. (A plain `--next --backend vk` whole-reference_scene run times out in
the buffer-creation loop, not on memory.)

**`--max-draw-meshes N` / `--max-gpu-mem <GiB>` (GPU-budget LOD).**
`gpu_budget_lod.cc:ApplyGpuBudgetLOD()` runs once after load: it ranks meshes by
prototype size, keeps the **N most prominent** at full geometry within the count
and VRAM budgets, and **merges the entire long tail into one instanced bbox-proxy
mesh** — a unit cube instanced once per overflow instance, each instance
transform box-fitting that original prototype's object-space AABB, tinted by the
mesh's flat color. The proxy preserves instancing (so its ~2 GiB instance buffer
still fits) and per-instance frustum cullability (proxy prototype AABB is the
unit cube, transformed per instance). The scene then uploads as **~N+1 buffers**.
Simple preview only: the proxy boxes shade flat, no shadows/GI.

```sh
M=<asset-root>/reference_scene/usd/reference_scene.usda
# 40 GiB host cgroup; keep the 4000 biggest meshes full, cap full VRAM at 16 GiB,
# merge the other ~79.8 k meshes into one bbox proxy:
systemd-run --user --scope -p MemoryMax=44G -p MemorySwapMax=2G \
  ./build/lusdview --headless --next --backend vk \
    --max-tris 80000000 --max-gpu-mem 16 --max-draw-meshes 4000 \
    --frames 2 --screenshot reference_scene.ppm $M
```

```
next: 83801 draws, 42869753 instances, 56553204 unique tris, instXform VRAM ~2.06 GB
gpu-budget LOD: 4000 meshes full (0.08 GiB), merged 79801 meshes / 42730328
                instances into bbox proxy (now 4001 draws)
render stats: 4001/4001 meshes visible, 42869757 instances, 581550424 drawn tris, 4001 draw calls
```

**`--no-robust-frame` (robust auto-framing, on by default).** The reference_scene's *raw*
scene bbox is **170,091 units across** — a handful of far-flung meshes sit at
±tens of thousands while the actual reference_scene occupies a ~9,200-unit region near the
origin. `fitToScene` on the raw bbox renders the reference_scene as a sub-pixel speck.
`ComputeRobustSceneBounds()` fixes the framing with **mass-weighted endpoint
trimming**: each mesh's world-AABB min/max endpoints carry its geometry mass
(`instanceCount × triCount`), and per axis 1% of the *mass* is trimmed from each
tail. Sparse far elements weigh ~nothing and drop out; the dense reference_scene defines
the kept range. It only overrides framing when the result is materially tighter
(robust diag < 70% of full), so normal scenes that fill their bbox are
untouched. It is computed from the full per-mesh set **before** the LOD merge
(post-merge the proxy's 42 M instances would swamp the weighting) and cached.
The named-USD-camera path (`--camera`) is unaffected.

```
robust auto-frame: trimmed 1% outliers, scene diag 170091.6 -> 9265.6 (95% smaller)
```

With both on, the whole reference_scene uploads in seconds instead of stalling, fits well
under 16 GiB VRAM, renders all 42.9 M instances through the existing per-instance
frustum culling, and frames Motunui (shoreline, palms as full trunks + bbox-proxy
foliage) instead of a speck. **Limitations:** the proxy is rebuilt only on load
(not per frame / per view), it is a flat-shaded box soup (no materials/textures
on the merged tail), and the full-mesh set is chosen by prototype size, not
screen-space importance — a per-view promotion pass (à la `-lodStream`) is the
follow-up.

### 2.6.5 Per-frame view-dependent raster LOD (`--raster-lod`, lusdview)

§2.6.4's GPU-budget LOD is a **load-time** decision: it bounds upload/VRAM by
fixing one full/proxy split for the whole session. `--raster-lod` is the
**per-frame, view-dependent** follow-up it called for. It runs inside the
existing raster instance cull (`compactMeshInstances`, shared by the sync,
headless, and async-worker cull paths), so it composes with — and is independent
of — the load-time merge.

For every instance that passes the frustum cull, the cull classifies it by its
**projected screen radius** (`ProjectedRadiusPx` = `focalPx · worldRadius /
viewDepth`, from the instance's world-AABB):

- `px < --raster-lod-cull-px` (default `1.5`) → **dropped** (sub-pixel).
- `px < --raster-lod-full-px` (default `48`) → **collapsed to a shared unit-box
  proxy** (box-fit onto the instance's world AABB via `BoxFitXform`, tinted by
  the instance color), accumulated across all prototypes into one instanced draw.
- otherwise → drawn **full-detail**.

Box proxies render on both raster backends: GL (`initBoxProxy`) and Vulkan
(`initBoxProxyRaster` / `drawBoxProxies`, reusing the prototype instanced
pipeline — meshId `-1`, geometric-normal shading). This matters for the headless
path specifically: **headless forces the Vulkan backend**, so on reference_scene/benchmark scene
captures the VK box proxies are what keep the distant scene visible. The feature
is **off by default** and the LOD-off path is byte-identical (verified MAE 0 on
both backends). The cull itself is accelerated by a coarse per-prototype spatial
grid (`BuildRtLodGrid`) that frustum-rejects whole cells before the per-instance
loop.

```sh
M=<asset-root>/reference_scene/usd/reference_scene.usda
# Whole reference_scene, VK raster, robust auto-frame; collapse anything under 48 px to a
# box, drop anything under 1 px:
systemd-run --user --scope -p MemoryMax=44G -p MemorySwapMax=2G \
  ./build/lusdview --headless --next --backend vk \
    --raster-lod --raster-lod-cull-px 1 --raster-lod-full-px 48 \
    --max-tris 80000000 --frames 4 --screenshot reference_scene.ppm $M
```

Measured on the RTX 5060 Ti (whole reference_scene, 42.9 M instances, VK raster, robust
auto-frame, 4 frames so the focal length settles — see note below):

| Mode | Full-detail instances | `present` (GPU+readback) | Distant scene |
|------|----------------------|--------------------------|---------------|
| `--raster-lod` off | 23.7 M visible | ~5620 ms | full |
| size-cull only (`--raster-lod-cull-px 4`, no proxy) | 69 k | ~555 ms | **dropped** |
| box proxies (`--raster-lod-cull-px 1 --raster-lod-full-px 48`) | 15 + box soup | ~480–610 ms | **kept as boxes** |

So size-culling alone cuts visible instances ~340× (23.7 M → 69 k) and the GPU
present ~10× (5.6 s → 0.56 s); switching the drop to a box proxy keeps the same
~10× frame-time win **without** losing the distant reference_scene — the far vegetation
clumps become cheap gray boxes (~12 tris each) instead of vanishing. Unlike
§2.6.4's merge this re-selects every time the camera settles, so pulling the
camera in promotes near boxes back to full geometry.

> **Note on focal length.** The viewport height / aspect are not final until
> after the first `resizeViewport`, so frame 1's projected-radius math uses a
> stale focal length and the cull re-runs on frame 2. Use `--frames ≥ 4` for
> headless LOD captures so the final frame reflects the settled camera.

This is a per-frame raster analogue of the RT path's view-dependent LOD
(`rt_lod.{hh,cc}`, §2.6.3-adjacent), sharing the same `ProjectedRadiusPx` /
`BoxFitXform` math (`lod_math.hh`). **Limitations:** hard switch at `full-px`
(no stochastic crossfade band — the raster path has no accumulator to resolve a
dither), box proxies are flat-shaded (no materials/textures), and `full-px` is a
single global threshold rather than a per-prototype importance score.

### 2.6.6 Realtime large-scene profiles (`--large-scene-profile`, lusdview)

`lusdview --large-scene-profile off|auto|benchmark|reference_scene|alab` is a preset layer
over the existing Vulkan large-scene flags. It is meant for repeatable benchmark scene,
reference scene, and ALab smoke/performance runs without memorizing the full flag list:
profiles enable `--next`, prefer the Vulkan backend, enable raster/RT LOD, set
conservative draw/VRAM budgets, and print the resolved settings at startup.
`auto` only applies a profile when the input path clearly names benchmark scene, reference scene
or Moana, or ALab; otherwise it resolves to `off`.

Explicit CLI flags win. For example, `--large-scene-profile instance-heavy
--max-gpu-mem 6 --raster-lod-full-px 32` keeps the reference scene preset but uses the
user's GPU budget and LOD threshold. Profiles do **not** enable texture resize or
BCn/compressed texture paths; those remain controlled only by
`--texture-max-size`, `--texture-budget-mb`, and `--texture-compress`.

Scene-specific defaults:

- `benchmark`: raises the triangle cap to avoid the old 30 M-triangle truncation,
  uses the named overview camera when no camera is passed, and enables district
  LOD streaming with host/VRAM budgets.
- `reference_scene`: caps full raster uploads by draw count and VRAM, then relies on
  per-frame raster LOD to keep the distant reference_scene visible as box proxies.
- `alab`: enables parent-relative composition paths because the public ALab
  layout uses `../lightingrenderovers/`, then applies the same Vulkan/LOD budget
  preset pattern.

Optional local harness:

```sh
LUSDVIEW=./build_ninja/lusdview \
BENCHMARK=/path/to/benchmark.usda \
REFERENCE_SCENE=/path/to/reference_scene.usda \
ALAB=/path/to/alab.usda \
  bash examples/lusdview/tests/run-large-scene-profiles.sh

LUSDRENDER=./build_ninja/tools/lusdrender/lusdrender \
BENCHMARK=/path/to/benchmark.usda \
REFERENCE_SCENE=/path/to/reference_scene.usda \
ALAB=/path/to/alab.usda \
  bash tools/lusdrender/tests/run-large-scene-profiles.sh
```

The harnesses skip scenes whose environment variables are unset, write one image
and one log per scene, and check that the resolved profile settings appeared in
the log. Each writes `summary.tsv` under `OUT_DIR` with profile, asset, exit
code, elapsed seconds, image bytes, and log path. Use `LUSDVIEW_EXTRA_ARGS` or
`LUSDRENDER_EXTRA_ARGS` to add per-run flags such as `--rt`, `--mode albedo`,
`-stats`, or `-gpuShade preview`. `LUSDVIEW_SCENE_TIMEOUT` defaults to `10m` and
bounds each viewer scene run. They are intentionally not required ctests because
these large assets are not tracked in the repository.

### 2.6.7 Ray-traced full reference_scene (lusdview `--hip`, AMD RX 9070 XT)

§2.6.4 is a *rasterizer* preview: it merges the long tail into a flat box-proxy so
the per-mesh draw path stays cheap. The **HIP ray tracer** (`--hip`) takes the
opposite approach — it ignores the raster draw/buffer budget entirely and traces
the **full** geometry: every prototype's real triangles, instanced by the real
per-instance transforms, in one 2-level BVH (per-prototype BLAS + a TLAS over all
instances). On a 16 GiB GPU the **whole Moana reference_scene fits** — all 42.9 M instances
over 30.0 M unique triangles (≈17.5 B effective), no LOD, no proxy.

```sh
M=<asset-root>/reference_scene/usd/reference_scene.usda
# Full reference_scene, HIP ray tracing, framed on the hero shot camera.
# --max-instances 0 = unlimited (the 16 M default truncates by mesh order).
./build/lusdview --headless --next --hip \
  --camera shotCam --max-instances 0 --rt-samples 2 \
  --frames 4 --screenshot reference_scene.ppm $M
```

```
next: 83798 draws, 42869753 instances, 30009608 unique tris (17508653596 effective), instXform VRAM ~2.06 GB, up=Y
[rt_scene_build] 83798 meshes -> 30009608 tris / 42869754 instances: phaseA(geom) 28518 ms, phaseB(assemble+TLAS) 56137 ms
HIP: traced 42869754 instances, 30009608 unique tris, 91138 colors -> reference_scene.ppm
```

Measured on the RX 9070 XT (gfx1201, 16 GiB): **2 m 03 s** wall-clock end-to-end,
**~28.7 GiB host RSS** during the CPU scene build, and the device acceleration
structure fits under 16 GiB VRAM (no OOM). The result is the iconic Motunui
shoreline — mossy coral-rock boulders in the foreground, sandy beach, the line of
bare ironwood trees.

Notes and gotchas:

- **`--next` is mandatory.** The standard Tydra loader rejects the reference_scene's
  `..`-relative payloads (same "Unsafe asset path" as benchmark scene, §2.6.1); `--next`
  composes them. `--hip` then traces whatever geometry the loader produced — it
  does **not** apply the §2.6.4 raster box-proxy LOD.
- **`--max-instances 0`.** The ray tracer defaults to a 16 M instance cap; the
  reference_scene has 42.9 M, so the default **truncates by mesh build order** (not frustum
  visibility) and drops most of the scene. Pass `0` for the full reference_scene.
- **Named cameras.** `reference_scene.usda` authors `shotCam`, `birdseyeCam`, `beachCam`,
  `dunesACam`, `grassCam`, `palmsCam`, `rootsCam`; select one with `--camera`
  (a name that doesn't match silently falls back to whole-scene auto-fit, which
  for the reference_scene is a speck — see §2.6.4's robust framing). `--rt-samples N` adds
  Halton(2,3) sub-pixel AA.
- **What makes it feasible.** Two earlier fixes: (1) the CPU scene build is
  parallelized (`rt_scene_build.cc` — per-mesh geometry, per-instance transforms,
  and TLAS all run multithreaded; byte-identical to the serial path); and (2) for
  a headless `--hip` screenshot lusdview **skips the rasterizer's `uploadScene` /
  per-frame `renderViewportScene`** (`rtOwnsScreenshot_`), which would otherwise
  spend minutes culling 42.9 M instances for a frame that is immediately
  discarded. Set `LUSDVIEW_RT_TIMING=1` to print the `[rt_scene_build]` phase
  breakdown and the retained host residency split (geometry, BVH/instances,
  analytic points, materials, textures, and volumes).

For CUDA/NVIDIA systems with a tighter host budget, use a bounded near-full
profile instead of allowing legacy point/curve triangle proxies to expand
without a limit:

```sh
./build/lusdview --headless --next --cuda --load-payloads \
  --large-scene-profile instance-heavy --max-tris 20000000 \
  --max-instances 2000000 --vram-budget 12 --rt-samples 1 \
  --frames 1 --screenshot reference_scene-cuda.png $M
```

When `--max-tris` is between 1 and 64 million, RT converts legacy point and
curve carriers into a bounded analytic splat stream (`max-tris / 8` samples)
instead of materializing triangle tubes. Meshes retain priority. The 20M
reference scene profile measured about 26.2M KiB peak RSS and 4.6 GiB CUDA VRAM on an
RTX 5060 Ti; curve and unoriented-point quality is intentionally reduced.

### 2.7 RenderScene optimization for realtime viewers

Payload deferral solves structural loading, but realtime web/native viewers have
a second pressure point after composition: authored scenes can expand to thousands
of duplicate materials, many equivalent texture records, thousands of draw-call
mesh nodes, and tens of thousands of transform-only prim nodes. LightUSD now has
opt-in `RenderSceneConverterConfig` switches for this viewer-oriented path:

| Option | Default | Effect |
|---|---:|---|
| `dedup_materials_by_texture_identity` | `false` | Deduplicate equivalent render materials by full shader parameters and canonical texture identity. Also enables a source-graph signature cache so duplicate source materials can reuse an existing render material ID before full conversion. |
| `merge_meshes` | `false` | Merge compatible meshes sharing a material to reduce draw calls. |
| `merge_meshes_bake_transform` | `true` | Bake mesh node world transforms into vertex data while merging so meshes under different transforms can still aggregate. |
| `flatten_optimized_render_tree` | `false` | Replace the authored prim hierarchy with a compact render-only root containing renderable meshes/lights/cameras in world space. This intentionally breaks prim-tree fidelity. |

All destructive optimizations are disabled by default. Existing conversion paths
therefore preserve the authored prim tree and one-render-material-per-converted
source material unless an app explicitly opts into the viewer path.

The material optimization has two stages:

1. A conservative source-graph signature cache compares authored Material /
   Shader / NodeGraph payloads with local material paths normalized. A cache hit
   reuses the canonical render material ID and skips full material conversion.
2. A post-conversion material/texture compaction pass remaps mesh material IDs,
   material subset IDs, instance material IDs, and shader texture IDs to
   canonical render objects. Texture identity uses resolved image ID, wrapping,
   UDIM linkage/remap, scale/bias, output channel, UV primvar, and transform.

Mesh merging is applied after material/texture compaction. It only merges meshes
that are safe for render aggregation: no skeletal skinning, no blend-shape
targets, no per-face material subset map, and compatible vertex attribute
layouts. When transform baking is enabled, packed direction attributes that
cannot be transformed as float3 are dropped from the merged mesh so consumers can
recompute them rather than reading corrupted normals/tangents.

Flattened render tree mode is separate from mesh merging. It keeps renderable
nodes only and assigns each kept node its previous world matrix as its new local
matrix under a synthetic root. This substantially reduces JS/renderer object
construction overhead in viewer-only workflows, but it should not be used by
tools that need authored prim paths, hierarchy editing, or exact selection
round-tripping.

The web MaterialX demo exposes the same switches as URL/query/UI settings:

```
nativeMaterialDedup=true
nativeMeshMerge=true
nativeMeshMergeBakeTransform=true
nativeFlattenRenderTree=true
meshAggregation=off|material
```

On a representative flattened large scene with resized PNG textures, the fully
native viewer path reduced render mesh nodes from a few thousand to a few
hundred, compacted the JS node tree from tens of thousands of nodes to hundreds,
kept the final material count stable, and reduced Three.js construction from
roughly a second to a few hundred milliseconds. Source-material signature caching
also reduced native material conversion from roughly second-scale to
double-digit milliseconds in that workload. Treat these as shape-of-improvement
numbers rather than a portable benchmark; exact results depend on material
duplication, mesh compatibility, and hierarchy depth.

---

### 2.8 GPU/VRAM round (2026-07) — historical

The July 2026 lusdview/lusdrender VRAM work (validated 6 GiB / 15 GiB fit
configs, host-memory weld/texture matrix, raster LOD for non-instanced
geometry, `-vk` compute tiling, TLAS `PREFER_FAST_BUILD`) is superseded by the
later sections; the tables and recipes are in git history
(`git log -- doc/large-scene.md`). Durable outcomes, so they are not re-derived:

- LightRT VK BLAS compaction stays ON by default (`LRT_VK_BLAS_COMPACT=0` opts
  out): benchmark `-vkInstanced` 1051→525 MiB; reference_scene full instance set
  7153→5408 MiB and 49.6→31.7 s.
- Device-local MDI instance mirror is auto-enabled at ≥20 % VRAM headroom
  (`LUSDVIEW_MDI_DEVLOCAL=0/1` forces it); it costs ~1.37 GB on reference_scene and does
  nothing under `--raster-lod`, whose frame is primitive-bound.
- TLAS `PREFER_FAST_BUILD` was measured and rejected, and the `-vk` tiled
  ray/hit dispatch did not pay off (implemented, off by default).
- The `--raster-lod` knobs remain the budget lever for tight-VRAM raster runs;
  reference_scene RT cost is dominated by the grid + LOD-bound BLAS set, not the rt-lod
  pixel thresholds.

### 2.6.8 Next-core load/compose profile on a 32-core, 125 GiB host (2026-10)

Measured on the workstation that hosts the benchmark scene checkout
(`/path/to/benchmark`, 10.5 GB, 63,684 files; 32 cores, 125 GiB RAM,
NVMe). Tooling: the next-core CLI (`build-next/lusdcat`, formerly `next_usdcat`),
`--compose-threads-auto`, `LIGHTUSD_NEXT_TIMING=1`.

| Case | threads | load+compose | write | total | peak RSS | output |
|---|---:|---:|---:|---:|---:|---|
| proxy LOD, geometry deferred (`--defer-payloads`) | 1 | 10.6–11.1 s | 7.3–7.9 s | 18.0–18.7 s | 1.07 GiB | 190 MB USDA |
| proxy LOD, geometry deferred | auto | **2.22–2.36 s** | **0.75–0.80 s** | **2.97–3.15 s** | 1.35 GiB | 190 MB USDA (byte-identical) |
| proxy LOD, payloads loaded (`--load-payloads`) | 1 | 13.9–14.6 s | 9.1–9.4 s | 23.0–24.0 s | 1.41 GiB | 286 MB USDC |
| proxy LOD, payloads loaded | auto | **3.47–3.56 s** | 9.2–9.3 s | **12.8 s** | 1.90 GiB | 286 MB USDC (byte-identical) |
| one district at full LOD (`map_phosphate_mine = full`) | 1 | 126–133 s | 62–63 s | 188–196 s | 6.8 GiB | 580 MB USDC (4,458,013 paths/specs) |
| one district at full LOD | auto | **69–72 s** | 59–76 s | **130–148 s** | **20.9–25.2 GiB** | 580 MB USDC (byte-identical) |
| whole reference_scene at full LOD (all 45 districts `full`) | 1 | **79.1 min** (sources 20.5 min, structure 24.6 min, compose 34.8 min, flatten 8.8 min, finalize 7.1 min) | 35.3 min | **114.4 min (1.9 h)** | ~59 GiB | **3.01 GB USDC** (61,833,354 paths/specs) |
| whole reference_scene at full LOD | auto | *not re-measured* | — | — | — | — |

**The threaded next core is the default for native builds** (`LIGHTUSD_NEXT_ENABLE_THREAD`
now defaults ON outside Emscripten; WASM stays OFF). Every figure above is
byte-verified: the threaded and single-threaded outputs hash identically
(SHA-256) for the 190 MB USDA, the 286 MB proxy USDC and the 580 MB district
USDC, with `--compose-threads 8` and `--compose-threads 16` matching as well.

**The parallel path is the difference between seconds and minutes.** With
threads on, the source warm covers the whole namespace up front
(`warm=0.6–20 s`, `source_hits` = every node, `source_misses` = 0) and the
serial structure walk then consumes cache hits. On the proxy scene the compose
is **4–5× faster** and the USDA write **9× faster**; on the full-LOD district
the compose is **~1.8× faster**. The crate (USDC) write does not speed up
much (9.1 s → 9.3 s on the proxy) because **its serial `BuildFieldsAndSpecs`
pass dominates**, not the token/string interning pre-pass: on the proxy the
pre-pass (`tables`) takes only 0.22–0.34 s of a 9.2–9.8 s write, while
`fields_specs` takes 7.3–10.5 s (`PATHS` 0.8–1.0 s, `structural_payloads`
0.3–0.4 s). `BuildFieldsAndSpecs` walks the composed specs serially
(~15 µs/spec at the proxy's 499,915 specs, ~9 µs/spec for the district's
4.46 M), so the crate write is the next target; the parallel ASCII (USDA)
writer already hides its build behind workers.

**The full-reference_scene case fits in RAM — and now completes.** With the whole
reference_scene promoted to `districtLod = full` (all 45 districts) the next-core loader
composes **10,224,245 prims** (10,217,261 source misses) and writes a
**3.01 GB** USDC in **114 min** on this 32-core / 125 GiB host, peaking around
**59 GiB RSS** (measured by sampling `/proc/<pid>/status`). The memory budget
therefore fits with room to spare; the wall is compose time, not RAM.

That is the *whole reference_scene* in one process. The per-district full LOD
(§2.6.2) remains the production-correct approach — one district at full LOD is
~3.5 min and ~8 GiB — but the full-reference_scene load is now demonstrated to
complete rather than exhausting the host.

**Memory is the price.** The threaded path keeps per-worker resolver snapshots
and the merged sources cache alive, so the full-LOD district peaks at
**20.9–25.2 GiB** versus **6.8 GiB** single-threaded, and the proxy rises from
1.07 to 1.35 GiB. `--compose-threads 8`/`16` do not reduce the peak (the merge,
not the worker count, dominates). **Re-measure the whole-reference_scene full-LOD case
before using the threaded build for it** — single-threaded it peaks at ~59 GiB,
and the threaded warm phase is expected to add a large multiple of that.

**Historical Debug measurements** (instrumented on the full-LOD district
case; these are not Release optimization baselines):

| Phase | Time | Note |
|---|---:|---|
| `sources` (per-prim source resolution) | 74–92 s | 733,439 source misses for 735,484 nodes |
| `structure` (tree/instance pass) | 89–110 s | includes `sources` |
| `compose` (opinion composition) | 40–57 s | |
| crate write `fields_specs` | 41–42 s | 4.46 M specs |

For the whole reference_scene the same phases scale to `sources` 20.5 min, `structure`
24.6 min, `compose` 34.8 min, and a crate write of 35.3 min (of which
`fields_specs` alone is 27 min for 61.8 M specs), so both the compose and the
writer are linear in the composed prim count.

The later Release handoff measured district `fields_specs` at **10.4–10.6 s**
and total crate write at **17.7–18.2 s**, rather than the 41–42 s field pass
above. Compare binaries from the same build configuration and paired runs;
allocator, thread policy, and teardown materially affect these measurements.

**What the per-prim cost actually is** (correcting an earlier reading of these
numbers): `ExpandList` calls `ExpandArcs` 7,057,537 times for 735,484 nodes
(≈9.6 per node). Those are **not** redundant re-expansions of the same arcs —
`DeriveChildSources` re-spins each *parent* source onto the child's own site,
so the 9.6 expansions are that prim's own contributing sources (one per
contributing layer), each looked up at the prim's own path. **85.6% of them
find no spec at that site** (the prim does not exist in that layer) and return
after a single lookup; those negative lookups cost 10.7–14.1 s of the ~55 s
spent in `ExpandArcs`.

One follow-up remains after the filter below landed; the other is still open:

1. *Cheap negative lookups (safe) — landed 2026-10-22.* Answer "does any layer
   of this stack have a spec at this site?" with one probe against a per-stack
   Bloom filter over the union of authored prim-path hashes, instead of one
   `unordered_map<string,uint32_t>` probe per layer. The filter is exact (a
   hash collision only costs the old per-layer probes), so which specs are
   found does not change. See the A/B below.
2. *Arc-free ancestor reuse (semantics-sensitive).* Reusing a parent's
   already-expanded result for a child that authors no arcs of its own would
   remove the O(n·depth) expansion term entirely, but it must preserve the
   exact strength ordering and the implied-class re-rooting rules, so it needs
   the pxr parity gate before it can land.

**Landed: per-stack negative-lookup filter (2026-10-22).** `Cache::Impl::Specs()`
now keeps one `PathHashFilter` per layer stack: a 2-probe Bloom filter over
every prim path authored by any layer in the stack. It is built lazily after a
stack has seen enough negative cache misses to amortize the union scan
(`max(24, paths / (8 × layers))` misses), capped at 2 MiB per stack and 32 MiB
in total, and is cleared wherever `spec_cache_by_stack_` is cleared
(`InvalidateLayer`, layer reload, transient-cache trim). Below three layers a
filter only builds if the stack is queried heavily enough to amortize it, so
small reference stacks do not pay for one. A false positive falls through to
the old probes; a negative is answered with two bit tests.

Measured with the same benchmark scene fixtures (SHA-256 of every output unchanged):

| Case | metric | before | after |
|---|---|---:|---:|
| proxy LOD, payloads loaded | load+compose | 3.62 s | 3.16 s |
| | `structure` | 1.49 s | 0.90 s |
| | `compose` | 1.31 s | 0.85 s |
| one district, deferred (424,104 nodes) | load+compose | 44.7 s | 33.4–36.2 s |
| | `structure` | 14.54 s | 10.7–12.0 s |
| | `compose` | 11.73 s | 8.7–9.3 s |
| one district, full LOD (735,484 nodes) | `structure` | 29.06 s | 26.74 s |
| | `compose` | 22.10 s | 19.94 s |
| | build_stage | 55.0 s | 51.9 s |

Filters are cheap: on the deferred district 872 stacks built one, inserting
**170,522** path entries in **35 ms** total and using **305 KiB**. The root
stack alone answers ~3.3 M negative lookups on the deferred district (719 k on
the full-LOD loaded case). The phase numbers are 8–10% faster on the full-LOD
case; the 25% deferred `structure` gain is partly warm-cache variance between
runs. Wall-clock totals on the full district (163.7 s → 161.4 s) stay dominated
by the crate write.

**Landed in this pass (measured, A/B):**

| Change | Effect |
|---|---|
| Resolver captures the file device/inode during the existence probe that resolves the path (no second `stat`) | ~22% faster resolution; ~44% faster with 8 resolving threads (the registry mutex was a contention point) |
| Path normalization fast path (already-normalized paths are returned unchanged) | ~17% faster resolution; cumulative 7.1 µs → 4.45 µs per resolve |
| Registry lookups skip the mutex when a resolver has no callbacks or memory assets | removes the shared lock from the hot path |
| Asset bytes are read straight into the destination container (`AssetResolver::ReadAssetToString`) | 128 MiB read through the C ABI: peak RSS 267 MB → 136 MB (~49% lower) |
| Crate writer: dropped a write-only 4.5 M-entry `path → fieldset` map | less writer memory/time, no behavior change |
| Composition: per-ancestor source keys are built once and reused | fewer string allocations per composed prim |
| Composition: negative spec lookups are no longer cached (one shared empty result) | full-LOD district peak RSS 8.1 → 6.5 GiB; spec-lookup time 14.1 → 10.7 s for 7.4 M lookups |
| Build: next-core threads default ON for native builds (WASM stays OFF) | proxy compose 4–5× faster, district compose ~1.8× faster, byte-identical output |
| Composition: per-stack Bloom filter answers negative `Specs()` sites with one probe instead of one per layer | full-LOD district `structure`/`compose` 8–10% faster, deferred structure ~25% faster; output SHA-256 unchanged; 305 KiB of filters on 872 stacks |

Reproduce with:

```sh
M=/path/to/benchmark/benchmark.usda
# threaded (default for native builds)
LIGHTUSD_NEXT_TIMING=1 ./build-next/lusdcat -f --compose-threads-auto \
  --defer-payloads -o /tmp/benchmark.usda "$M"          # proxy structure
LIGHTUSD_NEXT_TIMING=1 ./build-next/lusdcat -f --compose-threads-auto \
  --load-payloads -o /tmp/benchmark.usdc "$M"            # proxy + payloads
# single-threaded reference (build configured with
# -DLIGHTUSD_NEXT_ENABLE_THREAD=OFF)
LIGHTUSD_NEXT_TIMING=1 ./build-next-st/lusdcat -f --compose-threads 1 \
  --load-payloads -o /tmp/benchmark-st.usdc "$M"
sha256sum /tmp/benchmark.usdc /tmp/benchmark-st.usdc      # must match
```

Full-LOD district wrapper (one district promoted to `full`):

```usda
#usda 1.0
(
    subLayers = [ @/path/to/benchmark/benchmark.usda@ ]
    upAxis = "Z"
)
over "world" { over "mp_wz_reference_scene" { over "mp_wz_reference_scene_paths" {
    over "mp_wz_reference_scene_geo" {
        over "map_phosphate_mine" ( variants = { string districtLod = "full" } ) {}
    }
} } }
```

### 2.6.9 Python binding (abi3) load/compose profile (2026-10)

This is the same benchmark scene fixture measured **through the Python API**
(`lightusd.load`), not the CLI, to check that the extension adds no second
parse/compose copy and to A/B the `158add057..3455474cf` commit range. Both
sides were built as in-place abi3 extensions from clean worktrees; each run is
a fresh process (so VmHWM is the process peak). RSS is read from
`/proc/self/status`.

| Case | build | wall (median, range) | CPU | RSS Δ | peak RSS |
|---|---|---:|---:|---:|---:|
| proxy (`load_payloads=False`) | `3455474cf` | 1.085 s (1.007–1.113) | 8.21 s | 1223 MiB | 1256 MiB |
| proxy | `158add057` | 1.036 s (1.035–1.072) | 8.74 s | 1241 MiB | 1272 MiB |
| proxy, 7 runs | `3455474cf` | 1.040 s (0.985–1.081) | 8.46 s | 1214 MiB | 1246 MiB |
| proxy, 7 runs | `158add057` | 1.050 s (0.990–1.098) | 9.37 s | 1239 MiB | 1275 MiB |
| payloads (`load_payloads=True`) | `3455474cf` | **1.668 s (1.596–1.703)** | **17.56 s** | **1876 MiB** | **1908 MiB** |
| payloads | `158add057` | 2.217 s (2.140–2.273) | 21.72 s | 2163 MiB | 2195 MiB |

Stage accounting is identical on both builds: 480.0 MiB / 13,768 prims for
proxy, and 897.7 MiB / 53,765 prims / 65.76 M point elements with payloads.
Proxy wall time is within run-to-run noise; the eager path is **25% faster,
19% less CPU and 13% lower RSS**, so the commit range is not a regression in
either metric.

**No double allocation.** Per-process `mallinfo2` after the eager load reports
`uordblks 972 MiB` plus `hblkhd` (mmap-backed) `261 MiB` = **1233 MiB live**
against 897.7 MiB of stage accounting — container/metadata overhead, not a
second geometry copy. `hblkhd` is not part of `uordblks` (a 64 MB NumPy
allocation moves them by +3.0 MiB and +64.4 MiB respectively). On the proxy
case the live heap is 678 + 77 = 755 MiB. After `Stage.close()` the live heap
falls to **3 MiB**, so composition memory is not leaked.

The rest of the RSS is allocator retention. glibc keeps freed composition
arenas: `malloc_trim(0)` reclaims 437 MiB on the proxy load (1242 → 806 MiB
RSS) and ~586 MiB with payloads (1940 → 1299 MiB). That memory is free, not
live. Peak-allocation profiling (heaptrack) attributes the largest sites to
allocator internals (646 MB), `TimeSampleBlock::find_or_store` (196 MB),
`lu_value_set_lazy` (125 MB) and `TimeSampleBlock::add` (123 MB); it shows no
second copy of a geometry array.

Zero-copy holds through the binding: `np.asarray(attribute)` aliases the
native buffer (two views share the same data pointer), and a 3.4 MiB array
adds 4.3 MiB RSS at 0.01 ms. `Stage.flattened()` is the one deliberate
second allocation — it returns a second stage (+338 MiB, 1.4 s on the proxy
scene); use `flatten_file()` to stream instead. Instance modes do not duplicate
geometry either: the CLI eager path measures 1808 MB native, 1816 MB holder and
1859 MB prototypes.

**Binding vs CLI.** With the same untrusted policy (`--aousd-strict`, which is
what the Python C API uses) the CLI eager path is 1,839 MB / 1.48 s; the Python
eager path is 1,900 MB / 1.67 s. Proxy is 1,235 MB / 1.02 s (CLI) vs 1,240 MB / 1.09 s (Python). The
remaining gap (≤100 MB, ≤0.2 s) is the interpreter and NumPy, not duplicate
scene data.

**Allocator matters more than the binding.** The native CLI links jemalloc
(`LIGHTUSD_NEXT_TOOLS_ALLOCATOR` auto-detect); the abi3 extension uses the
platform allocator (glibc here). On the eager path:

| Allocator | wall | CPU | peak RSS | RSS after close |
|---|---:|---:|---:|---:|
| jemalloc (`LD_PRELOAD`) | 1.137 s | 10.91 s | 1937 MiB | 1457 MiB |
| glibc | 1.630 s | 17.28 s | 1920 MiB | 1630 MiB |

Peak is the same — the difference is time and retained free arena. For
large Python loads, preload jemalloc:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 python app.py
```

Do not link an allocator into the abi3 wheel: the allocator is a process-wide
choice and belongs to the application embedding the extension.

Reproduce (one process per sample):

```sh
python3 setup.py build_ext --inplace
for p in 0 1; do
  PYTHONPATH=python python3 - "$p" <<'EOF'
import sys, time, lightusd
payloads = sys.argv[1] == "1"
t = time.perf_counter()
st = lightusd.load("/path/to/benchmark/benchmark.usda",
                   composed=True, load_payloads=payloads)
dt = time.perf_counter() - t
rss = next(int(l.split()[1]) for l in open("/proc/self/status")
           if l.startswith("VmRSS:")) / 1024
print({"payloads": payloads, "seconds": round(dt, 3),
       "rss_mib": round(rss, 1),
       "stats_mib": round(st.stats["memory_bytes"] / 1048576, 1),
       "prims": st.stats["prim_count"]})
st.close()
EOF
done
```

## 3. Roadmap (remaining implementation)

The loader bounds memory and streams payloads. Cross-directory cwp anchoring
(§3.1) and referenced-prim descendant reconstruction (§3.2) are now **fixed** —
benchmark scene composes its full proxy-mode structure (32,811 prims, geometry deferred)
in ~1.7 GiB. The items below are remaining enhancements.

### 3.1 Cross-directory cwp anchoring — FIXED

**Was:** a loaded layer's PrimSpecs got their working-path stamped from
`GetBaseDir(authored_asset_path)` — a *relative* path — instead of the *resolved
absolute* base directory, so nested references lost their parent-directory
context (e.g. `prefabs/...` inside `map_source/mp_wz_reference_scene_geo.usd` anchored to
the scene root instead of `map_source/`). A second case: when sublayer
flattening merged a stronger `over` (carrying the root layer's cwp) onto a weaker
`def` that authored the reference (e.g. benchmark scene's root `over mp_wz_reference_scene_paths`
over `mp_wz_reference_scene.usd`'s `def`), the merged prim kept the root cwp and
mis-anchored the arc.

**Fix (implemented):**
- Pass the **resolved** path (not the authored relative path) to
  `LoadLayerFromMemory` at every composition load site, so a loaded layer's
  prims are stamped with their resolved directory: `LoadAsset`
  (`src/composition.cc`), `DefaultLoadAndOwnLayer` (`src/composition-graph.cc`),
  and `pcp::LayerRegistry::GetOrLoad` (`src/pcp/layer-registry.cc`).
- In sublayer merge (`CombinePrimSpecRec`, `src/composition.cc`), when a stronger
  `over` absorbs a weaker `def`/`class`, adopt the **defining** layer's
  asset-resolution anchor (cwp + search paths) so reference/payload arcs authored
  in the defining layer resolve against the correct directory.

Regression test: `tests/feat/large-scene/` (`feat-large-scene`) uses a
filename-collision fixture (a malformed decoy `leaf.usda` in the root dir vs. the
real one in `sub/`) so the parse-once registry's parse count only goes non-zero
when the reference anchors to the correct directory — it fails if either fix is
reverted. Full unit suite (853 cases) unaffected.

### 3.2 Referenced-prim descendant reconstruction — FIXED

**Was:** `CompositionGraph` built a `PrimIndex` per prim by walking only the
**root-layer** namespace (`BuildPrimIndex` DFS over the root-layer PrimSpec's
children) and `ScanArcsAndEnqueueTasks` scanned only a prim's *own* arcs. Child
prims introduced *by a reference* (e.g. `paths.usd`'s `mp_wz_reference_scene_geo`, which
itself references `geo.usd`) were never added to the namespace, so their arcs
were never followed and they never reached the Stage. benchmark scene's chain
(`mp_wz_reference_scene` → `paths` → `geo` → prefabs → proxy payloads) stopped a few
levels in (≈6 files parsed, 0 deferred payloads).

**Fix (implemented, `src/composition-graph.cc`):** `BuildPrimIndex` now expands
the **composed** namespace. After building a prim's index it gathers the union
of child names across *all* of the index's nodes (`GatherComposedChildNames`),
and builds each child's index by **descending every node of the parent index one
namespace level** (`PrimIndexBuilder::BuildChildFrom` / `SeedDescendedNodes`):
each descended node keeps its parent's arc type / layer stack / strength so the
child inherits the parent's reference & payload arcs, and the descended prims'
own arcs are then scanned and followed (which pulls nested references such as
`geo.usd`). This reconstructs referenced descendants *and* follows their nested
arcs, recursively.

**Result:** benchmark scene (proxy, LoadNone) composes **32,811 prims / 122 files / 373
deferred payloads** in **~1.7 GiB / ~3 s** — the full structural composition,
geometry deferred. Regression test `feat-large-scene` asserts a referenced
prim's grandchild (`/P/M`) is reconstructed. Full unit suite (853) unaffected.

`pcp::Cache::BuildStage` now mirrors this namespace expansion for stage
reconstruction: it starts from cached root PrimIndices, descends each composed
parent index with `PrimIndexBuilder::BuildChildFrom`, and composes temporary
child indices for referenced descendants. Lazy `ComputePrimIndex()` remains
local to explicitly requested prim paths, but full-stage reconstruction now
matches the eager engine for referenced grandchildren. Regression:
`pcp_buildstage_reference_grandchildren_test`. Deferred PCP payload streaming now
also reprocesses the newly loaded payload node, so references/payloads authored
inside a streamed payload layer are visible immediately after `LoadPayload`.
Regression: `pcp_external_payload_load_reprocesses_nested_arcs_test`.

### 3.3 mmap-through-composition / fd bounds

mmap zero-copy (`USDLoadOptions::mmap_zero_copy`) defers large uncompressed
float arrays to disk on the legacy direct-Stage reader. Full composed-layer mmap
is now supported through the **next PCP backend**
(`LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON`): every file-backed USDC layer is read
with lazy arrays and mmap enabled, each `LazyArrayRef` retains shared ownership
of its own `CrateDataSource`, and that reference survives Layer → composed
PrimSpec → rebuilt Stage copies without materialization. This naturally handles
many sublayers/references/payload files rather than relying on a single global
mmap table. `LargeSceneLoadOptions::mmap_zero_copy` controls both lazy arrays and
mmap for the whole next-PCP load (default ON); disabling it produces the same
composed result with eagerly decoded, owned array values.

The legacy CompositionGraph backend deliberately stays in copy mode for
composed layers: its `Stage` mmap table addresses one source buffer and cannot
identify per-layer sources. Extending that representation would duplicate the
shared-source ownership already provided by next PCP. Direct legacy
`LoadUSDCFromFile(..., mmap_zero_copy=true)` remains supported for one-file
Stages.

The descriptor-bound part is now implemented: `AssetResolutionResolver` tracks a
per-resolver concurrent open file-descriptor / asset-handle budget
(`max_file_descriptors`, default **1024**). `LargeSceneLoadOptions` forwards this
limit into sublayer/reference/payload loading. `open_asset()` reports a hard
error when the budget is reached instead of falling through to an OS-level
`EMFILE` failure, and the composition graph treats that specific resolver error
as fatal even when ordinary missing references/payloads are skippable. The
current filesystem and mmap paths close descriptors before returning from each
open, so this is a guard on concurrent opens, not an LRU for long-lived mappings.

### 3.4 Asset-centric composition (ALab) — FIXED

ALab loads assets via subLayer-aggregated entity files and layers shots from
many department sublayers. Three fixes make it compose:
- **Compose a referenced/payload layer's own subLayers**
  (`src/pcp/layer-registry.cc`, `LayerRegistry::GetOrLoad`): when a
  reference/payload target aggregates its prims through its own `subLayers` (the
  ALab entity pattern), compose them after load — otherwise the referenced
  content is empty. Mirrors the LIVRPS `LoadAsset`.
- **`references`/`payload` list-op accumulation across sublayers**
  (`src/composition.cc`, `CombinePrimSpecRec`): these are list-ops, so opinions
  from each layer in a sublayer stack accumulate (USD prepend/append) rather than
  the stronger whole-value winning. A shot whose department layers each
  `prepend references` to `/root` now keeps them all. The newly-introduced arcs
  are anchored to the layer that authored them (generalizing the over+def
  cwp rule to def+def).
- **Child-bearing reference targets expand** (`src/composition-graph.cc`,
  `GatherComposedChildNames`): a reference target that authors no props/metas of
  its own but has children (a parent of geometry) is no longer skipped during
  namespace expansion.

Regression: `feat-large-scene` scenario 2 (`fixture/multi/`) covers all three —
two sublayers each `prepend references` to `/P`, one asset aggregating geometry
through its own subLayers; both `/P/MA` and `/P/MB` must reconstruct.

### 3.5 Variant-content composition — FIXED

CompositionGraph recorded variant *selections* but `ResolveAndApplyVariants` was
a no-op, so the selected variant's *content* (child prims + their nested
references/payloads/clips) was never composed — variant-gated geometry (e.g.
baked_procedurals' `GEO`, or ALab characters behind their `geo`/`alfro`
variants) was dropped. Two fixes:
- **Merge variantSet content across composition** (`CombinePrimSpecRec`,
  `src/composition.cc`): the `variantSet "x" = { … }` blocks live in
  `variantSets()` and were not merged when a prim received opinions from
  multiple layers, so a prim selecting a variant whose set is defined in a
  weaker sublayer lost the content. Now the per-variant content is merged.
- **Compose variant content in the DAG** (`PrimIndexBuilder::ApplyVariantContent`,
  `src/composition-graph.cc`): after a prim's arcs are evaluated, compose the
  variant selection (strongest per set across nodes) and, for every node that
  authors a matching variant set, resolve the selected variant
  (`VariantSelectPrimSpec`, injecting the composed selection) and repoint the
  node at the resolved PrimSpec — **materialized in a synthetic layer** owned by
  the composition context (since `find_primspec_at` has no variant-path
  support). The variant's child prims then enter the namespace and the normal
  expansion follows their arcs. Because the selection and the variant set may be
  authored on different nodes, this handles **both** variant sets defined in the
  root layer stack **and** variant sets introduced via a reference (asset
  defines the set, a stronger layer selects it — the ALab character pattern).
- **Re-scan selected variant self-arcs** (`PrimIndexBuilder::FinishBuild` /
  `ApplyVariantContent`, `src/composition-graph.cc`): after a node is repointed
  at its resolved variant PrimSpec, the DAG re-scans that PrimSpec and drains the
  task queue again. This covers variant options that author composition arcs on
  the selected prim itself (e.g. a `prepend references` or `prepend payload` in
  the variant option metadata), not only arcs on child prims.

Regression: `feat-large-scene` scenarios 3 (`fixture/variant/`, set in a weaker
sublayer) and 4 (`fixture/variant-ref/`, set in a referenced asset) — both
require `/P/GEO` and `/P/GEO/M` to reconstruct. Scenario 5
(`fixture/variant-self-arc/`) requires a selected variant's self-reference to
compose `/P/M`, a selected variant's self-payload to compose `/Q/PM` in
`LoadAll`, that same payload to be recorded as deferred on `/Q` in `LoadNone`,
and `load_payload("/Q")` / `unload_payload("/Q")` to add/remove `/Q/PM` after
`rebuild_stage`. Scenario 6 (`fixture/deferred-nested/`) streams a deferred
payload from a subdirectory; that payload references `./leaf.usda`, and
`load_payload("/P")` must compose `/P/Nested/M`, proving lazy payload parsing
uses the resolved payload path for cwd stamping and re-scans arcs authored on
the newly loaded payload node. On the ALab shot this composes the character
geometry (3,293 → 4,560 prims, 1,271 → 2,540 deferred payloads); benchmark scene is
unchanged (32,811). Value clips themselves (`clips` metadata) are resolved at
Tydra / attribute-evaluation time (`enable_value_clips`), not during
composition.

### 3.6 Smaller items

- **Budget-by-extent — implemented**: `payload_policy_with_prim` exposes the
  authoring `PrimSpec`, and `LargeSceneLoadOptions::payload_extent_budget` adds
  an optional projected-area cap from authored `extentsHint` while preserving
  the byte budget and byte-only fallback for unannotated payloads.
- **Layer-owned mmap — implemented in next PCP**: per-array shared
  `CrateDataSource` ownership survives composition and Stage publication. The
  legacy single-source mmap table remains direct-load-only by design. If future
  code bounds long-lived mappings by open handles rather than closing the file
  after `mmap`, add an LRU on top of the existing descriptor budget.
- **Threaded streaming — implemented**: graph/cache mutation is serialized and
  rebuilt Stages are published as immutable shared snapshots. A worker may call
  `load_payload`/`unload_payload` while readers retain `stage_snapshot()`.

---

## 4. UnrealEngine USD Stage exports

A fourth scene class beyond the three above: UE's "Export Level → USD" writes a
binary-crate root with thousands of per-instance `prepend references` into
per-asset `SM_*.usd` files (geometry inline under a `variantSet "LOD"`, one
selected LOD) plus `MI_*.usd` UsdPreviewSurface materials and 2k-textures.
Multi-GB scenes (1–3 GB on disk, 250–750 textures) compose to 4k–13k meshes.
Two UE-specific path pathologies, and the fixes (commit `3f314f0be`):

- **Escaping parent-relative arcs**: references are authored against the export
  machine's directory layout (e.g. `@../../../../../USD_Exports/<Scene>/Assets/
  mesh.usd@`), which resolves outside the scene root and silently composed
  nothing. The resolver now applies a **suffix fallback** on a literal-resolution
  miss: strip the un-anchorable prefix (leading `../` runs, Windows drive,
  `/`), then retry progressively shorter path suffixes — longest first, down to
  the basename — against the search paths (`Assets/mesh.usd` matches under the
  scene root). Default on; `lusdcat --no-asset-path-fallback` opts out. Each
  rebase is surfaced as a deduped warning, and total misses warn once per path.
- **Drive-prefixed paths** (`@F:/USD_Exports/...@`): with
  `allow_parent_relative_paths`, the security validator now demotes the drive
  prefix to a relative path instead of rejecting, and the same suffix fallback
  rebases it.

The fallback lives in `AssetResolutionResolver::resolve()` so every consumer —
lusdcat/lusdzconvert flatten, `CompositionGraph`, and the wasm in-memory
resolver (browser folder upload) — inherits it with no duplicated logic.

## 5. USDZ conversion at scale (texture pipeline + wasm heap)

Findings from converting multi-GB scene folders to USDZ (native `lusdzconvert`,
the Node/wasm CLI, and headless-Chrome in-tab):

### 5.1 Texture re-encode parallelization (commit `627d0472b`)

PNG re-encode of hundreds of 2k textures was the wall-clock bottleneck
(sequential single-thread: 15–44 min). Now:

- **Native**: texture packing runs in three phases — sequential dedupe/UDIM/
  asset reads → `ProcessTexture` on a `std::thread` pool → sequential archive
  naming/stats — byte-identical output to the sequential order.
  `UsdzConvertOptions::num_threads` (0 = all cores), `lusdzconvert -numThreads`.
  Pair with `-DLIGHTUSD_WITH_FPNGE=ON` to replace the scalar-fpng encoder
  (`FPNG_NO_SSE=1`). A 731-texture 2.5 GB scene: **33 s** vs 43 m 53 s via the
  sequential wasm CLI.
- **Node CLI**: `--texture-codec js` runs PNG decode / gamma-aware box resize /
  encode on a `worker_threads` pool (pngjs + node's native zlib); non-PNG falls
  back to the wasm path. 14–25× on PNG-heavy scenes, ~25–35 % larger output
  (zlib-6 vs fpng).
- **Browser**: the same `textureProcessor` hook (now driven with bounded
  concurrency) accepts a `createImageBitmap` + OffscreenCanvas processor
  (`web/js/src/texture-processor-browser.mjs`): 5–9× over in-tab wasm fpng.
  Benchmarked hw (ANGLE/Vulkan) vs SwiftShader with
  `web/js/tests/bench-usdzconvert-browser.mjs`: **a wash** — Chrome's image
  codecs are CPU-side either way; only the sub-second canvas raster touches the
  GPU. The pipeline is codec-bound, not raster-bound.

### 5.2 wasm large-heap fixes (commits `3f314f0be`, `627d0472b`)

- wasm32's 2 GB ceiling cannot hold a composed multi-GB scene folder; wasm64 is
  required (`LIGHTUSD_WASM64=1`; in-tab needs Chrome ≥133 memory64).
- wasm64 `MAXIMUM_MEMORY` raised 8 GB → **16 GB** (address-space reservation
  only, with `ALLOW_MEMORY_GROWTH`): the in-heap folder flatten of a 2.5 GB
  scene requests ~10 GB.
- `-sMALLOC=emmalloc` → **dlmalloc** for wasm64: emmalloc asserts
  (`debug_region_is_consistent` in `emmalloc_free`) once the heap grows past
  ~8 GB; dlmalloc completes the identical run.
- Node's `fs.writeFileSync` caps a single write at 2³¹−1 bytes; the CLI writes
  >2 GiB USDZ outputs in 1 GiB `fs.writeSync` chunks.

With these, a 2.5 GB UE-export folder converts to a 2.67 GB passthrough USDZ
in-process on wasm64 in ~64 s (and validates by re-loading).

---

## 6. Full-flatten memory (next/pcp engine)

§2's `LargeSceneLoader` bounds memory by **deferring payloads**. The separate
`next/pcp` engine (`src/next`, exercised by `next_usdcat -f`) does the opposite:
it produces a **fully composed, self-contained flattened layer** — like
`usdcat --flatten` — holding every composed PrimSpec, value array, and time
sample in RAM with no deferral. So peak RSS scales with the *flattened-content*
size, not the on-disk scene size.

Measured under a hard 32 GB cgroup cap
(`systemd-run --user --scope -p MemoryMax=32G -p MemorySwapMax=0 /usr/bin/time -v ...`):

| Scene (full flatten) | next RSS | next time | usdcat RSS | usdcat time | flatten lines |
|---|---|---|---|---|---|
| Kitchen_set (Pixar) | 0.13 GB | <1 s | — | — | ~66 k |
| ALab (Animal Logic) | **0.06 GB** | 0.8 s | 0.13 GB | 1.4 s | ~36 k |
| benchmark scene (Activision) | **2.3 GB** | 45 s | 3.1 GB | 76 s | ~3.45 M |
| reference scene (Disney) | **8.3 GB** | 1 m 48 s | 8.5 GB | ~5 m | ~6.1 M |

next now flattens **at or below usdcat's RSS on every scene, and faster** — a
5.2× cut on benchmark scene (was 11.8 GB) and 3.2× on reference scene (was 26.6 GB), with
**byte-identical output** (verified by `cmp` against the pre-optimization flatten).

The peak used to be dominated by three avoidable residencies on the write path,
now all closed (the lazy-ValueRef read path keeps crate arrays as byte-range
references into the memory-mapped source until they are actually needed):

- **Whole output text held in RAM.** `next_usdcat -f` built the entire multi-GB
  USDA string before a single write. It now **streams** per-prim to stdout via
  `WriteUSDA(std::ostream&, ...)` — the serialized text never coexists with the
  composed stage.
- **Every lazy geometry array materialized in place during write.** Printing a
  value went through the const array accessors, which decode the crate-backed
  array into a resident `std::vector` *in place* (and delete the lazy ref), so by
  end-of-write every array was simultaneously resident. The writer now decodes a
  lazy array into a **throwaway temporary** (`Value::materialized_copy()`),
  bounding resident decoded-array memory to ~one array.
- **Time samples eagerly decoded at parse time.** `TimeSampleStorage::add_dedup`
  hashed each sample value, and `Value::hash()` forces a materialize — so every
  array-valued time sample was decoded into the heap at parse and stayed resident
  for the stage lifetime (benchmark scene: ~3.4 M of ~3.45 M flatten lines are
  time-sample data — the dominant cost). Lazy samples now **skip content dedup**
  and stay byte-range references until the writer decodes them one at a time.

All three preserve (and slightly improve) speed: less allocation, no redundant
decode at parse/clone, no giant-string copy. The §2 bounded loader is still the
path to use when geometry must stay on disk entirely; the difference here is that
a *full* flatten no longer carries gratuitous peak overhead.

### 6.1 Historical: `next_usdcat` vs the then-current `lusdcat` (2026-06-29)

A June 2026 snapshot compared `next_usdcat` (the `src/next` PCP engine,
streaming flattened USDA to a `FILE*`) against the then-current `lusdcat`
library pipeline: benchmark scene 9.71 s / 2.93 GiB versus 1:46.7 / 10.37 GiB; reference scene
43.42 s / 9.67 GiB versus a hard failure on `vegetation_instancer.usd` against the
512 MiB per-asset cap. That snapshot is pre-optimization and is superseded by
§6.2 (USDA writer), §8 and §9 (reader/writer work); the tables stay in git
history (`git log -- doc/large-scene.md`).

Still current from that section: a trusted scene whose referenced layers exceed
the 512 MiB composition safety cap needs `--relax-asset-cap` (raises the
per-layer cap to 8 GiB) or `--max-composition-asset-mb=N`. The default stays
512 MiB for untrusted input.

reference scene's large XGen arrays are also still kept lazy/mmap-backed during
compose/write. The USDC reader decodes older packed array-count headers (low
32 bits = element count, high 32 bits = auxiliary metadata) before applying
guards. It still rejects malformed over-cap arrays through
file-size/addressability checks, but does not reject valid lazy POD arrays
purely because their packed header exceeds the default eager-decode element
guard.

### 6.2 ASCII (USDA) writer optimizations

On a fully-composed large scene the **write phase dominates** total wall-clock
(benchmark scene ~57 %, reference scene ~63 % before this work). The `src/next` USDA writer
(`src/next/writer/{value-printer,usda-writer,dtoa}.{hh,cc}`) was optimized in two
passes. Every change is **byte-identical** to the serial writer — verified by
`cmp`/sha256 against the prior binary across the 615-file `tests/usda` +
`tests/usdc` corpus and on benchmark scene + reference scene (10.18 GB hash match) + ALab, in both
serial and threaded (4/8/16) modes. Output to `/dev/null`, Release build, on a
32-thread / 16-core workstation:

| Scene (flatten write phase) | Before | After |
|---|---:|---:|
| benchmark scene (4.25 GB) | 728 MB/s | **~850–925 MB/s** |
| reference scene (10.18 GB) | 371 MB/s, 26.2 s | **~1000 MB/s, ~11.7 s** |
| reference scene peak RSS | 9.67 GiB | **~8.0 GiB** |

#### Pass 1 — per-element number formatting

Large arrays are the bulk of the bytes, so per-scalar formatting is the serial
hot loop. Two byte-identical changes:

- **No `num_` round-trip.** Each scalar used to format into a stack buffer, append
  to a reused member `std::string`, then be copied *again* into the chunk buffer.
  It now formats once into a stack buffer and appends directly. New
  `dtos_to(char*, float/double)` (`writer/dtoa`) and `IntTo`/`UIntTo(char*, …)`
  (`strfmt.hh`); `ChunkedStream` drops the `num_` member. Serial writer
  +~16 % (benchmark scene 218 → 253 MB/s, reference scene piped 162 → 176 MB/s).
- **Indent cache.** `WriteIndent` emits the indentation prefix in one write from a
  `thread_local` cached padding string instead of `depth` tiny writes.

#### Pass 2 — parallel writer rearchitecture

A profile (a debug switch that skips array-value formatting) showed **~95 % of
reference scene's write phase was array formatting running serially on the main thread**.
The previous parallel design split work by **subtree frontier**, which only
parallelized *leaf* subtrees — but reference scene's geometry lives in *interior* prims,
whose arrays were serialized on the main thread regardless of thread count (write
scaled only ~1.6× from 1→8 threads).

The writer was rebuilt around a **document-ordered task list**:

1. **Phase 1 (serial, cheap):** one pre-order walk of the whole stage emits cheap
   structural bytes inline as `Text` tasks and **offloads every array value**
   (≥ 256 elements) as a task referencing the borrowed array. No giant array is
   formatted here, so the walk is fast (~1.6 s on reference scene).
2. **Phase 2 (parallel):** a worker pool formats **byte-balanced segments** (each
   a contiguous task run) while the main thread writes the finished segment
   buffers **in document order**. Synchronization is lock-free (atomics + a
   bounded look-ahead window that back-pressures workers and bounds in-flight
   memory) — no per-task mutex/condvar, so it scales to ~250 k tasks.

The critical refinement is that **no single giant array may pin one worker** (a
single 44 M-element array was an 8 s serial segment). Giant arrays are split into
element-range chunks formatted concurrently:

- **Directly chunkable arrays** — non-lazy, or *uncompressed*-lazy that can be
  aliased zero-copy from the memory-mapped crate (new `CanBorrowLazyFlat`,
  `types/value-view`) — are chunked **in place** with no decode or copy.
- **Giant compressed-lazy** numeric arrays are **decoded once** into an owned
  buffer (held in a deque, freed as soon as their chunks are consumed, so peak
  memory stays bounded) and then chunked.
- **Half-backed types** (`half`, `quath`, …) widen to float via `as_float_array`,
  so reference scene's 21 M-element `quath` orientation arrays chunk through the float
  range printer too. This was the last serial bottleneck.

New `value-printer` API: `ArrayElementCount`, `IsChunkableType`,
`IsChunkableArray`, and `PrintArrayRangeToStream` (formatting an element range is
byte-identical to the full array printer — the foundation of chunk splitting). The
serial path (`LIGHTUSD_NEXT_NUM_THREADS=1`) is untouched and remains the
correctness oracle. The auto worker cap was raised 8 → 16 now that the balanced
design scales with cores (both scenes still improve at 16+).

Tests: `test_array_range_split_parity` (range reconstruction == full print across
types and cut points) and `test_parallel_writer_parity` (1-thread vs 8-thread
byte-identical) in `tests/next/test_writer.cc`.

The remaining serial cost is the ~6 s Phase-1 build walk on reference scene (not yet
overlapped with Phase 2) — the next available lever.

### 6.3 USDA `/dev/null` write vs USDC crate write

Do not compare the USDA `/dev/null` write numbers above with
`next_usdcat -f -o out.usdc` timings as if they were the same writer path.
They exercise different serializers:

- `-o /dev/null` has no `.usdc` suffix, so `next_usdcat` writes flattened USDA
  text through the optimized parallel ASCII writer. On the local 32-thread /
  16-core workstation, reference scene writes ~10.8 GB of USDA text to `/dev/null` in
  about **14 s** with `LIGHTUSD_NEXT_NUM_THREADS=16` and `--compose-threads 16`.
- `-o out.usdc` uses the next crate writer. reference scene's crate output is only
  ~2.5 GB, but the write phase is still about **36-40 s** even when the output
  path is a `.usdc` symlink to `/dev/null` or a real file under `/dev/shm`.
  Thus the time is not storage bandwidth.

The remaining USDC cost is internal crate construction. As detailed in
[`crate-writer.md`](crate-writer.md), the reference scene crate writer is dominated by the
serial per-spec structural build and global interning/dedup tables over ~4.25M
specs (paths, tokens/strings, fieldsets, value-block dedup). Final byte emission
and array encoding are a smaller slice. Threaded crate writer paths help
moderately: for reference scene, a `/dev/null`-symlink USDC write was ~47.6 s with
`LIGHTUSD_NEXT_NUM_THREADS=1` and ~36.1 s with `LIGHTUSD_NEXT_NUM_THREADS=16`,
but they do not remove the global-dedup floor. Writing the same USDC to
`/dev/shm` remains ~40 s, confirming the bottleneck is CPU/structure building,
not disk I/O.

### 6.4 Gaussian splat coverage

The next/lusdview loader recognizes the public
`ParticleField3DGaussianSplat` schema as a point-like render carrier. It
retains the ALab payload's 2,274,589 positions, uses the largest scale as the
raster billboard diameter, and converts the DC spherical-harmonic coefficient
plus opacity for display. NVIDIA Vulkan headless capture was verified on the
full ALab payload and on `tests/usda/lusdview-gaussian-splat.usda`.

The native LightRT CPU ellipse primitive is now connected to lusdrender's next
RT-preview collector. Gaussian-only stages build an analytic ellipse BVH and
render without triangle conversion; quaternion roll is retained in the leaf
representation. `LUSDR_GAUSSIAN_MAX` bounds diagnostic loads. Large fields are
split into 262,144-splat LightRT chunks (`LUSDR_GAUSSIAN_CHUNK` overrides the
size); CPU tracing visits every chunk, while Vulkan traces chunks sequentially
and retains the nearest hit per ray. CUDA's serialized
point path consumes the same ellipse/roll fields, and the Vulkan compute BVH now
traverses the serialized point leaves natively. NVIDIA Vulkan was verified on
the full ALab payload (1,914,280 non-transparent splats) without tessellated
triangles; CPU chunked tracing was verified on the full ALAB payload
(1,914,278 non-transparent splats) at 15 chunks; Vulkan uses the same chunking
when the NVIDIA driver is available.

The lusdview carrier path retains Gaussian radii, normals, and major-axis
orientation. Its CUDA and Vulkan ray-tracing scene builders upload native
ellipse records and traverse a dedicated analytic point BVH; no triangle proxy
is emitted. The minimal fixture and the full ALab payload were captured on
NVIDIA CUDA and Vulkan with zero RT triangles. The HIP backend now uploads and
binds the same ellipse buffers and kernel arguments; on hosts without ROCm it
reports the runtime as unavailable instead of silently dropping the primitive.
CUDA now discards an analytic point hit when the triangle traversal found a
nearer surface, keeping Gaussian occlusion behavior consistent with Vulkan,
HIP, and LightRT.

The lusdview RT builders split the analytic point BVH into bounded chunks while
retaining one compact attribute stream. `LUSDVIEW_GAUSSIAN_CHUNK=N` controls the
per-BVH point limit (default 262,144); CUDA, HIP, and Vulkan traverse every
chunk and choose the nearest valid ellipse. This limits BVH construction
working memory and avoids a monolithic point-BVH allocation on large fields.
The Vulkan carrier record is packed to 60 bytes per splat (center, major axis,
normal, radii, and RGBA) instead of five padded `vec4` fields. Vulkan reports
the record and BVH/order staging sizes alongside the point/chunk counts,
making GPU residency visible when tuning for an 8-GiB device.

Large Gaussian fields no longer pass through the general `RenderPoints` copy
path in lusdview. Their lazy numeric arrays are read as views and emitted as
bounded 64K-sample `DrawPointsCPU` chunks (configurable through
`LoadOptions::pointChunkSamples`), avoiding a second full positions/scales
allocation and allowing the renderer to consume the field as multiple bounded
carriers. Non-finite, zero-radius, and opacity-below-0.01 samples are rejected
before carrier/BVH construction, including non-finite transformed centers. The
point BVH now reuses the retained world-center stream instead of making another
full center copy during construction. The ALab test retained 1,914,278 visible
samples across 35 point prims and still completed native Vulkan RT setup in
about 0.6 s at 32x32.
When the completed scene is handed to Vulkan, those point and curve carriers
are moved into the renderer's native RT/raster storage instead of copied; the
scene keeps their metadata and bounds while releasing the duplicate CPU
payload. Backends that do not retain CPU carriers continue using the existing
const-reference upload path.
Ordinary `UsdGeomPoints` now use the same lazy, bounded carrier emission, so
large non-Gaussian particle fields no longer materialize a complete
`RenderPoints` object before being copied into the viewer scene. Constant or
per-point widths, normals, display color, and display opacity are sliced with
each chunk; authored normals continue to select the analytic disk/oval RT
carrier. Curve conversion similarly skips retaining authored control points in
the viewer-only `RenderCurves` result after tessellation, removing another full
control-point allocation while preserving the default library API behavior.

Progressive `lusdview --next` loads now apply the same bounded producer queue to
non-mesh carriers: each Points/Gaussian chunk and each Curves carrier is handed
to the renderer as soon as conversion produces it. The completed `DrawScene`
retains only carrier metadata and bounds for selection/purpose controls, rather
than a second full CPU copy. The queue's existing byte limit therefore bounds
mesh, point, curve, and texture events together; cancellation also releases a
carrier that cannot be queued.

Decoded texture payloads use that same queue when eager texture conversion is
selected. After upload, ordinary RGBA/compressed staging is released while the
resolved asset identity remains available for budget eviction and re-decode;
UDIM, Ptex, and mutable page-cache carriers keep their specialized source state.
This prevents progressive loads from pinning a second scene-wide CPU texture
copy after the GPU has consumed it.

Dense UsdVol grids follow the same rule: each VDB field is queued and uploaded
individually, while the progressive scene retains only dimensions, transforms,
and bounds. The queue byte limit includes the float density payload, and a
cancelled volume event aborts conversion without leaving later grids resident.
One-shot Vulkan/GL loads use a derived total density budget as well (override it
with `LoadOptions::volumeMemoryBudgetBytes`); oversized grids are reduced with
bounded nearest sampling and later grids are skipped once the budget is
exhausted. The selected VDB array is moved into the render carrier, avoiding a
second full dense copy during field extraction.

DomeLight HDR conversion is also capped by the texture max-edge policy (or a
4K preview fallback when no edge is configured). Source pixels are sampled into
the bounded RGB staging image and released before cube/prefilter allocation, so
an oversized HDR does not create a full-resolution float-RGB duplicate beside
the IBL baker's working buffers.

Non-instanced round and flat curve strands use bounded native BVHs as well.
Curve points are read through the lazy array-view path, and complete strands are
packed into chunks instead of duplicating the full source array for each BVH.
`LUSDR_CURVE_CHUNK=N` controls the segment limit (default 262,144); the CPU
integrator checks every chunk and Vulkan tessellates each round-curve chunk at
the upload boundary. Lusdrender builds those chunks one curve prim at a time,
so a scene with many large curve prims does not accumulate a second
scene-wide transformed point/radius stream before chunking. Authored points are
consumed directly from lazy float views; only value-clip curves require a
bounded per-prim materialized fallback. Direct curve chunks default to 262,144
segments; instanced prototype BLASes retain a 1,048,576-segment default when
the variable is unset, and both paths honor `LUSDR_CURVE_CHUNK=N`. In `-stats`
mode, inconsistent
`curveVertexCounts`/`points` data is reported as `rt skipped curves` with an
invalid-data count rather than being silently partially rendered. Curve hit
metadata is owned by each bounded chunk, so chunking also bounds the retained
segment shading stream rather than only the LightRT geometry build. GPU curve
fallback chunks retain only tessellated positions and indices; normals are
recomputed during flattening and unused UV staging is omitted.
During Vulkan/HIP/D3D fallback conversion, each consumed native round-curve
BVH is released immediately after bounded tessellation, so native curve
acceleration data does not remain resident alongside the complete fallback
triangle stream.
Already-bounded identity-indexed curve and point chunks are transferred into
the GPU upload batch instead of copied a second time; indexed meshes and
partial chunks retain sparse remapping and validation.

The flat next-loader mesh path uses the same bounded native-BVH policy. Mesh
triangles are split at `LUSDR_TRIANGLE_CHUNK=N` (default 262,144), with global
material, UV, color, and normal indices preserved across chunks. Shadow and
primary rays visit all chunks; `-stats` reports aggregate chunk memory.
The Vulkan flat upload path independently bounds device residency with
`LUSDR_GPU_TRIANGLE_CHUNK=N` (default 262,144). It uploads/traces one BLAS/AS
at a time and reduces nearest hits before shading, so a large non-instanced
scene does not require one monolithic 8-GiB GPU allocation.
Vulkan Gaussian splats are represented by bounded analytic BVH chunks. If the
point descriptor/node buffers cannot be allocated, lusdview reports the
allocation failure and falls back to camera-facing raster splats instead of
binding null Vulkan buffers. `LUSDVIEW_GAUSSIAN_GPU_BUDGET_MB=N` forces a
point-buffer ceiling for regression tests; when available, the Vulkan memory
budget is also used to leave 256 MiB of headroom.
For raster camera-facing helper geometry, Vulkan uses fence-retired upload
buffers of at most 262,144 vertices per draw range; ranges are kept in separate
buffers until the frame completes, avoiding unsafe reuse of a buffer recorded
by multiple draw calls; any reusable monolithic helper buffer is released when
the chunked path is selected. `LUSDVIEW_VK_HELPER_CHUNK_VERTS=N` overrides the
per-range limit for diagnostics and regression tests.
The `lusdrender` next-loader reports retained texture count and resident bytes
in `-stats` (`rt textures: N (resident X MiB)`), including UDIM tiles and
generated mip capacities. It also releases large, non-time-sampled Mesh,
curve, Points, point-instancer, and Gaussian source arrays after extraction and
mesh-light collection when the rendered geometry is static. Animated geometry
keeps its authored arrays for subsequent frame re-streaming; `-stats` reports
the reclaimed bytes as `rt released static geometry`.
All GPU backends reject aggregate triangle counts above the 32-bit hit-ID
limit before allocation, with an explicit diagnostic instead of truncating
chunk offsets.

For lusdview CUDA/HIP scene builds, `LUSDVIEW_RT_BUILD_THREADS=N` bounds
geometry extraction concurrency (default `min(host_threads, 8)`). Assembly now
moves each completed mesh's SoA into the final scene and releases the consumed
per-mesh storage immediately. Geometry extraction and assembly run in bounded
worker-sized batches; `LUSDVIEW_RT_BUILD_BATCH_MESHES=N` overrides the default
of twice the worker count when a tighter CPU-memory ceiling is needed. This
avoids retaining intermediate geometry for the entire scene while later meshes
are still waiting for assembly.
Points and curve CPU fallback proxies are also split at
`LUSDVIEW_RT_PROXY_CHUNK_TRIS=N` (default 262,144), so a large non-analytic
field cannot create one monolithic proxy before RT batching begins.
After a static HIP RT build, lusdview also releases the uploaded CPU carriers
for Points, Curves, and decoded/compressed texture payloads; deformable scenes
retain the arrays required for pose/refit updates.
When a HIP build does retain triangle refit state, the uploaded Gaussian point
arrays and point-BVH metadata are still released because refit only updates
triangle topology/poses.

The native RT texture table is usage-driven. It marks material texture slots
and environment-light maps before decoding/packing, then leaves unreferenced
source textures out of the compact texel and descriptor buffers. This avoids
retaining disconnected decoded images in the 8-GiB-class GPU path while
preserving source-to-table remapping for materials, lights, UDIMs, and mip
chains. The diagnostic reports source count, packed descriptor count, packed
MiB, and skipped unused sources; `lusdview_lightrt_bridge_test` covers the
compaction and mip/UDIM mapping behavior.

Lusdrender now keeps the shared texture-budget lease alive for every retained
base image and generated mip chain. Mip storage is charged before allocation;
when `-texBudgetMb` is full, the renderer retains the base level and reports
`rt texture mip fallbacks` instead of temporarily exceeding the texture cap.
This matters for scenes with many materials because decoder-time budgeting
alone otherwise released its reservation as soon as each temporary decode
object went out of scope.

Lusdview's pre-finalization texture budget also counts the complete retained
CPU payload: base images, authored mip levels, and compressed payloads. The
budget pass therefore cannot measure only level zero and then exceed the cap
when mip/compression processing runs.
The native RT texture table now receives that same byte budget for Vulkan,
CUDA, and HIP. Entries that cannot fit are mapped to the backend fallback
texture with an explicit budget-rejected diagnostic, rather than allowing the
RT staging buffer to grow beyond the scene budget.
Vulkan lusdview also keeps this table across TLAS-only rebuilds (camera/LOD
changes); only texture uploads, budget changes, scene replacement, or RT
re-enable rebuild it. This avoids repeatedly decoding and repacking the same
large texture set during interactive traversal.

Lusdrender's Vulkan mesh-chunk path similarly releases each source mesh's
positions, normals, UVs, and indices immediately after that mesh has been
copied into its bounded GPU upload batch. Only the per-triangle shading data
needed after all chunk traces is retained. This reduces the peak from source
geometry plus chunk staging plus shading arrays to source geometry plus the
accumulated shading arrays, without changing nearest-hit reduction.

The same shared bounded chunk builder is used by the HIP/ROCm backend. When
`LUSDR_GPU_TRIANGLE_CHUNK=N` is exceeded, HIP traces one chunk at a time and
reduces nearest hits before CPU shading, releasing consumed source geometry in
the same way. If ROCm is unavailable, the backend reports the runtime error
and exits cleanly rather than silently producing an incomplete image.
The Direct3D 11 backend now follows the same bounded upload and nearest-hit
reduction policy, so its single-dispatch implementation no longer requires a
monolithic scene/BVH allocation.
Ordinary `UsdGeomPoints` are accumulated into bounded disc/sphere mesh chunks
for the triangle backends; `LUSDR_GPU_TRIANGLE_CHUNK` therefore bounds
point-cloud geometry objects and descriptor count instead of creating one GPU
mesh per point. Authored-normal points use the LightRT analytic ellipse carrier
when that primitive is available, avoiding triangle expansion and reducing
host/GPU geometry memory for large disk/oval point sets.
Round curves use the same LightRT tessellation fallback on Vulkan, HIP/ROCm,
and D3D11; the helper is no longer hidden behind a Vulkan-only compile guard.

Gaussian splats follow the same backend policy: Vulkan ray query keeps the
native analytic ellipse BVH, while HIP/ROCm and D3D11 use the bounded oriented
ellipse tessellation fallback because those LightRT interfaces do not expose
analytic ellipse primitives. This prevents AMD scenes from reaching the GPU
backend with zero geometry and preserves a visible, diagnosable fallback.
For those fallback backends the loader also skips constructing the native
ellipse arrays entirely, avoiding a full Gaussian CPU-data duplicate before
tessellation.
The fallback collector now reads Gaussian attributes through lazy array views,
so positions, scales, orientations, opacity, and SH data are not copied into
five full temporary vectors before tessellation.
Both lusdview and lusdrender avoid decoding an oversized compressed SH array
when only its DC color is needed for preview; they report the skipped optional
payload and use the neutral fallback color instead. Small and zero-copy SH
arrays retain their authored color.
It also applies opacity to the fallback color buckets and rejects non-finite or
near-zero opacity samples, matching native ellipse filtering. Each color bucket
is flushed at `LUSDR_GPU_TRIANGLE_CHUNK`, preventing HIP/ROCm and D3D11 from
retaining a single multi-million-triangle proxy for a large field. Gaussian
fallback chunks omit redundant per-vertex normals and dummy UVs; the GPU
flattener recomputes geometric normals from the retained positions.
The native HIP/AMD path also preflights the complete Gaussian buffer footprint
against `LUSDVIEW_GAUSSIAN_GPU_BUDGET_MB` or live `hipMemGetInfo` free memory
(with 256 MiB headroom), reporting a bounded error before partial device upload
when the field cannot fit.

Chunk vertex remapping is sparse and chunk-local rather than an array indexed by
the source mesh's full vertex count. A small GPU chunk therefore no longer
allocates a second full-mesh remap table while processing a very large mesh.

## 7. Verification

```
cd build && cmake --build . -j16
# cross-directory cwp anchoring regression test (§3.1):
cd build && ctest -R feat-large-scene --output-on-failure
# the same suite end-to-end on the next::pcp::Cache backend (extent budget,
# worker-thread snapshot, cross-layer payload-owner anchoring):
cmake -S . -B build-next-pcp -DCMAKE_BUILD_TYPE=Release \
  -DLIGHTUSD_USE_NEXT_PCP_LARGE_SCENE=ON -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_BUILD_EXAMPLES=OFF -DLIGHTUSD_BUILD_TOOLS=OFF
cd build-next-pcp && make feat-large-scene -j16 && \
  ctest -R feat-large-scene --output-on-failure
# focused payload-policy owner-anchor regression (next pcp unit level):
cd build-next && ctest -R 'next_test_pcp$' --output-on-failure
# structural load within budget (absolute path works from any cwd now):
<build>/large-scene-load <asset-root>/benchmark/benchmark.usda --mode=none
# expect: ~32,811 total prims, 373 deferred payloads, RSS ~1.7 GiB.
# --load-some=N streams deferred proxy geometry on demand.
# full composition + USDA writer stress checks:
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/reference_scene/usd/reference_scene.usda
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/alab/_merged_ALab/entry.usda
cd build && ctest --output-on-failure     # no regressions (2 pre-existing
                                           # MaterialX failures are unrelated)
# suffix-fallback unit tests (§4):
cd build && ./unit-test-lightusd ioutil_asset_path_suffix_candidates_test \
                                 comp_reference_suffix_fallback_test
# browser conversion bench (§5):
cd web/js && xvfb-run -a node tests/bench-usdzconvert-browser.mjs --hw \
  --scene <scene-dir> --root <root.usd> --case browser:png:1024
```

## 8. Next-core large-scene optimization, October 2026

This pass builds on the existing per-stack negative-spec lookup filter. The
comparison baseline includes that filter. Release builds use the same compiler,
`-O3`, thread support, and jemalloc. The baseline library was also rebuilt with
the current benchmark CLI so raw-layer writes and explicit large-file limits
can be compared without changing its parsing/composition/writing algorithms;
its proxy and district USDC hashes match the saved original executable.
Full-file readback exposed an independent 1 GiB ceiling in the generic blob
decoder: the full crate's value-representation table expands to 1,413,255,776
bytes. For subsequent large-file comparisons, a qualification-only baseline
lifts that codec ceiling after the reader's existing table/budget checks,
retaining its original temporary-buffer allocation and copy. Its proxy and
district hashes also match. Reports retain the distinct executable hashes.

Implemented changes:

- Crate writing reuses a consecutive property's parent-path lookup, interns
  element views directly, reuses the empty hash-table slot found during lookup,
  and caches inferred type-name token IDs lazily. Authored type names retain
  their original path, token insertion order is unchanged, and cache state is
  reset for every write. The optional incremental ancestor-prefix hash patch
  was tested and dropped: three paired district runs showed no benefit
  (write median 12.63 s without it, 13.00 s with it).
- Composition releases each warm worker after its contributions are remapped
  and merged. Cache teardown pins library-loaded file layers, destroys their
  borrowers, then releases independent layers in a bounded pool (at most eight
  workers), synchronously before returning. Caller/preloaded/custom-reader
  layers retain serial destruction. Explicit serial composition stays serial.
- One-shot composition reuses a resolved source site's string when it exactly
  matches the source-cache key, before namespace construction allocates output
  paths. Mutable cache access restores an owned key first. After the namespace
  walk, the fill pass retains the address-stable source vectors while dropping
  their lookup keys and hash index. Cancellation, invalidation and cache reuse
  retain ordinary lookup semantics; long-lived caches keep their default
  ownership policy.
- Automatic parsers inside outer composition loading/warm pools stay serial;
  explicit parser hints remain honored. Thread hints and crate table limits
  are forwarded consistently through root, arc, prefetch, sublayer and reload
  paths. This avoids multiplying automatic reader pools for every loaded file.
- The crate reader reuses one lazily-created arena across PATHS and stage-build
  phases, joining and releasing it on every return, including cancellation.
  Value-representation tables decode directly into their final storage after
  table-count and memory-budget validation, avoiding a full temporary/copy.
  An explicit-budget decoder handles large validated tables; the ordinary
  C decoder retains its 1 GiB default, and reader security defaults remain.
  The USDA lexer exposes borrowed identifier/number/path tokens for transient
  parser use; its existing owning-token interface, rewind state, diagnostics,
  numeric conversions and persistent authored values retain their semantics.
  Direct owning-token consumption keeps its original scan-and-return path;
  materializing into cached lookahead first would add an unnecessary copy.
- USDA range formatting now includes owned string, token, asset-path and
  path-expression arrays, preserving escaping and document order. The existing
  decode-once path remains in place. Optional diagnostics distinguish skeleton
  and pipeline wall time from accumulated build/format worker time and record
  writer wait, direct-stream time and ready-buffer bytes.

The final small-scene paired run used one warmup and three alternating measured
pairs, automatic parse/compose/write counts, normal teardown, warm inputs, and
`/dev/null` output. Times below are medians; RSS is the median process peak.
The acceptance check uses the largest measured peak for each binary, with a
maximum 5% growth allowance; all three cases pass.

| Case | Process, baseline → candidate | Load + compose | Write | Peak RSS, GiB |
|---|---:|---:|---:|---:|
| benchmark scene proxy → USDC | 3.27 → 2.98 s | 0.991 → 0.887 s | 1.821 → 1.680 s | 1.71 → 1.68 |
| benchmark scene full district → USDC | 34.84 → 26.95 s | 18.22 → 9.73 s | 13.83 → 14.32 s | 18.86 → 15.88 |
| benchmark scene full district → USDA | 25.59 → 15.81 s | 20.50 → 10.18 s | 2.385 → 2.381 s | 21.01 → 15.92 |

These are 8.9%, 22.6% and 38.2% end-to-end improvements respectively. The
district improvement comes from loading/composition/reclamation; this final
sample does not show a district crate-write speedup. Maximum observed RSS
ratios are 0.989, 0.799 and 0.800, respectively.

The proxy USDC remains 286,484,306 bytes with SHA-256
`162c9059a1cc01ff442c8a1fc9d18b0e726fa25d51bd2a4e79813a340035ea54`;
the district remains 580,261,850 bytes with SHA-256
`98619c408f717732b2ad7c2a90493303331f2d65f3e86c9656b62ccc877439bc`.
Each of the three retained writer patches was checked against both hashes
before the composition/parser work, and the current candidate matches them too.
The district emits 4,458,013 specs/paths and 7,727,525,276 USDA bytes.

The district USDA timing reports zero direct-stream work for giant compressed
integer arrays. Its existing shared decode slots already avoid per-chunk
re-decoding. A new compressed-block formatting scheduler is therefore not
included without evidence of benefit and a bounded-memory implementation.

The all-45-district full-LOD scale checks use eight composition/writer workers
and automatic parsing, with normal teardown. Both produce 10,224,245 prims;
USDC contains 61,833,354 specs/paths and 3,014,654,530 bytes, and direct USDA
contains 48,719,017,497 bytes. The USDC process peak is 80.76 GiB against the
matching saved warm baseline's 78.39 GiB (+3.02%); the fresh USDA pair peaks at
82.89 GiB baseline and 80.60 GiB candidate (-2.76%). Both pass the 5% RSS gate.
Earlier candidates exceeding that gate were rejected; property-slot reservation
also increased RSS and was dropped.

These single full-scale comparisons do **not** demonstrate a throughput win.
USDC elapsed time was 485.03 s baseline and 694.28 s candidate, with 625,469
versus 910,309 major faults; USDA was 259.14 s and 288.41 s. A separate candidate
validation write took 131.48 s in `fields_specs`, against 326.55 s in the
throughput run. Retain these paging-sensitive runs as scale/memory evidence;
use repeated matching pairs before drawing full-scene speed conclusions. Full
USDA text exceeds the 48 GB validation-file cap, so its hash is streamed rather
than retaining the text in shared memory.

The final reader-enabled binary also completed the full USDC scale run with
the same counts/byte size and a 70.24 GiB process peak (RSS gate passes). This
run incurred substantial swap and 1,322.50 s elapsed time; completed build
artifact caches were advised out during the run. Its elapsed time is diagnostic
only. Per-second RSS/swap/fault samples are retained; a smaller resident peak
under paging must not be interpreted as the same-sized reduction in allocation.

The full USDC checksum remains
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.
OpenUSD and the final next reader both reopen it; next reports all 10,224,245
prims after decoding the table above 1 GiB. Baseline/candidate raw USDA exports
from that same crate both contain 48,723,062,524 bytes with SHA-256
`583e382e228b9278069b8aadfad565c92a20c23b7e2b68c9ebd99c36cd041bca`.
This raw-export byte comparison is separate from the direct-composition USDA
throughput case above; crate reconstruction changes the text representation.

Verification for this implementation:

- Standalone Debug and thread-disabled suites: 66 checks each, with the two
  optional AOUSD corpus checks skipped. ASan/UBSan: 16 focused checks passed
  with LeakSanitizer disabled because the execution environment uses tracing.
  TSan: five focused checks passed, including the sparse large-buffer guard.
- Pinned OpenUSD flatten differential: 718 inputs, no unexpected differences
  or next errors. The native compatibility script also passes with a local
  shim for the environment's spurious Node synchronous-spawn `EPERM` on
  successful zero-exit children; failed exits and signals still propagate.
- Native Release build passes. Native regression passes 46/47 checks; the
  generated supported-schema registry/ledger mismatch is present unchanged at
  `HEAD`, independent of this pass.
- Both fresh WASM32 products build. The combined serial module required the
  crate reader's arena member/cleanup to be conditional on thread support.
  The full web run passes 54 checks; the existing memory64 fidelity test passes
  on locally available Node 24, and all 67 pinned Menagerie closure models
  pass. Remaining web gaps are two URDF export parity differences in the
  current workspace, three browser checks blocked by socket restrictions,
  and two simulation checks lacking the separate MuJoCo WASM runtime.

The 7.7 GB raw USDA rewrite was extended to seven alternating measured pairs
after a noisy three-pair result. Parse medians are equal at 5.469 s; total
process medians are 13.06 s baseline and 13.51 s candidate, with ranges
12.39–14.07 s and 12.22–15.26 s. Write medians are 4.476 s and 4.568 s; cleanup
accounts for most of the remaining spread. Maximum RSS differs by less than
0.01%, and raw USDA bytes remain identical (SHA-256
`765953e1a9e769ae92d7c0b208e42f31f1efa6a8dd38ecb4cfb018fea5d2e277`).
This case demonstrates no parser/writer throughput win. The lexer copy fix is
retained with identical token/rewind semantics, and large string-array range
support is covered separately by owned-array and threaded escaping tests.

reference scene follows benchmark scene in the qualification sequence. With all thread hints
automatic, process medians are 15.29 → 15.58 s for USDC and 9.35 → 10.66 s for
USDA. The USDC write itself improves from 10.027 to 9.117 s, but the conservative
serial inner-parser policy increases load/composition from about 3.4 to 5.0 s.
Maximum RSS ratios are 0.943 and 0.971. This default-policy result is a throughput
regression, with a lower resident peak; do not describe it as a scene-load win.

An explicit parser-hint sweep (eight composition workers) measures median
load/composition of 5.760, 4.229, 3.895 and 3.622 s for auto, two, four and eight
parser threads. A separate matched baseline/candidate run with
`--parse-threads 8` (other hints automatic, one warmup, three alternating
measured pairs) gives:

| reference scene output | Process, baseline → candidate | Load + compose | Write | Peak RSS, GiB |
|---|---:|---:|---:|---:|
| USDC | 15.13 → 14.18 s | 3.361 → 3.350 s | 9.826 → 9.201 s | 4.74 → 4.73 |
| USDA | 10.73 → 9.15 s | 3.731 → 3.184 s | 4.399 → 4.000 s | 5.37 → 5.28 |

Maximum RSS ratios are 0.998 and 0.989. The tuned candidate matches the default
scene's exact hashes: USDC is 2,512,205,846 bytes with SHA-256
`a8f5092209cbcd30841c129354bda0aaae5ea21607f7bd70725ef933d7b2c478`;
USDA is 10,375,532,893 bytes with SHA-256
`8fb8b237c0d757f52946d6a3772284facf6521b3e399fb0f6b570f3ec9c87b2b`.
Both baseline/candidate default exports match too, and the crate reopens with
next and OpenUSD. The reference scene timing exposes 1.32 s of direct compressed-int
streaming within a 3.55 s USDA write. No additional parallel compressed-int
scheduler is included; this remains a measured follow-up hotspot rather than
an unverified bounded-buffer implementation.

ALab uses a per-file symlink union of the base package and techvar fragment
(techvar overwrites the base files), with all payloads loaded; the root is the
packaged `ALab/entry.usda`, so no custom entry is involved. §9.3 records the
reproduction. Its empty baked placeholder stays in the merged
scene; frame 1004 is tested independently as an authored layer, and the other
baked frames remain deferred. The merged scene contains 24,466 prims and emits
732,541,268 USDC bytes or 4,566,009,415 USDA bytes. Repeated process medians are
1.39 → 1.37 s and 1.40 → 1.38 s respectively: effectively flat at this scale.
Maximum RSS ratios are 1.002 and 0.993; the candidate's median peaks are 0.985
and 1.916 GiB.

Merged ALab hashes match baseline/candidate: USDC
`2f7176c8008f0ae219c58159ef19f3179dab26b2d35073f5fc5aeac174dc7493`
and USDA
`f764d58c78cae61a597579fc228e778fe7acb9e749322583f386c0106bc266e7`.
Both readers reopen the validation crate. Preserved custom Maya metadata
warnings are retained; no missing-asset load is accepted as a partial scene.

The separate baked frame contains ten prims. Parse, USDC rewrite and USDA
rewrite medians are 0.15 → 0.15 s, 0.57 → 0.57 s and 3.18 → 3.17 s, with maximum
RSS ratios 1.001, 1.000 and 1.005. Its matching rewrite hashes are USDC
`a8d085175ee96b67a66a023b2e1ca1618956562824b27728521e322444731884`
(3,537,903,797 bytes) and USDA
`5ca18bdadea9d1f009ce54a01eed4bb967115b091faa1df31890e5b208880819`
(10,039,453,727 bytes). The rewritten crate reopens with next and OpenUSD.

## 9. Automatic arc-layer reader budget follow-up

This pass targets the automatic-parser penalty above. Its baseline is the
completed implementation in `02bc4deed`, rather than the pre-optimization
baseline in §8. Explicit parser hints keep their existing behavior.

Automatic background arc prefetch divides its loading budget between files
and parsers. It starts at most `max(1, composition_threads / 4)` file workers
(also limited by the initial queue), and assigns each at most eight parser
threads, with `file_workers * parser_threads <= composition_threads`. Inputs
smaller than 64 MiB stay serial. Other composition warm workers retain serial
automatic parsing. This bounds the prefetch requests; it is not a global cap
across concurrent namespace workers, foreground loads and prefetch.

The size gate uses the actual reader input/source length, with no additional
filesystem query. It covers native files, borrowed/owned buffers and individual
USDZ entries; archive container bytes do not determine an entry's policy.
`min_parallel_parse_bytes` defaults to zero in the USDA, crate and layer-load
options, preserving ordinary reader hints. Composition supplies the threshold
only for automatic background prefetch. The USDA parser restores its original
hint after every parse, including failure, so a reused parser can alternate
between small serial and large parallel inputs. Security and memory limits
remain unchanged.

Two earlier versions were rejected. Lending parser workers to every prefetched
layer raised the benchmark scene district peak from 15,671,604 to 22,170,512 KiB. A
filesystem-query size gate passed the three-pair scene run but failed the
extended composition-only check with maximum RSS ratio 1.070. The reader-source
revision below was requalified separately; rejected measurements are not mixed
into its result.

The accepted run uses one warmup and three alternating measured pairs,
automatic parse/compose/write hints, normal teardown and `/dev/null` output.
benchmark scene runs first, followed by reference scene and merged ALab. Times are medians; RSS
is the median process peak. All seven maximum-peak RSS comparisons pass the
5% gate (ratios 0.996, 0.977, 0.980, 0.996, 0.988, 1.004 and 1.009):

| Case | Process, baseline → candidate | Load + compose | Median peak RSS, GiB |
|---|---:|---:|---:|
| benchmark scene proxy → USDC | 2.84 → 2.91 s | 0.833 → 0.837 s | 1.638 → 1.640 |
| benchmark scene district → USDC | 29.06 → 27.88 s | 11.420 → 10.915 s | 16.96 → 17.01 |
| benchmark scene district → USDA | 15.63 → 15.35 s | 10.272 → 10.017 s | 16.53 → 16.03 |
| reference scene → USDC | 15.74 → 15.40 s | 5.010 → 3.955 s | 4.495 → 4.483 |
| reference scene → USDA | 10.92 → 9.52 s | 5.090 → 3.892 s | 5.213 → 5.165 |
| Merged ALab → USDC | 1.37 → 1.39 s | 0.596 → 0.570 s | 0.987 → 0.988 |
| Merged ALab → USDA | 1.46 → 1.46 s | 0.580 → 0.572 s | 1.943 → 1.951 |

reference scene's automatic load/composition time falls by 21.1% for the USDC case and
23.5% for USDA; process time falls by 2.2% and 12.8%. ALab remains effectively
flat. These results do not establish a writer speedup: the reference scene crate-write
medians are 9.126 → 9.670 s, while USDA write medians are 3.718 → 3.677 s.
Earlier explicit-parser samples in §8 use different runs/settings and are not
a paired comparison with this automatic policy.

The separate seven-pair benchmark scene composition-only check measures process
medians of 15.49 → 15.45 s and load/composition of 9.941 → 9.865 s. Load ranges
are 9.219–11.139 s and 9.788–10.802 s. Maximum RSS ratio is 1.000714; the gate
passes. Treat benchmark scene as effectively neutral in this follow-up rather than
claiming an additional speedup from the three-pair output sample.

Validation explicitly selects `--parse-threads 0` and reproduces the exact §8
USDC hashes for proxy, district, reference scene and merged ALab, and the §8 USDA hashes
for district, reference scene and merged ALab. Each retained validation crate reopens
with next and OpenUSD. Throughput output stays on `/dev/null`; validation uses
task-owned `/dev/shm` files below the aggregate 48 GB cap, promptly removed,
and streaming hashes for text. The all-45-district benchmark scene and separate baked
frame 1004 scale checks were not repeated in this follow-up; their results
remain those of §8. No new raw-parser/writer throughput win is claimed.

The final candidate SHA-256 is
`c784552326968f5d6b09e4ea264997abe95cc8fc8b8038a6e66291b07a8c0e20`.
Standalone Debug completes 66 checks with two optional AOUSD checks skipped.
Four affected reader/PCP checks pass in each thread-disabled, ASan/UBSan and
TSan build. The new synthetic test compares serial and size-gated USDA/USDC
loads through files, borrowed memory, owned memory and USDZ entries, and checks
USDA fast-path selection across threshold boundaries, reuse and failed parses.
LeakSanitizer remains disabled in this traced environment. The pinned OpenUSD
flatten differential completes 718 inputs with no unexpected differences or
next errors (681 pass, 18 intentional, 11 oracle skips and eight fixture skips).

Both fresh WASM32 products build. The Node 24 regression profile reports 55
successful entries and two failures, matching the earlier URDF parity gaps in
§8: missing mesh extents in the schema utility test and the same URDF USDA-hash
difference in the variant-overload suite. The profile initially skips generic
native/WASM validation because its default CLI path is absent; running that
gate separately against the existing native Release CLI passes all six inputs.
Browser and physics profiles were not repeated in this follow-up.

### 9.1 Large-crate readback limits (2026-10)

The USDC reader's structural-table caps are count bounds independent of the
byte budget. The composed benchmark scene district crate (580,261,850 B,
4,458,013 specs) contains **12,642,785 FIELDS entries**, above the secure
default `max_fields = 10,485,760`, so a default read-back fails even with a
generous memory budget:

```
Offset 530826639: Too many fields: 12642785 > 10485760
```

Raise the table caps for trusted datasets. `lusdcat` exposes
`--max-crate-elements N` (sets tokens/strings/fields/fieldset indices/specs/
paths) and `--max-memory-mib` for the byte budget:

```sh
./build_ninja/lusdcat --rewrite-layer --max-crate-elements 100000000 \
  --max-memory-mib 16384 --output-format usdc -o out.usdc district.usdc
```

The Python binding exposes the same opt-in; zero keeps the secure defaults:

```python
stage = lightusd.load("district.usdc", max_memory=4 << 30,
                      max_crate_elements=100_000_000)
```

Functional evidence (timing under a loaded host, not a throughput claim): the
district rewrite is 578,776,001 B with SHA-256
`e049a40ff95d5074984d9c556e6101b6172a6f1864778e68562bf44c31f9fc09`, and the
Python load reports 745,246 prims / 4.07 GiB of stage accounting. The
all-45-district crate has 61,833,354 specs/paths (§8), so use a correspondingly
larger cap and memory budget. The reader now includes the actual count, limit
and a "raise the crate table limit" hint; `lusdcat` adds the flag hint.

### 9.2 Reader fieldset decode without per-field strings; writer dedup-table note (2026-10)

Two findings from the district rewrite case (§9.1 command: parse a 580 MB
composed crate, write it back unchanged, `/dev/null` output, automatic thread
counts on a 32-core host).

**Rejected: the staged-path block-dedup table is never used by a file write.**
`doc/crate-writer.md` calls out that `InternBlock` (`crate-writer-impl.inc`)
pre-sizes its open-addressed `block_slots_` table at most 1M slots and rehashes
while a large scene interns millions of blocks. Raising that cap is a no-op for
every seekable-sink crate write (which is what `lusdcat -o file.usdc` and
`WriteLayerToFile` do): such a write streams VALUE blocks straight to the sink
(`stream_values_ = true`), `Impl::Write` skips the `GrowBlockSlots` setup, and
`InternBlock` returns through `StreamInternBlock` before touching
`block_slots_`. Instrumenting the end of `Write` on the district rewrite prints:

```
[probe] stream_values=1 block_slots=0 block_slots_used=0 stream_dedup=1086623
```

The streaming path already dedups with a segmented, grow-without-rehash table
(`StreamDedup`, 1,086,623 unique blocks for 2.89 M passed-through arrays), so
there is no 1M-slot ceiling to lift and no measurement to make. The pre-sized
table only matters for `WriteLayerToMemory`/`WriteToMemory`.

**Landed: fieldset decode returns field-name views instead of owning strings.**
`ResolveFieldset`/`ResolveFieldsetRaw` used to copy each field's name out of the
token table into a fresh `std::string`; the district crate has 12,642,785
FIELDS, and every field is decoded at least once (prim fieldsets are decoded by
the prim collect, twice, and again by the body pass), so a rewrite built and
destroyed well over ten million short-lived strings purely to compare their
names against literals. The scratch records now hold `std::string_view`s into
the token blob (stable for the whole stage build; the one path that needs to
own a name — unknown-extension metadata — copies explicitly), and
`DecodePropMetaField` takes a view. Semantics, ordering, diagnostics and the
warnings' text are unchanged.

Paired district-rewrite measurement (one warmup, three alternating measured
pairs, warm `/dev/shm` input, `/dev/null` output, 1-minute load ≤ 6):

| Metric | baseline | candidate | delta |
|---|---:|---:|---:|
| Process wall, median | 19.78 s | 19.21 s | −2.9% |
| CPU (user+sys), median | 34.11 s | 32.75 s | −4.0% |
| Retired instructions (`perf stat`, 2 runs) | 134.6 G | 130.5 G | −3.1% |
| Peak RSS, median | 6.98 M KiB | 6.98 M KiB | −0.01% |
| `BuildStage`, median | 4496 ms | 4333 ms | −3.6% |
| `bodies`, median | 881 ms | 843 ms | −4.3% |
| Minor faults, median | 3,424,448 | 3,348,138 | −2.2% |

Phase-level medians move both ways inside the run-to-run spread (`fields_specs`
6.82 s → 6.33 s and `property_index` 66 ms → 126 ms are not attributable to a
reader-only change); the aggregate wall/CPU/instruction agreement and the
fault count are the evidence, not any single phase. Reading the district and
the full reference_scene, and composing the district, still produce the §8/§9.1 hashes
exactly.

Fresh writer attribution on the same rewrite (instrumented bins carry ~1.7 s of
timer overhead, so read the shares, not the absolutes): `prim_loop` is 93% of
`fields_specs`; inside the prim loop `EmitPrimProperties` is 64%, of which
`EncodeValue` 48% (half of that `InternBlock`/passthrough staging), token
interning 28%, property-path interning 17%, `EmitPropertySpec` 6% and authored
property-metadata fields 4%. This matches the earlier profile in §8 and
`doc/crate-writer.md`; the cost is spread over per-spec field construction and
global interning, with no single dominant routine left to remove.

Verification for this section: native suite 67/67 (`ctest` in `build_ninja`) and
standalone `next` Debug 71/71, 227 `tests/usdc/*.usdc` fixtures rewritten to
USDA byte-identically by the baseline and candidate binaries, and these
unchanged outputs — district rewrite `e049a40f…` (578,776,001 B), composed
district `98619c40…` (580,261,850 B), reference_scene `a8f50922…` (2,512,205,846 B)
(snapshot note in §9.3).

### 9.3 Qualification of the fieldset-name change (2026-10)

Sanitizers and thread checking (all in build trees outside the repository):

| Check | Build | Result |
|---|---|---|
| ASan + UBSan (value-name lifetime is the risk here) | `Debug`, `-fsanitize=address,undefined -fno-sanitize=vptr`, standalone `next` | 70/71 pass, **zero sanitizer reports** |
| ASan district crate parse | same tree, `lusdcat -l` on the 580 MB composed district | 735,484 prims, no reports (156 s, ASan-slow) |
| TSan with `LIGHTUSD_NEXT_FINE_LOCKS=ON` | `Debug`, `LIGHTUSD_NEXT_BUILD_TSAN_TESTS=ON` | 5/5 pass (`tsan_propname`, `pcp_parallel`, `usdc_reader`, `crate_dict`, `instance_group`) |

The one ASan failure is `next_test_instance_group`'s wall-clock gate
(`BuildStage=16.0 s` against a 6 s bound). That check already exempts itself
under `LIGHTUSD_TEST_TSAN` because "timing gates are meaningless under
instrumentation"; it has no equivalent ASan exemption, so a `Debug`+ASan
`BuildStage` that is ~4x slower trips it. Every semantic check in that test
passed.

Baked-frame identity (ALab `baked_procedurals/payload/stoat_body_main.1004.usd`,
ten prims, `--rewrite-layer --max-crate-elements 100000000`): baseline and
candidate both reproduce the §9 hashes exactly — USDC
`a8d08517…`/3,537,903,797 B and USDA `5ca18bda…`/10,039,453,727 B.

reference scene composition (`reference_scene.usda`, `--parse-threads 8 --compose-threads 8`,
USDC to `/dev/null`), one warmup plus three alternating pairs:

| Metric | baseline | candidate | delta |
|---|---:|---:|---:|
| Process wall, median | 15.23 s | 15.01 s | −1.4% |
| CPU (user+sys), median | 30.94 s | 30.87 s | −0.2% |
| Peak RSS, median | 4.71 GiB | 4.73 GiB | +0.4% |
| `build_stage` | 3689 ms | 3707 ms | +0.5% |

That is the expected result for this scene: only ~3.7 s of a 15 s run is
`BuildStage`, and the crate reader is a fraction of that, so a reader-side cut
does not move reference_scene. The district rewrite in §9.2 is the case that isolates it.
Both binaries emit the same reference_scene crate (1,815,093,161 B, SHA-256
`267d237a…`), but that is **not** the §9 size/hash (2,512,205,846 B,
`a8f50922…`): the reference_scene copy on this machine resolves 20 elements
(`elements/` has exactly the 20 referenced dirs, all present) and composes to a
1.82 GB crate, so the §9 number came from a larger reference_scene snapshot. Treat the
`a8f50922…` value as tied to that snapshot, not as a property of the current
local asset; the matched-binary equality above is the meaningful check.

Two further paired sets on a quieter host (one warmup, three or four
alternating pairs, no swap):

| Case | Metric | baseline | candidate | delta |
|---|---|---:|---:|---:|
| reference scene `reference_scene.usda` → USDA | process wall | 8.48 s | 8.57 s | +1.1% |
| | CPU | 42.95 s | 42.81 s | −0.3% |
| | peak RSS | 4.37 GiB | 4.37 GiB | −0.1% |
| | `build_stage` | 3762 ms | 3895 ms | +3.5% (spread 3606–3971 both) |
| Baked 1004 → USDC | process wall | 0.91 s | 0.91 s | 0.0% |
| | peak RSS | 312.4 MiB | 312.5 MiB | +0.05% |
| Baked 1004 → USDA | process wall | 1.44 s | 1.43 s | −0.7% |
| | CPU | 34.20 s | 34.36 s | +0.5% |
| | peak RSS | 1017.0 MiB | 1019.8 MiB | +0.3% |
| District rewrite (re-run) | process wall | 19.38 s | 19.05 s | −1.7% |
| | CPU | 33.90 s | 32.75 s | −3.4% |
| | peak RSS | 6.655 GiB | 6.653 GiB | −0.04% |

The district re-run agrees with §9.2 (−2.9% wall / −4.0% CPU then, −1.7% /
−3.4% now; the candidate CPU median is 32.75 s in both). reference scene and the baked
frame are neutral: they are dominated by composition and by the USDA text
writer, not by crate field decoding. No case comes near the 1.05× RSS ceiling.

Merged ALab needs **no ad-hoc entry**: it is the packaged `ALab/entry.usda`
composed over the official merged package (`doc/benchmarks.md`: `cp -al
ALab-2.3.0/ALab _merged_ALab`, then `cp -alf techvar_assets/fragment/.
_merged_ALab/fragment/`). The merged stage reproduces exactly on this machine:
**24,466 prims**, USDC `2f7176c8…` = 732,541,268 B, USDA `f764d58c…` =
4,566,009,415 B, and both the baseline and the candidate binary emit those
bytes (the empty `baked_procedurals/main.usda` placeholder stays, as §9 says;
the baked frame above is measured separately).

The one trap, worth recording because it silently produces the wrong scene: the
techvar fragment must *overwrite* the base package's files, not fill gaps. 2,111
paths exist in both trees and the base copies are ~213-byte placeholder layers
(the techvar versions of those same paths are 0.99 GB of real content); a "link
the base, add only techvar files that are missing" union keeps the placeholders
and composes `entry.usda` to a 397 KB / 0.2 s stage instead of 732 MB /
24,466 prims.

WASM32: both web products build cleanly (`-DLIGHTUSD_WASM_PRODUCT=next`,
342 targets; `=legacy`, 411 targets, `MinSizeRel`). The next module was also
exercised functionally in Node: `lightusd_next_converter_rewrite` on five
`tests/usdc` crates (suzanne, curves, usdskel-001, shader,
skeleton-purpose-001) returns bytes identical to the native `lusdcat
--rewrite-layer --output-format usdc` rewrite of the same fixture — e.g.
suzanne 50,018 B `2269a062…` from both, with matching token/path/spec counts.

Full-scale limits of the all-45-district crate (`benchmark_fullLOD2.usdc`,
3,014,654,530 B, 61,833,354 specs): it declares **176,656,972 fields**, so the
default `--max-crate-elements` and even 100M are rejected with the
"raise the crate table limit" hint; at 300M it then needs a larger memory budget
than 16 GiB (`Compressed integer workspace exceeds cumulative allocation
budget` after 64 s / 7 GB), and at `--max-memory-mib 49152` it loads with
**81.3 GiB peak RSS** and zero swap (147–152 s per run on a quiet host; the
same input measured 357 s while another job held the machine). It is a
one-at-a-time job: nothing else should be resident while it runs.

Full-scale paired run on that crate (176,656,972 fields, 61,833,354 specs;
1 warmup + 2 alternating pairs, zero process swap, peak RSS 81.33 GiB for both
binaries):

| Metric | baseline | candidate | delta |
|---|---:|---:|---:|
| Process wall, median | 152.1 s | 147.9 s | −2.8% |
| CPU (user+sys), median | 331.8 s | 318.1 s | −4.1% |
| CPU user / system | 217.9 / 113.9 s | 213.0 / 105.1 s | −2.3% / −7.7% |
| Peak RSS | 81.33 GiB | 81.33 GiB | +0.002% |
| `BuildStage` | 70.0 s | 65.0 s | −7.2% |
| `properties` | 14.84 s | 13.90 s | −6.4% |
| `bodies` | 12.17 s | 11.40 s | −6.3% |
| `prims` | 4.57 s | 4.09 s | −10.6% |
| `sort` | 3.87 s | 3.28 s | −15.2% |
| `property_index` | 1838 ms | 1081 ms | −41% |
| `ReadFieldsets` | 2273 ms | 2191 ms | −3.6% |
| `ReadSpecs` | 1572 ms | 1529 ms | −2.7% |
| `ReadPaths` | 10.23 s | 9.66 s | −5.5% |
| `ReadFields` | 5.65 s | 6.23 s | +10% (one 7.11 s outlier; other run 5.36 s) |

This is the case the change was aimed at: at 176.7M fields the per-field string
churn is large enough to show up beyond the district's 12.6M, and the gain
lands where the fieldset is consumed (`properties`, `bodies`, `prims`,
`property_index`). Phases that never touch field names (`ReadFields`,
`ReadPaths.fill`) move the other way inside noise. No memory cost: peak RSS is
identical to within 2 MiB.

Reproduce (quiet host, ~85 GiB resident, one run at a time):

```sh
python3 tests/next/bench-large-scene.py \
  --baseline <baseline-lusdcat> --candidate build_ninja/lusdcat \
  --manifest full45-parse.json --output /tmp/r-full45 \
  --repeat 2 --warmup 1 --max-load 20 --min-available-gib 10 \
  --max-process-swap-mib 512 --max-rss-growth 0.05
```

with `full45-parse.json` = one case `{mode: "parse", input:
benchmark_fullLOD2.usdc, args: ["--max-crate-elements", "300000000",
"--max-memory-mib", "49152"]}`.
3,014,654,530 B, 61,833,354 specs): it declares **176,656,972 fields**, so the
default `--max-crate-elements` and even 100M are rejected with the
"raise the crate table limit" hint; at 300M it then needs a larger memory budget
than 16 GiB (`Compressed integer workspace exceeds cumulative allocation
budget` after 64 s / 7 GB), and at `--max-memory-mib 49152` it loads in 357 s
with **84.8 GiB peak RSS** and zero swap. A paired full45 run is therefore a
one-at-a-time, ~85 GB-resident, multi-hour job (the §8 record has the same
scene's compose+write at 79–114 min); it is not something to run alongside any
other load on this workstation.

## 10. Compose/write round 2: faults, writer per-field costs, walk spec probes (2026-10-25)

Starting point: the §8/§9 branch state (`e3ac6e4a6` plus the fault-counter
instrumentation), full45 at load+compose 152–166 s, crate write 212–230 s,
process 370–396 s, peak RSS 75–79 GiB. Everything below is byte-neutral and was
checked against the district rewrite (`e049a40f…`), the composed district
(`98619c40…`) and full45 (`a0175bc1…`) hashes.

### 10.1 What the profile said

`perf record --call-graph dwarf` on the current binary (district rewrite and
district compose, `/dev/null` sink), before any change:

- **25–30% of all CPU was kernel time**: page faults plus `madvise`. The full45
  compose took 68 M minor faults and 260 s of system time out of ~1030 s CPU;
  `fields_specs` alone took 10.9 M minor faults. The host runs THP in
  `madvise` mode and jemalloc with default decay, so every freed-then-reused
  range re-faults 4 KiB pages.
- `fields_specs` is many small per-property costs, not one: inside
  `EmitPrimProperties` (57% of `BuildFieldsAndSpecs`), value encoding 21%
  (half of it hashing passthrough blocks for dedup), property-path interning
  19% (`FindPathKey` + `AddPathRec`: random probes into three large arrays),
  time-sample fields 12%, the `property_type_name` cold-map lookup 7.7% plus
  the string hash of the type name it returns, the per-prim `emitted` hash set
  4.7%. Per prim: literal field-name hashing, `primChildren` rebuilt from a
  10 M-entry `parent -> children` string map, and in the composed case
  `release_value_payloads` 14% (90% of it kernel purge time).
- The unattributed ~30 s of the serial structure walk is
  `IsInstanceableSources` -> `SpecsFor` -> `Specs`: the walk hashes and probes
  the site of every `Src` of every prim, and ~90 M of the ~100 M Srcs on full45
  have no specs (they are dropped later by `CompactOpinionSources`).
  `ComposedActive` ran twice per prim, `prototype_of` was looked up by string
  for every prim, and `Path::append_child` re-validated every child name
  (`unicode_xid_is_continue`, 1.5% of the process).
- `FlattenInstancesExtracted` and `Layer::finalize` each build a 10.2 M-entry
  `unordered_map<string,uint32_t>` over all prims.
- The compose fill could not be attributed by `perf` (worker stacks unwind to
  `[unknown]`); by reading, `CopyLocalOpinions` built a `std::string` per
  property per source, re-interned it in `set_property_type_name` /
  `ensure_property_meta` / `compose_connections`, and allocated a `ColdData`
  (13 `unordered_map`s) for nearly every composed prim just to hold property
  type names.

The previously planned "field-block passthrough with token remap" (W1 in the
older plan) was dropped: the writer emits fields in its own canonical order
with its own reps, the Pixar-written district crates use another order, and
`PrimSpec` keeps no handle to the source spec, so a passthrough could not
reproduce today's bytes.

### 10.2 Changes

**A. Optional jemalloc configuration compiled into the CLI tools**
(`src/next/tools/jemalloc-conf.cc`, CMake `LIGHTUSD_NEXT_JEMALLOC_CONF`,
**default empty**). jemalloc reads the `malloc_conf` symbol before
`/etc/malloc.conf` and `MALLOC_CONF`, so both still override it; the TU is only
compiled into `lusdcat` when the allocator found is jemalloc and the variable
is non-empty.

Transparent huge pages (`thp:always`) were measured as the candidate default
and **rejected as a default**. Alternating `MALLOC_CONF` runs of the district
compose (two repeats each, `/dev/null` sink, pre-B binary):

| conf | process | sys | minor faults | peak RSS |
|---|---:|---:|---:|---:|
| default | 28.9–30.2 s | 33–36 s | 7.8–8.4 M | 21.6–22.5 GB |
| `thp:always` | 22.5–23.7 s | 13–14 s | 0.5–0.7 M | 18.5–18.6 GB |
| `thp:always,dirty_decay_ms:-1,muzzy_decay_ms:-1` | 22.2–24.5 s | 12.7–12.9 s | 0.28–0.38 M | 19.3–20.1 GB |
| `thp:always,background_thread:true,dirty_decay_ms:30000` | 23.3–25.9 s | 14.9–15.6 s | 0.66–1.0 M | 18.6–20.3 GB |

Without THP, longer decay or a background purge thread did not help (A–E
binary, district: 24.6–28.3 s vs 19.4–19.9 s with `thp:always`). But at full45
scale THP lost, back to back on the same A–E binary (both runs touched swap,
so these are directional, not a timed claim):

| full45 → USDC `/dev/null` | `thp:always` | THP off |
|---|---:|---:|
| process wall | 401.6 s | 327.0 s |
| load+compose / write | 192.6 / 182.8 s | 134.6 / 166.0 s |
| serial `structure` | 90.1 s | 47.4 s |
| system time | 304 s | 211 s |
| peak child swap | 2.1 GB | 1.3 GB |

With the host's THP `defrag=madvise`, a huge-page fault on a `MADV_HUGEPAGE`
range compacts synchronously; at a 75 GiB resident set on a fragmented host
those stalls land on the serial walk. Since full45 is the target, THP stays
opt-in (`-DLIGHTUSD_NEXT_JEMALLOC_CONF=thp:always` or `MALLOC_CONF=thp:always`
at run time) for working sets well below free memory. The measurements in
§10.3 labelled "A + B" and "A–D" include `thp:always`; the final B–E pair does
not.

**B. Writer `fields_specs`** (`crate-writer-fields.inc`, `-properties.inc`,
`-tables.inc`, `-impl.inc`):

- B1. The declared property type name is an interned `TypeNameTable` id on
  `PropSlot` (`type_name_id`, 12 -> 16 bytes per slot) instead of a
  `ColdData::prop_type_names` string map entry. `set_property_type_name(PropNameId,
  TypeNameId)` is the hot setter; the string overloads remain and a name set
  before the slot exists (the USDA parser declares the type ahead of the value)
  waits in the cold map and is adopted by `add_property` /
  `add_property_slot`; a full type table (u16) also falls back to the map.
  The writer maps `TypeNameId -> token` through a vector, so the per-property
  cold-map lookup and type-name string hash are gone; the crate reader interns
  each attribute name once per record.
- B2. The next property's / next prim's path-table slot is prefetched one
  iteration ahead (`PrefetchPathSlot`).
- B3. The per-prim `emitted` hash set is replaced by the adjacent-duplicate
  check a name-sorted slot index allows (the set is kept for unsorted indices).
- B4. `primChildren` comes straight from `child_indices` when a one-pass integer
  check proves the layer fully linked (every non-root, non-holder prim linked
  exactly once from its path parent), using prim-name tokens recorded by
  `BuildTables`; the 10 M-entry string map is only built when the check fails.
- B5. Field-name literals (`active`, `properties`, `primChildren`, `custom`,
  `connectionPaths`, `targetPaths`, `timeSamples`, ...) are interned at first
  use and cached; property-name tokens are recorded once in `BuildTables`
  (which also stops building a `std::string` per slot) and reused for the
  `properties` field.

**C. Serial structure walk** (`pcp/cache.cc`, `cache-storage.cc`,
`cache-arcs.cc`, `cache-internal.hh`, `prim/path.cc`):

- C1. `Src::spec_state` (unknown / none / some) is stamped by `ExpandArcs` when
  a source is expanded (it already probes `Specs()` there) and by `SpecsFor`
  on resolution; it resets on copy and survives a move, so it crosses
  `MergeSources`' vector steal and `CompactOpinionSources`. `SpecsFor` answers
  "none" without hashing the site, and `CompactOpinionSources` drops such
  sources without a memo. Same `discarded_sources` count before and after.
- C2. `ComposedActive` is computed once per prim; `RegisterInstance` reports
  whether the prim was instanceable so the `prototype_of` string lookup only
  runs for those; work items are moved off the stack; child paths use
  `Path::append_child_unchecked` (names come from parsed spec names).

**D. Compose fill** (`composition/composition-merge.cc`, `layer/prim-spec.*`):
`CopyLocalOpinions` is id-keyed: no `std::string` per property per source,
`PropNameId` overloads for `connection*`, `set_spline_source`,
`set_raw_default_source`, `set_array_edit`, `set_property_type_name`; the
string-keyed relationship-form check and `relationship_names()` only run when
the prim records relationship data (`has_relationship_data()`). With B1 most
composed geometry prims no longer allocate a `ColdData` at all.

### 10.3 Measurements

All runs: `tests/next/bench-large-scene.py`, `/dev/null` sink, warm inputs,
one warmup plus three alternating pairs, `--max-load 24 --min-available-gib 8
--max-process-swap-mib 512 --max-rss-growth 0.05`, baseline
`lusdcat-base-7bc83b3b5` (medians; RSS gate = largest peak ratio ≤ 1.05).
`MALLOC_CONF` is not set in the harness: the compiled-in default is part of
the candidate.

**A + B (allocator conf + writer), district** — compose
`/tmp/lightusd-scene-p0/benchmark_mine_full.usda` → USDC and rewrite of the
composed district crate:

| district compose → USDC | baseline | candidate | ratio |
|---|---:|---:|---:|
| process wall | 36.95 s | 21.46 s | 0.58 |
| system time | 34.1 s | 12.6 s | 0.37 |
| minor faults | 8.21 M | 0.42 M | 0.05 |
| peak RSS (median) | 20.3 GiB | 16.9 GiB | 0.83 (max ratio 0.83, pass) |
| `warm` / `structure` / `compose` | 3.56 / 5.88 / 5.47 s | 2.69 / 3.68 / 3.07 s | 0.76 / 0.63 / 0.56 |
| `build_stage` / `reclaim` | 13.59 / 2.68 s | 8.72 / 1.62 s | 0.64 / 0.60 |
| `tables` / `fields_specs` | 0.63 / 10.79 s | 0.36 / 5.88 s | 0.58 / 0.55 |
| `structural_payloads` / `PATHS` | 2.00 / 1.29 s | 1.17 / 1.04 s | 0.58 / 0.81 |

| district rewrite → USDC | baseline | candidate | ratio |
|---|---:|---:|---:|
| process wall | 19.28 s | 12.91 s | 0.67 |
| system time | 8.2 s | 2.2 s | 0.26 |
| minor faults | 3.22 M | 0.23 M | 0.07 |
| peak RSS | 6.65 GiB | 6.21 GiB | 0.93 (pass) |
| crate `BuildStage` (read) | 4.51 s | 3.19 s | 0.71 |
| `tables` / `fields_specs` | 0.43 / 6.37 s | 0.23 / 4.17 s | 0.53 / 0.65 |
| `structural_payloads` / `PATHS` | 1.82 / 1.17 s | 1.15 / 0.90 s | 0.63 / 0.77 |

This pair ran with `thp:always` compiled in. The compose-side phases move in
this pair only through the allocator;
their own changes (C, D) are measured separately below. The baseline's
compose runs were 30.2 / 38.7 / 37.0 s (its spread is the host's THP
compaction and purge behaviour under the 8 M faults), the candidate's
21.9 / 21.5 / 21.4 s.

**E. Flatten / finalize** (`pcp/cache.cc`, `layer/layer.{hh,cc}`): the walk
records the composed-layer index of every prim `RegisterInstance` accepts, so
`FlattenInstances*` find prototypes and members without a path->index map over
all prims (a miss still falls back to building it once); the
`/Flattened_Prototype_N` clash check only scans root prims (the name is a root
path). The composed output layer opts into a deferred path index
(`Layer::set_lazy_path_index`): `finalize()` skips the 10 M-entry map and the
first `prim_at_path` / `index_at_path` / mutator builds it under a lock. Source
layers keep the eager index. District compose: `flatten` 0.47 -> 0.08 s,
`finalize` 0.27 -> 0.07 s.

**A–D, district compose and reference scene** (with `thp:always` compiled in; same protocol; the district baseline was
faster in this pair than in the A+B pair, 26.8 vs 37.0 s median, so compare
ratios within a pair only):

| case | baseline | candidate | ratio | max RSS ratio |
|---|---:|---:|---:|---:|
| district compose → USDC, process | 26.8 s | 19.9 s | 0.74 | 0.75 |
| ↳ `warm` / `structure` / `compose` | 3.09 / 3.88 / 3.61 s | 2.38 / 2.41 / 2.27 s | 0.77 / 0.62 / 0.63 | |
| ↳ `reclaim` / `fields_specs` | 1.78 / 8.21 s | 1.24 / 6.64 s | 0.70 / 0.81 | |
| reference scene compose → USDC (8 threads), process | 14.8 s | 9.3 s | 0.63 | 0.99 |
| ↳ `build_stage` / `fields_specs` | 3.70 / 5.52 s | 2.76 / 3.46 s | 0.75 / 0.63 | |
| reference scene compose → USDA (8 threads), process | 15.4 s | 8.5 s | 0.55 | 0.98 |

Byte identity of the A–E candidate: district rewrite `e049a40f…`, composed
district `98619c40…`, reference scene USDC `267d237a…` (1,815,093,161 B, same as the
baseline binary on this snapshot), merged ALab USDC `2f7176c8…`
(732,541,268 B) and USDA `f764d58c…` (4,566,009,415 B). Native `ctest` 67/67
and standalone `next` Debug 71/71 pass after each phase.

**B–E, full45** (no compiled allocator conf; `--repeat 2 --warmup 1`, all runs
zero swap). The pair's last baseline run was refused by the load gate (an
unrelated job started), so the baseline column is one measured run; its warmup
took 409.8 s.

| full45 compose → USDC | baseline | candidate (median of 2) | ratio |
|---|---:|---:|---:|
| process wall | 387.8 s | 320.9 s (333.6 / 308.2) | 0.83 |
| user / system CPU | 766.9 / 244.6 s | 652.2 / 215.9 s | 0.85 / 0.88 |
| peak RSS | 75.4 GiB | 73.0 GiB | 0.97 |
| `build_stage` (load+compose) | 145.8 s | 125.5 s | 0.86 |
| `warm` / `structure` / `compose` | 47.9 / 52.5 / 48.7 s | 48.5 / 50.9 / 48.9 s | 1.01 / 0.97 / 1.00 |
| `flatten` / `finalize` | 12.2 / 7.4 s | 1.6 / 1.0 s | 0.13 / 0.14 |
| `tables` / `fields_specs` | 8.0 / 120.1 s | 6.1 / 84.2 s | 0.75 / 0.70 |
| `path_tokens` / `structural_payloads` / `PATHS` | 18.3 / 29.9 / 19.4 s | 18.9 / 27.7 / 19.7 s | 1.03 / 0.92 / 1.02 |

What did **not** move at full scale: `structure`, `warm` and the `compose`
fill. The spec-state byte (C1) removes the per-Src site hash, but on full45 the
walk's cost is elsewhere (the per-prim `PrimSpec`/path/child-name work and
`RegisterInstance`); the id-keyed merge (D) did not shorten the fill. Both are
kept (they do strictly less work and are byte-neutral), but their benefit is
only visible on the district. The full45 win is the writer (B: −36 s) and the
index-free flatten/finalize (E: −17 s).

**F. `path_tokens`** (`crate-writer-tables.inc`). `RegisterPathElementTokens`
split every one of the 62 M paths and hashed each component as a fresh
`std::string`. It now skips components provably interned already: the stem
components before the longest `/`-bounded prefix shared with the previous
record's stem, and property names whose `(pointer, length)` view into the
`PropNameTable` was split before; components are interned from the view
without a copy. `SynthesizeAncestorPaths` answers parent probes that fall
inside the ancestor range the previous walk proved present without hashing the
stem. Both are exact: interning a present token and probing a present path
were no-ops. District rewrite lap 1.38 -> 0.56 s; full45 lap 18.9 -> 9.6 s (single runs,
host load ~22 from an unrelated job, so indicative only). That full45 run
(B–F) took 304.9 s at 72.8 GiB peak and reproduced `a0175bc1…`
(3,014,654,530 B).

**Token-order bug found by the fixture sweep (fixed).** B5 initially interned
relationship names and time-sample-only property names (not slots, so not
recorded by `BuildTables`) while emitting their specs, earlier than the
`properties` field that used to intern them. That moved their TOKENS
position: 2 of the 227 `tests/usdc` fixtures
(`aousd-unknown-property-metadata`, `rel-inherits-none-001`) differed in
USDC, while every scene hash still matched. `PropNameToken` now never interns;
unrecorded names are interned at the `properties` field as before. After the
fix: 227/227 fixtures byte-identical in USDA and USDC, district rewrite
`e049a40f…`, composed district `98619c40…`, reference scene `267d237a…`, merged ALab
`2f7176c8…` / `f764d58c…`, baked 1004 USDC `a8d08517…`.

Verification for this round: native `ctest` 67/67 and standalone `next` Debug
71/71 after every phase and after F; the fixture sweep above; full45
`a0175bc1…` with both the A–E and the final B–F binaries. Not yet run: TSan
and ASan/UBSan trees for the `Src::spec_state` stamping (C1) and the lazily
built `Layer` path index (E), and a complete full45 pair for B–F on a quiet
host.

### 10.4 Commits `ee72a9779` and `4a0aa8688` (previously unrecorded)

**`ee72a9779` — "Resolve repeated lazy-array ranges early; parallel delta encoder".**
Two byte-neutral crate-writer changes:

- The streaming writer's `(source identity, offset, length) -> output offset`
  cache is now a flat open-addressed table instead of an `unordered_map`, and
  `TryPassThrough` probes it before building a `DataBlock`. Instance copies of
  one source array resolve without a `shared_ptr` copy or node lookup; the
  answer is the same offset `StreamInternBlock` returns from that table.
  District rewrite `fields_specs` ~5.0 → ~4.8 s.
- `EncodeDeltaU32` uses a chunked parallel encoder for columns of ≥ 4 M
  entries in threaded builds: per-chunk deltas and histograms (merged; the
  common delta is the most frequent, ties to the smallest), per-chunk value
  sizes, a prefix sum for write positions, and chunks aligned to 4 elements so
  no two share a code byte. Wide deltas are counted in an open-addressed
  table. Neutral on the district, where `structural_payloads` is bound by
  first-touch page faults rather than encoding; the full45 measurement was
  pending at commit time.

`test_lazy_array` checks the parallel encoder against the serial C encoder
(mixed widths, an exact most-common tie, wide deltas, counts not a multiple
of 4). District rewrite/compose, reference scene, merged ALab and baked 1004 hashes are
unchanged; `tests/usdc` fixtures are below the parallel threshold and were
byte-identical after the range-table change.

**`4a0aa8688` — "Speed up full45 compose: warm merge, walk, merge fast path,
compaction".** Byte-neutral compose-side changes for the all-district benchmark scene
flatten:

- Warm merge: worker cache entries are drained with the hashes their table
  already stores and their keys moved into the main `SrcCache` (no rehash or
  copy of ~10 M path keys); the next entry's slot is prefetched.
- Structure walk: one path per work item instead of two identical copies; the
  path is moved into the `PrimSpec` and read back from the stored prim;
  `IsInstanceableSources` is evaluated once per prim
  (`RegisterInstanceKnownInstanceable`).
- `ComposeChildNames` dedups with a short linear scan / `string_view` hash set
  instead of a `std::set` node per child.
- `CopyLocalOpinions`: when the composed prim has no slots yet and the source
  index is name-sorted, skip the per-property target lookup (an O(n) scan of
  the unsorted target index) and reserve the slots; repeated ids still look
  up, as before.
- Source compaction before the fill runs over distinct source vectors in
  parallel (~90 M `Src` destructors); counts are unchanged.

full45 (single run, quiet host): load+compose 109.5 s, compact 14.1 → 2.9 s,
compose fill 48.9 → 35.9 s, process 295 → 257 s; output `a0175bc1…`
(3,014,654,530 B). District compose `98619c40…` unchanged; pcp/composition/
flatten ctests pass.

### 10.5 Serial spec-memo priming (this pass)

Adopted each warm worker's `StackSpecCache` entries into the main cache during
`MergeSources` (remapping `layer_identifier` pointers to the copied main
stack's identifier vector by index, and adopting any worker-local stack via
the existing `AdoptStack` path). The fill's priming loop (`SpecsFor` per
non-variant, non-suppressed `Src`) then resolves as an open-addressed cache
hit instead of re-probing every layer of every worker-resolved site.

Result (paired full45 compose-only, baseline `lusdcat-H` vs candidate,
warmup 0, repeat 2, zero swap): elapsed median ratio 0.969 (≈3.1% faster),
peak RSS max ratio 1.000 (passes the 1.05 gate). Single full45 run reproduced
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`
(3,014,654,530 B); district compose `98619c40…` unchanged; next tests pass.

### 10.6 Parallel `fields_specs` build with an ordered commit (this pass)

Lever W2 from the resume memo: the `fields_specs` lap (prim/property specs and
their VALUE blocks) is chunked over prims and encoded on worker threads. Output
is byte-identical; the design reproduces the serial writer's first-use order
exactly, which is what the TOKENS/STRINGS/PATHS tables, the block-dedup
diagnostics and the `required_version_minor_` upgrade all depend on.

**Worker encoding** (`crate-writer-impl.inc`, `-tables.inc`,
`-values.inc`, `-properties.inc`, `-fields.inc`). After the serial pre-phase
(pseudo-root spec, prescans, `BuildTables`, `Layer` path-index warm, PropName
`intern` pre-warm + `publish_snapshot`, `w_base_{tok,str,path}` capture) the
layer's prims are cut into contiguous chunks of 1024 (only when
`nprims >= 8192` and `ResolveThreads() > 1`; `CrateWriteOptions::parallel_fields`
turns it off). Each worker owns a `WorkerCtx` and encodes its chunk into
private buffers:

- tokens/strings: a private table plus ONE ordered event stream; workers never
  read the live global tables (the commit thread mutates them), so a worker
  miss always logs `(kind, local id, text)` with the text owned by the ctx.
  Ids are provisional, offset by the phase-start base, so `v < base` is already
  final and `v >= base` remaps through the chunk's `fin_tok` / `fin_str`.
- paths: a private open-addressed table; misses log a `PathEvent` with the
  precomputed hash. Transient path text (e.g. relationship targets and prop
  paths built per prim) is copied into ctx storage up front and re-split from
  that copy, so the event never holds a view into caller scratch.
- blocks: an intern/push op log with the content digest precomputed on the
  worker. Field `ValueRep`s and spec path indices are provisional (op index+1
  / path id); inline Token/AssetPath (`token` domain) and String (`string`
  domain) payloads are provisional too.
- fields/fieldsets/specs: chunk-local columns (local field ids), merged at
  commit by rebasing with the field/fieldset counts already emitted.

**Ordered commit** (main thread, `CommitChunk`). Chunks commit strictly in
prim order as they finish; the owner worker parks (it may not `Reset` its ctx)
until its previous chunk is committed, which bounds transient memory to W
chunks. Per chunk the replay is: intern events in event order (token ids and
string ids come out in the exact serial first-use order) → path events
(`FindPathKey` probe + `AddPathRec`, owned text re-copied into `path_owned_`,
so `paths_` only ever holds views that outlive the write) → block ops (fix
provisional ids inside tagged blobs to FINAL values, then replay the exact
`InternBlockWithHash` / `StreamInternBlockWithDigest` / push semantics, so
`blocks_deduped_` and the VALUE layout match serial) → fields (token column +
rep remap) → fieldsets (flat-id rebase) → specs (`path_index` remap,
`fieldset_index` rebase) → counter merge (`required_version_minor_`,
passed-through/re-encoded arrays).

Blob-internal provisional ids are fixed by a per-tag walker (`BlockBlobTag`:
token/string vectors, token/string/path list ops, variant selection map,
relocates, dictionaries, time-sample headers, splines, reference/payload item
lists, scalar path expressions, unregistered nested recs; untagged blocks use
the encoder-recorded `embedded_value_rep_offsets` list). The fix-up runs BEFORE
the dedup probe so identical-block matching compares final bytes exactly like
the serial writer. A worker-precomputed digest covers the provisional bytes, so
the commit re-hashes a block only when the walker actually modified it (large
pass-through geometry blocks stay digest-free on the main thread). Worker
errors abort with a clean serial fallback as long as no chunk has been
committed (the global tables are still pristine); afterwards they fail the
write. Per-worker `FieldToken` / type-name caches and table snapshots keep
workers off the main thread's vectors.

**Results** (paired, baseline `lusdcat-H` vs candidate, quiet host, warmup 0,
repeat 2, zero process swap; two full pairs measured 0.83 / 0.82 wall and
0.47 / 0.46 `fields_specs`):

| case | metric | baseline | candidate | ratio |
|---|---|---:|---:|---:|
| full45 compose → USDC | process wall | 297.5 s | 243.9 s | **0.82** |
| | `fields_specs` | 88.2 s | 40.4 s | **0.46** |
| | peak RSS | 73.1 GiB | 73.3 GiB | 1.002 |
| ALab merged → USDC | wall | 1.42 s | 1.27 s | 0.89 |
| | `fields_specs` | 0.47 s | 0.40 s | 0.84 |
| | peak RSS | 0.95 GiB | 0.96 GiB | 1.009 |
| reference scene compose → USDC | wall | 13.7 s | 11.4 s | 0.83 |
| | `fields_specs` | 5.18 s | 2.45 s | 0.47 |
| | peak RSS | 4.27 GiB | 4.29 GiB | 1.004 |

District compose drops its `fields_specs` lap from 7.3 s to 3.5 s and its wall
from 24.4 s to 17.0 s (single runs). All RSS ratios pass the 1.05 gate and no
run touched swap. A staged-mode check (`WriteLayerToMemory`, district layer,
parallel vs serial fields) produced byte-identical crates
(`98ad30c3…`, 579,974,418 B), so the staged and streaming branches agree.

**Byte identity**: full45 `a0175bc1292f…` (3,014,654,530 B), district compose
`98619c40…` (580,261,850 B), district rewrite `e049a40f…` (578,776,001 B),
reference scene `267d237a…` (1,815,093,161 B), baked 1004 `a8d08517…`
(3,537,903,797 B) and merged ALab `2f7176c8…` (732,541,268 B) all reproduce. The 227-fixture `tests/usdc` sweep (`--rewrite-layer`, USDA and
USDC) is identical to the baseline binary, including a run with the parallel
threshold forced to zero so every fixture exercises the worker path; the full
native ctest suite (67 tests) passes.

A `LIGHTUSD_NEXT_ENABLE_THREAD=OFF` native build also compiles again: the
compactor's thread-count computation in `cache.cc` sat outside the thread
guard. Its lusdcat reproduces the district hash and the 227-fixture sweep.

**What the commit costs**: on full45 the commit pass is ~94% of the parallel
section, so the win is bounded by the serial parts of the lap (path-table
insertion for 61.8 M specs, VALUE-section streaming, per-block fix-ups). The
per-chunk `specs_.reserve(size + delta)` that made the first version quadratic
(libstdc++ reserves the exact request) was removed; the remaining per-chunk
merges use `insert`/`push_back` geometric growth.

### 10.7 Writer serial tail: pipelined commit, PATHS / path_tokens, pre-phase (round 3)

After W2 the full45 write phase was dominated by main-thread work: the
`CommitChunk` replay (~94% of the parallel section), the `PATHS` section
(parent lookups, token lookups), `path_tokens` (ancestor synthesis, path
element tokens) and the serial `fields_specs` pre-phase. Every change below is
byte-neutral; each keeps the serial writer's first-use order.

**Pipelined commit** (`crate-writer-fields.inc`). `CommitChunk` is split into
stages over disjoint writer state, each on its own thread and each strictly
in chunk order:

- A1: token/string intern events (token and string tables);
- A2: path events (path table, main thread, owns error handling);
- B: block ops (id fix-up, digest redo, `StreamInternBlockWithDigest` / push);
- C: fields, fieldsets, specs, counters.

A1(k) and A2(k) follow chunk readiness; B(k) waits for A1(k), A2(k) and
B(k-1); C(k) for B(k) and C(k-1). A worker parks until C is done with its
chunk, so transient memory stays at W chunks. The fin vectors moved into
`WorkerCtx` (capacity reused across chunks). With the commit pipelined,
encode throughput mattered again: the worker cap is 16 for layers with at
least 256 chunks (~262 K prims; district `fields_specs` 3.1 → 2.5 s going from
8 to 16) and stays 8 below that, because every worker holds up to one encoded
chunk — a roughly fixed cost that showed up as +~50 MB peak RSS on merged ALab
(~24 chunks) with 16 workers.

**Cheaper path probes.** `path_slots_` entries are 64-bit: record index + 1 in
the low half and the record hash's high 32 bits as a fingerprint, so probe
chains no longer load `path_hashes_` / `paths_` until the fingerprint matches.
Because a chain is now walked in the slot array alone, the table is sized for
load ≤ 3/4 instead of ≤ 1/2, so the 64-bit slots cost no more memory than the
old 32-bit ones (reference scene: 8 M × 8 B instead of 16 M × 4 B; at load ≤ 1/2 they
had added 64 MB there, and full45 keeps 128 M slots at load 0.48).
Stage A2 prefetches the slot 8 events ahead; stage B prefetches the stream
dedup bucket 16 ops ahead and its first chain entry 8 ahead, plus the
source-range slot of pass-through blocks (district B −9%). `PathEvent` lost its
`std::string owned_text` copy: it records the index of the ctx `path_store`
string its views point into, and A2 probes with the existing views and, only
on a miss, adopts that string into `path_owned_` by move (no copy, no
allocation).

**`fields_specs` pre-phase.** `ChildrenFullyLinked` runs both passes over
parallel slices (atomic per-child link counts; the answer is an AND, so it is
order-independent). The Layer path index is built only when some prim path has
a variant component — the only case in which workers probe prims by path
(`Layer::ensure_path_index` is double-checked under a mutex, so an unexpected
probe would still be safe). District pre-phase 0.6 → 0.15 s.

**`PATHS`** (`crate-writer-sections.inc`). The parent/element loop (a
read-only `FindPath` per record) and the path/token/jump array build run over
parallel slices (`ParallelRanges`, new in `crate-writer-impl.inc`); token ids
are looked up with `token_to_index_.find` (all element tokens are registered
by `RegisterPathElementTokens`; a miss would redo the build serially with
`InternToken`).

**`path_tokens`** (`crate-writer-tables.inc`): a parallel read-only filter,
then the existing ordered pass over the flagged records only. Presence only
grows during the ordered pass, so an unflagged record would add nothing.
`SynthesizeAncestorPaths` flags a record only when its prim part (property) or
its stem's parent is missing — the ordered walk stops at the first present
ancestor, so two probes decide it. `RegisterPathElementTokens` flags records
with any component missing from the token table (same split + LCP skip as the
ordered pass); an unflagged record still advances the LCP reference (all its
components are interned).

**Results** (bench harness vs `lusdcat-H`, warmup 0, zero process swap;
candidate `lusdcat-R3b` = this round):

| case | metric | baseline | candidate | ratio |
|---|---|---:|---:|---:|
| full45 → USDC (repeat 2) | process wall | 267.5 s | 188.0 s | **0.70** |
| | `fields_specs` | 82.0 s | 17.7 s | 0.22 |
| | `PATHS` | 17.4 s | 6.8 s | 0.39 |
| | `path_tokens` | 8.8 s | 2.4 s | 0.27 |
| | peak RSS | 73.1 GiB | 73.3 GiB | 1.003 |
| ALab merged (repeat 3) | wall | 1.19 s | 1.07 s | 0.90 |
| | peak RSS | 0.958 GiB | 0.951 GiB | 0.994 |
| reference scene compose, 8 threads (repeat 3) | wall | 12.6 s | 8.3 s | 0.66 |
| | `fields_specs` | 4.59 s | 1.39 s | 0.30 |
| | peak RSS | 4.27 GiB | 4.23 GiB | 0.991 |

A full45 pair of the first candidate (before the two RSS fixes below) against
the W2 binary measured 0.84 wall, 0.997 RSS. Process wall still includes the
~32–38 s `Stage` teardown (the harness does not pass `--fast-exit`).

Byte identity: full45 `a0175bc1…`, district compose `98619c40…`, district
rewrite `e049a40f…`, staged district `98ad30c3…` (parallel = serial), the
no-thread build's district `98619c40…`; the 227-fixture sweep (USDA + USDC) is
identical to `lusdcat-H`, also with every parallel threshold forced to 0
(chunk size 3); a TSan build with forced thresholds is race-free on all 227
fixtures; native ctest 67/67.

**RSS bisect.** The first candidate failed the gate on merged ALab (1.054) and
grew reference scene by +~140 MB over W2. An RSS-vs-lap trace showed a constant
+80 MB with jemalloc decay disabled (64 MB = 64-bit slots at load ≤ 1/2, the
rest small buffers) and +~140 MB with default decay; ALab's excess vanished at
`--write-threads 8`. Fixes: the ≤ 3/4 slot sizing and the 8-worker cap for
small layers (both above). After them reference scene is below W2 and ALab at H.

**Rejected in this round**

- `MADV_HUGEPAGE` on the path slot array and the stream dedup / source-range
  tables: district stage A −20%, but on full45 no stage-A gain and B /
  `structural_payloads` got slower (THP `defrag=madvise` direct compaction on
  the doubling tables), consistent with the earlier `thp:always` result.
- A parallel relationship-name pre-warm (slice-local name collection, ordered
  intern): its slices finish in ~18 ms on the district, but the call returns
  after ~0.27 s — the threads' exit after allocation-heavy work costs more
  than the 0.10 s serial loop (cause not pinned down). Kept serial.
- Coalescing small VALUE blocks before the sink (plan item 1d): the file sink
  is already a `BufferedFileSink`.


### 10.8 Sharded path table + parallel stage A2 (round 4)

After round 3, commit stage A2 (path events) was the critical stage on
full45 (~11.7 s busy for 61.8 M paths). Changes (byte-neutral):

- `path_slots_` became 16 independent shards (`path_shards_`), selected by hash
  bits 28..31 (disjoint from the slot-index bits and the 32-bit fingerprint);
  `paths_` / `path_hashes_` stay global, so record ids keep the serial
  first-use order. Each shard grows on its own (re-placing only its entries).
- Workers bucket their chunk's path events by shard (`BucketPathEvents`).
  Stage A2 then (1) probes all events, shards in parallel, (2) appends the
  misses as new records in event order (serial), (3) inserts the new records,
  shards in parallel. A chunk's events are distinct keys (the worker's table
  dedups them), so probes only see earlier chunks' inserts and the ids equal
  the one-by-one replay. The team (`ShardTeam`: commit thread + 3 helpers,
  `min(4, threads)`) is woken twice per chunk.

full45 stage busy times: A2 11.7 → 7.1–8.6 s (probe 2.0, serial append 4.3,
insert 1.2; team of 8 is barely better than 4). The append is not CPU in the
loop (perf: <0.1% of samples) — most likely first-touch faults on the 2.5 GB
`paths_` / `path_hashes_` arrays contending with the workers' faults;
pre-touching at setup only moved the cost (rejected).

**No end-to-end gain yet**: stage B (block ops, ~10.2–10.8 s) is now the
critical stage. Paired vs `lusdcat-R3b` (repeat 2/3, zero swap): full45 wall
0.988 (`fields_specs` 18.5 → 17.8 s), RSS 1.000; ALab 1.009 / 1.005;
reference scene 1.036 / 0.995. Kept as the prerequisite for a parallel stage B.

Stage B breakdown (sampled, full45): 63.3 M ops, 22.8 M pass-through refs,
10.2 M raw pushes, 11.3 M blocks re-hashed after the fix-up; fix-up ~29%,
re-hash ~22%, dedup + stream ~48%. Nearly all re-hashed blocks carry embedded
ValueReps (time samples, dictionaries, ...), whose fix-up needs earlier
blocks' final offsets (`fin_block`) and so cannot leave B. Rejected: a
worker-side id fix-up step (`CommitStageF`, ids + digest redo for blocks
without embedded reps + token/spec remaps) — B unchanged (1.28 → 1.21 s
district, 10.2 → 10.8 s full45), reverted.

**Data race fixed** (present since W2): workers read
`required_version_minor_` as a fast-path guard in `EncodeValue` /
array-edit / path-expression encoding while the commit (now stage C) writes
it. Harmless for the output (the final value is a max) but undefined
behaviour; TSan caught it once on `suzanne.usdc` with forced thresholds. The
guards now call `WRequireMinor` unconditionally (it compares against the
worker's own ctx). Two further TSan sweeps (227 fixtures, forced thresholds)
are race-free.

Verification: district compose `98619c40…`, district rewrite `e049a40f…`,
no-thread build district `98619c40…`; 227-fixture sweep identical (normal and
forced-parallel); ctest 67/67.

### 10.9 Parallel streaming stage B, stage W, ctx pool (round 5)

Round 4 left commit stage B (block ops) critical on full45 (~10.5 s busy).
Changes (byte-neutral; staged mode keeps the serial `CommitStageB`):

- The streaming dedup and source-range tables are 16 shards each
  (`ShardedStreamDedup`, `ShardedSourceRanges`, by high hash bits); the API is
  unchanged for the serial callers.
- `FixOpBlobIds` takes a pass mask: `kFixIds` (token/string/path ids, need
  only the chunk's interned ids) and `kFixReps` (embedded ValueReps, need
  `fin_block`). The walk reads only counts/lengths, which neither pass
  rewrites, so split passes produce the same bytes.
- `CommitStageBStream` per chunk: P1 (parallel slices, `ShardTeam` of
  min(4, threads)) runs the id pass on every block and, for blocks without
  embedded reps, the digest redo and the probes of the global tables (which
  hold exactly the earlier chunks' entries). P2 (serial, op order) runs the
  embedded-rep pass and the remaining probes against chunk-local
  generation-stamped tables (`ChunkDedupTables`), and plans every write's
  offset exactly as `StreamRawBlock` aligns it (`stream_plan_pos_`). P3
  (parallel shards) inserts the chunk's new entries. Keys are unique (inserted
  only after a miss), so lookups do not depend on insertion order;
  `blocks_deduped_` counts as before.
- New stage W writes the planned blocks in op order and checks each offset
  against the plan (a mismatch, or `stream_plan_pos_ != value_stream_pos_`
  after the run, fails the write). C now follows W, so a ctx is reused only
  after its blocks are written.
- Workers spent more time parked than encoding (district: 8.6 s encode vs
  15.7 s parked, summed over 16 workers): each waited for C on its own ctx.
  The ctx is now taken from a pool (2 per worker when ≥ 256 chunks, else 1)
  and C returns it. A 4-per-worker pool parked less but was not faster on
  full45.

full45 stage busy times after the change: A2 8.9–9.5 s, B 6.9–8.5 s, W
4.4–5.4 s, C 6.7–7.5 s; worker encode ~8.5 s per worker.

**Results** (paired vs `lusdcat-R4`, repeat 2/3, zero swap): full45 wall
0.988 (`fields_specs` 18.3 → 15.8 s), RSS 1.001; reference scene wall 0.91
(`fields_specs` 1.49 → 1.12 s), RSS 1.026 (two ctx per worker; 1.015 vs
`lusdcat-H`); ALab 1.000 / 1.002. full45 `a0175bc1…`, district `98619c40…`,
district rewrite `e049a40f…`, staged district `98ad30c3…` (parallel =
serial), no-thread build district `98619c40…`; 227-fixture sweep identical
normally and with all thresholds forced to 0 (incl. the wide-pool gate); two
TSan sweeps with forced thresholds race-free; ctest 67/67.

### 10.10 PATHS payload built alongside the other structural payloads (round 6)

`WritePathsSection` became `BuildPathsSectionPayload` (own `StreamWriter`
buffer) and runs as a fifth task with the TOKENS / FIELDS / FIELDSETS / SPECS
payload builds; the section is appended at its usual position, so the file
layout is unchanged. It reads only the finished path and token tables. The
old fallback that interned a missing element token while writing PATHS (it
would have produced a token absent from the already-built TOKENS section)
is now a write error; no fixture or scene reaches it.

Paired vs `lusdcat-R5` (repeat 2/3, zero swap): full45 wall 0.961 (`PATHS`
lap 6.65 → 0.96 s — now just the append; `structural_payloads` 17.2 →
17.9 s), RSS 1.003; reference scene 0.94 / 1.001; ALab 0.98 / 1.009. Peak RSS stays in
compose (the write phase tops out at ~62 GB on full45). Hashes, sweep, ctest
and a forced-threshold TSan sweep unchanged/clean.

### 10.11 Delta encoder: wide-delta counting (round 7)

Per-task timing inside `structural_payloads` (full45, `RUSAGE_THREAD`):
FIELDSETS took 18.3 s and 2.5 M minor faults, all in `EncodeDeltaU32` of its
238 M-entry column (FIELDS: 177 M entries, 0.7 s). The parallel encoder from
`ee72a9779` counts deltas outside [-32768, 32767] exactly in per-chunk hash
tables and merges them serially; FIELDSETS interleaves `UINT32_MAX`
sentinels, so ~124 M wide deltas are almost all distinct (one into and one
out of each sentinel) — GBs of hash table and a 124 M-insert serial merge.

The most common delta (ties: smallest) only needs the wide values that can
reach the best small count `bs`. Pass 1 now feeds wide deltas to a per-chunk
Misra–Gries summary (64 counters): a value occurring more than W/65 times
overall (W = wide deltas) occurs more than its chunk's share/65 times in some
chunk and survives that chunk's summary. When `bs > W/65`, every
non-surviving wide value has count < `bs` (cannot win or tie), so an exact
count of the ≤ 512 survivors (one more parallel pass) gives the same choice;
otherwise the encoder counts every wide delta exactly, as before. Output is
unchanged by construction; `test_lazy_array` gained three parallel-vs-serial
cases (FIELDSETS shape, a winning wide delta, almost no small deltas), passing
in the Debug tree and under TSan with asserts on.

full45 (single run): FIELDSETS delta 17.5 → 1.5 s, `structural_payloads`
18.3 → 6.1 s (the PATHS payload, 6.1 s, is now the longest task), write
phase 43 → 35 s.

Paired vs `lusdcat-R6` (repeat 2/3, zero swap): full45 wall 184.1 → 171.4 s
(**0.931**), `structural_payloads` 19.7 → 5.9 s, RSS 1.001; reference scene 7.73 →
6.38 s (**0.825**), `structural_payloads` 1.67 → 0.40 s, RSS 1.002; ALab
1.009 / 0.997. Hashes: full45 `a0175bc1…`, district `98619c40…`, district
rewrite `e049a40f…`, reference scene `267d237a…`, merged ALab `2f7176c8…`, baked 1004
rewrite `a8d08517…`; 227-fixture sweep identical; ctest 67/67.

### 10.12 Parallel layer teardown and a phased warm merge (rounds 8–9)

**Teardown.** `lusdcat` destroys the composed stage before exiting (the
bench harness does not pass `--fast-exit`), and on full45 that took 32–38 s:
10 M `PrimSpec` destructors (metadata, property values, cold data) run
serially. `Layer::~Layer` now releases the prims of a layer with ≥ 64 K prims
in parallel slices (moving an empty `PrimSpec` into each slot), then the
vector frees the shells; shared payloads are refcounted atomically. Same
threshold as the parallel `finalize`, so small and per-worker layers stay
serial. District teardown 1.38 → 0.37 s, full45 32–38 → 5.6 s (the rest is
mostly the allocator returning pages). TSan with the threshold forced to 2
(227-fixture sweep) was race-free.

**Warm merge.** `MergeSources` folds each warm worker's resolved sources into
the main `SrcCache`. Its serial loop remapped every `Src` (~90 M on full45)
out of cold worker memory, adopting worker-local layer stacks / namespace
mappings lazily at first reference (which fixes the main tables' order).
Now, per worker:

1. (parallel) probe each key in the main cache — unchanged during the merge;
   a worker's keys are distinct — and record the first position (in the
   serial pass's event order) of every local stack / mapping reference;
2. (serial) adopt stacks and mappings in first-reference order (identical
   tables); mappings are moved out of the worker (discarded afterwards)
   instead of copied;
3. (parallel) build one remapped arc-chain / arc-sites object per distinct
   local pointer, then remap the `Src` records in place;
4. (serial) move keys and vectors into the main cache in index order.

The main cache index is reserved once for all workers' deltas before the
merges (it was regrown inside each merge: 3.6 s). Phases run in parallel when
a worker delta has ≥ 8192 entries (reference scene's ~35 K deltas benefit; ALab's tiny
ones stay serial, +~16 ms). full45 merge loop 28 → 11.2 s; ALab with the
parallel path forced was TSan-clean and reproduced its hash.

**Results** (paired vs `lusdcat-R7`, repeat 2/3, zero swap; R9 = before the
8192 threshold): full45 wall 155.1 → 122.3 s (**0.788**), `warm` 41.4 →
26.7 s, peak RSS 73.4 → 71.9 GiB (0.980); reference scene 0.937 (merge loop 0.38 →
0.48 s at the old 65 536 threshold, back to ~0.37 s with 8192); ALab 1.048
(1.05 → 1.10 s). Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene
`267d237a…`, merged ALab `2f7176c8…`; ctest 67/67; no-thread build OK.

**Next: the structure walk** (`structure` 41–46 s, serial). Its own counters
(sources ~5, child names ~3.5, register ~4.2, prototype map ~1.2, add_prim
~3.5, push ~2.5 s) cover about half; the rest is per-prim `PrimSpec`
construction/move and walk overhead. A parallel walk needs subtree-parallel
builds spliced in DFS order (prim index / parent / `fill_` / instance-index
remapping), coordination of instance registration, and thread-safe source
cache seeding for instanceable subtrees.

### 10.13 Structure walk: spec memos primed in the warm merge (round 10)

A main-thread-only profile of the structure phase (perf `--time` window,
filtered to the walk's thread — the arc-layer prefetch parses layers on other
threads meanwhile) put `Specs()` first: the walk calls `SpecsFor` for nearly
every `Src` (child names, instanceability, activation), and the warm merge had
cleared every merged `Src`'s memo (workers never memoize), so each call hashed
the site string and probed a large per-stack spec cache; negatives (not
cached) also went through the path filter / `FindSpecs`.

The merge's parallel remap phase now primes the memo exactly as the main
`SpecsFor` would store it: a hit in the main spec cache sets the pointer and
state; a miss runs the read-only `FindSpecs` (per-layer path lookups; the lazy
path index is built under its own lock) and stamps `kNone` when empty
(non-empty results are left to the walk, which caches them). The worker's
spec-cache adoption moved ahead of this phase — it adopts only stacks no
taken `Src` references, so the adoption order is unchanged. Priming runs only
when the phase is parallel (a worker delta ≥ 8192): serially it merely moved
the walk's work and, bypassing the path filter, enlarged it (ALab +8%).

During the full45 walk `SpecsFor` now resolves 38.5 M memo hits and 270 M
known-empty states with 2.1 M full lookups (before: ~75 M lookups). Paired vs
`lusdcat-R9b` (repeat 2, zero swap): full45 wall 124.6 → 115.5 s (**0.927**),
`structure` 38.7 → 28.2 s, RSS 0.998; with the parallel-only gate (R10b) ALab
0.991 and reference scene 0.993 vs R9b (repeat 3). Hashes: full45 `a0175bc1…`, district
`98619c40…`, reference scene `267d237a…`, merged ALab `2f7176c8…`; ctest 67/67; TSan
(ALab, parallel merge forced) clean.

Remaining walk counters (full45): sources ~4, register ~4.1, child names
~3.1, add_prim ~3.6, push ~2.5, prototype map ~1.2 s; the rest is per-prim
overhead. A subtree-parallel walk remains the larger, riskier lever.

### 10.14 Subtree-parallel structure walk; deferred (USD-style) opinion fill (round 11)

**Parallel structure walk** (`ParallelStructureWalk`, cache.cc; large scenes:
≥ 64 K warm sources and > 1 thread). Byte-identical to the serial walk:

- Workers walk subtrees speculatively and read-only: the `Src` memo is not
  written and `Specs()` misses resolve into a thread-local store
  (`SpecReadOnlyTLS`), never into the shared spec caches or filters.
  Prototype choice is first-in-DFS-order, so a prim is walked as an instance
  when its key was seen earlier in its task or claimed at an earlier DFS rank
  (`WalkClaims`: hierarchical ranks = item indices from the top list down
  through nested tasks, compared lexicographically).
- Work is handed off whenever a worker idles with nothing queued
  (`WalkDonate`): the entry becomes a task at exactly its DFS position (a
  placeholder item); tasks are taken DFS-earliest first (priority queue by
  rank). Coarser hand-off measured slower (a task's items are only usable when
  the whole task is done, and the commit is in DFS order).
- The committing thread replays the items in DFS order with the serial walk's
  exact instance registration (`CommitWalk`): a prim walked as a prototype that
  registration makes an instance drops its walked subtree; a prim walked as an
  instance that is really a prototype, or the depth limit, falls back to the
  serial walk (everything reset). What the walk could not do read-only is done
  at the commit, at its DFS position, with all workers parked at a prim
  boundary (`WalkPause`): a source-cache miss runs `SourcesForPath`, an
  unseeded prototype (the serial walk seeds its children's sources) is seeded;
  then that prim is re-walked at its own rank (a held claim that is a proper
  prefix of the claimant's rank is the same position) and committed in place.
- **Compact records, parallel prim construction** (option 1): `WalkItem` holds
  the composed `Path` (moved), `Src*`, instance key and links -- no `PrimSpec`.
  The commit only registers and assigns indices (nodes / parents / roots /
  `fill_` / `instance_prim_index_`); `MaterializeWalk` then sizes the prim
  vector once (`Layer::resize_prims`) and builds every `PrimSpec` in parallel
  (name, moved path via the new `set_path(Path&&)`, instance prototype from
  `prototype_of`, children from a CSR filled in commit order).
- Hand-off cost: task lookup without the queue lock (two-level table published
  before the id), an atomic `done` flag with a short pause-spin and a
  waiter-only notify, lock-free `running`/`parked` pause counters.

The first version (commit moving 312-byte `PrimSpec`s built on other cores)
was correct but not faster on full45 (commit ~22 s of work); OpenUSD keeps a
~64-byte `Usd_PrimData` per prim (`PcpPrimIndex*`, path, type info, child /
sibling links, flags) and resolves opinions through the prim index, which is
what the compact records follow.

Results (paired vs `lusdcat-R10b`, repeat 2/3, zero swap): full45 wall
113.5 → 107.3 s (**0.945**), `structure` 27.0 → 17.7 s, peak RSS 1.024;
reference scene 0.963 (RSS 1.017); ALab 1.018 (serial walk; noise). Hashes: full45
`a0175bc1…`, district `98619c40…`, reference scene `267d237a…`, merged ALab
`2f7176c8…`; ctest 67/67; no-thread build OK; TSan with the walk forced on
(ALab, reference scene) race-free and hash-identical.

**Deferred opinion fill** (`-DLIGHTUSD_NEXT_DEFERRED_FILL=ON`, off by
default; run-time switch `CompositionOptions::defer_opinion_fill`, set by
lusdcat for `-f` to USDC). The composition builds the skeleton layer (option
1's materialized prims) and installs a `PrimFiller` (`DeferredFiller`) on it
instead of running `FillOpinions`; the filler owns the fill-phase source /
spec storage and, via `keepalive`, the composition Cache. The USDC writer
composes each prim right before encoding it (in its worker threads) and
releases it to a skeleton afterwards (`PrimSpec::release_to_skeleton`; the
path string stays in place because the writer keeps views into it). Holder
instance flattening becomes per-prim fix-ups applied after each fill;
`finalize_properties` runs per prim. The writer's pre-scans skip composed
content (property-count sizing, `BuildTables` types / properties / metadata,
relationship-name pre-warm); a layer with variant-component paths is filled up
front (cross-prim variant-owner reads). Output is semantically identical but
not byte-identical (token / string table order): the USDA conversions of the
deferred and eager crates are identical for the district (7.7 GB, `cmp`),
reference scene, merged ALab and full45 (`583e382e…`).

Paired vs the default build (`lusdcat-R11b`, repeat 2/3, zero swap): full45
wall 101.8 → 100.7 s (0.988), peak RSS 1.001, user CPU 892 → 800 s,
`fields_specs` 16.8 → 24.3 s; reference scene 5.23 → 4.37 s (**0.836**), peak RSS
4.39 → 3.53 GiB (**0.806**); merged ALab 1.10 → 1.11 s but peak RSS 1.234
(the Cache kept for the write outweighs the saving), so stages under 64 K
prims now fill eagerly (ALab byte-identical to the default build, same RSS).

Single-run full45: wall ~94.7 s (eager ~104 s), load+compose 51.8 s,
`fields_specs` 24.2 s (the fill moved into the writer), peak RSS 73.5 GiB vs
73.2 GiB eager: the full45 peak is in the warm phase (source resolution), not
the composed layer, so deferring the fill does not lower it. (With only the
value payloads released after encoding, the peak was 81 GiB.) (Before the
skeleton release, reference scene was slower / larger: 5.8 → 6.4 s, 4.5 → 5.4 GB.) TSan
on the deferred build (ALab, reference scene) race-free, semantically identical.

### 10.15 Warm-phase peak RSS: inert composition sources, metadata ext (round 12)

Units in §10.15–§10.22: GiB = 2³⁰ bytes. Process RSS comes from `/proc` /
`time` (KiB ÷ 1,048,576), allocator figures from jemalloc's `stats.*` /
`thread.peak` (bytes ÷ 2³⁰), heap-profile figures from `jeprof` (MiB ÷ 1024).

**Where the peak was.** Phase markers plus jemalloc's `stats.allocated` /
`stats.resident` (read through `mallctl`) on full45 with R11b:

- During the warm workers, allocated memory climbs linearly to ~59 GiB.
- At the start of the structure walk it is 48 GiB allocated / 54 GiB resident.
- At the end of the fill it is 47.6 GiB allocated / 63.6 GiB resident.
- Process peak: 67–73 GiB.

A sampled heap profile (`MALLOC_CONF=prof:true,lg_prof_sample:21` plus
`prof.dump` at phase boundaries; `jeprof --text --cum`) put ~24.8 GiB under
`ExpandList` (composition sources) and ~12.8 GiB under the crate reader (the
loaded layers).

The warm builds **104 M `Src`s** for 10.3 M prims: 13.2 GiB of vectors plus
11.5 GiB of site strings. **90 M of them are inert**, meaning no stack layer has
a spec at the site. The fill-time compaction (`CompactOpinionSources`) only
dropped them right before the fill, after they had overlapped the walk. The
freed pieces then sat interleaved with live data: resident minus allocated was
15–19 GiB. An explicit `arena.<all>.purge` recovered only ~5 GiB, so the gap is
fragmentation rather than dirty pages.

**1. Inert sources dropped while warming** (`Cache::Impl::DropInertSources`,
`IsInertSource`; cache-storage.cc). An inert source contributes nothing:
- no opinions, child names, activation or instanceability;
- nothing derived from it does either, because a layer cannot hold a spec
  below a site it has no spec at;
- the exception is a mapping pair that re-roots namespace at or below the site.

Kept, so every reader sees the same list:
- the first source (`ComputeInstanceKeyImpl` is positional about it);
- variant and relocation-anchor sources;
- unresolved sources;
- sources whose mapping has a pair at or below the site (they feed `MapsFrom`,
  which the instance key and the instance subtree filter read).

A list is left whole when:
- any of its sources is in a stack with relocates (`RelocationsAt` reads
  departures for every source, and a departed child derives from this list);
- or any composed child has no cached list yet (a later derivation would see
  the compacted list; ExpandList's dedup and ordering read the whole list).

Spec states that a copy reset are looked up (`SpecsFor`).

Timing in the warm, gated by `release_build_sources` (`warm_drop_inert_`):
- a leaf drops right in `ExpandWarmNode`;
- a parent drops when its worker processes the parent's last child
  (`WarmTask::parent_children` carries the parent's child names); a donated
  sibling leaves it whole;
- prototypes, whose children are seeded from filtered sources, and the serial
  frontier discovery are left alone.

**2. Worker source vectors shrunk to size** before the merge, on the worker's
own thread. `shrink_to_fit` is a no-op in this `-fno-exceptions` build, so the
vector is rebuilt (worker slack was 28% of `Src` vector bytes).

**3. Composed prims no longer all allocate the metadata ext.** The fill's
clips snapshot read `out->meta().clips()` through the mutable accessor. That
allocated a `PrimMetadataCold` on every composed prim: 10.2 M on full45,
3.5 M of them with no cold field set.

**4. `PrimMetadataCold` split** (1,264 B). The full45 composed census was:

| field | prims |
|---|---:|
| customData | 5.1 M |
| kind | 3.6 M |
| instance_prototype | 1.1 M |
| apiSchemas | 0.5 M |
| any other cold field | 0 |

Those fields stay in `PrimMetadataCold`. The rest (doc / comment /
displayName / permission / variants / relocates / list-op edits / assetInfo /
sdrMetadata / clips / ordering / unknown fields / arc edits) moves to a
lazily allocated `PrimMetadataRare` under it. All access goes through the
`PrimMetadata` accessors, so only prim-spec.cc and prim-spec-storage.cc change.

full45, single runs (jemalloc allocated / resident, process peak):

| step | end of warm workers | end of fill | process peak |
|---|---:|---:|---:|
| R11b | 51 / 53 GiB | 48 / 64 GiB | 67–73 GiB |
| + 1, 2 | 33 / 34 GiB | 48 / 58 GiB | 61.5 GiB |
| + 3, 4 | — | — | **50.5 GiB** |

The peak is now in the opinion fill; the warm phase stays under 38 GiB.

**Rejected.**
- *Post-merge parallel drop* (the same rule over the merged cache before the
  walk). Superseded: it ran after the warm peak, and the freed holes were
  never reused (−9 GiB allocated but no lower process peak).
- *Eager child derivation in the warm* (derive a node's children's lists at
  expansion, then compact it). Byte-identical, but ALab 1.054× / reference scene 1.045×
  slower. On the serial discovery it computed the frontier's lists, which is
  the workers' parallel work. And a donated child whose list the donor had
  already derived made the receiver recompute the whole ancestor chain.
- *jemalloc purge* before the walk / fill: −5 GiB resident, 1.2 s, and the peak
  unchanged. The gap is fragmentation.

**Verification.**
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep: 1,933 inputs (tests/usda, tests/usdc and the AOUSD
  supplemental composition / value_resolution / compliance cases), `-f
  --compose-threads 4`, USDA and USDC, byte-identical to R11b.
- Rewrite sweep (705 fixtures, USDA and USDC) identical.
- ctest 67/67.
- TSan (ALab 8 threads, reference scene 16 threads) race-free and hash-identical.
- Deferred-fill build: district USDA conversion unchanged (`6f476a6e…`).

**Paired vs `lusdcat-R11b`** (zero swap):

| case | wall | peak RSS |
|---|---:|---:|
| full45, repeat 2 | 98.4 → 92.0 s (**0.935**) | 73.2 → 50.7 GiB (**0.693**) |
| merged ALab, repeat 3 | **0.883** | 0.896 |
| reference scene, repeat 3 | 0.961 | 0.955 |

On full45:
- warm workers +0.4 s;
- merge −1.4 s;
- fill −2.9 s (Src compaction 2.4 → 1.0 s);
- writer `tables` −1 s and `fields_specs` −1.3 s (smaller metadata).

**Deferred fill re-measured after round 12** (`lusdcat-R12def` =
`-DLIGHTUSD_NEXT_DEFERRED_FILL=ON`, paired vs `lusdcat-R12b`, zero swap):

| case | wall | peak RSS | notes |
|---|---:|---:|---|
| full45, repeat 2 | 89.8 → 93.9 s (1.045) | 1.009 | compose fill 5.7 → 2.4 s, `tables` 3.8 → 1.4 s, `fields_specs` 15.3 → 24.3 s; user CPU 0.927 |
| reference scene, repeat 3 | **0.830** | **0.836** | 4.10 → 3.43 GiB |
| merged ALab, repeat 3 | 1.000 | 0.991 | under 64 K prims, fills eagerly |

District USDA conversion unchanged (`6f476a6e…`).

On full45 the peak only moves:
- Eager peaks at the end of the opinion fill (50.7 GiB).
- Deferred tops out at 44.8 GiB in compose but peaks in the writer's
  `value_emit` (51.0 GiB), about 6 GiB higher than eager at the same point.

A heap profile after `fields_specs` (deferred) explains it. The per-prim
`Src` vectors are released as filled; what the deferred run additionally
keeps is the namespace-mapping pool (`nm_pool_`, 2.3 GiB, which
`ComposeOpinions` reads for relationship targets until the last prim) plus
the fragmentation the fill/release churn leaves. The rest is common to both
modes:
- loaded layers, 11.8 GiB;
- skeleton `PrimSpec` vector, 4.0 GiB;
- prim paths, 2.7 GiB;
- writer stage buffers, 3.6 GiB.

The writer's ordered-commit pipeline also absorbs the fill worse than the
standalone parallel fill: +9 s `fields_specs` against −5.7 s fill/tables.

Verdict: keep deferred opt-in. It pays off on reference scene-sized stages; on full45
it needs a cheaper mapping store and a fill-aware writer chunking before it
can win.

### 10.16 Shared floor: loaded layers and skeleton PrimSpecs (rounds 13–14)

After round 12, both fill modes shared the same floor: about 12 GiB of loaded
layers, the composed PrimSpecs with their paths, and the composed opinions.
Heap profiles (jemalloc `prof.dump` at the walk start, after the walk commit,
after materialization and at the fill end) and censuses of the composed and
layer metadata pointed at per-object overheads, not at the data itself.

**Round 13: layer and opinion metadata.**

1. **Shared property metadata.** `PropMeta` is 688 bytes. On full45, the
   5.0 M composed and 0.95 M layer property-metadata blocks set only
   `interpolation`, plus `elementSize` on about 0.8 M of them.
   - Property metadata moved out of `PrimSpec::ColdData` into a sorted
     per-prim `PropMetaTable`. `ColdData` is 12 `unordered_map`s (672 B) and
     had been allocated by 2.95 M composed prims just for interpolation.
   - Metadata holding only interpolation / elementSize is interned: one
     immutable `PropMeta` per distinct value, copied on any mutable access
     (`ensure_property_meta`), so callers keep the same API.
   - Sharing happens in `finalize_properties`, right after each prim's fill
     (`share_property_metas`), and directly when the crate reader installs
     metadata (`set_property_meta`).
2. **Arc list-op edits moved to `PrimMetadataCold`.** On full45, 621 K of the
   632 K layer prims with a rare metadata tier had it only for `arc_edits`.
3. **`PrimMetadata::trim_ext`** frees metadata tiers that a merge touched and
   later cleared. Composed prims inherited authored `variantSetNameEdits`,
   which the fill then strips, leaving an empty 1.1 KB rare tier behind.
4. **Decoded-value cache in the crate reader** (`FindDecoded` /
   `StoreDecoded`). A crate stores each distinct value once, and every spec
   that authors it points at the same ValueRep. Dictionaries (benchmark scene prefab
   prims all carry a small nested `customData`) and arrays (lazy and eager)
   are now decoded once per file and shared; both are refcounted and
   copy-on-write.
   - The cache is keyed by the raw ValueRep and cleared per parse.
   - Exception: reference `customData` is decoded through a ValueRep built
     from the current stream position, and the caller reads on after it.
     That path stays uncached. Caching it broke reference scene: a layer failed to
     load, and the flatten silently turned the `instancer` prim into an
     empty `over`.

**Round 14: skeleton build.**

5. **Value blocks pre-sized** to the reserved property index. Doubling a
   vector of 64-byte `Value`s left about 20% slack.
6. **Walk task item vectors rebuilt to size** when each task finishes, and
   instance-key strings released at commit right after `AssignPrototype`
   (the prototype maps keep their own copies).
7. **Fill preparation moved before materialization.** Spec-memo priming,
   spec-cache / source-cache index release and the inert-source compaction
   now run between the walk commit and the parallel PrimSpec build
   (`prepare_fill`, passed to `ParallelStructureWalk` as
   `before_materialize`). The freed sources (~19 M) no longer overlap the
   PrimSpec array's first touch.

**full45, single runs:**

| step | process peak |
|---|---:|
| R12b | 50.7 GiB |
| + 1–3 | 42.8 GiB |
| + 4 (R13) | 39.5 GiB |
| + 5, 6 | 39.4 GiB |
| + 7 (R14) | **36.4 GiB** |

Loaded layers, at fill end: 12.1 → 6.5 GiB.

**Verification (R14).**
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933 inputs) and rewrite sweep (705 fixtures), USDA
  and USDC, byte-identical to R11b.
- ctest 67/67.
- TSan (ALab 8 threads, reference scene 16 threads) race-free and hash-identical.
- Deferred build: district USDA conversion unchanged (`6f476a6e…`).

**Paired results (zero swap):**

| comparison | case | wall | peak RSS |
|---|---|---:|---:|
| R13 vs R12b | full45, repeat 2 | 0.946 | **0.777** (50.8 → 39.5 GiB) |
| R13 vs R12b | ALab, repeat 3 | 0.989 | 0.930 |
| R13 vs R12b | reference scene, repeat 3 | 0.976 | 0.889 |
| R14 vs R13 | full45, repeat 2 | 1.012 (noise) | 0.923 (39.5 → 36.5 GiB) |
| R14 vs R12b | full45, repeat 2 | **0.957** | **0.718** (50.8 → 36.5 GiB) |
| R14 vs R12b | ALab, repeat 3 | 0.979 | 0.932 |
| R14 vs R12b | reference scene, repeat 3 | 0.948 | 0.894 |

**What is left** (full45 at fill end, about 24.7 GiB allocated / 34 GB
resident):
- composed opinions, ~7 GiB: mostly 64-byte `Value`s and property slots;
- loaded layers, 6.5 GiB;
- PrimSpec arrays, 4.5 GiB, at 312 B per PrimSpec; `PrimMetadata`'s inline
  arc vectors and variant selection are 128 B of that and empty on composed
  prims;
- composed path strings, 2.3 GiB;
- namespace-mapping pool, 2.3 GiB;
- walk/prototype bookkeeping, ~1.8 GiB;
- about 9 GiB of jemalloc fragmentation (resident minus allocated).

The next structural levers:
- a slimmer `PrimSpec`: the arc lists behind a lazy proxy, ~1.1 GiB; about
  480 call sites use the members directly;
- a mapping pool that chains to its parent instead of copying pairs;
- dropping the 19 M inert sources that instance lists keep.

### 10.17 Slimmer PrimSpec (round 15)

`PrimSpec` was 320 bytes, and `PrimMetadata` accounted for 160 of them. Its
four inline arc lists (`references` / `payloads` / `inherits` /
`specializes`, 24 B each) and the legacy `variantSelection` string (32 B)
are empty on composed prims and on most layer prims.

`layer/lazy-containers.hh` adds two pointer-sized containers (the pointer
is null while empty):
- `LazyStringList` keeps the subset of the `std::vector<std::string>` API the
  codebase uses: iteration, `push_back`, `insert`, `erase`, `assign`,
  `clear`, comparison.
- `LazyString` does the same for `std::string`: `find`, `substr`, `+`,
  comparison.

Both convert implicitly to the const container, so the ~480 readers compile
unchanged. Fixes were needed in 7 places, all writers or pointer returns:
- the parser's `SelectArc`;
- the crate reader's `apply_arc_field` (`.mut()`);
- the composition `merge_arcs`, which takes a `LazyStringList&` and
  allocates only when an arc lands;
- the C API and USDA writer's `&...get()`;
- memory accounting.

The metadata headers are included inside `namespace lightusd::next` by
`prim-spec.hh`, so `prim-spec.hh` includes `lazy-containers.hh` first.

Results:
- `PrimMetadata` 160 → 72 B; `PrimSpec` 320 → **232 B**.
- `WalkItem`'s instance key is now a `unique_ptr<std::string>` (most items
  are not registered): 96 → 72 B per walk item.

Paired vs `lusdcat-R14` (zero swap, `--max-load 48`). The full45 runs leave
the 1-minute load above 24, so the stricter gate refused the next run.

| case | wall | peak RSS |
|---|---:|---:|
| full45, repeat 2 | 0.964 | 0.968 (36.6 → 35.4 GiB) |
| reference scene | 0.977 | 0.987 |
| merged ALab | 1.03 (0.04 s, noise) | 0.994 |

R15 vs R12b cumulative on full45: peak 50.8 → 35.4 GiB (0.696).

Verification:
- All four reference hashes match.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67.
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

The full45 peak is now in the structure walk (35.4–35.8 GiB). At the start
of the walk about 31.1 GiB is resident: layers 6.5 GiB, sources about 7.7 GiB,
mapping pool 2.3 GiB, source/spec indexes and fragmentation.

### 10.18 Shared namespace-mapping strings (round 16)

The mapping pool (`nm_pool_`) held **4.12 M** `NamespaceMapping`s on full45,
about 2.3 GiB in all. `InternMapping` has no dedup, and the mappings are
nearly all distinct and all still referenced:
- 10.3 M pairs, 64 B each as `std::pair<std::string, std::string>`;
- 1.56 GiB of path strings.

Only **2.97 M** of the 20.6 M pair strings are distinct (0.46 GiB). A composed
mapping copies its inner pairs' sources and its outer pairs' targets, so the
same paths repeat through every mapping derived from them.

`pcp::PathStr` (namespace-mapping.hh) is an immutable, refcounted path
string: one pointer, null when empty. Copies share one node.
- `NamespaceMapping::Pair` is now `std::pair<PathStr, PathStr>` (16 B).
- `PathStr` converts implicitly to `const std::string&` and provides
  comparison and `+`, so the 6 call sites outside the header that read
  `.pairs` compile unchanged.
- `Compose` carries inner targets through the new `ApplyShared`, which
  returns the input node (unmapped path) or the pair's target node (exact
  prefix match) instead of building a new string.

Pool on full45:
- 20.6 M pair strings in 7.05 M nodes (0.80 GiB);
- pairs 0.61 → 0.15 GiB;
- total ≈ 2.3 → 1.0 GiB.

Full interning (down to 2.97 M) would save about 0.35 GiB more but hashes
20 M strings on the merge's critical path, so it was not done.

The compose phase now tops out below the write phase. The full45 process
peak moved to the writer's `value_emit` (about 33.4 GiB, anon only: the
layers' mmap pages are dropped by then). At the same point mid-compose,
anonymous RSS is 28.6 GiB, against 31.5 GiB for R15.

Paired vs `lusdcat-R15` (`--max-load 48`, zero swap):

| case | wall | peak RSS |
|---|---:|---:|
| full45, repeat 2 | **0.967** | **0.940** (35.5 → 33.4 GiB) |
| reference scene | 0.988 | 0.994 |
| merged ALab | 1.011 | 0.991 |

R16 vs R12b cumulative on full45: peak 50.8 → 33.4 GiB (0.656).

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67.
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

Next: the write-phase peak. `value_emit` adds about 4.7 GiB over
`fields_specs`, so the writer's value-emit buffers are now the full45
maximum.

### 10.19 Write-phase structural payloads (round 17)

After round 16 the full45 peak was in the writer, in the interval the RSS
tracer labels `value_emit`. The tracer labels samples by the last lap, so
this is actually the structural payload build that follows it: TOKENS,
FIELDS, FIELDSETS, SPECS and PATHS are built concurrently. Per-builder
jemalloc `thread.peak` on full45:

| builder | thread peak | payload | time |
|---|---:|---:|---:|
| PATHS (61.8 M paths) | 3.51 GiB | 32 MiB | 5.75 s |
| FIELDSETS | 1.75 GiB | 487 MiB | |
| FIELDS (177 M fields) | 1.67 GiB | 131 MiB | |
| SPECS | 0.35 GiB | | |

Together they made +4.7 GiB of RSS over `fields_specs`.

1. **`CompressCrateBlob` / `EncodeDeltaU32` / `EncodeDeltaS32` no longer
   zero-fill their bound-sized output.** `std::vector::resize(bound)` made
   an input-sized buffer resident (~1.3 GiB for the FIELDS value reps) and
   kept that capacity after shrinking. They now write into uninitialized
   scratch and return an exact-size copy. The parallel delta encoder
   already sized its output exactly.
2. **PATHS builder slimmed** from ~56 to ~21 bytes per path at its peak:
   - a 4-byte element offset into the record's stem replaces a 16-byte
     `string_view` (a property's element is its name);
   - `encoded` / `is_prop` / `is_root` / `has_sibling` share one flags byte;
   - the CSR child lists, cursor and parent links are freed once the
     pre-order and subtree sizes exist;
   - the pre-order doubles as the path-index column;
   - each column is compressed and freed in turn.

   Thread peak 3.51 → 1.56 GiB; 5.75 → 5.18 s.

The full45 write-phase maximum is down from 33.3 to 31.7 GiB, so the
process peak is back in compose (~33.0 GiB, including ~3.3 GiB of mmap
file pages).

Paired vs `lusdcat-R16` (`--max-load 48`, zero swap):

| case | wall | peak RSS | notes |
|---|---:|---:|---|
| full45, repeat 2 | 1.002 | 0.989 (33.2 → 32.9 GiB) | `structural_payloads` 5.86 → 5.19 s |
| reference scene | 0.981 | 1.001 | `structural_payloads` 0.936 |
| merged ALab | 1.011 (noise) | 0.999 | |

R17 vs R12b cumulative on full45: peak 50.8 → 32.9 GiB (0.647).

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67.
- Staged mode (`WriteLayerToMemory`, district): parallel == serial
  (`98ad30c3…`).
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

### 10.20 Inert sources in instance lists (round 18)

Round 12's warm-time drop left a list whole whenever a composed child had no
cached list yet. That covers every later instance (warm skips its subtree)
and every prototype (whose children are seeded from its filtered sources).
The census at the start of the walk on full45 found 9.0 M inert sources
(2.2 GiB) still held, nearly all of them in 1.06 M instanceable lists, every
one of which has a shared (arc-targeted, `MapsFrom`) source.

When the walk does expand an instanceable prim with shared sources,
`PreseedPrototypeChildren`, the walk's commit-time seeding and the serial
`BuildStage` all derive its children from the shared sources alone, and the
drop keeps every shared source. So nothing reads such a list whole again,
and `ExpandWarmNode` now drops its inert sources:
- **later instance** (the claim fails): no child check (`children` null),
  provided the list has a shared source (`HasSharedSource`);
- **prototype**: after `PreseedPrototypeChildren`, checked against the
  filtered child names it seeded.

full45 single run:
- peak 33.0 → **32.1 GiB**;
- structure-walk maximum 32.9 → 30.3 GiB;
- the process peak is now in the opinion fill (~32.1 GiB).

Paired vs `lusdcat-R17` (`--max-load 48`, zero swap):

| case | wall | peak RSS | notes |
|---|---:|---:|---|
| full45, repeat 2 | 0.999 | 0.977 (32.9 → 32.1 GiB) | warm workers 0.976, build_stage 0.979 |
| reference scene | 1.002 | 1.009 | noise |
| merged ALab | 0.989 | 0.997 | |

R18 vs R12b cumulative on full45: peak 50.8 → 32.1 GiB (0.632).

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933, `--compose-threads 4`) and rewrite sweep (705)
  identical in USDA and USDC.
- ctest 67/67.
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

### 10.21 Src diet: shared arc context (round 19)

`Src` was 136 bytes. Four of its fields, 64 B inline, are set once per arc
and propagated unchanged to every source derived through it:
- `offset` (16 B);
- `expression_variables`, `arc_chain` and `arc_sites` (three
  `shared_ptr`s, 48 B).

Census on full45 at the start of the walk:
- 24.66 M sources; expression variables on 95%, chain / sites on 54%,
  non-identity offsets on 49%;
- only **2.93 M distinct** (expression variables, chain, sites, offset)
  tuples.

They now live in one shared `SrcContext`:
- `Src` holds a `shared_ptr<SrcContext>` and is **80 B**, with the hot
  fields reordered to pack.
- Reads go through accessors (`offset()`, `expression_variables()`, ...).
- Setters copy-on-write the context, and leave it alone when the value does
  not change.
- `DeriveChildSources` and the relocate-arrival path share the parent's
  context outright (`set_context`). Copying the four fields one by one
  had first given every source its own context: 24.7 M objects and +1.2 GiB
  of RSS.
- The worker→main merge remaps the chain and site trail once per context
  (a per-thread memo), not once per source.

The ~70 field accesses were rewritten mechanically from compile errors,
using each error's column, so same-named fields of other types (layer
stacks, opinion items, nodes) were left alone.

Result on full45: 4.12 M context objects for 24.66 M sources.
- warm workers' RSS 26.0 → 24.5 GiB;
- structure walk 30.5 → 29.0 GiB;
- the process peak, in the opinion fill where most sources are already
  compacted, moves only 32.2 → 32.0 GiB.

The smaller `Src` also makes copies, moves and cache-line reads cheaper.

Paired vs `lusdcat-R18` (`--max-load 48`, zero swap):

| case | wall | user CPU | peak RSS | notes |
|---|---:|---:|---:|---|
| full45, repeat 2 | **0.962** (83.2 → 80.1 s) | 0.872 | 1.000 | warm workers 0.828, warm merge 0.768, structure 0.918, build_stage 0.872 |
| reference scene | **0.941** | | 1.006 | warm workers 0.838, merge 0.765 |
| merged ALab | 0.968 | 0.830 | 0.997 | |

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67.
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

### 10.22 Composed opinion storage: Value 48 B, packed cold metadata (round 20)

1. **`lu_value` 64 → 48 B** (`Value` is a thin wrapper over it).
   - The inline-store union carried `max_align_t`, which forced 16-byte
     alignment and padded the 8-byte header plus 40 inline bytes to 64.
     Every inline scalar needs at most 8-byte alignment (double / int64
     lanes), and value.c reads the store through `memcpy`, so the union's
     alignment member is now `uint64_t` / `double`.
   - The layout is reported at run time by `lu_abi_get()` (`value_size`,
     `value_alignment`, `store_offset`). The web feature runtime reads it
     dynamically, and the prebuilt wasm modules bundle their own C core.
   - **This is a layout change of a public C struct.** Consumers built
     against the old header must rebuild. `LU_CORE_C_VERSION_*` was not
     bumped; that decision is left to review.
   - On full45 this saves about 16 B × 40 M composed values: fill peak
     32.0 → 31.3 GiB.
2. **apiSchemas list-op edits off the rare tier.** On full45, 505,626
   composed prims carried the 1.1 KB `PrimMetadataRare` only for
   `apiSchemaEdits` and its qualifier. These now live in a small
   `ApiSchemaListOp` behind a pointer in `PrimMetadataCold`, and `trim_ext`
   frees it when it is back at its defaults. `PrimMetadataRare` 1104 → 848 B.
3. **`PrimMetadataCold` packed, 184 → 128 B** (6.7 M composed prims):
   - the three flags share one word;
   - `instance_prototype` (1.06 M prims) is a `LazyString`;
   - `apiSchemas` (0.5 M prims) is a `LazyStringList`.

Single runs on full45: peak 32.0 → 31.3 (1) → 31.0 (2) → **30.1 GiB** (3).

Paired vs `lusdcat-R19` (`--max-load 48`, zero swap):

| case | wall | peak RSS | notes |
|---|---:|---:|---|
| full45, repeat 2 | **0.973** (80.2 → 78.0 s) | **0.938** (32.1 → 30.1 GiB) | `fields_specs` 0.944: smaller values to read |
| reference scene | 0.982 | **0.908** (3.68 → 3.34 GiB) | |
| merged ALab | 1.000 | 0.987 | |

R20 vs R12b cumulative on full45: wall 92.3 → 78.0 s, peak 50.8 → 30.1 GiB
(0.593).

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67, including the C `test_storage` ABI check.
- Staged mode (district): parallel == serial (`98ad30c3…`).
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

### 10.23 Composed path strings built at exact size (round 21)

Census at fill end on full45: the 10.2 M composed prim paths (average 174
characters, 1.65 GiB of text) held **2.68 GiB** of heap.

`Path::append_child[_unchecked]` built each path as `path_ + "/" + name`.
The temporary grows by doubling (libstdc++ allocates at least twice the old
capacity), and `Path(std::string&&)` / `PrimSpec::set_path(Path&&)` keep that
capacity for the life of the stage.

The child and property builders now use `JoinExact` (reserve the exact
length, then append), and so do the same pattern's other per-prim sites:
- `DeriveChildSources`' site join (`c.site`), which every derived source
  keeps;
- the USDA layer builder's prim paths (`Layer` builder `begin_prim`).

The bytes are the same strings, so all outputs are unchanged. Path heap at
fill end: 2.68 → **1.66 GiB**.

Single run on full45: peak 30.1 → **28.5 GiB**. The structure walk drops the
most (28.3 → 26.2 GiB), since site strings and composed paths both live
there.

Paired vs `lusdcat-R20` (`--max-load 48`, zero swap):

| case | wall | peak RSS | notes |
|---|---:|---:|---|
| full45, repeat 2 | 0.995 | **0.944** (30.2 → 28.5 GiB) | warm workers 0.955 |
| reference scene | 0.977 | 0.989 | structure 0.883 |
| merged ALab | 0.978 | 0.990 | |

R21 vs R12b cumulative on full45: wall 92.3 → 77.9 s, peak 50.8 → 28.5 GiB
(0.56).

Verification:
- Hashes: full45 `a0175bc1…`, district `98619c40…`, reference scene `267d237a…`,
  merged ALab `2f7176c8…`.
- Composition sweep (1,933) and rewrite sweep (705) identical in USDA and USDC.
- ctest 67/67.
- TSan (ALab, reference scene) clean and hash-identical.
- Deferred build: district USDA unchanged.

What is left on the path side is the instancing bookkeeping, about 0.9 GiB
of full-path copies:
- `prototype_of`, 1.06 M path → path entries (0.43 GiB);
- `instances_by_prototype` (0.23 GiB);
- `instance_prim_index_` (0.25 GiB).

These could be keyed by prim index. A shared-prefix (parent + name) path
representation would need a `Path` API change: `str()` returns
`const std::string&` across the codebase.

### 10.24 Instancing maps share path nodes (round 22)

The instancing bookkeeping held about 0.9 GiB of full-path copies on full45,
for 1.06 M instances in ~7 K groups:
- `prototype_of` (path → prototype path), 0.43 GiB;
- `instances_by_prototype` member lists, 0.23 GiB;
- `instance_prim_index_`, 0.25 GiB.

Every instance path was copied into all three, and every member repeated
its prototype's path. Now they share refcounted `PathStr` nodes:
- `prototype_of` is a `string_view`-keyed map whose value
  (`PrototypeOfEntry`) owns the key's node plus the prototype node shared
  by the whole group. C++17 has no heterogeneous lookup, so the view key
  keeps `find(std::string)` allocation-free.
- `instances_by_prototype` member lists and `instance_prim_index_`
  (`InstanceIndexEntry`) reuse the same nodes.
- `prototype_by_key` holds the prototype node.

The public `Cache` API (`IsInstance`, `GetPrototype`,
`GetInstancesForPrototype`, ...) is unchanged.

Effect on full45 (single run): structure walk 26.2 → 25.6 GiB, fill
28.4 → 27.9 GiB. The process peak only moves 28.42 → 28.27 GiB, because it
is now the writer's structural payload build (~28.3 GiB), where these maps
are already gone.

Paired vs `lusdcat-R21` (`--max-load 48`, zero swap):

| case | wall | peak RSS |
|---|---:|---:|
| full45, repeat 2 | 1.004 | 0.995 (28.43 → 28.30 GiB) |
| reference scene | 0.994 | 1.011 (noise) |
| merged ALab | 0.989 | 1.004 |

Verification:
- All four hashes.
- Composition sweep (1,933) and rewrite sweep (705) identical.
- ctest 67/67, including the `Cache` instancing API tests.
- TSan (ALab, reference scene) clean.
- Deferred build: district USDA unchanged.

### 10.25 Migration to the benchmark host (2026-10-26)

The work moved to a second host: Ryzen 9 3950X (16C/32T), 60 GiB RAM,
Ubuntu 26.04, g++ 15.2, and glibc 2.43. The source checkout, OpenUSD tree,
scene inputs, and benchmark scripts were placed under separate local roots
(`$REPO_ROOT`, `$OPENUSD_ROOT`, `$ASSET_ROOT`, and `$BENCH_ROOT`). The source
tree hash matched the original checkout.

The inputs and benchmark kit were copied to the host; network addresses,
hostnames, and user directory names are omitted.

On the benchmark host, jemalloc is linked through the runtime `.so.2`
(`-DLIGHTUSD_NEXT_ALLOC_JEMALLOC=...`; no `-dev` symlink).

Benchmark-host smoke results:

| case | wall | peak RSS | hash |
|---|---:|---:|---|
| district | 13.5 s | 6.08 GiB | `98619c40…` |
| full45 | 125.1 s | 29.1 GiB | `a0175bc1…` |
| merged scene | 3.0 s | 0.77 GiB | `2f7176c8…` |
| reference scene | 8.5 s | 3.23 GiB | `267d237a…` |

ctest 61/61 (no TSan-only suite in this configuration).

**Open item: one reference-scene run produced `a8f50922…`** (8 parse /
8 compose / 8 write threads). It was the first run after the copy. Then 21
further runs, 12 warm-cache and 4 cold-cache (page cache evicted with
`posix_fadvise`) plus 5 other thread mixes, all gave `267d237a…`. The initial
difference looked like a scheduling race, but §10.27 resolved it: that run
used `--max-crate-elements 300000000` (shared with full45), while the later
runs did not. `a8f50922…` is the full reference-scene output, and
`267d237a…` was missing five silently dropped arrays.

### 10.26 Allocator evaluation on benchmark host (2026-10-27)

Host: benchmark host, Ryzen 9 3950X 16C/32T, 60 GiB RAM, quiet.
Same source tree (R22), byte-identical outputs from every allocator (all
hashes match per workload). All timings are medians; peak RSS is the
maximum over runs.

**Candidates.**
- Native:
  - glibc 2.43 (`-DLIGHTUSD_NEXT_ALLOCATOR=system`);
  - system jemalloc 5.3 (the current `auto` pick);
  - **mimalloc v3.6.0, vendored in `src/external/mimalloc`**
    (`-DLIGHTUSD_NEXT_ALLOCATOR=mimalloc`: `src/static.c` as an OBJECT
    library with `MI_MALLOC_OVERRIDE`, so it replaces malloc / new in
    `lusdcat`).
- WASM, through the new `LIGHTUSD_WASM_MALLOC` option (web/CMakeLists.txt;
  the last `-sMALLOC` wins), Emscripten 6.0.13, node 24.19:
  - emscripten's dlmalloc, emmalloc and mimalloc;
  - each built for wasm32 and wasm64 (memory64).
- Not measured: snmalloc 0.7.6, rpmalloc 2.0.1 and tcmalloc (gperftools
  2.19.1). Fetching and building them on benchmark host was denied by the session's
  permission policy.
- Google's MiniMalloc (github.com/google/minimalloc) is an offline
  static buffer-placement solver for ML accelerators (known lifetimes →
  offsets), not a general-purpose heap. It is not applicable here and was
  not benchmarked.

(The reference-scene allocator run used the default cap and the truncated
stage described in §10.27; the allocator ratios are unaffected.)

**Native** (`lusdcat -f` → USDC; district, reference scene 8/8/8 and merged ALab
×3, full45 ×2, interleaved):

| workload | glibc wall / RSS | jemalloc wall / RSS | mimalloc wall / RSS |
|---|---:|---:|---:|
| district | 11.00 s / 7.36 GiB | 6.88 s / 4.97 GiB | 6.22 s / 5.34 GiB |
| reference scene | 6.53 s / 3.63 GiB | 4.86 s / 3.34 GiB | 4.19 s / 3.72 GiB |
| merged ALab | 1.37 s / 0.79 GiB | 1.17 s / 0.78 GiB | 1.10 s / 1.11 GiB |
| full45 | 104.7 s / 35.4 GiB | 70.6 s / 29.3 GiB | 67.3 s / 33.8 GiB |
| geomean vs jemalloc | 1.390 / 1.186 | 1 / 1 | **0.914 / 1.185** |

Other observations:
- glibc spends 2–5× jemalloc's system time.
- mimalloc takes 10–24× fewer minor faults (4.9 M vs 13.5 M on full45):
  it reserves 1 GiB arenas up front.

mimalloc tuning (environment variables, same binary):

| setting | full45 wall / RSS | merged ALab wall / RSS |
|---|---:|---:|
| default | 66.1 s / 33.7 GiB | 1.11 s / 1.11 GiB |
| `MIMALLOC_PURGE_DELAY=0` | 70.9 s / **27.9 GiB** | 1.21 s / 0.92 GiB |
| `MIMALLOC_ARENA_EAGER_COMMIT=0` | 73.6 s / 33.2 GiB | 1.24 s / 0.84 GiB |
| both | 76.3 s / 26.9 GiB | 1.30 s / 0.74 GiB |

With `PURGE_DELAY=0`, mimalloc matches jemalloc on full45 (70.9 vs 70.6 s)
at a 4–5% lower peak.

**WASM** (one fresh node process per run, ×3 interleaved). Workloads:
- `render`: `RenderStream.begin` plus `getMeshCopy` of every mesh, the
  path a web viewer takes;
- `flatten`: `NextFlattenSession`.

Inputs are self-contained reference scene elements flattened natively
(`$BENCH_ROOT/wasm-inputs`). "heap" is the final wasm linear
memory, which never shrinks: what a browser tab commits.

`the large flatten input` flatten is excluded from the wasm32 numbers: mimalloc
reached the 2 GiB wasm32 limit there (dl/emmalloc peaked at 1.9 GiB).
Nothing approached the 16 GiB wasm64 limit.

| target | allocator | time | heap | process RSS | `.wasm` size |
|---|---|---:|---:|---:|---:|
| wasm32 | emmalloc (current default) | 1 | 1 | 1 | 4,042,023 B |
| wasm32 | dlmalloc | 0.980 | 0.988 | 0.999 | +5.6 KB |
| wasm32 | mimalloc | **0.875** (0.75–1.03) | 1.214 (0.70–1.98) | 0.966 | +60 KB |
| wasm64 | dlmalloc (current default) | 1 | 1 | 1 | 4,374,187 B |
| wasm64 | emmalloc | 1.027 | 1.005 | 1.016 | −6 KB |
| wasm64 | mimalloc | **0.921** (0.78–1.03) | 1.453 (0.84–2.63) | 1.001 | +63 KB |

mimalloc's heap overhead is largest on small scenes, where its fixed
arenas / pages dominate (sceneObject01 16.8 → 33.3 MiB, sceneObject02 +51%). On
the large flattens it is smaller than dlmalloc (sceneObject03 −30%,
sceneObject04 −16%).

The wasm64 build carries `-sASSERTIONS=2`, so compare wasm64 against
wasm64 only.

**Adopted (2026-10-27).**
- Native CLI tools default to the vendored mimalloc
  (`LIGHTUSD_NEXT_ALLOCATOR=mimalloc`), compiled with
  `MI_DEFAULT_PURGE_DELAY=0` (a local hook in `src/util/options.c`; the
  `MIMALLOC_PURGE_DELAY` env var still overrides it). `auto` keeps the old
  installed-library search (jemalloc, tcmalloc, mimalloc). Fresh default
  build on benchmark host: district / reference scene / ALab / full45 hashes unchanged
  (98619c40 / 267d237a / 2f7176c8 / a0175bc1); full45 71.6 s / 27.6 GiB
  peak; ctest 61/61.
- The WASM next module defaults to dlmalloc for both wasm32 and wasm64
  (the legacy module keeps emmalloc; it was not measured). mimalloc stays
  opt-in via `LIGHTUSD_WASM_MALLOC=mimalloc`.

**Verdict (the reasoning).**
- **Native:** mimalloc is the best single choice when tuned. With
  `purge_delay=0` it equals jemalloc's speed at a slightly lower peak on
  full45. Untuned, it is ~9% faster at ~18% more RSS. Vendored, it also
  works where no system jemalloc exists (Windows / macOS / static builds).
  jemalloc remains a sound `auto` default where it is installed. glibc is
  clearly worst (1.39× slower).
- **WASM:**
  - mimalloc is the fastest (−12.5% wasm32, −8% wasm64) but commits
    21–45% more heap and grows small scenes the most. That is a poor
    default for memory-capped browser tabs.
  - dlmalloc is marginally better than the current wasm32 default
    emmalloc (−2% time, −1% heap) for +5.6 KB of code.
  - Suggested: dlmalloc as the default for both wasm32 and wasm64, and
    mimalloc opt-in (`LIGHTUSD_WASM_MALLOC=mimalloc`) for speed-first or
    multithreaded builds, where dlmalloc's global lock serializes threads.

Raw data: `$BENCH_ROOT/{native-small.csv, native-full45.csv,
wasm.jsonl, wasm64.jsonl, mimalloc-tuning.csv}`.
Scripts: `allocbench.sh`, `wasmsuite.sh`, `wasmbench.mjs`,
`wasm64-followup.sh`.

Side findings (not allocator related; follow up separately):
- `RenderStream` aborts on `sceneObject04` (render only; flatten is fine).
- `sceneObject05`, `the large flatten input` and `sceneObject02` render do not finish within
  60 s on any allocator.
- `getMeshCopy` costs ~30 ms per mesh (sceneObject06: 1,080 meshes in 36 s).

### 10.27 The array-cap output discrepancy was silent data loss (2026-10-27)

The open item from §10.25 (`a8f50922…` once vs `267d237a…`) was not a
race. The differing run used `--max-crate-elements 300000000`. With the flag
the output is `a8f50922…` (2,512,205,846 B) every time; without it, it is
`267d237a…` (1,815,093,161 B), also every time. The larger output retains
five arrays that had been silently dropped.

Without the flag, the default 16 M-element array cap silently dropped values
from two geometry payloads. Only the declarations remained. The run exited 0
with no warning. Even a plain `--rewrite-layer` of a large groom payload
produced an 80 KB file.

Three bugs combined:
1. **Regression from `5b78bdef9` (2026-08-04).** The lazy-array probe in
   `UnpackArrayUncached` was capped at `max_array_elements`, so a compressed
   block could not advertise a count it doesn't hold. That also capped
   *uncompressed* arrays, whose `count × stride` bytes must already be in the
   file. Meanwhile the FIELDS validation (`crate-reader-sections.cc`) still
   accepted over-cap lazy arrays. The over-cap value fell through to eager
   decode, which refused it, and `ResolveFieldset` dropped the field with a
   warning.
2. **Warnings never left the reader.** `ConvertLoadedUSDC` (pcp layer
   loading) discarded `USDCLoadResult::warnings`, unlike the USDA path.
3. **The writer skips an unencodable `default` field** (on purpose, so pxr
   can still read the layer). Anything dropped upstream vanished without a
   trace.

Fixes:
- **Uncompressed arrays:** probed uncapped again; their count is file-backed.
  They stay lazy, pass through to the writer byte for byte, and materializing
  them still enforces the cap (`max_elements`).
- **Compressed arrays:** stay capped, and an over-cap compressed array now
  fails the layer load in FIELDS validation, instead of being accepted and
  then dropped field by field.
- **Warnings:** `ConvertLoadedUSDC` forwards reader warnings, prefixed
  `USDC layer <path>:`.
- **`lusdcat`:** prints the `raise --max-crate-elements` hint for this error
  and for composition warnings that carry it.
- **Test:** `test_lazy_array` covers both cases. Built without the fix, it
  fails on `cp->is_lazy()`.

The reference scene without the flag now warns that the two layers failed to
load (composition continues, as for any unloadable arc) and writes
`adf9f5f3…` (1,719,598,316 B). With the flag nothing changes: `a8f50922…`.

The bench kit now passes the flag for the reference scene: `allocbench.sh`,
the manifests and the README reference hash.

The forwarded warnings exposed one more silent drop in a separate scene's
vegetation groom payload, where eight `inactiveIds` fields (`Int64ListOp`)
cannot be decoded by the crate reader. Instance deactivation is lost there;
the output hash is unchanged because that data was not present in the output.
Follow up by supporting `Int64ListOp`.

Verification:
- Hashes unchanged: district `98619c40…`, full45 `a0175bc1…`, merged scene
  `2f7176c8…`.
- ctest 61/61; Debug `test_lazy_array` passes.
- Composition sweep (1,933 inputs) and rewrite sweep (705 fixtures), USDA
  and USDC: byte-identical to R22.
- The reference scene with the flag, 10 plain runs at 2–32 threads: all
  `a8f50922…`.
- TSan build (`-fsanitize=thread`, system malloc), reference scene at 8 and
  16 threads: 0 reports, `a8f50922…`; 78 s / 65 s, ~10 GiB peak.


### 10.28 Structure-walk hand-off granularity, measured (2026-10-27, rejected)

The parallel structure walk (round 11, §10.14) hands a not-yet-walked entry to
another thread whenever that thread is idle (`idle > queued`) and the donating
walk has emitted `donate_after` items. On full45 that produces **2.09 M tasks
for 12.3 M items**: the hierarchy is broad and shallow, so the natural
hand-off unit (a depth-≥ 7 subtree) is ~5 items.

Measured cost (one full45 flatten on benchmark host, `LIGHTUSD_NEXT_TIMING=1`, temporary
per-task timers; R22 = current `main`-line tree):

| counter | value |
|---|---:|
| walk items / tasks | 12.3 M / 2.09 M |
| tasks emitting ≤ 2 items | 1.41 M |
| task walk time, summed over threads | 103.7 s |
| of which the item loop | 92–104 s |
| per task | ~40–50 µs |
| `add_task`, summed (mutex hold only 1.0–1.7 s) | 78–85 s |
| worker queue section, summed (cv idle 24–37 s) | 108–121 s |
| commit | 9.8 s, of which 6.5 s waiting on tasks |
| materialize | 3.1 s |
| structure phase (wall) | 13.5–15.0 s |

So the walk burns ~104 s of thread time where the intrinsic per-item cost is
~1.15 µs (measured by walking the whole tree on one thread: 10.2 M items in
11.8 s), and the in-order commit waits behind the resulting task stream. The
sequencer is the task hand-off itself: ~4 M mutex acquisitions (one add, one
pop per task) with a 0.5 µs critical section but a ~2.5 µs futex hand-off.

Note the host is 16 physical cores / 32 SMT (a 32-thread CPU loop scales 1 →
32 as 19x), so the walk also over-subscribes its own 32 threads.

**Attempt 1 — bundle consecutive DFS entries per task.** The stack pops in
pre-order, so the top `k` entries are exactly the next `k` DFS positions; a
task can own that contiguous range (one placeholder, one task, `end` unchanged
per frame). A per-root parent list in the task (`WalkCommitCtx::task_parents`)
maps each root back through the committing frame's `gidx`.

Bundling alone does not work, because a hand-off of `k` entries transfers only
~5 k items while the donor still walks `donate_after` (512) items between
hand-offs: the top phase ends up walking the tree itself.

| bundle | tasks | commit | materialize | structure | output |
|---:|---:|---:|---:|---:|---|
| 1 (R22) | 2.09 M | 9.8 s (6.5 wait) | 3.1 s | 13.5–15.0 s | reference |
| 4 | 19 k | 2.3 s | 3.2 s | 17.9 s (top = 10.7 s) | `a0175bc1…` |
| 8 | 18 k | 2.4 s | 3.1 s | 11.5 s | `a0175bc1…` |
| 12 | 16 k | — | — | 24.9 s (serial fallback) | `a0175bc1…` |
| 16 | 14 k | — | — | 23.1 s (serial fallback) | `a0175bc1…` |
| 32 | 5.8 k | — | — | 24.8 s (serial fallback) | `a0175bc1…` |
| 128 | 4.9 k | — | — | 22.0 s (serial fallback) | `a0175bc1…` |

Bundle 8 is the only size that both stayed on the parallel path and was
byte-identical — and it saves only ~2 s because the top phase then walks
3.7 M items (4.4 s) and the workers only get 8.9 s of work.

**Blocker — the instance-guess fallback is scheduling dependent.**
`bundle ≥ 12` hits `walked_as_instance && !is_instance` in `CommitWalk`: a
prim was claimed as an instance because the registry held an *earlier* rank
for its key, but the prim holding that rank sits inside a subtree the commit
dropped (it walked as a prototype and was an instance). The prim then turns
out to be the prototype while its subtree was never walked, and the walk
aborts to the serial path (structure ~24 s, i.e. 1.7x).

Its probability is a function of the *split*, not of the bundle code: R22 never
hit it on full45 in these runs, bundle 8 never hit it, bundle ≥ 12 always did.
Fixing it properly means resolving that prim at commit time (mark it, pause the
walkers, re-walk the prim as a prototype at its DFS position and commit that
in its place — the item must be replaced, not committed, and no later item in
the frame may refer to it).

**Attempt 2 — that re-walk (rejected).** Implementing the resolution removes
the fallback (`fails=0`) but changes instance decisions relative to the serial
walk: the district composed 734,638 prims instead of 735,484 (846 missing,
different per run), and the USDC hash moved away from `98619c40…`. The
re-walked prim claims its key again at rank `[k, 0]`; the registry's
"held rank is a proper prefix of ours" rule accepts it, so the prim expands —
but the *other* prims' claims are still shaped by the speculative pass, and
the dropped-subtree case the re-walk papers over is exactly the case where
those claims are stale. Reverted; the conservative fallback stays.

**Writer pipeline, same session (no change adopted).** Temporary per-stage
timers in `BuildFieldsAndSpecsParallel` on full45 (`fields_specs` 12.2 s):
encode summed over 16 workers 79–89 s, A1 0.8 s, **A2 7.1–7.6 s (serial,
61.8 M path records), B 6.8 s, W 4.8–5.5 s, C 6.0–6.2 s**, gate waits 18–24 s,
worker wait for a ctx 32 s. The wall (12 s) exceeds the slowest stage (A2,
7.1 s): the ordered pipeline loses ~5 s to hand-off/stall, not to CPU
contention. Rejected experiments: `kFieldsChunkPrims` 1024 → 4096 (12.36 s,
no change), ctx pool 2 W → 4 W (12.01 s), encode workers 8 / 12 / 16 (13.9 /
12.5 / 12.3 s — 16 is best). A2, B and C are single-threaded ordered appends;
parallelising one of them is the next real lever here.

**State.** No code change from this section; the tree is R22 (2ec4f06f0) and
all hashes still match: district `98619c40…`, full45 `a0175bc1…`, merged ALab
`2f7176c8…`, reference scene (with `--max-crate-elements`) `a8f50922…`; ctest 61/61.

### 10.29 Parallel first-use interning of the prim/type/property-name tables (2026-10-27)

`BuildTables` was the last fully serial phase in the write path (3.59 s on
full45). Split measured with temporary timers:

| part | full45 |
|---|---:|
| prim names (9.1 M distinct of 10.2 M `InternToken` calls) | 1.40 s |
| type names (`InternToken`, 10.2 M calls) | 0.60 s |
| property slots (51.6 M iterations, interning only the first use of each id) | 0.59 s |
| metadata probing + `InternString` (doc, apiSchemas, edits, variantSets, references, inherits) | 1.00 s |

None of it can be reordered: the crate's token indices are the table order, so
the fix keeps the serial order **by construction** and only parallelises the
work that decides it.

- **Collect (parallel, one slice per thread).** Each slice walks its prim
  range exactly like the serial loop and records, per text, the smallest
  `(prim index << 32 | item ordinal)` it sees, in a per-slice
  `unordered_map<string_view, uint64_t>` (token texts and string texts
  separately — `InternString` interned the token too, so a string item gets
  the same key in both maps). Property names are deduped per slice through a
  small `prop_seen` byte vector keyed by `PropNameId`, so the 51.6 M repeat
  slots never touch the hash.
- **Merge (serial).** Fold the slices into one map keeping the minimum key —
  the first use of a text is in the lowest slice, so a text seen earlier wins.
- **Replay (serial).** Sort by key and `InternToken` / `InternString` in that
  order: exactly the serial sequence of table insertions.
- **Id arrays (parallel).** `prim_name_tokens_` is now filled by a read-only
  concurrent scan of the (now immutable) token map, and `prop_name_tokens_`
  from the seen ids. The values are order independent.

`BuildPrimTablesParallel` returns false (serial loop) for layers under 65 536
prims, in non-threaded builds, and when a deferred-fill layer is present
(that build interns per prim at encode time by design).

Results (`tables` lap; paired, `/dev/null`):

| scene | baseline | candidate | elapsed (paired median) | peak RSS |
|---|---:|---:|---:|---:|
| district | 234 ms | **180 ms** | — | — |
| reference scene 8/8/8 | 94 ms | **58 ms** | 0.960 | 0.999 |
| merged ALab | 8.2 ms | 8.2 ms | 1.012 (0.83 s run; unchanged path) | 0.997 |
| full45 | 3605 / 3566 ms | **2087 / 2071 ms** | 0.962 (69.09 → 66.49 s) | 1.004 |

full45 run-to-run spread in this pair was dominated by the structure walk
(12.2–14.8 s across four runs), not by this change; the `tables` delta
(-1.5 s) is the clean signal. Transient RSS: the per-slice maps hold ~891 K
distinct tokens, ~+80 MB, inside the 1.05x budget.

What is left in the 2.1 s: collect 0.82 s, merge 0.58 s, sort+intern 0.51 s,
prim/property id fill 0.11 s. The merge is the next target — a k-way merge of
per-slice key-sorted runs would intern directly and drop the global map's
~5 M string-keyed probes (~0.6 s). Intern (0.3 s of it) is the ~891 K real
insertions and is inherent.

Verification:
- Hashes unchanged: district `98619c40…`, full45 `a0175bc1…`, merged ALab
  `2f7176c8…`, reference scene (with `--max-crate-elements`) `a8f50922…`.
- Rewrite sweep, 227 fixtures × {USDA, USDC} = 454 outputs, byte-identical to
  `lusdcat-base` (R22) — this is the token-order check.
- Staged mode (`WriteLayerToMemory`): parallel and `SERIAL_FIELDS=1` writes of
  the district are byte-identical.
- ctest 61/61 (1 skip). Non-threaded build compiles and its district hash
  matches; the `LIGHTUSD_NEXT_DEFERRED_FILL=ON` build compiles and still takes
  the serial path (`tables` 69 ms on the district).

### 10.30 OpenUSD v26.11.1 on the reference scenes (2026-10-27)

Reference measurement of the same work in pxrUSD, for context on what the
rounds above bought. Full write-up: [large-scene-vs-openusd.md](large-scene-vs-openusd.md);
raw records + scripts: `$BENCH_ROOT/usd-26.11-vs-lightusd.md`.

Build: v26.11.1 (96c198916), Release, `PXR_ENABLE_PYTHON_SUPPORT=ON`,
imaging / GL / Vulkan / usdview / tests / examples / all image+geometry
plugins OFF (6m44s, 3291 targets). Boost is **not** required in v26.11.1
(only with OpenVDB). The benchmark host has no Python dev headers and no sudo, so
CPython 3.13.7 was built from source for the bindings.

Phases measured with `bench/usd_phase_bench.py`
(`Open` / `Traverse` / `Flatten` / `Sdf.Layer.Export`), cross-checked with
`usdcat -f --usdFormat usdc` (126.7 vs 126.1 s on the district). LightUSD is
`lusdcat -f ... --output-format usdc` (§4.1 defaults). Both tools read the
same input file: the bench wrappers nest `over` prims on one line, which
pxr's PEGTL text parser rejects ("Expected }"), so they were re-emitted one
prim body per line; `lusdcat` on the reformatted wrappers reproduces
`98619c40…` / `a0175bc1…` exactly, so the reformat is semantics-preserving.
(Side note: a prim body opening on the same line as its parent's `{` is
accepted by LightUSD's parser and rejected by pxrUSD's.)

District (`district scene`, 1 district, 724 700 stage prims, 9.5 M paths):

| phase | pxrUSD 26.11.1 | LightUSD | ratio |
|---|---:|---:|---:|
| load + compose (`Open`+`Flatten`) | 80.33 s | 3.92 s | 20.5x |
| write (`Export` → USDC) | 45.20 s | 2.33 s | 19.4x |
| total | 126.10 s | 6.25 s | 20.2x |
| peak RSS | 11.35 GB | 4.79 GB | 2.37x |
| output | 500.9 MB | 580.3 MB | |

Cumulative districts (`cumulative scene set`, paths measured on the USDC):

| n | stage prims | paths | pxr total | pxr RSS | LightUSD total | LightUSD RSS | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 724 700 | 9.5 M | 126.1 s | 11.4 GB | 6.3 s | 4.8 GB | 20.2x |
| 5 | 1 632 126 | 23.8 M | 286.8 s | 20.5 GB | 11.8 s | 10.5 GB | 24.3x |
| 15 | 2 015 184 | 28.8 M | 340.3 s | 23.2 GB | 14.4 s | 12.6 GB | 23.6x |
| 45 | 10 187 143 | ~132 M | not runnable | 24.2 GB (load only) | 63.2 s | 29.9 GB | — |

full45 (`full45 scene`, all 45 districts): pxr's load alone is 34.2 s
(`Open`) + 8.4 s (`Traverse` 10.2 M prims) at 24.2 GB — 0.67x LightUSD's whole
compose+write. `Flatten` memory grows ~0.6 GB per 1 M written paths beyond the
stage (11.4 → 20.5 → 23.2 GB), i.e. ~100+ GB at ~132 M paths, over this host's
60 GiB, so pxr cannot flatten the full scene here (runs were capped with
`ulimit -v 44000000`).

Reading:
- pxr's `Open` is lazy; nearly all composition cost sits in `Flatten`.
  LightUSD's single `load+compose` does that work in 20-24x less time.
- pxr's flattened-layer `Export` is 19x slower than LightUSD's streaming
  writer; per output byte ~23x (LightUSD's crate is 15% larger).
- Composition scale is comparable (pxr 724 700 vs LightUSD 735 484 prims on
  the district, 1.0-1.5% apart), so the ratios are not an input-size artefact.

### 10.31 `fields_specs` pipeline: where the 12 s actually goes (2026-10-27)

Round 23 left the write phase's largest lap unexamined at stage level.
Temporary timers in `BuildFieldsAndSpecsParallel` (per-stage work and gate
waits, per-chunk completion timestamps, worker `Reset()` / notify / context-wait
accounting) on full45:

| measurement | value |
|---|---:|
| `fields_specs` lap | 12.0–12.2 s |
| last chunk **encoded** | 10.41–10.53 s |
| stage drain after that (A1 → A2 → B → W → C) | 15–20 ms |
| stage work / gate (s): A1 | 0.79 / 9.6 |
| A2 | 7.46 / 2.9 |
| B | 6.80 / 3.6 |
| W | 4.66 / 5.7 |
| C | 5.55 / 4.7 |
| workers: encode / `Reset()` / context wait | 77.5–79 / 4.0–4.3 / ≈86 s |
| worker utilisation | 46–47 % |
| `notify_all`, `done[]` lock | 0.10 s, 0.006 s |
| A2 split: probe / **serial append** / insert | 2.41 / **3.73** / 1.63 s |

So the lap ends when the last chunk is *encoded* — the ordered stages are
never the tail — yet the encoders are idle half the time, and every downstream
stage is idle 25–75 %. The ring is in-order (chunk k must traverse A1→C in
sequence) and back-pressured by the context pool.

Worker-count fit (`--write-threads` 2/4/8/16 → 31.7 / 19.6 / 13.4 / 12.0 s)
gives `t ≈ 7.4 s + 48.5 s / W`: a **fixed ≈7.4 s floor that matches A2's
single-threaded work**, plus encode work divided by W. (Fitted on W=2/4; the
model under-predicts W=16 by ~14 %, i.e. large W also loses to contention —
see the work inflation below.) (The encode *total work*
also grows with W: 50.9 s at W=2 → 79 s at W=16, +55 % — the encoders do not
scale, which is a separate contention effect.)

Three fixes were tried and measured on full45, all reverted:

1. **Per-stage condition variables + `notify_one`** instead of one shared
   `mut`/`cv` with `notify_all` on every chunk completion (the hand-off
   wake-herd looked like the culprit: ~0.3–1 ms per hand-off, ≈100× a normal
   wakeup). Result: 12.14 → 11.99 s, last-chunk 10.53 → 10.41 s, `notify`
   cost 101 → 46 ms — a 1 % win only, so the herd is *not* the limiter.
   Reverted rather than carry six condition variables for 1 %.
2. **Parallel ordered append** in A2 (count each contiguous event slice's
   misses → prefix sum → each slice writes its own pre-assigned contiguous
   range, ids identical to the serial order). Result: append 3.73 → 4.90 s,
   A2 7.77 → 8.68 s, lap 11.7 → 12.9 s — worse. Two extra shard-team
   rendezvous per chunk (4 → 6) and a second pass over the events cost more
   than the parallel stores save.
3. **Whole-store adoption** (move the worker's `path_store` deque per chunk
   instead of moving + re-splitting each new path's transient text, so the
   events' stem/name views stay valid untouched). Result: 12.18 s — no change.
   The append is memory-bound (≈60 ns per path record over 61.8 M records,
   first-touch stores into 2.5 GB), not adoption-bound.

**Verdict.** The lap is bounded by A2's per-record building (≈7.4 s,
memory-bound: probe 2.4 s over 61.8 M keys, append 3.7 s of first-touch stores
for `paths_` + `path_hashes_`, insert 1.6 s) plus `encode_work / W`
(≈3 s at W=16, with W the effective ceiling 16). Synchronisation is not the
lever; the remaining candidates are structural: slim the per-path record (the
side array `path_hashes_` alone is 8 B × 61.8 M = 0.5 GB of stores and RSS —
the shard slots already carry fingerprints), or reduce the encode work growth
(+55 % from W=2 to W=16). Both are multi-hour changes with a bounded ~3–4 s
payoff; the W-fit says nothing below ≈7.4 s is reachable without changing what
A2 stores.

### 10.32 Warm merge: pipelined worker teardown and parallel spec adoption (2026-10-27)

The warm phase's merge (rounds 8–10) was 7.4–7.9 s of the 16.5 s warm on
full45. Phase timers in `MergeSources` (summed over the 32 worker merges,
`LIGHTUSD_NEXT_TIMING`; total `merge_loop` 5.97 s + `destroy` 1.21 s):

| phase | s | parallel? |
|---|---:|---|
| prescan (probe the main cache, record first refs) | 0.91 | yes |
| adopt stacks/mappings in first-reference order | 0.59 | **no** |
| build remapped chain / site objects | 0.80 | yes |
| adopt worker `StackSpecCache` entries | 1.15 | **no** |
| remap `Src` records in place | 0.95 | yes |
| **move keys + value vectors into the main cache** | **1.37** | **no** |

The prescan and remap walk the same ~90 M cold `Src` records (~7 GB each), so
they are bandwidth-bound and near their floor; the serial pieces were the
target.

**1. Pipelined worker teardown.** `MergeSources` adopts or remaps every
reference a worker has, so the worker can be released while the next one
merges. The loop used to call `workers[t].reset()` inline (1.21 s of the merge
wall, i.e. 16 % of it, doing nothing). One helper thread now takes the victim
on a one-slot queue (so at most two workers are alive, as before — the peak-RSS
overlap is unchanged) and destroys it in the shadow of the next merge:
`destroy` 1207 → **0.2 ms**, merge 7441 → 6402 ms in the same run.

**2. Parallel per-stack spec adoption.** The worker `StackSpecCache` entries
were inserted into the main per-stack caches in one serial loop. The target
stack of each worker stack is resolved serially (adoption mutates the main
tables) and the main vector is pre-sized, then the insertions run on all
threads with a single atomic work counter — the target caches are disjoint, so
this is embarrassingly parallel and order-free (a hash map's insertion order
carries no meaning). `spec` 1180 → **421–456 ms**.

Results (paired, `/dev/null`, full45, baseline = R22 before round 23):

| | baseline | now | ratio |
|---|---:|---:|---:|
| `next_warm.merge_ms` | 7894.9 | **5530.2** | 0.700 |
| `next_warm.merge_loop_ms` | 6228.0 | 5193.4 | 0.834 |
| full45 elapsed (median, n=2) | 68.94 s | **64.77 s** | **0.9395** |
| peak RSS | 30.05 GB | 29.99 GB | 0.998 |
| merged ALab elapsed / RSS | 0.85 s | 0.84 s | 0.988 / 0.998 |
| reference scene elapsed / RSS | 4.18 s | 4.12 s | 0.986 / 0.990 |

(The elapsed ratio includes round 23's `tables` change: 3607 → 2080 ms; the
warm work alone is the −2.37 s in `merge_ms`.)

What is left in the 5.1 s merge: prescan 0.70, adopt 0.65, chain 0.83, spec
0.43, remap 0.98, **insert 1.36 (serial)**. The insert is one random probe per
entry into a 10.2 M-slot table (163 MB) — memory-latency bound at ~130 ns per
entry — so parallelising it needs a sharded index (per-shard slots + value
deques, since the value `std::vector<Src>` addresses are held by the walk).
Expected ~1 s, and safe for the merge's ordering semantics: seeded entries are
skipped by the prescan and a worker's own insertions stay in insertion order,
so the first-reference adoption order (the only order that affects ids) does
not change.

Verification: district `98619c40…`, full45 `a0175bc1…` (2 runs), merged ALab
`2f7176c8…`, reference scene `a8f50922…`; composition sweep 1933 inputs × {USDA, USDC}
all identical to R22 (3866/3866); rewrite sweep 705 × 2 all identical
(1410/1410); staged `WriteLayerToMemory` parallel-vs-serial identical; ctest
61/61; non-threaded build compiles and produces the district hash.

### 10.33 Warm-merge insertion prefetch probe (2026-10-27)

Prefetching every taken entry among the next eight worker entries, instead of
only the first, was tested against the round-24 binary on district compose.
With one warmup and three alternating pairs, median elapsed increased by 2.4%
(candidate/baseline 1.024); peak RSS ratio was 1.004 and sampled process swap
was zero. This is a district-only measurement, not a full45 result. The change
was reverted; sharding the source cache remains the larger insertion lever.

### 10.34 Parallel warm-merge index insertion (2026-10-27)

The merge now appends the taken keys and address-stable source vectors in their
original order, then inserts their already-known hashes into the source-cache
index in parallel. The prescan has established that the keys are distinct and
absent. Threads atomically claim empty slots; readers resume only after the
threads join. The serial path remains for small merges, tables with tombstones,
index overflow, and compilers without the atomic builtins. This preserves the
entry order used for first-reference adoption without sharding the value store.

Against round 24, full45 alternating pairs (n=2, no warmup, `/dev/null`) had
median elapsed 66.19 -> 64.85 s (ratio 0.980), warm merge 5458.5 -> 4977.4 ms,
and maximum RSS ratio 1.000. District (n=3, one warmup) had median elapsed
ratio 1.019, while warm merge fell 636.8 -> 604.5 ms. ALab elapsed/RSS ratios
were 1.000/1.001 and reference scene 1.012/1.004 (n=3, one warmup). All paired runs
reported zero sampled process swap. Small-scene elapsed variation exceeds the
warm-merge saving, so the district and reference scene overall regressions are recorded
rather than attributed solely to this change.

Full45 output SHA-256 `a0175bc1...`, ALab `2f7176c8...`, reference scene `a8f50922...`,
and the non-threaded district `98619c40...` match their references. The
composition sweep matched 3866/3866 outputs, native CTest passed (60 passed,
1 skipped), and the non-threaded build compiled. No writer code changed.

### 10.35 Ordered slice replay for writer tables (2026-10-27)

The per-slice token/string first-use maps are converted to key-sorted runs in
parallel. Slice prim ranges are contiguous and ordered, so serial replay of the
runs in slice order preserves first use, including when a text appears in more
than one slice. `InternToken` and `InternString` deduplicate those repeated
texts. This eliminates the separate global minimum-key maps and their string
probes; token runs are replayed before string runs as in the previous code.

Against round 25, full45 alternating pairs (n=2, no warmup) reduced the
`tables` lap from 2029.1 to 1488.9 ms; median total elapsed 64.65 -> 63.78 s
(ratio 0.986), maximum RSS ratio 1.003. District (n=3, one warmup) reduced
`tables` from 157.9 to 139.0 ms, while overall elapsed ratio was 1.035. ALab
elapsed/RSS ratios were 1.012/1.000, reference scene 0.993/1.000 (n=3, one warmup).
All paired runs had zero sampled process swap. The short-scene elapsed changes
are reported as measured and do not establish a causal regression.

Full45, ALab, reference scene, and non-threaded district hashes match their references;
the 705-fixture rewrite sweep matched 1410/1410 outputs. Staged parallel and
serial output matched each other and the pre-change staged harness byte for
byte. Native CTest passed (60 passed, 1 skipped); non-threaded and deferred-fill
builds compiled. The full45 table result is the useful signal; whole-run times
include structure-walk variation.
This describes round 26's measured intermediate implementation. Round 27
replaced the sorted temporary runs with first-use event vectors to remove the
per-slice sort; see §10.37 for its separate district measurement and pending
full45 check.

### 10.36 Teardown worker-count and source-cache clear probes (2026-10-27)

Raising the `Layer` destructor's worker cap from 16 to 32 reduced one full45
teardown observation from 5.8 to 5.4 s, but paired total elapsed was 1.003x
round 26 and teardown observations overlapped (baseline 5.37/5.26 s, candidate
5.40/5.37 s). Peak RSS ratio was 0.999. Reverted: the apparent first-run
change did not repeat in the pair.

A second probe cleared the `SrcCache`'s per-entry vectors/strings on 16 threads
before releasing its deques. District median elapsed was 1.005x and reclaim
was 265–285 ms, indistinguishable from the 265–277 ms baseline. On full45 the
first candidate/baseline pair measured reclaim 1644/1650 ms and elapsed
65.23/65.69 s, but the harness refused the next pair because host available
memory fell below its 4 GiB reserve (the host also had essentially all swap
occupied). The experiment was reverted; the partial full45 pair is not a
performance claim.

### 10.37 First-use event vectors for writer tables (2026-10-27)

Each table-building slice now stores a text in an `unordered_set` and appends
it to an event vector only on first insertion. Since the scan visits prims in
ascending order and slices cover consecutive prim ranges, these vectors are
already the first-use sequence; this removes the per-slice sort and stores
string views rather than `(key, string_view)` pairs. District hash remains
`98619c40…`; the 705-fixture rewrite sweep is 1410/1410. Paired district
elapsed ratio was 0.979 (three pairs) with peak RSS ratio 1.008, though the
individual timings were noisy. In the combined round-27–30 full45 run the table lap was effectively flat
against round 26 (median 1612 vs 1605 ms); see §10.41. The event vectors
replace the sort while keeping peak RSS within the required limit.

### 10.38 32-bit path-hash side table (2026-10-27)

`path_hashes_` stores only the low 32 bits. The path-index slot already stores
the high 32 bits, so lookup still checks the exact 64-bit hash in two pieces;
rehash reconstructs the original hash from the slot tag and side value. This
halves the side array (about 0.5 GiB to 0.25 GiB on full45) and keeps path
record ids and probe order unchanged. District hash is unchanged; paired
district peak RSS ratio was 0.990. ALab and reference scene paired elapsed/RSS ratios
were 1.000/1.005 and 1.012/0.994, and all three hashes match. The combined full45 run in §10.41 confirms output identity and a 0.992 peak
RSS ratio against round 26.

### 10.39 Sparse ordered PATHS token replay (2026-10-27)

`RegisterPathElementTokens` already runs a parallel pre-pass that identifies
path records whose components may be absent from the starting token table. The
serial replay now stores only those record indices in per-slice ordered lists
and skips the second scan over every path. For each selected record, the prior
non-root stem is recovered from the adjacent path records; skipped records
cannot introduce tokens because the pre-pass found all their components in
the initial table. District output remains `98619c40…`; the path-token lap
moved from 151–152 to 140–148 ms. Overall district paired elapsed was 1.018x
and peak RSS 1.016x vs round 26, so this small-scene comparison does not show a
whole-run benefit. On full45 the path-token lap fell from a 2162 ms baseline median to 2052 ms
in the round-27–30 candidate pair; the full hash and RSS also pass (§10.41).

### 10.40 Packed per-slice property-name flags (2026-10-27)

`BuildPrimTablesParallel` now stores each slice's seen-`PropNameId` flags as
64-bit words instead of one byte per id. The main thread merges slices with
wordwise OR and visits only set bits when filling the id array. Property IDs
are still interned during the original prim-order event replay; this bitmap
only controls the order-independent lookup array. District output remains
`98619c40…`; paired district elapsed ratio was 0.982 and peak RSS ratio 0.990
(n=3, one warmup), though the `tables` lap itself was within run-to-run noise.
The rewrite sweep matched 1410/1410 outputs. The full45 pair had elapsed
ratio 0.974, peak RSS ratio 0.992, and zero sampled swap; the full output hash
is unchanged (§10.41).


### 10.41 Combined rounds 27–30 full45 verification (2026-10-27)

Against the round-26 binary, two alternating full45 flatten pairs (`/dev/null`,
zero warmup) produced median elapsed 68.46 -> 66.69 s (ratio 0.974) and maximum
peak RSS 29,965,632 -> 29,729,176 KiB (ratio 0.992). All four runs had zero
sampled process swap. The PATHS token lap median fell 2162 -> 2052 ms; tables
were 1605 -> 1612 ms, within run-to-run noise. One candidate full45 output has
SHA-256 `a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.

Current ALab and reference scene output hashes remain `2f7176c8…` and `a8f50922…`;
paired elapsed/RSS ratios are 1.000/0.997 and 0.993/0.992. District and
non-threaded district hashes remain `98619c40…`. The 705-fixture rewrite sweep
matched 1410/1410. Staged parallel, staged serial, and the pre-change staged
harness outputs match byte-for-byte (`98ad30c3…`). Native CTest passed 60 tests
with one expected skip; non-threaded and deferred-fill builds compile.

A separate full45 rewrite-only pair was attempted against the 3.01 GB flattened
USDC file. It was stopped by the standard 4 GiB headroom monitor when the
reader/writer process reached about 42 GiB RSS. It produced no valid timing
pair and is not used as evidence; the paired full45 flatten covers the current
writer path while staying within the host reserve.

### 10.42 Compact path records (2026-10-27)

`PathRec` previously held two `std::string_view`s (32 bytes). It now stores two
data pointers and two 32-bit lengths (24 bytes; enforced with `static_assert`),
and returns ordinary `string_view`s to existing readers. Empty views use a
non-null empty-string pointer. The record still references the same stable
layer, property-name, or owned-path storage; hash and path ordering are
unchanged. At 61,833,354 full45 paths, the path vector is 494,666,832 bytes
smaller (about 472 MiB), before allocator effects.

The round-31 full45 pair against round 30 had median elapsed ratio 0.993 and
maximum peak-RSS ratio 0.985; all four runs sampled zero process swap and met
the 4 GiB reserve. The direct full45 USDC output SHA-256 matches round 30:
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.
District compose paired elapsed/RSS ratios were 0.986/1.022 (three repeats,
within the 5% RSS gate). ALab and reference scene paired runs also passed the 5% RSS
gate, with zero sampled swap: 0.996 and 1.001 peak-RSS ratios respectively.
Their elapsed medians were affected by cold-page and run-order variance and
are not treated as performance claims. The native build completed after the
change. The 705-fixture rewrite sweep matched 1,410/1,410 outputs, and staged
parallel/serial/pre-change files matched byte-for-byte (`98ad30c3…d022a54`).

### 10.43 Compact path hash fingerprints (2026-10-27)

Round 32 removed the low-hash side array entirely and compared path text after
the slot's 32-bit tag matched. Full45 output stayed identical and peak RSS
fell (max ratio 0.986 vs round 30), but median elapsed regressed 2.5% (ratio
1.025). The extra path-string comparisons are not worth that loss, so the
zero-side-array variant was replaced.

Round 33 keeps only the low 16 hash bits per path; the slot's high 32 bits plus
this value form a 48-bit lookup fingerprint. Exact stem/name comparison still
resolves fingerprint collisions. Rehash recomputes the full hash from the
stable path views, avoiding the old 32-bit-per-path side array. This leaves a
118 MiB fingerprint vector at 61.8M paths, compared with 236 MiB for the
round-31 32-bit low-hash array.

Against round 30, the full45 paired median elapsed ratio was 0.985 (64.50 to
63.52 s) and maximum peak-RSS ratio was 0.984 (29,767,828 to 29,296,912 KiB);
all runs sampled zero process swap and passed the 4 GiB reserve. The full45
file SHA-256 remains `a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.
District elapsed/RSS ratios were 0.997/0.996; ALab and reference scene passed the RSS
gate at 1.001 and 0.963, respectively, with zero sampled swap. The 705-fixture
rewrite sweep matched 1,410/1,410; staged parallel, serial, and pre-change
outputs matched (`98ad30c3…d022a54`). The native build completed.

### 10.44 Bounded top-level structure-walk queue probe (2026-10-27, rejected)

The top-level speculative walk currently enqueues every eligible depth-2
subtree, unlike worker donation, which checks idle workers against queued
work. A temporary cap of four queued tasks per worker let the top walk process
eligible entries inline while the queue was full. District composition stayed
byte-identical (`98619c40…`); paired elapsed/RSS ratios were 0.997/0.976.

The first full45 attempt stopped when the benchmark's live monitor observed
less than its required 4 GiB reserve. After removing duplicate verified
scratch outputs from tmpfs, the paired run completed against round 33: elapsed
median 62.935 -> 63.995 s (ratio 1.017), maximum peak RSS ratio 1.008, and
zero sampled process swap. The structure lap median increased 13.66 -> 14.23 s.
This is a target-scene regression, so the cap was reverted and the native
build refreshed to match the source. The slower candidate's full45 output was
not separately hashed; the district hash remained `98619c40…`. Do not keep
this queue cap.

### 10.45 Depth-threshold donation probe (2026-10-27, rejected)

Round 35 changed worker donation from depth >= 2 to depth >= 3, leaving the
top-level task queue unchanged. District elapsed/RSS ratios were 1.020/1.013.
On paired full45 runs against round 33, elapsed median rose 62.125 -> 62.980 s
(ratio 1.014), peak RSS ratio was 1.003, and sampled child swap stayed zero.
The structure lap increased 12.94 -> 13.70 s. Reverted; no hash or sweep was
run for this slower candidate.

### 10.46 Compose-thread scaling (2026-10-27, no default change)

The same R33 executable was measured across compose thread counts. On the
district flatten, medians were 18.40 s at 1 thread, 8.33 at 4, 6.65 at 8,
and 6.12 at both 16 and 32. Peak RSS rose from 3.65 GiB at 16 to 3.99 GiB
at 32. Full45 paired samples at 16, 24, and 32 threads were noisy and close:
medians were 62.87, 62.87, and 63.64 s; 16 used less peak RSS than 32 (27.70
vs 27.81 GiB). At 32, warm workers were faster but structure was slower.
These data do not justify changing the automatic thread policy.

A full45 compose-only serial probe reached about 36 GiB child RSS and the
runner stopped it when available memory crossed the required 4 GiB reserve.
It produced no timing result; the reserve guard was left unchanged.

### 10.47 A2 path-slot prefetch distance (2026-10-27, rejected)

Doubling the stage-A2 path-slot prefetch distance from 8 to 16 was paired
against round 33 at 16 compose threads. Elapsed medians were 64.40 and
63.96 s (ratio 0.993), while `fields_specs` was effectively unchanged at
10.71 and 10.72 s. Peak RSS stayed within the gate and sampled child swap was
zero. This small elapsed difference is within run variation; restored the
distance to 8.

### 10.48 Removing the low path fingerprint (2026-10-27, rejected)

With the 16-bit side fingerprint removed, lookup still filtered on the slot's
32-bit tag and resolved matches with exact stem/name equality. Against round
33 at 16 compose threads, elapsed medians were 63.705 and 63.835 s and the
`fields_specs` median increased 7.4% (10.72 -> 11.51 s). Maximum RSS was
effectively flat; no swap was sampled. The side fingerprint was restored.

### 10.49 Inline small instance-key sets in the speculative walk (2026-10-27, rejected)

`WalkLocalKeys` stores the first eight keys as `string_view`s into the owning
`WalkItem::ikey` strings,
using an `unordered_set` only after the inline capacity is exceeded. The
strings remain owned by the task items until the speculative walk completes
and commit consumes them.

District paired elapsed/RSS ratios were 0.990/0.997 (three runs). The first
five full45 pairs showed a lower unpaired candidate median, but the follow-up
five pairs showed the same wide run-to-run spread. Across all ten matched
pairs, the median baseline-minus-candidate elapsed difference was -0.165 s
(candidate slightly slower); the corresponding structure-lap difference was
only +0.252 s. The unpaired medians (63.665 vs 62.60 s) were skewed by
variation between runs and do not establish a gain. Maximum peak RSS ratio
was 1.006 and sampled child swap was zero. Reverted the inline set.

Before reverting, the emitted full45 USDC was byte-identical to the saved reference:
3,014,654,530 bytes, SHA-256
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.
ALab elapsed/RSS ratios were 1.000/1.010; reference scene ratios were 1.013/0.998;
both passed the 5% RSS gate with zero sampled child swap. The composition
identity sweep covered 1,934 listed inputs (3,868 USDA/USDC outputs), all
`SAME` against R33. These checks confirm correctness, but the timing result
does not justify retaining the source change.

### 10.50 Structure-walk task-shape profile (2026-10-27, instrumentation removed)

Temporary counters on two same-binary full45 runs observed 1.75 M and 1.88 M
tasks, respectively, confirming substantial schedule variation. Only 4,362
and 4,394 tasks recorded any local instance key (about 0.25%); the rest left
the task-local set empty. This makes avoiding its occasional node allocation
too small to explain a multi-second change. Rank depths and items per task
were also highly skewed: most tasks emitted one or two items, with a long
tail. The counters were removed after collection; no timing claim is based on
these instrumented runs.

### 10.51 Borrowed instance-key views (2026-10-27, rejected)

After the task-shape profile showed few keys per task, the task-local
`unordered_set` was changed to store `string_view`s into `WalkItem::ikey`
instead of copying each key. District paired elapsed/RSS ratios were
1.020/1.014; the structure lap median rose 1.139 -> 1.220 s. Reverted without
a full45 run. The owned-string set remains the simpler baseline.

### 10.52 FIFO structure-task queue (2026-10-27, rejected)

The queue was changed from a rank-ordered priority heap to FIFO, based on
top-level tasks being discovered in DFS order. District elapsed/RSS ratios
were 0.989/1.002. On full45, four of five matched pairs were slower; the
median paired elapsed penalty was 1.82 s, with a 0.36 s median structure-lap
penalty. Maximum RSS ratio was 1.001 and sampled child swap was zero. Reverted:
the explicit DFS-rank priority improves the target workload.

### 10.53 Inline DFS-rank storage (2026-10-27, rejected)

Round 43 replaced each speculative walk's `vector<uint32_t>` rank with a small
inline buffer (eight entries) and heap overflow for deeper ranks. This was
intended to remove the common short-rank allocation from task creation and
priority-queue comparisons. On five paired full45 runs against round 33, the
median baseline-minus-candidate elapsed delta was +0.51 s (candidate faster),
but only three of five pairs favored the candidate; structure-lap median delta
was +0.524 s. The gain is within observed run variation. District elapsed was
2.2% slower and reference scene 1.0% slower; ALab was flat. Maximum peak-RSS ratio was
0.998 and timed child swap was zero.

Correctness passed: the full45 USDC was byte-identical to the saved output
(3,014,654,530 bytes, SHA-256
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`), and the
composition sweep matched 3,866/3,866 USDA/USDC outputs over 1,934 inputs.
Because the apparent full45 gain is small and inconsistent, with slower small
scenes and more complex rank storage, the inline representation was reverted.

### 10.54 Priority-queue backing-store reservation (2026-10-27, rejected)

Round 44 reserved space for 2^21 task IDs in the structure walk's priority
queue, based on the observed full45 task count below 1.9 M, to avoid vector
growth and copies. The district paired run (three repetitions) was 1.0% slower
and had a 1.5% higher maximum peak RSS ratio. Since the fast loop showed no
benefit and a measurable RSS increase, the reservation was reverted without a
full45 run.

### 10.55 Reuse the fill-source duplicate scan (2026-10-27, pending full45 flatten)

`prepare_fill` already sorts every `fill_` source-vector pointer to compact
each distinct vector once. `FillOpinions` then repeated the same full sort to
identify vectors shared by multiple prims, so it would not release one before
all its users were filled. The first sorted pass now extracts duplicate
pointers while scanning runs, compacts the target list in place, and passes
the sorted duplicate list into `FillOpinions`. Shared vectors are released
after all fill workers join. Group detection and unique compaction share one
linear scan.

On full45, the candidate has 10,224,245 compact targets, zero duplicated
source vectors, and the first pointer sort plus duplicate scan takes 313–316
ms. Reusing that result removes a second sort of this same ~10.2 M pointer
list from the fill path; the observed `build_stage` reduction is 0.38 s.

The full45 compose-only paired run (two pairs) improved elapsed median from
48.78 to 48.225 s (ratio 0.989); `build_stage` improved 33.313 to 32.933 s,
while the structure walk stayed flat (14.104 to 14.153 s). Maximum peak-RSS
ratio was 1.001 and all child-swap samples were zero. District paired elapsed
ratio was 0.995 and max RSS ratio 1.008; a direct district output remained
580,261,850 bytes with SHA-256
`98619c408f717732b2ad7c2a90493303331f2d65f3e86c9656b62ccc877439bc`.
ALab and reference scene elapsed ratios were 0.988 and 0.992, with max RSS ratios 1.004
and 0.996. The 1,934-input composition sweep matched 3,866/3,866 outputs.
After combining duplicate detection and compaction into one scan, the sweep was
rerun and again matched 3,866/3,866; a direct district USDC remained byte
identical. The follow-up district pair was run under high host load and is not
timing evidence; remeasure the final micro-refinement when the host is quiet.

Full45 flatten and direct output identity were later verified in §10.58.

### 10.56 Parallelize the fill-source pointer sort (2026-10-27, pending full45)

Round 48 keeps the R47 duplicate-scan reuse, then sorts the compact-target
pointers as parallel chunks followed by pairwise parallel merge rounds. It uses
one temporary pointer array; on full45 that is about 82 MiB. District's
`pointer_sort` lap fell from 21–22 ms to 9–11 ms. The district paired elapsed
median was noisy (ratio 1.030, max RSS ratio 0.959) under elevated host load;
the direct USDC remained 580,261,850 bytes with SHA-256
`98619c408f717732b2ad7c2a90493303331f2d65f3e86c9656b62ccc877439bc`.
ALab elapsed/RSS ratios were 0.982/0.998 and reference scene 1.002/0.998; sampled child
swap was zero. The 1,934-input composition sweep matched 3,866/3,866 outputs.

After reverting the round-49 token-replay experiment, the current R47–48
binary was paired again on ALab and reference scene (two repeats, one warmup). Elapsed
and max-RSS ratios were 1.006/1.000 for ALab and 0.981/1.002 for reference scene;
sampled child swap remained zero. This confirms the small-scene gates on the
current source after the writer change was reverted.

Full45 compose-only and flatten pairs, RSS, swap, and hash checks were later
completed in §10.58.

### 10.57 Cache predecessor path indices during token replay (2026-10-27, rejected)

Round 49 packs each flagged path index with the preceding non-root path index
during the parallel prepass. Ordered token replay can then recover the prior
stem directly instead of scanning backward for each flagged path. The change
compiled, and the 705-fixture USDA/USDC rewrite sweep matched 1,410/1,410
outputs.

The initial district pair set was host-load contaminated. In a follow-up
four-pair run, median whole-run elapsed was 6.20 vs 6.245 s (ratio 1.007),
maximum peak RSS ratio was 1.002, and all sampled child-swap values were zero.
The `path_tokens` medians were effectively flat (142.49 ms baseline vs
142.03 ms candidate). ALab and reference scene also showed flat path-token laps; their
paired elapsed/RSS ratios were 0.989/1.001 and 0.993/1.000. District output
remains byte-identical at 580,261,850 bytes, SHA-256
`98619c408f717732b2ad7c2a90493303331f2d65f3e86c9656b62ccc877439bc`.

The experiment was reverted: it adds per-slice event storage without a
measured speedup on the fast loop or small scenes. The 1,410/1,410 rewrite
sweep passed before the revert. Full45 validation for the retained rounds
47–48 is recorded in §10.58.

### 10.58 Full45 validation of rounds 47–48 (2026-10-27)

After moving saved `.usdc` validation outputs out of tmpfs, the paired harness
completed safely with a 4 GiB live reserve, zero sampled child swap, and the
5% peak-RSS gate. Against R33, five alternating full45 compose-only pairs had
a median elapsed ratio of 1.004 and maximum peak-RSS ratio 0.996. Five full45
flatten pairs had elapsed median ratio 1.020 (median of per-pair ratios 1.008)
and maximum peak-RSS ratio 0.999. Three of five pairs favored the baseline in
each mode, so no end-to-end speedup is established; run-to-run phase variation
is larger than the measured change.

The targeted fill lap did improve consistently: full45 `next_fill.compact_ms`
fell from a 766 ms compose-only median to 597 ms (about 22%), while the
candidate's parallel `pointer_sort` lap was 132–140 ms. This is a phase-level
saving of roughly 170 ms, not a demonstrated whole-run gain. Keep rounds
47–48 as a low-risk fill-phase optimization for now, and measure subsequent
changes against this validated candidate.

A direct candidate full45 flatten wrote 3,014,654,530 bytes to `/dev/shm`; its
SHA-256 matches the reference exactly:
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`. ALab
and reference scene paired checks on the same post-R49-revert candidate passed with
elapsed/RSS ratios 1.006/1.000 and 0.981/1.002, zero child swap. The district
hash also matches. The saved validation outputs have been moved to
`$TMP_DIR`; `/tmp` now holds only about 3 GiB.

### 10.59 Same-parent sibling bundles in the structure walk (2026-10-27, accepted)

Round 50 groups up to eight consecutive pending children with the same parent
and depth into one speculative structure-walk task. Each task walks its roots
in serial sibling order using the same local instance-key set, and the commit
still consumes task placeholders in DFS order. The task count falls from about
1.75–1.88 M to about 0.54–0.72 M on full45; bundled roots are reported in the
timing-gated `[next_walk]` line.

Against R33, five alternating full45 compose-only pairs improved median elapsed
from 49.18 to 42.92 s (ratio 0.873); maximum peak RSS ratio was 0.992. Five
flatten pairs improved 63.43 to 57.93 s (ratio 0.913), with maximum peak RSS
ratio 0.993. Every timed run had zero sampled child swap. The structure phase
median fell 14.71 to 9.08 s. Direct full45 flatten output remained 3,014,654,530
bytes with SHA-256 `a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.

Correctness gates passed: the 1,934-input USDA/USDC composition sweep matched
3,868/3,868 outputs; direct district, ALab, reference scene and full45 output hashes
matched their references. ALab/reference scene paired elapsed/RSS ratios were
1.000/1.002 and 0.974/0.998, with zero child swap. Keep R50. Remaining work
should profile commit/materialization and the still-large speculative walk
record count before changing instance-claim or DFS authority.

### 10.60 Inline structure-donation eligibility probe (2026-10-27, rejected)

Round 51 replaced the per-node `std::function` donation-eligibility predicate
with a small inline policy object. The intended saving was in the speculative
walk hot loop; donation callbacks and all traversal/instance decisions stayed
unchanged. The candidate compiled and completed five alternating district
flatten pairs against the R50 executable. Median elapsed was 1.0035x baseline,
maximum peak RSS was 1.0009x, and sampled child swap was zero. The measured
phase timings were flat, so the predicate abstraction is retained; R51 was
reverted. The district benchmark also completed byte-identical output checks.

### 10.61 Increase same-parent sibling bundle cap to 16 (2026-10-27, accepted)

Round 52 doubles R50's maximum same-parent, same-depth sibling roots per
speculative task from eight to sixteen. It preserves ordered root processing
within each task and the existing DFS-ranked commit. Against the R50 executable,
five alternating full45 flatten pairs had median elapsed ratio 0.9867 and
maximum peak-RSS ratio 1.0009; all sampled child-swap values were zero. The
full45 compose-only screen (three alternating pairs) had elapsed ratio 0.9858
and maximum peak-RSS ratio 1.0011, also with zero sampled swap. Thus the
measured full flatten improvement is about 1.3%, with memory within the 5%
gate.

The output hash remained `a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`
at 3,014,654,530 bytes. The 1,934-input composition sweep matched 3,868/3,868
outputs. ALab elapsed/RSS ratios were 1.000/1.004; reference scene ratios were
0.984/1.001; both passed with zero sampled child swap. The five-pair district
elapsed ratio was 1.000 and RSS ratio 0.998. Keep R52 and continue testing
larger same-parent caps against this binary; the earlier larger DFS-range
bundle rejection (§10.28) used different task/rank semantics.

### 10.62 Larger sibling caps and committed-index storage probes (2026-10-27, rejected)

Round 53 increased the cap from 16 to 32. Its three-pair full45 compose-only
screen was effectively flat against R52 (elapsed ratio 0.9974, max RSS ratio
0.9991); the lower task count did not produce a reliable timing gain. Round 54
tested cap 24 and was also flat (elapsed ratio 0.9998, max RSS ratio 1.0147).
Keep cap 16.

Round 55 removed the temporary per-task `gidx` vector by storing each committed
global index on its `WalkItem`. It compiled and passed the district run, but
five district pairs were 1.0124x elapsed with 1.0050x max RSS. Three full45
compose-only pairs were flat at 1.0005x elapsed and 1.0032x max RSS. The extra
walk-item state did not pay for itself; the temporary parent-index vector is
retained. R53–55 were reverted.

### 10.63 Increase A2 path-shard helper team from four to eight (2026-10-27, rejected)

Round 56 raised only the `fields_specs` A2 path probe/insert team cap to eight;
the separate streaming block team stayed at four. Five district pairs were
0.9913x elapsed and 1.0017x maximum RSS. A three-pair full45 flatten screen
was 0.9950x elapsed and 0.9994x max RSS, but the required five-pair full45
flatten rerun measured 1.0064x elapsed and 1.0026x max RSS. All sampled child
swap values were zero. The 1,410-output rewrite sweep, staged parallel/serial
byte comparison (579,974,418 bytes each), and direct full45 hash/size checks
passed. ALab was neutral and reference scene improved, but the target full45 median
regressed; keep the four-thread A2 cap. R56 was reverted.

### 10.64 Increase parallel `fields_specs` chunk size from 1024 to 2048 (2026-10-27, rejected)

Round 57 doubled `kFieldsChunkPrims` from 1024 to 2048, aiming to reduce
per-chunk coordination. Five alternating full45 flatten pairs against R52
measured median elapsed ratio 1.0030 and maximum peak-RSS ratio 1.0037; all
sampled child-swap values were zero. The district five-pair screen had already
regressed at 1.038 elapsed ratio, despite 0.998 RSS. The larger chunks do not
produce a repeatable target-scene gain, so retain 1024 and restore the R52
binary.

### 10.65 Small stack scratch for walk commit indices (2026-10-28, rejected)

Round 58 replaced the per-call `gidx` vector with a 64-entry stack buffer and
the same vector fallback for larger task results. This targets allocation
overhead across the roughly 0.4–0.5 M walk tasks without adding a field to
every `WalkItem`. Five district compose pairs regressed (elapsed ratio 1.0177,
maximum peak-RSS ratio 0.9982). The three-pair full45 compose-only screen was
flat/slower (elapsed ratio 1.0026, maximum peak-RSS ratio 0.9991). All sampled
child swap remained zero. Revert R58; the per-call vector remains.

### 10.66 Inline short task DFS ranks (2026-10-28, rejected)

Round 59 stored ranks of up to eight indices inline in each task and reused a
rank prefix buffer per worker; deeper ranks retained a vector fallback. This
avoided one small rank-vector allocation per task. Five district compose pairs
improved slightly (elapsed ratio 0.9948, maximum peak-RSS ratio 0.9976), and
the three-pair full45 compose-only screen improved 0.93% (elapsed ratio 0.9907,
maximum peak-RSS ratio 1.0009). However, the five-pair full45 flatten result
regressed 0.87% (elapsed ratio 1.0087, maximum peak-RSS ratio 1.0045). All
timed runs had zero sampled child swap. Since the primary flatten path lost,
revert R59 and retain vector-backed task ranks.

### 10.67 Skip clearing the per-task commit-index scratch (2026-10-28, rejected)

Round 60 used an uninitialized `uint32_t[]` for `CommitWalk`'s local-to-global
index scratch. The intended invariant is that a parent slot is read only after
that parent commits and writes it. Five district compose pairs were flat
(elapsed ratio 1.0035, maximum peak-RSS ratio 0.9986); the three-pair full45
compose-only screen regressed 1.8% (elapsed ratio 1.0182, maximum peak-RSS
ratio 1.0049), with zero sampled child swap. The 43 MB of avoided initialization
was not measurable as a benefit; restore the initialized vector.

### 10.68 Reuse DFS-rank buffers by commit recursion depth (2026-10-28, rejected)

Round 61 retained a rank vector per active `CommitWalk` recursion level and
reused its capacity for subsequent tasks at that level. Five district compose
pairs improved 0.87% (elapsed ratio 0.9913, maximum peak-RSS ratio 0.9971),
but the three-pair full45 compose-only screen regressed 0.45% (elapsed ratio
1.0045, maximum peak-RSS ratio 1.0006). Timed runs had zero sampled child swap.
The district signal did not transfer to full45, so restore the per-call rank
copy.

### 10.69 Reuse the committed parent-index array as child cursors (2026-10-28, accepted)

Round 62 removes `MaterializeWalk`'s separate `cur` vector, which copied the
full `off` prefix array before filling the CSR child-index array. Committed
parents precede their children. During the ascending child pass, each parent's
`ctx.parents` slot has already served as that prim's input parent index and is
then repurposed as its next-child cursor. `off` remains intact for the final
child ranges; each child is still appended in ascending commit order.

Against R52, an initial five-pair full45 flatten set improved median elapsed
by 0.23% (ratio 0.9977; max-RSS ratio 1.0076). An independent five-pair
confirmation set improved 0.48% (ratio 0.9952; max-RSS ratio 1.0007). Both
sets had zero sampled child swap. Three full45 compose-only pairs measured
0.9981 elapsed and 0.9994 max RSS. Five ALab and reference scene pairs were neutral at
1.000 elapsed; their maximum RSS ratios were 1.0018 and 1.0007, with zero
sampled swap.

Correctness gates passed: the 1,934-input composition sweep had 2,456 byte-
identical USDA/USDC outputs and 1,412 matching return-code-only outcomes. The
direct USDC sizes and hashes match all four references: district 580,261,850
bytes (`98619c408f717732b2ad7c2a90493303331f2d65f3e86c9656b62ccc877439bc`),
full45 3,014,654,530 bytes
(`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`),
merged ALab 732,541,268 bytes
(`2f7176c8008f0ae219c58159ef19f3179dab26b2d35073f5fc5aeac174dc7493`), and
reference scene 2,512,205,846 bytes
(`a8f5092209cbcd30841c129354bda0aaae5ea21607f7bd70725ef933d7b2c478`). Keep
R62 and continue measuring the remaining structure-walk and writer phases.

### 10.70 Raise warm-merge worker cap to 32 (2026-10-28, rejected)

Round 63 raised `MergeSources`' parallel worker cap from 16 to 32. Five
full45 flatten pairs regressed to a 1.0115 median elapsed ratio against R62
(57.56 -> 58.22 s); maximum peak RSS ratio was 1.0011. Three compose-only
pairs were 0.9887 elapsed / 0.9954 RSS, but the five-pair district screen
regressed 1.0157 / 1.0083. Full45's merge loop rose from a roughly 4.6–4.7 s
baseline to 5.1–5.2 s; the compose-only signal did not carry through flatten.
All sampled child swap remained zero. Revert the cap increase and retain 16.

### 10.71 Shorten the commit's task-ready spin (2026-10-28, rejected)

Round 64 reduced the ordered commit's spin-before-condition-variable loop from
4096 to 1024 iterations. Five district compose pairs improved 1.56% in median
elapsed time (maximum RSS ratio 0.9818), but five full45 flatten pairs were
flat at 0.9997 elapsed and 1.0042 maximum RSS; sampled swap stayed zero. The
district gain did not transfer to the target flatten path, so restore 4096.

A timing-only full45 profile identified the next large walk costs: top phase
1.2 ms, ordered commit 4.16 s (1.17 s summed task-ready wait), and materialize
2.92 s. Within materialization, CSR setup/fill was 87 ms and parallel PrimSpec
construction/child assignment was 1.60 s; the outer materialize lap also
includes fill preparation. The top phase is negligible, so subsequent probes
should target commit replay or PrimSpec construction. Temporary timers were
removed after collecting this profile.

### 10.72 Avoid the temporary Path::name string during materialization (2026-10-28, rejected)

Round 65 added a `Path::name_view()` accessor and a `PrimSpec` string-view
constructor so each materialized prim could copy its name directly without a
temporary string. Five district compose pairs regressed 1.0141 elapsed with
1.0030 maximum RSS. Five full45 flatten pairs were flat at 1.0002 elapsed and
1.0061 maximum RSS. The name-view path does not improve the target workload;
remove the API and retain `Path::name()`.

### 10.73 Set materialized prim names in place (2026-10-28, accepted)

Round 66 replaces the temporary `PrimSpec(it.path.name())` plus move
assignment with `ps.set_name(it.path.name())` on the default-constructed
output prim. Five district compose pairs improved 0.69% (elapsed ratio
0.9931; maximum RSS ratio 0.9994). Five full45 flatten pairs improved 0.36%
(ratio 0.9964; maximum RSS ratio 0.9978), with zero sampled process swap.
ALab and reference scene were neutral at 1.000 elapsed; maximum RSS ratios were 1.0011
and 1.0029. The direct full45 output remained 3,014,654,530 bytes with SHA-256
`a0175bc1292f189afdb4108261cf2b36d9db13129691fdea87e34f873ced9867`.
The full CTest run passed 60 tests with one expected skip. Keep R66.

