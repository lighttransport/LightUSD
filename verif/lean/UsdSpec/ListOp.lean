import Mathlib.Data.List.Basic
import Mathlib.Data.Fintype.EquivFin
/-!
# List operations (AOUSD USD Core Specification v1.1.0, §6.6.3)

SPEC: core_spec.md L1271-1708 (6.6.3 List Operations).

Every definition is executable (`#eval`, `decide`) and transcribed bullet-for-bullet
from the LaTeX math blocks of the spec (`combine`; `apply`, `WellFormed` and the
bridge theorems are derived, not spec text; some results need `[Infinite α]`). Element sequences are plain `List α`; the
spec's uniqueness requirement (L1308-1337) is carried as `List.Nodup` hypotheses
where a theorem needs it, never as a subtype.

Spec issues found while formalizing are tagged `SPEC-ISSUE: LEAN-NNN` and listed in
`verif/lean/ISSUES.md`.
-/

namespace UsdSpec

/-- The three sequences of a composable list operation. SPEC: L1324-1341. -/
structure Composable (α : Type) where
  deleted   : List α
  prepended : List α
  appended  : List α
  deriving DecidableEq, Repr, BEq, Inhabited

/-- A list operation is either explicit or composable. SPEC: L1298-1322, L1362-1367. -/
inductive ListOp (α : Type)
  | explicit (items : List α)
  | comp (c : Composable α)
  deriving DecidableEq, Repr, BEq, Inhabited

namespace ListOp

variable {α : Type} [DecidableEq α]

/-- The normative default value `I′`. SPEC: L1343-1351. -/
def default : ListOp α := .comp ⟨[], [], []⟩

omit [DecidableEq α] in
/-- `I′ ≠ (explicit: ⟨⟩)`. SPEC: L1353-1360. -/
theorem default_ne_explicit_nil : (default : ListOp α) ≠ .explicit [] := by
  simp [default]

def isExplicit : ListOp α → Bool
  | .explicit _ => true
  | .comp _ => false

/-- Iteration. SPEC: L1369-1388. The deleted sequence is ignored (L1387-1388). -/
def iterate : ListOp α → List α
  | .explicit e => e
  | .comp c => c.prepended.filter (· ∉ c.appended) ++ c.appended

/-- Combining `⊔`. SPEC: L1421-1483.
  * stronger explicit absorbs (L1433-1438);
  * composable over explicit yields explicit with the three-part ordering L1448-1453;
  * composable over composable: deleted / prepended / appended orderings L1469-1482. -/
def combine : ListOp α → ListOp α → ListOp α
  | .explicit e, _ => .explicit e
  | .comp s, .explicit e =>
      .explicit (s.prepended.filter (· ∉ s.appended)
        ++ e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧ x ∉ s.prepended)
        ++ s.appended)
  | .comp s, .comp c =>
      .comp {
        deleted :=
          c.deleted.filter (fun x => x ∉ s.prepended ∧ x ∉ s.appended)
          ++ s.deleted.filter (fun x => x ∉ s.prepended ∧ x ∉ s.appended ∧ x ∉ c.deleted),
        prepended :=
          s.prepended.filter (· ∉ s.appended)
          ++ c.prepended.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧ x ∉ s.prepended),
        appended :=
          c.appended.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧ x ∉ s.prepended)
          ++ s.appended }

@[inherit_doc combine] infixl:65 " ⊔ " => combine

/-- Applying a list operation as an *edit* to a weaker, already-iterated element list.
This is the operation implementations actually run (LightUSD `ApplyStringListOp`);
`iterate_combine` ties it to `⊔`. It is the composable-over-explicit case of `⊔`
read as a function of the explicit items. -/
def apply : ListOp α → List α → List α
  | .explicit e, _ => e
  | .comp c, w =>
      c.prepended.filter (· ∉ c.appended)
      ++ w.filter (fun x => x ∉ c.appended ∧ x ∉ c.deleted ∧ x ∉ c.prepended)
      ++ c.appended

/-- The admissibility condition from L1298-1337: explicit sequences are unique,
and each composable sub-sequence is unique. Overlap between different
composable fields is allowed by the spec. -/
def WellFormed : ListOp α → Prop
  | .explicit items => items.Nodup
  | .comp c => c.appended.Nodup ∧ c.prepended.Nodup ∧ c.deleted.Nodup

private theorem nodup_filter {β : Type} (xs : List β) (p : β → Bool)
    (h : xs.Nodup) : (xs.filter p).Nodup := by
  induction xs with
  | nil => simp
  | cons x xs ih =>
    rw [List.nodup_cons] at h
    obtain ⟨hnot, hnodup⟩ := h
    simp only [List.filter_cons]
    split_ifs with hp
    · rw [List.nodup_cons]
      exact ⟨fun hx => hnot ((List.mem_filter.mp hx).1), ih hnodup⟩
    · exact ih hnodup

private theorem nodup_append_of_disjoint {β : Type} [DecidableEq β]
    (xs ys : List β) (hx : xs.Nodup) (hy : ys.Nodup)
    (hxy : ∀ x ∈ xs, x ∉ ys) : (xs ++ ys).Nodup := by
  rw [List.nodup_append]
  refine ⟨hx, hy, ?_⟩
  intro x hx y hy' hxy'
  subst y
  exact hxy x hx hy'

/-- Applying a well-formed list operation to a unique weaker ordering preserves
element uniqueness, as required by the list-operation domain (L1271-1337).
The pre/app overlap is removed from the prepended run, and each weaker element
shadowed by any authored edit is removed before the appended run. -/
theorem apply_nodup (L : ListOp α) (weaker : List α)
    (hL : WellFormed L) (hw : weaker.Nodup) : (apply L weaker).Nodup := by
  cases L with
  | explicit items => exact hL
  | comp c =>
    obtain ⟨ha, hp, hd⟩ := hL
    let pre := c.prepended.filter (· ∉ c.appended)
    let middle := weaker.filter
      (fun x => x ∉ c.appended ∧ x ∉ c.deleted ∧ x ∉ c.prepended)
    have hpre : pre.Nodup := nodup_filter _ _ hp
    have hmiddle : middle.Nodup := nodup_filter _ _ hw
    have hpm : ∀ x ∈ pre, x ∉ middle := by
      intro x hx hm
      have hxpre : x ∈ c.prepended := (List.mem_filter.mp hx).1
      have hmpre := of_decide_eq_true (List.mem_filter.mp hm).2
      rcases hmpre with ⟨_, _, hmpre⟩
      exact hmpre hxpre
    have hpa : ∀ x ∈ pre, x ∉ c.appended := by
      intro x hx
      exact of_decide_eq_true (List.mem_filter.mp hx).2
    have hma : ∀ x ∈ middle, x ∉ c.appended := by
      intro x hx
      exact (of_decide_eq_true (List.mem_filter.mp hx).2).1
    have hfirst := nodup_append_of_disjoint pre middle hpre hmiddle hpm
    have hlast := nodup_append_of_disjoint (pre ++ middle) c.appended hfirst ha
      (by
        intro x hx ha'
        rcases List.mem_append.mp hx with hp' | hm'
        · exact hpa x hp' ha'
        · exact hma x hm' ha')
    simpa [apply, pre, middle, List.append_assoc] using hlast

/-- The combining operation is closed over the spec's well-formed list-op
domain (L1421-1483): every output sequence remains duplicate-free. -/
theorem combine_wellFormed (S W : ListOp α)
    (hS : WellFormed S) (hW : WellFormed W) : WellFormed (S ⊔ W) := by
  cases S with
  | explicit e => exact hS
  | comp s =>
    cases W with
    | explicit e =>
      obtain ⟨hsA, hsP, hsD⟩ := hS
      have hpre : (s.prepended.filter (· ∉ s.appended)).Nodup :=
        nodup_filter _ _ hsP
      have hmid : (e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧
          x ∉ s.prepended)).Nodup := nodup_filter _ _ hW
      have hpm : ∀ x ∈ s.prepended.filter (· ∉ s.appended),
          x ∉ e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧
            x ∉ s.prepended) := by
        intro x hx hm
        have hxpre := (List.mem_filter.mp hx).1
        have hmid' := of_decide_eq_true (List.mem_filter.mp hm).2
        exact hmid'.2.2 hxpre
      have hpa : ∀ x ∈ s.prepended.filter (· ∉ s.appended), x ∉ s.appended := by
        intro x hx; exact of_decide_eq_true (List.mem_filter.mp hx).2
      have hma : ∀ x ∈ e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧
          x ∉ s.prepended), x ∉ s.appended := by
        intro x hx
        exact (of_decide_eq_true (List.mem_filter.mp hx).2).1
      have hfirst := nodup_append_of_disjoint
        (s.prepended.filter (· ∉ s.appended))
        (e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧
          x ∉ s.prepended)) hpre hmid hpm
      have hlast := nodup_append_of_disjoint
        (s.prepended.filter (· ∉ s.appended) ++
          e.filter (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧
            x ∉ s.prepended)) s.appended hfirst hsA
        (by
          intro x hx ha
          rcases List.mem_append.mp hx with hp | he
          · exact hpa x hp ha
          · exact hma x he ha)
      simpa [WellFormed, combine, List.append_assoc] using hlast
    | comp c =>
      obtain ⟨hsA, hsP, hsD⟩ := hS
      obtain ⟨hcA, hcP, hcD⟩ := hW
      let d1 := c.deleted.filter (fun x => x ∉ s.prepended ∧ x ∉ s.appended)
      let d2 := s.deleted.filter
        (fun x => x ∉ s.prepended ∧ x ∉ s.appended ∧ x ∉ c.deleted)
      let p1 := s.prepended.filter (· ∉ s.appended)
      let p2 := c.prepended.filter
        (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧ x ∉ s.prepended)
      let a1 := c.appended.filter
        (fun x => x ∉ s.appended ∧ x ∉ s.deleted ∧ x ∉ s.prepended)
      let a2 := s.appended
      have hd1 : d1.Nodup := nodup_filter _ _ hcD
      have hd2 : d2.Nodup := nodup_filter _ _ hsD
      have hp1 : p1.Nodup := nodup_filter _ _ hsP
      have hp2 : p2.Nodup := nodup_filter _ _ hcP
      have ha1 : a1.Nodup := nodup_filter _ _ hcA
      have ha2 : a2.Nodup := hsA
      have hdd : ∀ x ∈ d1, x ∉ d2 := by
        intro x hx hy
        have hxC := (List.mem_filter.mp hx).1
        have hyP := of_decide_eq_true (List.mem_filter.mp hy).2
        exact hyP.2.2 hxC
      have hpp : ∀ x ∈ p1, x ∉ p2 := by
        intro x hx hy
        have hxP := (List.mem_filter.mp hx).1
        have hyP := of_decide_eq_true (List.mem_filter.mp hy).2
        exact hyP.2.2 hxP
      have haa : ∀ x ∈ a1, x ∉ a2 := by
        intro x hx hy
        exact (of_decide_eq_true (List.mem_filter.mp hx).2).1 hy
      have hdel := nodup_append_of_disjoint d1 d2 hd1 hd2 hdd
      have hpre := nodup_append_of_disjoint p1 p2 hp1 hp2 hpp
      have happ := nodup_append_of_disjoint a1 a2 ha1 ha2 haa
      simpa [WellFormed, combine, d1, d2, p1, p2, a1, a2] using
        (show (a1 ++ a2).Nodup ∧ (p1 ++ p2).Nodup ∧ (d1 ++ d2).Nodup
          from ⟨happ, hpre, hdel⟩)

/-- Reduced form. SPEC: L1607-1625. -/
def reduce : ListOp α → ListOp α
  | .explicit e => .explicit e
  | .comp c =>
      .comp {
        deleted := c.deleted.filter (fun x => x ∉ c.appended ∧ x ∉ c.prepended),
        prepended := c.prepended.filter (· ∉ c.appended),
        appended := c.appended }

/-- Reduction preserves the spec's per-sequence uniqueness invariant. -/
theorem reduce_wellFormed (L : ListOp α) (hL : WellFormed L) :
    WellFormed (reduce L) := by
  cases L with
  | explicit items => exact hL
  | comp c =>
    obtain ⟨ha, hp, hd⟩ := hL
    exact ⟨ha, nodup_filter _ _ hp, nodup_filter _ _ hd⟩

/-- A list op is reduced when no element repeats across sequences. SPEC: L1609-1612. -/
def IsReduced : ListOp α → Prop
  | .explicit _ => True
  | .comp c => (∀ x ∈ c.prepended, x ∉ c.appended)
      ∧ (∀ x ∈ c.deleted, x ∉ c.appended ∧ x ∉ c.prepended)

instance : DecidablePred (IsReduced (α := α)) := by
  intro L; cases L <;> simp only [IsReduced] <;> infer_instance

/-- Chaining `⨆`, strongest first: `⨆⟨A, B, C⟩ = A ⊔ (B ⊔ C)`. SPEC: L1639-1655.
The spec writes `A ⊔ B ⊔ C` without fixing an association; the two readings are
congruent (`combine_assoc_congruent`) but not always equal. -/
def chain : List (ListOp α) → ListOp α
  | [] => default
  | L :: rest => L ⊔ chain rest

/-- Congruence `≅`. SPEC: L1484-1494. -/
def Congruent (A B : ListOp α) : Prop :=
  ∀ C : ListOp α, iterate (A ⊔ C) = iterate (B ⊔ C) ∧ iterate (C ⊔ A) = iterate (C ⊔ B)

@[inherit_doc Congruent] infix:50 " ≅ " => Congruent

/-- An inert list op has no effect on traversal (SPEC: L1571-1574); formalized as
congruence with the default `I′`. -/
def Inert (L : ListOp α) : Prop := L ≅ default

/-! ## Worked examples from the spec, checked by `decide`. -/

/-- SPEC: L1455-1460: `(del ⟨5⟩, pre ⟨5⟩, app ⟨5⟩) ⊔ (explicit ⟨4,5,6⟩) = (explicit ⟨4,6,5⟩)`. -/
example : (ListOp.comp ⟨[5], [5], [5]⟩ ⊔ ListOp.explicit [4, 5, 6] : ListOp Nat)
    = .explicit [4, 6, 5] := by decide

/-- SPEC: L1600-1604: `I′ ⊔ I′ = I′`. -/
theorem default_combine_default : (default ⊔ default : ListOp α) = default := by
  simp [default, combine]

/-! ## Basic algebra of `⊔` -/

/-- L1: a stronger explicit list op absorbs. SPEC: L1433-1438.
CLAIM: `S ∈ L_explicit ⟹ S ⊔ W → S`. -/
theorem combine_explicit (e : List α) (W : ListOp α) : (.explicit e ⊔ W) = .explicit e := by
  cases W <;> rfl

/-- L2: composable over explicit is explicit. SPEC: L1441-1446. -/
theorem combine_comp_explicit_isExplicit (s : Composable α) (e : List α) :
    (ListOp.comp s ⊔ .explicit e).isExplicit = true := rfl

/-- Composable over composable is composable. SPEC: L1462-1467. -/
theorem combine_comp_comp_isExplicit (s c : Composable α) :
    (ListOp.comp s ⊔ .comp c).isExplicit = false := rfl

theorem iterate_explicit (e : List α) : iterate (.explicit e : ListOp α) = e := rfl

/-- `apply L [] = iterate L`: applying to nothing is iterating. -/
theorem apply_nil (L : ListOp α) : apply L [] = iterate L := by
  cases L <;> simp [apply, iterate]

/-- `I′` is the identity edit. -/
theorem apply_default (l : List α) : apply (default : ListOp α) l = l := by
  simp [apply, default]

/-- `I′ ⊔ W = W` for every `W` (stronger than the spec's `≅`, L1592-1598). -/
theorem default_combine (W : ListOp α) : (default ⊔ W) = W := by
  cases W with
  | explicit e => simp [default, combine]
  | comp c => simp [default, combine, List.filter_true]

/-- `S ⊔ I′ = S` when `S` is reduced (the spec only claims `≅`, L1584-1590). -/
theorem combine_default_of_reduced (S : ListOp α) (h : IsReduced S) : (S ⊔ default) = S := by
  cases S with
  | explicit e => rfl
  | comp c =>
    obtain ⟨hp, hd⟩ := h
    have e1 : c.deleted.filter (fun x => x ∉ c.prepended ∧ x ∉ c.appended ∧ x ∉ ([] : List α))
        = c.deleted :=
      List.filter_eq_self.mpr (fun x hx => by have := hd x hx; simp [this.1, this.2])
    have e2 : c.prepended.filter (· ∉ c.appended) = c.prepended :=
      List.filter_eq_self.mpr (fun x hx => by simpa using hp x hx)
    cases c
    simp only [default, combine, List.filter_nil, List.nil_append, List.append_nil,
      ListOp.comp.injEq, Composable.mk.injEq] at e1 e2 ⊢
    exact ⟨e1, e2, trivial⟩

/-! ## The bridge theorems: `⊔` versus `apply` -/

/-- L5 (bridge): `iterate (S ⊔ W) = apply S (iterate W)`.
CLAIM (L1654-1655): "Normative traversal of a chain of list ops should yield same
ordering of elements as if the chain was combined to a single list operation." -/
theorem iterate_combine (S W : ListOp α) : iterate (S ⊔ W) = apply S (iterate W) := by
  cases S with
  | explicit e => cases W <;> rfl
  | comp s =>
    cases W with
    | explicit e => rfl
    | comp c =>
      simp only [combine, iterate, apply, List.filter_append, List.filter_filter,
        List.append_assoc]
      congr 1
      · apply List.filter_congr
        intro x hx
        by_cases h : x ∈ s.appended
        · simp [h]
        · simp [List.mem_filter, h, hx]
      · congr 1
        apply List.filter_congr
        intro x _
        simp only [List.mem_append, List.mem_filter, decide_eq_true_eq]
        by_cases hsa : x ∈ s.appended <;> by_cases hsp : x ∈ s.prepended <;>
          by_cases hsd : x ∈ s.deleted <;> by_cases hca : x ∈ c.appended <;>
          simp [hsa, hsp, hsd, hca]

/-- L6 (homomorphism): applying a combined op equals applying the ops in turn.
No uniqueness hypotheses are needed. -/
theorem apply_combine (S W : ListOp α) (l : List α) :
    apply (S ⊔ W) l = apply S (apply W l) := by
  cases S with
  | explicit e => cases W <;> rfl
  | comp s =>
    cases W with
    | explicit e => rfl
    | comp c =>
      simp only [combine, apply, List.filter_append, List.filter_filter, List.append_assoc]
      congr 1
      · apply List.filter_congr
        intro x hx
        by_cases h : x ∈ s.appended
        · simp [h]
        · simp [List.mem_filter, h, hx]
      · congr 1
        · apply List.filter_congr
          intro x _
          simp only [List.mem_append, List.mem_filter, decide_eq_true_eq]
          by_cases hsa : x ∈ s.appended <;> by_cases hsp : x ∈ s.prepended <;>
            by_cases hsd : x ∈ s.deleted <;> by_cases hca : x ∈ c.appended <;>
            simp [hsa, hsp, hsd, hca]
        · congr 1
          apply List.filter_congr
          intro x _
          simp only [List.mem_append, List.mem_filter, decide_eq_true_eq]
          by_cases hsa : x ∈ s.appended <;> by_cases hsp : x ∈ s.prepended <;>
            by_cases hsd : x ∈ s.deleted <;> by_cases hca : x ∈ c.appended <;>
            by_cases hcd : x ∈ c.deleted <;> by_cases hcp : x ∈ c.prepended <;>
            simp [hsa, hsp, hsd, hca, hcd, hcp]

/-- `chain` applied: `apply (chain Ls) l = foldr apply l Ls` — the normative
"apply weakest to strongest" reading (SPEC: L4371-4379 for arc lists). -/
theorem apply_chain (Ls : List (ListOp α)) (l : List α) :
    apply (chain Ls) l = Ls.foldr (fun L acc => apply L acc) l := by
  induction Ls with
  | nil => simp [chain, apply_default]
  | cons L rest ih => simp [chain, apply_combine, ih]

/-! ## Congruence -/

/-- L7: congruence is extensional equality of `apply`. -/
theorem congruent_iff_apply (A B : ListOp α) : A ≅ B ↔ ∀ l, apply A l = apply B l := by
  constructor
  · intro h l
    have := (h (.explicit l)).1
    rw [iterate_combine, iterate_combine] at this
    exact this
  · intro h C
    refine ⟨?_, ?_⟩
    · simp [iterate_combine, h]
    · have hi : iterate A = iterate B := by simpa [apply_nil] using h []
      simp [iterate_combine, hi]

theorem Congruent.refl (A : ListOp α) : A ≅ A := fun _ => ⟨rfl, rfl⟩
theorem Congruent.symm {A B : ListOp α} (h : A ≅ B) : B ≅ A :=
  fun C => ⟨(h C).1.symm, (h C).2.symm⟩
theorem Congruent.trans {A B C : ListOp α} (h₁ : A ≅ B) (h₂ : B ≅ C) : A ≅ C :=
  fun D => ⟨(h₁ D).1.trans (h₂ D).1, (h₁ D).2.trans (h₂ D).2⟩

/-- Equivalent list operations are congruent. SPEC: L1497-1501. -/
theorem congruent_of_eq {A B : ListOp α} (h : A = B) : A ≅ B := h ▸ Congruent.refl A

/-- L11: chaining is associative up to congruence. SPEC: L1657-1662. -/
theorem combine_assoc_congruent (A B C : ListOp α) : ((A ⊔ B) ⊔ C) ≅ (A ⊔ (B ⊔ C)) := by
  rw [congruent_iff_apply]; intro l; simp [apply_combine]

/-- Congruence is a congruence for `⊔` on the left and right. -/
theorem combine_congr_left {A B : ListOp α} (h : A ≅ B) (C : ListOp α) : (A ⊔ C) ≅ (B ⊔ C) := by
  rw [congruent_iff_apply] at *; intro l; simp [apply_combine, h]
theorem combine_congr_right {A B : ListOp α} (h : A ≅ B) (C : ListOp α) : (C ⊔ A) ≅ (C ⊔ B) := by
  rw [congruent_iff_apply] at *; intro l; simp [apply_combine, h]

/-- `S ⊔ I′ ≅ S`. SPEC: L1584-1590. -/
theorem combine_default_congruent (S : ListOp α) : (S ⊔ default) ≅ S := by
  rw [congruent_iff_apply]; intro l; simp [apply_combine, apply_default]

/-- `I′ ⊔ W ≅ W`. SPEC: L1592-1598. -/
theorem default_combine_congruent (W : ListOp α) : (default ⊔ W) ≅ W :=
  congruent_of_eq (default_combine W)

/-- Inert ops are exactly the identity edits. -/
theorem inert_iff (L : ListOp α) : Inert L ↔ ∀ l, apply L l = l := by
  simp [Inert, congruent_iff_apply, apply_default]

/-- L13: dropping an inert op anywhere in a chain gives a congruent chain. SPEC: L1676-1686. -/
theorem chain_drop_inert_congruent (pre post : List (ListOp α)) {I : ListOp α} (hI : Inert I) :
    chain (pre ++ I :: post) ≅ chain (pre ++ post) := by
  rw [congruent_iff_apply]; intro l
  rw [inert_iff] at hI
  simp [apply_chain, List.foldr_append, hI]

/-- L12: a chain may stop at the first explicit op. SPEC: L1667-1674. -/
theorem chain_stop_at_explicit (pre : List (ListOp α)) (e : List α) (post : List (ListOp α)) :
    chain (pre ++ .explicit e :: post) = chain (pre ++ [.explicit e]) := by
  induction pre with
  | nil => simp [chain, combine_explicit]
  | cons L rest ih => simp [chain, ih]

/-! ## Reduction -/

theorem reduce_isReduced (L : ListOp α) : IsReduced (reduce L) := by
  cases L with
  | explicit e => trivial
  | comp c =>
    refine ⟨?_, ?_⟩
    · intro x hx; simp only [List.mem_filter, decide_eq_true_eq] at hx; exact hx.2
    · intro x hx
      simp only [List.mem_filter, decide_eq_true_eq] at hx ⊢
      exact ⟨hx.2.1, fun h => hx.2.2 h.1⟩

theorem reduce_of_isReduced (L : ListOp α) (h : IsReduced L) : reduce L = L := by
  cases L with
  | explicit e => rfl
  | comp c =>
    obtain ⟨hp, hd⟩ := h
    cases c
    simp only [reduce, ListOp.comp.injEq, Composable.mk.injEq, and_true]
    exact ⟨List.filter_eq_self.mpr (fun x hx => by simpa using hd x hx),
      List.filter_eq_self.mpr (fun x hx => by simpa using hp x hx)⟩

/-- L9: `reduce` is idempotent. -/
theorem reduce_reduce (L : ListOp α) : reduce (reduce L) = reduce L :=
  reduce_of_isReduced _ (reduce_isReduced L)

/-- Reduction preserves `apply`, hence congruence. SPEC: L1630-1634 (`A ≅ B ⟹ reduced(A) ≅ reduced(B)`). -/
theorem apply_reduce (L : ListOp α) (l : List α) : apply (reduce L) l = apply L l := by
  cases L with
  | explicit e => rfl
  | comp c =>
    simp only [reduce, apply, List.filter_filter, List.append_assoc]
    congr 1
    · apply List.filter_congr; intro x _; simp
    · congr 1
      apply List.filter_congr; intro x _
      simp only [List.mem_filter, decide_eq_true_eq]
      by_cases ha : x ∈ c.appended <;> by_cases hp : x ∈ c.prepended <;> by_cases hd : x ∈ c.deleted <;>
        simp [ha, hp, hd]

theorem reduce_congruent (L : ListOp α) : reduce L ≅ L := by
  rw [congruent_iff_apply]; intro l; exact apply_reduce L l

theorem reduce_congruent_of_congruent {A B : ListOp α} (h : A ≅ B) : reduce A ≅ reduce B :=
  (reduce_congruent A).trans (h.trans (reduce_congruent B).symm)

/-! ## What congruence determines (L10)

The spec claims `A ≅ B ⟹ reduced(A) = reduced(B)` (L1630-1636). Congruence
only observes `iterate`, and `iterate` never looks at the *order* of the deleted
sequence, so the claim holds for `prepended`/`appended` and for the *set* of
deleted elements, but not for their order. `exists_congruent_reduce_ne` is the
counterexample. The characterization needs an element outside both ops (an
infinite element type suffices); over a finite element type congruence is
coarser than the spec assumes (`apply_comp_eq_iff` would fail for `α = Unit`).

SPEC-ISSUE: LEAN-001 — `A ≅ B ⟹ reduced(A) = reduced(B)` is false as stated
(deleted-sequence order is unobservable); see `exists_congruent_reduce_ne`.
-/

omit [DecidableEq α] in
/-- A marker element splits an equality of lists uniquely. -/
theorem append_cons_inj {y : α} {l₁ r₁ l₂ r₂ : List α}
    (h : l₁ ++ y :: r₁ = l₂ ++ y :: r₂) (h₁ : y ∉ l₁) (h₂ : y ∉ l₂) : l₁ = l₂ ∧ r₁ = r₂ := by
  induction l₁ generalizing l₂ with
  | nil =>
    cases l₂ with
    | nil => simpa using h
    | cons z l₂ =>
      simp only [List.nil_append, List.cons_append, List.cons.injEq] at h
      exact absurd (h.1 ▸ List.mem_cons_self) h₂
  | cons z l₁ ih =>
    cases l₂ with
    | nil =>
      simp only [List.cons_append, List.nil_append, List.cons.injEq] at h
      exact absurd (h.1 ▸ List.mem_cons_self) h₁
    | cons z' l₂ =>
      simp only [List.cons_append, List.cons.injEq] at h
      have := ih h.2 (fun hm => h₁ (List.mem_cons_of_mem _ hm)) (fun hm => h₂ (List.mem_cons_of_mem _ hm))
      exact ⟨by rw [h.1, this.1], this.2⟩

/-- Elements an op removes from (or overrides in) the weaker list. -/
def kills (c : Composable α) (x : α) : Prop :=
  x ∈ c.appended ∨ x ∈ c.deleted ∨ x ∈ c.prepended

instance (c : Composable α) : DecidablePred (kills c) := fun _ => by unfold kills; infer_instance

theorem apply_comp_eq_iff [Infinite α] (s c : Composable α) :
    (∀ l, apply (.comp s) l = apply (.comp c) l) ↔
      s.prepended.filter (· ∉ s.appended) = c.prepended.filter (· ∉ c.appended)
      ∧ s.appended = c.appended
      ∧ ∀ x, kills s x ↔ kills c x := by
  constructor
  · intro h
    obtain ⟨y, hy⟩ := Infinite.exists_notMem_finset
      (s.prepended ++ s.appended ++ s.deleted ++ c.prepended ++ c.appended ++ c.deleted).toFinset
    simp only [List.mem_toFinset, List.mem_append, not_or] at hy
    obtain ⟨⟨⟨⟨⟨hsp, hsa⟩, hsd⟩, hcp⟩, hca⟩, hcd⟩ := hy
    have hy' := h [y]
    simp only [apply, List.filter_cons, List.filter_nil, decide_eq_true_eq] at hy'
    rw [if_pos ⟨hsa, hsd, hsp⟩, if_pos ⟨hca, hcd, hcp⟩] at hy'
    simp only [List.append_assoc, List.singleton_append] at hy'
    have hsplit := append_cons_inj hy'
      (fun hm => hsp (List.mem_filter.mp hm).1) (fun hm => hcp (List.mem_filter.mp hm).1)
    refine ⟨hsplit.1, hsplit.2, ?_⟩
    intro x
    have hx := h [x]
    simp only [apply, List.filter_cons, List.filter_nil, decide_eq_true_eq] at hx
    rw [hsplit.2] at hx
    have hx1 := List.append_cancel_right hx
    have h1' := hsplit.1
    rw [hsplit.2] at h1'
    rw [h1'] at hx1
    have hx' := List.append_cancel_left hx1
    unfold kills
    rw [hsplit.2]
    by_cases hs : x ∈ c.appended ∨ x ∈ s.deleted ∨ x ∈ s.prepended <;>
      by_cases hc : x ∈ c.appended ∨ x ∈ c.deleted ∨ x ∈ c.prepended
    · exact ⟨fun _ => hc, fun _ => hs⟩
    · exfalso
      rw [if_neg (by tauto), if_pos (by tauto)] at hx'
      simp at hx'
    · exfalso
      rw [if_pos (by tauto), if_neg (by tauto)] at hx'
      simp at hx'
    · exact ⟨fun h => absurd h hs, fun h => absurd h hc⟩
  · rintro ⟨h1, h2, h3⟩ l
    dsimp only [apply]
    rw [h1, h2]
    congr 2
    apply List.filter_congr; intro x _
    have := h3 x
    unfold kills at this
    rw [h2] at this
    apply decide_eq_decide.mpr
    tauto

/-- L8: a congruent pair with an explicit member is equal. SPEC: L1514-1518.
Needs an element outside the ops (true for every USD element type). -/
theorem eq_of_congruent_explicit [Infinite α] {e : List α} {B : ListOp α}
    (h : (.explicit e : ListOp α) ≅ B) : (.explicit e : ListOp α) = B := by
  rw [congruent_iff_apply] at h
  cases B with
  | explicit f => simpa [apply] using h []
  | comp c =>
    exfalso
    obtain ⟨y, hy⟩ := Infinite.exists_notMem_finset
      (e ++ c.prepended ++ c.appended ++ c.deleted).toFinset
    simp only [List.mem_toFinset, List.mem_append, not_or] at hy
    obtain ⟨⟨⟨he, hp⟩, ha⟩, hd⟩ := hy
    have := h [y]
    simp only [apply, List.filter_cons, List.filter_nil, decide_eq_true_eq] at this
    rw [if_pos ⟨ha, hd, hp⟩] at this
    have hmem : y ∈ e := by rw [this]; simp
    exact he hmem

/-- Congruent explicit ops are equal; congruent composable ops agree on the
iterated prefix, the appended sequence, and the deleted *set*. -/
theorem reduce_congruent_of_congruent' [Infinite α] {s c : Composable α}
    (h : (.comp s : ListOp α) ≅ .comp c) :
    (reduce (.comp s : ListOp α)).iterate = (reduce (.comp c : ListOp α)).iterate
    ∧ (∀ x, x ∈ (match reduce (.comp s : ListOp α) with | .comp r => r.deleted | _ => [])
          ↔ x ∈ (match reduce (.comp c : ListOp α) with | .comp r => r.deleted | _ => [])) := by
  rw [congruent_iff_apply, apply_comp_eq_iff] at h
  obtain ⟨h1, h2, h3⟩ := h
  refine ⟨?_, ?_⟩
  · simp only [reduce, iterate]
    have hidem : ∀ (l a : List α), (l.filter (· ∉ a)).filter (· ∉ a) = l.filter (· ∉ a) :=
      fun l a => List.filter_eq_self.mpr (fun x hx => (List.mem_filter.mp hx).2)
    rw [hidem, hidem, h1, h2]
  · intro x
    simp only [reduce, List.mem_filter, decide_eq_true_eq]
    have hk := h3 x
    unfold kills at hk
    have hpre : (x ∈ s.prepended ∧ x ∉ s.appended) ↔ (x ∈ c.prepended ∧ x ∉ c.appended) := by
      have := congrArg (x ∈ ·) h1
      simpa [List.mem_filter] using this
    rw [h2] at hpre hk ⊢
    constructor
    · rintro ⟨hd, ha, hp⟩
      have : x ∈ c.deleted := by
        rcases hk.mp (Or.inr (Or.inl hd)) with h | h | h
        · exact absurd h ha
        · exact h
        · exact absurd (hpre.mpr ⟨h, ha⟩).1 hp
      exact ⟨this, ha, fun hp' => hp (hpre.mpr ⟨hp', ha⟩).1⟩
    · rintro ⟨hd, ha, hp⟩
      have : x ∈ s.deleted := by
        rcases hk.mpr (Or.inr (Or.inl hd)) with h | h | h
        · exact absurd h ha
        · exact h
        · exact absurd (hpre.mp ⟨h, ha⟩).1 hp
      exact ⟨this, ha, fun hp' => hp (hpre.mp ⟨hp', ha⟩).1⟩

/-- Counterexample to the literal spec claim `A ≅ B ⟹ reduced(A) = reduced(B)`
(L1635): deleting `⟨1, 2⟩` and deleting `⟨2, 1⟩` are congruent, both reduced,
and not equal. -/
theorem exists_congruent_reduce_ne :
    ∃ A B : ListOp Nat, A ≅ B ∧ IsReduced A ∧ IsReduced B ∧ reduce A ≠ reduce B := by
  refine ⟨.comp ⟨[1, 2], [], []⟩, .comp ⟨[2, 1], [], []⟩, ?_, by decide, by decide, by decide⟩
  rw [congruent_iff_apply]; intro l
  simp only [apply, List.filter_nil, List.nil_append, List.append_nil]
  apply List.filter_congr; intro x _
  apply decide_eq_decide.mpr
  simp only [List.mem_cons, List.not_mem_nil]
  tauto

/-- Executable congruence test (sound and complete for an infinite element type). -/
def congrCheck : ListOp α → ListOp α → Bool
  | .explicit e, .explicit f => e == f
  | .comp s, .comp c =>
      s.prepended.filter (· ∉ s.appended) == c.prepended.filter (· ∉ c.appended)
      && s.appended == c.appended
      && (s.appended ++ s.deleted ++ s.prepended).all (fun x => decide (kills c x))
      && (c.appended ++ c.deleted ++ c.prepended).all (fun x => decide (kills s x))
  | _, _ => false

theorem congruent_iff_congrCheck [Infinite α] (A B : ListOp α) : A ≅ B ↔ congrCheck A B = true := by
  cases A with
  | explicit e =>
    cases B with
    | explicit f =>
      simp only [congrCheck, beq_iff_eq]
      constructor
      · intro h; have := eq_of_congruent_explicit h; simpa using this
      · intro h; subst h; exact Congruent.refl _
    | comp c =>
      simp only [congrCheck, Bool.false_eq_true, iff_false]
      intro h; have := eq_of_congruent_explicit h; simp at this
  | comp s =>
    cases B with
    | explicit f =>
      simp only [congrCheck, Bool.false_eq_true, iff_false]
      intro h; have := eq_of_congruent_explicit h.symm; simp at this
    | comp c =>
      rw [congruent_iff_apply, apply_comp_eq_iff]
      simp only [congrCheck, Bool.and_eq_true, beq_iff_eq, List.all_eq_true, List.mem_append,
        decide_eq_true_eq]
      constructor
      · rintro ⟨h1, h2, h3⟩
        refine ⟨⟨⟨h1, h2⟩, ?_⟩, ?_⟩
        · intro x hx; exact (h3 x).mp (by unfold kills; tauto)
        · intro x hx; exact (h3 x).mpr (by unfold kills; tauto)
      · rintro ⟨⟨⟨h1, h2⟩, h3⟩, h4⟩
        refine ⟨h1, h2, fun x => ⟨fun hk => h3 x ?_, fun hk => h4 x ?_⟩⟩ <;> unfold kills at hk <;> tauto

/-! ## The worked congruence examples of the spec (L1520-1570), decided mechanically. -/
section Examples
open ListOp
private def C₁ : ListOp Nat := .comp ⟨[50], [10], [10, 50]⟩
private def C₂ : ListOp Nat := .comp ⟨[], [10], [10, 50]⟩
private def C₃ : ListOp Nat := .comp ⟨[], [], [10, 50]⟩
private def C₄ : ListOp Nat := .comp ⟨[50], [], [10, 50]⟩
private def C₅ : ListOp Nat := .comp ⟨[50], [10, 50], [10, 50]⟩
private def C₆ : ListOp Nat := .comp ⟨[10, 50], [10, 50], [10, 50]⟩
private def Cm : ListOp Nat := .comp ⟨[], [], [10, 50]⟩
private def Cn : ListOp Nat := .comp ⟨[], [], [50, 10]⟩
private def Ck : ListOp Nat := .comp ⟨[], [50], [10]⟩
private def Cl : ListOp Nat := .comp ⟨[], [], [50, 10]⟩

/-- SPEC: L1533-1535 `C₁ ≅ C₂ ≅ C₃ ≅ C₄ ≅ C₅ ≅ C₆`. -/
example : C₁ ≅ C₂ ∧ C₂ ≅ C₃ ∧ C₃ ≅ C₄ ∧ C₄ ≅ C₅ ∧ C₅ ≅ C₆ := by
  simp only [congruent_iff_congrCheck]; decide
/-- SPEC: L1549-1551 `Cm ≇ Cn`. -/
example : ¬ (Cm ≅ Cn) := by rw [congruent_iff_congrCheck]; decide
/-- SPEC: L1563-1565 `Ck ≇ Cl` even though `iterate Ck = iterate Cl`. -/
example : ¬ (Ck ≅ Cl) ∧ iterate Ck = iterate Cl := by
  rw [congruent_iff_congrCheck]; decide
end Examples

end ListOp
end UsdSpec
