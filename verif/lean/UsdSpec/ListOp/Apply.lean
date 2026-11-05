import UsdSpec.ListOp

/-!
# Implementation-shaped list-op application

LightUSD applies list ops as edits over an already-resolved weaker list. Two
code paths exist:

* `ApplyStringListOp` (src/next/layer/prim-spec-storage.cc:352-384): push
  prepended items uniquely, push weaker items not deleted uniquely, push
  `added` uniquely (legacy, modelled in `ListOp/Legacy.lean`), then for each
  appended item erase it and push it at the end, then `ApplyStringListOrder`
  (legacy `ordered`, also in `Legacy.lean`).
* `ApplyInt64Op` (src/next/layer/prim-spec-storage.cc:182-241): prepended
  minus appended, base minus deleted minus appended, added minus appended,
  then appended, then `UniqueIds` (keep the first occurrence, except appended
  ids keep the last).

Both are modelled here with `added = []`/`ordered = []` and proven equal to the
normative `ListOp.apply` (hence, via `iterate_combine`, to `⊔`) whenever the
authored sequences and the weaker list are duplicate-free (spec L1308-1337).
The duplicate-free hypothesis is necessary: the C++ de-duplicates, the spec
model does not (`applyCpp_ne_apply_of_dup`).
-/

namespace UsdSpec.ListOp

/-- Close a `decide p = decide q && ...` goal by propositional reasoning. -/
macro "bool_prop" : tactic =>
  `(tactic| (apply Bool.eq_iff_iff.mpr;
             simp only [Bool.and_eq_true, decide_eq_true_eq, List.mem_cons, List.mem_singleton,
               List.mem_append, List.not_mem_nil, not_or, ne_eq, not_false_eq_true, and_true];
             try tauto))

variable {α : Type} [DecidableEq α]

/-- Keep the first occurrence of each element; `seen` are elements already emitted. -/
def dedupGo (seen : List α) : List α → List α
  | [] => []
  | x :: xs => if x ∈ seen then dedupGo seen xs else x :: dedupGo (x :: seen) xs

/-- Keep the first occurrence of each element. -/
def dedupFirst (l : List α) : List α := dedupGo [] l

/-- `result.push_back(x)` unless `x` is already present. -/
def pushUnique (r : List α) (x : α) : List α := if x ∈ r then r else r ++ [x]

/-- `ApplyStringListOp` with `added = []` and `ordered = []`. -/
def applyCpp (c : Composable α) (w : List α) : List α :=
  let r1 := c.prepended.foldl pushUnique []
  let r2 := w.foldl (fun r x => if x ∈ c.deleted then r else pushUnique r x) r1
  c.appended.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r2

/-- `ApplyInt64Op` with `added = []`. `UniqueIds(result, appended)` keeps the
first occurrence of non-appended ids and the last occurrence of appended ids;
since every appended id occurs only in the trailing `appended` block, that is
`dedupFirst` of the prefix followed by the appended block de-duplicated keeping
last occurrences (`dedupLast`). -/
def dedupLast (l : List α) : List α := (dedupFirst l.reverse).reverse

def applyCppInt64 (c : Composable α) (base : List α) : List α :=
  dedupFirst (c.prepended.filter (· ∉ c.appended)
    ++ base.filter (fun x => x ∉ c.deleted ∧ x ∉ c.appended))
  ++ dedupLast c.appended

/-! ## Lemmas about `dedupGo` -/

theorem dedupGo_congr {seen seen' : List α} (h : ∀ x, x ∈ seen ↔ x ∈ seen') (l : List α) :
    dedupGo seen l = dedupGo seen' l := by
  induction l generalizing seen seen' with
  | nil => rfl
  | cons x xs ih =>
    simp only [dedupGo]
    by_cases hx : x ∈ seen
    · have hx2 : x ∈ seen' := (h x).mp hx
      simp only [hx, hx2, ite_true]
      exact ih h
    · have hx2 : x ∉ seen' := fun h' => hx ((h x).mpr h')
      simp only [hx, hx2, ite_false, List.cons.injEq, true_and]
      apply ih; intro y; simp [h y]

theorem dedupGo_append (seen l₁ l₂ : List α) :
    dedupGo seen (l₁ ++ l₂) = dedupGo seen l₁ ++ dedupGo (l₁ ++ seen) l₂ := by
  induction l₁ generalizing seen with
  | nil => simp [dedupGo]
  | cons x xs ih =>
    simp only [List.cons_append, dedupGo]
    by_cases hx : x ∈ seen
    · simp only [hx, ite_true, ih]
      congr 1
      apply dedupGo_congr; intro y
      by_cases hy : y = x <;> simp [hy, hx]
    · simp only [hx, ite_false, ih, List.cons_append]
      congr 2
      apply dedupGo_congr; intro y; simp only [List.mem_append, List.mem_cons]; tauto

theorem dedupGo_eq_filter (seen l : List α) :
    dedupGo seen l = (dedupGo [] l).filter (· ∉ seen) := by
  induction l generalizing seen with
  | nil => rfl
  | cons x xs ih =>
    simp only [dedupGo, List.not_mem_nil, ite_false, List.filter_cons]
    rw [ih seen, ih [x], ih (x :: seen), List.filter_filter]
    by_cases hx : x ∈ seen
    · simp only [hx, ite_true, not_true_eq_false, decide_false, Bool.false_eq_true, ite_false]
      apply List.filter_congr; intro y _
      by_cases hy : y = x
      · subst hy; simp [hx]
      · bool_prop
    · simp only [hx, ite_false, not_false_eq_true, decide_true, ite_true, List.cons.injEq, true_and]
      apply List.filter_congr; intro y _
      bool_prop

theorem dedupGo_of_nodup (seen l : List α) (h : l.Nodup) (hd : ∀ x ∈ l, x ∉ seen) :
    dedupGo seen l = l := by
  induction l generalizing seen with
  | nil => rfl
  | cons x xs ih =>
    rw [List.nodup_cons] at h
    simp only [dedupGo, hd x List.mem_cons_self, ite_false, List.cons.injEq, true_and]
    apply ih _ h.2
    intro y hy
    simp only [List.mem_cons, not_or]
    exact ⟨fun e => h.1 (e ▸ hy), hd y (List.mem_cons_of_mem _ hy)⟩

theorem dedupFirst_of_nodup (l : List α) (h : l.Nodup) : dedupFirst l = l :=
  dedupGo_of_nodup [] l h (fun _ _ => List.not_mem_nil)

theorem dedupFirst_append_of_nodup (l₁ l₂ : List α) (h : l₁.Nodup) :
    dedupFirst (l₁ ++ l₂) = l₁ ++ (dedupFirst l₂).filter (· ∉ l₁) := by
  simp only [dedupFirst]
  rw [dedupGo_append, dedupGo_of_nodup _ _ h (fun _ _ => List.not_mem_nil), List.append_nil,
    dedupGo_eq_filter]

theorem foldl_pushUnique (r₀ l : List α) :
    l.foldl pushUnique r₀ = r₀ ++ dedupGo r₀ l := by
  induction l generalizing r₀ with
  | nil => simp [dedupGo]
  | cons x xs ih =>
    simp only [List.foldl_cons, pushUnique, dedupGo]
    by_cases hx : x ∈ r₀
    · simp only [hx, ite_true, ih]
    · simp only [hx, ite_false, ih, List.append_assoc, List.singleton_append]
      congr 2
      apply dedupGo_congr; intro y; simp only [List.mem_append, List.mem_singleton, List.mem_cons]
      tauto

theorem foldl_pushUnique_filter (d r₀ l : List α) :
    l.foldl (fun r x => if x ∈ d then r else pushUnique r x) r₀
      = (l.filter (· ∉ d)).foldl pushUnique r₀ := by
  induction l generalizing r₀ with
  | nil => rfl
  | cons x xs ih =>
    by_cases hx : x ∈ d <;> simp [List.filter_cons, hx, ih]

theorem foldl_moveToEnd (r₀ l : List α) (h : l.Nodup) :
    l.foldl (fun r x => r.filter (· ≠ x) ++ [x]) r₀ = r₀.filter (· ∉ l) ++ l := by
  induction l generalizing r₀ with
  | nil => simp
  | cons a l ih =>
    rw [List.nodup_cons] at h
    simp only [List.foldl_cons]
    rw [ih _ h.2, List.filter_append, List.filter_filter, List.filter_singleton]
    simp only [h.1, not_false_eq_true, decide_true, ite_true, List.append_assoc,
      List.singleton_append]
    congr 1
    apply List.filter_congr; intro y _
    bool_prop

/-! ## The equivalence theorems -/

/-- `ApplyStringListOp` agrees with the spec on duplicate-free inputs. -/
theorem applyCpp_eq_apply (c : Composable α) (w : List α)
    (hp : c.prepended.Nodup) (ha : c.appended.Nodup) (hw : w.Nodup) :
    applyCpp c w = apply (.comp c) w := by
  simp only [applyCpp, apply]
  rw [foldl_pushUnique, List.nil_append, ← dedupFirst, dedupFirst_of_nodup _ hp,
    foldl_pushUnique_filter, foldl_pushUnique, dedupGo_eq_filter, ← dedupFirst,
    dedupFirst_of_nodup _ (hw.filter _), foldl_moveToEnd _ _ ha,
    List.filter_append, List.filter_filter, List.filter_filter]
  simp only [List.append_assoc]
  congr 2
  apply List.filter_congr; intro x _
  bool_prop

/-- `ApplyInt64Op` agrees with the spec on duplicate-free inputs. -/
theorem applyCppInt64_eq_apply (c : Composable α) (base : List α)
    (hp : c.prepended.Nodup) (ha : c.appended.Nodup) (hb : base.Nodup) :
    applyCppInt64 c base = apply (.comp c) base := by
  simp only [applyCppInt64, apply, dedupLast]
  rw [dedupFirst_append_of_nodup _ _ (hp.filter _),
    dedupFirst_of_nodup _ (hb.filter _),
    dedupFirst_of_nodup _ (List.nodup_reverse.mpr ha), List.reverse_reverse, List.filter_filter]
  simp only [List.append_assoc]
  congr 2
  apply List.filter_congr; intro x _
  simp only [List.mem_filter]
  bool_prop

/-- The duplicate-free hypothesis is needed: the implementation de-duplicates. -/
theorem applyCpp_ne_apply_of_dup :
    applyCpp (⟨[], [], []⟩ : Composable Nat) [1, 1] ≠ apply (.comp ⟨[], [], []⟩) [1, 1] := by
  decide

/-- Corollary: the implementation's edit of the weaker *iterated* list is the
iteration of the combined op — the spec's normative traversal (L1654-1655). -/
theorem applyCpp_iterate (c : Composable α) (W : ListOp α)
    (hp : c.prepended.Nodup) (ha : c.appended.Nodup) (hw : (iterate W).Nodup) :
    applyCpp c (iterate W) = iterate (.comp c ⊔ W) := by
  rw [applyCpp_eq_apply c _ hp ha hw, iterate_combine]

end UsdSpec.ListOp
