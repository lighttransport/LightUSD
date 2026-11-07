import UsdSpec.Composition.Strength
import UsdSpec.ListOp
import Mathlib.Data.Finset.Card

/-!
# A toy executable prim-index builder (AOUSD USD Core Specification v1.1.0, §10)

SPEC: core_spec.md L4217-4243 (`compose_helper` / `compose` outline),
L4244-4875 (composition operators), L5010-5077 (strength ordering),
examples L5081-5374.

`UsdSpec.Strength` checks the six worked examples against *hand-built* prim
index trees. Here the tree is *computed* from toy layers:

* a layer has a name, sublayers and prim specs; a prim spec carries
  `references`/`payloads` (`(layer?, path)`, `none` = internal), `inherits`,
  `specializes`, `variantSets` and variant `selections`; explicit/composable
  edits of arc fields merge across the layer stack through the proved ListOp
  model, with absent opinions distinct from explicit empty lists;
* `composeTree` follows the outline: compose the parent path, append the
  child name to every node ("ancestral opinions", L4223-4228), then add the
  arcs authored at the child path on every node (direct arcs, namespace depth =
  the child's depth);
* sublayer stacks use a terminating branch-local DFS without a depth cutoff;
  missing and cyclic sublayer edges produce diagnostics and are ignored;
* prim arcs expand recursively at their target sites, with a finite active-site
  set and explicit cycle diagnostics;
* implied classes (L5234-5282, L5315-5374): every inherit / specialize subtree
  below a reference or payload node is copied, mapped through the arc's path
  translation (`target ↦ referencing prim`, identity elsewhere), to the
  referencing node's layer stack with `implied := true`, bottom-up so that the
  copies propagate to the root;
* variant selections are resolved in two passes: the tree without variants
  gives a strength order, the strongest authored selection along it is used
  (L4782-4823; selection computation L4806-4815);
* `Strength.strengthOrder` orders the nodes; `compose` expands every node's
  layer stack and keeps the layers that have a spec (L4232-4242).

Simplifications (toy, documented): implied class nodes do not expand their own
authored arcs; relocates, selective payload loading rules, and variant-set-name
list editing remain out of scope. Raw authored-site arc cycles are detected;
variant-context and relocation-origin cycles need the richer model.
Global LoadAll/LoadNone is supported by composeWithLoadPolicy.
Offset/asset resolution remains abstract.

SPEC-ISSUE: LEAN-521 records the defects of the outline pseudocode;
SPEC-ISSUE: LEAN-522 records that the Example 6 tables list sites that
`compose` (which keeps only layers with a spec) cannot produce.
-/

namespace UsdSpec.Composition.PrimIndex

open UsdSpec.Strength

/-- Path elements: prim names and variant selections. -/
inductive PathElem
  | prim (name : String)
  | variant (set sel : String)
  deriving DecidableEq, Repr

abbrev Path := List PathElem
abbrev SiteKey := String × Path

/-- Text form: `/A/B{v=a}C`. -/
def renderAux : Bool → Path → String
  | _, [] => ""
  | afterVariant, .prim n :: rest => (if afterVariant then n else "/" ++ n) ++ renderAux false rest
  | _, .variant s v :: rest => "{" ++ s ++ "=" ++ v ++ "}" ++ renderAux true rest

def render : Path → String
  | [] => "/"
  | p => renderAux false p

/-- Namespace depth: number of prim elements (L5054-5056). -/
def depth (p : Path) : Nat := (p.filter fun e => match e with | .prim _ => true | _ => false).length

structure PrimSpec where
  path : Path
  references : List (Option String × Path) := []
  payloads : List (Option String × Path) := []
  referenceEdits : Option (ListOp (Option String × Path)) := none
  payloadEdits : Option (ListOp (Option String × Path)) := none
  inheritEdits : Option (ListOp Path) := none
  specializeEdits : Option (ListOp Path) := none
  inherits : List Path := []
  specializes : List Path := []
  variantSets : List String := []
  selections : List (String × String) := []
  deriving DecidableEq, Repr

structure Layer where
  name : String
  sublayers : List String := []
  specs : List PrimSpec := []
  deriving DecidableEq, Repr

abbrev Layers := List Layer

def findLayer (ls : Layers) (n : String) : Option Layer := ls.find? (·.name == n)

/-- Sublayer errors required by Core L4264-4269; the offending edge is ignored. -/
inductive LayerStackIssue
  | missing (layer : String)
  | cycle (layer : String)
  deriving DecidableEq, Repr

structure LayerStackResult where
  layers : List String
  issues : List LayerStackIssue
  deriving DecidableEq, Repr

/-- Finite branch-local DFS. Each recursive call removes the current layer
from the available set, so termination follows from its cardinality rather
than arbitrary fuel. Sibling branches share their original available set:
shared diamond sublayers are preserved, not globally deduplicated. -/
def layerStackVisit (ls : Layers) (available : Finset String) (root : String) : LayerStackResult :=
  match findLayer ls root with
  | none => ⟨[], [.missing root]⟩
  | some layer =>
    if _h : root ∈ available then
      let children := layer.sublayers.map fun child => layerStackVisit ls (available.erase root) child
      ⟨root :: children.flatMap (·.layers), children.flatMap (·.issues)⟩
    else ⟨[], [.cycle root]⟩
termination_by available.card
decreasing_by exact Finset.card_erase_lt_of_mem _h

/-- Ordered layer stack and errors, with no depth cutoff. -/
def layerStackChecked (ls : Layers) (root : String) : LayerStackResult :=
  layerStackVisit ls (ls.map (·.name)).toFinset root

/-- The layer stack rooted at n, strongest first (L4250-4270). The legacy
fuel argument remains for API compatibility; sublayer traversal is unbounded. -/
def layerStack (ls : Layers) (_fuel : Nat) (root : String) : List String :=
  (layerStackChecked ls root).layers

theorem layerStack_fuel_independent (ls : Layers) (n m : Nat) (root : String) :
    layerStack ls n root = layerStack ls m root := rfl

theorem layerStackVisit_missing (ls : Layers) (available : Finset String) (root : String)
    (h : findLayer ls root = none) :
    layerStackVisit ls available root = ⟨[], [.missing root]⟩ := by
  rw [layerStackVisit, h]

theorem layerStackVisit_cycle (ls : Layers) (available : Finset String) (root : String)
    (layer : Layer) (hfind : findLayer ls root = some layer) (h : root ∉ available) :
    layerStackVisit ls available root = ⟨[], [.cycle root]⟩ := by
  simp [layerStackVisit, hfind, h]

/-- Every emitted layer was available on entry to this branch. In particular,
no excluded ancestor can reappear below the edge that excluded it. -/
theorem layerStackVisit_mem (ls : Layers) (available : Finset String) (root name : String)
    (hx : name ∈ (layerStackVisit ls available root).layers) : name ∈ available := by
  rw [layerStackVisit] at hx
  cases hfind : findLayer ls root with
  | none => simp only [hfind, List.not_mem_nil] at hx
  | some layer =>
    rw [hfind] at hx
    by_cases h : root ∈ available
    · simp only [dite_eq_left h, List.flatMap_map, List.mem_cons, List.mem_flatMap] at hx
      rcases hx with rfl | ⟨child, _, hc⟩
      · exact h
      · exact Finset.mem_of_mem_erase (layerStackVisit_mem ls (available.erase root) child name hc)
    · simp only [dite_eq_right h, List.not_mem_nil] at hx
termination_by available.card
decreasing_by exact Finset.card_erase_lt_of_mem h

theorem layerStackVisit_excludes (ls : Layers) (available : Finset String)
    (root name : String) (h : name ∉ available) :
    name ∉ (layerStackVisit ls available root).layers := by
  intro hx
  exact h (layerStackVisit_mem ls available root name hx)

theorem layerStack_mem_known (ls : Layers) (fuel : Nat) (root name : String)
    (h : name ∈ layerStack ls fuel root) : name ∈ ls.map (·.name) := by
  have hmem : name ∈ (ls.map Layer.name).toFinset :=
    layerStackVisit_mem ls (ls.map Layer.name).toFinset root name
      (by simpa only [layerStack, layerStackChecked] using h)
  exact List.mem_toFinset.mp hmem

def specsIn (ls : Layers) (layer : String) (p : Path) : List PrimSpec :=
  ((findLayer ls layer).map (·.specs) |>.getD []).filter (·.path == p)

def stackSpecs (ls : Layers) (fuel : Nat) (root : String) (p : Path) : List PrimSpec :=
  (layerStack ls fuel root).flatMap fun l => specsIn ls l p

/-- Internal tree with the original crossed-arc trail. Namespace extension
changes the current site but retains this trail for ancestral cycle checks. -/
inductive INode
  | mk (layer : String) (path : Path) (arc : Option ArcInfo)
      (children : List INode) (trail : List SiteKey)
  deriving Repr

namespace INode
def layer : INode → String | .mk l _ _ _ _ => l
def path : INode → Path | .mk _ p _ _ _ => p
def arc : INode → Option ArcInfo | .mk _ _ a _ _ => a
def children : INode → List INode | .mk _ _ _ cs _ => cs
def trail : INode → List SiteKey | .mk _ _ _ _ seen => seen
end INode

abbrev Sel := String → Option String

/-- Indexed list. -/
def enum {α : Type} (l : List α) : List (Nat × α) := (List.range l.length).zip l

/-- Absent opinions contribute no operation. A nonempty legacy DSL list is
an explicit opinion; `some (.explicit [])` represents an authored clear. -/
def arcOpinion {α : Type} (legacy : List α) (edits : Option (ListOp α)) :
    Option (ListOp α) :=
  match edits with
  | some op => some op
  | none => if legacy.isEmpty then none else some (.explicit legacy)

/-- Arc-list opinions are strongest first, just like stackSpecs. -/
def mergeArcOpinions {α : Type} [DecidableEq α] (ops : List (ListOp α)) : List α :=
  ListOp.iterate (ListOp.chain ops)

/-- This executable merge is the normative weak-to-strong application, not
concatenation of authored lists. -/
theorem mergeArcOpinions_eq_foldr {α : Type} [DecidableEq α] (ops : List (ListOp α)) :
    mergeArcOpinions ops = ops.foldr ListOp.apply [] := by
  rw [mergeArcOpinions, ← ListOp.apply_nil, ListOp.apply_chain]

/-- An authored explicit clear hides all weaker arcs. -/
theorem mergeArcOpinions_clear {α : Type} [DecidableEq α] (rest : List (ListOp α)) :
    mergeArcOpinions (.explicit [] :: rest) = [] := rfl

/-- Strong explicit lists replace rather than concatenate weaker opinions. -/
theorem mergeArcOpinions_explicit {α : Type} [DecidableEq α]
    (items : List α) (rest : List (ListOp α)) :
    mergeArcOpinions (.explicit items :: rest) = items := rfl

/-- Arcs authored at a site, with field list editing across its layer stack.
Variant-set-name list editing remains separate from these four arc fields;
names are unioned across layers. -/
def authoredArcs (ls : Layers) (fuel : Nat) (sel : Sel) (layer : String) (p : Path) :
    List (ArcInfo × String × Path) :=
  let ss := stackSpecs ls fuel layer p
  let d := depth p
  let inh := enum (mergeArcOpinions (ss.filterMap fun sp => arcOpinion sp.inherits sp.inheritEdits))
  -- Variant-set names compose as a union across the layer stack: a set
  -- declared in several layers yields one variant arc (first occurrence wins).
  let vs := enum (ss.flatMap (·.variantSets)).eraseDups
  let refs := enum (mergeArcOpinions (ss.filterMap fun sp => arcOpinion sp.references sp.referenceEdits))
  let pays := enum (mergeArcOpinions (ss.filterMap fun sp => arcOpinion sp.payloads sp.payloadEdits))
  let sps := enum (mergeArcOpinions (ss.filterMap fun sp => arcOpinion sp.specializes sp.specializeEdits))
  inh.map (fun (i, t) => (⟨.inherit, d, false, i⟩, layer, t)) ++
  vs.filterMap (fun (i, s) => (sel s).map fun v => (⟨.variant, d, false, i⟩, layer, p ++ [.variant s v])) ++
  refs.map (fun (i, (l, t)) => (⟨.reference, d, false, i⟩, l.getD layer, t)) ++
  pays.map (fun (i, (l, t)) => (⟨.payload, d, false, i⟩, l.getD layer, t)) ++
  sps.map (fun (i, t) => (⟨.specialize, d, false, i⟩, layer, t))

/-- All possible active raw source sites: every known stack root paired
with every authored spec path. Unknown or unauthored targets are leaves. -/
def knownSites (ls : Layers) : Finset SiteKey :=
  (ls.flatMap fun layer => (ls.flatMap (·.specs)).map fun spec =>
    (layer.name, spec.path)).toFinset

structure ArcCycle where
  source : Option SiteKey
  target : SiteKey
  kind : Option ArcKind
  deriving DecidableEq, Repr

structure ArcExpansion where
  node : Option INode
  cycles : List ArcCycle
  deriving Repr

/-- Re-entering a crossed site, or targeting an ancestor of a crossed source,
closes an authored-arc cycle. Names use path-element prefixes, not byte prefixes. -/
def closesArcCycle (trail : List SiteKey) (layer : String) (path : Path) : Bool :=
  trail.any fun old => old.1 == layer && path.isPrefixOf old.2

/-- Expand the authored-arc graph with explicit cycle diagnostics. Every active
recursive step removes its source from a finite set. Inactive targets are
retained as leaf nodes, as in the outline. No fuel cutoff is used. Namespace
and variant-context completeness remain separate from this raw-site model. -/
def expandAvailable (ls : Layers) (sel : Sel) (available : Finset SiteKey)
    (trail : List SiteKey) (layer : String) (path : Path) (arc : Option ArcInfo) : ArcExpansion :=
  if closesArcCycle trail layer path then
    ⟨none, [⟨trail.head?, (layer, path), arc.map (·.kind)⟩]⟩
  else if (stackSpecs ls 0 layer path).isEmpty then
    ⟨some (.mk layer path arc [] ((layer, path) :: trail)), []⟩
  else if h : (layer, path) ∈ available then
    let children := (authoredArcs ls 0 sel layer path).map fun (info, targetLayer, targetPath) =>
      expandAvailable ls sel (available.erase (layer, path)) ((layer, path) :: trail)
        targetLayer targetPath (some info)
    ⟨some (.mk layer path arc (children.filterMap (·.node)) ((layer, path) :: trail)),
      children.flatMap (·.cycles)⟩
  else ⟨none, [⟨trail.head?, (layer, path), arc.map (·.kind)⟩]⟩
termination_by available.card
decreasing_by exact Finset.card_erase_lt_of_mem h

/-- Public graph expansion and diagnostics. -/
def expandChecked (ls : Layers) (sel : Sel) (layer : String) (path : Path)
    (arc : Option ArcInfo) : ArcExpansion :=
  expandAvailable ls sel (knownSites ls) [] layer path arc

/-- Legacy entry point; fuel no longer limits authored-arc expansion. -/
def expand (ls : Layers) (sel : Sel) (_fuel : Nat) (layer : String)
    (path : Path) (arc : Option ArcInfo) : INode :=
  (expandChecked ls sel layer path arc).node.getD (.mk layer path arc [] [])

/-- The retained compatibility parameter cannot truncate authored-arc
expansion or change its cycle diagnostics. -/
theorem expand_fuel_independent (ls : Layers) (sel : Sel) (n m : Nat)
    (layer : String) (path : Path) (arc : Option ArcInfo) :
    expand ls sel n layer path arc = expand ls sel m layer path arc := rfl

mutual
/-- Structural namespace extension preserves the original cycle trail. -/
def extendTree (e : PathElem) : INode → INode
  | .mk l p a cs trail => .mk l (p ++ [e]) a (extendTrees e cs) trail
def extendTrees (e : PathElem) : List INode → List INode
  | [] => []
  | c :: cs => extendTree e c :: extendTrees e cs
end

def extend (e : PathElem) (_fuel : Nat) (node : INode) : INode := extendTree e node

mutual
/-- Add direct arcs to all inherited source nodes. Both original crossed sites
and the current namespace context seed each new expansion's cycle trail. -/
def addDirectTree (ls : Layers) (sel : Sel) (context : List SiteKey) : INode → INode
  | .mk l p a cs trail =>
    let seen := (l, p) :: (trail ++ context)
    let available := knownSites ls \ seen.toFinset
    let direct := (authoredArcs ls 0 sel l p).filterMap fun (info, targetLayer, targetPath) =>
      (expandAvailable ls sel available seen targetLayer targetPath (some info)).node
    .mk l p a (addDirectTrees ls sel ((l, p) :: context) cs ++ direct) trail
def addDirectTrees (ls : Layers) (sel : Sel) (context : List SiteKey) : List INode → List INode
  | [] => []
  | c :: cs => addDirectTree ls sel context c :: addDirectTrees ls sel context cs
end

def addDirect (ls : Layers) (sel : Sel) (_fuel _walkFuel : Nat) (node : INode) : INode :=
  addDirectTree ls sel [] node

theorem addDirect_fuel_independent (ls : Layers) (sel : Sel) (n m a b : Nat)
    (node : INode) :
    addDirect ls sel n a node = addDirect ls sel m b node := rfl

mutual
/-- Diagnostic-preserving form of direct-arc expansion. The tree matches
`addDirectTree`; rejected cyclic edges are returned to the caller. -/
def addDirectTreeChecked (ls : Layers) (sel : Sel) (context : List SiteKey) :
    INode → INode × List ArcCycle
  | .mk l p a cs trail =>
    let seen := (l, p) :: (trail ++ context)
    let available := knownSites ls \ seen.toFinset
    let (oldChildren, oldIssues) := addDirectTreesChecked ls sel ((l, p) :: context) cs
    let expanded := (authoredArcs ls 0 sel l p).map fun (info, targetLayer, targetPath) =>
      expandAvailable ls sel available seen targetLayer targetPath (some info)
    (.mk l p a (oldChildren ++ expanded.filterMap (·.node)) trail,
      oldIssues ++ expanded.flatMap (·.cycles))
def addDirectTreesChecked (ls : Layers) (sel : Sel) (context : List SiteKey) :
    List INode → List INode × List ArcCycle
  | [] => ([], [])
  | c :: cs =>
    let (head, headIssues) := addDirectTreeChecked ls sel context c
    let (tail, tailIssues) := addDirectTreesChecked ls sel context cs
    (head :: tail, headIssues ++ tailIssues)
end

/-- Fuel-free checked direct expansion for clients that need diagnostics. -/
def addDirectChecked (ls : Layers) (sel : Sel) (node : INode) : INode × List ArcCycle :=
  addDirectTreeChecked ls sel [] node

def isClassArc (a : Option ArcInfo) : Bool :=
  match a with
  | some ai => ai.kind == .inherit || ai.kind == .specialize
  | none => false

def isRefArc (a : Option ArcInfo) : Bool :=
  match a with
  | some ai => ai.kind == .reference || ai.kind == .payload
  | none => false

/-- Path translation of a reference arc `src ↦ dst`, identity outside `src`. -/
def mapPath (src dst p : Path) : Path :=
  if src.isPrefixOf p then dst ++ p.drop src.length else p

mutual
/-- Implied class copies remain structural; expansion of their own newly
expressed authored arcs is still a separate model limitation. -/
def impliedCopyTree (l : String) (src dst : Path) : INode → INode
  | .mk _ p a cs trail =>
    .mk l (mapPath src dst p) (a.map fun ai => { ai with implied := true })
      (impliedCopyTrees l src dst cs) trail
def impliedCopyTrees (l : String) (src dst : Path) : List INode → List INode
  | [] => []
  | c :: cs => if isClassArc c.arc then
      impliedCopyTree l src dst c :: impliedCopyTrees l src dst cs
    else impliedCopyTrees l src dst cs
end

def impliedCopy (l : String) (src dst : Path) (_fuel : Nat) (node : INode) : INode :=
  impliedCopyTree l src dst node

def sameSite (a b : INode) : Bool := a.layer == b.layer && a.path == b.path

mutual
/-- Bottom-up implied-class propagation without a tree-depth cutoff. -/
def propagateTree : INode → INode
  | .mk l p a cs trail =>
    let cs' := propagateTrees cs
    let implied := cs'.flatMap fun c =>
      if isRefArc c.arc then
        (c.children.filter (isClassArc ·.arc)).map (impliedCopyTree l c.path p)
      else []
    let fresh := implied.foldl (fun acc n => if acc.any (sameSite n) then acc else acc ++ [n]) cs'
    .mk l p a fresh trail
def propagateTrees : List INode → List INode
  | [] => []
  | c :: cs => propagateTree c :: propagateTrees cs
end

def propagate (_fuel : Nat) (node : INode) : INode := propagateTree node

/-- `compose_helper` (L4218-4232) as a tree. -/
def composeTreeAux (ls : Layers) (root : String) (sel : Sel) (fuel : Nat) : Path → INode
  | [] => .mk root [] none [] []
  | e :: rest =>
    -- paths are built root-first; recursion on the reversed path
    addDirect ls sel fuel fuel (extend e fuel (composeTreeAux ls root sel fuel rest))

def composeTreeAuxChecked (ls : Layers) (root : String) (sel : Sel) :
    Path → INode × List ArcCycle
  | [] => (.mk root [] none [] [], [])
  | e :: rest =>
    let (base, inheritedIssues) := composeTreeAuxChecked ls root sel rest
    let (tree, directIssues) := addDirectChecked ls sel (extendTree e base)
    (tree, inheritedIssues ++ directIssues)

def composeTreeWith (ls : Layers) (root : String) (sel : Sel) (fuel : Nat) (p : Path) : INode :=
  propagate fuel (composeTreeAux ls root sel fuel p.reverse)

def composeTreeWithChecked (ls : Layers) (root : String) (sel : Sel) (p : Path) :
    INode × List ArcCycle :=
  let (tree, issues) := composeTreeAuxChecked ls root sel p.reverse
  (propagateTree tree, issues)

theorem composeTreeAux_fuel_independent (ls : Layers) (root : String) (sel : Sel)
    (n m : Nat) (p : Path) : composeTreeAux ls root sel n p =
      composeTreeAux ls root sel m p := by
  induction p with
  | nil => rfl
  | cons e rest ih =>
      simp [composeTreeAux, addDirect, extend, ih]

theorem composeTreeWith_fuel_independent (ls : Layers) (root : String) (sel : Sel)
    (n m : Nat) (p : Path) :
    composeTreeWith ls root sel n p = composeTreeWith ls root sel m p := by
  unfold composeTreeWith
  rw [composeTreeAux_fuel_independent ls root sel n m p.reverse]
  rfl

mutual
def toNodeTree : INode → Node
  | .mk l p a cs _ => .mk ⟨l, render p⟩ a (toNodeTrees cs)
def toNodeTrees : List INode → List Node
  | [] => []
  | c :: cs => toNodeTree c :: toNodeTrees cs
end

def toNode (_fuel : Nat) (node : INode) : Node := toNodeTree node

mutual
/-- Complete preorder of source sites, with no depth truncation. -/
def sitesTree : INode → List SiteKey
  | .mk l p _ cs _ => (l, p) :: sitesTrees cs
def sitesTrees : List INode → List SiteKey
  | [] => []
  | c :: cs => sitesTree c ++ sitesTrees cs
end

def sitesI (_fuel : Nat) (node : INode) : List SiteKey := sitesTree node

/-- Strongest authored selection along a strength-ordered site list. -/
def selFrom (ls : Layers) (fuel : Nat) (sites : List (String × Path)) : Sel := fun s =>
  (sites.flatMap fun (l, p) => (stackSpecs ls fuel l p).flatMap (·.selections)).findSome? fun (k, v) =>
    if k == s then some v else none

theorem selFrom_fuel_independent (ls : Layers) (n m : Nat)
    (sites : List (String × Path)) : selFrom ls n sites = selFrom ls m sites := by
  funext s
  simp [selFrom, stackSpecs, layerStack]

/-- Sites of an internal tree in strength order (via `Strength.strengthOrder` on
the rendered tree, then back to paths by lookup). -/
def orderedSites (fuel : Nat) (t : INode) : List (String × Path) :=
  let all := sitesI fuel t
  (strengthOrder (toNode fuel t)).filterMap fun s => all.find? fun (l, p) => l == s.layer && render p == s.path

theorem orderedSites_fuel_independent (ls : Layers) (root : String) (sel : Sel)
    (n m : Nat) (p : Path) :
    orderedSites n (composeTreeWith ls root sel n p) =
      orderedSites m (composeTreeWith ls root sel m p) := by
  rw [composeTreeWith_fuel_independent ls root sel n m p]
  simp [orderedSites, sitesI, toNode]

/-- The prim index: two passes for variant selections. -/
def composePrimIndex (ls : Layers) (root : String) (fuel : Nat) (p : Path) : Node :=
  let t₀ := composeTreeWith ls root (fun _ => none) fuel p
  let sel := selFrom ls fuel (orderedSites fuel t₀)
  toNode fuel (composeTreeWith ls root sel fuel p)

/-- Two-pass prim-index construction with all skipped authored-arc cycles
reported to the caller. The second pass supplies the final tree/diagnostics. -/
def composePrimIndexChecked (ls : Layers) (root : String) (p : Path) : Node × List ArcCycle :=
  let (firstTree, _) := composeTreeWithChecked ls root (fun _ => none) p
  let sel := selFrom ls 0 (orderedSites 0 firstTree)
  let (tree, issues) := composeTreeWithChecked ls root sel p
  (toNodeTree tree, issues)

/-- `compose` (L4234-4243): strength-ordered `(layer, path)` opinions, keeping
only layers of each node's layer stack that have a spec at the node's path. -/
def compose (ls : Layers) (root : String) (fuel : Nat) (p : Path) : List Site :=
  (strengthOrder (composePrimIndex ls root fuel p)).flatMap fun s =>
    (layerStack ls fuel s.layer).filterMap fun l =>
      if (((findLayer ls l).map (·.specs)).getD []).any (fun sp => render sp.path == s.path)
      then some ⟨l, s.path⟩ else none

/-- Global LoadNone removes payload opinions at every source; other arc and
selection opinions remain intact. Selective per-composed-prim loading is not
modeled by this all/none policy. -/
def withoutPayloads (ls : Layers) : Layers :=
  ls.map fun layer => { layer with specs := layer.specs.map fun spec =>
    { spec with payloads := [], payloadEdits := none } }

/-- Executable prim-stack composition under global LoadAll/LoadNone. -/
def composeWithLoadPolicy (loadPayloads : Bool) (ls : Layers) (root : String)
    (fuel : Nat) (p : Path) : List Site :=
  compose (if loadPayloads then ls else withoutPayloads ls) root fuel p

theorem composeWithLoadPolicy_all (ls : Layers) (root : String) (fuel : Nat) (p : Path) :
    composeWithLoadPolicy true ls root fuel p = compose ls root fuel p := rfl

theorem withoutPayloads_idempotent (ls : Layers) :
    withoutPayloads (withoutPayloads ls) = withoutPayloads ls := by
  simp [withoutPayloads, List.map_map]

/-! ## The six worked examples (L5081-5374) as toy layers -/

section Examples

def P (names : List String) : Path := names.map .prim
def sp (path : Path) : PrimSpec := { path }

/-- Example 1, L5081-5126. -/
def ex1L : Layers :=
  [ { name := "root.usda", sublayers := ["sub_1.usda", "sub_2.usda"], specs := [sp (P ["Root"])] },
    { name := "sub_1.usda", specs := [sp (P ["Root"])] },
    { name := "sub_2.usda", specs := [sp (P ["Root"])] } ]

/-- Example 2, L5127-5183. A `variantSet "v" = {…}` statement records `"v"` in
`variantSets` (as the text parser does). -/
def ex2L : Layers :=
  [ { name := "root.usda", specs :=
      [ sp (P ["Inherit"]),
        { path := P ["Root"], references := [(some "ref.usda", P ["Ref"])], variantSets := ["v"] },
        sp (P ["Root"] ++ [.variant "v" "a"]) ] },
    { name := "ref.usda", specs :=
      [ sp (P ["Inherit"]),
        { path := P ["Ref"], inherits := [P ["Inherit"]], variantSets := ["v"],
          selections := [("v", "a")] },
        sp (P ["Ref"] ++ [.variant "v" "a"]) ] } ]

/-- Example 3, L5184-5233. -/
def ex3L : Layers :=
  [ { name := "root.usda", specs :=
      [ { path := P ["Root"], references := [(some "root_ref.usda", P ["RootRef"])] },
        { path := P ["Root", "Child"], references := [(some "child_ref.usda", P ["ChildRef"])] } ] },
    { name := "root_ref.usda", specs := [sp (P ["RootRef"]), sp (P ["RootRef", "Child"])] },
    { name := "child_ref.usda", specs := [sp (P ["ChildRef"])] } ]

/-- Example 4, L5234-5282. -/
def ex4L : Layers :=
  [ { name := "root.usda", specs :=
      [ sp (P ["RootClass"]), sp (P ["RefClass"]),
        { path := P ["Root"], inherits := [P ["RootClass"]],
          references := [(some "ref.usda", P ["Ref"])] } ] },
    { name := "ref.usda", specs :=
      [ sp (P ["RefClass"]), { path := P ["Ref"], inherits := [P ["RefClass"]] } ] } ]

/-- Example 5, L5283-5314. -/
def ex5L : Layers :=
  [ { name := "root.usda", specs :=
      [ sp (P ["Ref1"]), sp (P ["Ref2"]),
        { path := P ["Root"], references := [(none, P ["Ref1"]), (none, P ["Ref2"])] } ] } ]

/-- Example 6, L5315-5354. -/
def ex6L : Layers :=
  [ { name := "root.usda", specs := [{ path := P ["Root"], references := [(some "ref.usda", P ["Ref"])] }] },
    { name := "ref.usda", specs :=
      [ { path := P ["Class_1"], specializes := [P ["Class_2"]] }, sp (P ["Class_2"]),
        { path := P ["Ref"], specializes := [P ["Class_1"]] } ] } ]

/-- Example 6 with inherits (L5355-5374). -/
def ex6iL : Layers :=
  [ { name := "root.usda", specs := [{ path := P ["Root"], references := [(some "ref.usda", P ["Ref"])] }] },
    { name := "ref.usda", specs :=
      [ { path := P ["Class_1"], inherits := [P ["Class_2"]] }, sp (P ["Class_2"]),
        { path := P ["Ref"], inherits := [P ["Class_1"]] } ] } ]

/-- A one-site authored reference cycle is reported and its repeated target
is omitted from the expansion. -/
def selfRefL : Layers :=
  [{ name := "self.usda", specs :=
      [{ path := P ["A"], references := [(none, P ["A"])] }] }]

def F : Nat := 4

/-- The computed prim indices reproduce the hand-built trees' strength orders. -/
theorem ex2_tree : strengthOrder (composePrimIndex ex2L "root.usda" F (P ["Root"])) = strengthOrder ex2 := by
  decide +kernel
theorem ex3_tree :
    strengthOrder (composePrimIndex ex3L "root.usda" F (P ["Root", "Child"])) = strengthOrder ex3 := by
  decide +kernel
theorem ex4_tree : strengthOrder (composePrimIndex ex4L "root.usda" F (P ["Root"])) = strengthOrder ex4 := by
  decide +kernel
theorem ex5_tree : strengthOrder (composePrimIndex ex5L "root.usda" F (P ["Root"])) = strengthOrder ex5 := by
  decide +kernel
theorem ex6_tree : strengthOrder (composePrimIndex ex6L "root.usda" F (P ["Root"])) = strengthOrder ex6 := by
  decide +kernel
theorem ex6i_tree :
    strengthOrder (composePrimIndex ex6iL "root.usda" F (P ["Root"])) = strengthOrder ex6i := by
  decide +kernel

/-- The opinion tables (`compose`, layers with a spec). Examples 1-5 match the
spec tables exactly. -/
theorem ex1_table : compose ex1L "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "sub_1.usda" "/Root", S "sub_2.usda" "/Root"] := by decide +kernel
theorem ex2_table : compose ex2L "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "root.usda" "/Inherit", S "root.usda" "/Root{v=a}",
     S "ref.usda" "/Ref", S "ref.usda" "/Inherit", S "ref.usda" "/Ref{v=a}"] := by decide +kernel
theorem ex3_table : compose ex3L "root.usda" F (P ["Root", "Child"]) =
    [S "root.usda" "/Root/Child", S "child_ref.usda" "/ChildRef",
     S "root_ref.usda" "/RootRef/Child"] := by decide +kernel
theorem ex4_table : compose ex4L "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "root.usda" "/RootClass", S "root.usda" "/RefClass",
     S "ref.usda" "/Ref", S "ref.usda" "/RefClass"] := by decide +kernel
theorem ex5_table : compose ex5L "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "root.usda" "/Ref1", S "root.usda" "/Ref2"] := by decide +kernel

/-- Example 6: the spec table lists `root.usda /Class_1` and `root.usda /Class_2`,
but `root.usda` has no such specs; they are prim-index *nodes* (implied
specializes), not opinions. `compose` as written (`if layer.has_spec(path)`,
L4239) drops them. SPEC-ISSUE: LEAN-522. -/
theorem ex6_table_ne : compose ex6L "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "ref.usda" "/Ref", S "ref.usda" "/Class_1", S "ref.usda" "/Class_2"] ∧
    strengthOrder (composePrimIndex ex6L "root.usda" F (P ["Root"])) =
    [S "root.usda" "/Root", S "ref.usda" "/Ref", S "root.usda" "/Class_1", S "ref.usda" "/Class_1",
     S "root.usda" "/Class_2", S "ref.usda" "/Class_2"] := by decide +kernel

theorem ex6i_table_ne : compose ex6iL "root.usda" F (P ["Root"]) =
    [S "root.usda" "/Root", S "ref.usda" "/Ref", S "ref.usda" "/Class_1", S "ref.usda" "/Class_2"] := by
  decide +kernel

/-- Fuel stability: more fuel does not change the examples. -/
theorem fuel_stable :
    compose ex2L "root.usda" F (P ["Root"]) = compose ex2L "root.usda" (F + 4) (P ["Root"]) ∧
    compose ex6L "root.usda" F (P ["Root"]) = compose ex6L "root.usda" (F + 4) (P ["Root"]) := by
  decide +kernel

theorem self_reference_cycle_reported :
    (expandChecked selfRefL (fun _ => none) "self.usda" (P ["A"]) none).cycles =
      [{ source := some ("self.usda", P ["A"]), target := ("self.usda", P ["A"]),
         kind := some .reference }] ∧
    (expandChecked selfRefL (fun _ => none) "self.usda" (P ["A"]) none).node.isSome := by
  decide +kernel

theorem self_reference_composition_cycle_reported :
    (composePrimIndexChecked selfRefL "self.usda" (P ["A"])).2 =
      [{ source := some ("self.usda", P ["A"]), target := ("self.usda", P ["A"]),
         kind := some .reference }] := by
  decide +kernel

end Examples

end UsdSpec.Composition.PrimIndex
