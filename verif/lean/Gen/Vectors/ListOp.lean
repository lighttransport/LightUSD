import UsdSpec.ListOp
import Gen.Inc

/-!
List-op vectors: `LEAN_LISTOP` rows exercise `apply`/`combine` against the
C++ `ApplyStringListOp` / `ApplyInt64ListOps`; `LEAN_LISTOP_CHAIN` rows
exercise chained application against `ApplyInt64ListOps` on a stack.
-/
namespace Gen.ListOpVec
open UsdSpec UsdSpec.ListOp

/-- All duplicate-free sequences of length ≤ 2 over `{1,2,3}`. -/
def seqs : List (List Int) :=
  [[], [1], [2], [3], [1,2], [1,3], [2,1], [2,3], [3,1], [3,2]]

def compOps : List (Composable Int) :=
  seqs.flatMap fun d => seqs.flatMap fun p => seqs.map fun a => ⟨d, p, a⟩

def allOps : List (ListOp Int) :=
  seqs.map ListOp.explicit ++ compOps.map ListOp.comp

/-- Weaker lists used for the exhaustive apply table. -/
def weakers : List (List Int) := [[], [1], [2], [2, 1], [3, 1], [1, 2, 3], [3, 2, 1]]

def encodeOp : ListOp Int → String
  | .explicit e => "E:" ++ intList e
  | .comp c => "C:" ++ intList c.deleted ++ "|" ++ intList c.prepended ++ "|" ++ intList c.appended

/-- `LEAN_LISTOP(id, is_explicit, expl, pre, app, del, weaker, expected)` -/
def applyRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for op in allOps do
    for w in weakers do
      let (isE, e, p, a, d) := match op with
        | .explicit e => ("1", e, [], [], [])
        | .comp c => ("0", [], c.prepended, c.appended, c.deleted)
      out := out ++ row "LEAN_LISTOP" [toString id, isE, cstr (intList e), cstr (intList p),
        cstr (intList a), cstr (intList d), cstr (intList w), cstr (intList (apply op w))]
      id := id + 1
  return out

/-- Spec worked examples (L1455-1460 and the congruence examples L1520-1570 as iterate checks). -/
def specRows : String :=
  let ex : List (ListOp Int × List Int) :=
    [ (.comp ⟨[5], [5], [5]⟩, [4, 5, 6]),
      (.comp ⟨[50], [10], [10, 50]⟩, []), (.comp ⟨[], [10], [10, 50]⟩, [7]),
      (.comp ⟨[], [50], [10]⟩, [10, 50]), (.comp ⟨[], [], [50, 10]⟩, [10, 50]) ]
  Id.run do
    let mut out := ""
    let mut id := 100000
    for (op, w) in ex do
      let (isE, e, p, a, d) := match op with
        | .explicit e => ("1", e, [], [], [])
        | .comp c => ("0", [], c.prepended, c.appended, c.deleted)
      out := out ++ row "LEAN_LISTOP" [toString id, isE, cstr (intList e), cstr (intList p),
        cstr (intList a), cstr (intList d), cstr (intList w), cstr (intList (apply op w))]
      id := id + 1
    return out

/-- `LEAN_LISTOP_CHAIN(id, ops, expected_iterate)`; ops strongest first, `;`-separated. -/
def chainRows : String := Id.run do
  let ops := allOps.toArray
  let mut g : Lcg := ⟨0x5EED1234⟩
  let mut out := ""
  for id in [0:1500] do
    let (g1, r1) := g.next
    let (g2, r2) := g1.next
    let (g3, r3) := g2.next
    let (g4, r4) := g3.next
    g := g4
    let n := 2 + r4 % 3
    let picks := [r1, r2, r3].take n |>.map fun r => ops[r % ops.size]!
    let expected := iterate (chain picks)
    out := out ++ row "LEAN_LISTOP_CHAIN" [toString id, cstr (";".intercalate (picks.map encodeOp)),
      cstr (intList expected)]
  return out

def files : List (String × String) :=
  [ ("listop-vectors.inc", header "Gen.Vectors.ListOp" ++
      "// LEAN_LISTOP(id, is_explicit, explicit, prepended, appended, deleted, weaker, expected)\n" ++
      "// expected = UsdSpec.ListOp.apply op weaker = iterate (op ⊔ explicit weaker)\n" ++
      applyRows ++ specRows),
    ("listop-chain-vectors.inc", header "Gen.Vectors.ListOp" ++
      "// LEAN_LISTOP_CHAIN(id, ops, expected): ops strongest-first, `;`-separated,\n" ++
      "// each `E:items` or `C:deleted|prepended|appended`; expected = iterate (chain ops)\n" ++
      chainRows) ]

end Gen.ListOpVec
