import UsdSpec.ListOp.Legacy

/-!
# Ordered prim and property children (AOUSD USD Core Specification v1.1.0, §11)

SPEC: core_spec.md L5937-6004 (Ordered Prim Children, Ordered Property
Children), with `populate_stage` / `populate_children` L5840-5935.

Faithful transcription of the pseudocode `reorder`, `list_ordered_prim_children`
and `list_ordered_property_children` on a small model: a scene object's
contributing layer stacks (strongest first), each with its relocated children
(`removed`, `extended`, `renamed` source→target) and its specs (strongest first)
carrying `primChildren` / `primOrder` / `properties` / `propertyOrder`.
`path_element_sorted` is a parameter (`sort`), see UsdSpec/Path/Order.lean.

Findings:
* SPEC-ISSUE: LEAN-603 — the spec `reorder` (ordered names first, then the
  rest) is not pxr's `SdfApplyListOrdering`/`_ReorderKeysHelper`
  (UsdSpec.ListOp.reorder in UsdSpec/ListOp/Legacy.lean), which keeps the
  prefix before the first ordered name in front and moves each ordered name
  together with the following run of unordered names (`reorder_ne_pxr`).
* SPEC-ISSUE: LEAN-601 — prose L5948-5950 vs the final filter L5981:
  the filter also removes a relocate target that reuses a name relocated in
  the *same* layer stack (`rename_target_dropped`); allowed by the literal
  bullets (LEAN-514), rejected by pxr.
* SPEC-ISSUE: LEAN-604 — `unique` is only applied inside the spec loop, so a
  strongest layer stack with no spec for the prim can leave duplicates
  (`dup_without_specs`); the result is `Nodup` when the strongest layer stack
  has a spec (`listOrderedPrimChildren_nodup`).
* SPEC-ISSUE: LEAN-602 — `populate_stage` (L5855-5898): `return
  create_stage(layer)` is indented inside the `while` loop (returns after the
  pseudo-root), and the payload-loading flag of the population mask
  (L5847-5849) is never passed to `compose`. Verified by reading; not modelled.
-/

namespace UsdSpec.Population

/-! ## `unique` and `reorder` -/

variable {α : Type} [DecidableEq α]

/-- First occurrence kept (L5971-5972). -/
def unique : List α → List α
  | [] => []
  | x :: xs => x :: (unique xs).filter (· ≠ x)

theorem mem_unique {x : α} {l : List α} : x ∈ unique l ↔ x ∈ l := by
  induction l with
  | nil => simp [unique]
  | cons y ys ih =>
    simp only [unique, List.mem_cons, List.mem_filter, ih, decide_eq_true_eq]
    by_cases h : x = y <;> simp [h]

theorem nodup_unique (l : List α) : (unique l).Nodup := by
  induction l with
  | nil => simp [unique]
  | cons y ys ih =>
    simp only [unique, List.nodup_cons, List.mem_filter, decide_eq_true_eq]
    exact ⟨by simp, ih.filter _⟩

/-- SPEC: core_spec.md L5951-5954 (`reorder`). -/
def reorder (names order : List α) : List α :=
  order.filter (· ∈ names) ++ names.filter (· ∉ order)

theorem reorder_nodup {names order : List α} (hn : names.Nodup) (ho : order.Nodup) :
    (reorder names order).Nodup := by
  unfold reorder
  rw [List.nodup_append]
  refine ⟨ho.filter _, hn.filter _, ?_⟩
  intro a ha b hb hab
  subst hab
  simp only [List.mem_filter, decide_eq_true_eq] at ha hb
  exact hb.2 ha.1

/-- `reorder` is a permutation (on duplicate-free inputs, as the pseudocode
always passes `unique(...)`). -/
theorem reorder_perm {names order : List α} (hn : names.Nodup) (ho : order.Nodup) :
    (reorder names order).Perm names := by
  have h1 : (order.filter (· ∈ names)).Perm (names.filter (· ∈ order)) := by
    rw [List.perm_ext_iff_of_nodup (ho.filter _) (hn.filter _)]
    intro x; simp [and_comm]
  have h2 := List.filter_append_perm (fun x => decide (x ∈ order)) names
  unfold reorder
  refine (h1.append_right _).trans ?_
  simpa using h2

/-- SPEC-ISSUE: LEAN-603 — spec `reorder` vs pxr `_ReorderKeysHelper`
(children `x a b`, `primOrder = [b, a]`): spec `b a x`, pxr 26.11 `x b a`. -/
theorem reorder_ne_pxr :
    reorder ["x", "a", "b"] ["b", "a"] = ["b", "a", "x"] ∧
    UsdSpec.ListOp.reorder ["b", "a"] ["x", "a", "b"] = ["x", "b", "a"] := by
  decide

/-- They agree when the first child is an ordered name followed by nothing
movable, e.g. `a x b`: both give `b a x` (pxr 26.11 confirms). -/
example : reorder ["a", "x", "b"] ["b", "a"] = ["b", "a", "x"] ∧
    UsdSpec.ListOp.reorder ["b", "a"] ["a", "x", "b"] = ["b", "a", "x"] := by decide

/-! ## Model -/

structure Spec where
  primChildren : List String := []
  primOrder : List String := []
  properties : List String := []
  propertyOrder : Option (List String) := none
  deriving DecidableEq, Repr

/-- One contributing layer stack: `relocated_children(scene_object)` and its
strength-ordered specs (strongest first). -/
structure Stack where
  removed : List String := []
  extended : List String := []
  renamed : List (String × String) := []
  specs : List Spec := []
  deriving DecidableEq, Repr

def rename (ren : List (String × String)) (c : String) : String :=
  (ren.lookup c).getD c

def specStep (pc : List String) (s : Spec) : List String :=
  reorder (unique (pc ++ s.primChildren)) (unique s.primOrder)

/-- Loop body of `list_ordered_prim_children` for one layer stack. -/
def stackStep (sort : List String → List String)
    (st : List String × List String) (ls : Stack) : List String × List String :=
  let pc := st.1.filter (· ∉ ls.removed)
  let pc := pc.map (rename ls.renamed)
  let pc := pc ++ sort ls.extended
  let rel := st.2 ++ ls.removed ++ ls.renamed.map Prod.fst
  (ls.specs.reverse.foldl specStep pc, rel)

/-- SPEC: core_spec.md L5956-5981 (`list_ordered_prim_children`); `stacks`
strongest first. -/
def listOrderedPrimChildren (sort : List String → List String) (lss : List Stack) :
    List String :=
  let st := lss.reverse.foldl (stackStep sort) ([], [])
  st.1.filter (· ∉ st.2)

/-- SPEC: core_spec.md L5995-6001 (`list_ordered_property_children`); specs
strongest first. -/
def strongestPropertyOrder (specs : List Spec) : Option (List String) :=
  specs.findSome? (·.propertyOrder)

def listOrderedPropertyChildren (sort : List String → List String)
    (defProps : List String) (specs : List Spec) : List String :=
  reorder (sort (unique (defProps ++ specs.flatMap (·.properties))))
    ((strongestPropertyOrder specs).getD [])

/-! ## Theorems -/

theorem nodup_specStep (pc : List String) (s : Spec) : (specStep pc s).Nodup :=
  reorder_nodup (nodup_unique _) (nodup_unique _)

theorem nodup_foldl_specStep : ∀ (l : List Spec) (pc : List String), l ≠ [] →
    (l.foldl specStep pc).Nodup
  | [], _, h => absurd rfl h
  | [s], pc, _ => by simpa using nodup_specStep pc s
  | s :: t :: rest, pc, _ => nodup_foldl_specStep (t :: rest) (specStep pc s) (by simp)

/-- The result is duplicate-free whenever the strongest layer stack has a spec. -/
theorem listOrderedPrimChildren_nodup (sort : List String → List String) (s : Stack)
    (rest : List Stack) (hs : s.specs ≠ []) :
    (listOrderedPrimChildren sort (s :: rest)).Nodup := by
  unfold listOrderedPrimChildren
  simp only [List.reverse_cons, List.foldl_append, List.foldl_cons, List.foldl_nil]
  apply List.Nodup.filter
  exact nodup_foldl_specStep _ _ (by simpa using hs)

/-- SPEC-ISSUE: LEAN-604 — without a strongest spec, a rename onto an existing
child duplicates it. -/
theorem dup_without_specs :
    listOrderedPrimChildren id
      [{ renamed := [("X", "Y")] }, { specs := [{ primChildren := ["X", "Y"] }] }] = ["Y", "Y"] := by
  decide

/-- Cross-stack rename back (`X→Y` in a middle stack, `Y→X` in the strongest):
the name `X` stays forbidden. Prose and code agree; pxr 26.11 gives `[Z]`. -/
theorem rename_back_cross_stack :
    listOrderedPrimChildren id
      [{ renamed := [("Y", "X")], specs := [{}] }, { renamed := [("X", "Y")], specs := [{}] },
       { specs := [{ primChildren := ["X", "Z"] }] }] = ["Z"] := by
  decide

/-- SPEC-ISSUE: LEAN-601 — same-stack `A→B`, `C→A` (valid under the literal
bullets, `UsdSpec.Relocates.chain_valid_literal`): the relocated `C`, now named
`A`, is dropped by the final filter although no *stronger* stack introduced
`A` (L5948-5950). -/
theorem rename_target_dropped :
    listOrderedPrimChildren id
      [{ renamed := [("A", "B"), ("C", "A")], specs := [{}] },
       { specs := [{ primChildren := ["A", "C"] }] }] = ["B"] := by
  decide

/-- Only the strongest authored `propertyOrder` matters (L6003-6004). -/
theorem propertyOrder_strongest_only (sort : List String → List String) (defProps : List String)
    (pre post post' : List Spec) (s : Spec) (o : List String)
    (hpre : ∀ x ∈ pre, x.propertyOrder = none) (hs : s.propertyOrder = some o)
    (hp : post'.map (·.properties) = post.map (·.properties)) :
    listOrderedPropertyChildren sort defProps (pre ++ s :: post') =
      listOrderedPropertyChildren sort defProps (pre ++ s :: post) := by
  have ho : ∀ q : List Spec, strongestPropertyOrder (pre ++ s :: q) = some o := by
    intro q
    unfold strongestPropertyOrder
    rw [List.findSome?_append]
    rw [List.findSome?_eq_none_iff.mpr hpre]
    simp [hs]
  have hf : post'.flatMap (·.properties) = post.flatMap (·.properties) := by
    rw [List.flatMap_def, List.flatMap_def, hp]
  unfold listOrderedPropertyChildren
  rw [ho, ho]
  simp only [List.flatMap_append, List.flatMap_cons, hf]

/-- Property order is a permutation of the property set when the strongest
`propertyOrder` is duplicate-free (with `sort = id`). -/
theorem propertyChildren_perm (defProps : List String) (specs : List Spec)
    (hO : ((strongestPropertyOrder specs).getD []).Nodup) :
    (listOrderedPropertyChildren id defProps specs).Perm
      (unique (defProps ++ specs.flatMap (·.properties))) :=
  reorder_perm (nodup_unique _) hO

/-- SPEC-ISSUE: LEAN-605 — unlike `primOrder` (L5980, `unique(spec.prim_order)`),
the property path passes `propertyOrder` without `unique`, so a repeated
entry duplicates a property. -/
theorem propertyOrder_dup :
    listOrderedPropertyChildren id [] [{ properties := ["a"], propertyOrder := some ["a", "a"] }] =
      ["a", "a"] := by
  decide

end UsdSpec.Population
