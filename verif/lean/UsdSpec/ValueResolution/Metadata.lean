import UsdSpec.ListOp
import UsdSpec.Dict

/-!
# Metadata resolution (AOUSD USD Core Specification v1.1.0, §12.1)

SPEC: core_spec.md L6111-6269 (Metadata Resolution).

An *opinion stack* for a field is the strength-ordered list of the opinions
of the composed specs, **strongest first** (as produced by composition,
`UsdSpec.Strength.strengthOrder`); an entry is `none` when that spec does not
author the field.

Resolvers and their algebra:
* strongest-wins fields (`typeName` on prims, generic metadata):
  `strongest = foldr (· <|> ·) none` — the *first-Some* monoid (`Option.orElse`,
  associative, identity `none`);
* `variability`: prim definition, else the **weakest** opinion (L6185-6190) —
  first-Some of the reversed stack;
* `custom`: OR over the stack (L6192-6198), `false` when schema-defined — the
  `(Bool, ||, false)` monoid;
* dictionaries: `UsdSpec.Dict.resolveDict` (strongest-first left fold; the fold
  direction is SPEC-ISSUE LEAN-102);
* list ops: `UsdSpec.ListOp.chain`, whose application is the weakest-to-strongest
  fold of `apply` (`listField_eq`, via `ListOp.apply_chain`);
* layer metadata: root layer only (L6257-6260);
* specifier: special rule (L6118-6131), see `SPEC-ISSUE: LEAN-701`.
Not modelled: `typeName` on schema-defined properties (L6149-6158).
-/

namespace UsdSpec.ValueResolution.Metadata

/-! ## Strongest-wins: the first-Some monoid -/

/-- Strongest authored opinion. SPEC: L6113-6115 ("retrieves the strongest opinion"). -/
def strongest {α : Type} : List (Option α) → Option α
  | [] => none
  | o :: os => o <|> strongest os

theorem orElse_assoc {α : Type} (a b c : Option α) : ((a <|> b) <|> c) = (a <|> (b <|> c)) := by
  cases a <;> rfl
theorem orElse_none_left {α : Type} (a : Option α) : (none <|> a) = a := rfl
theorem orElse_none_right {α : Type} (a : Option α) : (a <|> none) = a := by cases a <;> rfl

theorem strongest_eq_foldr {α : Type} (os : List (Option α)) :
    strongest os = os.foldr (fun a b => a <|> b) none := by
  induction os with
  | nil => rfl
  | cons o os ih => simp [strongest, ih]

/-- Associativity makes the stack splittable anywhere (e.g. per layer stack). -/
theorem strongest_append {α : Type} (a b : List (Option α)) :
    strongest (a ++ b) = (strongest a <|> strongest b) := by
  induction a with
  | nil => rfl
  | cons o os ih => simp only [List.cons_append, strongest, ih, orElse_assoc]

theorem strongest_eq_findSome {α : Type} (os : List (Option α)) :
    strongest os = os.findSome? id := by
  induction os with
  | nil => rfl
  | cons o os ih => cases o <;> simp [strongest, ih, List.findSome?]

/-- Fallback (L6262-6266). -/
def withFallback {α : Type} (fallback : α) (os : List (Option α)) : α :=
  (strongest os).getD fallback

/-! ## typeName (L6142-6183) -/

/-- Prim `typeName`: strongest authored opinion (L6146-6147). -/
def primTypeName (os : List (Option String)) : Option String := strongest os

/-- Property `typeName`: the prim definition's, if schema-defined; else the
strongest authored (L6149-6158). -/
def propTypeName (schemaType : Option String) (os : List (Option String)) : Option String :=
  match schemaType with
  | some t => some t
  | none => strongest os

/-- Authored opinions cannot change a schema-defined `typeName` (L6164-6166). -/
theorem propTypeName_schema (t : String) (os : List (Option String)) :
    propTypeName (some t) os = some t := rfl

/-! ## variability (L6185-6190) -/

/-- Prim definition, else the *weakest* opinion. -/
def variability {V : Type} (schema : Option V) (os : List (Option V)) : Option V :=
  match schema with
  | some v => some v
  | none => strongest os.reverse

theorem variability_eq_foldl {V : Type} (os : List (Option V)) :
    variability none os = os.foldl (fun acc a => a <|> acc) none := by
  simp only [variability, strongest_eq_foldr, List.foldr_reverse]

/-- The weakest opinion wins: a stronger opinion never changes the result once a
weaker one is authored. CLAIM: "value resolution will return the *weakest*
opinion" (L6189-6190). -/
theorem variability_weakest {V : Type} (os : List (Option V)) (v : V) :
    variability none (os ++ [some v]) = some v := by
  simp [variability, strongest]

/-! ## custom (L6192-6198) -/

/-- `true` iff any opinion is `true`; schema-defined properties are never custom. -/
def custom (schemaDefined : Bool) (os : List (Option Bool)) : Bool :=
  !schemaDefined && os.any (· == some true)

theorem custom_eq_fold (os : List (Option Bool)) :
    custom false os = os.foldr (fun o acc => (o == some true) || acc) false := by
  induction os with
  | nil => rfl
  | cons o os ih => simp_all [custom, List.any_cons]

/-- OR monoid: the stack can be split anywhere. -/
theorem custom_append (a b : List (Option Bool)) :
    custom false (a ++ b) = (custom false a || custom false b) := by
  simp [custom, List.any_append]

theorem custom_schema (os : List (Option Bool)) : custom true os = false := by simp [custom]

/-! ## Dictionaries (L6200-6236) -/

/-- Dictionary field: combine the authored dictionaries with `∪`, strongest first. -/
def dictField {α : Type} [DecidableEq α] (os : List (Option (Dict α))) : Dict α :=
  Dict.resolveDict (os.filterMap id)

/-- The spec's `customData` example L6211-6235: `def sphere {a = true}` over
`over {b = false}` resolves to `{a = true, b = false}`. -/
example : dictField [some [("a", .leaf true)], some [("b", .leaf false)]] =
    ([("a", .leaf true), ("b", .leaf false)] : Dict Bool) := by decide

/-- On shape-compatible stacks (no leaf-vs-dict clash, LEAN-101) the fold
direction does not matter. -/
theorem dictField_eq_foldr {α : Type} [DecidableEq α] (os : List (Option (Dict α)))
    (hwf : ∀ d ∈ os.filterMap id, Dict.WF d)
    (hc : ∀ d₁ ∈ os.filterMap id, ∀ d₂ ∈ os.filterMap id, Dict.Compat (.dict d₁) (.dict d₂)) :
    dictField os = (os.filterMap id).foldr Dict.merge [] := by
  unfold dictField
  exact Dict.resolveDict_eq_resolveDictR_of_compat _ hwf hc

/-! ## List ops (L6238-6255) -/

/-- List-op field: the chain of the authored opinions, strongest first. -/
def listField {α : Type} [DecidableEq α] (os : List (Option (ListOp α))) : ListOp α :=
  ListOp.chain (os.filterMap id)

/-- Applying the resolved list op = applying the opinions weakest to strongest.
CLAIM: "combines the entire stack of values" (L6240-6242). -/
theorem listField_eq {α : Type} [DecidableEq α] (os : List (Option (ListOp α))) (l : List α) :
    ListOp.apply (listField os) l = (os.filterMap id).foldr (fun L acc => ListOp.apply L acc) l :=
  ListOp.apply_chain _ l

/-- Pruning at the first explicit opinion is congruent (L6245-6247). -/
theorem listField_prune {α : Type} [DecidableEq α] (pre post : List (ListOp α)) (e : List α) :
    ListOp.chain (pre ++ ListOp.explicit e :: post) ≅ ListOp.chain (pre ++ [ListOp.explicit e]) := by
  rw [ListOp.congruent_iff_apply]; intro l
  rw [ListOp.apply_chain, ListOp.apply_chain, List.foldr_append, List.foldr_append]
  rfl

/-! ## Layer metadata (L6257-6260) -/

/-- Only the root layer's opinion counts; sublayers and arcs are ignored. -/
def layerMeta {α : Type} (root : Option α) (_others : List (Option α)) : Option α := root

theorem layerMeta_ignores {α : Type} (root : Option α) (a b : List (Option α)) :
    layerMeta root a = layerMeta root b := rfl

/-! ## specifier (L6118-6131)

An opinion is a `(specifier, viaDirectInherit)` pair. `viaDirectInherit` holds
when the spec's prim-index node, or one of its ancestors in the graph, was
introduced by a non-ancestral inherit arc (OpenUSD: `PcpIsInheritArc(node) &&
!node.IsDueToAncestor()` walking `GetParentNode`, `pxr/usd/usd/stage.cpp`
`_GetPrimSpecifierImpl`; LightUSD: `src/next/pcp/cache-opinions.cc`
`ComposeOpinions` — "a class reached through an ancestral inherit resolves in
plain strength order; only a DIRECT inherit class is weaker than a def"). -/

inductive Specifier
  | «def»
  | over
  | «class»
  deriving DecidableEq, Repr

structure SpecOpinion where
  spec : Specifier
  viaDirectInherit : Bool
  deriving DecidableEq, Repr

def SpecOpinion.defining (o : SpecOpinion) : Bool := o.spec != .over

/-- Bullet 1: all opinions are `over`. -/
def undefining (os : List SpecOpinion) : Bool := os.all (·.spec == .over)

def strongestDefining (os : List SpecOpinion) : Option Specifier :=
  (os.find? (·.defining)).map (·.spec)

def strongestDefiningNonDirect (os : List SpecOpinion) : Option Specifier :=
  (os.find? (fun o => o.defining && !o.viaDirectInherit)).map (·.spec)

/-- "all of the prim's defining specifiers are `class`" (read non-vacuously). -/
def allDefiningClass (os : List SpecOpinion) : Bool :=
  os.any (·.defining) && (os.filter (·.defining)).all (·.spec == .class)

/-- Bullet 2 (L6126-6128). -/
def classBullet (os : List SpecOpinion) : Bool :=
  strongestDefiningNonDirect os == some .class || allDefiningClass os

/-- Bullet 3 (L6129-6131). -/
def defBullet (os : List SpecOpinion) : Bool :=
  strongestDefining os == some .def || strongestDefiningNonDirect os == some .def

/-- OpenUSD / LightUSD resolution: scan strongest first, skip `over`; `def` wins;
a `class` not via a direct inherit wins; a direct-inherit `class` is remembered
and scanning continues for a `def`. -/
def resolveSpecifier : List SpecOpinion → Specifier
  | [] => .over
  | o :: os =>
    match o.spec with
    | .over => resolveSpecifier os
    | .def => .def
    | .class =>
      if !o.viaDirectInherit then .class
      else match resolveSpecifier os with
        | .def => .def
        | _ => .class

/-- Characterization: the first opinion that is a `def` or a non-direct `class`
decides; otherwise `class` if anything is defining, else `over`. -/
theorem resolveSpecifier_eq (os : List SpecOpinion) :
    resolveSpecifier os =
      match os.find? (fun o => o.spec == .def || (o.spec == .class && !o.viaDirectInherit)) with
      | some o => o.spec
      | none => if os.any (·.defining) then .class else .over := by
  induction os with
  | nil => rfl
  | cons o os ih =>
    obtain ⟨s, d⟩ := o
    cases s <;> cases d
    · rfl
    · rfl
    · simp only [resolveSpecifier, List.find?_cons, SpecOpinion.defining, List.any_cons, ih]
      rfl
    · simp only [resolveSpecifier, List.find?_cons, SpecOpinion.defining, List.any_cons, ih]
      rfl
    · rfl
    · simp only [resolveSpecifier, List.find?_cons, SpecOpinion.defining, List.any_cons, ih]
      cases h : List.find? (fun o => o.spec == Specifier.def ||
          (o.spec == Specifier.class && !o.viaDirectInherit)) os with
      | none =>
        by_cases ha : (os.any fun x => x.spec != Specifier.over) = true <;> simp [ha] <;> rfl
      | some o =>
        have hp := List.find?_some h
        obtain ⟨s', d'⟩ := o
        cases s' <;> cases d' <;> simp_all <;> rw [show (Specifier.class == Specifier.def) = false from rfl]

/-- `over` iff bullet 1. -/
theorem resolveSpecifier_over_iff (os : List SpecOpinion) :
    resolveSpecifier os = .over ↔ undefining os = true := by
  induction os with
  | nil => simp [resolveSpecifier, undefining]
  | cons o os ih =>
    obtain ⟨s, d⟩ := o
    cases s <;> cases d <;> simp [resolveSpecifier, undefining] at * <;>
      first | exact ih | (split <;> simp_all)

/-- The two defining bullets overlap. `over "A" (inherits = </C>, references = </B>)`
with `def "C"` and `class "B"` (the direct-inherit spec `def C` is stronger than
the referenced `class B`): the strongest defining opinion is `def` (bullet 3)
and the strongest defining opinion not from a direct inherit is `class`
(bullet 2). OpenUSD 26.11 and the scan resolve `def`.
SPEC-ISSUE: LEAN-701 — the specifier bullets L6124-6131 are neither exclusive nor exhaustive. -/
def overlapEx : List SpecOpinion := [⟨.over, false⟩, ⟨.def, true⟩, ⟨.class, false⟩]

theorem specifier_bullets_overlap :
    classBullet overlapEx = true ∧ defBullet overlapEx = true ∧
      resolveSpecifier overlapEx = .def := by decide

/-- …and leave a gap. `over "A" (inherits = [</C1>, </C2>])`, `class "C1"`,
`def "C2"`: every defining opinion is via a direct inherit, the strongest is
`class`, and not all are `class`; no bullet applies. OpenUSD 26.11 resolves
`def`. SPEC-ISSUE: LEAN-701. -/
def gapEx : List SpecOpinion := [⟨.over, false⟩, ⟨.class, true⟩, ⟨.def, true⟩]

theorem specifier_bullets_gap :
    undefining gapEx = false ∧ classBullet gapEx = false ∧ defBullet gapEx = false ∧
      resolveSpecifier gapEx = .def := by decide

/-- The pxr comment example (stage.cpp): `over A` → `class C` (root, direct
inherit implied) → `def B` (reference) → `class C` (other): `def`. -/
example : resolveSpecifier [⟨.over, false⟩, ⟨.class, true⟩, ⟨.def, false⟩, ⟨.class, true⟩] = .def := by
  decide

/-- …and even contradict OpenUSD when exactly one bullet holds.
`over "A" (inherits = [</C1>, </C2>], references = </B>)` with `class "C1"`,
`def "C2"`, `class "B"`: only bullet 2 holds (the strongest defining opinion not
from a direct inherit is `B`'s `class`; the strongest defining is `class`), so
the text resolves `class`; OpenUSD 26.11 resolves `def` (the direct-inherit
`class C1` is skipped and the scan finds `def C2` before `class B`).
SPEC-ISSUE: LEAN-701. -/
def contraEx : List SpecOpinion := [⟨.over, false⟩, ⟨.class, true⟩, ⟨.def, true⟩, ⟨.class, false⟩]

theorem specifier_bullet_contradicts_pxr :
    undefining contraEx = false ∧ classBullet contraEx = true ∧ defBullet contraEx = false ∧
      resolveSpecifier contraEx = .def := by decide

/-- Exhaustive check: where exactly one bullet holds, the scan agrees with it on
every stack of up to two opinions; with three opinions the only disagreements
are the `contraEx` pattern (direct `class`, then direct `def`, then non-direct
`class`). -/
def allOps : List SpecOpinion :=
  [⟨.def, false⟩, ⟨.def, true⟩, ⟨.over, false⟩, ⟨.over, true⟩, ⟨.class, false⟩, ⟨.class, true⟩]

def agreesWhenUnique (os : List SpecOpinion) : Bool :=
  match undefining os, classBullet os, defBullet os with
  | true, false, false => resolveSpecifier os == .over
  | false, true, false => resolveSpecifier os == .class
  | false, false, true => resolveSpecifier os == .def
  | _, _, _ => true

theorem agreesWhenUnique_le2 :
    (allOps.flatMap fun a => allOps.map fun b => [a, b]).all agreesWhenUnique = true ∧
    (allOps.map fun a => [a]).all agreesWhenUnique = true := by decide

theorem disagreements_3 :
    (allOps.flatMap fun a => allOps.flatMap fun b => allOps.map fun c => [a, b, c]).filter
      (fun l => !agreesWhenUnique l) = [[⟨.class, true⟩, ⟨.def, true⟩, ⟨.class, false⟩]] := by decide

end UsdSpec.ValueResolution.Metadata
