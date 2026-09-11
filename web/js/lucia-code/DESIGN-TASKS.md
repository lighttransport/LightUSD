# Lucia Code Asset Clinic: Design and Task Roadmap

Status: proposed  
Scope: `web/js/lucia-code` and supporting LightUSD native/WASM APIs  
Primary audience: Lucia Code implementers and reviewers

## 1. Product direction

Lucia Code should become a USD-native asset conditioning workbench. Its core
job is to turn imperfect generated or imported 3D assets into smaller,
editable, physically usable, and portable USD/USDZ deliverables.

The flagship workflow is:

```text
Import generated mesh
  -> diagnose
  -> repair topology
  -> decompose parts
  -> reduce and generate LODs
  -> regenerate UVs and tangents
  -> simplify materials and bake textures
  -> create colliders
  -> validate dependencies and USD structure
  -> export USD/USDZ
```

Lucia is not intended to replace a general-purpose DCC. Features should focus
on inspection, deterministic conversion, validation, and packaging rather than
manual polygon modeling, sculpting, animation authoring, or full shader-graph
creation.

## 2. Product principles

1. **Never destroy the source.** Retain imported bytes and author results into
   the working/session layer or a new export.
2. **Measure every operation.** Show before/after topology, memory, file-size,
   visual-error, and validation metrics where applicable.
3. **Preview potentially destructive work.** Repair, reduction, baking, and
   decomposition require an explicit preview and confirmation.
4. **Keep edits deterministic.** Identical input and options must produce
   identical topology, paths, names, and output bytes where practical.
5. **Make every accepted edit undoable.** One operation is one command-stack
   entry, even when it authors several USD properties or files.
6. **Prefer local processing.** Geometry, source USD, and textures remain in the
   browser unless the user explicitly chooses an external service.
7. **Use AI for orchestration and semantics, not geometric truth.** The
   assistant may propose tools, names, or groupings; native algorithms perform
   and validate geometry changes.
8. **Separate target profiles.** Portable USD/USDZ, web, mobile AR, games, and
   physics engines have different constraints and must not silently share one
   lossy preset.

## 3. Operation contract

New processors should expose the same conceptual lifecycle:

```js
{
  id: 'mesh.reduce',
  analyze(selection, options),
  preview(selection, options, progress, abortSignal),
  apply(session, previewResult),
  validate(result),
  metrics: { before, after, warnings }
}
```

Implementation requirements:

- Heavy processing runs in a Web Worker backed by WASM where appropriate.
- Typed arrays are transferred rather than cloned when ownership permits.
- Long-running work accepts an `AbortSignal` and emits bounded progress events.
- Preview results are disposable and cannot mutate the working stage.
- Apply returns an undo snapshot or reversible USD delta.
- Structured errors contain an operation ID, affected prim paths, and a stable
  error code suitable for the activity log and assistant.
- Algorithms record their version and normalized options in operation history.

## 4. Shared analysis model

Build a shared `AssetReport` before adding independent repair buttons. All
processors should consume the same facts rather than rescanning geometry.

```ts
interface AssetReport {
  stage: StageMetrics;
  meshes: Map<string, MeshMetrics>;
  materials: Map<string, MaterialMetrics>;
  textures: Map<string, TextureMetrics>;
  dependencies: DependencyMetrics;
  physics: PhysicsMetrics;
  issues: AssetIssue[];
  score: {
    geometry: number;
    materials: number;
    textures: number;
    usd: number;
    physics: number | null;
  };
}
```

An `AssetIssue` must include severity, stable rule ID, affected paths,
explanation, whether an automatic fix is safe, and the operation that fixes it.
The model should support layer-, stage-, prim-, and schema-level rules, following
the structure of the
[OpenUSD validation framework](https://openusd.org/dev/api/md_pxr_usd_validation_usd_validation__r_e_a_d_m_e.html).

## 5. Phase 1: make generated assets usable

### LC-100 — Asset Health Report

**Goal:** Explain why an asset is expensive, visually broken, physically
unsuitable, or non-portable before changing it.

Tasks:

Implementation note: the first Lucia slice now provides a deterministic
Three.js-side report with geometry counts, GPU-memory estimation, material and
texture inventory, stable issue IDs for non-finite positions, malformed index
counts, zero-area faces, missing normals, and missing UVs, plus a Geometry score
card. The remaining checks and issue actions below are intentionally still
tracked as future work.

- [x] LC-101 Define `AssetReport`, `AssetIssue`, metric units, and stable rule IDs.
- [x] LC-102 Analyze triangle/vertex/object/material counts and estimated GPU memory.
- [x] LC-103 Detect zero-area faces, duplicate indices, unused vertices, tiny
      triangles, boundary edges, non-manifold edges, and disconnected components.
      The report now covers duplicate indices, unused vertices, boundary and
      non-manifold edges, and disconnected components; tiny-triangle thresholds
      are now scale-aware. Safe cleanup can also fill validated simple planar
      boundary loops when the explicit boolean `fillPlanarHoles` option is
      enabled; malformed option values are rejected and broader topology-aware
      repairs remain pending.
- [x] LC-104 Detect inconsistent winding, invalid normals, missing tangents, and
      invalid or missing topology arrays.
      Normal-map materials now trigger missing-tangent diagnostics, and present
      tangent arrays are checked for size and finite components; shared-edge
      inconsistent winding and inverted authored normals are now detected and
      offered to safe cleanup/recompute; missing position/index storage and
      invalid index values now produce actionable topology diagnostics. Missing
      normals use the same recompute operation ID as invalid authored normals,
      so safe repair plans include both cases.
- [x] LC-105 Report UV presence, non-finite values, out-of-range coordinates,
      overlapping faces, island count, and approximate texel-density variance.
      The report now includes deterministic coarse overlap and UV-island estimates.
- [x] LC-106 Report missing assets, unresolved paths, texture dimensions, material
      count, shader type, and unsupported shader nodes.
      Health reports now flag oversized image dimensions in addition to missing
      image data; malformed non-positive or fractional image dimensions now get
      a distinct error and separate count from unloaded images. The Health UI
      exposes dimensions, byte estimates, slots,
      and color spaces; material inventories now flag unsupported nodes,
      unreachable nodes, and unresolved graph references; external resolver
      paths are now classified explicitly in USD Doctor manifests and the
      panel, while optional host resolver plug-in registration is now surfaced.
      Health also flags
      attached pixel buffers whose RGBA cardinality does not match their image
      dimensions before texture baking; malformed dimensions no longer produce
      negative, fractional, or non-finite inventory byte estimates. Health
      stage summaries also expose deterministic material counts, shader type
      lists, and unsupported-shader counts without requiring consumers to
      rebuild those aggregates from the detailed inventory. Inventory IDs and
      shader labels are normalized at the report boundary so malformed loader
      metadata cannot break sorting or produce unstable numeric/object keys.
      Texture inventory also rejects non-finite pixel values and non-iterable
      array-like pixel descriptors without aborting the report.
      USD Doctor accepts an optional host-provided resolver-plugin registry and
      reports normalized scheme/capability descriptors plus deterministic
      per-resolver dependency registration status; resolver loading remains
      outside the browser-side Doctor boundary. Dependency manifests preserve
      the same normalized resolver inventory and status records for downstream
      audit consumers. Duplicate registrations resolve to a canonical
      descriptor independent of host insertion order, and registered resolver
      state survives scene resets. Unregistered resolver schemes now produce a
      stable Doctor warning. `LuciaProject.setResolverPlugins()` provides
      the host-facing registration boundary, invalidating only USD diagnostics
      without marking authored scene data dirty; equivalent normalized
      registrations are idempotent and do not trigger another refresh.
- [x] LC-107 Report up-axis, meters-per-unit, negative scales, extreme transforms,
      missing `defaultPrim`, and composition errors.
      USD metadata and composition diagnostics are covered by USD Doctor; mesh
      reports now also flag negative and numerically extreme scale axes; Doctor
      validates defaultPrim against depth-aware root prims and validates authored
      kind/purpose tokens; the Doctor panel now displays the authored value lists;
      resolver-dependent composition semantics remain pending. Health transform
      analysis now also reports non-finite position/rotation components and
      extreme translations, in addition to negative/extreme scale axes. Health
      exposes aggregate negative-scale, extreme-transform, and
      non-finite-transform counters in a dedicated Transform inspection section,
      with issue-specific explanations.
- [x] LC-108 Add Geometry, Materials, Textures, USD, and Physics score cards.
      Lucia renders deterministic Geometry, Materials, Textures, and USD cards;
      the Physics card reports topology readiness; collider/shape generation
      now has deterministic guide convex-hull and reduced-triangle-collider
      actions with undoable activity summaries.
- [x] LC-109 Add `Select`, `Explain`, `Fix`, and `Fix all safe issues` actions.
- [x] LC-110 Cache analysis by stage revision and invalidate only affected domains.
      Cache keys now include package asset bytes and relevant image metadata
      (dimensions, color space, normal convention, and MIME type), so metadata-only
      texture changes cannot reuse a stale Health report; cached texture
      diagnostics are also type-checked and discarded when corrupted.
      Asset analysis is reused across inspector refreshes and invalidated by
      scene/assets revisions; USD Doctor analysis is independently invalidated
      by USD/assets revisions.
      Health and USD Doctor reports now use independent scene/USD/package-asset
      revision domains; ordinary scene mutations invalidate all affected
      domains, while USD metadata repair preserves the Health report cache. Asset
      edits and bake outputs now invalidate only package-asset data; a bounded,
      local-storage-backed cache now reuses reports across reloads using a
      deterministic USDA/package-content key, ignores corrupt or oversized
      entries, and evicts least-recently-used entries from a bounded LRU index.
      Cache reads also reject structurally incomplete reports before they reach
      the Health renderer and remove invalid entries so storage self-heals.
      Asset Health now reports malformed non-object traversal nodes as stable
      diagnostics instead of crashing during scene analysis. Optional normal,
      tangent, and UV buffers with non-iterable backing data are likewise
      reported and excluded from channel analysis without aborting the mesh.
      Malformed material-group collections are also reported and excluded from
      subset inspection rather than crashing the report.
      Browser smoke coverage verifies persistence by reopening the Health report
      after a page reload.
      Native validation now receives its required empty-options JSON argument
      from the Lucia session wrapper, avoiding a browser-only embind failure.

Acceptance criteria:

- A report over a 100k-triangle local asset does not block the UI thread.
- Selecting an issue highlights its exact prims or mesh elements.
- Rules produce deterministic IDs and results in browser and Node tests.
- `Fix all safe issues` never invokes reconstructive or appearance-changing work.

### LC-120 — Safe Mesh Cleanup

**Goal:** Correct common generated-mesh defects without intentionally changing
the visible surface.

Implementation note: the current Lucia slice provides a worker-backed indexed
cleanup pass that removes degenerate, duplicate, invalid-index, and non-finite
faces, compacts unused vertices, preserves aligned normals, UVs, colors, custom
vertex attributes, and indexed material groups. Existing USD `GeomSubset`
indices are rewritten from retained face ranges, so subset material bindings
survive safe cleanup. The broader boundary-fill and semantic cleanup work
remains below.

Tasks:

- [x] LC-121 Replace the current worker implementation with an indexed mesh core
      shared by analysis and processing.
      A shared checked indexed-mesh interchange validator now normalizes
      unindexed topology and is used by the operation-layer geometry boundary;
      xatlas, CPU projection, connected-component, cleanup, retopology-worker,
      normal, and tangent processing boundaries now use the same validator;
      AssetReport now consumes the non-throwing inspection companion so malformed
      meshes remain diagnosable without aborting the stage scan; the projection
      worker now uses the same core as well. The throwing contract enforces
      bounded vertex/index counts before unindexed normalization, with shared
      10M-vertex/30M-index safety ceilings. Retopology now consumes the same
      indexed boundary and carries all supported aligned vertex buffers through
      its source-vertex remap; a single shared processing implementation remains
      pending. Retopology now uses a reduction-specific result validator, so
      UV-less meshes remain eligible while optional aligned buffers are still
      checked before USD authoring. Safe cleanup now validates every authored
      aligned and custom vertex attribute before worker transfer, rejecting
      malformed buffers rather than silently dropping them; the shared cleanup
      core applies the same contract for direct worker/API callers. The
      bounded `hasInvalidValue` scan is centralized in the indexed-mesh module
      and reused by cleanup, xatlas, authoring, and operation layers. Large
      tangent/UV validation paths also use direct bounded scans rather than
      spreading typed arrays or materializing per-vertex length arrays; xatlas
      custom-primvar validation follows the same contract. xatlas and
      retopology result validators also reject malformed custom-attribute
      collections and all required/optional non-iterable result buffers with
      typed Lucia errors. The
      same module now provides allocation-free indexed triangle traversal used
      by physics analysis, component extraction, geometry canonicalization, and
      retopology boundary-lock derivation, including bounded group ranges.
      A canonical worker normalization view now expands unindexed topology only
      after validation and widens indexed buffers to Uint32 once; xatlas and
      retopology workers consume that shared view. Projection transfer and
      LightRT ray-projection workers now consume it as well, keeping source and
      target triangle ordinals tied to the same canonical index buffers.
      Position and index safety ceilings are now checked before non-canonical
      position buffers are widened, preventing avoidable allocation before
      malformed or oversized worker input is rejected.
      Caller-provided safety ceilings are also required to be non-negative
      safe integers; the non-throwing inspection companion reports malformed
      limits as invalid rather than weakening its bounds.
      Caller ceilings cannot exceed the shared global maxima, so individual
      workers cannot opt out of the repository-wide mesh safety policy.
      The shared validator also requires an explicit boolean triangle-mode
      flag, preventing malformed callers from weakening topology validation
      through truthy/falsy coercion.
      Non-Float32 position buffers are checked again after widening so finite
      Float64 coordinates that overflow the worker Float32 representation are
      rejected before transfer.
      Indexed-to-corner expansion for LightRT AO and projection inputs is now a
      shared primitive too, including UV and normal buffers, so workers cannot
      drift in corner ordering or typed-array handling.
      That primitive also validates aligned strides and every source index,
      keeping direct worker/API callers fail-closed when bypassing the higher
      level mesh normalizer.
      The cleanup worker/core now consumes the same view before welding,
      filtering, or face-varying remapping.
      Cleanup worker results also require supported typed numeric buffers for
      vertex-aligned and face-varying attributes before USD authoring.
      Cleanup result validation now applies the same requirement to primary
      topology and UV/index buffers as well.
      Retopology result validation applies the same typed-buffer requirement
      to core topology, source remaps, aligned attributes, and custom data.
      UV and skin-transfer result validators now apply the same requirement to
      transferred coordinates, hit masks, influences, and distance buffers.
      The shared normal/tangent worker-vector validator also rejects untyped
      result payloads before they can reach mesh authoring.
      UV-transfer normal preparation uses the same typed-buffer contract before
      projection workers are started.
      The supported numeric-array contract is enforced by a private indexed-
      mesh allowlist and exposed through an immutable predicate, preventing
      operation validators from drifting independently. Xatlas seam-remap
      utilities consume that same contract for
      source xrefs and custom aligned attributes.
      Xatlas request validation and USD mesh authoring now consume it too,
      eliminating duplicated typed-array allowlists at both boundaries.
      Xatlas aligned normals, colors, tangents, and skin buffers are also
      typed-only before worker transfer.
      Primary xatlas positions and triangle indices are now typed-only at the
      request boundary as well.
      The public geometry-operation boundary now applies the same authentic
      typed-array requirement to position/index buffers and host-aligned
      cleanup, retopology, and skin attributes before worker transfer; plain
      arrays remain supported only by the explicitly direct cleanup/component
      analysis APIs that normalize their input by contract.
      Shared numeric-array kind classification also preserves Uint8ClampedArray
      custom primvars as integer USD attributes instead of silently dropping them.
      Face-subset edge uses now cover feature-aware component boundary and
      concavity metadata, and UV-space island connectivity reuses the same
      edge representation; only policy-specific seam and crease walks remain
      local to their consumers.
      Indexed edge-use construction is also centralized and shared by cleanup
      and component analysis, preserving deterministic face order and
      undirected edge keys; the larger single shared processing implementation
      remains pending. The same primitive now exposes face adjacency for
      cleanup and physics analysis, while crease/concavity component analysis
      retains its feature-aware adjacency policy.
      Component splitting, component previews, crack-merge previews, and
      material parameterization now share one operation-boundary topology
      normalization helper, keeping typed-buffer validation and index widening
      consistent across these entry points.
      Retopology, xatlas unwrap, and projected UV operations now use the same
      canonical position/index pair as well; implicit topology is materialized
      once before worker or projection dispatch.
      Normal and tangent recomputation now use that same pair, eliminating
      operation-specific implicit-index generation before worker dispatch.
      Direct UV-space baking now uses the same bounded topology boundary before
      transferring geometry to the raster or occlusion worker.
      Projected baking now reuses normalized source and target topology before
      LightRT/BVH projection and attribute sampling.
      Source-UV transfer now also shares the canonical source and target
      buffers before projection-worker dispatch.
      High-to-low projection now uses the same normalized source and target
      topology before ray generation and hit-result validation.
      Skin-weight transfer now normalizes source and target topology once before
      rig-worker dispatch and target authoring; target faces are preserved even
      though only source faces participate in nearest-weight transfer.
      Safe mesh cleanup now enters its worker through the same bounded indexed
      topology normalizer, including explicit materialization of unindexed
      triangle topology.
      The standalone crack-merge preview helper uses the same cleanup core and
      topology normalization, keeping preview and authoring classifications in
      sync for unindexed callers.
      The CPU UV baker now applies the shared vertex/index safety limits before
      typed-array coercion while retaining per-face skipping for malformed
      out-of-range corners used by its diagnostic result.
      Vertex compaction for component extraction and cleanup now uses a shared
      typed, array-like-safe remap primitive. Face-varying value/index
      compaction is now shared as well, covering filtered cleanup faces and
      preserving source value types; retopology-specific reduction remapping
      remains separate because its native xref contract is reduction-specific.
      Component extraction and safe cleanup also accept plain array-like input
      buffers while producing typed remapped output for worker and authoring
      boundaries. Unindexed normalization now applies the same explicit index
      safety ceiling before allocating its generated topology. Cleanup worker
      result validation now rejects malformed collections and non-iterable
      buffers with typed Lucia errors, and enforces unique valid custom
      primvar names before authoring, including collisions across aligned and
      face-varying attribute collections.
      The direct cleanup core also rejects null, array, and non-object inputs
      before attempting mesh-buffer conversion. Material-group collections and
      ranges are now validated before cleanup instead of being silently
      ignored, preventing accidental loss of material assignment. Direct
      cleanup attribute inputs also require valid unique names and iterable
      value/index buffers before compaction.
      Authored crease edges and optional sharpness weights now pass through
      cleanup and are remapped with compacted vertices; edges touching
      discarded vertices are omitted and overlapping remapped edges retain the
      strongest weight.
      Cleanup result validation also enforces the remapped edge/weight contract
      before USD authoring.
      Authored lightmap UVs now travel through the face-varying compaction and
      remap path alongside the default UV set, preserving `st1` values and
      indices during safe cleanup.
      Connected-component extraction now remaps authored crease edges and
      sharpness weights into each generated part, and component authoring also
      carries the lightmap UV set.
      Retopology now seam-expands and remaps indexed authored lightmap UVs as
      `st1`; unindexed lightmap data is rejected rather than leaving stale
      values after reduction.
      UV session reads now distinguish absent UV sets from malformed authored
      face-varying values or indices and fail closed before topology edits.
      The retopology worker applies the same explicit iterable-buffer and
      custom-attribute collection checks to direct worker messages, producing a
      stable protocol error instead of an incidental iteration or conversion
      exception; custom vertex primvar names are also validated for safe syntax
      and uniqueness before reduction, preventing silent authoring drops.
      UV base-color rasterization and UV-island connectivity now consume the
      same bounded indexed-triangle iterator, as do normal, tangent, occlusion
      surface processing, physics topology analysis, and cleanup face filtering; skip/degenerate
      accounting and CPU/worker parity are preserved across these topology
      passes.
      Physics analysis also rejects oversized implicit or explicit topology
      before synthesizing an index buffer, returning a stable invalid report
      instead of allocating beyond the shared mesh ceiling.
      Asset duplicate fingerprinting now skips malformed position/index
      topology, preventing invalid meshes from entering the duplicate cache or
      triggering unnecessary implicit-index materialization.
      Diagnostic topology now uses an allocation-free shared enumerator for
      both indexed and implicit unindexed meshes, preserving fail-soft issue
      reporting while removing duplicate face access logic.
      Operation-layer geometry validation now uses the indexed validator's
      allocation-free mode for implicit topology; explicit index materialization
      remains confined to normalization paths that actually transfer topology
      to workers.
- [x] LC-122 Weld vertices using a scale-aware tolerance and preserve vertex and
      face-varying primvars.
      Cleanup welding now derives its quantum from the actual mesh extent,
      including sub-unit meshes, while preserving UV/normal/color/custom-attribute
      seams. The indexed cleanup core now also accepts explicit face-varying
      primvar value/index buffers, keeps their corner ordering through winding
      normalization and face filtering, and compacts retained values. Cleanup
      now carries explicit `uvs` + `uvIndices` through the same path, so
      authored face-varying UV seams survive welding and filtered faces; the
      cleanup operation reads authored `primvars:st` corner data from the USD
      session before dispatch, and typed face-varying `primvars:*` arrays with
      explicit corner indices are carried through the generic cleanup path.
      Malformed authored corner values or indices now fail closed before the
      worker rather than being silently cleared by a safe cleanup. Reserved
      USD primvar names are rejected for generic face-varying attributes, and
      a face-varying displayColor channel takes precedence over flattened
      Three.js vertex colors to prevent duplicate authored declarations.
      Connected-component extraction now also compacts face-varying value/index
      buffers per generated component, including authored UV corners forwarded
      by the operation layer.
      Three.js extraction and broader USD face-varying authoring remain pending.
      Retopo now preserves aligned colors, tangents, skin influences, and custom
      vertex attributes through reduction. Face-varying retopo primvars are now
      expanded into seam-aware source vertices before reduction and remapped onto
      the reduced vertices with explicit indices; explicit sharp-edge and vertex
      lock selections are expanded to all corresponding seam vertices.
      Tolerance welding now uses neighboring spatial buckets plus an exact
      Euclidean distance check, avoiding rounded-key boundary misses and
      diagonal over-welding while preserving all aligned attribute seams.
      The direct cleanup core now rejects non-finite or negative tolerance
      values before deriving the scale-aware weld radius.
- [x] LC-123 Remove degenerate/duplicate faces, unused vertices, and isolated
      components below configurable area/volume thresholds.
      Cleanup now accepts a bounded scale-normalized minimum face-area threshold
      while retaining zero as the default; the Operations inspector and local
      assistant expose the control. An opt-in scale-normalized minimum
      component-area threshold now drops isolated fragments while preserving
      surviving material-group ranges. An opt-in normalized component-volume
      threshold now filters only closed components; open/non-manifold surfaces
      are retained conservatively.
- [x] LC-124 Unify winding per connected orientable component.
- [x] LC-125 Fill simple planar boundary loops with previewed triangulation.
      Cleanup now offers an explicit `fillPlanarHoles` option using deterministic
      ear-clipping for simple planar loops; non-planar, ambiguous, and
      non-manifold boundaries are conservatively skipped. The result and
      activity history report the number of loops filled; the read-only
      `previewPlanarHoleFill` contract reports retained, removed, and proposed
      triangle counts plus the exact proposed vertex triplets before authoring;
      proposed triangles now also render as a world-space viewport line overlay
      with a clear-preview toggle and without authoring changes; richer hole
      classification remains pending.
- [x] LC-126 Normalize face counts, indices, subdivision metadata, normals, and
      interpolation tokens before USD authoring.
      Cleanup output is now validated for non-empty triangle shape, finite
      positions, index bounds, and aligned normal/UV/color/custom buffers before
      USD authoring. Material-group ranges are validated too and existing
      `GeomSubset` face indices are updated before authoring; malformed or
      overlapping input ranges are rejected before worker processing. Non-empty
      authored group lists must also cover the complete ordered index buffer,
      preventing unassigned faces from losing material bindings. The
      cleanup worker transfers explicit face-varying buffers without losing
      corner indices; switching a primvar from face-varying to vertex
      interpolation now removes its stale index array for UVs, normals,
      colors, skin data, tangents, and custom primvars; interpolation-token
      normalization for other authored properties remains pending. Rewrites
      also remove stale aligned declarations when compact USDA properties share
      a line, with a regression covering same-line UV index cleanup. Rewrites
      with no material groups now clear existing `GeomSubset` index arrays so
      stale face assignments cannot survive a cleanup.
      Mesh-authoring validation uses bounded typed-array scans instead of
      transient spread clones for untrusted UV, skin, and custom attributes.
      Authored normals are now copied into a fresh Float32 buffer and
      normalized to unit length before serialization; zero-length and
      non-finite vectors are rejected rather than inventing a shading basis.
      The operation-layer cleanup and worker-result validation boundaries use
      the same no-clone scan policy for topology and aligned buffers.
      Retopology now repeats optional aligned-attribute and skin-value checks
      inside the worker before typed-array coercion, covering direct worker
      callers as well as the UI operation path.
      Bounded subdivision metadata (`interpolateBoundary`,
      `faceVaryingLinearInterpolation`, and `triangleSubdivisionRule`) is now
      normalized and authored from the mesh boundary; broader subdivision
      topology metadata remains pending. Interpolation-token
      normalization now canonicalizes all standard USD tokens (`constant`,
      `uniform`, `varying`, `vertex`, `faceVarying`, and `instance`) while the
      individual authoring operations continue to enforce their supported
      data cardinality. The USD session authoring boundary now validates and normalizes
      indexed positions/indices before serializing topology and primvars, and
      accepts only the USD allowlist (`none`, `catmullClark`, `loop`) for an
      explicit subdivision scheme.
      Provided UV, normal, color, tangent, and skinning buffers are now rejected
      when their aligned cardinality or finite/integer constraints are invalid,
      preventing stale authored primvars from surviving a failed replacement.
      Cleanup welding keys and worker transfers now include valid aligned tangent
      frames, so vertices with distinct tangent handedness cannot be merged.
      Safe cleanup now also forwards complete skinIndex/skinWeight buffers for
      skinned meshes, validates their influence data, and compacts them with
      the vertices; morph-target meshes remain explicitly unsupported.
      Explicit custom and face-varying primvars now receive the same strict
      name, supported-typed-array, stride, finite-value, cardinality, and
      corner-index validation; unsupported backing containers are rejected
      instead of being silently omitted from the authored mesh.
      Caller-declared custom interpolation is now explicit: vertex custom
      attributes accept only `vertex`, and indexed face-varying attributes
      accept only `faceVarying`; unsupported tokens are rejected rather than
      silently rewritten. Broader interpolation-token support remains pending.
      Display-color edits now author an explicit `constant` interpolation and
      remove stale `primvars:displayColor:indices`, so a single edited RGB
      value cannot inherit obsolete face-varying indexing; replacement handles
      both compact and multiline USDA array formatting without duplicating the
      authored primvar.
      Cleanup also rejects malformed skin influence cardinality, non-integer or
      out-of-range joint indices, and non-finite/negative weights before the
      typed-array conversion that would otherwise truncate or silently drop
      unsafe data.
      The USD mesh authoring boundary applies the same paired-buffer,
      non-negative-weight, and unsigned-16-bit joint-index checks to direct
      callers.
      Cleanup authoring opts into an explicit clear-missing-attributes mode,
      removing stale aligned UV, normal, color, tangent, and skin primvars
      when the validated cleanup result has no corresponding buffer.
      It also receives the source custom-primvar name inventory and removes
      only known custom primvars omitted by validation, preserving validated
      custom attributes and unrelated authored data.
- [x] LC-127 Display changed-element overlays and before/after issue counts.
      Cleanup activity entries now retain before/after triangle and vertex counts
      plus deterministic issue-count deltas; geometry repair, UV, and retopology
      activity entries now retain the same issue-count delta. After a mesh
      mutation, Lucia draws a disposable capped pre-operation triangle-edge
      overlay over the rebuilt scene so removed or moved topology remains
      visually attributable without entering the USD or command snapshot.
- [x] LC-128 Add Preserve Original, Game-ready, Physics-ready, and Aggressive AI
      Cleanup presets.
      The cleanup inspector now exposes deterministic Preserve Original,
      Game-ready, Physics-ready, and Aggressive presets with bounded scale-aware
      welding tolerances; the local assistant forwards named preset requests;
      simple planar boundary filling is confirmation-gated and now composes with
      component area/volume filtering after rebuilding topology; semantic AI
      cleanup remains pending.

Acceptance criteria:

- Attributes remain aligned after vertices are welded or duplicated.
- Safe cleanup is idempotent: a second run produces no additional changes.
- Invalid output cannot be applied to the working stage.
- The operation has fixture coverage for empty, non-indexed, face-varying,
  disconnected, and malformed meshes.

### LC-140 — Error-driven Reduction and LODs

**Goal:** Produce usable lower-detail geometry while preserving silhouettes,
boundaries, UV seams, and material boundaries.

Candidate foundation:
[meshoptimizer](https://meshoptimizer.org/) provides attribute-aware
simplification plus vertex-cache, overdraw, fetch, and quantization tools. Its
MIT license requires attribution in product documentation or credits.

Tasks:

- [x] LC-141 Integrate the pinned vendored meshoptimizer version in native and WASM builds.
- [x] LC-142 Replace uniform face sampling with error-driven simplification.
- [x] LC-143 Support target triangle count, percentage, and maximum geometric error.
- [x] LC-144 Lock open boundaries, UV seams, material borders, and user selections.
      Retopology now deterministically locks duplicated-position UV seams in
      addition to open/material boundaries, authored crease/sharp-edge
      endpoints, and explicit user vertex locks. Assistant and direct callers
      may now provide validated sharp-edge pairs for the same lock path;
      the mock assistant also forwards pairs mentioned in reduction requests;
      richer interactive seam selection policies remain pending. The Operations
      inspector exposes the lock-border toggle, selected vertex list, and
      sharp-edge pairs used to construct the deterministic mask.
      meshoptimizer simplification now enables border locking by default and
      preserves UV seams through the indexed attribute-aware path; complete
      ordered material ranges are simplified independently so their borders
      cannot collapse across subsets. Open-boundary vertices are now derived
      into an explicit lock mask and merged with validated caller-supplied
      `lockedVertices`. The Operations inspector accepts validated zero-based
      vertex indices and inclusive ranges (for example `0, 4, 10-13`) and
      passes the resulting mask to the worker. Shift-clicking a mesh vertex in
      the viewport adds it to the per-mesh selection and Alt-clicking removes
      it. UV-seam locking is now an explicit Operations control, enabled by
      default and forwarded to the worker, so users can intentionally trade
      seam preservation for additional simplification. Assistant retopology
      requests validate the same boolean and understand “without UV seams”;
      assistant-generated LOD chains carry the same preference through every
      generated level. The Operations control persists across inspector/tab
      rerenders, and browser smoke verifies the visible state is retained. The
      public operations setter and assistant tool validators reject malformed
      seam-policy values before a worker is created.
      it; freehand vertex painting remains future work.
      Locked vertices are also shown as a disposable amber viewport point
      overlay, with no mutation to the loaded Three.js geometry.
- [x] LC-145 Include normals and UVs in the simplification error metric when both channels are present.
- [x] LC-146 Report achieved reduction and normalized geometric error.
- [x] LC-147 Add an interactive before/after wipe and error heatmap.
      Mesh mutations now retain capped pre/post world-space edge captures and
      expose an accessible viewport wipe slider; the two sides are clipped at
      the selected split and color/legend text identifies before versus after.
      A correspondence-independent nearest-point displacement comparison now
      provides bounded normalized heatmap samples and a viewport point heatmap;
      true triangle-surface correspondence remains future work. Comparison
      distance aggregates now use scalar reductions instead of a temporary
      distance array.
- [x] LC-148 Generate configurable LOD chains and distance previews.
- [x] LC-149 Author LODs as explicit sibling meshes or USD variants; never replace
      the source without keeping an undoable original.
- [x] LC-150 Optimize the reduced index order for the post-transform cache and
  compact vertices in that final fetch order.

Acceptance criteria:

The current slice provides a configurable `generateLODChain` operation and
authors deterministic `_LOD1`, `_LOD2`, … sibling meshes from the untouched
source. Complete material-range subsets are retained on generated siblings.
Distance-based viewport selection, partial/unassigned subset handling, and
variant authoring remain pending.

- Output contains no out-of-range indices or degenerate triangles.
- Locked boundaries and seams are preserved in regression fixtures.
- LOD paths and variant names are deterministic and collision-safe.
- Reduction can be cancelled without changing the stage.

### LC-160 — UV Generation and Inspection

**Goal:** Generate bakeable texture coordinates and make UV quality visible.

Current foundation: [xatlas](https://github.com/jpcy/xatlas), vendored under
`src/external/xatlas/`. The pinned core is a small, dependency-free C++11
library that provides chart generation, parameterization, packing, progress
callbacks, and seam vertex remapping. The adapter owns checked mesh conversion,
aligned primvar transfer, deterministic option normalization, and USD authoring.
The vendored snapshot is intentionally isolated so it can be replaced later if
a newer atlas generator meets the same dependency-free native/WASM constraints.

Tasks:

- [x] LC-161 Add UV checker, overlap heatmap, distortion heatmap, island overlay,
      and texel-density metrics.
      UV checker/wireframe and a coarse overlap heatmap are now available;
      distortion heatmap, density variation metrics, and explicit island-boundary
      overlays are now available.
- [x] LC-162 Add planar, box, cylindrical, and spherical projection.
      Lucia exposes deterministic projection modes alongside xatlas and authors
      the result through the same undoable mesh-edit path; projection and atlas
      operations both honor the selected default `st` or lightmap `st1` set.
- [x] LC-163 Integrate the pinned xatlas native/WASM build behind a small
      LightUSD UV-parameterization adapter.
      The adapter validates output buffer shapes, finite values, index bounds,
      source-remap cardinality, and source-remap bounds before USD authoring;
      the worker carries the input source-vertex count across the boundary so
      malformed xref values cannot pass when no aligned attributes are present.
- [x] LC-164 Expose chart, seam, padding, resolution, rotation, and density controls.
      Lucia now exposes padding, resolution, chart rotation, texels/unit, and
      maximum chart size, seam weighting, and deterministic balanced/lightmap/
      quality presets.
- [x] LC-165 Correctly duplicate points and every aligned primvar at UV seams.
      xatlas source-index mapping now transfers normals and vertex colors;
      authored vec4 tangent frames are also remapped through seam duplication
      when present, and are retained when normal-based recomputation is not
      available; tangent input and output cardinality/finiteness are validated
      at the xatlas worker boundary;
      four-influence skin indices/weights are now transferred and authored as
      vertex primvars; arbitrary vertex-aligned attributes are now remapped and
      authored as vertex primvars; remap indices, names, strides, and finite
      values are validated before authoring; duplicate aligned-attribute names
      are rejected at the xatlas request, worker-result, and USD-authoring
      boundaries instead of silently overwriting a primvar. Attribute-only
      normal/tangent updates preserve existing material subsets. Existing material groups are
      now transferred when xatlas proves one-to-one face order through `xref`;
      xatlas results must include a complete `xref` before any seam-sensitive
      data can be authored;
      unsafe reorderings and unsupported morph/subset cases remain guarded.
      UV/topology/normal authoring now removes stale tangent primvars whenever a
      replacement tangent frame is not supplied; selected sharp-edge policies
      remain guarded future work. Xatlas custom-attribute input and seam remap
      now require supported numeric typed arrays, preventing plain-array data
      from being accepted and later dropped at the USD authoring boundary.
      Request validation also rejects non-iterable primary and aligned buffers
      before the xatlas worker is created, including null or non-object
      top-level request values.
      Exported seam-remap helpers apply the same non-empty `xref` and
      malformed-attribute checks when called outside the worker. UV atlas
      result validation also requires custom attributes to remain supported
      typed numeric buffers, preventing a plain-array result from being
      silently discarded during USD authoring. Reserved USD channel names are
      rejected at this same result boundary so replacement channels cannot
      collide with the authoring path.
      Material-group transfer correctly treats `xref` as output-vertex aligned
      rather than requiring it to have the corner-index buffer’s length.
      The exported transfer helper enforces the same typed result buffers when
      called directly outside the unwrap operation.
      Custom xatlas result names that collide with aligned loader channels are
      also rejected before USD authoring.
      The primary xatlas positions, UV, index, and source-remap buffers are
      likewise required to remain typed numeric arrays at the exported result
      boundary, as are optional chart IDs and aligned seam attributes.
- [x] LC-166 Author `primvars:st` with correct face-varying interpolation and
      explicit primvar indices when supplied; xatlas output continues to use
      vertex interpolation because it duplicates seam vertices.
- [x] LC-167 Support authoring a separate lightmap UV set as `primvars:st1`.
      Atlas, projection, and UV-transfer operations preserve the selected set;
      the toolbar and assistant accept explicit `lightmap`/`st1` requests and
      reject unknown UV-set names before mutation. Lightmap UV transfer keeps
      tangent recomputation tied to the default `st` set, avoiding accidental
      normal-map changes when only `st1` is edited.
- [x] LC-168 Transfer UVs from a source mesh to reduced or reconstructed geometry.
      The deterministic retopology worker preserves source UV values and now
      emits an auditable source-vertex remap; a conservative two-sided normal
      ray helper now transfers source UVs to target vertices by nearest-hit
      barycentric interpolation; the undoable `scene.transfer_uvs` operation
      runs those queries in the LightRT/BVH projection worker and is exposed
      through the assistant and a confirmation-gated toolbar action. General
      reconstructed-surface integration remains pending.
      Per-vertex hit masks now preserve pre-existing target UVs for misses
      instead of silently authoring `(0, 0)`; malformed masks are rejected at
      the worker boundary, and targets without authored normals now receive a
      deterministic topology-derived normal fallback before transfer; malformed
      authored normals remain rejected at the same boundary.
      the operation boundary.
      Target material groups are now validated and carried through UV transfer,
      preventing existing GeomSubset assignments from being cleared by mesh
      authoring.
      When target normals are present, the operation also regenerates the
      target tangent frame from the transferred UVs before authoring, avoiding
      stale normal-map tangents. Transfer now selects the source `uv1` buffer
      and preserves target misses within `primvars:st1` when the lightmap set
      is requested.
- [x] LC-169 Add checkerboard preview and atlas export.
      The Health UV preview includes checkerboard/wireframe/overlap rendering
      and exports both a preview PNG and a 2048px atlas PNG.
- [x] LC-170 Normalize xatlas chart packing options and expose deterministic
      option presets. Lucia clamps and normalizes xatlas options at the worker
      boundary and exposes balanced/lightmap/quality defaults, including
      explicit convex-hull axis rotation and optional 4x4 block alignment.
      Fixed-density multi-atlas results receive up to three bounded retries at
      increasing single-atlas resolutions; results that still span multiple
      atlases are rejected before authoring instead of being silently collapsed
      into a single UV set. Native output also exposes per-vertex chart and atlas
      indices for future multi-atlas authoring. The worker validates finite
      positions, complete in-range triangles, matching optional attributes,
      aligned chart metadata, and bounded numeric atlas options before invoking
      the WASM module.

Acceptance criteria:

- Generated coordinates and indices pass USD mesh validation.
- Configured padding holds at the requested texture resolution.
- UV output is deterministic for the same mesh and settings.
- Seam duplication does not corrupt normals, colors, skin weights, or subsets.

### LC-180 — Normals and Tangents

**Goal:** Repair shading data independently of topology repair.

Tasks:

- [x] LC-181 Detect non-finite, zero-length, mis-sized, and inverted normals.
      Asset Health combines authored-normal validation with topology-aware face
      orientation checks and exposes deterministic repair actions.
- [x] LC-182 Recompute area-weighted vertex normals for indexed and non-indexed
      meshes. Face-varying normal authoring is also available through the
      explicit `faceVarying` interpolation mode, which emits one normalized
      face normal per corner plus `normals:indices`; weighted smoothing remains
      a vertex-interpolation policy.
- [x] LC-183 Add angle-based smoothing and weighted normals.
      Lucia now exposes deterministic area- and angle-weighted recomputation
      with a bounded smoothing-angle control; split-vertex sharp-edge authoring
      remains pending. The shared recompute boundary rejects unknown weighting
      policies and non-finite or out-of-range smoothing angles instead of
      silently selecting a different shading policy. Non-manifold faces that
      only meet at a vertex are excluded from the same smoothing fan rather
      than being averaged across an unshared edge.
- [x] LC-184 Preserve selected sharp edges and material boundaries.
      Normal recompute can preserve material boundaries from validated geometry
      groups; the worker now accepts validated explicit sharp-edge vertex-pair
      masks and treats those edges as smoothing barriers. USDA crease chains
      (`creaseIndices`/`creaseLengths`) are now consumed automatically when no
      explicit mask is supplied. Interactive edge selection and authored
      `creaseSharpness` are now honored when reading chains: zero or
      negative-strength chains are omitted from smoothing barriers, positive
      chains expand deterministically into adjacent sharp edges, and malformed
      per-chain sharpness arrays reject safely. Re-authoring emits full
      sharpness by default and preserves optional positive per-edge values via
      `sharpEdgeSharpness`; chain-level crease data is now preserved for normal
      and retopo re-authoring, while advanced interactive chain topology editing
      remains future work.
      Chain count and total chain-vertex input are also bounded by the indexed
      mesh safety limit before barrier expansion, including assistant/API input.
      Text-form chain selection is separately bounded to 1 MiB before parsing.
      Explicit sharp-edge selections use the same text limit and a bounded
      unique-edge count before normal or retopo barrier construction.
      Normal recomputation now reads authored crease strengths as expanded
      edge data, deduplicates overlapping chains using the strongest weight,
      and carries those strengths back through USD authoring.
      Partial: normal recomputation now treats authored material group ranges as
      smoothing barriers by default, with an explicit inspector opt-out; the
      Operations inspector accepts validated explicit sharp-edge vertex pairs
      (`0:2; 4:5`) and passes them to the worker. The shared normal core now
      rejects self-pairs and out-of-range endpoints, including malformed
      assistant/API or authored metadata input, instead of silently ignoring
      it. Selected sharp-edge pairs are now normalized and authored as
      deterministic two-vertex USD crease chains with full sharpness; the
      session can round-trip those chains. Material-group ranges now pass
      through the same checked topology boundary before worker creation, so malformed groups cannot leave a
      rejected normal operation with a live worker. Selected sharp-edge/crease
      primvar authoring is covered for explicit vertex-pair selections, and
      existing crease chains are prefilled in the normal-repair inspector;
      clearing that field explicitly authors empty crease arrays rather than
      falling back to stale authored edges.
- [x] LC-185 Integrate MikkTSpace-compatible tangent generation after UV changes.
      Lucia now computes a Lengyel/Mikk-compatible vertex tangent frame in a
      worker, authors `primvars:tangents`, and exposes an undoable repair action.
- [x] LC-186 Support normal-map Y convention inspection and conversion.
      Generated normal bakes now expose OpenGL (+Y) and DirectX (-Y) output;
      Health texture inventory now reports authored/generated Y-convention
      metadata; browser conversion now supports non-square project image assets
      with explicit confirmation and undo.
- [x] LC-187 Add normal/tangent visualization and before/after shading comparison.
      Lucia previews sampled normal/tangent frames and reports deterministic
      mean/max angular deltas against a captured baseline.
      The Operations inspector now shows a read-only sampled object-space frame
      preview with normal/tangent overlays; before/after shading comparison
      now retains a user-captured baseline and reports mean/max angular deviation
      for the sampled normal and tangent frames; reference-renderer parity remains
      pending.

Acceptance criteria:

- Tangents are regenerated or invalidated whenever their source UVs/normals change.
- Results remain consistent between Lucia preview and a reference renderer fixture.
- Interpolation and element counts are valid when authored back to USD.

## 6. Phase 2: make assets production-ready

### LC-200 — Material Audit and Simplification

**Goal:** Convert complex or unsupported networks into predictable portable PBR
materials with a clear fidelity report.

MaterialX should be the graph-analysis/interchange layer. Its current standard
includes reusable nodes and physically based shading definitions; its examples
cover Standard Surface, UsdPreviewSurface, and glTF PBR. See the
[MaterialX specification](https://materialx.org/Specification.html) and
[developer reference](https://materialx.org/DeveloperReference.html).

Tasks:

- [x] LC-201 Inventory shaders, nodes, bindings, subsets, textures, and color spaces.
      Asset Health now emits deterministic material and texture inventories,
      including shader names, graph inputs, subset ranges, binding slots,
      dimensions, byte estimates, and color-space metadata.
      Health reports now inventory material shader types, serializable node graphs,
      mesh bindings, texture slots, dimensions, color spaces, and geometry subset
      ranges, and flag sRGB tagging on data maps; full USD binding provenance
      remains pending.
- [x] LC-202 Detect unsupported nodes and identify bakeable graph subtrees.
      Health reports unsupported shader/node IDs, unreachable graph nodes, and
      the subset of nodes that can be evaluated by Lucia's bake operations.
      Health reports now flag ShaderMaterial, RawShaderMaterial, unknown material
      types, and unknown serializable graph nodes as not fully auditable, and
      identify basic constant/texture/arithmetic nodes that are candidates for
      baking; texture/image-backed connected-subgraph evaluation now accepts an
      explicit caller-owned resolver.
      A read-only evaluator now resolves finite literal arithmetic outputs and
      fails closed with separate unsupported-node and unresolved-reference lists.
      Float, color, and vector literal node variants are evaluated with the
      same finite-value and vector-cardinality limits.
      Image and texture nodes remain unsupported unless that resolver returns a
      finite scalar or 1–4 component vector; invalid resolver results are
      rejected without invoking arbitrary shader code.
      Resolver callbacks receive detached node snapshots, so a resolver cannot
      mutate the serialized source graph through the evaluation boundary.
      Evaluation depth is bounded to prevent pathological serialized graphs
      from exhausting the JavaScript call stack.
      Evaluation also bounds the number of visited nodes, preventing wide
      graphs from exceeding the evaluator’s deterministic work budget.
      Surface/material roots return deterministic evaluated channel maps, while
      explicit output roots continue to return their selected scalar/vector
      output value.
      When a caller requests one channel, unrelated unsupported shader inputs
      do not invalidate that channel’s independent evaluation.
      The read-only `scene.inspect_material_graph` assistant tool exposes the
      optimizer and evaluator summaries for the selected mesh without changing
      the stage or material graph; callers may request a validated output
      channel explicitly.
      Channel evaluation also follows an `output.surface` connection into a
      standard-surface/material root, matching common USDA graph topology while
      retaining fail-closed handling for unsupported nodes.
- [x] LC-203 Constant-fold graphs and remove unreachable nodes and inputs.
      The deterministic material-graph optimizer folds literal constants,
      arithmetic/unary/clamp/mix subgraphs, and removes nodes unreachable from output or
      surface roots; USD graph authoring remains confirmation-gated.
      The authored fold also handles finite color3f/float3/vector3f triples
      and color4f/float4/vector4f tuples for the supported arithmetic IDs,
      with strict same-shape or scalar-broadcast evaluation and typed tuple
      emission.
      Serializable graph folding also handles literal scalar/boolean
      `select`/`if` nodes, while vector predicates remain unsupported and are
      preserved for review. The confirmation-gated USDA fold supports the
      corresponding `UsdSelect` node with the same finite scalar/boolean
      condition policy.
      The same serializable subset folds `ifgreater`, `ifgreatereq`, `ifless`,
      `iflesseq`, and `ifequal` nodes with finite scalar comparands and finite
      selected branches; vector comparands remain untouched.
      Implemented: serializable graphs identify nodes reachable from output/surface
      roots and preserve opaque or externally referenced nodes; cleanup also
      retains external references to a nested Shader prim whether the reference
      names the prim itself or one of its properties. Confirmation-gated USDA
      authoring covers the allowlisted scalar and 3/4-component literal subset;
      unsupported node families remain unchanged and are reported as informational
      cleanup candidates. Folded nodes have their consumed inputs removed in the
      immutable rewrite preview, while authored changes remain confirmation-gated.
      The preview optimizer and immutable rewrite snapshot now perform the
      deterministic constant folding and input inlining described above;
      folded preview nodes are emitted as self-contained constants with their
      consumed inputs removed, so stale references cannot survive the fold;
      retained unresolved or unsupported nodes are deep-cloned as well, keeping
      opaque rewrite-preview inputs detached from the authored graph.
      Optimizer snapshots now accept `parameters` as the serialized input
      container as well as `inputs`, matching the evaluator's MaterialX-style
      graph contract.
      Both graph paths reject array, typed-array, and scalar input containers
      instead of spreading their implementation properties into synthetic
      shader inputs.
      Map-backed input containers are normalized to ordinary key/value records
      across optimization, evaluation, inventory, and translation, preserving
      the same fail-closed boundary for unsupported container types.
      Optimizer and rewrite snapshots preserve the authored `inputs` versus
      `parameters` container spelling instead of introducing a duplicate field.
      Rewrite previews also report the sorted input keys removed when folded
      nodes are inlined, making cleanup reviewable before authoring; the mesh
      inspector surfaces the aggregate count alongside the fold and unreachable
      node counts.
      Asset-report graph collection discovery now reuses the same Map/array/object
      node-entry normalization instead of maintaining a divergent parser.
      Asset-report inventory and graph fingerprints reuse the same normalized
      input-map boundary, keeping diagnostics and equivalence analysis aligned
      with optimizer/evaluator fail-closed behavior.
      Immutable rewrite snapshots now use the same shared root selection when
      deciding which folded nodes may remain as graph roots.
      The shared root helper accepts plain serialized node maps, arrays, and
      Map collections while preserving their stable node identifiers.
      Id-less array nodes receive the same deterministic `node:<index>` IDs as
      the graph parser, keeping root selection stable at the API boundary.
      Explicit numeric node IDs, including zero, are preserved through the same
      normalization boundary instead of being mistaken for missing IDs.
      the allowlisted literal subset also folds common min/max, abs, sqrt,
      floor, ceil, and saturate utility nodes while retaining fail-closed
      behavior for unknown operations;
      clamp and mix folding resolves canonical semantic input names before
      positional fallback, so serialized input-property order does not alter
      the folded result;
      binary arithmetic folding likewise resolves `a`/`b` or `left`/`right`
      (including `input1`/`input2`) before positional fallback for stable
      subtraction, division, and power evaluation;
      literal `lerp` and `smoothstep` utility nodes use the same semantic-name
      resolution and remain rejected when their inputs produce non-finite
      results, including equal smoothstep edges;
      In-memory unary graph nodes resolve `value`, `input`, and `x` aliases
      independently of serialized property order, while retaining extra
      metadata inputs in the immutable snapshot.
      authored MaterialX unary IDs `ND_sign_*`, `ND_round_*`, `ND_fract_*`,
      `ND_exp_*`, and `ND_log_*` now fold finite scalar/vector inputs while
      retaining invalid logarithm domains;
      Common MaterialX one-minus and negate aliases (`ND_one_minus_*`,
      `ND_oneminus_*`, `ND_negate_*`, and `ND_negative_*`) use the same
      confirmation-gated authored folding path.
      The common `ND_invert_*` color/vector alias is also folded as a
      componentwise one-minus operation when its finite typed input is safe.
      `ND_reciprocal_*` and `ND_inverse_*` use the same finite, zero-safe
      authored folding path.
      `ND_sin_*`, `ND_cos_*`, and `ND_tan_*` are also supported for finite
      scalar/vector inputs, with non-finite results retained for review.
      Scalar `ND_saturate_float` now uses finite clamp semantics, while the
      color3 luminance-blend form remains restricted to color3 inputs.
      Arithmetic folding rejects incompatible vector shapes and non-finite folded results
      rather than producing invalid graph values.
      A confirmation-gated USDA authoring path now folds direct finite scalar
      `UsdAdd`, `UsdSubtract`, `UsdMultiply`, `UsdDivide`, `UsdMin`, `UsdMax`,
      `UsdPower`, `UsdAbs`, `UsdNegate`, `UsdSqrt`, `UsdFloor`, `UsdCeil`,
      `UsdOneMinus`, `UsdSaturate`, `UsdClamp`, `UsdLerp`, `UsdSmoothStep`,
      `UsdDot`, `UsdLength`, `UsdNormalize`, and `UsdCross` Shader nodes into
      connected consumers and removes them only when no external references
      remain; compact USDA statement formatting is supported and unsupported or
      externally referenced nodes are retained.
      The authored path also accepts the common `UsdInverse` reciprocal alias,
      retaining zero-denominator nodes for review just like `UsdReciprocal`.
      Common MaterialX arithmetic IDs (`ND_add_*`, `ND_multiply_*`,
      `ND_dotproduct_*`, `ND_crossproduct_*`, and the allowlisted unary forms)
      are also recognized with their `in1`/`in2`/`in` and `out` conventions;
      unknown MaterialX nodes remain untouched.
      The reviewed `ND_convert_*` scalar/color3/vector3 subset also folds
      explicit scalar broadcast and color3/vector3 layout conversions, while
      dimensional mismatches remain unchanged.
      Typed MaterialX `ND_constant_float`, `ND_constant_color3`,
      `ND_constant_vector3`, `ND_constant_color4`, and `ND_constant_vector4`
      nodes are likewise inlined only when their finite `inputs:value` is
      structurally referenced by a local consumer.
      MaterialX `ND_ifgreater_*`, `ND_ifgreatereq_*`, `ND_ifless_*`,
      `ND_iflesseq_*`, and `ND_ifequal_*` branches are folded when comparison
      operands and both branch values are finite literals, accepting either
      `value1/value2` or `in1/in2` naming.
      MaterialX `ND_remap_*` and `ND_range_*` nodes fold componentwise using
      `in/inlow/inhigh/outlow/outhigh`, including scalar broadcast and the
      deterministic zero-width input-range fallback.
      `ND_mix_*` folds finite `bg`/`fg` scalar or vector values with a finite
      scalar `mix` factor, preserving strict shape compatibility.
      MaterialX `ND_saturate_*` uses its distinct luminance-blend semantics
      (rather than `UsdSaturate` clamping) for finite color3/vector3 input and
      scalar `amount`.
      `ND_extract_*` folds bounded integer component selection from `inputs:in`
      and `inputs:index`; malformed, fractional, or out-of-range indices are
      retained for review.
      `ND_combine2_*`, `ND_combine3_*`, and `ND_combine4_*` fold scalar
      `in1`…`in4` channels into finite typed tuples. `ND_separate2/3/4_*`
      identity folds now select the referenced `out[rgbaxyzw]` component;
      other channel-routing nodes remain pending because their output-name
      semantics are not inferred.
      Literal `ND_swizzle_*` channel strings using `rgbaxyzw` are also folded
      with explicit source-component bounds checks; dynamic or invalid channel
      strings remain unchanged.
      `ND_luminance_*` folds finite 3-component color/vector inputs to a
      scalar using the canonical Rec.709 coefficients.
      The Blender-compatible `ND_hsvadjust_color3` variant and the native
      `ND_hsv_adjust_color3` factorized form fold finite RGB/control inputs
      through deterministic HSV conversion; malformed or non-finite controls
      remain unchanged.
      Dependency-ordered folding also resolves relative and absolute local
      output references, allowing safe scalar arithmetic chains to collapse in
      one optimization pass. Nodes with remaining internal prim/property
      references are folded for connected consumers but retained in the USDA
      source, alongside externally referenced nodes.
      Subtraction, division (with zero-divisor rejection), powers, unary
      one-minus/negation, and clamping are folded when all inputs are finite
      literals; unsupported nodes remain unchanged for later graph evaluation.
      Finite vector `dot`, `length`, `normalize`, and 3D `cross` utility nodes
      are folded with dimensionality and zero-length checks.
      In-memory graph optimization also folds finite `round`, `fract`, `sign`,
      `exp`, and positive-domain `log` utility nodes for scalar and vector
      values, retaining invalid logarithm inputs for review.
      Reciprocal/inverse utility nodes now fold finite scalar and vector
      values, while zero denominators remain unchanged for review.
      Sine, cosine, and tangent utility nodes likewise fold componentwise;
      non-finite inputs or results remain unchanged through the finite gate.
      Finite 1–4 component typed-array literals are normalized to numeric
      vectors before folding, matching loader-side GPU buffer representations.
      Reachable constant nodes are emitted with portable plain-array values,
      so optimized graph snapshots do not retain typed-array implementation
      details.
      `LuciaOperations.previewMaterialGraph` now exposes the optimization
      result for a selected render mesh without authoring USD graph changes.
      The preview also exposes an immutable rewritten snapshot that inlines
      folded literals into consumers and removes folded/unreachable nodes;
      unresolved or unsupported nodes are retained for review.
      The selected-mesh inspector exposes the same preview with explicit
      folded/unreachable node counts and no mutation control.
      Optimizer and evaluator boundaries now reject malformed scalar `nodes`
      collections as empty graphs instead of enumerating their implementation
      properties as fabricated shader nodes.
      Texture resolver snapshots also reject cyclic or unsupported non-plain
      metadata values before invoking user callbacks, preserving fail-closed
      evaluation for malformed serialized graphs.
      Confirmation-gated USDA authoring now removes only nested Shader prims
      unreachable from the selected Material surface output. Authored values
      and connections are preserved verbatim; shaders referenced outside the
      Material are retained, and a graph without a resolvable surface root is
      a no-op. General USD constant folding and graph-node conversion remain
      pending.
      Serializable graph optimization now prioritizes explicit output roots,
      so disconnected generic surface helpers are removed while canonical
      shader roots remain available for output-less material snapshots.
      When an explicit output exists, even canonical shader roots must now be
      reachable from that output; malformed disconnected shader fixtures are
      therefore classified as unreachable instead of being preserved.
      Health graph inventory and material-equivalence fingerprints share the
      same root policy, keeping unreachable-node diagnostics and deduplication
      candidates consistent with optimizer previews.
- [x] LC-204 Merge structurally identical materials and repair redundant bindings.
      Partial: Health now detects structurally equivalent materials using stable
      shader, scalar, color, and texture-slot fingerprints, and reports a
      deterministic merge candidate. A confirmation-gated USD binding rewrite
      now accepts an explicit old→canonical material path map and redirects only
      exact `material:binding` targets without deleting definitions; mapping
      chains now collapse to one canonical target and cycles are rejected before
      authoring. A separate confirmation-gated definition merge now removes an
      explicitly mapped duplicate `Material` only after rewriting exact bindings
      and proving no residual path references remain. Lucia now derives a
      deterministic nested-USD path map for structurally identical Material
      definitions and exposes the automatic merge through the assistant;
      graph-aware equivalence remains conservative and bounded to local shader
      topology; unsupported or dynamic graphs remain distinct for review.
      Material equivalence canonicalizes top-level authored property order
      while preserving nested array/block structure; unsupported graph forms
      remain outside the merge candidate set.
      The inspector and session also expose the exact deterministic old→canonical
      mapping as a non-mutating preview before confirmation.
      The assistant exposes the same mapping through the read-only
      `scene.inspect_material_merges` tool before the confirmation-gated merge.
      Comment stripping is quote-aware, so hash characters in authored string
      values cannot collapse distinct material definitions.
      Material repair entry points also normalize null/non-object mappings
      before validation, avoiding incidental boundary exceptions.
      Render-material fingerprints now include a deterministic graph topology
      and literal/connection signature independent of node IDs, so Health can
      identify equivalent serialized material graphs without merging them.
      USDA material equivalence now canonicalizes nested shader-graph property
      order as well as top-level material properties; graph node identity and
      connection remapping remain intentionally conservative. Local Shader
      node names and relative connection targets are now canonicalized
      quote-safely, so equivalent graphs with renamed internal nodes can be
      identified without rewriting external paths or authored strings.
      Canonical local Shader IDs are assigned from stable node-body signatures,
      so equivalent graphs remain merge candidates when their nested Shader
      declarations are authored in different orders. Disconnected serialized
      graph nodes are excluded from the equivalence fingerprint, so harmless
      dead-node baggage does not prevent a merge candidate; USDA merge
      candidates apply the same reachability rule to nested Shader declarations.
      Local connection references participate in bounded deterministic
      signature-refinement passes (up to the local shader count, capped at 16),
      preserving graph topology when otherwise similar nodes are connected to
      different local shader inputs, including deeper chains.
      Equivalent local output references with either `./Node` or `Node`
      spelling are canonicalized identically; external absolute references
      remain unchanged.
      Absolute references rooted at the selected Material and targeting a
      declared local Shader are canonicalized with the same narrow scope.
      Equivalence fingerprints also apply the safe finite scalar USDA graph
      fold, so direct-literal and arithmetic-equivalent material graphs can be
      recognized as the same reviewed candidate.
      Material declaration and nested Shader discovery are structural scans, so
      declaration-shaped text in USDA strings or comments cannot create phantom
      equivalence groups or merge candidates.
      Residual-reference checks during confirmed definition removal use the
      same structural mask, so path-looking prose does not block a safe merge;
      real non-binding references still retain the source definition.
      Equivalence analysis falls back to each material's structural body when
      optional graph folding or cleanup cannot safely analyze that material,
      keeping read-only health scans deterministic and non-throwing.
      Package-relative asset references are lexically normalized in
      equivalence fingerprints, while resolver and absolute references remain
      unchanged.
- [x] LC-205 Translate supported inputs among MaterialX Standard Surface,
      UsdPreviewSurface, and metallic-roughness PBR.
      A deterministic format-neutral translator now carries the shared base
      color, metallic, roughness, opacity, emission, normal, IOR, and specular
      channels where supported, and returns sorted unsupported inputs plus
      explicit rename/loss explanations. Equivalent aliases now use canonical,
      insertion-order-independent precedence. A confirmation-gated authored
      shader operation now renames recognized `inputs:` channels in place,
      preserves values and connections, refuses target collisions, and reports
      skipped channels; authored specular/IOR inputs are retained and reported
      as unsupported when translating to metallic-roughness; broader graph-node
      conversion remains intentionally outside this supported-input task. The USDA authoring scanner is quote- and
      comment-aware, so matching text in string values or comments is never
      renamed. The session can also resolve the exact shader
      connected to a selected mesh’s material binding for higher-level tools;
      the Material inspector exposes explicit source/target selectors when that
      connection is available. Conflicting equivalent aliases are now reported
      explicitly when the canonical alias wins, so translation previews do not
      silently hide a dropped authored contribution.
      Serializable material graphs also have a read-only translation preview
      that preserves opaque connections, normalizes the target shader node type,
      and reports unsupported channels and target-key collisions without mutation.
      The translation preview accepts Map-backed node collections alongside
      object and array forms, matching the optimizer/evaluator graph contract.
      It also updates an existing `type`, `nodeType`, or `category` discriminator
      in place, avoiding stale source-type fields in translated graph snapshots.
      Translation snapshots now deep-clone opaque connection values and
      untouched nodes, including typed arrays, so callers cannot mutate the
      source graph through a preview result.
      The clone boundary detects cyclic opaque values and fails closed with a
      stable validation error instead of recursing without a bound.
      Translation snapshots also reject unsupported non-plain opaque objects
      instead of silently converting them to empty object values.
      Graph translation now treats scalar, array, and typed-array node/input
      collections as malformed boundaries: malformed collections are preserved
      for review without inventing translated channel keys, while recognized
      shader implementation IDs may still be normalized.
      Authored USDA translation now also renames recognized MaterialX and
      UsdPreviewSurface implementation IDs, while leaving unknown or
      metallic-roughness IDs unchanged.
      Serializable graph translation recognizes the corresponding MaterialX
      implementation-ID node types as shader roots, so their portable channel
      aliases are translated consistently with named standard-surface nodes.
- [x] LC-206 Show unsupported/fidelity differences before conversion.
      The Material inspector now presents a read-only UsdPreviewSurface
      fidelity preview with translated values, renamed channels, and explicit
      unsupported-input/approximation notes before authored conversion; the
      translator also returns structured per-input loss reasons for downstream
      conversion and audit consumers.
      Translation and authored-property entry points normalize null or
      non-object request envelopes before format validation. Known channel
      values that are non-finite or outside the supported scalar/vector shape
      are rejected with an explicit loss reason instead of being forwarded.
- [x] LC-207 Convert material-only differences to parameters, primvars, or variants
      when the target profile supports them.
      A deterministic, read-only planner now identifies finite channel-only
      differences across material entries and selects an allowlisted primvar
      or variant strategy per target profile, retaining source aliases and
      explicit unsupported reasons; authored parameter conversion and
      schema-specific graph wiring are confirmation-gated and fail closed for
      unresolved shader graphs. The
      selected-mesh operation and read-only assistant tool now expose this
      planner without mutating material bindings or shader graphs.
      Parameterization discovery also evaluates finite literal-only values from
      serialized standard-surface/material graphs, keeping texture- and
      unsupported-node inputs explicitly out of the authored candidate set.
      Representation capabilities are stored in the canonical target-profile
      definitions and consumed directly by the planner. Reviewed primvar
      candidates can now be materialized into validated face-varying attribute
      buffers and authored through the existing confirmation-gated USD mesh
      path. The assistant exposes inspection separately from confirmed primvar
      authoring. Reviewed variant candidates can also be authored through a
      confirmation-gated mesh variant set with deterministic material
      selections, while the original Material definitions and base binding
      remain intact. Variant authoring requires absolute existing Material prim
      paths and rejects collisions or malformed selections; automatic mode
      continues to prefer primvars. Primvar materialization applies the shared
      indexed-mesh index ceiling before allocating face-varying output buffers.
      When authored Material paths and a resolvable surface Shader are
      available, confirmed primvar parameterization also inserts typed
      `UsdPrimvarReader_float` or `UsdPrimvarReader_color3f` nodes and rewires
      only the selected surface inputs; malformed or unsupported shader graphs
      remain unchanged while the mesh primvars are still validated.
- [x] LC-208 Consolidate per-face assignments without losing USD subsets.
      Adjacent same-material face ranges are consolidated before
      retopology, with complete ordered coverage validation; non-adjacent
      assignments are now stably reordered and consolidated before the worker,
      preserving every triangle and the resulting USD subset ranges. Group
      validation also requires a safe, triangle-aligned index-buffer count
      before any subset rewrite or worker transfer.

Acceptance criteria:

- Conversion never silently drops an unsupported contribution.
- The preview lists exact approximations and values used.
- Equivalent materials are merged deterministically.
- Material bindings remain correct through hierarchy and collection bindings.

### LC-220 — UV-space Texture Baker

**Goal:** Preserve appearance after retopology, reduction, or material
simplification.

Implementation note: the current Lucia slice replaces the gradient placeholder
with a worker-backed shared UV-space rasterizer for base color, vertex colors,
object-space normals, configurable dilation, malformed-face accounting, and
missed-texel reporting. The same worker family now routes LightRT/BVH
high-to-low projection and supported material channels through the shared bake
contract.

Tasks:

- [x] LC-221 Replace the current preview-gradient bake with a shared UV-space
      baking module. Use LightRT for high-to-low ray queries and the existing
      BVH implementation (`examples/common/bvh`) compiled into the WASM worker;
      use WebGPU/WebGL2 only for optional preview acceleration and readback.
- [x] LC-222 Bake base color, normal, roughness, metallic, occlusion, emissive,
      opacity, object ID, and material ID.
      Base color, normal, roughness, metallic, opacity, and emissive are now
      available; LightRT-BVH occlusion is now available; RGBA data textures
      are now sampled in UV space for supported material channels; deterministic
      object-ID and material-ID outputs are now available as raw RGB maps.
      Generated assets also record the baked channel and whether a CPU-readable
      source texture was sampled. UI and assistant bake commands now return a
      truthful incomplete result when cancellation or worker failure prevents
      asset creation, instead of reporting a download unconditionally.
- [x] LC-223 Support high-to-low projection, ray distance, cage offset, and misses.
      LightRT occlusion now exposes a bounded ray-radius control and transparent
      UV misses; the shared LightRT WASM binding now also exposes batched closest
      hits with distance, triangle ID, and (w,u,v) barycentrics for projection;
      a worker projection contract now rasterizes target UVs, applies bounded
      cage offset/ray distance, and returns source hit buffers; the session
      operation selects target/source meshes and validates the result;
      `bakeProjected` now interpolates source base-color or object-space-normal
      attributes into a transparent target texture with hit/miss statistics.
      Deterministic same-target-face dilation now fills eligible near misses
      without crossing UV ownership boundaries.
      Bake-worker results are now validated as non-array objects before any
      result fields are read, so malformed worker messages fail with a stable
      contract error instead of a raw property-access exception.
      Projection rays use world-space target/source geometry when prims carry
      independent transforms.
      Base-color, roughness, metallic, opacity, and emissive projection sample
      CPU-readable source RGBA images through interpolated source UVs when
      available, with scalar/material fallback; the bake records whether image
      sampling was used. Scalar maps consistently collapse to the red data
      channel before RGB output encoding.
      Projected bakes now estimate transferable ray, mesh, hit, ownership, and
      source-texture buffers before starting and reject requests above Lucia's
      bounded 256 MiB working-memory budget.
      Source image sampling now follows source face material indices, including
      per-material texture descriptors and color-space metadata; local and
      projected bakes now use supported literal graph channels as a validated
      per-material fallback before scalar/material defaults. Projected scalar/color fallback values also use
      source face material assignments when no source vertex attribute exists.
- [x] LC-224 Add supersampling, island dilation, and transparent-background handling.
      The shared rasterizer now supports deterministic 1×/4× coverage samples;
      dilation now tracks UV-island ownership and rejects cross-island source
      texels; transparent misses remain supported.
- [x] LC-225 Support channel packing and correct color/data texture color spaces.
      A deterministic RGBA channel packer now accepts normalized or byte planes;
      Lucia now exposes undoable LightUSD-backed packing of project image assets
      with per-slot source/channel provenance and linear/data output metadata;
      the Textures inspector exposes the output channel count, color space, and
      all four source/channel selectors; slots above the selected output count
      are disabled and excluded from authored provenance. Assistant and direct pack
      callers now reject unknown output color spaces, invalid output channel
      counts, unsupported slots, and fractional/out-of-range source channels
      before native packing, avoiding silent channel substitution; sources
      beyond the requested output count are rejected as well.
      Native packed and alpha-cleanup results are also required to carry
      non-empty encoded byte buffers with bounded dimensions and matching
      channel metadata before project assets are authored.
      The deterministic local assistant mock also routes explicit R/G/B/A
      source-texture packing requests, including an explicit 1–4 output
      channel count, to the same validated operation.
      Malformed null, array, unknown-slot, and non-object descriptor inputs now
      fail with the same stable pack-channel error at the direct API boundary.
      Generated pack output names are also restricted to safe package-relative
      PNG paths, matching the USDZ asset-path policy.
- [x] LC-226 Add texture resizing, duplicate detection, unused-alpha removal, and
      configurable maximum dimensions.
      USD Doctor detects byte-identical package members; the bake utility now
      provides deterministic bounded RGBA resizing and identifies removable
      opaque alpha channels; Health now detects byte-identical CPU-readable
      texture identities; proven-opaque generated assets can now be re-encoded
      as RGB PNGs from the Textures inspector with undo support.
      Oversized project image assets can now be resized with aspect-ratio
      preservation and undo support; the Textures inspector uses the active
      target profile's maximum dimension, while the assistant tool accepts an
      explicit maximum override. Resize now requires integral positive source,
      target, and limit dimensions. Alpha analysis now rejects malformed image
      dimensions and non-finite samples before classifying alpha as unused.
      Assistant and direct resize callers now reject non-finite, fractional,
      zero, and over-limit maximum dimensions before image decoding.
      The assistant execution route preserves explicit zero values instead of
      converting them to the default dimension before operation validation.
      It also reports resize success only after the undoable project change is
      actually recorded, so decode or encode failures cannot be reported as
      completed operations.
- [x] LC-227 Update USD material paths and package assets as one undoable command.
      Texture renames update exact USD references, package asset keys, and export
      remaps in one command; project package state is restored on undo/redo.
      New texture paths are constrained to safe package-relative syntax at both
      assistant and direct-operation boundaries.
- [x] LC-228 Add missed-texel counts and coverage ratio to the UV baker result.
      The result now also reports deterministic mean/max RGB encoding error and
      the number of compared covered texels; worker coverage, pixel cardinality,
      dilation count, island count, and error statistics are validated before
      texture authoring; high-to-low geometric error remains part of the
      projection work.

Architecture constraints:

- [x] LC-229 Add a worker-oriented LightRT/BVH baking ABI with transferable mesh,
      UV, material, and texture buffers; keep source USD and generated assets
      outside the command snapshot.
      The shared LightRT WASM binding now exposes batched BVH occlusion and
      closest-hit queries;
      the combined WASM target compiles the vendored xatlas implementation
      with `XA_MULTITHREADED=0`, and dedicated xatlas/LightRT WASM regressions
      verify the active source and ABI mapping.
      Lucia now uses the batched occlusion ABI for its LightRT occlusion bake;
      the projection worker now uses the same transferable closest-hit ABI;
      richer material buffers now transfer per-face material indices and packed
      scalar/color values to the worker, preserving multi-material channel
      semantics without including source USD in command snapshots; malformed
      index cardinality, negative indices, and non-finite material values are
      rejected before rasterization. CPU-readable image buffers are now carried
      per material with scalar fallback when an image is unavailable; malformed
      image descriptors are rejected before transfer. Projected baking now
      materializes deterministic 1×1 fallback descriptors for mixed textured
      and untextured materials, so an absent slot cannot dereference a null
      pixel buffer. All
      worker-facing geometry paths now validate finite positions, UV stride and
      count, and complete in-range index triplets before transferring buffers.
      The public CPU rasterizer also rejects negative/fractional indices and
      non-finite position/UV values before typed-array coercion, while
      out-of-range face references retain the deterministic skipped-face miss
      policy.
- [x] LC-230 Define deterministic UV-triangle rasterization, chart dilation,
      ray/cage miss policy, tangent-frame convention, and cancellation cleanup.
      Lucia now defines deterministic center-sample UV rasterization, fixed
      self-hit offset, seeded cosine-hemisphere occlusion rays, and transparent
      misses for the LightRT occlusion channel; dilation now preserves the
      deterministic UV-island owner for each propagated texel. Projected
      baking keeps that island owner separate from target triangle ordinals
      used for barycentric tangent-frame lookup; face ownership uses triangle
      ordinals consistently for multi-island meshes and per-face material/color
      lookup.
      all Lucia worker-backed operations now reject the active promise on
      cancellation and clean up the worker handle. Worker callbacks use
      identity-checked teardown across tangent, normal, retopo, cleanup,
      xatlas, skin-transfer, UV-transfer, projection, and bake workers, so a
      late callback from a cancelled operation cannot terminate or clear a
      newer operation. The progress overlay now
      exposes a worker-aware Cancel operation control; progress/cancellation
      cancellation policy unification remains pending. Successful results, runtime worker
      errors, and explicit worker error messages now all terminate and detach
      their worker handles; synchronous tangent, normal, cleanup, and raster
      workers now convert internal exceptions into the same explicit error
      protocol instead of leaving an operation pending. Transferable
      `postMessage` failures now also terminate and detach their worker before
      rejecting, covering clone and detached-buffer errors. Projection workers now release
      LightRT tracer/BVH handles with `finally` on both success and failure.
      UV transfer also rejects zero-length target normals before ray
      generation; invalid worker input is rejected
      before transfer with channel-specific Lucia errors, and normal/tangent
      worker outputs are validated for expected cardinality and finiteness
      before USD authoring; projection results also validate target-pixel
      cardinality, bounds, coverage, and target-face ownership before texture
      authoring. Normal baking now exposes an explicit object-space default
      and deterministic tangent-space encoding using the authored vec4
      tangent handedness convention; when tangents are absent or malformed,
      the bake derives a deterministic frame from positions, UVs, and normals
      and records whether the frame was authored or recomputed. Projected
      tangent normals transform source normals and target tangent directions
      into a common world-space frame before encoding, including determinant-
      based handedness flips for mirrored transforms. Assistant bake requests
      now validate channel and normal-convention enums before dispatch,
      matching the worker and authoring contract; direct operation callers now
      receive the same explicit normal-space and normal-Y validation. Assistant
      bake requests also reject non-finite, fractional, and out-of-range
      resolution, dilation, sample, and occlusion-radius values before a worker
      is created, keeping tool-call validation aligned with the operation
      clamps and memory limits.
      Projected-bake tool calls apply the same resolution and dilation bounds
      and validate ray distance and cage offset before projection work starts.
      Direct `LuciaOperations` bake and projected-bake callers enforce those
      finite typed control ranges too, before scene lookup or worker creation.
      Direct bake encoding now rejects a null `canvas.toBlob` result with a
      stable Lucia error instead of dereferencing it during asset authoring.
      Projected bake results now return covered-target ratio, projected-hit
      ratio, missed-hit count, and dilation count alongside the generated asset;
      asset provenance and assistant activity report the same values.
      Retopology and xatlas result validators also reject fractional,
      negative, or out-of-range joint indices and negative weights before
      remapped skin data reaches USD authoring.
      Retopology result metadata now also rejects missing-range sentinel values
      such as non-finite, fractional, or zero source vertex counts before an
      optional source remap is trusted.
      All worker result validators require joint indices and weights to be
      returned as a complete pair, matching the USD authoring contract. Large
      CPU projection and UV-bake buffers now use bounded finite/integer scans
      instead of spread-based temporary copies before transfer or rasterization.
      Direct projection attribute and UV-island helpers also reject
      non-iterable array-like inputs with the same stable shape error used by
      worker-facing callers.
      Operation progress now passes through one bounded UI normalization
      contract with a non-empty fallback message, covering malformed or
      out-of-range worker and synchronous progress payloads.
      All public operation progress callbacks now use that same normalized
      payload contract, including legacy overloaded entry points.
      Projection worker result validation likewise rejects null and array
      payloads before reading hit-buffer fields. Projection worker results also
      carry source and target triangle cardinalities, allowing face ordinals to
      be checked against the transferred meshes rather than only for
      non-negative sentinel validity. Hit distances must be finite and
      non-negative; the explicit `Infinity` distance sentinel remains allowed
      only for misses.
- [x] LC-231 Reuse the LightUSD color-management and texture utilities for
      channel semantics, sRGB/data handling, image resizing, and encoding.
      Generated bake assets now record explicit sRGB versus linear/data channel
      semantics and normal-map Y convention; projected color textures now apply
      the shared sRGB transfer policy when a linear source is sampled, while
      data channels remain raw. Lucia texture resizing now uses the shared
      deterministic RGBA filter with linear-light RGB treatment for sRGB assets,
      linear filtering for data/raw assets, and untouched alpha; the authored
      resize color-space policy is recorded on the generated project asset. UV-
      space raster baking now applies the same explicit source and destination
      transfer policy for CPU-readable per-material color textures, while
      scalar/data channels bypass color conversion; direct UV baking preserves
      each material texture's authored color-space tag through the worker
      descriptor and the single-texture fallback path instead of falling back
      to an implicit sRGB default. The dependency-free LightUSD color-space
      matrix utility is now shared by Lucia bake conversion and the LightUSD
      loader color-management path, so AP0/AP1/P3/Rec.2020/Adobe RGB primary
      transforms use one canonical chromaticity-derived implementation. Full
      LightUSD color-
      management reuse for non-sRGB primaries now accepts resolved finite 3x3
      matrix, gamma, and linear-bias metadata in CPU and worker bake paths;
      projected texture sampling applies the same metadata per source material
      with a validated single-texture fallback;
      generated direct and projected bake assets retain the normalized transform
      provenance in bake statistics;
      Embedded RGB matrix-shaper ICC profiles can now be decoded and encoded
      through the same validated bake transform used by CPU and worker paths;
      unsupported LUT, non-RGB, and malformed profiles fail closed; non-uniform
      per-channel TRCs are preserved as explicit gamma triplets. Generated
      profiles round-trip deterministically within fixed-point
      ICC precision. Channel packing now validates the resolution, per-texel
      cardinality, and finiteness of every supplied plane before encoding.
      Resize filtering now applies the matching transfer curve for encoded
      Display-P3, Rec.2020, and Adobe RGB assets, while linear variants and
      raw/data channels remain untransformed.
      Even no-op resizes validate the color-space tag before returning a
      copied image, keeping the allowlisted conversion boundary consistent.
      Deterministic resize likewise rejects non-finite RGBA samples before
      filtering, and normal-map Y conversion validates dimensions, convention,
      and finite samples before channel inversion. Color-space conversion also
      requires exactly three finite RGB components. Lucia bake conversion now
      reuses LightUSD's Display-P3, Rec.2020, ACEScg, and ACES2065-1 primary
      matrices with explicit linear/sRGB/Rec.2020 transfer handling. Adobe RGB
      (1998) now uses the shared D65 chromaticity-derived matrix and gamma-2.2
      transfer in direct and projected bake conversion. Color-space aliases now pass through one
      allowlisted normalizer, and unknown spaces fail closed even when source
      and destination labels happen to match.
      Bake transform normalization also accepts LightUSD resolved metadata
      fields (`sourceToDisplayLinear`, `sourceGamma`, and `sourceLinearBias`)
      without requiring an intermediate adapter object.
- [x] LC-232 Add CPU/native and WASM parity fixtures for barycentric coverage,
      occlusion, normal baking, dilation, and missed-texel accounting.
      The LightRT WASM ABI regression now covers batched occlusion and closest
      hits, and compares closest-hit distance/triangle/(w,u,v) output against a
      deterministic CPU triangle-ray reference; CPU raster coverage/dilation
      and tangent-space normal fixtures are now present; the bake-worker
      protocol is also compared byte-for-byte with the CPU raster reference for
      supersampling, coverage, and dilation. Native/WASM end-to-end occlusion
      integration now exercises the LightRT occlusion bake worker twice and
      verifies deterministic RGBA output, coverage, and missed-texel accounting;
      the LightRT WASM regression also drives the projection worker and verifies
      island ownership, target-face ordinals, and hit/miss sentinel behavior;
      the shared deterministic occlusion raster/ray path now has a CPU reference
      baker, and an isolated Lucia regression compares WASM output byte-for-byte
      with that reference for RGBA pixels, coverage, and missed texels;
      normal-channel worker output now also has a byte-for-byte CPU raster
      parity fixture using the tangent-frame encoder;
      the Lucia test suite now drives the actual LightRT WASM occlusion bake
      path and compares RGBA pixels, coverage, and missed-texel accounting
      against the CPU baker;
      the same parity fixture drives the actual WASM projection worker and
      compares target coverage, source-face ordinals, distances, and
      barycentrics against the CPU closest-hit reference;
      Projection worker result validation also requires typed numeric hit,
      barycentric, pixel, coverage, owner, and optional target buffers.
      a dedicated UV-transfer parity fixture now compares WASM and CPU UVs,
      hit masks, and miss accounting, including canonicalized Uint16 source
      indices; the transfer worker also prioritizes LightRT hit sentinels over
      miss distances when selecting between paired normal rays;
      End-to-end WASM LightRT occlusion texture baking now has a dedicated
      worker parity fixture. A native LightRT feature fixture now covers the
      deterministic center-sample barycentric policy across all covered texels,
      miss accounting, and closest-hit/occlusion batch ABI; full native
      texture encoding integration remains outside this fixture task. That native fixture now
      also rasterizes covered UV samples against a separate blocker surface
      and verifies deterministic batched visibility for every covered texel.

Optional web output may use KTX2/Basis Universal, which supports portable GPU
textures and supercompression. Keep this separate from USDZ profiles because
USDZ has its own allowed image formats and packaging rules. See the
[KTX2 specification](https://github.khronos.org/KTX-Specification/ktxspec.v2.html)
and [USDZ specification](https://openusd.org/dev/spec_usdz.html).

Acceptance criteria:

- No uncovered texel remains inside a valid UV island after dilation.
- Normal baking passes a known tangent-space fixture.
- Cancelling a bake releases GPU resources and does not add project assets.
- Export contains all newly referenced textures under deterministic safe paths.

### LC-240 — Physics Shape Generation

**Goal:** Create efficient, inspectable colliders from render geometry.

OpenUSD supports triangle mesh, simplified mesh, convex decomposition, convex
hull, bounding box, and bounding sphere approximations. Explicit processed
colliders can be authored as sibling guide-purpose geometry. See
[OpenUSD Physics](https://openusd.org/release/api/usd_physics_page_front.html).

Tasks:

- [x] LC-241 Analyze watertightness, connected components, volume, center of mass,
      inertia approximation, and dynamic/static suitability.
      Asset Health now includes a deterministic per-mesh physics analysis with
      topology validity, connected components, signed/absolute volume, center
      of mass, AABB inertia diagonal approximation, and dynamic/static
      suitability flags.
- [x] LC-242 Fit box, sphere, capsule, and cylinder primitives with error metrics.
      Physics analysis now returns deterministic AABB box, enclosing sphere,
      dominant-axis capsule, and dominant-axis cylinder fits with mean/max and
      normalized point-distance errors; collider authoring remains separate.
      Primitive fitting now computes bounds, radii, and error reductions with
      scalar scans, avoiding temporary per-point error arrays for large meshes.
- [x] LC-243 Generate one convex hull.
      Lucia now exposes a deterministic single-hull generator backed by the
      installed Three.js QuickHull implementation, with compact remapped
      vertices and validated triangle indices. Multi-hull decomposition remains
      explicitly deferred.
- [ ] LC-244 Deferred. Do not vendor V-HACD, Manifold, or another general convex
      decomposition library in the first Lucia milestone. Revisit only after
      primitive fitting, one-hull generation, collider authoring, and target
      profile limits have shipped.
- [x] LC-245 Generate a reduced triangle collider for static geometry.
      Lucia reuses validated meshoptimizer reduction with locked boundaries and
      authors the reduced result as a dedicated guide-purpose triangle collider;
      skinned and morph-target meshes are rejected, and the source remains
      undoable and unchanged.
- [x] LC-246 Add target profiles for PhysX, Bullet/Jolt, Web physics, and generic USD.
      Version-1 PhysX, Bullet, Jolt, and generic-USD collider profiles now join
      the quality-gate registry with deterministic triangle, material, memory,
      and watertightness limits; shape generation and engine-specific cooking
      validation now rejects sampled/approximate hull metrics for strict
      PhysX/Bullet/Jolt cooking profiles and suggests authoring an exact guide
      hull.
- [x] LC-247 Author collider siblings with `purpose = "guide"`, collision APIs,
      approximation tokens, and collision groups.
      Single convex-hull generation now authors a dedicated `purpose = "guide"`
      sibling with `PhysicsCollisionAPI` plus `PhysicsMeshCollisionAPI` and
      `physics:approximation = "convexHull"` through a material-free authoring
      path; explicit reduced triangle colliders use approximation `none` so
      their authored geometry is consumed directly. The USD session now also
      provides validated collision-group authoring with
      `CollectionAPI:colliders`, membership includes, and filtered-group
      relationships; UI exposure and strict engine cooking-readiness validation
      are now covered by the inspector, assistant, and target profiles.
      The inspector now exposes deterministic collision-group creation, and
      the assistant accepts validated collision-group requests with optional
      member/filter path arrays, merge-group names, and inverted filtering.
      Additive Ctrl/Cmd tree selection and Shift range selection now supply all
      selected paths to the inspector group command while retaining a primary
      selection for editing.
      The viewport also renders deterministic selection boxes for every
      selected path while preserving the primary box-selection API; assistant
      scene summaries now include the complete selected-path set.
      Assistant-driven prim rename and delete mutations now remap or remove
      selected descendants through the same selection-safe path helpers.
      Strict PhysX/Bullet/Jolt target gates reject sampled hull metrics that
      cannot be safely passed to engine cooking.
- [x] LC-248 Add collider overlay, contact-shape preview, hull count, and volume error.
      Generated `_Collider` and `_TriangleCollider` guide siblings are now
      discovered by the render bridge and can be toggled together in the viewport;
      Asset Health exposes hull-fit
      error and volume per analyzed mesh; generated collider changes report
      source/result counts and reduction error. Asset Health now also reports
      bounded convex-hull vertex/triangle counts and hull-volume error, using
      deterministic approximate sampling for larger meshes. Health now also renders a bounded world-space
      2D contact-shape projection for generated convex-hull or reduced-triangle
      guide siblings, including projection axes and vertex count; clicking or
      keyboard-activating the preview selects the generated guide collider;
      Convex-hull diagnostics now use a deterministic 4096-point bounded sample
      for larger meshes and label the resulting fit as approximate, preserving
      the source/sample counts in Health.

Acceptance criteria:

- Dynamic-body presets never silently use unsupported concave triangle collision;
  PhysX/Bullet/Jolt quality gates now reject authored `_TriangleCollider` guides
  and suggest convex-hull generation.
- Target hull vertex limits are enforced and reported.
- Generated colliders remain aligned after hierarchy transforms.
- Render visibility and collider guide visibility are independent.

### LC-260 — Geometric Part Decomposition

**Goal:** Turn fused or fragmented generated meshes into editable parts without
conflating geometric segmentation with semantic naming.

Tasks:

- [x] LC-261 Split by connected component and material subset.
      Lucia now extracts edge-connected triangle components deterministically,
      preserves material-group ranges, and authors numbered sibling meshes
      while retaining an undoable hidden source. Authored part names now use
      the shared deterministic collision-safe allocator, and component stats
      report the exact names written to USD. Confirmation-gated component
      renaming also resolves those allocator suffixes, so collision-safe
      parts remain editable after authoring.
- [x] LC-262 Identify and optionally remove tiny floating fragments.
      The split operation accepts a minimum face threshold and reports omitted
      component triangles; components below the threshold are not authored.
- [x] LC-263 Merge components separated only by small cracks or duplicated seams.
      `previewCrackMerge` now provides a confirmation-ready, read-only report
      of component counts and merged-component count for a positive weld
      tolerance, together with the validated cleanup result; it does not
      author changes by itself.
      `LuciaOperations.previewCrackMerge` now provides the same report for a
      selected render mesh without mutating the scene.
      `LuciaOperations.mergeCracks` and the confirmation-gated assistant tool
      `scene.merge_cracks` now author the validated weld through the safe mesh
      cleanup path, with a positive finite tolerance requirement.
      The selected-mesh inspector now offers a read-only preview followed by
      confirmation before the crack weld is authored.
- [x] LC-264 Segment using concavity, curvature, and nearly planar boundaries.
      Partial: component extraction and preview now accept a bounded optional
      dihedral split angle, producing deterministic crease-aware components;
      preview components also expose boundary/non-manifold edge counts,
      bounded normal deviation, maximum internal dihedral angle, and a
      deterministic count of curved internal edges, plus a conservative
      nearly-planar marker;
      closed manifold components additionally expose orientation-normalized
      convex/concave edge counts; open or non-manifold components mark
      concavity as unreliable instead of guessing from incomplete topology;
      an opt-in `splitConcavity` mode now separates reliable concave internal
      edges while leaving open and non-manifold meshes conservatively intact;
      degenerate-face normals also disable the concavity classifier;
      boundary components also report closed-loop and scale-aware planar-loop
      counts, giving review tooling explicit evidence for simple planar
      boundary grouping without authoring or filling those loops; component
      previews now also expose deterministic local vertex rings for each planar
      boundary loop and coplanar loop-group membership;
      nearly-planar evidence now requires at least one valid face normal, so
      all-degenerate components cannot be mistaken for planar panels;
      the component inspector exposes the optional crease angle and a
      confirmation-gated reliable-concavity split for sibling/child authoring;
      the same concavity choice is now propagated through correction previews,
      correction authoring, and component-name proposals;
      Concavity and curvature segmentation are now available through explicit
      options; the component inspector also exposes a read-only aggregate
      segmentation-evidence preview for boundary loops, planar loops, curvature,
      and reliable/unreliable concavity. Coplanar planar-boundary loops are now
      grouped deterministically for review; topology-changing boundary grouping
      remains outside this read-only segmentation scope. The same evidence is available through the
      read-only `scene.inspect_component_segmentation` assistant tool.
- [x] LC-265 Detect repeated components and propose USD instances/prototypes.
      Partial: deterministic duplicate-geometry findings now produce separate,
      read-only instancing candidates with source paths and review rationale;
      reports now carry deterministic local-transform and material-ID
      compatibility evidence, and suggestions lower confidence with explicit
      rationale when those differ; automatic replacement remains pending.
      The semantic-suggestion inspector now displays both compatibility
      results alongside each instancing candidate.
      Suggestion generation also ignores malformed mesh inventory entries and
      non-array collections at its public report boundary.
      A confirmation-gated `scene.create_instance` path now authors a new
      typed sibling with `instanceable = true` and an internal prim reference;
      an optional reviewed duplicate path copies only local xform operations
      for placement, while the source and detected duplicate remain unchanged
      for review. Automatic replacement of an existing duplicate and prototype
      consolidation remain beyond this task. The Health semantic-suggestions panel now
      exposes this as an explicit confirmation-gated “Author review instance”
      action, including the source and transform provenance.
- [x] LC-266 Preview component IDs and allow merge/split corrections.
      A read-only `previewConnectedComponents` contract now exposes stable
      per-face component IDs, filtered-face `-1` sentinels, source-face lists,
      material-index summaries, and triangle/vertex counts before authoring.
      `LuciaOperations.previewConnectedComponents` now exposes that contract
      directly for selected render meshes without mutating the USD session.
      `correctComponentPreview` now applies explicit merge-group and discard
      corrections deterministically, remapping labels and preserving source
      face/material provenance for a later confirmed authoring step.
      `LuciaOperations.correctConnectedComponentsPreview` exposes the same
      correction contract for selected render meshes without mutating USD.
      Corrected previews also report deterministic discarded IDs, merged groups,
      and whether the proposed correction changes the segmentation.
      The component authoring operation now accepts the same validated
      merge/discard correction envelope and re-extracts corrected components,
      preserving aligned and face-varying primvars before the existing
      undoable sibling authoring step.
      The selected-mesh inspector accepts `0+2; 1+3` merge groups and
      comma-separated discard IDs, reports the corrected preview, and provides
      a confirmation-gated Apply corrections action for undoable authoring.
      The same correction envelope is available through the confirmation-gated
      `scene.correct_components` assistant tool.
- [x] LC-267 Author stable child prims with transferred materials and primvars.
      Component parts currently clone source material bindings and preserve
      material subset ranges, and now transfer aligned normals, UVs, colors, and
      custom vertex attributes into the generated siblings; child-prim hierarchy
      authoring is now available through `LuciaUsdSession.setMeshGeometryChild`,
      which performs deterministic nested authoring with collision checks and
      preserves the source mesh. Component splitting now exposes an explicit
      `asChildren` mode in the operation, assistant, and inspector so callers
      choose sibling versus child hierarchy deliberately.
- [x] LC-268 Add optional assistant-proposed semantic names after deterministic
      segmentation, requiring user confirmation.
      `proposeComponentNames` now generates deterministic, collision-safe
      component labels with source-face provenance; proposals are explicitly
      marked authoring-required and never rename prims automatically.
      The assistant now exposes a read-only `scene.propose_component_names`
      tool for these deterministic labels; it returns authoring-required
      proposals and never renames prims without a separate confirmed action.
      The confirmation-gated `scene.rename_component_parts` tool now applies
      reviewed ID-to-name maps to generated sibling or child parts with safe
      identifier, missing-part, and collision checks in one undoable batch.
      Proposals now reserve names already present in the USD tree before
      generating deterministic labels, including nested prim names.
      Component name proposals also carry bounded boundary, non-manifold,
      normal-deviation, and nearly-planar evidence for review; they remain
      conservative and do not infer labels from geometry automatically.
      The Operations inspector also exposes these read-only proposals using
      the active segmentation and minimum-face settings.
      Role-aware component naming now reuses the deterministic source-name role
      tokens and raises confidence only for that explicit evidence; generated
      names remain numbered, read-only proposals requiring confirmation.
      Conservative geometry-only naming now recognizes nearly planar parts with
      a planar boundary loop as lower-confidence `Panel` proposals. Optional
      image/model proposals use LC-321's explicitly approved, validated,
      review-only inference boundary; provider invocation and broader
      geometry-only semantic naming remain outside this deterministic path.

Acceptance criteria:

- Component extraction preserves the rendered surface and material assignment.
- Repeated-part proposals include a geometric tolerance and visual comparison.
- AI naming cannot change topology or apply without confirmation.

### LC-280 — USD Structure and Packaging Doctor

**Goal:** Make the final scene composable, portable, and predictable outside
Lucia Code.

Implementation note: the current Lucia slice includes a deterministic USDA
metadata/dependency audit, composition graph summary, downloadable dependency
manifest, and confirmation-gated repair for defaultPrim, upAxis, and
metersPerUnit. Resolver-aware composition arcs, localization, USDZ compliance,
and packaging repair remain below.

Tasks:

- [x] LC-281 Add composition/dependency graph inspection.
      Partial: USD Doctor now emits sorted root-layer dependency nodes/edges
      and disconnected package-asset nodes for deterministic orphan review;
      nested USDA references now resolve relative to their layer directory;
      recursive composition cycles are reported as deterministic errors;
      dependency cycle traversal uses an explicit stack so deeply nested layer
      chains cannot overflow the browser call stack;
      the Doctor graph now renders each edge with its computed dependency
      status instead of presenting resolver or missing targets as healthy, and
      graph nodes and serialized edges carry that same status for manifest
      consumers;
      resolver-aware non-file arcs and stronger composition semantics remain.
      The Doctor API also normalizes plain object package maps into the same
      deterministic asset inventory contract as Map-backed packages.
- [x] LC-282 Detect missing references, payloads, sublayers, textures, and resolver-
      dependent paths.
      Partial: USD Doctor classifies discovered references/payloads/sublayers,
      recursively checks available USDA layers, and now refuses to satisfy
      dependency presence through absolute or parent-traversal paths. Each
      dependency now reports a stable `available`, `missing`, `unsafe`, or
      `anonymous` status in addition to the compatibility `present` boolean;
      the same status is serialized in the downloadable dependency manifest;
      Doctor also emits a fixed-key status-count summary, including zero-valued
      statuses, for stable UI and manifest serialization; resolver plug-in
      semantics remain pending. Manifest serialization normalizes unknown
      dependency status tokens back to the documented enum. Malformed or absent package collections now
      produce an empty diagnostic inventory instead of aborting analysis;
      serialized status counts are recomputed from those normalized dependency
      records rather than trusting supplied totals. Map-backed package
      inventories also discard non-string member keys at the API boundary and
      tolerate malformed byte payloads while reporting empty members. Resolver
      dependencies now record the required `usd` or `texture` capability when
      inferable, warn on registered plugins that do not advertise it, and
      preserve the capability result in dependency manifests. The Doctor panel
      displays registered, unregistered, and capability-mismatch states with
      the required capability.
- [x] LC-283 Repair `defaultPrim`, up-axis, units, kind, purpose, and material bindings.
      Safe Doctor repair now also normalizes invalid authored kind/purpose
      tokens to allowlisted model/default values; its public repair boundary
      allowlists metadata keys and validates identifiers, axes, and positive
      finite units before authoring. Missing USDA layer metadata blocks are
      created only when at least one valid stage repair is requested. Confirmation-
      gated material binding operations now validate explicit absolute
      old→canonical maps and rewrite only exact scalar `material:binding` or
      `material:binding:collection[:name]` targets without deleting definitions;
      collection membership itself is never changed. Same-layer inherits/
      specializes arcs with exactly one direct source binding now produce
      read-only inferred repair candidates; external composition, layer
      strength, and collection membership still require explicit review.
      The Doctor material review panel displays the inferred material target
      next to the source arc for that review.
      Doctor reports deterministic, explicitly non-automatic review candidates
      for collection and inherits/specializes material bindings so repair UI
      can require composed-stage confirmation.
      Review candidates now carry the authored prim path instead of collapsing
      all collection/inheritance findings to the layer root.
      Their aggregate Doctor issues use the same precise path when there is a
      single authored candidate.
      USD Doctor comment/string sanitization is quote-aware, preserving hash
      characters in authored string values during metadata scans.
      Exact material binding rewrites now use the same quote/comment-aware
      boundary and cannot rewrite matching text inside USDA strings or comments.
      metersPerUnit diagnostics are likewise scoped to the layer metadata block,
      so authored prim properties cannot satisfy or invalidate stage units.
      Kind and purpose repair is likewise scoped away from the layer metadata
      block, preventing nonstandard stage-level text from being rewritten as
      if it were prim metadata.
      Kind and purpose diagnostics use the same scope, so nonstandard values
      in the layer block do not create false prim-metadata issues or repair
      candidates.
      USD Doctor’s explicit, collection, and inherited-binding diagnostics use
      the same structural scan, so quoted examples and comments do not suppress
      unused-material findings or create review candidates.
      Material definition discovery also uses the structural scan, preventing
      declaration-shaped strings and comments from creating phantom materials.
      Explicit dependency localization also ignores `@asset@` examples inside
      USDA strings and comments while preserving mapped authored references.
      Metadata diagnostics likewise extract only structural quoted assignments,
      preventing embedded examples from being reported as invalid stage values.
      Root-prim discovery accepts valid parenthesized declaration metadata, so
      `defaultPrim` validation does not misclassify metadata-bearing roots.
      Layer-level `defaultPrim` and `upAxis` reads are scoped to the USDA layer
      metadata block, excluding same-named properties authored on prims.
      Metadata repair uses the same structural delimiter scan, preserving
      parentheses inside quoted metadata values.
      Prim-level kind/purpose repair only normalizes assignments already authored
      on a prim; it never inserts those fields into the layer metadata block.
      The USD Doctor panel displays those review candidates and explicitly
      withholds automatic repair controls.
      Confirmation-gated `scene.repair_inherited_material_bindings` now accepts
      an explicit composed-stage review result and authors a direct binding
      override only when the target prim has no conflicting authored binding;
      composition inference remains outside the automatic Doctor boundary.
      Dependency manifests preserve the same review-only records for downstream
      audit consumers.
      Manifest serialization normalizes control characters in those labels and
      reasons so hand-edited review records remain safe to export.
- [x] LC-284 Detect unused materials, orphaned assets, inactive prims, and duplicate
      geometry suitable for instancing.
      Partial: USD Doctor now inventories explicit USDA Material definitions and
      reports materials without explicit bindings and counts authored inactive
      prims, and detects duplicate local geometry with an instancing
      candidate path using compact fingerprints plus canonical equality checks.
      It also identifies binding targets not defined in the
      current layer without assuming they are invalid; collection material
      bindings are now reported explicitly and suppress conservative
      unused-material warnings until composed collection membership can be
      evaluated; inherits/specializes arcs receive the same conservative
      treatment. Inherited/collection binding repair and automatic instancing
      authoring remain pending. Mesh candidates use deterministic
      position/topology canonicalization when bounded, with an exact-array
      fallback for larger meshes.
      USD Doctor now exposes deterministic `orphanMaterials` and
      `orphanAssets` candidate arrays in its read-only report and dependency
      manifest, and the inspector surfaces them without offering deletion or
      repair actions. Orphan material reporting remains suppressed whenever
      collection or inherited binding semantics could account for usage.
      Doctor also exposes deterministic `inactivePrimPaths` for targeted review;
      inactive prims remain authored state and are not reactivated automatically.
- [x] LC-285 Add flatten-selected and extract-to-reference operations.
      Partial: confirmation-gated extraction now moves a selected prim subtree
      into a generated package-relative USDA layer and replaces it with an
      exact same-path reference; the generated layer and USD edit share one
      undoable command, with safe path and collision checks; generated layers
      author a matching defaultPrim for reference portability. Package-local USDA
      references can also be flattened back into the working layer while
      retaining the package asset; general composition flattening, nested
      external-layer handling, and richer layer metadata preservation remain
      pending.
- [x] LC-286 Localize dependencies and rewrite package-relative paths.
      Partial: USD Doctor now exposes confirmation-ready explicit path
      localization that rewrites only exact `@asset@` references, rejects
      absolute/resolver/parent-traversal destinations, and detects destination
      collisions. Loaded project assets are moved atomically and export remaps
      are recorded, including chained moves; automatic resolver localization
      and package-member naming policy remain pending. Null/non-object
      localization maps produce a deterministic no-op at the public boundary.
- [x] LC-287 Validate USDZ allowed file types, zero-compression ZIP constraints,
      64-byte data alignment, path safety, and missing package members.
      USD Doctor now detects normalized package-member path collisions, unsafe
      paths, empty members, and unsupported package-member extensions; ZIP
      compression, encryption, central-directory integrity, and 64-byte
      alignment are now validated on every USDZ export and before imported USDZ
      bytes are handed to the native loader; local and central-directory member
      names, compression flags/methods, and uncompressed payload CRCs must also
      agree.
- [x] LC-288 Generate a deterministic dependency manifest and export report.
      USD Doctor exposes a sorted JSON dependency manifest for download,
      including metadata, the complete recursively discovered dependency set,
      package members, dependency graph, issue count, and score. Manifest
      serialization ignores malformed report collections and dependency nodes
      so a hand-edited report cannot crash export; manifest source names are
      reduced to basenames so local filesystem paths are not exported.
      Assistant activity paths are likewise restricted to valid USD prim paths.
      Exported scores are clamped to the documented 0–100 range.
      Manifests also preserve sanitized orphan-material, orphan-asset, and
      inactive-prim candidate paths when present, without turning them into
      executable cleanup actions.
- [x] LC-289 Add target checks for portable USDZ and stricter Apple/web profiles.
      Implemented: Apple AR and portable USDZ profiles now provide stricter,
      versioned triangle, material, texture, GPU-memory, and UV checks; portable
      USDZ additionally incorporates unresolved-dependency and USD Doctor error
      checks, including unsafe and anonymous dependencies that are not package
      members, and USDZ export always applies the portable gate even when the
      user selected a non-blocking viewer profile. An explicit
      `external-reference` profile preserves intentional external references;
      ZIP package-layout checks remain in USD Doctor. Portable gates now fail
      closed when no USD Doctor report is available and require a valid
      `defaultPrim` plus Y/Z `upAxis` metadata for mobile/Apple/portable
      profiles. USDZ export also validates the generated archive itself before
      download, in addition to validating imported archives.

The official OpenUSD toolset treats recursive asset localization and compliance
checking as part of package creation. USDZ additionally requires uncompressed,
unencrypted ZIP storage and 64-byte-aligned file data. See the
[OpenUSD toolset](https://openusd.org/release/toolset.html) and
[USDZ specification](https://openusd.org/dev/spec_usdz.html).

Acceptance criteria:

- Export refuses unresolved required assets unless the user explicitly chooses an
  external-reference profile.
- Package paths cannot escape the archive or collide after normalization.
- Reopening an exported package produces the same dependency manifest.
- Portable-profile fixtures pass Lucia validation and an available `usdchecker`.

## 7. Phase 3: intelligent workflow assistance

### LC-300 — Target Profiles and Repair Plans

- [x] LC-301 Define versioned constraints for mobile AR, web product viewer,
      desktop real-time, physics simulation, archival USD, and 3D printing.
      Partial: version-1 mobile AR, web viewer, desktop real-time, physics,
      archival USD, and 3D-printing profiles now evaluate deterministic triangle,
      material, GPU-memory, texture-dimension, and UV budgets where applicable;
      Apple AR and portable USDZ profiles add stricter metadata/dependency checks,
      while 3D printing additionally requires zero boundary and non-manifold edges;
      PhysX/Bullet/Jolt/generic-USD profiles now enforce declared convex-hull
      vertex budgets when hull metrics are available. Physical-scale profiles now
      require a positive authored `metersPerUnit` and now fail closed when the
      USD Doctor report is unavailable; wall-thickness and archival provenance
      checks remain pending.
      All profiles now include a deterministic texture-validity check for
      malformed image dimensions and incomplete RGBA buffers; the result is
      included in the serialized quality-gate checks. Physics profiles also
      fail closed when a present convex-hull vertex metric is negative,
      fractional, or non-finite. Portable USDZ checks likewise treat malformed
      Doctor issue/dependency collections as blocking instead of throwing or
      treating them as an empty healthy report.
      The quality-gate evaluator independently validates supplied texture
      descriptors and rejects malformed collections even when a caller omits
      the corresponding issue list. All profiles now also fail the gate for
      non-finite or numerically extreme mesh transforms, with affected mesh
      paths retained for review.
      Texture-dimension aggregation now scans inventories directly instead of
      spreading a mapped dimension array, keeping quality-gate evaluation safe
      for large texture sets.
      Material budget checks also fail closed when a materials array contains
      null or primitive entries.
      Texture budget and validity checks likewise fail closed for null or
      primitive texture entries.
      Malformed numeric report metrics now use a finite fail-closed sentinel,
      keeping generated quality gates schema-valid and serializable while they
      still fail the affected checks.
      the corresponding AssetReport issue entries.
      Serialized quality gates now recompute their top-level pass invariant
      instead of trusting untrusted manifest flags, and validation now requires
      the complete, ordered profile-specific check set so omitted constraints
      cannot make a forged gate appear valid. Live profile evaluation now fails
      closed for present non-finite or negative numeric report metrics instead
      of coercing malformed values to zero, and rejects a malformed supplied
      stage object rather than treating it as an empty scene.
      An omitted or null stage report now also fails closed instead of being
      interpreted as a healthy empty scene.
      Mesh aggregate counts now also require matching mesh inventory entries;
      omitted inventories cannot satisfy UV or transform checks from forged
      stage totals alone.
      Triangle and mesh totals are additionally required to be non-negative
      integers; fractional count metrics now fail closed.
      When a stage material count is authored, detailed material inventories
      must be present and cardinality-consistent before material budgets pass.
      UV-required profiles also cross-check each detailed mesh's explicit UV
      flag against the aggregate `uvMeshes` count.
      Malformed mesh inventory collections and entries now also fail transform
      and required-UV checks closed with a stable affected-path marker instead of throwing while
      collecting diagnostics; annotation output filters malformed affected-path
      values so serialized quality gates retain a string-only path contract.
      Quality-gate validation also requires affected paths and suggested fixes
      to be sorted, unique string arrays.
      Validation preserves detailed UV failures whose pass state includes
      per-mesh evidence in addition to the aggregate coverage values.
- [x] LC-302 Generate a deterministic proposed operation graph from an AssetReport.
      Partial: `proposeRepairPlan` groups supported fixes by prim, filters
      unsupported actions, emits stable dependency ordering, and the new
      `buildRepairOperationGraph` helper exposes deterministic operation and
      issue-provenance nodes plus dependency/trigger edges for Health
      visualization. Operation graph nodes now carry the planner's benefit,
      cost, estimated work, and destructive/undoable metadata, so graph
      consumers can render a complete proposal without joining plan records;
      the Health dependency-graph view now renders that operation metadata on
      each edge destination. Planner and graph-builder inputs now fail closed
      for malformed report, issue, and operation collections; cached issue
      provenance is also treated as absent rather than throwing during
      no-op-plan checks. External operation graphs deduplicate operation IDs
      and repeated trigger/dependency edges before deterministic sorting.
      Planner issue provenance now retains only non-empty string rule IDs,
      keeping generated recipe metadata serializable when a supported issue is
      malformed. No-op checks also reject operations whose fix or prim path is
      not a non-empty string. Planner and operation-graph inputs also reject
      syntactically invalid USD prim paths before they become operation IDs or
      graph nodes. Mesh-size work estimates also fail closed for negative or
      fractional counts and saturate at `Number.MAX_SAFE_INTEGER`, preventing
      malformed reports from producing misleading planner costs.
- [x] LC-303 Show expected benefits, destructive steps, dependencies, and estimated
      cost before execution.
      Partial: the Health inspector displays the generated plan with benefit,
      qualitative cost, deterministic mesh-size-based work units, dependency,
      and undoable/destructive annotations; execution remains
      confirmation-gated through the existing individual actions. Multi-step
      Health repairs and loaded recipes now share a preflight summary that
      aggregates benefits, cost levels, estimated work, dependency count, and
      destructive/undoable steps before execution.
- [x] LC-304 Allow users to disable/reorder compatible operations.
      The Health inspector now exposes per-step enable/disable controls and
      deterministic move-up/move-down ordering; dependency-invalid edits are
      rejected, and downloaded recipes preserve the edited plan.
- [x] LC-305 Re-run analysis after each operation and skip resolved/no-op steps.
      Recipe execution re-analyzes before each step and skips analyzer-generated
      operations whose triggering issues are resolved; each remaining operation
      uses the existing undoable mutation path.
- [x] LC-306 Save and reload processing recipes without embedding private paths.
      Health downloads and reloads version-1 deterministic recipes containing
      only safe profile data, operation IDs/prim paths, options, provenance, and
      edited ordering; parser compatibility and path-safety checks are enforced.
      Recipe creation and editing tolerate malformed top-level and collection
      fields without
      producing non-serializable dependencies or provenance; explicit private
      and traversal path checks remain enforced. Serialized recipe parsing now
      rejects malformed operation collection types instead of silently
      normalizing them; dependency and issue-provenance arrays also require
      non-empty string members.

### LC-320 — Semantic Scene Cleanup

- [x] LC-321 Suggest prim names and hierarchy from component geometry and optional
      user-approved image/model inference.
      Implemented: Health now shows deterministic, read-only role suggestions from
      stable mesh-name tokens (for example wheel, door, handle, fastener, and
      ground-contact), and component-name proposals now include an explicit
      child-parent hierarchy recommendation with rationale. Component
      proposals reserve existing names deterministically, terminate safely
      through repeated collisions, and fall back from malformed component IDs
      to valid index-based suffixes. The same
      deterministic suggestions are available through the read-only
      `scene.inspect_semantic_suggestions` assistant tool.
      An optional inference boundary accepts image/model suggestions only when
      the caller explicitly supplies `approvedInference: true`; paths, roles,
      labels, confidence, rationale, hierarchy, and proposed names are validated,
      inference suggestions remain review-only, and malformed or duplicate
      candidates are discarded. No model invocation or USD authoring is implicit.
      Health grouping candidates now carry the same explicit child-parent
      hierarchy recommendation as component-name proposals, while remaining
      read-only and confirmation-gated.
      The Health inspector renders that recommended parent path alongside the
      component count so the proposed hierarchy is visible before authoring.
      Stable semantic-role suggestions now expose and render the same
      confirmation-gated child-parent recommendation.
      Pivot and orientation hints require finite three-component bounds vectors;
      malformed report geometry is ignored rather than producing invalid
      semantic recommendations.
      Component naming proposals normalize malformed hierarchy sources to the
      layer root and accept set-backed reserved-name collections consistently.
- [x] LC-322 Detect likely functional relationships such as wheels, doors, handles,
      repeated fasteners, and ground-contact regions.
      Partial: conservative role-token detection reports likely wheel, door,
      handle, lighting, fastener, glazing, chassis, seat, and ground-contact
      relationships with deterministic confidence and rationale; common plural
      tokens such as wheels, doors, handles, and headlights are recognized too.
- [x] LC-323 Suggest pivots, orientation, and grouping with confidence scores.
      Partial: deterministic geometry hints now report off-center pivot,
      dominant-axis orientation, and disconnected-component grouping candidates
      with confidence and rationale; authoring remains confirmation-gated and
      image/model inference remains out of scope.
- [x] LC-324 Keep semantic suggestions separate from deterministic geometry results.
      Semantic suggestions use a separate read-only module and Health
      section; they are never included in repair plans or authored automatically.
- [x] LC-325 Record assistant proposals and user decisions in activity history.
      Partial: assistant tool calls now record proposal, declined, executed, and
      rejected outcomes in the project activity stream without marking the
      scene dirty; only validated USD prim paths are surfaced as affected paths,
      so dependency maps and private asset names are not copied into activity
      metadata. Downloaded dependency manifests now persist the sanitized
      ordered decision records, and a tolerant read-back helper can recover
      only valid USD prim paths and known decisions; LuciaProject can restore
      those records as non-dirty audit history. In-memory recording and
      read-back apply the same decision and prim-path contract before entries
      reach export. Valid transform-provenance paths are also retained for
      confirmed instance authoring decisions.
      USD Doctor exposes that
      activity-only import in the UI; full project-state import and UI replay
      remain pending.

### LC-340 — Rig Preparation Diagnostics

- [x] LC-341 Inspect skeleton hierarchy, joint transforms, bind transforms, and skin
      weight normalization.
      Partial: AssetReport now performs a read-only typed-array skin audit and
      checks optional skeleton bone matrices for finite, invertible affine bases
      (including determinant-based singular-transform rejection),
      rejects present bone matrices with malformed cardinality,
      inverse-bind matrix count/invertibility, duplicate names, foreign parents,
      hierarchy cycles, and disconnected roots;
      the Health inspector now exposes per-skinned-mesh bone/root/weight status;
      a deterministic `bone.matrixWorld × inverseBind` identity residual is now
      reported for bind-pose consistency; Health also reports bounded mean/max
      current vertex displacement from bind-space using packed skeleton
      matrices, with malformed joint indices and negative weights rejected;
      rig diagnostics now use the shared bounded typed-array validator, so
      finite-value checks do not duplicate large skin or matrix buffers;
      richer animated pose comparison remains pending.
- [x] LC-342 Detect unweighted vertices, excessive influences, invalid joint indices,
      non-invertible transforms, and detached geometry.
      Partial: the audit reports unweighted vertices, weight sums, excessive
      influences, invalid joints, non-finite values, and non-invertible bones,
      with bounded deterministic per-vertex issue samples surfaced in Health;
      Health can select a mesh and highlight those sampled vertices in the
      viewport without authoring changes, and the same action toggles those
      highlights off without disturbing other overlays;
      it also reports skin-weighted meshes that have no attached skeleton with
      bones; bind-matrix consistency is now surfaced through the bind-pose
      residual audit and reported as an informational Health issue, while
      deeper detached-geometry analysis remains pending; all large input
      buffer checks are bounded before any derived report data is allocated.
- [x] LC-343 Transfer skin weights to a reduced mesh with visual error reporting.
      Partial: a confirmation-gated operation transfers normalized nearest-source
      weights to an unskinned target and records mean/max positional transfer
      distance and a per-target distance buffer used for a post-authoring
      viewport error heatmap; source weights/topology are rejected when malformed or negative,
      and valid source triangles use barycentric interpolation in a dedicated
      transferable worker; worker results now pass strict cardinality, finite
      non-negative weight, joint-range, normalized-sum, and distance-statistic
      validation before USD authoring; visual deformation comparison remains
      pending.
- [x] LC-344 Add bind-pose and influence heatmap previews.
      Partial: the Health inspector now renders a bounded, deterministic
      dominant-joint influence heatmap per skinned mesh, projected onto the
      two smallest spatial axes and alpha-weighted by the dominant influence;
      bind-pose analysis now also exposes up to 256 deterministically ordered
      per-bone residual samples for localized diagnostics, and Health renders the
      top 16 samples as a bounded residual bar preview; bind-pose/deformation
      comparison remains pending.

Automatic rig generation and animation authoring remain out of scope until these
diagnostics and transfer operations are reliable.

## 8. Cross-cutting UX

### Comparison modes

- [x] Before/after wipe
      Mesh operations capture bounded before/after geometry and expose a
      deterministic clipping-plane wipe control in the viewport.
- [x] Ghost overlay
      Comparison captures now expose a non-destructive ghost mode that draws
      translucent before/after geometry together; it can be toggled alongside
      wipe and error-heatmap modes and is cleared with the comparison state.
- [x] Surface-distance heatmap
      Geometry comparison samples now expose nearest-surface distance and
      normalized error as an explicit viewport heatmap mode, alongside wipe
      and ghost comparison views.
- [x] Normal-deviation heatmap
      Frame comparisons now retain bounded per-sample normal angular deltas
      and expose a non-destructive viewport heatmap with a deterministic
      blue-to-red deviation scale.
- [x] Wireframe density
      The selected mesh exposes a disposable, depth-independent wireframe
      density overlay in the Operations inspector; toggling it never authors
      geometry or changes USD state.
- [x] UV stretch and overlap
      The UV inspector renders bounded coarse overlap and distortion grids,
      island borders, and a textual legend without changing authored UV data.
- [x] Collider overlay
      Generated guide and triangle-collider meshes can be toggled as a
      non-destructive viewport overlay.
- [x] Material difference
      Before/after material signatures are captured around mutations and
      changed triangles receive a bounded magenta viewport overlay; rendered
      vertex colors are included alongside material fields, and the WASM
      binding exports USD displayColor data as stable Float32 RGB attributes;
      the mode is disposable and does not alter authored geometry or materials.
      Browser smoke coverage now applies a display-color change through the
      Material inspector and verifies that the material-difference control is
      exposed after the confirmed mutation.
- [x] LOD distance preview
      Authored `_LOD1+` siblings are selected deterministically from camera
      distance and normalized source bounds; the preview toggle never changes
      authored visibility or USD data.

### Export quality gates

Users should be able to save constraints such as:

```text
boundary edges = 0
missing dependencies = 0
triangles <= 100000
materials <= 8
texture dimension <= 4096
maximum reduction error <= 0.5%
```

- [x] LC-400 Create a versioned quality-gate schema.
      Target-profile results now use an explicit version-1 quality-gate
      envelope with a constructor and structural validator for persisted and
      export-manifest data; malformed or future-version gates are rejected.
      The validator also rejects duplicate check IDs so serialized gate results
      remain deterministic.
- [x] LC-401 Evaluate gates continuously from `AssetReport`.
      Health rebuilds the cached AssetReport only when scene/assets revisions
      change, then evaluates the selected versioned target profile on every
      report render; USD dependencies are independently revision-aware.
- [x] LC-402 Link every failed gate to affected prims and suggested fixes.
      Failed profile checks include deterministic affected paths and suggested
      fixes in the Health panel and export manifest; selection jumps to the first
      affected mesh, and confirmation-gated UV unwrap, retopology, and texture
      resize actions are offered when their processors/assets are available.
- [x] LC-403 Prevent export only for gates configured as blocking.
      Health exposes blocking versus advisory mode; USDA export is blocked for a
      failed blocking gate, while USDZ always enforces its selected portability
      gate before writing the package.
- [x] LC-404 Include results in the export manifest.
      The USD Doctor manifest includes the selected target-profile gate,
      individual checks, and its blocking/advisory mode.

### Accessibility and large-scene behavior

- [x] LC-410 Make reports, progress, previews, and issue actions keyboard accessible.
      Scene-tree rows now support keyboard selection and Up/Down/Home/End
      traversal, inspector and activity tabs expose tab semantics with arrow/Home/End traversal, and progress exposes
      an ARIA progressbar; reduced-motion and forced-colors modes preserve feedback;
      confirmation dialogs restore focus to their invoking control after
      confirm/cancel; panel toggles expose and maintain `aria-expanded` state
      and return focus to the toggle when collapsing a panel.
- [x] LC-411 Never encode issue severity or heatmap meaning by color alone.
      Severity is rendered as explicit ERROR/WARNING/INFO text, and the UV
      preview includes a textual legend for overlap, density, and island
      borders; frame previews likewise name their normal/tangent colors.
- [x] LC-412 Virtualize large issue lists and scene trees.
      Health reports generate only the current 200-issue window with
      deterministic previous/next paging. The scene tree flattens filtered
      hierarchy entries into a bounded 100-row viewport window with overscan
      and spacer rows; keyboard navigation scrolls to off-window entries while
      selection and visibility actions retain their original paths.
- [x] LC-413 Cap preview memory and provide a clear degradation path for large assets.
      Preview budgets cap renderer pixel ratio, aggregate texture bytes,
      texture dimensions, and UV/geometry preview sampling; Lucia reports each
      degradation while leaving authored assets unchanged.
      Partial: bake workers now estimate raster/ray/mesh working buffers and reject
      requests above a conservative 256 MiB budget with resolution/sample guidance;
      UV preview rendering now caps its deterministic line-work budget at 100,000
      triangles for normal scenes, reducing it to 75,000/50,000 for dense/huge
      scene tiers, and labels sampled output; the viewport now derives a deterministic
      renderer pixel-ratio/texture budget and downscales drawable preview textures
      into private canvases while leaving authored assets unchanged; the preview
      now enforces both per-texture dimensions and a total texture-byte budget.
      Byte-backed RGBA8 data textures use the same preview-only path, while other
      data formats are explicitly counted and announced as skipped; dense-scene
      geometry now receives a deterministic triangle-aligned preview draw-range
      cap while authored buffers remain unchanged; the degradation message also
      reports the number of omitted preview triangles.
- [x] LC-414 Preserve responsive inspector and report access on narrow screens.
      Partial: narrow layouts now keep inspector panels within the viewport,
      wrap long report paths/messages, compact field grids and controls below
      420px, wrap report action buttons and grid values without forcing width,
      preserve independent report scrolling, and align the mobile inspector
      bottom edge with the reduced activity-panel height and safe-area inset;
      viewport comparison controls now wrap within the viewport at 640px and
      reduce their range-control width below 420px; inspector, chat, and
      activity scroll regions now contain overscroll so touch/trackpad gestures
      do not move an underlying viewport while a report is being reviewed;
      the browser smoke test also asserts no horizontal document overflow and
      a fitting inspector at 390×844 and 320×568, with independent report
      scrolling at the handset size; device-level visual QA and broader
      responsive review remain.

## 9. Technical workstreams

### Native and WASM geometry kernel

- Introduce an owned indexed-mesh interchange type with positions, indices,
  normals, UVs, colors, skinning data, subsets, and arbitrary aligned primvars.
- Add checked Stage/Prim-to-mesh extraction and mesh-to-USD authoring APIs.
- Avoid USDA text rewriting for topology and typed attribute mutation.
- Compile processors as individually testable native code and a worker-oriented
  WASM module.
- Define memory ceilings and overflow checks before accepting untrusted counts.

### Session-layer editing

- Move from whole-stage USDA snapshots toward typed, validated session-layer
  deltas.
- Keep one logical user action atomic across prim edits and generated assets.
- Store large generated binary assets outside the command snapshot and reference
  them through content-addressed project storage.
- Preserve the current bounded history policy and report when history is pruned.

### Testing

Each processor needs:

- Native unit tests for algorithm and malformed-input behavior.
- Native/WASM parity fixtures.
- Determinism tests.
- Cancellation and memory-limit tests.
- USD round-trip tests including indexed and face-varying primvars.
- Browser smoke coverage for analyze, preview, apply, undo, and export.
- Visual golden tests only where numeric metrics cannot establish correctness.

Do not add generated build products, proprietary assets, or large binary fixtures.
Prefer procedural fixtures or deliberately small files in approved test locations.

The standard web regression gate includes `lucia-code/tests/core.test.mjs` so
Lucia processor, validation, authoring, and deterministic fixture coverage runs
alongside the LightRT and xatlas WASM ABI checks.

## 10. Dependency evaluation checklist

Before adding meshoptimizer, xatlas, MikkTSpace, V-HACD, Manifold, or another
geometry library:

- [x] Confirm license compatibility and required attribution.
      xatlas is MIT-licensed; the vendored directory retains its upstream
      license and records the source snapshot and adapter-only modifications.
- [x] Pin an audited version or commit.
      Lucia pins commit `f700c7790aaa030e794b52ba7791a05c085faf0c`.
- [x] Record upstream source and local modifications.
      `README.lightusd.md` records the upstream URL, snapshot date, excluded
      upstream components, and the LightUSD adapter boundary.
- [x] Verify exception-free C++17 compatibility or isolate required adaptations.
      The isolated xatlas core contains no C++ exception path, does not depend
      on Eigen, and is built through the native/WASM adapter boundary.
- [x] Verify deterministic output across supported architectures.
      Option normalization and the xatlas WASM mapping fixture validate stable
      atlas/remap output; broader architecture matrix coverage remains part of
      the release regression gate.
- [x] Measure native and WASM binary-size impact.
      A controlled combined WASM A/B build is now reproducible with
      `-DLIGHTUSD_WITH_XATLAS=ON|OFF`: the current ON output is 7,058,230
      bytes and the OFF output is 6,971,581 bytes, a delta of 86,649 bytes
      (1.24% over the no-xatlas baseline). The vendored xatlas object is
      154,468 bytes in the current WASM build. A native product target does
      not currently link xatlas, so no native product delta is applicable;
      the reproducible standalone measurement
      `measure-xatlas-native-size.sh` reports 179,580 bytes at `-O2` and
      129,934 bytes at `-Os` with the pinned exception-free flags. This
      object-only figure is recorded as attribution, not as a claim that the
      native LightUSD product currently ships xatlas.
- [x] Fuzz public parsers and geometry entry points.
      A deterministic Node fuzz-smoke fixture exercises malformed USDA text,
      USDZ byte buffers, and the public indexed cleanup, normalization, normal,
      and tangent entry points. Parser/validation failures are expected and
      accepted, while every rejection must remain a proper Error object; the
      bounded deterministic corpus runs in the standard Lucia test glob.
- [x] Confirm bounded allocations and count/stride overflow checks.
      The adapter bounds numeric options and validates finite, vertex-aligned,
      complete index buffers before transfer and validates all returned atlas
      buffers before USD authoring.
- [x] Build a small native/WASM parity fixture before product integration.
      `web/js/tests/xatlas-wasm.mjs` exercises the vendored xatlas WASM mapping
      and is included in the web regression profile.

Current UV dependency decision:

- xatlas: selected for now; MIT, dependency-free, and already suitable for
  native/WASM builds. The exact upstream commit is recorded in
  `src/external/xatlas/README.lightusd.md`.
- OpenABF: rejected because it requires Eigen and does not provide the complete
  charting/packing boundary Lucia needs.
- Microsoft UVAtlas: not selected; its repository is archived and its upstream
  README recommends against using it for new projects.
- libigl/CGAL: not selected for the browser core because their broader geometry
  stacks and Eigen/third-party requirements are disproportionate to this
  focused module.

## 11. Recommended delivery sequence

Implement vertical slices rather than completing all backend algorithms before
building the workflow:

1. `AssetReport` plus geometry issue UI.
2. Safe cleanup through analyze, preview, apply, undo, and re-analysis.
3. meshoptimizer reduction using the same lifecycle.
4. xatlas-based UV generation, deterministic packing, and checker preview.
5. normals/tangents and a complete mesh-conditioning preset.
6. USD dependency doctor and portable USDZ gate.
7. material audit and deterministic graph simplification.
8. LightRT/BVH UV-space texture baking.
9. primitive and one-hull collider generation; convex decomposition remains
   deferred.
10. part segmentation, instancing, and assistant-proposed semantics.

The first public milestone should complete steps 1–6. It would provide a coherent
local workflow for diagnosing, repairing, reducing, unwrapping, validating, and
packaging generated assets without depending on remote inference.

## 12. Definition of done for a new module

A module is complete only when:

- Analysis and limits are documented.
- Preview does not mutate the working stage.
- Apply is atomic and undoable.
- Cancellation leaves the stage and project assets unchanged.
- Before/after metrics are visible and saved to activity history.
- Typed USD data round-trips without primvar misalignment.
- Native/WASM parity, malformed-input, and browser workflow tests pass.
- Export validation catches missing or invalid generated dependencies.
- User-facing descriptions explain tradeoffs without promising lossless results.
- Third-party notices and version information are present where required.
