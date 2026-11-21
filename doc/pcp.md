# PCP: Prim Cache Population — OpenUSD, AOUSD, and the lightusd DAG Engine

**PCP** ("Prim Cache Population") is the name of OpenUSD's composition engine —
the subsystem that implements the layering-and-referencing semantics of USD and
hands the resulting strength-ordered opinion sources to higher-level libraries
such as `UsdStage`. The name is historical; in the OpenUSD module docs PCP is
introduced as *"Prim Cache Population … the behavior informally referred to as
Layering & Referencing"* (`pxr/usd/pcp/overview.dox`).

This document is the canonical PCP reference for lightusd. It covers three things
that describe the same composition model from three angles:

1. **The AOUSD composition model** — what the *Alliance for OpenUSD Core
   Specification v1.0.1* (Section 10) requires of any conforming implementation.
2. **OpenUSD's PCP implementation** — how the reference engine in
   `pxr/usd/pcp` actually builds the per-prim composition graph.
3. **lightusd's DAG engine** (`lightusd::composition_graph`) — a parallel
   implementation that deliberately mirrors PCP's data model and algorithm. Its
   full API reference is preserved below, verified against
   `src/composition-graph.hh`.

> **Two composition code paths in lightusd.** lightusd has two implementations:
> 1. The **iterative layer-flattening** pipeline (`CompositeAllArcs()` in
>    `src/composition.cc`) — the default, production path. See
>    [composition.md](composition.md) for the conceptual overview, LIVRPS
>    ordering, the LIVRPS-correctness analysis, instancing, and variants.
> 2. The **DAG engine** documented in the last half of this file
>    (`src/composition-graph.{hh,cc}`, namespace
>    `lightusd::composition_graph`) — the PCP-style code path that builds
>    `PrimIndex` graphs, then lowers them back to `PrimSpec`/`Stage` via the
>    same `composition-reconstruct.cc` pipeline.

**Reading guide.** Read [composition.md](composition.md) for the production
flattening pipeline and the analysis of whether it satisfies LIVRPS. Read *this*
file for the PCP engine model (OpenUSD + AOUSD) and the DAG-engine API.

---

## PCP ↔ AOUSD ↔ lightusd correspondence

The single most useful map in this document: each composition concept across the
three systems. Use it to jump between a symbol you know and its counterpart.

| Concept | OpenUSD PCP (symbol · header) | AOUSD § | lightusd `composition_graph` |
|---|---|---|---|
| Composition cache | `PcpCache` · `cache.h` | §10.1 | `composition_graph::CompositionGraph` (eager) · `pcp::Cache` (lazy + cached, `src/pcp/cache.hh`) |
| Per-prim composition graph | `PcpPrimIndex` · `primIndex.h` | §10 | `PrimIndex` |
| Shared COW node pool | `PcpPrimIndex_Graph` · `primIndex_Graph.h` | — | node pool inside `PrimIndex` |
| Graph node / opinion site | `PcpNodeRef`, `_Node` · `node.h` | §10 | `CompNode` |
| Arc + arc type | `PcpArc` · `arc.h`, `PcpArcType` · `types.h` | §10.3 | `ArcType` |
| Index-build algorithm | `Pcp_BuildPrimIndex` · `primIndex.cpp` | §10.4 | `PrimIndexBuilder` |
| Task queue | `Pcp_PrimIndexer`, `Task::Type` · `primIndex.cpp` | — | `CompositionTask` / `TaskType` |
| Namespace map (lazy expr) | `PcpMapExpression` · `mapExpression.h` | §10.5 | `MapExpr` |
| Namespace map (function) | `PcpMapFunction` · `mapFunction.h` | §10.5 | `NamespaceMapping` |
| Layer stack | `PcpLayerStack`, `PcpLayerStackIdentifier` · `layerStack.h` | §10.3.1 | `LayerStackEntry` |
| Opinion site | `PcpSite`, `PcpLayerStackSite` · `site.h` | — | `site_path_idx` + `layer_stack_idx` on `CompNode` |
| Instance key | `PcpInstanceKey` · `instanceKey.h` | §11.3.3 | `composition_graph::InstanceKey` |
| Payload inclusion control | `PcpCache` payload set · `cache.h` | §10.3.2.2 | `DeferredPayloadInfo` + `payload_policy` |
| Property composition | `PcpPropertyIndex` · `propertyIndex.h` | §11.3.2 | *(gap — value resolution handles this)* |
| Composition errors | `PcpErrorType` · `errors.h` | §10.6 | error strings via `nonstd::expected` |
| Change / dependency tracking | `PcpChanges`, `PcpDependencies` · `changes.h`, `dependencies.h` | — | `pcp::Cache` site→index reverse map + `Invalidate()` (no edit-diffing yet) |

Throughout, citations use **symbol names** rather than pinned line numbers, which
drift between OpenUSD checkouts.

---

## What PCP Is

PCP exists because USD scene description is organized across many files. A single
layer can describe a whole prim hierarchy, but splitting data across files lets
multiple departments collaborate and lets an asset (a rig, a model) be authored
once and reused many times. That reuse is expressed as **composition arcs** — a
reference points at an external file, an inherit points at a class, and so on.
PCP detects and interprets these arcs to bring the disparate files together into
a single, strength-ordered set of opinions for each prim (`overview.dox`).

PCP is *only* about finding the sources of opinions — not interpreting them.
Value resolution, scenegraph object identity, model hierarchy, and symmetry are
deliberately *not* part of PCP; they live in `Usd` and above. The main entry
point is `PcpCache` (`cache.h`), constructed for a root layer + optional session
layer + resolver context + payload inclusion set. Its two fundamental queries are
`PcpCache::ComputeLayerStack()` and `PcpCache::ComputePrimIndex()`; everything
else builds on those (AOUSD §10.1 defines composition as *"the process of
combining opinions for a prim path in multiple layers via a set of composition
operators … and providing a 'composed' view of a given prim"*).

---

## The AOUSD Composition Model

AOUSD Core Spec v1.0.1 **Section 10 (Composition)** is the contract both PCP and
lightusd implement. It defines the composition operators (§10.3), the strength
ordering / LIVERPS mnemonic (§10.4), namespace path translation (§10.5), and
error behavior (§10.6). The input to the algorithm is a *root layer*, a *prim
path*, and a flag for *whether payloads are included*; the output is a
strength-ordered list of layer specs that contribute opinions to that prim.

### Composition Operators

A **layer stack** is the strong-to-weak ordered set of layers obtained by
recursively gathering a root layer's sublayers (the root layer is first and
strongest). The operators, in the order the spec presents them:

| Operator | Field | AOUSD § | Summary |
|---|---|---|---|
| **Sublayers** | `subLayers` | §10.3.1 | Superimpose layers into a layer stack (root strongest). Each sublayer may carry a layer offset (time scale+offset), composed multiplicatively down the tree. |
| **References** | `references` | §10.3.2 | Add the composed opinions of a (layer, prim) target. Internal refs omit the asset path; an omitted prim path uses the target's `defaultPrim`. Carries a layer offset and a namespace mapping (§10.3.2.1.1). |
| **Payloads** | `payload` | §10.3.2.2 | Identical to references *except* they are only composed when the payload-load flag says so; otherwise the arc (and its offset) is ignored. The lazy-load mechanism. |
| **Inherits** | `inheritPaths` | §10.3.2.3 | Class-based arc: pull opinions from a prim in this layer stack *and from every upstream layer stack that introduced this one* (the "implied" inherits, §10.3.2.3). |
| **Specializes** | `specializes` | §10.3.2.4 | Same mechanism as inherits but **globally weak** (§10.4.1). |
| **Variants** | `variantSetNames` / `variants` | §10.3.2.5 | Select one variant from a named set. The selection is computed from the strongest `variantSelection` opinion and *deferred* until all other arcs have been evaluated. |
| **Relocates** | `relocates` / `layerRelocates` | §10.3.2.6 | Remap opinions from a source path (introduced via another arc) to a new path in this layer stack, subject to strict constraints. |

All arc lists are composed with **list editing** (ListOps, §10.3.2): the weakest
layer's list-op is applied to an empty list, then each stronger layer's list-op
in turn (prepend / append / delete / explicit / order), yielding a final
strong-to-weak list of arcs.

> **Implementation status (next).** The USDA parser records each arc field's
> list-op qualifier (prepend / append / delete / bare-explicit) into a per-spec
> `ArcListOpEdits`, and the USDA writer re-emits it. *Across the layer stack*,
> `CompositionOptions::apply_list_ops` (**default ON**) merges each arc field
> once across the site weakest→strongest per AOUSD SdfListOp rules
> (explicit-replace / prepend / append / delete / dedup), in LIVRPS order —
> validated to match pxrUSD `usdcat --flatten` on cross-layer delete/explicit
> assets. Set it false for the legacy path (each spec's arcs expanded
> independently / strong-first concatenation; an identically-authored arc in two
> sublayers expands twice). The crate (USDC) writer/reader preserve the
> qualifier via a companion `<arc>_listOp` token[] field (the flat token[]
> carries the within-spec effective list).
>
> **Relocates scope (next).** Relocates are applied as a *same-parent namespace
> rename only*: a composed source `/A/B` may be renamed to `/A/C` (same parent
> `/A`) as a post-composition rename in `BuildStage`. Reparenting under a
> different ancestor is not yet modeled.

### Strength Ordering: LIVERPS (and the older LIVRPS)

AOUSD §10.4 defines how to order two opinions X and Y for the same prim:

1. **Same spec path, same layer stack, different layers** → the opinion from the
   stronger layer wins (§10.4 Example 1, sublayer ordering).
2. **Local beats remote** — *"'local' opinions from a layer stack are stronger
   than all 'remote' opinions that are introduced via composition arcs authored
   in that layer stack."*
3. **Different arc types, same layer stack** → the stronger arc *type* wins,
   following this order (strongest first):

   > Inherits · Variants · Relocates · References · Payloads · Specializes

4. **Same arc type, same layer stack** → tie-break in order: the arc authored
   *deeper in namespace* is stronger (Example 3); else an *authored* arc beats an
   *implied* arc (Example 4); else the order computed when composing that layer
   stack's arc list decides (Example 5, sibling arc ordering).
5. **Specializes are globally weakest** (§10.4.1): if A specializes B, *all*
   opinions for A are stronger than *all* opinions for B — including implied
   opinions and opinions from arcs introduced by B.

The spec names rule 3's ordering with the **LIVERPS** mnemonic (pronounced
*"liver-peas"*):

> **L**ocal, **I**nherits, **V**ariants, **R**elocates, **R**eferences, **P**ayloads, **S**pecializes

> **LIVERPS vs. LIVRPS.** Older OpenUSD documentation and lightusd's
> [composition.md](composition.md#the-livrps-composition-ordering) use the
> six-letter **LIVRPS** form (Local, Inherits, Variants, References, Payloads,
> Specializes), which omits Relocates from the acronym but still places it at the
> same strength slot — between Variants and References. AOUSD v1.0.1 formalises
> that slot by spelling it out as the first **R** in **LIVERPS**. Both describe
> the *same* ordering; LIVERPS simply names the Relocates position explicitly.
> This ordering matches OpenUSD's `PcpArcType` enum exactly (see below).

### Namespace Path Translation

Each arc carries a **namespace mapping**: a bijection describing how prim paths
transform between the *source* namespace (where opinions are authored) and the
*target* namespace (where the arc was authored). Mappings are represented as
(source-prefix → target-prefix) pairs; a path under a source prefix is rewritten
by swapping the prefix. Mappings can be **inverted** and **composed** (so a chain
of nested arcs collapses to one mapping from the innermost source to the
outermost target). The **identity** mapping is `[(/, /)]`. A source path matched
by no prefix simply does not address anything in the target namespace (§10.5).

Worked examples from §10.3:

- A reference `def "A" (references = @ref.usda@</B>)` maps the referenced
  namespace at/under `/B` to `/A`: mapping `[(/A, /B)]` (§10.3.2.1.1).
- An inherit `def "A" (inherits = </B>)` maps `[(/A, /B), (/, /)]` — note the
  added identity pair (§10.3.2.3.1).
- Variants do **not** remap namespace (they are a branch in the same namespace):
  identity `[(/, /)]` (§10.3.2.5.2).
- **Relocates compose onto arc mappings** (§10.3.2.6.1). Given a layer stack with
  `relocates = { </A/B> : </A/C> }` and a reference on `/A` whose own mapping is
  `[(/Ref, /A)]`, the relocate contributes `[(/A/B, /A/C)]`, composing to the
  final reference mapping `[(/Ref, /A), (/Ref/B, /A/C')]`.

### Composition Errors

§10.6: a *composition error* is an error in the specification of a composition
operator. The algorithm **does not terminate** on one — it evaluates all other
operators for the prim and returns the opinions it could compute. Examples:
unopenable/cyclic sublayers, invalid reference/payload targets, invalid relocate
entries, missing inherit targets in the local layer stack. PCP mirrors this:
errors are first-class outputs (see *Error Types* below), not exceptions.

---

## OpenUSD's PCP Implementation

This section is the deep dive into `pxr/usd/pcp`. lightusd's DAG engine mirrors
it; the next major section documents the lightusd side.

### Data Model

The object graph is `PcpCache` → `PcpPrimIndex` → `PcpPrimIndex_Graph` →
`PcpNodeRef`:

- **`PcpCache`** (`cache.h`) caches composition results for a fixed root/session
  layer pair, resolver context, and payload inclusion set, and tracks the
  dependencies needed to invalidate them.
- **`PcpPrimIndex`** (`primIndex.h`) is the composition result for one prim: a
  graph of nodes plus a strong-to-weak `_primStack` of `Pcp_CompressedSdSite`
  entries (each a `(nodeIndex, layerIndex)` pair, see `types.h`) naming the prim
  specs that actually contribute opinions.
- **`PcpPrimIndex_Graph`** (`primIndex_Graph.h`) holds the nodes in a
  **copy-on-write shared node pool**, so structurally-identical subgraphs (and
  whole indexes) can be shared cheaply — this is what makes instancing and
  ancestral cloning affordable.
- **`PcpNodeRef`** (`node.h`) is a lightweight `(graph*, nodeIndex)` handle to a
  node. *"Child nodes are stored and composited in strength order. Each node
  holds information about the arc to its parent."* A node represents an **opinion
  site** = a (layer stack, path) pair (`PcpLayerStackSite`, `site.h`).

Per-node data (split into a shared `_Node` and per-graph `_UnsharedData`):

| Field | Meaning |
|---|---|
| `arcType` | `PcpArcType` of the arc to the parent |
| `mapToParent` | `PcpMapExpression` translating this site's namespace to the parent |
| `mapToRoot` | composed mapping all the way to the root node |
| `origin` | the node that *introduced* this one (≠ parent for implied class arcs) |
| `arcSiblingNumAtOrigin` | order among same-type siblings (lower = stronger) |
| `arcNamespaceDepth` | namespace depth at which the arc was introduced |
| `permission` | `SdfPermission` (public/private) governing contribution |
| flags | `inert`, `culled`, `restricted`/`permissionDenied`, `hasSpecs`, `hasSymmetry`, `hasValueClips`, `isDueToAncestor`, `hasTransitiveDirectArc`/`…AncestralArc` |

`PcpNodeRef` exposes `GetArcType()`, `GetParentNode()`, `GetOriginNode()`,
`GetSite()`/`GetPath()`/`GetLayerStack()`, `GetMapToParent()`/`GetMapToRoot()`,
the `IsInert()`/`IsCulled()`/`HasSpecs()`/`CanContributeSpecs()` predicates, and
children iteration. The layer stack itself (`PcpLayerStack`, `layerStack.h`) is
keyed by a `PcpLayerStackIdentifier` (root layer, session layer, resolver
context, expression-variable overrides) and owns the composed sublayer list,
offsets, and relocates table.

The conceptual shape of a `PcpPrimIndex` DAG (root strongest, children in
strength order):

```
Root (local layer stack)
├── Inherit → /__class__/Base
│   └── Reference → asset.usd</Base>
├── Variant → {modelingVariant=high}
├── Reference → model.usd</Model>
│   ├── Inherit → /__class__/ModelBase   (implied)
│   └── Payload → geo.usd</Geo>
└── Specialize → /__class__/Defaults     (propagated to root — globally weak)
```

### Arc Types and Strength Order

The arc strength order is the `PcpArcType` enum (`types.h`); lower value =
stronger. `Root` is special (no parent); the rest are *"listed in strength
order"*:

```cpp
enum PcpArcType {
    PcpArcTypeRoot,        // special: the prim's own site, no parent
    // The following are in strength order:
    PcpArcTypeInherit,     // strongest arc
    PcpArcTypeVariant,
    PcpArcTypeRelocate,    // between Variant and Reference
    PcpArcTypeReference,
    PcpArcTypePayload,
    PcpArcTypeSpecialize,  // weakest (but see globally-weak handling)
    PcpNumArcTypes
};
```

This is exactly the AOUSD LIVERPS order (Inherits, Variants, Relocates,
References, Payloads, Specializes) plus the special `Root`. `PcpIsClassBasedArc()`
returns true for Inherit and Specialize — *"The key characteristic of these arcs
is that they imply additional sources of opinions outside of the site where the
arc is introduced"* (`types.h`). A related `PcpRangeType` enum lets callers
iterate only a sub-range of the strength-ordered nodes (e.g. just references).

### The Build Algorithm: `Pcp_PrimIndexer` Task Queue

`Pcp_BuildPrimIndex()` (`primIndex.cpp`) builds one index. It is driven by a
`Pcp_PrimIndexer` that holds a priority queue of `Task`s. Crucially, the **task
evaluation order is distinct from the arc strength order**. The `Task::Type` enum
is a *flag* enum (`1 << n`) and *"must be in evaluation priority order"*:

```cpp
struct Task {
    enum Type {
        EvalNodeRelocations    = 1 << 0,   // highest priority — evaluated first
        EvalImpliedRelocations = 1 << 1,
        EvalNodeReferences     = 1 << 2,
        EvalNodePayloads       = 1 << 3,
        EvalNodeInherits       = 1 << 4,
        EvalNodeSpecializes    = 1 << 5,
        EvalImpliedSpecializes = 1 << 6,   // before implied classes (ancestral-opinion ordering)
        EvalImpliedClasses     = 1 << 7,
        EvalNodeAncestralVariantSets      = 1 << 8,
        EvalNodeAncestralVariantAuthored  = 1 << 9,
        EvalNodeAncestralVariantFallback  = 1 << 10,
        EvalNodeAncestralVariantNoneFound = 1 << 11,
        EvalNodeAncestralDynamicPayloads  = 1 << 12,
        EvalNodeVariantSets               = 1 << 13,
        EvalNodeVariantAuthored           = 1 << 14,
        EvalNodeVariantFallback           = 1 << 15,
        EvalNodeVariantNoneFound          = 1 << 16,
        EvalNodeDynamicPayloads           = 1 << 17,   // last — file-format args finalized
        EvalUnresolvedPrimPathError       = 1 << 18,
        None                              = 0
    };
    // ...
};
```

The `Task::PriorityOrder` comparator *"sorts tasks in priority order from lowest
priority to highest priority, so highest-priority tasks come last"* (the queue
pops the highest-priority task). For most task types the order between two tasks
is type-driven and otherwise arbitrary; but **variant** and **dynamic-payload**
tasks are processed in *node strength order* (`PcpCompareNodeStrength`), because a
variant selection or a file-format argument can depend on non-local opinions and
so must see stronger opinions first. `EvalImpliedClasses` has a subtle extra
rule: when two implied-class tasks are queued, an ancestor node must be processed
*after* its descendant (it relies on the node-index ordering of `operator<` as a
cheap ancestor test).

Why the evaluation order differs from the strength order: inherits and
specializes are *class-based* — evaluating them can introduce new arcs and can
change which variant or payload applies. So PCP resolves relocations, then
references/payloads, then the class arcs and their implied propagation, and only
*then* the deferred variants and dynamic payloads, even though the *resulting*
nodes are filed in LIVERPS strength order.

The per-node eval functions: `_EvalNodeRelocations`, `_EvalNodeReferences`,
`_EvalNodePayloads`, `_EvalNodeInherits`, `_EvalNodeSpecializes`,
`_EvalNodeVariantSets` (+ `…VariantAuthored` / `…VariantFallback` /
`…VariantNoneFound`), `_EvalImpliedClasses`, and `_EvalImpliedSpecializes`. A
sketch of the driver loop:

```text
build_prim_index(site):
    graph = new graph with Root node for `site`
    if site is not a variant path:
        clone the parent prim index and re-root it here   # ancestral opinions
    if site path is prohibited by relocations:
        mark root inert; return
    indexer.AddTasksForNode(root)                          # pre-scan arcs → enqueue Eval* tasks
    while indexer has tasks:
        task = pop highest-priority task
        switch task.type:
            EvalNodeReferences:  for each ref  → recurse build_prim_index(ref target),
                                                  insert result as a child Reference node,
                                                  enqueue that subgraph's tasks
            EvalNodeInherits:    insert Inherit child for each inheritPath; enqueue
            EvalNodeSpecializes: insert (inert) Specialize child; enqueue
            EvalImpliedSpecializes / EvalImpliedClasses:
                                 propagate the class arc into ancestor layer stacks
            EvalNodeVariantSets: resolve selection (authored → fallback → none), then
                                 recurse into the selected variant
            EvalNode*Payloads:   include only if the payload predicate/inclusion set allows;
                                 otherwise record as deferred
            ...
    cull_inert_and_no_opinion_subtrees(graph)
    enforce_permissions(graph)
    graph.Finalize()                                       # compute strength order, compact
```

### Ancestral Opinions and Cycle Detection

When PCP indexes a *child* prim (not a root), it first **clones the parent prim
index** and re-roots it at the child path (`_BuildInitialPrimIndexFromAncestor`).
This is how a reference or inherit authored on an *ancestor* correctly applies to
the child. The recursion carries a `PcpPrimIndex_StackFrame`
(`primIndex_StackFrame.h`) chain so that, when an arc would re-enter a (layer
stack, path) site already on the chain, PCP detects the **cycle** and emits a
`PcpErrorType_ArcCycle` (or `…SublayerCycle`) instead of recursing forever. The
`PcpSiteTracker` in `types.h` records the visited `(site, arcType)` segments for
this purpose.

### Implied Classes and Globally-Weak Specializes

Class-based arcs *imply* arcs in the layer stacks that introduced them. If a
referenced asset inherits `/_class_/Foo`, that inherit is *implied* in the
referencing layer stack too, so local overrides on `/_class_/Foo` compose
correctly. `_EvalImpliedClasses` propagates inherits across the arcs that brought
in the originating layer stack, computing each implied path through the namespace
mappings.

**Specializes are globally weak.** AOUSD §10.4.1 requires that if A specializes B,
*every* opinion for A outranks *every* opinion for B. PCP achieves this by
**propagating specialize nodes to the root** of the prim index:
`_EvalImpliedSpecializes` → `_FindSpecializesToPropagateToRoot`. The authored
specialize node is left inert as a placeholder; the propagated copy is filed at
the root so it sorts weaker than everything else, everywhere. (Phrase it as
"propagated to root so it is weaker everywhere," not merely "evaluated last" —
"evaluated last" is the *flattening-pipeline* approximation described in
composition.md.) Implied specializes are handled *before* implied classes
(`1<<6` before `1<<7`) to keep behavior stable when duplicate nodes arise from
ancestral opinions.

### Post-Passes: Culling, Permissions, Finalize

After the queue drains:

- **Culling** removes subtrees that contribute no opinions
  (`_CullSubtreesWithNoOpinions`), marking nodes `culled`. Inert nodes (structural
  placeholders that can never contribute, e.g. specialize placeholders) are
  likewise flagged. Both make `CanContributeSpecs()` false. Dependency
  information is retained so culling does not break change tracking.
- **Permission enforcement** (`_EnforcePermissions`) applies `SdfPermission`:
  once a node is permission-denied, weaker opinions beyond it cannot contribute.
- **`Finalize()`** (`primIndex_Graph.h`) computes the final strength order,
  compacts the node pool, and erases culled nodes for cache-efficient traversal.

### Namespace Mapping (`PcpMapExpression` and `PcpMapFunction`)

`PcpMapFunction` (`mapFunction.h`) is the concrete transform: a set of
(source → target) path pairs plus an `SdfLayerOffset` (time scale+offset). It can
`Compose`, `Inverse`, `MapSourceToTarget`/`MapTargetToSource`, and reports
`IsIdentity()` for fast-pathing.

`PcpMapExpression` (`mapExpression.h`) is a *lazy expression tree* whose value
type is `PcpMapFunction`. It exists *"solely to support efficient incremental
handling of relocates edits"*: it represents the tree of mapping operations and
their inputs (mutable `Variable`s), `Evaluate()`s to a cached `PcpMapFunction`,
shares common sub-expressions, and invalidates exactly the dependents of any
changed variable. Each node's `mapToParent`/`mapToRoot` are `PcpMapExpression`s.
Path translation across the whole index is exposed by
`PcpTranslatePathFromNodeToRoot()` / `PcpTranslatePathFromRootToNode()`
(`pathTranslation.h`).

### Property Composition: `PcpPropertyIndex`

`PcpPropertyIndex` (`propertyIndex.h`) is the property-level analogue of
`PcpPrimIndex`: a strong-to-weak `_propertyStack` of property specs (each tagged
with its originating node), built by `PcpBuildPropertyIndex()`, which computes /
reuses the owning prim index. AOUSD §11.3.2 defines the matching "Ordered
Property Children" rule (merge `propertyChildren`, sort by path element order,
apply the strongest authored `propertyOrder`). **lightusd's DAG engine has no
separate property index** — property opinions are resolved later in
reconstruction / value resolution.

### Instancing: `PcpInstanceKey`

`PcpInstanceKey` (`instanceKey.h`) is a structural hash of a prim index: it hashes
the index's arcs (type, source site, layer offset) and variant selections. *"Two
prim indexes with equal instance keys are guaranteed to have identical opinions
for child prims and their properties"* and so may share one prototype.

A prim index is **instanceable** (`Pcp_PrimIndexIsInstanceable`, `instancing.h`)
when the composed `instanceable` metadata is `true` *and* it has instanceable
nodes. A node is instanceable when:

```cpp
node.HasTransitiveDirectDependency() && node.CanContributeSpecs() && node.HasSpecs()
```

i.e. it sits in (or under) a *direct* composition arc and actually carries specs.
This lets two prims whose only difference is implied arcs in different layer
stacks (with no overrides) still count as equivalent for sharing. AOUSD §11.3.3
states the requirement implementation-neutrally: *"If the strongest opinion of
the `instanceable` metadata field is `true`, the resulting prim is an instance
prim so long as it has at least one composition arc … Local opinions are
discarded, which also includes local opinions contributing to `primChildren` …
It is up to the implementation how to model the **shared representation** of
instanced prims sharing the same arc."*

> **Terminology.** The AOUSD spec deliberately says **"shared representation"** and
> avoids "prototype"/"master". OpenUSD and lightusd both use **"prototype"** for
> the concrete shared object. The composition arcs that qualify a prim for
> instancing are `references`, `variantSets`, `payload`, `inheritPaths`, or
> `specializes`.

### Payloads and Dynamic Payloads

Payloads are deferred references. `PcpCache` owns the **payload inclusion set**:
`IsPayloadIncluded(path)`, `GetIncludedPayloads()`,
`RequestPayloads(include, exclude)` (which invalidates affected indexes), and
`FindPayloads(childrenPred, payloadPred)` to walk namespace and add payloads a
predicate accepts. During indexing, `_EvalNodePayloads` includes a payload only
if it is in the set; otherwise the arc is skipped (matching AOUSD §10.3.2.2 — an
unloaded payload and its time offset are ignored). **Dynamic payloads** (target
asset paths that depend on file-format arguments / expression variables) are
evaluated last (`EvalNodeDynamicPayloads = 1<<17`) once those arguments are
finalized.

### Change Tracking and Incremental Recomposition

PCP records, for every cached index, the sites it depends on, so an edit triggers
*minimal* recomposition:

- **`PcpDependency` / `PcpCulledDependency`** (`dependency.h`) record a
  `(indexPath, sitePath, mapFunc)` plus dependency flags. `PcpDependencyType`
  classifies a node's role: `Root`, `PurelyDirect`, `PartlyDirect`, `Ancestral`,
  `Virtual` (structural, no specs). `Pcp_Dependencies` (`dependencies.h`) is the
  cache-wide registry (`Add`/`Remove`/`ForEachDependencyOnSite`).
- **`PcpChanges`** (`changes.h`) represents the effect of a set of scene edits:
  `PcpLayerStackChanges` (`didChangeLayers`, `…Offsets`, `…Relocates`,
  `didChangeSignificantly`) and `PcpCacheChanges` (`didChangeSignificantly`,
  `didChangeSpecs`, `didChangePrims`, `didChangeTargets`, `didChangePath`).
  Processing is two-phase — build the change list (chasing dependencies), then
  apply it (invalidate caches) — with a `PcpLifeboat` retaining referenced layers
  until clients re-pull.

**lightusd's `composition_graph::CompositionGraph` has no equivalent** — it composes
once and recomposes the affected prim when a payload is loaded or unloaded. The
**`pcp::Cache`** engine (below) adds the cached/lazy counterpart: a per-prim result
cache, a site→index reverse-dependency map, and an explicit `Invalidate(path)` that
drops only the affected indices. It does *not* implement PCP's two-phase
`PcpChanges` edit-diffing — invalidation is explicit, not derived from scene edits.

### Error Types

`PcpErrorType` (`errors.h`) enumerates the composition error categories; each has
a `PcpErrorBase` subclass carrying the offending site. Composition continues past
all of them (AOUSD §10.6). The full set:

```
ArcCycle, ArcPermissionDenied, ArcToProhibitedChild,
IndexCapacityExceeded, ArcCapacityExceeded, ArcNamespaceDepthCapacityExceeded,
InconsistentPropertyType, InconsistentAttributeType, InconsistentAttributeVariability,
InternalAssetPath, InvalidPrimPath, InvalidAssetPath,
InvalidInstanceTargetPath, InvalidExternalTargetPath, InvalidTargetPath,
InvalidReferenceOffset, InvalidSublayerOffset, InvalidSublayerOwnership, InvalidSublayerPath,
InvalidVariantSelection, MutedAssetPath,
InvalidAuthoredRelocation, InvalidConflictingRelocation, InvalidSameTargetRelocations,
OpinionAtRelocationSource,
PrimPermissionDenied, PropertyPermissionDenied, SublayerCycle, TargetPermissionDenied,
UnresolvedPrimPath, VariableExpressionError
```

Errors are first-class outputs: each computation keeps `GetLocalErrors()`, and an
`allErrors` accumulator collects newly-discovered errors across recursive calls
(`overview.dox`, "Errors").

---

## The lightusd DAG Engine (`composition_graph`)

The rest of this document is the API reference for lightusd's DAG composition
engine — the PCP-style code path that builds a per-prim composition graph
(`PrimIndex`) preserving opinion provenance, supports lazy payload loading, and
detects instances during composition. It deliberately mirrors the OpenUSD design
described above; cross-references to the matching PCP symbols are noted inline.

> All names below are verified against `src/composition-graph.hh`.

**Header**: `src/composition-graph.hh` ·
**Implementation**: `src/composition-graph.cc` ·
**Namespace**: `lightusd::composition_graph` (aliased at `lightusd::CompositionGraph`)

### Overview

The engine implements AOUSD Core Spec Section 10 (Composition):

- Full **LIVRPS/LIVERPS** ordering via a priority-ordered task queue
- Globally-weak specializes (propagated to root)
- Multi-level implied inherits/specializes
- Instancing-aware composition (`InstanceKey`-based deduplication)
- Incremental payload load/unload

Memory layout is deliberately compact: nodes are 40-byte `CompNode` structs
stored in a contiguous pool and linked by `uint16_t` indices; paths, layer
stacks, and namespace-mapping expressions are interned in shared tables owned
by the `CompositionGraph`. (This is the lightusd counterpart to PCP's
copy-on-write `PcpPrimIndex_Graph`.)

### Arc types and ordering

```cpp
enum class ArcType : uint8_t {
  Root = 0,        // Root node (direct local opinions)
  SubLayer = 1,    // L: sublayer
  Inherit = 2,     // I: inherits (class-based)
  Variant = 3,     // V: variant selection
  Relocate = 4,    // Between V and R (matches PCP enum ordering)
  Reference = 5,   // R: references
  Payload = 6,     // P: payloads
  Specialize = 7,  // S: specializes (weakest, globally weak)
};

bool IsClassBasedArc(ArcType t);   // true for Inherit / Specialize
const char *ArcTypeName(ArcType t);
```

Lower enum value = stronger. `Relocate` sits between `Variant` and `Reference`,
matching OpenUSD's [`PcpArcType`](#arc-types-and-strength-order) ordering and the
AOUSD LIVERPS sequence. See
[composition.md](composition.md#the-livrps-composition-ordering) for the LIVRPS
strength semantics.

Tasks are processed in a separate priority order that drives graph construction
— the lightusd analogue of PCP's
[`Task::Type`](#the-build-algorithm-pcp_primindexer-task-queue):

```cpp
enum class TaskType : uint8_t {
  EvalRootNode = 0,
  EvalSubLayers = 1,
  EvalImpliedInherits = 2,
  EvalInherits = 3,
  EvalVariants = 4,
  EvalReferences = 5,
  EvalPayloads = 6,
  EvalImpliedSpecializes = 7,
  EvalSpecializes = 8,
  EvalRelocates = 9,
};
```

> Unlike PCP's `Task::Type` (a `1<<n` *flag* enum carrying both ancestral and
> dynamic-payload variants), lightusd's `TaskType` is a small contiguous enum and
> handles ancestral/dynamic cases within the builder rather than as distinct task
> kinds.

### CompNode

A single node in a prim's composition DAG (`<= 40` bytes, enforced by
`static_assert`). Children form a singly-linked sibling list in strength order
(strongest first); links are `uint16_t` indices into the node pool. This is the
lightusd counterpart to PCP's [`PcpNodeRef`/`_Node`](#data-model).

```cpp
struct CompNode {
  static constexpr uint16_t kInvalidIndex = 0xFFFF;

  uint16_t parent, first_child, next_sibling, origin;  // node indices
  ArcType  arc_type;
  uint8_t  depth;            // depth in the DAG
  NodeFlags flags;
  uint16_t layer_stack_idx;  // index into LayerStackEntry table
  uint16_t layer_idx;        // layer index within the stack
  uint16_t map_expr_idx;     // index into MapExpr pool
  uint32_t site_path_idx;    // index into interned path table
  int32_t  strength_order;   // pre-computed (lower = stronger)
  uint16_t sibling_num;      // order among same-type siblings at origin

  // queries: has_parent(), has_children(), has_next_sibling(),
  //          has_specs(), is_inert(), is_culled(),
  //          is_payload_deferred(), is_payload_loaded(), is_implied_arc()
};
```

### NodeFlags

```cpp
enum class NodeFlags : uint16_t {
  None             = 0,
  HasSpecs         = 1 << 0,  // authored specs at this path
  Inert            = 1 << 1,  // cannot contribute opinions
  Culled           = 1 << 2,  // pruned during culling
  PermissionDenied = 1 << 3,
  PayloadLoaded    = 1 << 4,
  PayloadDeferred  = 1 << 5,  // payload exists but not yet loaded
  IsImpliedArc     = 1 << 6,  // propagated implied inherit/specialize
  IsDueToAncestor  = 1 << 7,
};
// operators | & ~ and HasFlag(flags, flag) are provided.
```

These mirror the PCP node flags (`HasSpecs`, `Inert`, `Culled`,
`PermissionDenied`, `IsDueToAncestor`) described in [Data Model](#data-model).

### PrimIndex

The composition graph for a single prim: a DAG of `CompNode` in a contiguous
vector (node 0 is always the root), plus a pre-computed strength order for fast
value resolution. The counterpart to OpenUSD's [`PcpPrimIndex`](#data-model).

```cpp
class PrimIndex {
 public:
  const Path &GetPath() const;
  const CompNode &GetRootNode() const;             // node 0
  const CompNode &GetNode(uint16_t idx) const;
  uint16_t GetNodeCount() const;
  const std::vector<uint16_t> &GetStrengthOrder() const;  // strongest first
  bool HasAnySpecs() const;
  bool IsPayloadLoaded() const;                    // true if no deferred payloads
  bool IsInstanceable() const;                     // instanceable=true metadata
  std::string DumpToString() const;
};
```

`PrimIndex` borrows the shared path/layer-stack/map-expression tables from its
owning `CompositionGraph`, so it must not outlive that graph.

### Shared tables

Owned by `CompositionGraph`, referenced by every `PrimIndex`:

```cpp
struct LayerStackEntry {
  const Layer *layer;       // borrowed (owned by CompositionGraph)
  std::string identifier;   // asset path or layer name
  LayerOffset offset;       // cumulative time offset to root
};

// Namespace mapping with lazy composition up a parent chain.
struct MapExpr {
  NamespaceMapping mapping;  // direct mapping for this arc
  int32_t parent_expr;       // index of parent expr (-1 = identity)
  const NamespaceMapping &GetComposed(const std::vector<MapExpr> &pool) const;
  Path Apply(const Path &path, const std::vector<MapExpr> &pool) const;
};

// Provenance of a resolved opinion.
struct OpinionSource {
  uint16_t node_idx, layer_stack_idx, layer_idx;
  std::string site_path;
};
```

`MapExpr` is the engine's equivalent of OpenUSD's
[`PcpMapExpression`](#namespace-mapping-pcpmapexpression-and-pcpmapfunction): composed
mappings are computed lazily and cached. The underlying prefix-remapping type is
`NamespaceMapping` from `src/namespace-mapping.hh` — the analogue of
`PcpMapFunction` (see [composition.md](composition.md#namespace-mapping)).

### InstanceKey

A 128-bit structural signature of a prim's composition graph. Two prims with
equal keys have structurally identical arcs and produce identical child
namespaces, so they can share a prototype (AOUSD Core Spec §11.3.3). The
counterpart to OpenUSD's [`PcpInstanceKey`](#instancing-pcpinstancekey).

```cpp
struct InstanceKey {
  uint64_t hash_lo{0}, hash_hi{0};
  bool operator==(const InstanceKey &) const;
  bool operator!=(const InstanceKey &) const;
  bool is_valid() const;   // non-zero
};

struct InstanceKeyHasher { size_t operator()(const InstanceKey &) const; };

// Returns an invalid key if the prim is not instanceable.
InstanceKey ComputeInstanceKey(const PrimIndex &index,
                               const std::vector<LayerStackEntry> &stacks,
                               const std::vector<std::string> &path_table);
```

> The DAG engine's `InstanceKey` is distinct from the `Stage`/`PrimSpec`-level
> `lightusd::InstanceKey` in `src/core/instance-key.hh` used by
> `Stage::BuildInstancePrototypes()`. Both are 128-bit hashes of the same
> structural inputs (type name + composition arcs + variant selections); the
> core one uses SpookyHash. See [composition.md](composition.md#instancing).

### DeferredPayloadInfo

Context retained for a payload skipped during initial composition so it can be
loaded later — the lightusd analogue of PCP's
[payload inclusion set](#payloads-and-dynamic-payloads):

```cpp
struct DeferredPayloadInfo {
  Path prim_path;       // prim that owns the payload arc
  Payload payload;      // the payload descriptor
  uint16_t node_idx;    // node index in the PrimIndex
  std::string current_working_path;            // resolver context at discovery
  std::vector<std::string> asset_search_paths;
};
```

### CompositionGraphOptions

```cpp
struct CompositionGraphOptions {
  // Return true to load a payload, false to defer. nullptr => load eagerly.
  std::function<bool(const Path &prim_path, const Payload &payload)> payload_policy;

  uint32_t max_depth{256};        // recursion limit
  bool detect_instances{true};    // instancing detection during composition
  std::unordered_map<std::string, FileFormatHandler> fileformats;
  size_t max_memory_mb{16384};
  bool error_when_asset_not_found{false};
  bool error_when_unsupported_fileformat{false};
};
```

`payload_policy` is the lightusd counterpart to PCP's payload predicate /
inclusion set.

### CompositionGraph

The top-level engine. Move-only (it owns the layers loaded during composition).
Loosely corresponds to OpenUSD's [`PcpCache`](#data-model), though it does not
persist results across edits or perform PCP-style change tracking.

```cpp
class CompositionGraph {
 public:
  // Compose a scene from a root layer (sublayers should already be composed).
  static nonstd::expected<CompositionGraph, std::string> Compose(
      AssetResolutionResolver &resolver, const Layer &root_layer,
      const CompositionGraphOptions &options = {});

  // -- Query --
  const PrimIndex *GetPrimIndex(const Path &prim_path) const;   // nullptr if absent
  std::vector<Path> GetAllPrimPaths() const;
  size_t GetPrototypeCount() const;
  std::vector<Path> GetInstancesForPrototype(size_t prototype_idx) const;
  const PrimIndex *GetPrototypePrimIndex(size_t prototype_idx) const;

  // -- Lazy payloads --
  nonstd::expected<bool, std::string> LoadPayload(
      const Path &prim_path, AssetResolutionResolver &resolver);
  nonstd::expected<bool, std::string> UnloadPayload(const Path &prim_path);
  std::vector<Path> GetDeferredPayloadPaths() const;
  bool HasDeferredPayload(const Path &prim_path) const;

  // -- Stage construction --
  // Lowers the DAG back to a Stage via composition-reconstruct.cc; result is
  // structurally identical to the CompositeAllArcs + LayerToStage pipeline.
  bool BuildStage(Stage *stage, std::string *warn, std::string *err) const;

  // -- Diagnostics --
  size_t EstimateMemoryUsage() const;
  std::string DumpToString() const;
};
```

`Compose()` expects the **L** phase (sublayers) to already be flattened into
`root_layer`; use `CompositeSublayers()` from `src/composition.hh` first.

### PrimIndexBuilder (internal)

`CompositionGraph::Compose()` builds each `PrimIndex` with an internal
`PrimIndexBuilder` driven by a `std::priority_queue<CompositionTask>` — the
lightusd analogue of PCP's
[`Pcp_PrimIndexer`](#the-build-algorithm-pcp_primindexer-task-queue):

```cpp
struct CompositionTask {
  TaskType type{TaskType::EvalRootNode};
  uint16_t node_idx;     // node that spawned this task
  uint8_t  list_op_idx;  // ListOp index
  uint8_t  item_idx;     // item within ListOp
  bool operator<(const CompositionTask &) const;  // lower TaskType = higher prio
};
```

The builder scans a `PrimSpec`'s metadata for arcs (`ScanArcsAndEnqueueTasks`),
enqueues `Eval*` tasks, and processes them in `TaskType` order. It tracks two
cycle-detection sets — `(layer_id, prim_path)` pairs for references/payloads and
per-path sets for inherits/specializes (the counterpart to PCP's
[`PcpSiteTracker`/stack frames](#ancestral-opinions-and-cycle-detection)) —
collects variant opinions across phases, then computes the strength order and
culls inert nodes. This is the DAG-engine analogue of the phase-by-phase flow in
`CompositeAllArcs()`.

### Usage

```cpp
#include "composition-graph.hh"
#include "composition.hh"   // for CompositeSublayers

using namespace lightusd;

// 1) Flatten sublayers (L) into the root layer first.
Layer root = /* loaded + CompositeSublayers(...) */;

// 2) Build the DAG.
CompositionGraphOptions opts;
opts.detect_instances = true;
auto result = CompositionGraph::Compose(resolver, root, opts);
if (!result) { /* result.error() */ }

CompositionGraph graph = std::move(*result);

// 3) Inspect a prim's composition graph.
if (const PrimIndex *idx = graph.GetPrimIndex(Path("/Model/Geom"))) {
  for (uint16_t n : idx->GetStrengthOrder()) {
    const composition_graph::CompNode &node = idx->GetNode(n);
    // node.arc_type, node.has_specs(), ...
  }
}

// 4) Lower to a Stage (reuses composition-reconstruct.cc).
Stage stage;
std::string warn, err;
graph.BuildStage(&stage, &warn, &err);
```

Lazy payloads:

```cpp
for (const Path &p : graph.GetDeferredPayloadPaths()) {
  graph.LoadPayload(p, resolver);   // recomposes the affected prim
}
```

### Status

The DAG engine is a parallel implementation alongside the default
layer-flattening pipeline (see the header comment in `composition-graph.hh`).
`BuildStage()` is designed to produce a `Stage` structurally identical to the
`CompositeAllArcs()` + `LayerToStage()` path. For the conceptual model, LIVRPS
correctness analysis, instancing, and variants, see
[composition.md](composition.md).

---

## The lightusd Composition Cache (`pcp::Cache`)

`lightusd::pcp::Cache` (`src/pcp/cache.hh`, namespace `lightusd::pcp`) is the
cached, lazy, partial-composition counterpart to OpenUSD's `PcpCache`. It is a
thin layer *on top of* the `composition_graph` engine — it reuses `PrimIndex`,
`CompNode`, and `PrimIndexBuilder` unchanged — and adds the four things the eager
`CompositionGraph` lacks. It is the closest lightusd analogue to `PcpCache`'s
"compute on demand and remember" model.

**What it adds over `CompositionGraph`:**

1. **Lazy / partial composition.** `ComputePrimIndex(path)` builds and caches the
   `PrimIndex` for *just that prim* on demand, instead of eagerly composing every
   prim up front. You only pay for the prims you query.
2. **Parse-once layer-asset cache** (`pcp::LayerRegistry`, `src/pcp/layer-registry.hh`).
   A referenced / sublayer / payload file is resolved and parsed **once**, keyed by
   its resolved asset path, and shared across every prim that uses it. (The eager
   `CompositionGraph` re-parses each file per use.)
3. **Dependency tracking + invalidation.** As each `PrimIndex` is built, the cache
   records every opinion **site** — a `(resolved-layer-id, prim-path)` pair derived
   from each node's `layer_stack_idx`/`site_path_idx` — in a reverse map. `Invalidate(path)`
   drops the index for `path`, its namespace descendants, **and** every cached index
   that read a site at/under `path`; `InvalidateLayer(id)` does the same per layer.
4. **Threading.** Single-threaded by default (and the only path on wasm).
   `PrewarmPrimIndices()` / `BuildStage()` optionally build independent prim indices
   in parallel when `CacheOptions::num_threads != 1` and `LIGHTUSD_ENABLE_THREAD` is
   compiled in — each worker builds into its own context (no shared mutable state),
   with only the `LayerRegistry` serialized, then a deterministic merge folds the
   results in input order so single- and multi-threaded runs are identical.

**Decoupling.** To let the cache drive per-prim builds, `PrimIndexBuilder` was
refactored to depend on a `composition_graph::CompositionContext` (shared tables +
resolver + options + a `load_layer` seam) rather than directly on `CompositionGraph`.
Both engines now drive the same builder. Each cached prim owns its own
`CompositionContext` tables; the heavy parsed layers are shared via the registry.
The whole `Cache` lives behind a heap-pinned `Impl` (pimpl) so it is cheaply movable
without disturbing the cached indices' internal pointers.

The result cache, the reverse-dependency map, and the `LayerRegistry` use the
in-tree open-addressing robin-hood map `lightusd::HashMap` (`src/tiny-hashmap.hh`)
rather than `std::unordered_map` — exceptions-free, and its `find()` is move-safe
(it guards an empty table before masking). Thread-safety is provided externally
where needed: `LayerRegistry` serializes its load path with a mutex; the per-prim
maps are only mutated single-threaded (parallel workers build into private
contexts and a single-threaded barrier merges the results).

**API sketch** (`src/pcp/cache.hh`):

```cpp
using namespace lightusd;

// CompositeSublayers (the L phase) is run internally by Open().
auto r = pcp::Cache::Open(resolver, root_layer, pcp::CacheOptions{});
pcp::Cache cache = std::move(*r);

// Partial: compose only what you need (cached; stable pointer).
if (const auto *idx = cache.ComputePrimIndex(Path("/Model/Geom"), &warn, &err)) {
  for (uint16_t n : idx->GetStrengthOrder()) { /* idx->GetNode(n) ... */ }
}

// Lazy payloads, targeted invalidation.
cache.LoadPayload(Path("/Model"), &warn, &err);
cache.Invalidate(Path("/Model"));   // drops /Model + dependents only

// Full materialization (reuses composition-reconstruct.cc; structurally
// identical to CompositionGraph::BuildStage / CompositeAllArcs + LayerToStage).
Stage stage;
cache.BuildStage(&stage, &warn, &err);
```

**Scope (first cut).** Lazy + caches + explicit invalidate. There is no
`PcpChanges`-style processor that *diffs* arbitrary scene edits, no separate
property index, and instancing detection is left to the eager path / `Stage`-level
`BuildInstancePrototypes()`. Built behind the `LIGHTUSD_WITH_PCP` CMake option (ON).

---

## PCP ↔ lightusd: Where They Diverge

The two engines share a data model, but the lightusd DAG engine is intentionally
narrower than OpenUSD PCP:

- **No full `PcpChanges` edit-diffing.** `CompositionGraph` composes once, and
  `pcp::Cache` adds a persistent per-prim cache with a site→index reverse-dependency
  map and explicit `Invalidate(path)` / `InvalidateLayer(id)`. Neither implements
  PCP's two-phase change processor that *derives* the minimal recomposition set from
  an arbitrary scene edit — invalidation in `pcp::Cache` is explicit, caller-driven.
- **No separate property index.** There is no `PcpPropertyIndex`; property
  opinions are resolved during reconstruction / value resolution.
- **Simpler namespace mapping.** `NamespaceMapping` is a prefix-remap with lazy
  composition (`MapExpr`), not a full invertible expression tree of mutable
  variables tuned for incremental relocates edits.
- **The flattening pipeline is the production path.** `CompositeAllArcs()`
  (composition.md) is the default; the DAG engine is opt-in/parallel and exists
  for opinion introspection, lazy payloads, and instance detection.

For a per-phase, LIVRPS-correctness comparison of lightusd against OpenUSD PCP,
including the relocate-positioning and specializes-nesting caveats, see the
comparison table and correctness analysis in
[composition.md](composition.md#comparison-table) and
[composition.md](composition.md#livrps-correctness-analysis-for-lightusd).

---

## References

### OpenUSD PCP source (`pxr/usd/pcp/`)

- `overview.dox` — what PCP is; the name; errors, dependencies, change processing, path translation.
- `cache.h` — `PcpCache`, the top-level entry point and result cache.
- `primIndex.h` / `primIndex.cpp` — `PcpPrimIndex` and `Pcp_BuildPrimIndex` / `Pcp_PrimIndexer` / `Task::Type` / the `_EvalNode*` / `_EvalImplied*` functions.
- `primIndex_Graph.h` — copy-on-write shared node pool; `Finalize()`.
- `primIndex_StackFrame.h` — recursion frames for ancestral opinions + cycle detection.
- `node.h`, `arc.h`, `types.h` — `PcpNodeRef`/`_Node`, `PcpArc`, `PcpArcType` / `PcpRangeType`.
- `layerStack.h`, `layerStackIdentifier.h`, `site.h` — layer stacks, identifiers, sites.
- `mapExpression.h`, `mapFunction.h`, `pathTranslation.h` — namespace mapping + path translation.
- `propertyIndex.h` — `PcpPropertyIndex`.
- `instanceKey.h`, `instancing.h` — instancing key and instanceability predicate.
- `errors.h` — `PcpErrorType` and error classes.
- `changes.h`, `dependencies.h`, `dependency.h` — change processing and dependency tracking.

### lightusd source

- `src/composition-graph.hh` / `.cc` — this (eager) engine; also `CompositionContext` + `PrimIndexBuilder` reused by `pcp::Cache`.
- `src/pcp/cache.hh` / `.cc` — `pcp::Cache`, the cached/lazy composition engine (+ `cache-impl.hh`, `cache-parallel.cc`).
- `src/pcp/layer-registry.hh` / `.cc` — parse-once, resolved-path-keyed layer cache.
- `src/composition.hh` / `.cc` — default flattening pipeline (`CompositeAllArcs`).
- `src/composition-reconstruct.cc` — PrimSpec → Prim/Stage lowering (shared by both paths).
- `src/core/instance-key.hh` — `Stage`-level `InstanceKey` (SpookyHash).
- `src/namespace-mapping.hh` — `NamespaceMapping` prefix remapping.

### AOUSD Core Specification v1.0.1 (2025-12-12)

- **Section 10 (Composition)** — §10.1 definition; §10.3 operators (10.3.1 sublayers, 10.3.2 references, 10.3.2.2 payloads, 10.3.2.3 inherits, 10.3.2.4 specializes, 10.3.2.5 variants, 10.3.2.6 relocates); §10.4 strength ordering / LIVERPS (§10.4.1 globally-weak specializes); §10.5 namespace path translation; §10.6 composition errors.
- **Section 11.3 (Stage Population)** — §11.3.1 ordered prim children, §11.3.2 ordered property children, §11.3.3 scene-graph instancing.

### Related docs

- [composition.md](composition.md) — the default flattening pipeline, the LIVRPS table, the LIVRPS-correctness analysis, the OpenUSD-vs-lightusd comparison table, instancing examples, and variants.

---

## lightusd::pcp — native composition

The core implementation (`src/`) is a standalone, low-dependency rewrite
of the original lightusd core (runtime type dispatch, flat index-based storage,
SBO `Value`). It does **not** link the main library, so the main `pcp::Cache`
(built on `lightusd::Layer` / `composition-graph.cc`) cannot be reused directly.
Instead, `src/pcp/` is a **native, standalone re-implementation of the same PCP
design** on the core's own types (C++14, only `src/` + STL + vendored
`nonstd/expected`).

### Design

- **Lazy per-prim composition.** `Cache::ComputePrimIndex(path)` builds and caches
  one prim's composition graph on demand; you pay only for prims you query.
  `BuildStage()` materializes the whole composed scene into a `lightusd::Stage`.
- **`PrimIndex` / `CompNode`** mirror the main engine: a per-prim DAG of
  strength-ordered sources. Site prim paths are interned into a shared table
  (`CompNode` stores a `uint32` index) and deduped across all cached indices;
  layers are shared by `shared_ptr` (parse-once).
- **Parse-once `LayerRegistry`** keyed by resolved path; `Cache::PreloadLayer`
  registers an in-memory layer under an identifier (embedding helper).
- **Dependency-aware invalidation.** `Invalidate(path)` / `InvalidateLayer(id)`
  drop exactly the cached indices that read the affected sites (reverse `Site`
  maps).

### Supported arcs — full LIVRPS + relocates + instancing

Strength order `Local > Inherit > Variant > Reference > Payload > Specialize`:

- **Sublayers (L)** — the root layer stack `[root, sublayers…]`.
- **References (R)** — internal + external (resolved via `AssetResolver`), nested,
  with namespace mapping and cycle detection.
- **Payloads (P)** — deferred via `CompositionOptions::payload_policy`;
  `LoadPayload` / `UnloadPayload` / `HasDeferredPayload` / `GetDeferredPayloadPaths`.
- **Inherits / Specializes (I/S)** — class arcs; specializes are globally weakest
  (collected to the tail). **Implied** class arcs propagate into every ancestor
  layer stack on the reference chain (root + intermediate), so an override on the
  class at any level composes.
- **Variants (V)** — selection grafts the chosen variant's inline opinions and/or
  its **content subtree** (a variant that adds child prims). Multi-selection
  (`variantSelections`) and **cross-source** selection (selection on a stronger
  source than the variantSet) are supported.
- **Relocates** — same-parent namespace rename in `BuildStage`.
- **Ancestral composition** — a descendant of a referenced prim composes even
  with no local spec.
- **Instancing** — `instanceable` prims with the same structural `InstanceKey`
  (type + arcs + variant selections) share a prototype; `BuildStage` materializes
  instances as proxies (`PrimSpecMeta::instance_prototype`) so subtrees are not
  duplicated, while `UsdPrim` child access transparently follows the prototype.
  API: `IsInstance` / `GetPrototype` / `GetInstancesForPrototype` / `PrototypeCount`.

### One-call helpers

- `pcp::ComposeStageFromFile(filename, resolver, &stage, options, …)`
- `pcp::ComposeStageFromLayer(root_layer, resolver, &stage, root_id, options, …)`

### Threading

Built with `-DLIGHTUSD_ENABLE_THREAD=ON`:

- The `LayerRegistry` is thread-safe and serializes the same-asset resolve + parse
  + publish path, so a referenced/payload/sublayer file is parsed exactly once.
- The **`Cache` itself is thread-safe**: every public entry point (`ComputePrimIndex`,
  `BuildStage`, payload load/unload, invalidation, and all instancing/index queries)
  is serialized by a per-cache `std::recursive_mutex` (the `NEXT_PCP_LOCK` macro,
  compiled out in non-threaded builds). Multiple threads may safely query/compose a
  shared cache concurrently; results are stable borrowed pointers.
- **Parallel per-prim index building.** `PrewarmPrimIndices` with `num_threads != 1`
  (and more than one path) first prefetches first-level reference/payload layers into
  the shared registry, then builds the per-prim indices **concurrently**: the batch is
  split into contiguous chunks and each worker runs the ordinary build on its **own
  private `Cache::Impl`** that *borrows the shared, parse-once `LayerRegistry`*. Because
  a worker touches only its private tables plus the internally-locked registry and
  read-only layer data, the heavy composition work runs **without any shared cache
  lock**. Custom `AssetResolver` callbacks and `payload_policy` callbacks must be
  thread-safe when used with parallel prewarm. A deterministic, input-order merge then folds each
  worker's `PrimIndex` into the cache — remapping the worker's private layer-stack /
  path-table indices onto the shared tables (sites use identifier strings, so no remap)
  and assigning instance prototypes **in input order**, so the composed result is
  byte-for-byte identical to a serial build (only the internal interning *index
  integers* may differ, which is unobservable). `BuildStage` (a single recursive tree
  walk) remains serial.

Verified ThreadSanitizer-clean: 8 threads × 1000 iterations querying a shared cache
(`test_concurrent_queries`) and a parallel batch build whose every index + instance
grouping matches the serial baseline (`test_parallel_build_matches_serial`). Default
builds are sequential and zero-overhead — the parallel path and lock macro compile out.

### Known limitations / future work

- `BuildStage` is still serial — only the batch `PrewarmPrimIndices` build is
  parallelized (see Threading). Parallelizing the `BuildStage` tree walk is future work.
- Worker `sources_cache` entries are not merged back (only the published `PrimIndex`
  is), so a later query for a *non-prewarmed* path recomputes its sources — correct,
  just not pre-warmed.
- `CompNode` map-expression interning; variant-child relocate interplay.

### Tests

`tests/core/test_pcp.cc` (built with `-DLIGHTUSD_BUILD_TESTS=ON`) covers each
arc, ancestral composition, deferred payloads, instancing + proxies, relocates,
cross-source variants, implied class propagation (incl. intermediate stacks),
sublayer stack composition/cycle/depth errors, node-overflow rejection, the parallel
batch build vs. a serial baseline, concurrent shared-cache queries (TSan), and the
one-call helpers. `tests/core/test_pcp_parallel.cc` adds dedicated
minimal/complex/stress coverage for the parallel build on *synthetically generated*
scenes (up to ~900 paths × 8 threads × repeated rounds), plus same-asset parse-once
contention coverage, asserting every index and prototype grouping matches the serial
baseline and is stable across runs (TSan-clean). Both executables are registered as
`next_test_pcp` and `next_test_pcp_parallel` in CTest.
