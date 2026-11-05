import Mathlib.Data.List.Basic
/-! Shared list lemmas for the UsdSpec formalization. -/
namespace UsdSpec
example : ([1,2] : List Nat) ++ [3] = [1,2,3] := rfl
end UsdSpec
