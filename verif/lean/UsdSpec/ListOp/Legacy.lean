import UsdSpec.ListOp.Apply

/-!
# Deprecated list operations: `add` and `reorder`

SPEC: core_spec.md L1699-1708 ("Deprecated Operations"): `reorder` and `add`
"are not normatively specified. Supporting them would prevent list operations
from being closed under combination." The crate encoding keeps both for
compatibility only (L12634-12637: "no normative semantic").

LightUSD still *reads* and *applies* both, in `ApplyStringListOp`
(src/next/layer/prim-spec-storage.cc:352-384) and `ApplyInt64Op` (:182-241):

* `added` items are pushed uniquely **after** the prepended/weaker items and
  **before** the appended items are moved to the end (pxr `_AddKeys` →
  `_InsertIfUnique`; LightUSD runs the stages in the order prepend, weaker,
  add, append — the pxr order delete, add, prepend, append gives the same
  result since prepend/append move items and add only inserts if absent);
* `ordered` is applied last by `ApplyStringListOrder` (:325-350), a transcription
  of pxr `SdfListOp::_ReorderKeysHelper` (OpenUSD pxr/usd/sdf/listOp.cpp:676-721):
  de-duplicate `order`; for each key in that order find its first occurrence in
  the list and take, as one block, the key plus the run of following items that
  are *not* keys; emit the blocks in `order` sequence; whatever is left (the
  prefix before the first key, and leftovers if keys repeat in the list) goes
  first.

This file models both faithfully (`applyLegacy`, `reorder`) and proves:

1. `applyLegacy_no_legacy`: with no `added`/`ordered` the model is `applyCpp`,
   so the normative theorems of `Apply.lean` cover the vectors.
2. `reorder` is a permutation (`reorder_perm`), is the identity for an empty
   order (`reorder_nil_order`) and for an order disjoint from the list
   (`reorder_of_disjoint`).
3. `add` versus `append` (issues/aousd/05-add-listop-deprecation.md and its
   REVIEW demo `05-add-vs-append`): `add` equals `append` exactly when no added
   item is already present (`applyLegacy_added_eq_appended_of_fresh`), is always
   a permutation of it (`applyLegacy_added_perm_appended`), and differs in order
   otherwise (`add_ne_append`, the demo's pxr result reproduced by `decide`).
   Moreover `add` is not expressible by *any* normative op (`add_not_expressible`).
4. "Not closed under combination" (L1703), made formal: every normative edit
   keeps surviving weaker elements in their weaker relative order
   (`apply_preserves_relative_order`, `apply_sublist_of_iterate_nil`), while a
   `reorder` edit flips it (`reorder_flips_order`); hence no normative `ListOp`
   agrees with a `reorder` edit on all inputs (`reorder_not_expressible`), so a
   chain containing one cannot be combined into a single normative op.

No new spec issue: the spec's non-normative stance is confirmed; the `add`
deprecation wording is already issue 05 (cited above, not duplicated).
-/

namespace UsdSpec.ListOp

variable {α : Type} [DecidableEq α]

/-- A composable op plus the two deprecated sequences carried by the crate
format and by `StringListOps`/`Int64ListOp` (`added`, `ordered`). -/
structure LegacyOps (α : Type) where
  core    : Composable α
  added   : List α
  ordered : List α
  deriving DecidableEq, Repr, Inhabited

/-! ## `reorder` — `ApplyStringListOrder` / pxr `_ReorderKeysHelper` -/

/-- Remove the block headed by the first occurrence of `o`: `o` itself plus
the following run of items that are not order keys (`inSet`). Returns
`(block, scratch without the block)`, or `none` if `o` is absent. -/
def extractBlock (inSet : α → Bool) (o : α) : List α → Option (List α × List α)
  | [] => none
  | x :: xs =>
    if x = o then
      some (x :: xs.takeWhile (fun y => !inSet y), xs.dropWhile (fun y => !inSet y))
    else
      match extractBlock inSet o xs with
      | none => none
      | some (b, r) => some (b, x :: r)

/-- One iteration of the `for (o : unique_order)` loop over `(scratch, result)`:
`result.splice(result.end(), scratch, j, e)`. -/
def reorderStep (inSet : α → Bool) (sr : List α × List α) (o : α) : List α × List α :=
  match extractBlock inSet o sr.1 with
  | none => sr
  | some (b, s') => (s', sr.2 ++ b)

/-- `ApplyStringListOrder(order, list)`: de-duplicate `order`, move the blocks
to `result` in order, then `result.splice(result.begin(), scratch)`. -/
def reorder (order l : List α) : List α :=
  let uo := dedupFirst order
  let sr := uo.foldl (reorderStep (fun x => decide (x ∈ uo))) (l, [])
  sr.1 ++ sr.2

/-- `ApplyStringListOp` with `added` and `ordered` (prim-spec-storage.cc:352-384):
prepended uniquely, weaker-minus-deleted uniquely, added uniquely, appended
moved to the end, then `ApplyStringListOrder`. -/
def applyLegacy (ops : LegacyOps α) (w : List α) : List α :=
  let c := ops.core
  let r1 := c.prepended.foldl pushUnique []
  let r2 := w.foldl (fun r x => if x ∈ c.deleted then r else pushUnique r x) r1
  let r3 := ops.added.foldl pushUnique r2
  let r4 := c.appended.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r3
  reorder ops.ordered r4

/-- The pxr `_ReorderKeysHelper` example shape: blocks follow their key. -/
example : reorder [3, 1] [0, 1, 10, 11, 2, 3, 30] = [0, 3, 30, 1, 10, 11, 2] := by decide
/-- Keys absent from the list are skipped; the prefix stays first. -/
example : reorder [9, 2] [5, 2, 7, 1] = [5, 2, 7, 1] := by decide
/-- A repeated key in the list: only its first block moves, the rest is prefix. -/
example : reorder [1] [2, 1, 3, 1, 4] = [2, 1, 4, 1, 3] := by decide

/-! ### (1) No legacy sequences ⇒ the normative implementation model -/

theorem reorder_nil_order (l : List α) : reorder [] l = l := by
  simp [reorder, dedupFirst, dedupGo]

/-- With `added = []` and `ordered = []`, `applyLegacy` is `applyCpp`, so
`applyCpp_eq_apply`/`applyCpp_iterate` (Apply.lean) apply verbatim. -/
theorem applyLegacy_no_legacy (ops : LegacyOps α) (w : List α)
    (ha : ops.added = []) (ho : ops.ordered = []) :
    applyLegacy ops w = applyCpp ops.core w := by
  simp [applyLegacy, applyCpp, ha, ho, reorder_nil_order]

/-! ### (2) `reorder` is a permutation -/

theorem extractBlock_perm (inSet : α → Bool) (o : α) (l : List α) :
    ∀ b r, extractBlock inSet o l = some (b, r) → (b ++ r).Perm l := by
  induction l with
  | nil => intro b r h; simp [extractBlock] at h
  | cons x xs ih =>
    intro b r h
    by_cases hx : x = o
    · subst hx
      simp only [extractBlock, ite_true, Option.some.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      simp only [List.cons_append, List.takeWhile_append_dropWhile]
      exact List.Perm.refl _
    · simp only [extractBlock, hx, ite_false] at h
      cases hxs : extractBlock inSet o xs with
      | none => simp [hxs] at h
      | some br =>
        obtain ⟨b', r'⟩ := br
        simp only [hxs, Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        exact List.perm_middle.trans ((ih b' r' hxs).cons x)

theorem extractBlock_none_of_notMem (inSet : α → Bool) (o : α) (l : List α) (h : o ∉ l) :
    extractBlock inSet o l = none := by
  induction l with
  | nil => rfl
  | cons x xs ih =>
    simp only [List.mem_cons, not_or] at h
    simp [extractBlock, Ne.symm h.1, ih h.2]

theorem foldl_reorderStep_perm (inSet : α → Bool) (uo : List α) :
    ∀ s r : List α,
      ((uo.foldl (reorderStep inSet) (s, r)).1 ++ (uo.foldl (reorderStep inSet) (s, r)).2).Perm
        (s ++ r) := by
  induction uo with
  | nil => intro s r; exact List.Perm.refl _
  | cons o uo ih =>
    intro s r
    simp only [List.foldl_cons]
    unfold reorderStep
    cases hx : extractBlock inSet o s with
    | none => exact ih s r
    | some br =>
      obtain ⟨b, s'⟩ := br
      simp only
      refine (ih s' (r ++ b)).trans ?_
      have hb := extractBlock_perm inSet o s b s' hx
      -- s' ++ (r ++ b) ~ s' ++ (b ++ r) = (s' ++ b) ++ r ~ (b ++ s') ++ r ~ s ++ r
      calc List.Perm (s' ++ (r ++ b)) (s' ++ (b ++ r)) := List.Perm.append_left _ List.perm_append_comm
        _ = (s' ++ b) ++ r := (List.append_assoc _ _ _).symm
        List.Perm _ ((b ++ s') ++ r) := List.Perm.append_right _ List.perm_append_comm
        List.Perm _ (s ++ r) := List.Perm.append_right _ hb

/-- `ApplyStringListOrder` only permutes: nothing is added or dropped. -/
theorem reorder_perm (order l : List α) : (reorder order l).Perm l := by
  unfold reorder
  simpa using foldl_reorderStep_perm _ (dedupFirst order) l []

theorem mem_of_mem_dedupGo {x : α} (seen l : List α) (h : x ∈ dedupGo seen l) : x ∈ l := by
  induction l generalizing seen with
  | nil => simp [dedupGo] at h
  | cons y ys ih =>
    simp only [dedupGo] at h
    split at h
    · exact List.mem_cons_of_mem _ (ih _ h)
    · rcases List.mem_cons.mp h with rfl | h
      · exact List.mem_cons_self
      · exact List.mem_cons_of_mem _ (ih _ h)

theorem mem_of_mem_dedupFirst {x : α} {l : List α} (h : x ∈ dedupFirst l) : x ∈ l :=
  mem_of_mem_dedupGo [] l h

/-- An order whose keys are all absent from the list is a no-op. -/
theorem reorder_of_disjoint (order l : List α) (h : ∀ x ∈ order, x ∉ l) :
    reorder order l = l := by
  unfold reorder
  have key : ∀ (uo : List α) (inSet : α → Bool), (∀ x ∈ uo, x ∉ l) →
      uo.foldl (reorderStep inSet) (l, []) = (l, []) := by
    intro uo inSet huo
    induction uo with
    | nil => rfl
    | cons o uo ih =>
      simp only [List.foldl_cons]
      have ho : o ∉ l := huo o List.mem_cons_self
      simp only [reorderStep, extractBlock_none_of_notMem inSet o l ho]
      exact ih (fun x hx => huo x (List.mem_cons_of_mem _ hx))
  simp only
  rw [key _ _ (fun x hx => h x (mem_of_mem_dedupFirst hx))]
  simp

/-! ### (3) `add` versus `append` (issues/aousd/05-add-listop-deprecation.md) -/

/-- The `add` stage: `added` items are pushed uniquely. -/
theorem foldl_pushUnique_eq_moveToEnd (r₀ a : List α) (ha : a.Nodup) (h : ∀ x ∈ a, x ∉ r₀) :
    a.foldl pushUnique r₀ = a.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r₀ := by
  rw [foldl_pushUnique, foldl_moveToEnd _ _ ha, dedupGo_of_nodup _ _ ha h]
  congr 1
  symm
  exact List.filter_eq_self.mpr (fun x hx => by
    simp only [decide_eq_true_eq]; exact fun hxa => h x hxa hx)

/-- The weaker-side list after prepend and weaker-minus-deleted stages. -/
def weakerStage (c : Composable α) (w : List α) : List α :=
  w.foldl (fun r x => if x ∈ c.deleted then r else pushUnique r x) (c.prepended.foldl pushUnique [])

theorem mem_foldl_pushUnique (r₀ l : List α) (x : α) :
    x ∈ l.foldl pushUnique r₀ ↔ x ∈ r₀ ∨ x ∈ l := by
  induction l generalizing r₀ with
  | nil => simp
  | cons y ys ih =>
    simp only [List.foldl_cons, ih, pushUnique, List.mem_cons]
    split
    · rename_i hy; constructor
      · rintro (h | h); exact Or.inl h; exact Or.inr (Or.inr h)
      · rintro (h | rfl | h); exact Or.inl h; exact Or.inl hy; exact Or.inr h
    · simp only [List.mem_append, List.mem_singleton]; tauto

theorem mem_weakerStage (c : Composable α) (w : List α) (x : α) :
    x ∈ weakerStage c w ↔ x ∈ c.prepended ∨ (x ∈ w ∧ x ∉ c.deleted) := by
  simp only [weakerStage]
  rw [foldl_pushUnique_filter, mem_foldl_pushUnique, mem_foldl_pushUnique, List.mem_filter]
  simp

theorem nodup_foldl_pushUnique (r₀ l : List α) (h : r₀.Nodup) : (l.foldl pushUnique r₀).Nodup := by
  induction l generalizing r₀ with
  | nil => exact h
  | cons y ys ih =>
    simp only [List.foldl_cons, pushUnique]
    split
    · exact ih _ h
    · rename_i hy
      apply ih
      rw [List.nodup_append]
      refine ⟨h, List.nodup_singleton y, ?_⟩
      intro a ha b hb
      simp only [List.mem_singleton] at hb
      subst hb; exact fun e => hy (e ▸ ha)

theorem nodup_weakerStage (c : Composable α) (w : List α) : (weakerStage c w).Nodup := by
  simp only [weakerStage]
  rw [foldl_pushUnique_filter]
  exact nodup_foldl_pushUnique _ _ (nodup_foldl_pushUnique _ _ List.nodup_nil)

theorem mem_foldl_moveToEnd (r₀ l : List α) (x : α) :
    x ∈ l.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r₀ ↔ x ∈ r₀ ∨ x ∈ l := by
  induction l generalizing r₀ with
  | nil => simp
  | cons y ys ih =>
    rw [List.foldl_cons, ih]
    simp only [List.mem_append, List.mem_filter, List.mem_cons, ne_eq, decide_eq_true_eq]
    by_cases hxy : x = y <;> simp [hxy]

theorem nodup_foldl_moveToEnd (r₀ l : List α) (h : r₀.Nodup) :
    (l.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r₀).Nodup := by
  induction l generalizing r₀ with
  | nil => exact h
  | cons y ys ih =>
    simp only [List.foldl_cons]
    apply ih
    rw [List.nodup_append]
    refine ⟨h.filter _, List.nodup_singleton y, ?_⟩
    intro a ha b hb
    simp only [List.mem_singleton] at hb
    subst hb
    simpa using (List.mem_filter.mp ha).2

/-- `add a` **is** `append a` (onto the front of the appended sequence) exactly
when no added item is already present after the prepend/weaker stages. -/
theorem applyLegacy_added_eq_appended_of_fresh (c : Composable α) (a w : List α)
    (ha : a.Nodup) (h : ∀ x ∈ a, x ∉ c.prepended ∧ (x ∈ w → x ∈ c.deleted)) :
    applyLegacy ⟨c, a, []⟩ w = applyCpp { c with appended := a ++ c.appended } w := by
  have hfresh : ∀ x ∈ a, x ∉ weakerStage c w := by
    intro x hx
    rw [mem_weakerStage]
    have := h x hx
    tauto
  simp only [applyLegacy, applyCpp, reorder_nil_order, List.foldl_append]
  rw [← weakerStage, foldl_pushUnique_eq_moveToEnd _ _ ha hfresh]

/-- In general `add a` is a *permutation* of `append a`: same elements. -/
theorem applyLegacy_added_perm_appended (c : Composable α) (a w : List α) :
    (applyLegacy ⟨c, a, []⟩ w).Perm (applyCpp { c with appended := a ++ c.appended } w) := by
  simp only [applyLegacy, applyCpp, reorder_nil_order, List.foldl_append]
  rw [← weakerStage]
  rw [List.perm_ext_iff_of_nodup]
  · intro x
    simp only [mem_foldl_moveToEnd, mem_foldl_pushUnique]
  · exact nodup_foldl_moveToEnd _ _ (nodup_foldl_pushUnique _ _ (nodup_weakerStage c w))
  · exact nodup_foldl_moveToEnd _ _ (nodup_foldl_moveToEnd _ _ (nodup_weakerStage c w))

/-- The REVIEW demo `issues/aousd/REVIEW-demo/05-add-vs-append` (pxr 26.05 and
26.11 agree): weaker `[/A, /B]` (here `/A = 0`, `/B = 1`); `add /A` keeps
`[/A, /B]`, `append /A` yields `[/B, /A]`. The `added` item was already present,
so `add` leaves it in place while `append` moves it to the end. -/
theorem add_ne_append :
    applyLegacy (⟨⟨[], [], []⟩, [0], []⟩ : LegacyOps Nat) [0, 1] = [0, 1]
    ∧ applyCpp (⟨[], [], [0]⟩ : Composable Nat) [0, 1] = [1, 0]
    ∧ apply (.comp ⟨[], [], [0]⟩ : ListOp Nat) [0, 1] = [1, 0] := by decide

/-- `add` is not expressible by *any* normative list op: the three edits
`[] ↦ [0]`, `[0,1] ↦ [0,1]`, `[1,0] ↦ [1,0]` of `add 0` cannot all come from one
explicit or composable op (an op that produces `0` from nothing must prepend or
append it, and then it moves `0` in one of the two weaker lists). -/
theorem add_not_expressible :
    ¬ ∃ T : ListOp Nat, ∀ l, apply T l = applyLegacy ⟨⟨[], [], []⟩, [0], []⟩ l := by
  rintro ⟨T, h⟩
  have h0 := h []
  have h1 := h [0, 1]
  have h2 := h [1, 0]
  have e0 : applyLegacy (⟨⟨[], [], []⟩, [0], []⟩ : LegacyOps Nat) [] = [0] := by decide
  have e1 : applyLegacy (⟨⟨[], [], []⟩, [0], []⟩ : LegacyOps Nat) [0, 1] = [0, 1] := by decide
  have e2 : applyLegacy (⟨⟨[], [], []⟩, [0], []⟩ : LegacyOps Nat) [1, 0] = [1, 0] := by decide
  rw [e0] at h0; rw [e1] at h1; rw [e2] at h2
  cases T with
  | explicit e => simp [apply] at h0 h1; rw [h0] at h1; simp at h1
  | comp c =>
    simp only [apply, List.filter_nil, List.append_nil] at h0
    rcases List.append_eq_cons_iff.mp h0 with ⟨hp, happ⟩ | ⟨pre, hp, hrest⟩
    · -- prepended (filtered) = [], appended = [0]: `0` is moved to the end of `[0,1]`.
      simp only [apply, happ] at h1
      have := congrArg List.reverse h1
      simp at this
    · -- prepended (filtered) = 0 :: pre, appended = []: `0` is moved to the front of `[1,0]`.
      rcases List.append_eq_nil_iff.mp hrest.symm with ⟨rfl, happ⟩
      have hp' : c.prepended = [0] := by simpa [happ] using hp
      simp only [apply, hp', happ, List.append_nil] at h2
      simp at h2

/-! ### (4) "Not closed under combination": `reorder` is not a normative edit -/

/-- Positive theorem: every normative composable edit keeps the surviving weaker
elements (those not killed by `appended`/`deleted`/`prepended`) as a sublist,
i.e. in their weaker relative order. -/
theorem apply_preserves_relative_order (c : Composable α) (w : List α) :
    (w.filter (fun x => x ∉ c.appended ∧ x ∉ c.deleted ∧ x ∉ c.prepended)).Sublist
      (apply (.comp c) w) := by
  simp only [apply, List.append_assoc]
  exact (List.sublist_append_left _ _).trans (List.sublist_append_right _ _)

/-- A composable op that iterates to nothing is a pure deletion edit: its
output is a sublist of the weaker list. -/
theorem apply_sublist_of_iterate_nil (c : Composable α) (w : List α)
    (h : iterate (.comp c) = []) : (apply (.comp c) w).Sublist w := by
  simp only [iterate] at h
  rcases List.append_eq_nil_iff.mp h with ⟨hp, ha⟩
  have hp' : c.prepended = [] := by simpa [ha] using hp
  simp only [apply, hp', ha, List.filter_nil, List.nil_append, List.append_nil]
  exact List.filter_sublist

/-- The reorder op `S` used below: no core edit, `ordered = [1, 2]`. -/
def reorderOp : LegacyOps Nat := ⟨⟨[], [], []⟩, [], [1, 2]⟩

/-- `reorder [1,2]` adds and removes nothing, yet flips the relative order of
`1` and `2` in the weaker `[2, 1]`: the output is not a sublist of the input.
Two weaker lists `[1,2] ≠ [2,1]` map to the same output. -/
theorem reorder_flips_order :
    applyLegacy reorderOp [] = []
    ∧ applyLegacy reorderOp [1, 2] = [1, 2]
    ∧ applyLegacy reorderOp [2, 1] = [1, 2]
    ∧ ¬ (applyLegacy reorderOp [2, 1]).Sublist [2, 1] := by decide

/-- Formal content of L1703: no normative `ListOp` agrees with the `reorder`
edit on every weaker list, so a chain containing `reorderOp` has no single
combined normative op (combination is not closed over `reorder`). -/
theorem reorder_not_expressible :
    ¬ ∃ T : ListOp Nat, ∀ l, apply T l = applyLegacy reorderOp l := by
  rintro ⟨T, h⟩
  obtain ⟨e0, e1, e2, e3⟩ := reorder_flips_order
  have h0 := h []
  have h2 := h [2, 1]
  rw [e0] at h0; rw [e2] at h2
  cases T with
  | explicit e => simp [apply] at h0 h2; rw [h0] at h2; simp at h2
  | comp c =>
    have hsub := apply_sublist_of_iterate_nil c [2, 1] (by rw [← apply_nil]; exact h0)
    rw [h2] at hsub
    rw [e2] at e3
    exact e3 hsub

/-- The same conclusion stated against `combine`: there is no `ListOp` `T` such
that `iterate (T ⊔ W) = applyLegacy reorderOp (iterate W)` for every explicit `W`. -/
theorem reorder_not_combinable :
    ¬ ∃ T : ListOp Nat, ∀ e : List Nat, iterate (T ⊔ .explicit e) = applyLegacy reorderOp e := by
  rintro ⟨T, h⟩
  exact reorder_not_expressible ⟨T, fun l => by rw [← iterate_explicit l, ← iterate_combine, iterate_explicit]; exact h l⟩

end UsdSpec.ListOp
