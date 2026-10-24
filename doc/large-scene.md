# Loading large USD scenes in LightUSD

Production USD scenes are 10–20 GB on disk, spread across thousands of payloaded
files. This document analyzes three publicly-available scenes — **Moana Island**
(Disney), **ALab** (Animal Logic), and **Caldera** (Activision) — and describes
how LightUSD loads them within a bounded RAM budget (target: **fit a parse into
16 GB**, geometry deferred), plus the remaining implementation work.

See also [instancing.md](instancing.md) for the Island instancing analysis, and
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

### 1.1 Moana Island (`<asset-root>/island`, ~17 GB)

Root `usd/island.usda` (~22 KB) — a single `Xform "island"` (kind `assembly`)
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
| largest file | `xgGroundCover.usd` 652 MB |

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

### 1.3 Caldera (`<asset-root>/caldera`, 9.7 GB)

Root `caldera.usda` (text) → 4 subLayers
(`map_source/mp_wz_island.usd` + `cameras`/`breadcrumbs`/`endpoints`). The root
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
| Island | structure only, << 1 GB | ~17 GB |
| ALab | metadata ~0.9 GB | textures 7.3 GB + geometry |
| Caldera | structure ~1.4 GB (measured) | ~10 GB |

All three fit a structural parse well under 16 GB when payloads are deferred.

---

## 2. The LightUSD loading strategy

### 2.1 `LargeSceneLoader` (`src/large-scene-loader.{hh,cc}`)

A one-call, memory-bounded loader that wires the existing primitives together
and **keeps the composition graph alive** so geometry can be streamed in later:

```cpp
lightusd::LargeSceneLoadOptions opts;
opts.payload_mode = PayloadMode::LoadNone;       // defer all geometry payloads
opts.allow_parent_relative_paths = true;         // Caldera '..' paths
opts.dedup_layers = true;                        // parse each file once

lightusd::LargeSceneLoader loader;
loader.Load("caldera.usda", opts, &warn, &err);

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
references in Caldera.

For cold-load profiling across Caldera districts, use
`examples/lusdview/tests/run-caldera-matrix.sh`. It defaults to the main
super-terrain, two season sub-scenes, chem factory, hotel, capital cliffs, and
phosphate-mine cliffs, and writes a TSV summary while keeping the dataset
outside the repository:

```sh
CALDERA_ROOT=/mnt/disk1/data/caldera \
  TIMEOUT_SECS=180 \
  examples/lusdview/tests/run-caldera-matrix.sh
```

Set `SCENES_FILE` to a newline-delimited custom scene list. The harness forces
payload deferral and disables the preview cache, and reports early preview,
composition, render-prim collection, geometry estimation, first useful frame,
and full-upload timings separately.

To inventory every USD layer, including generated geometry and support layers,
run `examples/lusdview/tests/run-caldera-inventory.sh`. It performs bounded,
parallel parse-only probes and writes one TSV row per file with its class,
parse time, peak RSS, and parsed stage bytes. Use the inventory to select
composed-root candidates for the full viewer matrix; leaf geometry files are
measured for parser cost but are not misleadingly treated as standalone scenes.

### 2.4 `..`-relative paths

`CompositionGraphOptions::allow_parent_relative_paths` (added in this work)
threads through the reference/payload/`LoadPayload` asset-path validation so
`..` segments are permitted and delegated to the resolver. Without it the DAG
engine rejected such arcs (Caldera composes to almost nothing).

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

### 2.6 Measured result (Caldera)

`large-scene-load caldera.usda --mode=none` composes the full proxy-mode
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

### 2.6.1 Rendering Caldera (lusdrender + lusdview, NVIDIA RTX 5060 Ti)

Both viewers compose Caldera through the `next` loader (`LoadUSDComposed`,
payloads eager), which handles its `..`-relative payload paths and variant LOD.
The **default Tydra loader rejects Caldera** ("Unsafe asset path" on the
`..`-relative payloads), so lusdview must use `--next`.

**Scene facts.** The default (root-authored) LOD is `districtLod = proxy`
(12.67 M default-purpose triangles + ~26 M `purpose = guide` breadcrumb/endpoint
Points). The map is a flat ~8 km island, so **auto-framing the whole bounds is
useless** — frame a scene camera instead (`layers/cameras.usd`:
`phospate_mine_overview`, `map_capital_square`, `map_capital_overview`,
`map_airfield_overview`, `tile_p_beach`, …). **Use the exact prim name** —
most cameras are `map_`-prefixed (`map_capital_square`, not `capital_square`);
a name that doesn't match **silently falls back to auto-fit** (the useless
whole-island shot). List them with
`build/lusdcat layers/cameras.usd | grep 'def Camera'`. The guide Points
triangulate into huge planes that engulf the camera, so they must be hidden
(both viewers hide `guide` by default).

**lusdrender** (ray tracing) — renders the proxy correctly on every backend at
1280×720, `~4–5 GiB` RSS:

```sh
T=tests; M=<asset-root>/caldera/caldera.usda
./build/tools/lusdrender/lusdrender $M cald_cpu.png -rtPreview -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14
./build/tools/lusdrender/lusdrender $M cald_vk.png  -vk  -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14  # GPU compute trace
./build/tools/lusdrender/lusdrender $M cald_vkr.png -vkr -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14  # GPU ray query
# full LOD: NOT  -variant districtLod=full  (no-op — root authors per-district proxy). See §2.6.2 (wrapper layer).
```

This needed the GPU-collector fix: `-vk/-vkr` previously emitted **raw local**
points for every Mesh (no world transform / purpose / mask), piling all
districts at the origin; they now reuse the CPU path's world-space,
purpose-filtered, masked collection. (Native instances are not yet emitted on
the GPU path — negligible for Caldera: ~29 K of 12.70 M tris.)

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
- lusdview's 30 M default `--max-tris` budget **truncates** Caldera; raise it
  (`--max-tris 40000000`) to load the whole proxy (the log prints `truncated`).

**Known limitations (follow-ups).** ~~lusdview's VK ray-query (`--rt`) is
blank on Caldera~~ — **fixed**: the RT TLAS included *every* USD purpose, so
Caldera's ~26 M-tri `guide` breadcrumb planes (hidden in raster) engulfed the
camera; the TLAS now skips purposes hidden in the UI and the mine district
ray-traces correctly (see §2.8 for the measured recipe). The VK-raster flat
shading still wants the smooth-normal + headlight treatment the lusdrender
preview already uses. For full LOD, see §2.6.2.

### 2.6.1-a Island element composition and lusdview timings

The following table records each partial element under
`<asset-root>/island/usd/elements/*/element.usda`. Each run used the `next`
loader, eager payload resolution (`--load-payloads`), the Island large-scene
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
# Caldera (proxy composition baseline)
CALDERA=<asset-root>/caldera/caldera.usda
[ -f "$CALDERA" ] && LIGHTUSD_NEXT_TIMING=1 "$NEXT_TUSD" -l \
  --compose-threads-auto --defer-payloads "$CALDERA"

# ALab merged scene proxy mode
ALAB=<asset-root>/alab/_merged_ALab/entry.usda
[ -f "$ALAB" ] && "$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --lowmem --memstat "$ALAB"

# Moana Island payload-heavy scene
ISLAND=<asset-root>/island/usd/island.usda
[ -f "$ISLAND" ] && "$NEXT_RENDER" --next --compose-threads-auto --defer-payloads \
  --lowmem --memstat "$ISLAND"
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

### 2.6.2 Full LOD (`districtLod = full`) — per-shot, not whole-island

**Whole-island full LOD does not fit on a 62 GiB host, and the wall is host
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

- **A global `-variant districtLod=full` has no effect.** The root `caldera.usda`
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
    subLayers = [ @<asset-root>/caldera/caldera.usda@ ]
    upAxis = "Z"
)
over "world" { over "mp_wz_island" { over "mp_wz_island_paths" {
    over "mp_wz_island_geo" {
        over "map_phosphate_mine" ( variants = { string districtLod = "full" } ) {}
    }
}}}
```

Render the wrapper exactly like the scene, framing the matching camera:

```sh
W=caldera_mine_full.usda   # the wrapper above
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
only the whole *island* at full that does not fit (see top of this section).

| District (camera) | Default tris @ full | View |
|---|---:|---|
| `map_phosphate_mine` (`phospate_mine_overview`) | 29.8 M | ![phosphate mine](images/caldera/caldera-phosphate-mine-full.jpg) |
| `map_capital` (`map_capital_square`) | 44.3 M | ![capital square](images/caldera/caldera-capital-square-full.jpg) |
| `map_beachhead` (`map_beachhead_village`) | 23.6 M | ![beachhead village](images/caldera/caldera-beachhead-village-full.jpg) |
| `map_tile_p` (`tile_p_beach`) | 27.8 M | ![tile_p beach](images/caldera/caldera-tile-p-beach-full.jpg) |
| `map_airfield` (`map_airfield_overview`) | 36.9 M | ![airfield](images/caldera/caldera-airfield-full.jpg) |
| `map_arsenal` (`map_arsenal_ship`) | 24.1 M | ![arsenal shipyard](images/caldera/caldera-arsenal-shipyard-full.jpg) |
| `map_agricultural_center` (`map_agricultural_center_bridge`) | 18.1 M | ![agricultural center](images/caldera/caldera-agricultural-center-full.jpg) |

Every other authored camera frames one of these same districts from a different
pose — the district is promoted to `full` exactly as above, only the `-camera`
changes:

| District (camera) | Default tris @ full | View |
|---|---:|---|
| `map_phosphate_mine` (`phospate_mine_bridge`) | 29.8 M | ![mine bridge](images/caldera/caldera-phosphate-mine-bridge-full.jpg) |
| `map_capital` (`map_capital_overview`) | 44.3 M | ![capital overview](images/caldera/caldera-capital-overview-full.jpg) |
| `map_beachhead` (`map_beachhead_overview`) | 23.6 M | ![beachhead overview](images/caldera/caldera-beachhead-overview-full.jpg) |
| `map_tile_p` (`tile_p_road`) | 27.8 M | ![tile_p road](images/caldera/caldera-tile-p-road-full.jpg) |
| `map_airfield` (`map_airfield_waiting_room`) | 36.9 M | ![airfield waiting room](images/caldera/caldera-airfield-waiting-room-full.jpg) |
| `map_arsenal` (`map_arsenal_overview`) | 24.1 M | ![arsenal overview](images/caldera/caldera-arsenal-overview-full.jpg) |

(1280×720 `lusdrender -rtPreview -purpose default,render,proxy` shots, downscaled
to 512 px JPEG for the repo. The colored speckles in some frames are neighboring
districts still at `proxy`, drawn with their `displayColor`. The default-tri
count is per *district*, so it is identical across that district's cameras.)

#### Per-district `lusdview --next` composition measurements

These measurements promote one district to `districtLod = "full"` in a stronger
wrapper layer and leave the other 44 districts at their authored `proxy` LOD.
The input tree is `<asset-root>/caldera` (10,496,782,370 bytes, about 10.50
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
every row still composes the complete proxy island and its shared layers. The
capital and airfield rows are the heaviest host-memory cases; the phosphate
mine has the most converted mesh nodes because its full payload is highly
fragmented. This is why per-shot full LOD remains preferable to promoting all
45 districts at once.

#### Splitting `map_capital` into smaller load units

The `map_capital` full payload is itself a 42,470-byte crate, but its composed
content contains 114 top-level children below `/world/map_capital`. Loading
that crate directly is much cheaper than composing the complete island wrapper,
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
| Full-island wrapper, `map_capital = full` | 102.006 | 30.525 | 178.830 | 3623 | 45,498,738 | 26,134 | — | — |
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
M=<asset-root>/caldera/caldera.usda
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
`mp_wz_island_geo`).

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

### 2.6.4 Realtime island preview: GPU-budget LOD + robust auto-framing (lusdview)

`-lodStream` (above) is a lusdrender, ray-tracing, load-time selection. The
interactive **lusdview** rasterizer hits a different wall on a fully assembled
scene. The Moana island (`<asset-root>/island`) composes via `--next` to
**83,801 draws / 42.9 M instances / 56.5 M unique tris**, and after instance
dedup the geometry itself fits comfortably in 16 GiB VRAM (instance transforms
~2.06 GiB). The problem is **draw/buffer count**: the per-mesh raster path
creates ~84 k host-visible Vulkan buffers and stalls for **7+ minutes** before
the first frame. (A plain `--next --backend vk` whole-island run times out in
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
M=<asset-root>/island/usd/island.usda
# 40 GiB host cgroup; keep the 4000 biggest meshes full, cap full VRAM at 16 GiB,
# merge the other ~79.8 k meshes into one bbox proxy:
systemd-run --user --scope -p MemoryMax=44G -p MemorySwapMax=2G \
  ./build/lusdview --headless --next --backend vk \
    --max-tris 80000000 --max-gpu-mem 16 --max-draw-meshes 4000 \
    --frames 2 --screenshot island.ppm $M
```

```
next: 83801 draws, 42869753 instances, 56553204 unique tris, instXform VRAM ~2.06 GB
gpu-budget LOD: 4000 meshes full (0.08 GiB), merged 79801 meshes / 42730328
                instances into bbox proxy (now 4001 draws)
render stats: 4001/4001 meshes visible, 42869757 instances, 581550424 drawn tris, 4001 draw calls
```

**`--no-robust-frame` (robust auto-framing, on by default).** The island's *raw*
scene bbox is **170,091 units across** — a handful of far-flung meshes sit at
±tens of thousands while the actual island occupies a ~9,200-unit region near the
origin. `fitToScene` on the raw bbox renders the island as a sub-pixel speck.
`ComputeRobustSceneBounds()` fixes the framing with **mass-weighted endpoint
trimming**: each mesh's world-AABB min/max endpoints carry its geometry mass
(`instanceCount × triCount`), and per axis 1% of the *mass* is trimmed from each
tail. Sparse far elements weigh ~nothing and drop out; the dense island defines
the kept range. It only overrides framing when the result is materially tighter
(robust diag < 70% of full), so normal scenes that fill their bbox are
untouched. It is computed from the full per-mesh set **before** the LOD merge
(post-merge the proxy's 42 M instances would swamp the weighting) and cached.
The named-USD-camera path (`--camera`) is unaffected.

```
robust auto-frame: trimmed 1% outliers, scene diag 170091.6 -> 9265.6 (95% smaller)
```

With both on, the whole island uploads in seconds instead of stalling, fits well
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
path specifically: **headless forces the Vulkan backend**, so on island/Caldera
captures the VK box proxies are what keep the distant scene visible. The feature
is **off by default** and the LOD-off path is byte-identical (verified MAE 0 on
both backends). The cull itself is accelerated by a coarse per-prototype spatial
grid (`BuildRtLodGrid`) that frustum-rejects whole cells before the per-instance
loop.

```sh
M=<asset-root>/island/usd/island.usda
# Whole island, VK raster, robust auto-frame; collapse anything under 48 px to a
# box, drop anything under 1 px:
systemd-run --user --scope -p MemoryMax=44G -p MemorySwapMax=2G \
  ./build/lusdview --headless --next --backend vk \
    --raster-lod --raster-lod-cull-px 1 --raster-lod-full-px 48 \
    --max-tris 80000000 --frames 4 --screenshot island.ppm $M
```

Measured on the RTX 5060 Ti (whole island, 42.9 M instances, VK raster, robust
auto-frame, 4 frames so the focal length settles — see note below):

| Mode | Full-detail instances | `present` (GPU+readback) | Distant scene |
|------|----------------------|--------------------------|---------------|
| `--raster-lod` off | 23.7 M visible | ~5620 ms | full |
| size-cull only (`--raster-lod-cull-px 4`, no proxy) | 69 k | ~555 ms | **dropped** |
| box proxies (`--raster-lod-cull-px 1 --raster-lod-full-px 48`) | 15 + box soup | ~480–610 ms | **kept as boxes** |

So size-culling alone cuts visible instances ~340× (23.7 M → 69 k) and the GPU
present ~10× (5.6 s → 0.56 s); switching the drop to a box proxy keeps the same
~10× frame-time win **without** losing the distant island — the far vegetation
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

`lusdview --large-scene-profile off|auto|caldera|island|alab` is a preset layer
over the existing Vulkan large-scene flags. It is meant for repeatable Caldera,
Island, and ALab smoke/performance runs without memorizing the full flag list:
profiles enable `--next`, prefer the Vulkan backend, enable raster/RT LOD, set
conservative draw/VRAM budgets, and print the resolved settings at startup.
`auto` only applies a profile when the input path clearly names Caldera, Island
or Moana, or ALab; otherwise it resolves to `off`.

Explicit CLI flags win. For example, `--large-scene-profile instance-heavy
--max-gpu-mem 6 --raster-lod-full-px 32` keeps the Island preset but uses the
user's GPU budget and LOD threshold. Profiles do **not** enable texture resize or
BCn/compressed texture paths; those remain controlled only by
`--texture-max-size`, `--texture-budget-mb`, and `--texture-compress`.

Scene-specific defaults:

- `caldera`: raises the triangle cap to avoid the old 30 M-triangle truncation,
  uses the named overview camera when no camera is passed, and enables district
  LOD streaming with host/VRAM budgets.
- `island`: caps full raster uploads by draw count and VRAM, then relies on
  per-frame raster LOD to keep the distant island visible as box proxies.
- `alab`: enables parent-relative composition paths because the public ALab
  layout uses `../lightingrenderovers/`, then applies the same Vulkan/LOD budget
  preset pattern.

Optional local harness:

```sh
LUSDVIEW=./build_ninja/lusdview \
CALDERA=/path/to/caldera.usda \
ISLAND=/path/to/island.usda \
ALAB=/path/to/alab.usda \
  bash examples/lusdview/tests/run-large-scene-profiles.sh

LUSDRENDER=./build_ninja/tools/lusdrender/lusdrender \
CALDERA=/path/to/caldera.usda \
ISLAND=/path/to/island.usda \
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

### 2.6.7 Ray-traced full island (lusdview `--hip`, AMD RX 9070 XT)

§2.6.4 is a *rasterizer* preview: it merges the long tail into a flat box-proxy so
the per-mesh draw path stays cheap. The **HIP ray tracer** (`--hip`) takes the
opposite approach — it ignores the raster draw/buffer budget entirely and traces
the **full** geometry: every prototype's real triangles, instanced by the real
per-instance transforms, in one 2-level BVH (per-prototype BLAS + a TLAS over all
instances). On a 16 GiB GPU the **whole Moana island fits** — all 42.9 M instances
over 30.0 M unique triangles (≈17.5 B effective), no LOD, no proxy.

```sh
M=<asset-root>/island/usd/island.usda
# Full island, HIP ray tracing, framed on the hero shot camera.
# --max-instances 0 = unlimited (the 16 M default truncates by mesh order).
./build/lusdview --headless --next --hip \
  --camera shotCam --max-instances 0 --rt-samples 2 \
  --frames 4 --screenshot island.ppm $M
```

```
next: 83798 draws, 42869753 instances, 30009608 unique tris (17508653596 effective), instXform VRAM ~2.06 GB, up=Y
[rt_scene_build] 83798 meshes -> 30009608 tris / 42869754 instances: phaseA(geom) 28518 ms, phaseB(assemble+TLAS) 56137 ms
HIP: traced 42869754 instances, 30009608 unique tris, 91138 colors -> island.ppm
```

Measured on the RX 9070 XT (gfx1201, 16 GiB): **2 m 03 s** wall-clock end-to-end,
**~28.7 GiB host RSS** during the CPU scene build, and the device acceleration
structure fits under 16 GiB VRAM (no OOM). The result is the iconic Motunui
shoreline — mossy coral-rock boulders in the foreground, sandy beach, the line of
bare ironwood trees.

Notes and gotchas:

- **`--next` is mandatory.** The standard Tydra loader rejects the island's
  `..`-relative payloads (same "Unsafe asset path" as Caldera, §2.6.1); `--next`
  composes them. `--hip` then traces whatever geometry the loader produced — it
  does **not** apply the §2.6.4 raster box-proxy LOD.
- **`--max-instances 0`.** The ray tracer defaults to a 16 M instance cap; the
  island has 42.9 M, so the default **truncates by mesh build order** (not frustum
  visibility) and drops most of the scene. Pass `0` for the full island.
- **Named cameras.** `island.usda` authors `shotCam`, `birdseyeCam`, `beachCam`,
  `dunesACam`, `grassCam`, `palmsCam`, `rootsCam`; select one with `--camera`
  (a name that doesn't match silently falls back to whole-scene auto-fit, which
  for the island is a speck — see §2.6.4's robust framing). `--rt-samples N` adds
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
  --frames 1 --screenshot island-cuda.png $M
```

When `--max-tris` is between 1 and 64 million, RT converts legacy point and
curve carriers into a bounded analytic splat stream (`max-tris / 8` samples)
instead of materializing triangle tubes. Meshes retain priority. The 20M
Island profile measured about 26.2M KiB peak RSS and 4.6 GiB CUDA VRAM on an
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

### 2.8 Validated VRAM-fit configs: Island + Caldera on 6 GiB / 15 GiB (2026-07)

GPU-backend review outcome (NVIDIA RTX 5060 Ti 16 GiB, driver-reported
15.57 GiB device-local budget). Every row below was measured with
`benchmark/lusdrender/bench_vram.sh` (true VRAM: `nvidia-smi` 100 ms peak polling minus
idle baseline — earlier "VRAM" numbers in this doc that came from RSS are
host RAM, not VRAM). The **6 GiB rows additionally completed unchanged under
a 10 GiB VRAM ballast** (`benchmark/lusdrender/vram_ballast.py 10`), i.e. with only
~5.5 GiB of the card actually available. Cameras: island `shotCam`, caldera
`phospate_mine_overview`. lusdview headless default 1469×1284; lusdrender
1280×720.

Perf/VRAM changes this round (all image-neutral — byte-identical or
pass the cross-backend tolerance test):

- **LightRT VK BLAS compaction** (default ON, `LRT_VK_BLAS_COMPACT=0` opts
  out): caldera `-vkInstanced` 1051→525 MiB; island full instance set
  7153→5408 MiB **and** 49.6→31.7 s (the compaction rebuild batches BLAS
  builds in ≤512-proto waves — two submits per wave instead of one per
  prototype).
- **lusdview Caldera `--rt` fixed** (purpose-filtered TLAS, see §2.6.1):
  BLAS build 1907→901 ms, RT VRAM 2.73→1.23 GiB, mine district now traces.
- **lusdview device-local MDI instance mirror** (auto when
  VK_EXT_memory_budget shows ≥20 % headroom; `LUSDVIEW_MDI_DEVLOCAL=0/1`
  forces): island no-LOD 843→766 ms/frame. No effect with `--raster-lod`
  (that frame is primitive-bound: 55 M drawn tris of non-instanced unique
  geometry, 63 visible instances). Costs 1.37 GB VRAM on island — the auto
  headroom check keeps it off in tight-VRAM situations.
- **lusdrender flat GPU paths**: `-vkr` skips the unused CPU BVH (15.4→6.6 s
  on caldera), `-vk` builds it threaded (15.2→8.5 s); shade/write is
  row-parallel; `-stats` prints per-stage `[gpu-stats]` timings.
- **CUDA/HIP `--rt-samples`**: device-side accumulation, one readback at the
  end (was one `cuMemcpyDtoH` + host byte-loop per sample).
- Live VRAM: lusdview logs `vram=used/budget GiB` in `[present]`/`[vk_rt]`
  lines (VK_EXT_memory_budget).

**lusdview** (`T=./build/lusdview`, `ISL=<asset-root>/island/usd/island.usda`,
`CAL=<asset-root>/caldera/caldera.usda`):

| Config | Recipe | VRAM peak | Steady frame | Fits |
|---|---|---|---|---|
| Island raster, 15 GiB | `--next --camera shotCam` (mirror auto-on) | 1.55 GiB | 766 ms | 15 GiB |
| Island raster, 6 GiB | `--next --camera shotCam --max-gpu-mem 4 --raster-lod --raster-lod-full-px 32 --raster-lod-cull-px 3` + `LUSDVIEW_MDI_DEVLOCAL=0` | 0.25 GiB | ~362 ms | both |
| Island RT, 15 GiB | `--next --rt --rt-lod --rt-lod-full-px 96 --rt-lod-cull-px 1 --camera shotCam` | 4.29 GiB | — | both |
| Island RT, 6 GiB | `--next --rt --rt-lod --rt-lod-full-px 48 --rt-lod-cull-px 3 --camera shotCam` | 4.19 GiB | — | both (ballast-verified) |
| Caldera raster | `--next --camera phospate_mine_overview --max-tris 40000000` | 0.18 GiB | — | both |
| Caldera RT | raster recipe + `--rt` | 1.40 GiB | — | both (ballast-verified) |
| Caldera CUDA, 15 GiB | raster recipe + `--cuda` | 7.48 GiB | — | 15 GiB only |
| Caldera CUDA, 6 GiB | `--cuda --max-tris 8000000` | 1.98 GiB | — | both (ballast-verified) |
| Island CUDA | `--next --cuda --camera shotCam` | 2.39 GiB | — | both (ballast-verified) |

Notes: the island RT rt-lod px knobs barely move VRAM (4.19 vs 4.29 GiB —
the grid + LOD-bound BLAS set is the resident cost); both fit 6 GiB but with
little margin — the P2 lusdview BLAS-compaction port is the next lever. The
caldera CUDA cliff between `--max-tris 13000000` (5.53 GiB, marginal) and
`8000000` (1.98 GiB) is one huge district mesh crossing the cap; the 8 M cap
renders the same visible content at this camera (identical image stats).

**lusdrender** (`B=./build/tools/lusdrender/lusdrender`):

| Config | Recipe | VRAM peak | Wall | Fits |
|---|---|---|---|---|
| Island full set, 15 GiB | `LUSDR_INST_BUDGET=45000000 $B $ISL out.png -vkInstanced -camera shotCam -w 1280 -height 720 -maxMem 24` | 5.41 GiB | 31.7 s | 15 GiB (6 GiB too marginal) |
| Island, 6 GiB | `LUSDR_INST_BUDGET=16777216 LRT_VK_TLAS_SLICE=8388608 $B $ISL out.png -vkInstanced -rtLod -rtLodFullPx 48 -rtLodCullPx 3 -camera shotCam -w 1280 -height 720 -maxMem 14` | 0.40 GiB | 19.3 s | both (ballast-verified) |
| Caldera | `$B $CAL out.png -vkInstanced -camera phospate_mine_overview -w 1280 -height 720 -maxMem 14` | 0.52 GiB | 5.8 s | both (ballast-verified) |

Avoid flat `-vkr` on caldera when `-vkInstanced` works (0.95 vs 0.52 GiB);
its win this round is speed (6.6 s total after the CPU-BVH skip). The island
full-set row is the compaction headline: 42.8 M instances now render in
5.41 GiB VRAM (uncompacted: 7.15 GiB) — it even *passes* the nominal
5.5 GiB bar, but leaves no margin for a real desktop, so it is listed as the
15 GiB config.

**Remaining follow-ons (P2, unimplemented):** device-local node/block buffers for
the `-vk` compute path; tiled ray/hit dispatch; TLAS `PREFER_FAST_BUILD` for
settle rebuilds; a `--vram-budget <GiB>` umbrella flag for **lusdrender**
(lusdview has it: `examples/lusdview/main.cc` — the one number the whole budget
tree descends from); raster LOD for *non-instanced* meshes (the island
`--raster-lod` frame is bound by 55 M non-instanced drawn tris — the instanced
side is already solved). The two headline follow-ons from the 2026-07 round are
now done: the **lusdview BLAS-compaction port** (`LUSDVIEW_BLAS_COMPACT`,
`examples/lusdview/vk/vk_renderer.cc`), and the island RT 4.2 → est. ~2.7 GiB
saving it was expected to deliver.

### 2.9 Host-memory matrix: weld ratio + texture cap (`--next`, 2026-07-11)

Companion to §2.8, measuring a different thing: these are **host peak RSS**
(`/usr/bin/time -v`, so process startup is included), not device VRAM. They
exist to catch two `--next` regressions no unit test can see — a weld key that
over-splits (the memory win silently evaporates) and a texture cap that stops
bounding decode. Xvfb + NVIDIA RTX 5060 Ti.

Deferred-payload profiles (`examples/lusdview/tests/run-large-scene-profiles.sh`,
all three PASS). Each resolves `backend=vk --next=on --raster-lod=on --rt-lod=on
--max-gpu-mem=8.0` — the 8 GiB is `ComputeResourceBudget`'s half-of-16-GiB, not
a hardcode:

| Scene | Peak RSS | Prior baseline | Wall |
|---|---|---|---|
| Island | 0.45 GB | 0.49 GB | 2.9 s |
| Caldera | 2.06 GB | 2.18 GB | 6.5 s |
| ALab | 0.52 GB | 0.55 GB | 3.0 s |

**These profiles defer payloads and therefore decode 0 textures** — they do not
exercise the texture cap, and their weld ratios cover only the proxy geometry.
Add `--load-payloads` for the runs that do:

| Scene | Weld ratio | Textures | Wall | Peak RSS |
|---|---|---|---|---|
| Island | **1.04x** (21 159 184 verts / 20 437 898 points) | 0 | 72 s | 18.7 GB |
| Caldera | **1.48x** (31 905 889 / 21 547 797) | 0 | 32 s | 10.8 GB |
| ALab | **1.29x** (6 726 046 / 5 193 895) | 343, 1372 MB decoded (budget 1920 MB, **0 downscaled**) | 17 s | 5.3 GB |

The weld ratio is the number to watch: a ratio drifting toward the
corners-per-point count (~4-6x) means the weld key is too strict and the
deduplication is gone. Island composes 40.9 M instances / 11.7 G effective tris
in this configuration and still fits the 30 GiB host headroom.

```bash
CALDERA=<asset-root>/caldera/caldera.usda \
ISLAND=<asset-root>/island/usd/island.usda \
ALAB=<asset-root>/alab/_merged_ALab/entry.usda \
  bash examples/lusdview/tests/run-large-scene-profiles.sh

# The full-payload run (weld + texture cap), per scene:
/usr/bin/time -v ./build/lusdview --headless --large-scene-profile procedural-heavy \
  --load-payloads --frames 1 --screenshot alab.ppm \
  <asset-root>/alab/_merged_ALab/entry.usda
# watch: 'next: weld N vertices from M points (Kx)'
#        'next: textures N, decoded X MB (cap ..., budget ..., K downscaled)'
```

---

### 2.10 Raster LOD reaches non-instanced geometry (2026-07-11)

`--raster-lod` classified only instanced prototypes, so Island's unique geometry
was drawn in full no matter how small it was on screen. It now runs the same
Cull / Proxy / Full classification over non-instanced meshes.

The prerequisite was a loader bug: the `--next` static-batch path copied the
running **scene**-bounds accumulator into every batch as its AABB, so no unique
mesh could ever be outside the frustum or small on screen — the per-mesh frustum
cull was a no-op too. Each batch now carries its own world bounds (skinned
batches keep the conservative scene box, since their vertices are a single pose).

| Island, `--next`, 1456×1264 headless | drawn tris | meshes drawn |
|---|---|---|
| `--raster-lod` off | 21 646 M (43.2 M instances) | 100 801 / 100 801 |
| `--raster-lod` on, before | 40.3 M | 100 801 / 100 801 |
| `--raster-lod` on, after | **15.2 M** | 84 400 / 100 801 |

Regression test: `lusdview-noninstanced-lod`.

### 2.11 lusdrender `-vk` compute trace: ray tiling, and the device-local BVH
that did not pay (2026-07-11)

`lrt_vk_trace_scene` allocated the whole frame's rays and hits up front —
48 B × w × h × spp, i.e. **6.3 GB of VRAM for 1920×1080 at 64 spp**, on top of
the BVH — and dispatched them as one job. It now traces in fixed tiles
(`LRT_VK_RAY_TILE`, default 4 M rays = a 192 MiB working set). Rays are
independent, so this is exactly image-neutral, and it is also *faster*:

| Suzanne, 1024×1024 × 32 spp (33.5 M rays), RTX 5060 Ti | trace | ray+hit VRAM |
|---|---|---|
| one whole-frame dispatch | 3.61 s | ~1.6 GiB |
| tiled, 4 M rays | **2.83 s** | **192 MiB** |

Device-local node/block buffers (`LRT_VK_DEVICE_LOCAL=1`) were the other half of
the §2.8 plan, and they do **not** pay off — left implemented but off by default.
Measured on ALab (29.2 M tris; 297 MiB nodes + 1.6 GiB blocks, verified resident
in VRAM):

| | per-ray trace | 16.7 M-ray trace |
|---|---|---|
| host-visible BVH | 0.144 µs | 4.65 s |
| device-local BVH | 0.144 µs | 4.92 s (the staging copy) |

Traversal is simply not bound by where the BVH lives on this GPU, and moving it
into VRAM spends ~1.9 GiB of the budget this work exists to reclaim. The
reasoning still holds for a GPU whose traversal *is* bandwidth-bound, hence the
flag — but measure before enabling it.

Regression test: `tool-lusdrender-vk-ray-tiling` (4 tiles must be byte-identical
to one whole-frame dispatch).

### 2.12 TLAS `PREFER_FAST_BUILD`: measured, rejected (2026-07-12)

The last item of the §2.8 list, and it does not pay. On Island (`--rt --rt-lod`,
36 k TLAS instances) the TLAS acceleration-structure build is **0.7–4.6 ms of a
600–8400 ms rebuild** — the rebuild is dominated by the BLAS builds and the
instance array, not the TLAS. `PREFER_FAST_BUILD` would save ~1 ms of a rebuild
that only happens when the camera settles, and charge for it on every
accumulation frame afterwards.

Left as a lever (`LUSDVIEW_TLAS_FAST_BUILD=1`) for a GPU or scene where the
balance differs. `LUSDVIEW_RT_TIMING=1` now prints the AS-build time on its own
line, so the trade can be re-measured rather than re-argued.

---

### 2.6.8 Next-core load/compose profile on a 32-core, 125 GiB host (2026-10)

Measured on the workstation that hosts the Caldera checkout
(`/path/to/caldera`, 10.5 GB, 63,684 files; 32 cores, 125 GiB RAM,
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
| whole island at full LOD (all 45 districts `full`) | 1 | **79.1 min** (sources 20.5 min, structure 24.6 min, compose 34.8 min, flatten 8.8 min, finalize 7.1 min) | 35.3 min | **114.4 min (1.9 h)** | ~59 GiB | **3.01 GB USDC** (61,833,354 paths/specs) |
| whole island at full LOD | auto | *not re-measured* | — | — | — | — |

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

**The full-island case fits in RAM — and now completes.** With the whole
island promoted to `districtLod = full` (all 45 districts) the next-core loader
composes **10,224,245 prims** (10,217,261 source misses) and writes a
**3.01 GB** USDC in **114 min** on this 32-core / 125 GiB host, peaking around
**59 GiB RSS** (measured by sampling `/proc/<pid>/status`). The memory budget
therefore fits with room to spare; the wall is compose time, not RAM.

That is the *whole island* in one process. The per-district full LOD
(§2.6.2) remains the production-correct approach — one district at full LOD is
~3.5 min and ~8 GiB — but the full-island load is now demonstrated to
complete rather than exhausting the host.

**Memory is the price.** The threaded path keeps per-worker resolver snapshots
and the merged sources cache alive, so the full-LOD district peaks at
**20.9–25.2 GiB** versus **6.8 GiB** single-threaded, and the proxy rises from
1.07 to 1.35 GiB. `--compose-threads 8`/`16` do not reduce the peak (the merge,
not the worker count, dominates). **Re-measure the whole-island full-LOD case
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

For the whole island the same phases scale to `sources` 20.5 min, `structure`
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

Measured with the same Caldera fixtures (SHA-256 of every output unchanged):

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
M=/path/to/caldera/caldera.usda
# threaded (default for native builds)
LIGHTUSD_NEXT_TIMING=1 ./build-next/lusdcat -f --compose-threads-auto \
  --defer-payloads -o /tmp/caldera.usda "$M"          # proxy structure
LIGHTUSD_NEXT_TIMING=1 ./build-next/lusdcat -f --compose-threads-auto \
  --load-payloads -o /tmp/caldera.usdc "$M"            # proxy + payloads
# single-threaded reference (build configured with
# -DLIGHTUSD_NEXT_ENABLE_THREAD=OFF)
LIGHTUSD_NEXT_TIMING=1 ./build-next-st/lusdcat -f --compose-threads 1 \
  --load-payloads -o /tmp/caldera-st.usdc "$M"
sha256sum /tmp/caldera.usdc /tmp/caldera-st.usdc      # must match
```

Full-LOD district wrapper (one district promoted to `full`):

```usda
#usda 1.0
(
    subLayers = [ @/path/to/caldera/caldera.usda@ ]
    upAxis = "Z"
)
over "world" { over "mp_wz_island" { over "mp_wz_island_paths" {
    over "mp_wz_island_geo" {
        over "map_phosphate_mine" ( variants = { string districtLod = "full" } ) {}
    }
} } }
```

### 2.6.9 Python binding (abi3) load/compose profile (2026-10)

This is the same Caldera fixture measured **through the Python API**
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
st = lightusd.load("/path/to/caldera/caldera.usda",
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
Caldera composes its full proxy-mode structure (32,811 prims, geometry deferred)
in ~1.7 GiB. The items below are remaining enhancements.

### 3.1 Cross-directory cwp anchoring — FIXED

**Was:** a loaded layer's PrimSpecs got their working-path stamped from
`GetBaseDir(authored_asset_path)` — a *relative* path — instead of the *resolved
absolute* base directory, so nested references lost their parent-directory
context (e.g. `prefabs/...` inside `map_source/mp_wz_island_geo.usd` anchored to
the scene root instead of `map_source/`). A second case: when sublayer
flattening merged a stronger `over` (carrying the root layer's cwp) onto a weaker
`def` that authored the reference (e.g. Caldera's root `over mp_wz_island_paths`
over `mp_wz_island.usd`'s `def`), the merged prim kept the root cwp and
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
prims introduced *by a reference* (e.g. `paths.usd`'s `mp_wz_island_geo`, which
itself references `geo.usd`) were never added to the namespace, so their arcs
were never followed and they never reached the Stage. Caldera's chain
(`mp_wz_island` → `paths` → `geo` → prefabs → proxy payloads) stopped a few
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

**Result:** Caldera (proxy, LoadNone) composes **32,811 prims / 122 files / 373
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
geometry (3,293 → 4,560 prims, 1,271 → 2,540 deferred payloads); Caldera is
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
| Caldera (Activision) | **2.3 GB** | 45 s | 3.1 GB | 76 s | ~3.45 M |
| Moana Island (Disney) | **8.3 GB** | 1 m 48 s | 8.5 GB | ~5 m | ~6.1 M |

next now flattens **at or below usdcat's RSS on every scene, and faster** — a
5.2× cut on Caldera (was 11.8 GB) and 3.2× on Island (was 26.6 GB), with
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
  for the stage lifetime (Caldera: ~3.4 M of ~3.45 M flatten lines are
  time-sample data — the dominant cost). Lazy samples now **skip content dedup**
  and stay byte-range references until the writer decodes them one at a time.

All three preserve (and slightly improve) speed: less allocation, no redundant
decode at parse/clone, no giant-string copy. The §2 bounded loader is still the
path to use when geometry must stay on disk entirely; the difference here is that
a *full* flatten no longer carries gratuitous peak overhead.

### 6.1 Native `next_usdcat` vs current `lusdcat` (2026-06-29)

The table below is a LightUSD-vs-LightUSD snapshot of the native full-compose
path on the public large scenes. `next_usdcat` uses the `src/next` PCP engine and
streams the flattened USDA directly to a `FILE*`; `lusdcat` is the current
library pipeline. Both write USDA to `/dev/null` so the output text is generated
but not stored on disk. The `next_usdcat` numbers use a Release next build with
chunked value streaming in the ASCII writer.

Commands:

```sh
/usr/bin/time -v env LIGHTUSD_NEXT_TIMING=1 \
  build-next-release/next_usdcat -f -o /dev/null <root.usda>

/usr/bin/time -v \
  build_ninja/lusdcat -f --memstat --output-format=usda -o /dev/null <root.usda>
```

For trusted scenes with individual referenced USD layers larger than the current
512 MiB composition safety cap, current `lusdcat` has an explicit opt-in cap
relaxation:

```sh
/usr/bin/time -v \
  build_ninja/lusdcat -f --relax-asset-cap --memstat \
  --output-format=usda -o /dev/null <root.usda>
```

`--relax-asset-cap` raises the per-composition-layer cap to 8 GiB. Use
`--max-composition-asset-mb=N` when a tighter scene-specific cap is preferred.
The default remains the 512 MiB security-policy cap for untrusted inputs.

| Scene | `next_usdcat` load+compose | `next_usdcat` total | `next_usdcat` max RSS | current `lusdcat` result |
|---|---:|---:|---:|---|
| Caldera `caldera.usda` | 3.53 s | 9.71 s | 2.93 GiB | 1:46.7 total, 10.37 GiB max RSS |
| Moana Island `island.usda` | 15.38 s | 43.42 s | 9.67 GiB | failed after 11.6 s: `xgGroundCover.usd` exceeds the 512 MiB per-asset cap |
| ALab `ALab/entry.usda` | 0.33 s | 0.36 s | 62 MiB | not comparable: resolver missed a referenced ALab asset and built only a 43 KiB stage |

With `--relax-asset-cap`, current `lusdcat` passes the Moana Island cap failure:
the run completed composition in 8 iterations, wrote USDA to `/dev/null`, and
reported Stage in-use memory of 7.23 GiB. The measured full current pipeline was
9:09.98 elapsed with 30,416,912 KiB max RSS on a RelWithDebInfo build. The run is
still much slower and higher-memory than `next_usdcat`, but it no longer fails at
`xgGroundCover.usd` solely because that layer is 683,530,559 bytes.

The table above is the pre-optimization snapshot; the ASCII writer was then
optimized substantially (§6.2). Current full-composition validation uses
`next_usdcat -f -o /dev/null` so the scene is actually composed and flattened
while keeping the output off disk:

```sh
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/caldera/caldera.usda
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/island/usd/island.usda
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/alab/_merged_ALab/entry.usda
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/alab/_merged_ALab/entity/alab_set01/alab_set01.usda
```

Recent measurements on the local workstation:

| Scene | load+compose | total incl. USDA write | max RSS | flattened bytes |
|---|---:|---:|---:|---:|
| Caldera `caldera.usda` | 12.43 s | 1:36.17 | 2.11 GiB | 4.26 GB |
| Moana Island `island.usda` | 213.74 s | 7:44.93 | 7.40 GiB | 10.80 GB |
| ALab `_merged_ALab/entry.usda` | 8.25 s | 1:50.80 | 1.31 GiB | 4.75 GB |
| ALab set asset | 5.85 s | 38.67 s | 677 MiB | 1.62 GB |

Island's large XGen arrays are kept lazy/mmap-backed during compose/write. The
USDC reader decodes older packed array-count headers (low 32 bits = element
count, high 32 bits = auxiliary metadata) before applying guards. It still
rejects malformed over-cap arrays through file-size/addressability checks, but
does not reject valid lazy POD arrays purely because their packed header exceeds
the default eager-decode element guard.

For a pure USDC reader comparison, the pre-flattened Caldera crate isolates parse
memory from composition:

```sh
/usr/bin/time -v env LIGHTUSD_NEXT_TIMING=1 \
  build-next-release/next_usdcat -l <caldera-build>/caldera.flattened.usdc

/usr/bin/time -v \
  build_ninja/lusdcat -l --memstat <caldera-build>/caldera.flattened.usdc
```

| Input | Parser | Load/elapsed | Max RSS | Notes |
|---|---|---:|---:|---|
| `caldera.flattened.usdc` | `next_usdcat -l` | 2.57 s load / 3.26 s elapsed | 1.77 GiB | 53,972 prims; warns for unsupported string arrays |
| `caldera.flattened.usdc` | `lusdcat -l --memstat` | 7.38 s elapsed | 3.34 GiB | Stage in-use memory 2.53 GiB; USDC parser peak 189.8 MiB |

The current `next` reader is therefore faster and lower-memory on this crate
parse (~2.3x faster elapsed, ~47 % lower max RSS). On the full Caldera
compose+stream path, `next` is both faster (~11x wall-clock) and lower-memory
(~3.5x RSS).

### 6.2 ASCII (USDA) writer optimizations

On a fully-composed large scene the **write phase dominates** total wall-clock
(Caldera ~57 %, Island ~63 % before this work). The `src/next` USDA writer
(`src/next/writer/{value-printer,usda-writer,dtoa}.{hh,cc}`) was optimized in two
passes. Every change is **byte-identical** to the serial writer — verified by
`cmp`/sha256 against the prior binary across the 615-file `tests/usda` +
`tests/usdc` corpus and on Caldera + Island (10.18 GB hash match) + ALab, in both
serial and threaded (4/8/16) modes. Output to `/dev/null`, Release build, on a
32-thread / 16-core workstation:

| Scene (flatten write phase) | Before | After |
|---|---:|---:|
| Caldera (4.25 GB) | 728 MB/s | **~850–925 MB/s** |
| Island (10.18 GB) | 371 MB/s, 26.2 s | **~1000 MB/s, ~11.7 s** |
| Island peak RSS | 9.67 GiB | **~8.0 GiB** |

#### Pass 1 — per-element number formatting

Large arrays are the bulk of the bytes, so per-scalar formatting is the serial
hot loop. Two byte-identical changes:

- **No `num_` round-trip.** Each scalar used to format into a stack buffer, append
  to a reused member `std::string`, then be copied *again* into the chunk buffer.
  It now formats once into a stack buffer and appends directly. New
  `dtos_to(char*, float/double)` (`writer/dtoa`) and `IntTo`/`UIntTo(char*, …)`
  (`strfmt.hh`); `ChunkedStream` drops the `num_` member. Serial writer
  +~16 % (Caldera 218 → 253 MB/s, Island piped 162 → 176 MB/s).
- **Indent cache.** `WriteIndent` emits the indentation prefix in one write from a
  `thread_local` cached padding string instead of `depth` tiny writes.

#### Pass 2 — parallel writer rearchitecture

A profile (a debug switch that skips array-value formatting) showed **~95 % of
Island's write phase was array formatting running serially on the main thread**.
The previous parallel design split work by **subtree frontier**, which only
parallelized *leaf* subtrees — but Island's geometry lives in *interior* prims,
whose arrays were serialized on the main thread regardless of thread count (write
scaled only ~1.6× from 1→8 threads).

The writer was rebuilt around a **document-ordered task list**:

1. **Phase 1 (serial, cheap):** one pre-order walk of the whole stage emits cheap
   structural bytes inline as `Text` tasks and **offloads every array value**
   (≥ 256 elements) as a task referencing the borrowed array. No giant array is
   formatted here, so the walk is fast (~1.6 s on Island).
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
  so Island's 21 M-element `quath` orientation arrays chunk through the float
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

The remaining serial cost is the ~6 s Phase-1 build walk on Island (not yet
overlapped with Phase 2) — the next available lever.

### 6.3 USDA `/dev/null` write vs USDC crate write

Do not compare the USDA `/dev/null` write numbers above with
`next_usdcat -f -o out.usdc` timings as if they were the same writer path.
They exercise different serializers:

- `-o /dev/null` has no `.usdc` suffix, so `next_usdcat` writes flattened USDA
  text through the optimized parallel ASCII writer. On the local 32-thread /
  16-core workstation, Island writes ~10.8 GB of USDA text to `/dev/null` in
  about **14 s** with `LIGHTUSD_NEXT_NUM_THREADS=16` and `--compose-threads 16`.
- `-o out.usdc` uses the next crate writer. Island's crate output is only
  ~2.5 GB, but the write phase is still about **36-40 s** even when the output
  path is a `.usdc` symlink to `/dev/null` or a real file under `/dev/shm`.
  Thus the time is not storage bandwidth.

The remaining USDC cost is internal crate construction. As detailed in
[`crate-writer.md`](crate-writer.md), the Island crate writer is dominated by the
serial per-spec structural build and global interning/dedup tables over ~4.25M
specs (paths, tokens/strings, fieldsets, value-block dedup). Final byte emission
and array encoding are a smaller slice. Threaded crate writer paths help
moderately: for Island, a `/dev/null`-symlink USDC write was ~47.6 s with
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
<build>/large-scene-load <asset-root>/caldera/caldera.usda --mode=none
# expect: ~32,811 total prims, 373 deferred payloads, RSS ~1.7 GiB.
# --load-some=N streams deferred proxy geometry on demand.
# full composition + USDA writer stress checks:
env LIGHTUSD_NEXT_TIMING=1 build-next/lusdcat -f -o /dev/null \
  <asset-root>/island/usd/island.usda
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
| Caldera proxy → USDC | 3.27 → 2.98 s | 0.991 → 0.887 s | 1.821 → 1.680 s | 1.71 → 1.68 |
| Caldera full district → USDC | 34.84 → 26.95 s | 18.22 → 9.73 s | 13.83 → 14.32 s | 18.86 → 15.88 |
| Caldera full district → USDA | 25.59 → 15.81 s | 20.50 → 10.18 s | 2.385 → 2.381 s | 21.01 → 15.92 |

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

Island follows Caldera in the qualification sequence. With all thread hints
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

| Island output | Process, baseline → candidate | Load + compose | Write | Peak RSS, GiB |
|---|---:|---:|---:|---:|
| USDC | 15.13 → 14.18 s | 3.361 → 3.350 s | 9.826 → 9.201 s | 4.74 → 4.73 |
| USDA | 10.73 → 9.15 s | 3.731 → 3.184 s | 4.399 → 4.000 s | 5.37 → 5.28 |

Maximum RSS ratios are 0.998 and 0.989. The tuned candidate matches the default
scene's exact hashes: USDC is 2,512,205,846 bytes with SHA-256
`a8f5092209cbcd30841c129354bda0aaae5ea21607f7bd70725ef933d7b2c478`;
USDA is 10,375,532,893 bytes with SHA-256
`8fb8b237c0d757f52946d6a3772284facf6521b3e399fb0f6b570f3ec9c87b2b`.
Both baseline/candidate default exports match too, and the crate reopens with
next and OpenUSD. The Island timing exposes 1.32 s of direct compressed-int
streaming within a 3.55 s USDA write. No additional parallel compressed-int
scheduler is included; this remains a measured follow-up hotspot rather than
an unverified bounded-buffer implementation.

ALab uses a per-file symlink union of the base package and techvar fragment,
with all payloads loaded. Its empty baked placeholder stays in the merged
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
layer raised the Caldera district peak from 15,671,604 to 22,170,512 KiB. A
filesystem-query size gate passed the three-pair scene run but failed the
extended composition-only check with maximum RSS ratio 1.070. The reader-source
revision below was requalified separately; rejected measurements are not mixed
into its result.

The accepted run uses one warmup and three alternating measured pairs,
automatic parse/compose/write hints, normal teardown and `/dev/null` output.
Caldera runs first, followed by Island and merged ALab. Times are medians; RSS
is the median process peak. All seven maximum-peak RSS comparisons pass the
5% gate (ratios 0.996, 0.977, 0.980, 0.996, 0.988, 1.004 and 1.009):

| Case | Process, baseline → candidate | Load + compose | Median peak RSS, GiB |
|---|---:|---:|---:|
| Caldera proxy → USDC | 2.84 → 2.91 s | 0.833 → 0.837 s | 1.638 → 1.640 |
| Caldera district → USDC | 29.06 → 27.88 s | 11.420 → 10.915 s | 16.96 → 17.01 |
| Caldera district → USDA | 15.63 → 15.35 s | 10.272 → 10.017 s | 16.53 → 16.03 |
| Island → USDC | 15.74 → 15.40 s | 5.010 → 3.955 s | 4.495 → 4.483 |
| Island → USDA | 10.92 → 9.52 s | 5.090 → 3.892 s | 5.213 → 5.165 |
| Merged ALab → USDC | 1.37 → 1.39 s | 0.596 → 0.570 s | 0.987 → 0.988 |
| Merged ALab → USDA | 1.46 → 1.46 s | 0.580 → 0.572 s | 1.943 → 1.951 |

Island's automatic load/composition time falls by 21.1% for the USDC case and
23.5% for USDA; process time falls by 2.2% and 12.8%. ALab remains effectively
flat. These results do not establish a writer speedup: the Island crate-write
medians are 9.126 → 9.670 s, while USDA write medians are 3.718 → 3.677 s.
Earlier explicit-parser samples in §8 use different runs/settings and are not
a paired comparison with this automatic policy.

The separate seven-pair Caldera composition-only check measures process
medians of 15.49 → 15.45 s and load/composition of 9.941 → 9.865 s. Load ranges
are 9.219–11.139 s and 9.788–10.802 s. Maximum RSS ratio is 1.000714; the gate
passes. Treat Caldera as effectively neutral in this follow-up rather than
claiming an additional speedup from the three-pair output sample.

Validation explicitly selects `--parse-threads 0` and reproduces the exact §8
USDC hashes for proxy, district, Island and merged ALab, and the §8 USDA hashes
for district, Island and merged ALab. Each retained validation crate reopens
with next and OpenUSD. Throughput output stays on `/dev/null`; validation uses
task-owned `/dev/shm` files below the aggregate 48 GB cap, promptly removed,
and streaming hashes for text. The all-45-district Caldera and separate baked
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
byte budget. The composed Caldera district crate (580,261,850 B,
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
