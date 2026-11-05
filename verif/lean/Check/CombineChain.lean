import Lean.Data.Json
import UsdSpec.ListOp

/-!
`usdspec-check-corpus <dir>`: runs every `*.json` file of the AOUSD
supplemental corpus `data_types/tests/combine_chain/` through the Lean
list-op model. Each case is `{description, chain: [op...], combined_reduced: op}`
with ops `{explicit_items?, added_items?, prepended_items?, appended_items?,
deleted_items?, ordered_items?}` (int64 items). The chain is strongest first.

Checks per case:
1. `iterate (chain ops) = iterate combined_reduced` (normative traversal).
2. `reduce (chain ops)` equals `combined_reduced` on explicit items, prepended,
   appended, and on deleted as a *set* (the order of deleted items is not
   determined by congruence; see `UsdSpec.ListOp.exists_congruent_reduce_ne`).
   Deleted-order mismatches are reported as INFO, not failures.
-/
open Lean UsdSpec UsdSpec.ListOp

def intsOf (j : Json) (key : String) : Except String (Option (List Int)) :=
  match j.getObjVal? key with
  | .error _ => .ok none
  | .ok v => do
    let arr ← v.getArr?
    let xs ← arr.toList.mapM fun e => e.getInt?
    return some xs

def opOf (j : Json) : Except String (ListOp Int) := do
  let e ← intsOf j "explicit_items"
  let a ← intsOf j "added_items"
  let o ← intsOf j "ordered_items"
  if a.isSome && !(a.getD []).isEmpty then throw "added_items not supported (deprecated, L1699-1708)"
  if o.isSome && !(o.getD []).isEmpty then throw "ordered_items not supported (deprecated, L1699-1708)"
  match e with
  | some items => return .explicit items
  | none =>
    let d := (← intsOf j "deleted_items").getD []
    let p := (← intsOf j "prepended_items").getD []
    let ap := (← intsOf j "appended_items").getD []
    return .comp ⟨d, p, ap⟩

def showOp : ListOp Int → String
  | .explicit e => s!"explicit {e}"
  | .comp c => s!"(del {c.deleted}, pre {c.prepended}, app {c.appended})"

def sameSet (a b : List Int) : Bool := a.all (b.contains ·) && b.all (a.contains ·)

def checkFile (path : System.FilePath) : IO (Nat × Nat × Nat) := do
  let text ← IO.FS.readFile path
  let json ← match Json.parse text with
    | .ok j => pure j | .error e => throw (IO.userError s!"{path}: {e}")
  let cases ← match json.getArr? with
    | .ok a => pure a.toList | .error e => throw (IO.userError s!"{path}: {e}")
  let mut pass := 0
  let mut fail := 0
  let mut info := 0
  for c in cases do
    let desc := (c.getObjValAs? String "description").toOption.getD "?"
    let r : Except String (List (ListOp Int) × ListOp Int) := do
      let chainJ ← (← c.getObjVal? "chain").getArr?
      let ops ← chainJ.toList.mapM opOf
      let expected ← opOf (← c.getObjVal? "combined_reduced")
      return (ops, expected)
    match r with
    | .error e => IO.println s!"SKIP  {path.fileName.getD ""}: {desc}: {e}"; info := info + 1
    | .ok (ops, expected) =>
      let got := reduce (chain ops)
      let okIter := iterate got == iterate expected
      let (okCore, delOrder) := match got, expected with
        | .explicit g, .explicit e => (g == e, true)
        | .comp g, .comp e =>
          (g.prepended == e.prepended && g.appended == e.appended && sameSet g.deleted e.deleted,
           g.deleted == e.deleted)
        | _, _ => (false, true)
      if okIter && okCore then
        pass := pass + 1
        if !delOrder then
          info := info + 1
          IO.println s!"INFO  {desc}: deleted order differs (Lean {showOp got} vs corpus {showOp expected})"
      else
        fail := fail + 1
        IO.println s!"FAIL  {path.fileName.getD ""}: {desc}\n      chain: {ops.map showOp}\n      lean:   {showOp got}\n      corpus: {showOp expected}"
  return (pass, fail, info)

def main (args : List String) : IO UInt32 := do
  let some dir := args.head? | IO.eprintln "usage: usdspec-check-corpus <combine_chain dir>"; return 2
  let entries ← System.FilePath.readDir dir
  let files := (entries.filter fun e => e.path.extension == some "json").map (·.path)
    |>.qsort (fun a b => a.toString < b.toString)
  if files.isEmpty then IO.eprintln s!"no json files in {dir}"; return 77
  let mut tot := (0, 0, 0)
  for f in files do
    let (p, fl, i) ← checkFile f
    tot := (tot.1 + p, tot.2.1 + fl, tot.2.2 + i)
  IO.println s!"combine_chain: {tot.1} pass, {tot.2.1} fail, {tot.2.2} info across {files.size} files"
  return if tot.2.1 == 0 then 0 else 1
