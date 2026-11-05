import UsdSpec.ListOp

/-!
# Arc lists from per-layer list ops (AOUSD USD Core Specification v1.1.0, §10.3)

SPEC: core_spec.md L4364-4379 (Composition Arcs): "the corresponding list-op
from the weakest layer is applied to an empty list, then the list-op from the
next strongest layer is applied to the result, and so on … the first element
is the strongest of those arcs".

`arcList` takes the per-layer ops strongest first (layer-stack order) and is
the right fold of `apply` from `[]` — i.e. weakest applied first. It equals
the iteration of the chained op (`arcList_eq_iterate`), so the arc list of a
layer stack depends only on the combined list op.
-/

namespace UsdSpec.Arcs

open UsdSpec.ListOp

variable {α : Type} [DecidableEq α]

/-- SPEC: core_spec.md L4371-4379 (Composition Arcs). -/
def arcList (opsStrongestFirst : List (ListOp α)) : List α :=
  apply (chain opsStrongestFirst) []

/-- Weakest applied first, starting from the empty list. -/
theorem arcList_eq_foldr (ops : List (ListOp α)) :
    arcList ops = ops.foldr (fun L acc => apply L acc) [] := by
  simp [arcList, apply_chain]

theorem arcList_eq_iterate (ops : List (ListOp α)) :
    arcList ops = iterate (chain ops) := by
  simp [arcList, apply_nil]

theorem arcList_nil : arcList ([] : List (ListOp α)) = [] := by
  simp [arcList_eq_foldr]

theorem arcList_cons (L : ListOp α) (ops : List (ListOp α)) :
    arcList (L :: ops) = apply L (arcList ops) := by
  simp [arcList_eq_foldr]

/-- Two layers: strong prepends `b`, weak prepends `a` ⇒ `[b, a]`. -/
example : arcList [ListOp.comp ⟨[], ["b"], []⟩, ListOp.comp ⟨[], ["a"], []⟩] = ["b", "a"] := by
  decide

end UsdSpec.Arcs
