import UsdSpec.Crate.IntegerCoding
import Gen.Inc

/-!
Integer-coding vectors: `LEAN_INTCODE(id, n, values, bytes, tie)` where `values`
are uint32 bit patterns (decimal), `bytes` is `encode32` of them read as
`Int32`, and `tie = 1` when the most-common delta is not unique (OpenUSD picks
the largest, LightUSD the smallest — decode-safe either way — so the C++
*encoder* is only compared on tie-free rows; the decoder is compared on all).
-/
namespace Gen.IntCodeVec
open UsdSpec.Crate

def hasTie (xs : List Int32) : Bool :=
  let ds := toDeltasFrom 0 xs
  let counts := ds.map fun d => (ds.filter (· == d)).length
  match counts.max? with
  | none => false
  | some m => (counts.filter (· == m)).length > 1 && (ds.eraseDups.filter fun d => (ds.filter (· == d)).length == m).length > 1

def u32 (n : Nat) : Int32 := (UInt32.ofNat n).toInt32

def samples : List (List Int32) :=
  [ [], [0], [1], [u32 0xFFFFFFFF],
    [123, 124, 125, 100125, 100125, 100126, 100126],
    [5, 5, 5, 5], [0, 0, 0, 0, 0], [10, 20, 30, 40],
    [0, 127], [0, 128], [0, 129], [0, 32767], [0, 32768], [0, 32769],
    [u32 0x7FFFFFFF], [u32 0x80000000], [u32 0x7FFFFFFF, u32 0x80000000],
    [3, 3, 5, 5], [1, 0, 1, 0, 1, 0], (List.range 37).map u32,
    (List.range 64).map fun i => u32 (i * 1000),
    [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597] ]

def lcgSamples : List (List Int32) := Id.run do
  let mut g : Lcg := ⟨0xC0DEC0DE⟩
  let mut out := []
  for _ in [0:60] do
    let (g1, r) := g.next
    g := g1
    let n := 1 + r % 23
    let mut xs : List Int32 := []
    let mut cur : Nat := r % 1000
    for _ in [0:n] do
      let (g2, r2) := g.next
      g := g2
      let step := match r2 % 5 with
        | 0 => r2 % 4 | 1 => r2 % 200 | 2 => r2 % 70000 | 3 => r2 % 5000000000 | _ => 1
      cur := (cur + step) % 4294967296
      xs := xs ++ [u32 cur]
    out := out ++ [xs]
  return out

def rows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for xs in samples ++ lcgSamples do
    let vals := xs.map fun x => (x.toUInt32.toNat : Int)
    let bytes := (encode32 xs).map fun b => (b.toNat : Int)
    out := out ++ row "LEAN_INTCODE" [toString id, toString xs.length, cstr (intList vals),
      cstr (intList bytes), if hasTie xs then "1" else "0"]
    id := id + 1
  return out

def files : List (String × String) :=
  [ ("intcoding-vectors.inc", header "Gen.Vectors.IntegerCoding" ++
      "// LEAN_INTCODE(id, n, values_u32, bytes, tie): bytes = encode32 values (spec L11600-11683,\n" ++
      "// before LZ4); tie=1 => most-common delta not unique, compare decoder only.\n" ++ rows) ]

end Gen.IntCodeVec
