# Inspection, conversion and capture workflows

Build tools with `LIGHTUSD_BUILD_TOOLS=ON` and examples with
`LIGHTUSD_BUILD_EXAMPLES=ON`. The examples and generated-fixture regression
runner are documented in [next_workflows](../examples/next_workflows/README.md).

## Dependency inventory

```sh
lusddeps --json root.usdz
lusddeps --composed --json root.usda
lusddeps --all-variants --max-layers 1024 --max-records 100000 root.usda
```

`next/resolver/dependency-report.hh` exposes `CollectDependencies`. Records retain
the authored path, resolved identifier, referring layer, source location, kind,
and resolution status. The walker covers sublayers/references/payloads, asset
values and arrays, metadata dictionaries, sampled asset values, clips, variant
content and UDIM tiles (1001–1999). Package-local entry existence is checked.
Resolver callbacks and memory assets can replace filesystem resolution.

The default **authoredSelections** scope follows each layer's authored variant
selections. **allAuthoredVariants** visits every authored option, including
nested options, without generating a Cartesian product. **composedStage** uses
PCP composition, applies stronger cross-layer selections, scans only the
participating layer graph and final composed values, and evaluates asset
expressions with the winning opinion's inherited variable context. The JSON
`scope` field makes the choice explicit. Unknown metadata stored only as raw
USDA text is not decoded into asset dependencies.

Cycles, missing assets, load errors, and layer/record/depth/memory limits produce
`complete: false` and diagnostics. `--no-payloads` retains deferred payload
records but does not visit their layers. Exit codes: 0 complete for the requested
scope, 1 incomplete/unresolved, 2 usage/output error. Custom read callbacks must
also enforce their own allocation limits before returning buffers.

## Property explanation

```sh
lusdcat --explain /World.radius --time 0.5 --json root.usda
```

`next/eval/property-trace.hh` exposes `TraceProperty`. It evaluates through the
normal attribute evaluator, then requests the PCP property stack. Reports include
resolved value, default/sample/connection/clip/schema-fallback provenance,
interpolation/block flags, strong-to-weak authored opinions, source layer/site,
arc kind, accumulated time offset/scale, transformed sample times, authored
connection targets and expression-variable context. `connectionChain` follows
the composed first-target decision with cycle/depth diagnostics. For clips,
`clipSelection` reports the active index/asset and clip time; missing-value
interpolation also names its lower/upper assets and active stage times.
Collection is opt-in and defaults to 4096 opinions. Truncation is explicit.
DefaultTime is written as `"default"`.

`winning` identifies the first eligible authored opinion of the resolved source
kind. Clip and schema fallback provenance is carried separately in `resolution`.
Inline variant source ownership is recovered from the actual authoring spec,
not assumed to be the strongest layer.

## Crate inspection

```sh
lusddumpcrate --format json --limit 100 --path-filter /World scene.usdc
```

JSON includes the bootstrap, TOC sections, tokens, strings, fields, fieldsets,
paths and specs. Table counts distinguish matched/emitted/truncated records.
Filters and section switches work in JSON as in YAML. Raw ValueRep bits use hex
strings to preserve 64-bit values in JavaScript. This tool inspects crate data;
it does not flatten composition.

## Checker baselines and SARIF

```sh
lusdchecker --json asset.usda > baseline.json
lusdchecker --baseline baseline.json --json asset.usda
lusdchecker --baseline baseline.json --sarif asset.usda > results.sarif
```

Baselines are bounded to 16 MiB and must be checker JSON reports. Matching uses
(rule ID, USD location, severity, message). Reports retain all findings and the
original `valid` value, and add `baselineState`, new/existing counts and
`gatePassed`. Existing findings may pass the baseline gate; newly introduced
errors still fail. `--strict` also gates new warnings. Incomplete checker coverage
and strict parser warnings cannot be accepted through a baseline.

The process exits according to `gatePassed`. SARIF 2.1.0 maps findings to rules,
levels, file URIs and USD logical locations, with baseline states and coverage
properties. Ordinary JSON remains compact for existing consumers. Keep baseline
reports specific to the intended asset/check profile; matching does not use the
input filename as part of finding identity.

## Recursive comparison

```sh
lusddiff --json left-directory right-directory
lusddiff --recursive --json left.usdz right.usdz
```

Directories compare recursively. USDZ recursion is opt-in for file arguments;
packages encountered inside directories recurse automatically. USD layers use
the semantic layer diff (including numeric tolerances); other assets compare
bytes. Reports distinguish added, removed and modified entries and include
nested semantic differences. Package root-entry identity is significant.

Traversal rejects symlinks and ambiguous entry identities, caps entries at
100000, package nesting at 16 and bytes at 1 GiB. Composition flattening and
fast/low-memory/path-filter modes are not combined with collection comparison.
Exit codes: 0 equal, 1 different, 2 incomplete/error.

## Static GLB

See [usd_to_gltf](../examples/usd_to_gltf/README.md). The implementation is
`src/tydra/next/gltf-export.{hh,cc}` and supports both standalone next and native
builds. Conversion losses are explicit; `--strict` makes them fatal.

## CPU capture and authored products

```sh
lusdrender -rtPreview --aov depth scene.usda depth.pfm
lusdrender -rtPreview --all-products scene.usda capture.png
```

`--all-products` renders the stage-selected or explicitly selected RenderSettings
products in one process, retaining the composed stage and BVH between cameras.
Each raw ordered RenderVar gets a separate output. Filenames use the caller's
output prefix plus encoded product/variable paths, so authored product names
cannot write outside the requested directory. Duplicate destinations fail.
RenderPass commands are never executed.

Supported raw sources: `color`/`Ci`, `depth`/`z`, `worldNormal`/`N`, and `primId`.
Data outputs require PFM and bypass sRGB conversion, display quantization,
background plates and volume compositing. PFM stores RGB float32, bottom row
first; scalar values are repeated in RGB. Data uses one pixel-center sample.
Depth is distance along the camera ray in stage units (not camera-axis Z).
Normals are signed, normalized world-space surface normals. Data describes the
first geometric surface, without transparency integration. Background is zero.

Prim IDs identify **source mesh prims**, not individual instance placements.
They are deterministic 24-bit values exactly representable in float32. Each
capture writes an `.ids.json` table and rejects detected hash collisions or
unnamed/unsupported material sources. Curves and volumes are unsupported for
prim-ID capture. Instance-specific IDs and layered EXR remain future work.
The capture flags require the next CPU renderer; incompatible legacy/GPU or
subdivision overrides fail rather than silently using another output mode.

## Work still required from the roadmap

- Extend the viewer's transactional update path beyond revision-contiguous
  transform and ordinary mesh structural edits. That path validates the complete
  replacement before it mutates the renderer, retains unchanged GPU slots, and
  commits changed vertex buffers, world matrices, or staged replacement meshes
  as one backend transaction. Consecutive
  variant and payload edits are aggregated across their complete revision
  range. Ordinary mesh additions and removals reuse vacant slots or append new
  ones while leaving retained paths at stable indices. Same-count instance
  transform edits transactionally update only their mapped instance-buffer slice,
  while instance layout/color changes, deform-layout structural edits, and
  UDIM/Ptex texture replacement still use the full-scene fallback.
  Ordinary 2D texture edits stage all changed GL/Vulkan slots before
  publishing them, while unchanged texture slots retain their GPU resources.
  Deformable meshes can update vertices/transforms in place when their skin and
  morph layout is identical. Pure
  material edits with stable catalogs update only affected material slots while
  retaining meshes and textures. `RenderSession` already patches geometry-only
  topology/primvar changes by copy-on-write and reconverts only affected meshes.
  In lusdview, `get_scene_info` reports the retained root revision and resolved
  `layer_dependencies`; MCP `reload_layer {path}` reparses one of those layers,
  publishes its `StageChangeSet`, and uses the same GPU transaction when the
  resulting resource layouts remain compatible.
  Lusdview prepares the matching `RenderSession` candidate on the loader worker
  and commits it through a `SceneUpdateSink` on the render thread. The sink's
  `EndUpdate` performs the backend transaction, so sink rejection preserves both
  the retained render revision and the displayed GPU scene. Preparation is
  bounded to 256 MiB of estimated DrawScene geometry by default
  (`LUSDVIEW_RENDER_SESSION_MAX_MB`).
