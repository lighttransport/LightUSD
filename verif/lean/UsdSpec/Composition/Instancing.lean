import Mathlib.Data.List.Basic
import UsdSpec.Composition.Strength

/-!
# Instancing (AOUSD USD Core Specification v1.1.0, §10 Instancing; §11 Scene Graph Instancing)

SPEC: core_spec.md L5511-5795 (Instancing, Examples 1-3) and L6006-6020
(Scene Graph Instancing).

CLAIM (L5519-5522): "A prim is instanceable if all of the following conditions
are met: Its strongest opinion for the “instanceable” metadata field is “true”;
It contains one or more composition arcs".

CLAIM (L5524-5528): "When composing a prim that is a descendant of an
instanceable prim, any layer specs that do not come from a composition arc
authored on the instanceable prim are ignored and excluded from the output list
of layer specs. Any composition arcs authored on these excluded specs must also
be ignored during composition."

CLAIM (L5530-5535): "A layer spec “comes from a composition arc authored on the
instanceable prim” if it was introduced by a composition arc authored on the
instanceable prim itself, or on the source prim of such an arc, recursively.
Arcs authored on ancestors of the instanceable prim do not qualify (even if
those arcs introduced the instanceable prim into the scene)."

CLAIM (L5537-5543): two instanceable prims can share opinions if "Both prims
are composed from the same root layer" and "The strength-ordered list of
composition arcs authored on the prims, as well as any corresponding implied
arcs, that provide opinions are exactly the same for both prims."

## Model

A composed prim is its strength-ordered opinion list (strongest first; the
order itself is `UsdSpec.Strength.strengthOrder`, reused here only through
`Site`/`ArcKind`). Each opinion records the arc kind that introduced its node
(`none` = the root layer stack), its `Origin` relative to the prim, whether the
node has specs ("provides opinions"), the arc's layer offset and any
`instanceable` opinion. "Ignoring the arcs authored on excluded specs"
(L5527-5528) is modelled by origin propagation: a node introduced by an arc
authored on a `local`/`ancestral` spec inherits that origin, so it is excluded
together with its source spec (`descendantSites`).

Two readings of "contains one or more composition arcs" are given:
`isInstanceLiteral` (any arc in the prim index) and `isInstance` (OpenUSD
`Pcp_PrimIndexIsInstanceable` + `Pcp_ChildNodeIsInstanceable`,
pxr/usd/pcp/instancing.{h,cpp}: some node that is *direct* (transitively
introduced by an arc on the prim), can contribute specs and *has specs*).

The sharing key (`key`) follows L5537-5543 and OpenUSD `PcpInstanceKey`
(pxr/usd/pcp/instanceKey.cpp): root layer, then for every direct node that has
specs, strong to weak, `(arc kind, site, layer offset)`, plus the composed
variant selections (OpenUSD adds these; with selections that differ the
variant nodes' sites already differ).

SPEC-ISSUE: LEAN-531 — "It contains one or more composition arcs" (L5521) does
not say whether arcs on ancestors count; OpenUSD and LightUSD require an arc
authored on the prim itself (`ex_ancestralOnly`).
SPEC-ISSUE: LEAN-532 — "exactly the same" arcs (L5541-5543) leaves arc identity
undefined: OpenUSD compares (arc kind, target layer-stack site, layer offset),
not the authoring site — so a variant arc on the prim itself never shares
(`ex_variantOnly`) and a reference differing only by layer offset does not
share (`ex_offset`).
SPEC-ISSUE: LEAN-533 — L5522 counts arcs, but OpenUSD counts only arcs whose
nodes provide opinions: an instanceable prim whose only arc is unresolved or
targets a prim with no specs is *not* an instance (its local children are
kept). LightUSD formerly followed the literal reading and dropped the children;
the follow-up requires contributing direct arcs with specs (`ex_unresolvedArc`).
-/

namespace UsdSpec.Instancing
open UsdSpec.Strength (ArcKind Site)

/-- Origin of a node relative to the candidate instance prim (L5530-5535). -/
inductive Origin
  /-- The prim's own specs in the root layer stack. -/
  | «local»
  /-- Introduced (transitively) by an arc authored on an ancestor. -/
  | ancestral
  /-- Introduced by an arc authored on the prim itself, or on the source prim
  of such an arc, recursively (including the corresponding implied arcs). -/
  | direct
  deriving DecidableEq, Repr

structure Opinion where
  site : Site
  arc : Option ArcKind
  origin : Origin
  hasSpecs : Bool := true
  offset : Int := 0
  instanceable : Option Bool := none
  deriving DecidableEq, Repr

structure Prim where
  rootLayer : String
  /-- Strength order, strongest first. -/
  opinions : List Opinion
  variantSel : List (String × String) := []
  deriving DecidableEq, Repr

/-- Strongest authored `instanceable` opinion; fallback `false`. SPEC: L5521. -/
def instanceableValue (p : Prim) : Bool :=
  ((p.opinions.filter (·.hasSpecs)).filterMap (·.instanceable)).head?.getD false

/-- Literal L5522 condition: the prim index contains a composition arc. -/
def hasArcLiteral (p : Prim) : Bool := p.opinions.any (·.arc.isSome)

/-- OpenUSD `Pcp_ChildNodeIsInstanceable`: direct, introduced by an arc, has specs. -/
def instArc (o : Opinion) : Bool := o.arc.isSome && o.origin == .direct && o.hasSpecs

def isInstanceLiteral (p : Prim) : Bool := instanceableValue p && hasArcLiteral p

/-- Instance predicate (OpenUSD reading of L5519-5522). -/
def isInstance (p : Prim) : Bool := instanceableValue p && p.opinions.any instArc

/-- Specs kept for a descendant of an instance (L5524-5535): only those whose
node comes from an arc on the instance prim. -/
def descendantSites (ops : List Opinion) : List Site :=
  (ops.filter (·.origin == .direct)).map (·.site)

structure KeyArc where
  arc : ArcKind
  site : Site
  offset : Int
  deriving DecidableEq, Repr

structure Key where
  rootLayer : String
  arcs : List KeyArc
  variantSel : List (String × String)
  deriving DecidableEq, Repr

def keyArcs (ops : List Opinion) : List KeyArc :=
  ops.filterMap fun o =>
    match o.arc with
    | some k => if instArc o then some ⟨k, o.site, o.offset⟩ else none
    | none => none

/-- The sharing criteria of L5537-5543 as a key. -/
def key (p : Prim) : Key := ⟨p.rootLayer, keyArcs p.opinions, p.variantSel⟩

/-- `p` and `q` may share opinions. -/
def Shares (p q : Prim) : Prop := key p = key q

/-- IN1: sharing is an equivalence relation. -/
theorem shares_equivalence : Equivalence Shares where
  refl _ := rfl
  symm h := h.symm
  trans h₁ h₂ := h₁.trans h₂

instance : DecidableRel Shares := fun p q => inferInstanceAs (Decidable (key p = key q))

/-- Prototype assignment over a stage's prims: the instance keys in first-seen
order, a prototype per distinct key, instances mapped to the index of their key. -/
def instanceKeys (ps : List Prim) : List Key := (ps.filter isInstance).map key

def prototypeOf (ps : List Prim) (p : Prim) : Option Nat :=
  if isInstance p then some ((instanceKeys ps).idxOf (key p)) else none

theorem key_mem_instanceKeys {ps : List Prim} {p : Prim} (hp : p ∈ ps) (hi : isInstance p = true) :
    key p ∈ instanceKeys ps := by
  unfold instanceKeys; exact List.mem_map.2 ⟨p, List.mem_filter.2 ⟨hp, hi⟩, rfl⟩

/-- IN2: two instances of a stage get the same prototype iff their keys are equal.
The assignment is a function of the key (`→`) and injective on keys (`←`). -/
theorem prototypeOf_eq_iff {ps : List Prim} {p q : Prim} (hp : p ∈ ps) (_hq : q ∈ ps)
    (hip : isInstance p = true) (hiq : isInstance q = true) :
    prototypeOf ps p = prototypeOf ps q ↔ Shares p q := by
  unfold prototypeOf Shares
  simp only [hip, hiq, if_true, Option.some.injEq]
  exact List.idxOf_inj (key_mem_instanceKeys hp hip)

/-- Every prototype index is in range (one prototype per distinct key). -/
theorem prototypeOf_lt {ps : List Prim} {p : Prim} {n : Nat} (hp : p ∈ ps)
    (h : prototypeOf ps p = some n) : n < (instanceKeys ps).length := by
  unfold prototypeOf at h
  split at h
  · rename_i hi; simp at h; subst h
    exact List.idxOf_lt_length_iff.2 (key_mem_instanceKeys hp hi)
  · simp at h

/-- IN3: ancestral (and local) opinions never enter the key — removing them, or
changing them arbitrarily, leaves the sharing key unchanged (L5533-5535,
Example 2). -/
theorem keyArcs_cons_of_not {o : Opinion} {os : List Opinion} (h : instArc o = false) :
    keyArcs (o :: os) = keyArcs os := by
  unfold keyArcs; rw [List.filterMap_cons]
  cases ho : o.arc <;> simp [h]

theorem keyArcs_cons_congr (o : Opinion) {as bs : List Opinion} (h : keyArcs as = keyArcs bs) :
    keyArcs (o :: as) = keyArcs (o :: bs) := by
  unfold keyArcs at h ⊢; rw [List.filterMap_cons, List.filterMap_cons, h]

theorem keyArcs_filter_direct (ops : List Opinion) :
    keyArcs (ops.filter (·.origin == .direct)) = keyArcs ops := by
  induction ops with
  | nil => rfl
  | cons o os ih =>
    by_cases h : o.origin = .direct
    · rw [List.filter_cons_of_pos (by simpa using h)]; exact keyArcs_cons_congr o ih
    · have : instArc o = false := by simp [instArc, h]
      rw [List.filter_cons_of_neg (by simpa using h), ih, keyArcs_cons_of_not this]

theorem key_indep_nondirect (p q : Prim) (hr : p.rootLayer = q.rootLayer)
    (hv : p.variantSel = q.variantSel)
    (hd : p.opinions.filter (·.origin == .direct) = q.opinions.filter (·.origin == .direct)) :
    Shares p q := by
  unfold Shares key
  rw [hr, hv, ← keyArcs_filter_direct p.opinions, hd, keyArcs_filter_direct]

/-- Implied arcs that provide no opinions do not enter the key (L5792-5794). -/
theorem keyArcs_drop_noSpecs (ops : List Opinion) :
    keyArcs (ops.filter (·.hasSpecs)) = keyArcs ops := by
  induction ops with
  | nil => rfl
  | cons o os ih =>
    by_cases h : o.hasSpecs = true
    · rw [List.filter_cons_of_pos h]; exact keyArcs_cons_congr o ih
    · have : instArc o = false := by simp [instArc, h]
      rw [List.filter_cons_of_neg h, ih, keyArcs_cons_of_not this]

/-- The OpenUSD reading implies the literal one. -/
theorem isInstance_literal (p : Prim) (h : isInstance p = true) : isInstanceLiteral p = true := by
  unfold isInstance at h; unfold isInstanceLiteral hasArcLiteral
  simp only [Bool.and_eq_true, List.any_eq_true] at h ⊢
  obtain ⟨h1, o, ho, hi⟩ := h
  refine ⟨h1, o, ho, ?_⟩
  simp only [instArc, Bool.and_eq_true] at hi; exact hi.1.1

/-! ## IN4: the spec examples (L5545-5795), opinions built by hand from the USDA -/

def S (l p : String) : Site := ⟨l, p⟩
def loc (l p : String) (inst : Option Bool := none) : Opinion :=
  ⟨S l p, none, .local, true, 0, inst⟩
def dref (l p : String) : Opinion := ⟨S l p, some .reference, .direct, true, 0, none⟩
def aref (l p : String) (inst : Option Bool := none) : Opinion :=
  ⟨S l p, some .reference, .ancestral, true, 0, inst⟩
def dinh (l p : String) (specs : Bool) : Opinion := ⟨S l p, some .inherit, .direct, specs, 0, none⟩

/-! ### Example 1 (basic instanceability), L5547-5626 -/

def e1_A1 : Prim := ⟨"root.usda", [loc "root.usda" "/Asset_1" (some true), dref "asset.usda" "/Asset"], []⟩
def e1_A2 : Prim := ⟨"root.usda", [loc "root.usda" "/Asset_2" (some true), dref "asset.usda" "/Asset"], []⟩
/-- `/Asset_3`: reference to asset_ref.usda, whose `/Asset` references asset.usda
(an arc on the source prim of a direct arc: still direct). -/
def e1_A3 : Prim := ⟨"root.usda", [loc "root.usda" "/Asset_3" (some true),
  dref "asset_ref.usda" "/Asset", dref "asset.usda" "/Asset"], []⟩
/-- Opinions for `/Asset_1/Sphere` before instancing: root.usda `over "Sphere"` (local) and asset.usda. -/
def e1_A1_Sphere : List Opinion := [loc "root.usda" "/Asset_1/Sphere", dref "asset.usda" "/Asset/Sphere"]

example : isInstance e1_A1 ∧ isInstance e1_A2 ∧ isInstance e1_A3 := by decide
example : Shares e1_A1 e1_A2 := by decide
example : ¬ Shares e1_A1 e1_A3 ∧ ¬ Shares e1_A2 e1_A3 := by decide
/-- The table "Opinions for `/Asset_1/Sphere` and `/Asset_2/Sphere`" (L5618-5626). -/
example : descendantSites e1_A1_Sphere = [S "asset.usda" "/Asset/Sphere"] := by decide
example : prototypeOf [e1_A1, e1_A2, e1_A3] e1_A1 = some 0 ∧
    prototypeOf [e1_A1, e1_A2, e1_A3] e1_A2 = some 0 ∧
    prototypeOf [e1_A1, e1_A2, e1_A3] e1_A3 = some 2 := by decide

/-! ### Example 2 (ancestral arcs), L5628-5711. root.usda has no spec at `/LeftGroup/Asset`. -/

def e2_L : Prim := ⟨"root.usda", [aref "group_1.usda" "/Group_1/Asset" (some true), dref "asset.usda" "/Asset"], []⟩
def e2_R : Prim := ⟨"root.usda", [aref "group_2.usda" "/Group_2/Asset" (some true), dref "asset.usda" "/Asset"], []⟩

example : isInstance e2_L ∧ isInstance e2_R ∧ Shares e2_L e2_R := by decide

/-! ### Example 3 (implied arcs), L5704-5795. The inherit of `/class_Asset` in
asset.usda is implied into group_N.usda (which has an `over`, so it provides
opinions) and into root.usda (no spec). -/

def e3 (g : String) (classSpecs : Bool) : Prim :=
  ⟨"root.usda", [aref s!"group_{g}.usda" s!"/Group_{g}/Asset" (some true),
    dinh "root.usda" "/class_Asset" false, dinh s!"group_{g}.usda" "/class_Asset" classSpecs,
    dref "asset.usda" "/Asset", dinh "asset.usda" "/class_Asset" true], []⟩

example : isInstance (e3 "1" true) ∧ isInstance (e3 "2" true) := by decide
example : ¬ Shares (e3 "1" true) (e3 "2" true) := by decide
/-- "If those prims were removed from those layers, the two would be able to share" (L5792-5794). -/
example : Shares (e3 "1" false) (e3 "2" false) := by decide

/-! ## OpenUSD v26.11 edge cases (scratch probes; see ISSUES.md LEAN-531..533) -/

/-- `instanceable = true` reached only through an ancestral reference: not an
instance in OpenUSD or LightUSD; an instance under the literal L5522 reading. -/
def ex_ancestralOnly : Prim := ⟨"root.usda", [aref "grp.usda" "/G/A" (some true)], []⟩
example : isInstance ex_ancestralOnly = false ∧ isInstanceLiteral ex_ancestralOnly = true := by decide

/-- `instanceable = true` + `inherits = </NoClass>` (no such prim): OpenUSD keeps
the prim's local children (not an instance); literal reading and LightUSD
treat it as an instance. -/
def ex_unresolvedArc : Prim :=
  ⟨"root.usda", [loc "root.usda" "/MissingInh" (some true), dinh "root.usda" "/NoClass" false], []⟩
example : isInstance ex_unresolvedArc = false ∧ isInstanceLiteral ex_unresolvedArc = true := by decide

/-- Same reference, layer offsets 0 vs 10: different prototypes in OpenUSD. -/
def ex_offset (o : Int) : Prim :=
  ⟨"root.usda", [loc "root.usda" "/Off" (some true), { dref "asset.usda" "/Asset" with offset := o }], []⟩
example : ¬ Shares (ex_offset 0) (ex_offset 10) := by decide

/-- A variant set authored on the prim itself: the variant node's site is the
prim's own path, so two such prims never share (OpenUSD: distinct prototypes). -/
def ex_variantOnly (p : String) : Prim :=
  ⟨"root.usda", [loc "root.usda" p (some true),
    ⟨S "root.usda" (p ++ "{v=x}"), some .variant, .direct, true, 0, none⟩], [("v", "x")]⟩
example : isInstance (ex_variantOnly "/VarOnly1") ∧ ¬ Shares (ex_variantOnly "/VarOnly1") (ex_variantOnly "/VarOnly2") := by
  decide

/-- The strongest opinion wins: a local `instanceable = false` over a referenced `true`. -/
def ex_strongFalse : Prim := ⟨"root.usda",
  [loc "root.usda" "/Strong" (some false), { dref "asset.usda" "/Asset2" with instanceable := some true }], []⟩
example : isInstance ex_strongFalse = false := by decide

end UsdSpec.Instancing
