import Mathlib.Data.List.Basic
import Mathlib.Data.List.Sort

/-!
# Strength ordering — LIVERPS (AOUSD USD Core Specification v1.1.0, §10.4)

SPEC: core_spec.md L5010-5077 (Strength Ordering, Specializes) and the six
examples L5081-5374.

The model is an abstract *prim index*: a tree of nodes, each a site
`(layer, specPath)` introduced by a composition arc from its parent. Sibling
arcs are ordered by the spec's tie-breaks (L5035-5064):
arc-kind rank (I, V, Re, R, P, S) → deeper namespace first → authored before
implied → arc-list order. `strengthOrder` is the pre-order walk of the sorted
tree, except that every subtree introduced by a *specializes* arc is deferred
to a global FIFO queue that is drained after the main walk (and whose own
specializes children are deferred again) — this is the "globally weaker"
rule of L5068-5077, and it reproduces Example 6 exactly.

The spec lists Relocates among the arc kinds (L5035-5042) although relocates
are layer-stack metadata rather than list-op arcs (L4876-4891);
SPEC-ISSUE: LEAN-501 records the question of how the sibling tie-breaks
(namespace depth, authored/implied, list order) apply to them. The model keeps
the rank slot so that any answer can be plugged in.
-/

namespace UsdSpec.Strength

/-- Composition arc kinds in LIVERPS order. SPEC: L5035-5050. -/
inductive ArcKind
  | inherit | variant | relocate | reference | payload | specialize
  deriving DecidableEq, Repr

def ArcKind.rank : ArcKind → Nat
  | .inherit => 0 | .variant => 1 | .relocate => 2 | .reference => 3 | .payload => 4 | .specialize => 5

/-- How a node was introduced: arc kind, namespace depth of the prim the arc
was authored on (deeper = stronger, L5054-5056), authored vs implied
(L5058-5060), and position in the composed arc list (L5062-5064). -/
structure ArcInfo where
  kind : ArcKind
  depth : Nat
  implied : Bool
  index : Nat
  deriving DecidableEq, Repr

/-- Sort key: `(rank, depth-inverted, implied, index)`, smaller = stronger.
Negative integer depth preserves the ordering at every namespace depth. -/
def ArcInfo.key (a : ArcInfo) : List Int :=
  [a.kind.rank, -(a.depth : Int), if a.implied then 1 else 0, a.index]

/-- Sibling comparison: `true` means `a` is at least as strong as `b`.
SPEC: L5035-5064 (kind, then deeper namespace, then authored before implied,
then arc-list order), with no artificial depth bound. -/
def ArcInfo.le (a b : ArcInfo) : Bool := decide (a.key ≤ b.key)

structure Site where
  layer : String
  path : String
  deriving DecidableEq, Repr

/-- A prim-index node. `arc = none` only for the root. Children are kept in
the order they were introduced; `sortedChildren` applies the tie-breaks. -/
inductive Node
  | mk (site : Site) (arc : Option ArcInfo) (children : List Node)
  deriving Repr

namespace Node

def site : Node → Site | .mk s _ _ => s
def arc : Node → Option ArcInfo | .mk _ a _ => a
def children : Node → List Node | .mk _ _ cs => cs

def isSpecialize (n : Node) : Bool :=
  match n.arc with
  | some a => a.kind == .specialize
  | none => false

def stronger (a b : Node) : Bool :=
  match a.arc, b.arc with
  | some x, some y => x.le y
  | _, _ => true

/-- Stable insertion of `x` before the first element it is strictly stronger than. -/
def insertStable (x : Node) : List Node → List Node
  | [] => [x]
  | y :: ys => if stronger y x then y :: insertStable x ys else x :: y :: ys

/-- Children in strength order (stable: equal keys keep authored order). -/
def sortedChildren (n : Node) : List Node :=
  n.children.foldl (fun acc c => insertStable c acc) []

mutual
def size : Node → Nat
  | .mk _ _ cs => 1 + sizeList cs
def sizeList : List Node → Nat
  | [] => 0
  | c :: cs => size c + sizeList cs
end

-- Pre-order sites of a tree in child-list order (no sorting): the multiset
-- reference for completeness.
mutual
def allSites : Node → List Site
  | .mk s _ cs => s :: allSitesList cs
def allSitesList : List Node → List Site
  | [] => []
  | c :: cs => allSites c ++ allSitesList cs
end

end Node

open Node

/-- One level of the walk: emits the pre-order of the non-specializes part
of the sorted tree and collects, in encounter order, the roots of
specializes subtrees (a node's own specializes children before those found
deeper). SPEC: L5068-5077. -/
partial def mainWalkP (n : Node) : List Site × List Node :=
  let cs := n.sortedChildren
  let own := cs.filter isSpecialize
  let rest := (cs.filter (fun c => !c.isSpecialize)).map mainWalkP
  (n.site :: (rest.map (·.1)).flatten, own ++ (rest.map (·.2)).flatten)

/-- Fuel-bounded total version of `mainWalkP` (fuel = tree height suffices). -/
def mainWalk : Nat → Node → List Site × List Node
  | 0, n => ([n.site], [])
  | fuel + 1, n =>
    let cs := n.sortedChildren
    let own := cs.filter isSpecialize
    let rest := (cs.filter (fun c => !c.isSpecialize)).map (mainWalk fuel)
    (n.site :: (rest.map (·.1)).flatten, own ++ (rest.map (·.2)).flatten)

/-- Drain the FIFO of deferred specializes roots, level by level. -/
def levels (fuel : Nat) : List Node → List Site
  | [] => []
  | roots =>
    match fuel with
    | 0 => []
    | f + 1 =>
      let ws := roots.map (mainWalk f)
      (ws.map (·.1)).flatten ++ levels f (ws.map (·.2)).flatten

/-- The strength-ordered list of sites of a prim index. -/
def strengthOrder (root : Node) : List Site := levels root.size [root]

/-! ## The sibling order is a total preorder (lexicographic on keys) -/

theorem ArcInfo.le_refl (a : ArcInfo) : a.le a = true := by simp [ArcInfo.le]

theorem ArcInfo.le_total (a b : ArcInfo) : a.le b = true ∨ b.le a = true := by
  simp only [ArcInfo.le, decide_eq_true_eq]; exact List.le_total _ _

theorem ArcInfo.le_trans {a b c : ArcInfo} (h₁ : a.le b = true) (h₂ : b.le c = true) :
    a.le c = true := by
  simp only [ArcInfo.le, decide_eq_true_eq] at *; exact List.le_trans h₁ h₂

/-- Keys determine the four ordering fields, so two arcs that tie are
indistinguishable by every spec tie-break (rank, depth, implied, index). -/
theorem ArcInfo.key_inj {a b : ArcInfo}
    (h : a.key = b.key) :
    a.kind.rank = b.kind.rank ∧ a.depth = b.depth ∧ a.implied = b.implied ∧ a.index = b.index := by
  simp only [ArcInfo.key, List.cons.injEq, and_true] at h
  obtain ⟨h1, h2, h3, h4⟩ := h
  refine ⟨by omega, by omega, ?_, by omega⟩
  cases ha' : a.implied <;> cases hb' : b.implied <;> simp_all

/-- Namespace depths beyond 1024 remain distinguishable and deeper wins. -/
example : (ArcInfo.mk .inherit 2049 false 0).le ⟨.inherit, 2048, false, 0⟩ = true ∧
    (ArcInfo.mk .inherit 2048 false 0).le ⟨.inherit, 2049, false, 0⟩ = false := by decide

/-! ## Spec examples (L5081-5374), decided mechanically -/
section Examples

def S (l p : String) : Site := ⟨l, p⟩
def leaf (s : Site) (a : ArcInfo) : Node := .mk s (some a) []
def I (depth : Nat) (implied : Bool) (index : Nat) : ArcInfo := ⟨.inherit, depth, implied, index⟩
def V (depth : Nat) (index : Nat) : ArcInfo := ⟨.variant, depth, false, index⟩
def R (depth : Nat) (index : Nat) : ArcInfo := ⟨.reference, depth, false, index⟩
def Sp (depth : Nat) (implied : Bool) (index : Nat) : ArcInfo := ⟨.specialize, depth, implied, index⟩

/-- Example 2 (composition arc ordering), L5127-5183. Local, then the implied
inherit (I), the local variant (V), the reference (R) with its inherit and
variant. -/
def ex2 : Node :=
  .mk (S "root.usda" "/Root") none
    [ .mk (S "ref.usda" "/Ref") (some (R 1 0))
        [ leaf (S "ref.usda" "/Ref{v=a}") (V 1 0),
          leaf (S "ref.usda" "/Inherit") (I 1 false 0) ],
      leaf (S "root.usda" "/Root{v=a}") (V 1 0),
      leaf (S "root.usda" "/Inherit") (I 1 true 0) ]

example : strengthOrder ex2 =
    [S "root.usda" "/Root", S "root.usda" "/Inherit", S "root.usda" "/Root{v=a}",
     S "ref.usda" "/Ref", S "ref.usda" "/Inherit", S "ref.usda" "/Ref{v=a}"] := by decide

/-- Example 3 (namespace depth), L5184-5233: the reference authored on
`/Root/Child` (depth 2) beats the ancestral one authored on `/Root` (depth 1). -/
def ex3 : Node :=
  .mk (S "root.usda" "/Root/Child") none
    [ leaf (S "root_ref.usda" "/RootRef/Child") (R 1 0),
      leaf (S "child_ref.usda" "/ChildRef") (R 2 0) ]

example : strengthOrder ex3 =
    [S "root.usda" "/Root/Child", S "child_ref.usda" "/ChildRef",
     S "root_ref.usda" "/RootRef/Child"] := by decide

/-- Example 4 (implied vs authored), L5234-5282. -/
def ex4 : Node :=
  .mk (S "root.usda" "/Root") none
    [ .mk (S "ref.usda" "/Ref") (some (R 1 0)) [ leaf (S "ref.usda" "/RefClass") (I 1 false 0) ],
      leaf (S "root.usda" "/RefClass") (I 1 true 0),
      leaf (S "root.usda" "/RootClass") (I 1 false 0) ]

example : strengthOrder ex4 =
    [S "root.usda" "/Root", S "root.usda" "/RootClass", S "root.usda" "/RefClass",
     S "ref.usda" "/Ref", S "ref.usda" "/RefClass"] := by decide

/-- Example 5 (sibling arc ordering), L5283-5314. -/
def ex5 : Node :=
  .mk (S "root.usda" "/Root") none
    [ leaf (S "root.usda" "/Ref2") (R 1 1), leaf (S "root.usda" "/Ref1") (R 1 0) ]

example : strengthOrder ex5 =
    [S "root.usda" "/Root", S "root.usda" "/Ref1", S "root.usda" "/Ref2"] := by decide

/-- Example 6 (specializes), L5315-5374: `Ref` specializes `Class_1`, which
specializes `Class_2`; both specializes arcs are implied back to the root. -/
def ex6 : Node :=
  .mk (S "root.usda" "/Root") none
    [ .mk (S "ref.usda" "/Ref") (some (R 1 0))
        [ .mk (S "ref.usda" "/Class_1") (some (Sp 1 false 0))
            [ leaf (S "ref.usda" "/Class_2") (Sp 1 false 0) ] ],
      .mk (S "root.usda" "/Class_1") (some (Sp 1 true 0))
        [ leaf (S "root.usda" "/Class_2") (Sp 1 true 0) ] ]

example : strengthOrder ex6 =
    [S "root.usda" "/Root", S "ref.usda" "/Ref", S "root.usda" "/Class_1", S "ref.usda" "/Class_1",
     S "root.usda" "/Class_2", S "ref.usda" "/Class_2"] := by decide

/-- Example 6, the comparison with the arcs switched to inherits (L5355-5374). -/
def ex6i : Node :=
  .mk (S "root.usda" "/Root") none
    [ .mk (S "ref.usda" "/Ref") (some (R 1 0))
        [ .mk (S "ref.usda" "/Class_1") (some (I 1 false 0))
            [ leaf (S "ref.usda" "/Class_2") (I 1 false 0) ] ],
      .mk (S "root.usda" "/Class_1") (some (I 1 true 0))
        [ leaf (S "root.usda" "/Class_2") (I 1 true 0) ] ]

example : strengthOrder ex6i =
    [S "root.usda" "/Root", S "root.usda" "/Class_1", S "root.usda" "/Class_2",
     S "ref.usda" "/Ref", S "ref.usda" "/Class_1", S "ref.usda" "/Class_2"] := by decide

end Examples

end UsdSpec.Strength
