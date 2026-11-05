import Std.Tactic.BVDecide
/-!
# Crate value representations (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L12049-12201 (Value Representations, Inlined Value Representation).

The spec (L12056-12077) says: payload = "first 6 bytes", type = "penultimate byte",
flags = "last byte", with Array = "The last bit", Inlined = "The second last bit",
Compressed = "The third last bit". "Last bit" of the (little-endian) last byte is
ambiguous between its MSB (bit 63 of the u64) and its LSB (bit 56).

OpenUSD `pxr/usd/sdf/crateFile.h` `ValueRep` (L72-145) fixes the MSB reading:
`_IsArrayBit = 1<<63`, `_IsInlinedBit = 1<<62`, `_IsCompressedBit = 1<<61`,
`_IsArrayEditBit = 1<<60` (crate 0.14, outside the spec window), type
`(data >> 48) & 0xFF`, payload `data & ((1<<48)-1)`. LightUSD
`src/next/crate/crate-format.hh` `lightusd::next::ValueRep` (L166-217) is identical
(`is_array/is_inlined/is_compressed/is_array_edit/type_id/payload/raw/Make`).
pxr 26.11 `usdcat` output was inspected: every inlined rep carries flag byte `0x40`
(e.g. `token typeName` → `0x400b00000000000f`), and an out-of-line double
`0x000900000000008c`.

SPEC-ISSUE: LEAN-412 — "last bit / second last bit / third last bit" (L12073-12077)
does not say MSB-first; the LSB-first reading is incompatible with every pxr file.
-/

namespace UsdSpec.Crate.ValueRep

/-- A decoded value representation (OpenUSD layout). -/
structure Rep where
  payload      : UInt64   -- < 2^48
  ty           : UInt8
  isArray      : Bool
  isInlined    : Bool
  isCompressed : Bool
  isArrayEdit  : Bool := false
  deriving DecidableEq, Repr, Inhabited

def payloadMask : UInt64 := 0xFFFFFFFFFFFF

def bit (b : Bool) (k : UInt64) : UInt64 := if b then (1 : UInt64) <<< k else 0

/-- OpenUSD `ValueRep::_Combine` (+ the compressed / array-edit setters). -/
def pack (r : Rep) : UInt64 :=
  bit r.isArray 63 ||| bit r.isInlined 62 ||| bit r.isCompressed 61 ||| bit r.isArrayEdit 60 |||
    (r.ty.toUInt64 <<< 48) ||| (r.payload &&& payloadMask)

/-- OpenUSD accessors `IsArray/IsInlined/IsCompressed/IsArrayEdit/GetType/GetPayload`. -/
def unpack (x : UInt64) : Rep where
  payload := x &&& payloadMask
  ty := (x >>> 48).toUInt8
  isArray := (x >>> 63) &&& 1 == 1
  isInlined := (x >>> 62) &&& 1 == 1
  isCompressed := (x >>> 61) &&& 1 == 1
  isArrayEdit := (x >>> 60) &&& 1 == 1

theorem unpack_pack (r : Rep) (h : r.payload < 0x1000000000000) : unpack (pack r) = r := by
  obtain ⟨p, t, a, i, c, e⟩ := r
  simp only at h
  cases a <;> cases i <;> cases c <;> cases e <;>
    simp only [unpack, pack, bit, payloadMask, Rep.mk.injEq, ↓reduceIte, Bool.false_eq_true] <;>
    bv_decide

/-- Bits 56..59 are unused by every version in the spec window; any u64 with them zero
round-trips through `unpack`. -/
theorem pack_unpack (x : UInt64) (h : (x >>> 56) &&& 0xF = 0) : pack (unpack x) = x := by
  simp only [pack, unpack, bit, payloadMask]
  by_cases a : (x >>> 63) &&& 1 == 1 <;> by_cases i : (x >>> 62) &&& 1 == 1 <;>
    by_cases c : (x >>> 61) &&& 1 == 1 <;> by_cases e : (x >>> 60) &&& 1 == 1 <;>
    simp only [a, i, c, e, ↓reduceIte, Bool.false_eq_true] <;>
    simp only [beq_iff_eq] at a i c e <;>
    bv_decide

/-! ## The literal spec readings -/

/-- Flag readings of "the last bit", "the second last bit", "the third last bit"
of the last (most-significant, little-endian) byte. -/
structure Flags where
  isArray : Bool
  isInlined : Bool
  isCompressed : Bool
  deriving DecidableEq, Repr

/-- MSB-first reading (= OpenUSD): bits 63, 62, 61. -/
def flagsMSB (x : UInt64) : Flags :=
  ⟨(x >>> 63) &&& 1 == 1, (x >>> 62) &&& 1 == 1, (x >>> 61) &&& 1 == 1⟩
/-- LSB-first reading of the flag byte: bits 56, 57, 58. -/
def flagsLSB (x : UInt64) : Flags :=
  ⟨(x >>> 56) &&& 1 == 1, (x >>> 57) &&& 1 == 1, (x >>> 58) &&& 1 == 1⟩

theorem flagsMSB_eq_unpack (x : UInt64) :
    flagsMSB x = ⟨(unpack x).isArray, (unpack x).isInlined, (unpack x).isCompressed⟩ := rfl

/-- SPEC-ISSUE: LEAN-412 — pxr's inlined `token typeName` rep (observed in a 26.11 file)
is inlined under the MSB reading and not inlined under the LSB reading. -/
theorem readings_differ_on_pxr_token :
    flagsMSB 0x400b00000000000f = ⟨false, true, false⟩ ∧
    flagsLSB 0x400b00000000000f = ⟨false, false, false⟩ := by decide

/-- An LSB-reading writer's inlined token is read by OpenUSD as an out-of-line value
at offset 0xf with a set reserved bit 57. -/
theorem lsb_writer_misread :
    unpack 0x020b00000000000f = ⟨0xf, 0x0b, false, false, false, false⟩ := by decide

/-! ## Inlined values. SPEC: L12079-12201; OpenUSD `crateValueInliners.h`.

Only the low 4 payload bytes are used (L12084). Layouts, all little-endian within
the u32 `ival` that is `memcpy`'d into the payload: -/

/-- Type ids used below. SPEC: Value Types table; OpenUSD `crateDataTypes.h`. -/
def tyInt : UInt8 := 3
def tyHalf : UInt8 := 7
def tyFloat : UInt8 := 8
def tyDouble : UInt8 := 9
def tyToken : UInt8 := 11
def tyMatrix3d : UInt8 := 14
def tyVec3f : UInt8 := 24
def tyVec2h : UInt8 := 21

def inlined (ty : UInt8) (ival : UInt32) : UInt64 :=
  pack { payload := ival.toUInt64, ty := ty, isArray := false, isInlined := true,
         isCompressed := false }

/-- The 4-byte inline word of an inlined rep. -/
def ival (x : UInt64) : UInt32 := x.toUInt32

theorem ival_inlined (ty : UInt8) (v : UInt32) : ival (inlined ty v) = v := by
  simp only [ival, inlined, pack, bit, payloadMask, ↓reduceIte]; bv_decide

theorem unpack_inlined (ty : UInt8) (v : UInt32) :
    unpack (inlined ty v) = ⟨v.toUInt64, ty, false, true, false, false⟩ := by
  apply unpack_pack; simp only; bv_decide

/-- `int` (and every integral type in int32 range): the int32 bit pattern. -/
def encInt32 (i : Int32) : UInt32 := i.toUInt32
def decInt32 (v : UInt32) : Int32 := v.toInt32
theorem decInt32_encInt32 (i : Int32) : decInt32 (encInt32 (i)) = i := by
  simp [decInt32, encInt32]

/-- `float`, and `double` exactly representable as float: the IEEE binary32 bits
(OpenUSD `_EncodeInline(FP)`: `memcpy(ival, &f, 4)` of `float f`). So an inlined
double's payload is a binary32, not "directly cast" (L12096-12098 table). -/
def encF32Bits (b : UInt32) : UInt32 := b

/-- `GfVec*` with every component exactly an int8 (all scalar types): the int8 bytes,
component 0 lowest. -/
def encVecI8 : List Int8 → UInt32
  | [] => 0
  | a :: as => a.toUInt8.toUInt32 ||| (encVecI8 as <<< 8)

def decVecI8 (dim : Nat) (v : UInt32) : List Int8 :=
  (([0, 8, 16, 24] : List UInt32).take dim).map fun k => (v >>> k).toUInt8.toInt8

theorem decVecI8_encVecI8_3 (a b c : Int8) : decVecI8 3 (encVecI8 [a, b, c]) = [a, b, c] := by
  simp only [decVecI8, encVecI8, List.take_succ_cons, List.take_zero,
    List.map_cons, List.map_nil, List.cons.injEq, and_true]
  refine ⟨?_, ?_, ?_⟩ <;> bv_decide

theorem decVecI8_encVecI8_4 (a b c d : Int8) :
    decVecI8 4 (encVecI8 [a, b, c, d]) = [a, b, c, d] := by
  simp only [decVecI8, encVecI8, List.take_succ_cons, List.take_zero,
    List.map_cons, List.map_nil, List.cons.injEq, and_true]
  refine ⟨?_, ?_, ?_, ?_⟩ <;> bv_decide

/-- `GfVec2h`: fits in 4 bytes, so the two binary16 bit patterns, component 0 low
(L12091-12094; observed `half2 (0.5, 1.5)` → payload `0x3e003800`). -/
def encVec2h (h0 h1 : UInt16) : UInt32 := h0.toUInt32 ||| (h1.toUInt32 <<< 16)
def decVec2h (v : UInt32) : UInt16 × UInt16 := (v.toUInt16, (v >>> 16).toUInt16)
theorem decVec2h_encVec2h (h0 h1 : UInt16) : decVec2h (encVec2h h0 h1) = (h0, h1) := by
  simp only [decVec2h, encVec2h, Prod.mk.injEq]; constructor <;> bv_decide

/-- Square matrices with zero off-diagonal and int8 diagonal: the diagonal as int8
bytes (L12099-12101); same layout as `encVecI8`. -/
def encMatDiag : List Int8 → UInt32 := encVecI8

/-! ### Observed pxr 26.11 reps (scratch `v.usda` → `usdcat` → FIELDS section) -/

theorem pxr_half2 : inlined tyVec2h (encVec2h 0x3800 0x3e00) = 0x401500003e003800 := by decide
theorem pxr_half2_int : inlined tyVec2h (encVec2h 0x3c00 0xc000) = 0x40150000c0003c00 := by
  decide
/-- `double c = 0.5` is inlined as binary32 0.5 = 0x3f000000. -/
theorem pxr_double_half : inlined tyDouble (encF32Bits 0x3f000000) = 0x400900003f000000 := by
  decide
theorem pxr_int_neg7 : inlined tyInt (encInt32 (-7)) = 0x40030000fffffff9 := by decide
theorem pxr_float : inlined tyFloat (encF32Bits 0x40200000) = 0x4008000040200000 := by decide
theorem pxr_matrix3d : inlined tyMatrix3d (encMatDiag [2, 3, -1]) = 0x400e000000ff0302 := by
  decide
theorem pxr_half : inlined tyHalf 0x3800 = 0x4007000000003800 := by decide
/-- `double d = 0.1` is not float-exact: out-of-line at file offset 0x8c. -/
theorem pxr_double_outofline :
    unpack 0x000900000000008c = ⟨0x8c, tyDouble, false, false, false, false⟩ := by decide

end UsdSpec.Crate.ValueRep
