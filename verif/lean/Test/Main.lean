import UsdSpec

/-!
`usdspec-test`: runtime sanity checks of the executable definitions (the proofs
are checked by `lake build`; this exercises compiled code paths the generator
relies on, e.g. `DecidableEq`/`BEq` instances and `Repr`).
-/
open UsdSpec UsdSpec.ListOp

def check (name : String) (ok : Bool) : IO Bool := do
  IO.println s!"{if ok then "ok  " else "FAIL"} {name}"
  return ok

def main : IO UInt32 := do
  let mut all := true
  all := (← check "listop spec example L1455-1460"
    ((ListOp.comp ⟨[5], [5], [5]⟩ ⊔ ListOp.explicit [4, 5, 6] : ListOp Int) == .explicit [4, 6, 5])) && all
  all := (← check "listop apply == iterate ∘ combine (random sample)"
    (Id.run do
      let ops : List (ListOp Int) := [.comp ⟨[1], [2], [3]⟩, .explicit [3, 1], .comp ⟨[], [1, 2], [2]⟩]
      let ws : List (List Int) := [[], [1, 2, 3], [3, 2, 1], [2]]
      let mut ok := true
      for op in ops do
        for w in ws do
          ok := ok && (apply op w == iterate (op ⊔ .explicit w))
      return ok)) && all
  all := (← check "listop congrCheck on spec congruence classes"
    (congrCheck (.comp ⟨[50], [10], [10, 50]⟩ : ListOp Int) (.comp ⟨[], [], [10, 50]⟩)
      && !congrCheck (.comp ⟨[], [], [10, 50]⟩ : ListOp Int) (.comp ⟨[], [], [50, 10]⟩))) && all
  all := (← check "intcoding roundtrip"
    (let xs : List Int32 := [123, 124, 125, 100125, 100125, 100126, 100126]
     UsdSpec.Crate.decode32 xs.length (UsdSpec.Crate.encode32 xs) == some xs)) && all
  if all then IO.println "usdspec-test: PASSED"; return 0
  else IO.eprintln "usdspec-test: FAILED"; return 1
