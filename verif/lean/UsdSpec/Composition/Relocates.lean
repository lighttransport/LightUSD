import Mathlib.Data.List.Basic
import Mathlib.Data.List.Infix

/-!
# Relocates (AOUSD USD Core Specification v1.1.0, §10.3.7)

SPEC: core_spec.md L4876-5009 (Relocates); restriction list L4899-4917; the
source-opinion rule L4895-4897; `layerRelocates` field L2467-2489 (an unset
target means the source is removed, L2480-2481).

Paths are absolute prim paths as name lists (`[]` = `/`), as in
`UsdSpec/Composition/NamespaceMap.lean` (kept local so this file stands alone).

`entryError` checks one entry against the bullet list (one `RelocateError`
constructor per bullet), `validRelocates` the whole list, `Valid` is the Prop
form (`validRelocates_ok_iff`). `applyRelocates` walks a path root-to-leaf and
relocates each prefix that is a source; because of the "ancestral relocates"
bullet, sources of nested relocates are written in the already-relocated
namespace, which is exactly what the walk sees.

SPEC-ISSUE: LEAN-514 — the bullet "A target path must not be the same as a
source path" (L4910) is modelled literally (per entry). pxr 26.11 and LightUSD
read it across entries ("The target of a relocate cannot be the source of
another relocate in the same layer stack", invalidating *both* entries, not
"that entry" of L4900); under the literal reading the chain `/P/A→/P/B`,
`/P/C→/P/A` is valid (`chain_valid_literal`) and triggers LEAN-601
(UsdSpec/Population/Children.lean).

## Reflection: LightUSD `BuildStackRelocates`

Declared src/next/pcp/cache-layer-stack.inc:34, defined
src/next/pcp/cache-layers.cc:84-249. Candidates are deduplicated identical
`(src,dst)` pairs from layer metadata and per-prim `relocates` (L85-100).
Coverage of the bullets (L4902-4917):

* L4902 pseudo-root / root prim: PARTIAL. Source empty or a root prim is
  rejected (cache-layers.cc:119, 122 `sp.parent().str() == "/"`); a target that
  is `/` is rejected only indirectly (every source is under `/`, L121); a
  *root-prim target* (`/A/B → /C`) is deliberately accepted by OpenUSD
  (`Pcp_IsValidRelocatesEntry` and `_IsValidRelocatesEntryForComposing`,
  v26.11.1). Stock 26.11 relocates `/P/Child` to `/Moved` without errors.
  SPEC-ISSUE: LEAN-515 — L4902 forbids root-prim paths without distinguishing
  source and target, whereas OpenUSD forbids root-prim sources only. Preserve
  this compatibility behavior; `OkRootOpenUsd` records the distinction.
* L4903 prim paths: NOT checked here (paths are strings; a property path
  would pass).
* L4904 target at an existing ancestor: YES, `IsPathAtOrUnder(src, dst)` (L121).
* L4906 target below source: YES, `IsPathAtOrUnder(dst, src)` (L121).
* L4908 one-to-one: PARTIAL. Two sources with the same non-empty target are
  both invalidated (L176); one source with two *different* targets is NOT
  detected (only identical pairs are deduplicated, L93; `src_to_dst` keeps the
  last, L238).
* L4910 target equals source: YES for the same entry (`src == dst`, L119) and
  in the cross-entry reading of LEAN-514 (`a.second == b.first ||
  b.second == a.first`, L176-177, both invalidated).
* L4911 ancestral relocates: YES, a source strictly below another source is
  invalid (`strict_descendant(a.first, b.first)`, L184-187).
* Beyond the spec: a target strictly below another source (L184-187); a source
  that is purely local (no arc on a strict ancestor) is dropped, pxr's "Cannot
  relocate a prim defined in the local layer stack" (L124-166). The validator
  rules `core.layer.layerRelocates.{identity,source,target}`
  (src/next/validation/usd-validation-report.cc:199-203) report authoring-level
  problems separately.
-/

namespace UsdSpec.Relocates

abbrev PrimPath := List String

/-- A `layerRelocates` entry; `dst = none` is the unset target (removal),
SPEC: L2473-2481. -/
structure Relocate where
  src : PrimPath
  dst : Option PrimPath
  deriving DecidableEq, Repr

/-- One constructor per restriction bullet. SPEC: L4899-4917. -/
inductive RelocateError
  | rootPath (r : Relocate)          -- L4902
  | notPrimPath (r : Relocate)       -- L4903
  | targetIsAncestor (r : Relocate)  -- L4904-4905
  | targetBelowSource (r : Relocate) -- L4906-4907
  | notOneToOne (r : Relocate)       -- L4908-4909
  | targetIsSource (r : Relocate)    -- L4910
  | ancestralNotUsed (r : Relocate)  -- L4911-4917
  deriving DecidableEq, Repr

def StrictPrefix (a b : PrimPath) : Prop := a <+: b ∧ a ≠ b

instance (a b : PrimPath) : Decidable (StrictPrefix a b) := by
  unfold StrictPrefix; infer_instance

/-- Pseudo-root (`[]`) or root prim (`[n]`). -/
def IsRootish (p : PrimPath) : Prop := p.length ≤ 1

/-- Prim name component: nonempty, no property/variant/path punctuation. -/
def isPrimName (s : String) : Bool :=
  !s.isEmpty && s.toList.all (fun c => c ∉ ['.', '{', '}', '/', '[', ']', ':', '='])

def OkRoot (r : Relocate) : Prop := ¬ IsRootish r.src ∧ ∀ d ∈ r.dst, ¬ IsRootish d

/-- OpenUSD compatibility for LEAN-515: root-prim targets are allowed; the
source must be below a root prim and a nonempty target must be a prim path.
Other path validity restrictions remain the responsibility of `OkPrim`. -/
def OkRootOpenUsd (r : Relocate) : Prop :=
  ¬ IsRootish r.src ∧ ∀ d ∈ r.dst, d ≠ []

instance : DecidablePred OkRootOpenUsd := fun r => by
  unfold OkRootOpenUsd IsRootish; infer_instance

/-- Stock 26.11 accepts this root-target relocation, unlike literal L4902. -/
theorem rootTarget_openUsd_only :
    let r : Relocate := ⟨["P", "Child"], some ["Moved"]⟩
    OkRootOpenUsd r ∧ ¬ OkRoot r := by
  simp [OkRootOpenUsd, OkRoot, IsRootish]

def OkPrim (r : Relocate) : Prop :=
  (∀ s ∈ r.src, isPrimName s = true) ∧ ∀ d ∈ r.dst, ∀ s ∈ d, isPrimName s = true
def OkAncestor (r : Relocate) : Prop := ∀ d ∈ r.dst, ¬ StrictPrefix d r.src
def OkBelow (r : Relocate) : Prop := ∀ d ∈ r.dst, ¬ StrictPrefix r.src d
def OkOneToOne (rs : List Relocate) (r : Relocate) : Prop :=
  ∀ r' ∈ rs, (r'.src = r.src → r'.dst = r.dst) ∧ (∀ d ∈ r.dst, r'.dst = some d → r'.src = r.src)
def OkSame (r : Relocate) : Prop := r.dst ≠ some r.src
def OkAncestral (rs : List Relocate) (r : Relocate) : Prop :=
  ∀ r' ∈ rs, ¬ StrictPrefix r'.src r.src

instance : DecidablePred OkRoot := fun r => by unfold OkRoot IsRootish; infer_instance
instance : DecidablePred OkPrim := fun r => by unfold OkPrim; infer_instance
instance : DecidablePred OkAncestor := fun r => by unfold OkAncestor; infer_instance
instance : DecidablePred OkBelow := fun r => by unfold OkBelow; infer_instance
instance (rs) : DecidablePred (OkOneToOne rs) := fun r => by unfold OkOneToOne; infer_instance
instance : DecidablePred OkSame := fun r => by unfold OkSame; infer_instance
instance (rs) : DecidablePred (OkAncestral rs) := fun r => by unfold OkAncestral; infer_instance

/-- All bullets for one entry of `rs`. -/
def EntryOk (rs : List Relocate) (r : Relocate) : Prop :=
  OkRoot r ∧ OkPrim r ∧ OkAncestor r ∧ OkBelow r ∧ OkOneToOne rs r ∧ OkSame r ∧ OkAncestral rs r

/-- First violated bullet of entry `r`. SPEC: core_spec.md L4899-4917 (Relocates). -/
def entryError (rs : List Relocate) (r : Relocate) : Option RelocateError :=
  if ¬ OkRoot r then some (.rootPath r)
  else if ¬ OkPrim r then some (.notPrimPath r)
  else if ¬ OkAncestor r then some (.targetIsAncestor r)
  else if ¬ OkBelow r then some (.targetBelowSource r)
  else if ¬ OkOneToOne rs r then some (.notOneToOne r)
  else if ¬ OkSame r then some (.targetIsSource r)
  else if ¬ OkAncestral rs r then some (.ancestralNotUsed r)
  else none

/-- Checker. SPEC: core_spec.md L4899-4917 (Relocates). -/
def validRelocates (rs : List Relocate) : Except RelocateError Unit :=
  match rs.findSome? (entryError rs) with
  | some e => .error e
  | none => .ok ()

def Valid (rs : List Relocate) : Prop := ∀ r ∈ rs, EntryOk rs r

theorem entryError_eq_none_iff (rs : List Relocate) (r : Relocate) :
    entryError rs r = none ↔ EntryOk rs r := by
  unfold entryError EntryOk
  split_ifs <;> simp_all

theorem validRelocates_ok_iff (rs : List Relocate) : validRelocates rs = .ok () ↔ Valid rs := by
  unfold validRelocates Valid
  cases h : rs.findSome? (entryError rs) with
  | some e =>
    simp only [reduceCtorEq, false_iff]
    intro hv
    obtain ⟨r, hr, he⟩ := List.exists_of_findSome?_eq_some h
    rw [(entryError_eq_none_iff rs r).mpr (hv r hr)] at he
    cases he
  | none =>
    simp only [true_iff]
    intro r hr
    exact (entryError_eq_none_iff rs r).mp (List.findSome?_eq_none_iff.mp h r hr)

instance (rs : List Relocate) : Decidable (Valid rs) :=
  decidable_of_iff _ (validRelocates_ok_iff rs)

/-! ## Each bullet on an example -/

def mk (s : List String) (d : List String) : Relocate := ⟨s, some d⟩

/-- L4902. -/
example : validRelocates [mk ["Root"] ["Other"]] = .error (.rootPath (mk ["Root"] ["Other"])) := by
  decide
/-- L4903 (a property path). -/
example : validRelocates [mk ["A", "b.attr"] ["A", "c"]] =
    .error (.notPrimPath (mk ["A", "b.attr"] ["A", "c"])) := by decide
/-- L4904-4905: `/Prim/Child/GrandChild` to `/Prim/Child`. -/
example : validRelocates [mk ["Prim", "Child", "GrandChild"] ["Prim", "Child"]] =
    .error (.targetIsAncestor (mk ["Prim", "Child", "GrandChild"] ["Prim", "Child"])) := by decide
/-- L4906-4907: `/Prim/Child` to `/Prim/Child/GrandChild`. -/
example : validRelocates [mk ["Prim", "Child"] ["Prim", "Child", "GrandChild"]] =
    .error (.targetBelowSource (mk ["Prim", "Child"] ["Prim", "Child", "GrandChild"])) := by decide
/-- L4908-4909: one source, two targets; and two sources, one target. -/
example : validRelocates [mk ["A", "B"] ["A", "C"], mk ["A", "B"] ["A", "D"]] =
    .error (.notOneToOne (mk ["A", "B"] ["A", "C"])) := by decide
example : validRelocates [mk ["A", "B"] ["A", "C"], mk ["A", "D"] ["A", "C"]] =
    .error (.notOneToOne (mk ["A", "B"] ["A", "C"])) := by decide
/-- L4910. -/
example : validRelocates [mk ["A", "B"] ["A", "B"]] =
    .error (.targetIsSource (mk ["A", "B"] ["A", "B"])) := by decide
/-- L4911-4917: `/Root/Ref → /Root/RefRelocated` with a second relocate whose source
is `/Root/Ref/Ref2` (must be `/Root/RefRelocated/Ref2`). -/
example : validRelocates [mk ["Root", "Ref"] ["Root", "RefRelocated"],
      mk ["Root", "Ref", "Ref2"] ["Root", "X"]] =
    .error (.ancestralNotUsed (mk ["Root", "Ref", "Ref2"] ["Root", "X"])) := by decide
example : Valid [mk ["Root", "Ref"] ["Root", "RefRelocated"],
    mk ["Root", "RefRelocated", "Ref2"] ["Root", "X"]] := by decide
/-- The §10.3.7 namespace-mapping example (L4934-4939) and a removal (L2480). -/
example : Valid [mk ["A", "B"] ["A", "C"], mk ["Other", "B"] ["Other", "C"]] := by decide
example : Valid [⟨["A", "B"], none⟩, ⟨["A", "C"], none⟩] := by decide

/-- SPEC-ISSUE: LEAN-514 — a target that is another entry's source passes the
literal bullets; pxr 26.11 rejects both entries. -/
theorem chain_valid_literal : Valid [mk ["P", "A"] ["P", "B"], mk ["P", "C"] ["P", "A"]] := by
  decide

/-! ## One-to-one ⇒ the induced map is injective -/

theorem target_injective {rs : List Relocate} (hv : Valid rs) {r₁ r₂ : Relocate}
    (h₁ : r₁ ∈ rs) (h₂ : r₂ ∈ rs) {d : PrimPath} (e₁ : r₁.dst = some d) (e₂ : r₂.dst = some d) :
    r₁.src = r₂.src :=
  (((hv r₁ h₁).2.2.2.2.1 r₂ h₂).2 d (by simp [e₁]) e₂).symm

theorem source_functional {rs : List Relocate} (hv : Valid rs) {r₁ r₂ : Relocate}
    (h₁ : r₁ ∈ rs) (h₂ : r₂ ∈ rs) (e : r₁.src = r₂.src) : r₁.dst = r₂.dst :=
  (((hv r₂ h₂).2.2.2.2.1 r₁ h₁).1 e)

/-! ## Effect on namespace -/

/-- Walk the remaining components `rest`, `cur` being the already relocated
prefix; a prefix equal to a source moves to its target, or the path is removed
for an unset target. -/
def walk (rs : List Relocate) : PrimPath → PrimPath → Option PrimPath
  | cur, [] => some cur
  | cur, c :: rest =>
    match rs.find? (fun r => decide (r.src = cur ++ [c])) with
    | none => walk rs (cur ++ [c]) rest
    | some r =>
      match r.dst with
      | none => none
      | some d => walk rs d rest

/-- Map a path through a relocate set, ancestors first (L4911-4917). -/
def applyRelocates (rs : List Relocate) (p : PrimPath) : Option PrimPath := walk rs [] p

theorem walk_result (rs : List Relocate) (cur p q : PrimPath) (h : walk rs cur p = some q) :
    (p = [] ∧ q = cur) ∨ (∀ r ∈ rs, r.src ≠ q) ∨ (∃ r ∈ rs, r.dst = some q) := by
  induction p generalizing cur with
  | nil => simp [walk] at h; exact Or.inl ⟨rfl, h.symm⟩
  | cons c rest ih =>
    simp only [walk] at h
    split at h
    · rename_i hf
      rcases ih _ h with ⟨h1, h2⟩ | h1 | h1
      · subst h1; subst h2
        right; left
        intro r hr hrq
        have := List.find?_eq_none.mp hf r hr
        simp [hrq] at this
      · exact Or.inr (Or.inl h1)
      · exact Or.inr (Or.inr h1)
    · rename_i r hf
      split at h
      · cases h
      · rename_i d hd
        rcases ih _ h with ⟨h1, h2⟩ | h1 | h1
        · subst h2
          exact Or.inr (Or.inr ⟨r, List.mem_of_find?_eq_some hf, hd⟩)
        · exact Or.inr (Or.inl h1)
        · exact Or.inr (Or.inr h1)

/-- After applying a valid relocate set, a resulting path equals a relocate
source only if it is also a relocate target. SPEC: L4889-4897. -/
theorem relocated_source_not_present {rs : List Relocate} (hv : Valid rs) {p q : PrimPath}
    (h : applyRelocates rs p = some q) {r : Relocate} (hr : r ∈ rs) (hq : r.src = q) :
    ∃ r' ∈ rs, r'.dst = some q := by
  rcases walk_result rs [] p q h with ⟨_, h2⟩ | h1 | h1
  · subst h2
    have := (hv r hr).1.1
    simp [IsRootish, hq] at this
  · exact absurd hq (h1 r hr)
  · exact h1

/-- The ancestral example: `/Root/Ref/Ref2` reaches `/Root/X` via the relocated path. -/
example : applyRelocates [mk ["Root", "Ref"] ["Root", "RefRelocated"],
    mk ["Root", "RefRelocated", "Ref2"] ["Root", "X"]] ["Root", "Ref", "Ref2", "Leaf"] =
    some ["Root", "X", "Leaf"] := by decide
/-- An un-relocated source never fires once its ancestor moved. -/
example : applyRelocates [mk ["Root", "Ref"] ["Root", "RefRelocated"],
    mk ["Root", "Ref", "Ref2"] ["Root", "X"]] ["Root", "Ref", "Ref2"] =
    some ["Root", "RefRelocated", "Ref2"] := by decide
/-- Removal (unset target). -/
example : applyRelocates [⟨["A", "B"], none⟩] ["A", "B", "C"] = none := by decide

end UsdSpec.Relocates
