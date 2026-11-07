import UsdSpec.Crate.ValueRep
import UsdSpec.Crate.ListOpEncoding
import Gen.Inc

/-!
Crate ValueRep and list-op vectors.

`LEAN_VALUEREP(id, "u64 hex", type, is_array, is_inlined, is_compressed, "payload hex")`:
`u64` is `UsdSpec.Crate.ValueRep.pack` of the decoded fields (OpenUSD bit layout).
C++ check (`lightusd::next::ValueRep`, `src/next/crate/crate-format.hh`):
`ValueRep r(strtoull(u64,..,16))` then `static_cast<int>(r.type_id()) == type`,
`r.is_array()`, `r.is_inlined()`, `r.is_compressed()`, `r.payload() == strtoull(payload)`,
and `ValueRep::Make(CrateTypeId(type), payload, is_array, is_inlined, is_compressed).raw()`
equals `u64`.

`LEAN_CRATE_LISTOP(id, "header hex", "wire hex", is_explicit, "explicit", "added",
"prepended", "appended", "deleted", "ordered")`: `wire` is
`UsdSpec.Crate.ListOpEncoding.encode` (header byte, then u64-count + u64-LE items per
present vector in spec order); item lists are decimal uint64, comma-separated. Check
against `lightusd::next::Int64ListOp` (`bits`, `items[k]` in `kInt64ListOpBits` order).
-/
namespace Gen.CrateVec
open UsdSpec.Crate

def hexDigits (n : Nat) (width : Nat) : String :=
  let s := String.ofList (Nat.toDigits 16 n)
  String.ofList (List.replicate (width - s.length) '0') ++ s

def hex64 (x : UInt64) : String := "0x" ++ hexDigits x.toNat 16
def hex8 (x : UInt8) : String := "0x" ++ hexDigits x.toNat 2
def bytesHex (bs : List UInt8) : String := String.join (bs.map fun b => hexDigits b.toNat 2)
def b01 (b : Bool) : String := if b then "1" else "0"

open ValueRep in
def repSamples : List Rep :=
  [ ⟨0, 0, false, false, false, false⟩,
    ⟨0xf, tyToken, false, true, false, false⟩,              -- pxr token typeName
    ⟨0x3e003800, tyVec2h, false, true, false, false⟩,       -- pxr half2 (0.5, 1.5)
    ⟨0x3f000000, tyDouble, false, true, false, false⟩,      -- pxr double 0.5 (as f32)
    ⟨0x8c, tyDouble, false, false, false, false⟩,           -- pxr double 0.1, out of line
    ⟨0xfffffff9, tyInt, false, true, false, false⟩,         -- pxr int -7
    ⟨0xff0302, tyMatrix3d, false, true, false, false⟩,      -- pxr matrix3d diag(2,3,-1)
    ⟨0x58, 41, false, false, false, false⟩,                 -- pxr TokenVector field
    ⟨0x1234, tyInt, true, false, false, false⟩,
    ⟨0x1234, tyInt, true, false, true, false⟩,
    ⟨0xFFFFFFFFFFFF, 0xFF, true, true, true, false⟩,
    ⟨0x800000000000, tyFloat, true, false, true, false⟩,
    ⟨0x10, tyVec3f, false, true, false, false⟩ ]

def repRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for r in repSamples do
    let u := ValueRep.pack r
    let d := ValueRep.unpack u
    out := out ++ row "LEAN_VALUEREP" [toString id, cstr (hex64 u), toString d.ty.toNat,
      b01 d.isArray, b01 d.isInlined, b01 d.isCompressed, cstr (hex64 d.payload)]
    id := id + 1
  return out

open ListOpEncoding in
def listOpSamples : List CrateListOp :=
  [ ⟨true, [], [], [], [], [], []⟩,
    ⟨false, [], [], [], [], [], []⟩,
    ofSpec (.explicit [1, 2, 3]),
    ofSpec (.comp ⟨[3], [1, 2], []⟩),
    ofSpec (.comp ⟨[], [], [7]⟩),
    ofSpec (.comp ⟨[5, 6], [0xFFFFFFFFFFFFFFFF], [9, 10]⟩),
    ⟨false, [], [4], [], [], [], [2, 1]⟩,
    ⟨false, [], [1], [2], [3], [4], [5]⟩ ]

def ints (xs : List UInt64) : String := ",".intercalate (xs.map fun x => toString x.toNat)

def listOpRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for o in listOpSamples do
    out := out ++ row "LEAN_CRATE_LISTOP" [toString id, cstr (hex8 (ListOpEncoding.header o)),
      cstr (bytesHex (ListOpEncoding.encode o)), b01 o.isExplicit, cstr (ints o.explicit),
      cstr (ints o.added), cstr (ints o.prepended), cstr (ints o.appended),
      cstr (ints o.deleted), cstr (ints o.ordered)]
    id := id + 1
  return out

/-- All legal raw headers, with populated and empty present vectors. Expected
mode and retained vectors come from the OpenUSD setter model, not raw `decode`. -/
def listOpModeRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for empty in [false, true] do
    for code in List.range 128 do
      let h := UInt8.ofNat code
      let items (run : Nat) : List UInt64 := if empty then [] else [UInt64.ofNat (run + 10)]
      let raw : ListOpEncoding.CrateListOp :=
        ⟨ListOpEncoding.has h 1, items 0, items 1, items 2, items 3, items 4, items 5⟩
      let mut wire := [h]
      for (bit, run) in ListOpEncoding.wireBits.zip (List.range 6) do
        if ListOpEncoding.has h bit then wire := wire ++ ListOpEncoding.encodeVec (items run)
      let effective := ListOpEncoding.pxrRead raw h
      out := out ++ row "LEAN_CRATE_LISTOP_MODE" [toString id, cstr (hex8 h),
        cstr (bytesHex wire), b01 effective.isExplicit, cstr (ints effective.explicit),
        cstr (ints effective.added), cstr (ints effective.prepended),
        cstr (ints effective.appended), cstr (ints effective.deleted), cstr (ints effective.ordered)]
      id := id + 1
  return out

def files : List (String × String) :=
  [ ("crate-valuerep-vectors.inc", header "Gen.Vectors.Crate" ++
      "// LEAN_VALUEREP(id, u64_hex, type, is_array, is_inlined, is_compressed, payload_hex)\n" ++
      "// spec L12049-12077; OpenUSD layout: bit63 array, bit62 inlined, bit61 compressed,\n" ++
      "// bits 48-55 type, bits 0-47 payload.\n" ++ repRows),
    ("crate-listop-vectors.inc", header "Gen.Vectors.Crate" ++
      "// LEAN_CRATE_LISTOP(id, header_hex, wire_hex, is_explicit, explicit, added, prepended,\n" ++
      "//   appended, deleted, ordered) spec L12558-12636; uint64 items, u64 LE on the wire.\n" ++
      listOpRows ++ "// LEAN_CRATE_LISTOP_MODE: same columns; expected OpenUSD setter semantics.\n" ++
      listOpModeRows) ]

end Gen.CrateVec
