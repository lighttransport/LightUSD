# Composition in USD: LIVRPS, Instancing, and Variants

How lightusd composes a USD scene — LIVRPS arc ordering (compared against
OpenUSD PCP), instancing, and variants. For the DAG-engine API, see
[pcp.md](pcp.md).

## Overview

Composition is the core mechanism by which USD builds a resolved scene graph from
multiple layers, files, and composition arcs. Each prim in the final stage may
draw its opinions (property values, metadata, children) from many sources. The
AOUSD Core Spec (section 10) defines a strict strength ordering — **LIVRPS** —
that all conforming implementations must follow.

This document compares two implementations:

1. **OpenUSD PCP** (Prim Cache Population) — the reference implementation,
   a recursive DAG-based algorithm that preserves full opinion provenance.
2. **lightusd** — a progressive, iterative layer-flattening approach designed
   for minimal memory, zero dependencies, and security-focused parsing.

The goal is to analyze whether lightusd's approach correctly satisfies LIVRPS
ordering, document known limitations, and identify areas for future work.

This document covers the conceptual model plus **Instancing** and **Variants**.
lightusd also ships a parallel **DAG-based** engine that mirrors OpenUSD's PCP
design (`src/composition-graph.{hh,cc}`); its detailed API reference lives in
[pcp.md](pcp.md).

---

## The LIVRPS Composition Ordering

LIVRPS defines the relative strength of composition arcs. Stronger arcs override
weaker ones when they provide opinions for the same property or metadata field.
From strongest to weakest:

| Position | Arc Type | Description |
|----------|----------|-------------|
| **L** | Local & SubLayers | Direct opinions in the root layer and its sublayer stack |
| **I** | Inherits | Class-based opinion sharing (like OOP inheritance) |
| **V** | Variants | Switchable opinion sets within a prim |
| **R** | References | Cross-file prim inclusion |
| **P** | Payloads | Deferred cross-file prim inclusion (same semantics as references) |
| **S** | Specializes | Globally weak shared opinions (weaker than all other arcs everywhere) |

**Relocates** are not part of the LIVRPS mnemonic. They modify the composed
namespace by renaming prims after composition arcs are resolved (AOUSD Core Spec
10.3.2.6).

**Key spec sections:**
- 10.4 — LIVRPS ordering
- 10.4.1 — Specializes are globally weaker than all other opinions
- 10.3.2.5 — Variant selection is deferred: selections are computed using
  opinions from all arcs (I, R, P, S), not just local

---

## OpenUSD's PCP (Prim Cache Population)

OpenUSD's reference composition engine is **PCP** — *Prim Cache Population*. For
each prim it builds a **PcpPrimIndex**: a directed acyclic graph of `PcpNodeRef`
nodes (one per opinion source), constructed by a priority-ordered task queue
(`Pcp_BuildPrimIndex` in `pxr/usd/pcp/primIndex.cpp`). The DAG preserves full
opinion provenance, propagates implied class arcs, makes specializes globally
weak by propagating them to the graph root, and supports cycle detection, node
culling, lazy payloads, and incremental change tracking.

The arc strength order is the `PcpArcType` enum (`pxr/usd/pcp/types.h`): Root,
Inherit, Variant, **Relocate** (between Variant and Reference), Reference,
Payload, Specialize — matching the AOUSD LIVERPS ordering.

> **The full PCP engine model** — the data model, the task-queue algorithm,
> implied / globally-weak specializes, namespace mapping, instancing, change
> tracking, the error taxonomy, the AOUSD §10 spec mapping, and the
> **PCP ↔ AOUSD ↔ lightusd correspondence table** — lives in **[pcp.md](pcp.md)**,
> which also documents lightusd's mirroring DAG engine.

---

## lightusd's Progressive Layer-Flattening Approach

### Design Philosophy

lightusd takes a fundamentally different approach: instead of building a
composition graph, it **eagerly flattens** opinions from each arc type into a
single working `Layer` (a tree of `PrimSpec` objects). Each composition phase
processes the entire PrimSpec tree for one arc type, producing a new flattened
layer that becomes input to the next phase.

Design goals:
- **Minimal peak memory** — in-place variants free source data after copying
- **Zero external dependencies** — no Boost, TBB, or similar
- **Security-focused** — configurable depth limits, bounds checking, no exceptions
- **Simplicity** — a few thousand lines of C++ vs OpenUSD PCP's ~10,000+ lines

Core files:
- `src/composition.hh` — public API
- `src/composition.cc` — implementation
- `src/composition-reconstruct.cc` — PrimSpec-to-Prim conversion
- `src/namespace-mapping.hh` — path remapping utilities
- `src/core/composition-types.hh` — `Reference`, `Payload`, `ArcOrigin`, `LayerOffset`

### CompositeAllArcs — The Main Pipeline

`CompositeAllArcs()` orchestrates all composition arcs in LIVRPS order.
Sublayers (L) are composed before this function is called.

```
Input: Layer with sublayers already composed (L done)
  │
  ├─ Collect variant selection opinions (local)
  │
  ├─ I: CompositeInherits()
  │    └─ Collect variant opinions from inherited content
  │
  ├─ [V deferred — skip for now]
  │
  ├─ R: CompositeReferences()
  │    └─ Collect variant opinions from referenced content
  │
  ├─ P: CompositePayload()
  │    └─ Collect variant opinions from payload content
  │
  ├─ V: Deferred variant resolution
  │    ├─ ComputeVariantSelections() — strongest opinion wins
  │    └─ ApplyDeferredVariantSelectionsRec()
  │
  ├─ S: CompositeSpecializes() — applied last (globally weaker)
  │
  └─ Relocates: CompositeRelocates() — final namespace rename

Output: Fully composed Layer
```

Each phase checks whether the arc type is present before processing, avoiding
unnecessary work for simple scenes.

### Two PrimSpec Merging Primitives

All composition arcs ultimately merge PrimSpec trees using one of two operations:

**InheritPrimSpec**

Creates a copy of the source (weaker) PrimSpec, then overlays the destination
(stronger) PrimSpec's opinions on top. Properties present in `dst` override
those from `src`; properties only in `src` are preserved as defaults.

```
Result = copy(src)          // Start from weaker opinion
Result.metas = src.metas    // Copy metadata
Result.metas.update_from(dst.metas)  // Stronger overrides
Result.props[p] = dst.props[p] if exists, else keep src.props[p]
```

Used for: prepend references, prepend payloads, inherits, specializes.

**OverridePrimSpec**

Directly overlays the source (stronger) PrimSpec onto the destination (weaker).
Source properties replace destination properties.

```
dst.metas.update_from(src.metas)  // Stronger overrides
dst.props[p] = src.props[p] for each p in src
```

Used for: append references, append payloads.

Both operations implement AOUSD Core Spec section 12.2 metadata resolution:

| Spec | Rule | Implementation |
|------|------|---------------|
| 12.2.1 | Specifier: `over` + `def`/`class` = `def` | Strongest specifier wins; `over` defers to defining specs |
| 12.2.2 | typeName from defining specs only | Only `def`/`class` specs contribute typeName |
| 12.2.3 | Preserve uniform variability | If weaker opinion is `uniform`, keep it even if stronger is `varying` |
| 12.2.4 | Custom flag OR semantics | `custom = true` if ANY opinion says true |

### SubLayer Composition

`CompositeSublayersRec()`:

- Processes the root layer first (strongest), then sublayers in declaration order
- Uses `CombinePrimSpecRec()` for merging (weaker fills in gaps, does not override)
- **Cycle detection** via `IsVisited()` — tracks visited layer names in a stack of sets
- Applies layer offsets to timeSamples per Spec 10.3.1 via `ApplyLayerOffsetRec()`
- Tags PrimSpecs with arc origins via `TagPrimSpecArcOriginRec()` for implied
  inherit propagation

### Reference and Payload Composition

`CompositeReferencesRec()` and `CompositePayloadRec()` share the same structure:

1. **Depth-first traversal** — process children before the current prim
2. **Asset resolution** — `LoadAsset()` resolves paths using per-PrimSpec
   resolver state (current working path, search paths)
3. **Internal references** — empty asset path with absolute prim path treated as
   inherit-like operations within the same layer
4. **ListOp handling:**
   - `prepend` / `resetToExplicit` → `InheritPrimSpec()` (weaker fills in)
   - `append` / `add` → `OverridePrimSpec()` (stronger replaces); `add` is
     deprecated and treated as `append` with a warning (Spec 6.6.3.10)
   - `delete` → pre-pass filters matching arcs out of prepend/append
   - `order` → warns and skips (would require preserving the prepend/append
     merge distinction)
5. **Layer offset** — applied to timeSamples per Spec 10.3.2.2
6. **Arc origin tracking** — recorded for implied inherit propagation (Spec 10.3.2.3)
7. **Path prefix replacement** — `ReplaceRootPrimPathRec()` remaps referenced
   prim paths to the referencing prim's namespace

### Inherits Composition

`CompositeInheritsRec()`:

- Resolves all `inherits` ListEditQual ops (prepend, append, delete, order) into
  a single ordered path list via `ResolveListOps()`, then applies each target in
  order via `InheritPrimSpec()` — inherited content provides defaults
- Cycle detection via a visited-path set
- **Implied inherits** (Spec 10.3.2.3): multi-level propagation from
  referenced/payload layers via `inheritPaths` metadata (capped at depth 32).
  After applying an implied inherit, the inherited prim's own `inheritPaths`
  cascade. If matching class prims exist in the current layer stack, they are
  applied via `InheritPrimSpec()`.

### Specializes Composition

`CompositeSpecializesRec()`:

- Same structure as inherits composition (resolves ListEditQual ops, multi-target)
- Uses `InheritPrimSpecImpl()` — identical property-override semantics
- Applied last in `CompositeAllArcs()` to ensure globally weaker behavior
- Implied specializes via `specializePaths` metadata (Spec 10.3.2.4)

### Deferred Variant Evaluation

Implements AOUSD Core Spec 10.3.2.5 in three phases:

1. **Collect** — `CollectVariantSelectionOpinions()` gathers per-prim
   `VariantSelectionMap` values after each composition phase (local, I, R, P)
2. **Resolve** — `ComputeVariantSelections()` takes the strongest opinion per
   variant set name (first in the collected vector wins)
3. **Apply** — `ApplyDeferredVariantSelectionsRec()` applies resolved selections
   via `VariantSelectPrimSpec()`

This correctly ensures that variant selection can be influenced by opinions from
references and payloads, not just local opinions.

### Relocates

`CompositeRelocates()` implements AOUSD Core Spec 10.3.2.6:

- Validates entries via `ValidateRelocates()` from `namespace-mapping.hh`
- **Phase 1: Prim relocation** — detaches prims from source locations and
  reattaches at target locations. Handles same-parent renames, root-level
  renames (updates primspecs map key), and cross-parent moves (creates
  intermediate `over` prims as needed). Processes deepest paths first to
  avoid invalidating parent paths.
- **Phase 2: Path remapping** — remaps all path references throughout the tree
  using `NamespaceMapping::Apply()`: relationship targets, attribute connections,
  and composition arc target paths (inherits, specializes, internal references,
  internal payloads)
- Applied after all composition arcs as a final pass

### Namespace Mapping

`namespace-mapping.hh` provides path remapping utilities per Spec 10.5:

```cpp
struct NamespaceMapping {
  std::vector<std::pair<Path, Path>> entries;
  Path Apply(const Path &path) const;  // Prefix-match remapping
};
```

Factory functions:
- `MakeReferenceMapping()` — per Spec 10.3.2.1.1
- `MakeInheritMapping()` — per Spec 10.3.2.3.1
- `MakeRelocatesMapping()` — per Spec 10.3.2.6.1
- `ComposeNamespaceMappings()` — for nested arcs per Spec 10.5

Currently used primarily in relocates composition. Reference and payload
composition use `ReplaceRootPrimPathRec()` for path prefix replacement instead.

### In-Place Memory Optimization

The `*InPlace()` function variants take ownership of the source layer via
`std::unique_ptr<Layer>` and progressively free memory as PrimSpecs are
converted:

- `CompositeSublayersInPlace()`
- `CompositeReferencesInPlace()`
- `CompositePayloadInPlace()`
- `LayerToStageInPlace()`
- `PrimSpecToPrimInPlace()`

This reduces peak memory during composition of large scenes by releasing source
data as soon as it has been merged into the target.

---

## Comparison Table

| Aspect | OpenUSD PCP | lightusd |
|--------|------------|----------|
| **Data structure** | PcpPrimIndex DAG per prim | Flat PrimSpec tree (single working Layer) |
| **Algorithm** | Recursive, task-queue based | Iterative, phase-by-phase across whole tree |
| **Opinion retention** | All opinions preserved in DAG | Eagerly merged (destructive) |
| **Code size** | ~10,000+ lines (PCP module) | ~6,160 lines (composition.cc + .hh + composition-reconstruct.cc) |
| **LIVRPS encoding** | PcpArcType enum values + strength comparator | Sequential phase execution in CompositeAllArcs() |
| **Relocate position** | Between V and R in arc type enum | Applied after all arcs as final pass |
| **Specializes** | Propagated to graph root nodes | Applied as last composition phase |
| **Cycle detection** | All arc types (full graph walk) | All arc types (visited sets per arc type; depth limits as fallback) |
| **Multiple inherit targets** | Fully supported | Fully supported (processed in list order) |
| **Multiple specialize targets** | Fully supported | Fully supported (processed in list order) |
| **ListEditQual (inherits/specializes)** | Full support (prepend, append, delete, order) | Full support via ResolveListOpsT (prepend, append, delete, order) |
| **Implied inherits/specializes** | Multi-level propagation | Multi-level propagation via inheritPaths/specializePaths (depth-capped) |
| **Lazy payload evaluation** | Yes (can defer loading) | Yes (via `load_policy` callback in PayloadCompositionOptions) |
| **Deferred variant selection** | Yes | Yes (Spec 10.3.2.5 compliant) |
| **Layer offset application** | Yes | Yes (Spec 10.3.1, 10.3.2.2) |
| **Namespace mapping** | PcpMapExpression (composable, lazy) | NamespaceMapping (simple prefix matching) |
| **Memory model** | High (preserves full graph) | Low peak (in-place variants available) |
| **Dependencies** | Boost, TBB, etc. | Zero external dependencies |
| **Error handling** | C++ exceptions + error objects | nonstd::expected, error strings (no exceptions) |

---

## LIVRPS Correctness Analysis for lightusd

### L (Local/SubLayers): Correct

SubLayers are composed before `CompositeAllArcs()` is called.
`CompositeSublayersRec()` correctly:
- Processes root layer first (strongest), then sublayers in declaration order
- Uses `CombinePrimSpecRec()` with fill-in semantics (weaker opinions do not
  override stronger ones)
- Applies layer offsets to timeSamples
- Detects circular sublayer references via `IsVisited()`

### I (Inherits): Correct

`CompositeInherits()` correctly applies inherit arcs after local opinions. The
`InheritPrimSpec()` semantics are correct: inherited content provides defaults
that local opinions override.

- Multiple inherit targets per prim are supported (processed in list order)
- All ListEditQual ops are handled: `ResolveListOps()` folds prepend/append/
  delete/order into a single ordered path list, then each target is applied via
  `InheritPrimSpec()`; `add` is deprecated and treated as `append` with a warning
- Cycle detection via visited path set prevents infinite recursion
- Implied inherits: multi-level propagation from referenced/payload layers.
  When a referenced prim has `inherits`, those paths are propagated via
  `inheritPaths` metadata and applied if matching class prims exist in the
  referencing layer stack; the inherited prim's own `inheritPaths` cascade
  (depth-capped at 32).

### V (Variants): Correct

Deferred variant evaluation correctly implements Spec 10.3.2.5:
- Opinions collected from L, I, R, and P phases
- Strongest opinion wins per variant set name
- Applied after R and P, ensuring referenced/payload content contributes selections

### R (References): Correct

References correctly use:
- `InheritPrimSpec()` for `prepend`/`resetToExplicit` — local (stronger) opinions
  override referenced (weaker) content
- `OverridePrimSpec()` for `append` — appended content overrides existing values
- Layer offset application per Spec 10.3.2.2
- Arc origin tracking per Spec 10.3.2.3

- `delete` is handled by a pre-pass that filters matching arcs out of the
  prepend/append lists; `order` warns and skips (would require preserving the
  prepend/append merge distinction)
- Cycle detection via visited `(asset_path, prim_path)` set

### P (Payloads): Correct

Identical semantics to references. Same prepend/append handling, same layer
offset behavior. The only difference is that payloads are conceptually
deferrable, but lightusd resolves them eagerly.

### S (Specializes): Correct for Simple Cases

Specializes are applied last in `CompositeAllArcs()`, which correctly makes them
globally weaker than all other opinions per Spec 10.4.1.

- Multiple specialize targets supported, ListEditQual handled
- Implied specializes: propagation from referenced/payload layers via
  `specializePaths` metadata (Spec 10.3.2.4), applied if matching prims exist
  in the referencing layer stack
- Cycle detection via visited path set

**Caveat:** For complex scenarios with nested arcs, the flattening approach
cannot fully capture the "globally weaker" rule that OpenUSD PCP preserves
in its DAG-based strength comparator.

### Relocates: Different Positioning, Functionally Equivalent for Most Cases

lightusd applies relocates after all composition arcs as a final namespace
renaming pass. OpenUSD places Relocate between Variant and Reference in the arc
type strength enum, giving it a defined position in the DAG.

For most practical cases, the result is the same because relocates primarily
rename prims rather than contribute property opinions. The difference could
matter in edge cases where relocate-induced namespace changes affect which prims
are found by later-evaluated arcs — but since lightusd evaluates all arcs before
applying relocates, the relocation is purely a post-composition rename.

### Cycle Detection: All Arc Types Covered

Cycle detection is implemented for all arc types:
- **SubLayers**: `IsVisited()` with a stack of visited layer name sets
- **References/Payloads**: `ArcVisitedSet` tracking `(asset_path, prim_path)` pairs,
  shared across both references and payloads phases in `CompositeAllArcs()` for
  cross-arc cycle detection
- **Inherits/Specializes**: `PathVisitedSet` tracking visited prim paths within
  the same layer

The shared `ArcVisitedSet` catches cycles where an asset is loaded in the
references phase and then again in the payloads phase. Depth limits remain as
an additional safety net.

### Active Prim Metadatum

Prims with `active = false` (a composed metadatum) are removed from the
composed layer in a post-composition filtering pass. Both root-level and child
prims are handled. This ensures inactive prims and their descendants are excluded
from the final stage.

### Summary Verdict

lightusd's `CompositeAllArcs()` **correctly implements the overall LIVRPS
ordering** with full support for: multi-target inherits/specializes, ListEditQual
handling, cycle detection for all arc types, deferred variant evaluation
(Spec 10.3.2.5), and `active` prim metadatum filtering.

The remaining correctness risks are in edge cases: complex implied inherit
propagation across multiple layer stacks, and nested-arc specializes "globally
weaker" semantics. These affect spec compliance for advanced USD patterns but
do not impact the majority of real-world USD scenes.

---

## Instancing

Instancing lets multiple prims that share the same composition structure reuse a
single **prototype** representation. Instead of composing and storing each
identical subtree independently, the stage composes it once and shares the
result. The canonical use case is a scene with hundreds of identical assets
(trees, bolts, chairs) referenced from the same source: without instancing each
is a full copy in memory; with instancing they all share one prototype.

| Term | Meaning |
|------|---------|
| **Instance prim** | A prim with `instanceable = true` *and* at least one composition arc |
| **Prototype** | The shared composed representation that matching instances point to |
| **Instance key** | A structural signature (composition arcs + variant selections) determining which instances share a prototype |
| **Instance proxy** | (OpenUSD concept) A virtual prim at an instance path that transparently redirects to the prototype |

### Spec requirements (AOUSD Core Spec 11.3.3)

A prim becomes an **instance** only when **both** hold:

1. The strongest opinion of `instanceable` is `true`.
2. The prim has at least one composition arc — a value in `references`,
   `payload`, `inheritPaths`, `variantSets`, or `specializes`.

A prim with `instanceable = true` but no composition arc is **not** an instance.
`instanceable` (Spec 7.6.2.4.3, fallback `false`) is a *population* field
resolved by standard strongest-opinion rules; it does not affect a layer's
available specs. When a prim is an instance, the spec further requires:
single composition (all instances composed once), no overrides on the instance
prim, and **local opinions discarded** — including local contributions to
`primChildren`, so the child set comes only from the composition arc.

### USDA examples

Three trees sharing one prototype (identical reference, no variant difference):

```usda
def Xform "World" {
    def Xform "Tree_1" (
        instanceable = true
        references = @./tree.usda@
    ) { }
    def Xform "Tree_2" (
        instanceable = true
        references = @./tree.usda@
    ) { }
}
```

Different variant selections break sharing (different instance key → different
prototype):

```usda
def Xform "Chair_1" (
    instanceable = true
    references = @./chair.usda@
    variants = { string style = "modern" }
) { }

def Xform "Chair_3" (
    instanceable = true
    references = @./chair.usda@
    variants = { string style = "classic" }   # different prototype
) { }
```

`instanceable = true` with **no** composition arc is not an instance:

```usda
def Mesh "DirectMesh" (instanceable = true) {
    float3[] points = [(0,0,0), (1,0,0), (0,1,0)]   # local opinions kept
}
```

### lightusd implementation status

**Core layer**

- `instanceable` field: stored in `MetadataBase` (`src/core/metadata-base.hh`)
  with `has_/get_/set_/remove_instanceable()`; round-trips through USDA/USDC.
- Prim queries (`src/core/prim.hh`): `Prim::IsInstance()` (checks
  `instanceable=true`) and `Prim::HasCompositionArcs()` (references, payload,
  inherits, specializes, or variantSets).
- **Instance key** (`src/core/instance-key.{hh,cc}`): 128-bit SpookyHash of
  type name + references + payloads + inherits + specializes + variant
  selections. Two entry points:
  `ComputeInstanceKeyFromPrimSpec(const PrimSpec&, InstanceKey*)` and
  `ComputeInstanceKeyFromPrimMetas(const PrimMeta&, const std::string& type_name, InstanceKey*)`
  (the latter used by `Stage::BuildInstancePrototypes()`). The parallel DAG
  engine has its own `ComputeInstanceKey(const PrimIndex&, …)` — see
  [pcp.md](pcp.md#instancekey).

**Stage-level API** (`src/stage.hh`):

```cpp
size_t BuildInstancePrototypes();                       // group instances by InstanceKey
int    GetPrototypeIndex(const Path&) const;            // -1 if not an instance
int    GetPrototypeIndexForInstance(const Path&) const; // alias
bool   IsInstancePrim(const Path&) const;
std::vector<Path> GetInstancesForPrototype(int prototype_index) const;
std::string       GetPrototypeSourcePath(int prototype_index) const;
size_t num_prototypes() const;
void   ImportInstanceData(...);                         // from the DAG engine
```

`BuildInstancePrototypes()` registers only prims that satisfy *both* Spec
11.3.3 conditions (instanceable + at least one composition arc).

**Tydra / render layer** (`src/tydra/render-data.hh`): `RenderInstance` holds
`prototype_index`, `mesh_id` (index into the shared `RenderScene::meshes`),
`material_id`, and local/global matrices; `Node` carries `is_instance`,
`prototype_index`, and `instance_id`. `RenderScene::instances` is populated in
`ConvertToRenderScene()` after mesh conversion, with `mesh_id` pointing at
shared mesh data.

**WASM/JS** (`web/binding.cc`): `numInstances()`, `getInstance(id)`,
`getInstancesForMesh(mesh_id)`, plus `isInstance` / `prototypeIndex` /
`instanceId` on every node.

### Instancing gaps

| Gap | Description | Reference |
|-----|-------------|-----------|
| Prototype sharing | Each instance still holds a full Prim subtree copy; no memory dedup | Spec 11.3.3 |
| Local opinion filtering | Instance prims don't yet filter local opinions during population | Spec 11.3.3 |
| Instance proxies | No virtual proxy prims redirecting to a prototype | OpenUSD extension |
| Nested instancing | No instances within prototypes | OpenUSD extension |
| Edit restrictions | No enforcement that instance prims can't be overridden | Spec 11.3.3 |

Detection and grouping (the instance *key* path) is implemented; the remaining
work is the memory-sharing/population side. See
[`instancing.md`](instancing.md) for the full OpenUSD model and the scaling plan
for island-sized scenes.

---

## Variants

Variants provide multiple mutually-exclusive representations of an asset within
a single prim — e.g. level-of-detail, material options, or geometry
configurations. lightusd parses and writes variants in both USDA and USDC,
including nested variant sets, and resolves selections during composition (see
[Deferred Variant Evaluation](#deferred-variant-evaluation) above, which
implements Spec 10.3.2.5).

### Data structures

`Variant` and `VariantSet` live in `src/core/variant-types.hh` (a `Variant`
carries prim metadata, properties, prim children, and nested variant sets, all
behind accessors):

```cpp
struct Variant {
  PrimMeta &metas();                                   // variant metadata
  std::map<std::string, Property> &properties();        // properties
  std::vector<Prim> &primChildren();                    // child prims
  std::map<std::string, VariantSet> &variantSets();     // nested variant sets
};

struct VariantSet {
  std::string name;
  std::map<std::string, Variant> variantSet;            // variant name -> Variant
};
```

The composition-side `variantSet` statement is represented by `VariantSetSpec`
(holding `PrimSpec` children) in the same header. Variant *selections* are
stored in prim metadata as a `VariantSelectionMap`
(`= std::map<std::string,std::string>`, in `src/core/meta-variable.hh`):
`PrimMeta::variants` holds the selections and `PrimMeta::variantSets` holds the
authored variant-set list (a ListOp).

### USDA syntax

```usda
def Xform "Asset" (
    prepend variantSets = "lod"
    variants = { string lod = "high" }
)
{
    variantSet "lod" = {
        "high" { def Mesh "Mesh" { } }   # high-detail geometry
        "low"  { def Mesh "Mesh" { } }   # low-detail geometry
    }
}
```

Nested variant sets:

```usda
variantSet "lod" = {
    "high" ( prepend variantSets = "material" ) {
        def Mesh "Mesh" { }
        variantSet "material" = { "plastic" { } "metal" { } }
    }
}
```

### Authoring guidance

- Use `snake_case` set names and descriptive lowercase option names
  (`level_of_detail`/`high`, not `var1`/`opt_a`).
- One dimension of variation per variant set (a `material_type` set, not a set
  mixing LOD and material into `high_plastic`/`low_metal`).
- Keep nesting shallow — 3 levels is a practical maximum (deeper parsing works,
  but combinations explode: 3×3 = manageable, 3×3×4×4×3 = 432).

### Testing

Variant roundtrip tests live in `tests/usda/` (`variantSet-*.usda` for basic
parsing, `variantSet-nested-*.usda` for nested structures). Printing is driven
by `print_variantSetStmt()` in `src/pprinter.cc`; parsing by
`src/ascii-parser.cc`. Run a single compare:

```bash
node tests/compare-usda.js --detailed-diff \
  --lusdcat ./build/lusdcat --usdcat usdcat \
  tests/usda/variantSet-nested-001.usda
```

---

## Known Limitations and Gaps

| # | Limitation | Severity | Impact |
|---|-----------|----------|--------|
| 1 | Relocates applied as post-pass | Low | Post-composition rename (architecturally correct for flattening model) |
| 2 | `order` ListEditQual for references/payloads | Low | Warns and skips (implemented for inherits/specializes) |

---

## Future Work

Remaining items (all Low severity):

1. **Consider relocate positioning** — Evaluate whether applying relocates
   between V and R (matching OpenUSD) improves compatibility. In our flattening
   model, the post-pass approach is architecturally correct since relocates
   rename prims that must already exist.

2. **`order` ListEditQual for references/payloads** — Implemented for
   inherits/specializes via `ResolveListOpsT`. References/payloads warn+skip
   because they need to preserve the prepend/append merge semantic distinction.
   This qualifier is deprecated in USD and extremely rare in practice.

---

## References

- AOUSD Core Spec v1.1.0 (vendored at aousd/core/1.1.0), Sections 10 (Composition), 12 (Value Resolution)
- OpenUSD source:
  - `pxr/usd/pcp/primIndex.cpp` — core composition algorithm
  - `pxr/usd/pcp/strengthOrdering.cpp` — node strength comparison
  - `pxr/usd/pcp/types.h` — arc type enum and strength ordering
  - `pxr/usd/pcp/composeSite.cpp` — single-site field composition
- lightusd source:
  - `src/composition.hh` / `.cc` — default flattening pipeline
  - `src/composition-graph.hh` / `.cc` — parallel DAG engine (see [pcp.md](pcp.md))
  - `src/composition-reconstruct.cc` — PrimSpec-to-Prim conversion
  - `src/namespace-mapping.hh` — namespace mapping utilities
  - `src/core/composition-types.hh` — `Reference`, `Payload`, `ArcOrigin`, `LayerOffset`
  - `src/core/instance-key.{hh,cc}` — `InstanceKey` (instancing)
  - `src/core/prim.hh` — `Prim::IsInstance()` / `HasCompositionArcs()`
  - `src/stage.hh` — `Stage::BuildInstancePrototypes()` and instance queries
  - `src/core/variant-types.hh` — `Variant`, `VariantSet`, `VariantSetSpec`
- Related docs: [pcp.md](pcp.md) — DAG-engine (PrimIndex/CompositionGraph) API reference
