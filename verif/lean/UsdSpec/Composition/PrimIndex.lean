import UsdSpec.Composition.Strength

/-!
# A toy executable prim-index builder (AOUSD USD Core Specification v1.1.0, §10)

SPEC: core_spec.md L4200-4242 (`compose_helper` / `compose` outline),
L4244-4875 (composition operators), L5010-5077 (strength ordering),
examples L5081-5374.

`UsdSpec.Strength` checks the six worked examples against *hand-built* prim
index trees. Here the tree is *computed* from toy layers:

* a layer has a name, sublayers and prim specs; a prim spec carries
  `references` (`(layer?, path)`, `none` = internal), `inherits`,
  `specializes`, `variantSets` and variant `selections`;
* `composeTree` follows the outline: compose the parent path, append the
  child name to every node ("ancestral opinions", L4223-4228), then add the
  arcs authored at the child path on every node (direct arcs, namespace depth =
  the child's depth);
* arcs expand recursively at their target sites (fuel-bounded);
* implied classes (L5234-5282, L5315-5374): every inherit / specialize subtree
  below a reference or payload node is copied, mapped through the arc's path
  translation (`target ↦ referencing prim`, identity elsewhere), to the
  referencing node's layer stack with `implied := true`, bottom-up so that the
  copies propagate to the root;
* variant selections are resolved in two passes: the tree without variants
  gives a strength order, the strongest authored selection along it is used
  (L4719-4770);
* `Strength.strengthOrder` orders the nodes; `compose` expands every node's
  layer stack and keeps the layers that have a spec (L4232-4242).

Simplifications (toy, documented): implied class nodes do not expand their own
authored arcs; relocates, payload loading rules, list-op editing of arc lists
across layers and arc-cycle detection are out of scope (fuel bounds cycles).

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

/-- The layer stack rooted at `n`, strongest first (L4250-4270). -/
def layerStack (ls : Layers) : Nat → String → List String
  | 0, n => [n]
  | f + 1, n =>
    n :: ((findLayer ls n).map (·.sublayers) |>.getD [] |>.flatMap (layerStack ls f))

def specsIn (ls : Layers) (layer : String) (p : Path) : List PrimSpec :=
  ((findLayer ls layer).map (·.specs) |>.getD []).filter (·.path == p)

def stackSpecs (ls : Layers) (fuel : Nat) (root : String) (p : Path) : List PrimSpec :=
  (layerStack ls fuel root).flatMap fun l => specsIn ls l p

/-- Internal tree: layer-stack root, path, arc, children. -/
inductive INode
  | mk (layer : String) (path : Path) (arc : Option ArcInfo) (children : List INode)
  deriving Repr

namespace INode
def layer : INode → String | .mk l _ _ _ => l
def path : INode → Path | .mk _ p _ _ => p
def arc : INode → Option ArcInfo | .mk _ _ a _ => a
def children : INode → List INode | .mk _ _ _ cs => cs
end INode

abbrev Sel := String → Option String

/-- Indexed list. -/
def enum {α : Type} (l : List α) : List (Nat × α) := (List.range l.length).zip l

/-- Arcs authored on the specs of `(layer stack, p)`: list of
`(arc, target layer, target path)`. The composed arc list is the concatenation
over the layer stack, strongest first (no cross-layer list-op editing). -/
def authoredArcs (ls : Layers) (fuel : Nat) (sel : Sel) (layer : String) (p : Path) :
    List (ArcInfo × String × Path) :=
  let ss := stackSpecs ls fuel layer p
  let d := depth p
  let inh := enum (ss.flatMap (·.inherits))
  let vs := enum (ss.flatMap (·.variantSets))
  let refs := enum (ss.flatMap (·.references))
  let sps := enum (ss.flatMap (·.specializes))
  inh.map (fun (i, t) => (⟨.inherit, d, false, i⟩, layer, t)) ++
  vs.filterMap (fun (i, s) => (sel s).map fun v => (⟨.variant, d, false, i⟩, layer, p ++ [.variant s v])) ++
  refs.map (fun (i, (l, t)) => (⟨.reference, d, false, i⟩, l.getD layer, t)) ++
  sps.map (fun (i, t) => (⟨.specialize, d, false, i⟩, layer, t))

/-- Expand a site: its authored arcs, each recursively. -/
def expand (ls : Layers) (sel : Sel) : Nat → String → Path → Option ArcInfo → INode
  | 0, l, p, a => .mk l p a []
  | f + 1, l, p, a =>
    .mk l p a ((authoredArcs ls (f + 1) sel l p).map fun (ai, l', t) => expand ls sel f l' t (some ai))

/-- Append a path element to every node (ancestral opinions, L4223-4228). -/
def extend (e : PathElem) : Nat → INode → INode
  | 0, n => n
  | f + 1, .mk l p a cs => .mk l (p ++ [e]) a (cs.map (extend e f))

/-- Add the arcs authored at each node's (new) path. -/
def addDirect (ls : Layers) (sel : Sel) (fuel : Nat) : Nat → INode → INode
  | 0, n => n
  | f + 1, .mk l p a cs =>
    .mk l p a (cs.map (addDirect ls sel fuel f) ++
      (authoredArcs ls fuel sel l p).map fun (ai, l', t) => expand ls sel fuel l' t (some ai))

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

/-- The implied copy of a class subtree at layer stack `l`. -/
def impliedCopy (l : String) (src dst : Path) : Nat → INode → INode
  | 0, n => n
  | f + 1, .mk _ p a cs =>
    .mk l (mapPath src dst p) (a.map fun ai => { ai with implied := true })
      ((cs.filter (isClassArc ·.arc)).map (impliedCopy l src dst f))

def sameSite (a b : INode) : Bool := a.layer == b.layer && a.path == b.path

/-- Bottom-up implied-class propagation (Example 4/6 semantics). -/
def propagate : Nat → INode → INode
  | 0, n => n
  | f + 1, .mk l p a cs =>
    let cs' := cs.map (propagate f)
    let implied := cs'.flatMap fun c =>
      if isRefArc c.arc then
        (c.children.filter (isClassArc ·.arc)).map (impliedCopy l c.path p f)
      else []
    let fresh := implied.foldl (fun acc n => if acc.any (sameSite n) then acc else acc ++ [n]) cs'
    .mk l p a fresh

/-- `compose_helper` (L4215-4231) as a tree. -/
def composeTreeAux (ls : Layers) (root : String) (sel : Sel) (fuel : Nat) : Path → INode
  | [] => .mk root [] none []
  | e :: rest =>
    -- paths are built root-first; recursion on the reversed path
    addDirect ls sel fuel fuel (extend e fuel (composeTreeAux ls root sel fuel rest))

def composeTreeWith (ls : Layers) (root : String) (sel : Sel) (fuel : Nat) (p : Path) : INode :=
  propagate fuel (composeTreeAux ls root sel fuel p.reverse)

def toNode : Nat → INode → Node
  | 0, .mk l p a _ => .mk ⟨l, render p⟩ a []
  | f + 1, .mk l p a cs => .mk ⟨l, render p⟩ a (cs.map (toNode f))

/-- Pre-order (unsorted) `(layer, path)` list of an internal tree. -/
def sitesI : Nat → INode → List (String × Path)
  | 0, n => [(n.layer, n.path)]
  | f + 1, .mk l p _ cs => (l, p) :: cs.flatMap (sitesI f)

/-- Strongest authored selection along a strength-ordered site list. -/
def selFrom (ls : Layers) (fuel : Nat) (sites : List (String × Path)) : Sel := fun s =>
  (sites.flatMap fun (l, p) => (stackSpecs ls fuel l p).flatMap (·.selections)).findSome? fun (k, v) =>
    if k == s then some v else none

/-- Sites of an internal tree in strength order (via `Strength.strengthOrder` on
the rendered tree, then back to paths by lookup). -/
def orderedSites (fuel : Nat) (t : INode) : List (String × Path) :=
  let all := sitesI fuel t
  (strengthOrder (toNode fuel t)).filterMap fun s => all.find? fun (l, p) => l == s.layer && render p == s.path

/-- The prim index: two passes for variant selections. -/
def composePrimIndex (ls : Layers) (root : String) (fuel : Nat) (p : Path) : Node :=
  let t₀ := composeTreeWith ls root (fun _ => none) fuel p
  let sel := selFrom ls fuel (orderedSites fuel t₀)
  toNode fuel (composeTreeWith ls root sel fuel p)

/-- `compose` (L4233-4242): strength-ordered `(layer, path)` opinions, keeping
only layers of each node's layer stack that have a spec at the node's path. -/
def compose (ls : Layers) (root : String) (fuel : Nat) (p : Path) : List Site :=
  (strengthOrder (composePrimIndex ls root fuel p)).flatMap fun s =>
    (layerStack ls fuel s.layer).filterMap fun l =>
      if (((findLayer ls l).map (·.specs)).getD []).any (fun sp => render sp.path == s.path)
      then some ⟨l, s.path⟩ else none

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

end Examples

end UsdSpec.Composition.PrimIndex
