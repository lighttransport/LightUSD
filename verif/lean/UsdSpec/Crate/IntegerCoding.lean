import Std.Tactic.BVDecide
import Mathlib.Data.List.Basic
import Mathlib.Order.Basic
/-!
# Crate compressed integer arrays (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L11600-11683 ("Compressed Integer Arrays").

This models the *integer-coding* stage only: delta pass (L11614-11623), common
value (L11630-11633), 2-bit codes (L11640-11649), code-byte packing + padding
(L11651-11659), then the values. The LZ4 stage that follows (L11670-11683) is
NOT modelled; `encode32`/`encode64` produce the byte stream that would be handed
to LZ4, and `decode32`/`decode64` consume what LZ4 would hand back.

Byte-layout facts not stated by the spec are pinned from the reference
implementations:

* OpenUSD `pxr/usd/sdf/integerCoding.cpp` `_EncodeIntegers` (L268-322) /
  `_DecodeIntegers` (L325-352): common value written with `_WriteBits` (L302;
  `memcpy` of the native little-endian value, same width as the element type);
  code-byte count `(numInts*2+7)/8 = ⌈n/4⌉` (L304, L332); element `i` of a group
  of four occupies bits `2i..2i+1` of its code byte (`codeByte |= code << (2*i)`,
  L211; decode `(codeByte >> (2*i)) & 3`, L249), i.e. the FIRST element is in the
  LOW bits and the padding is the HIGH bits of the last byte; delta
  `x[i] - x[i-1]` with `x[-1] = 0`, computed in the signed element type and thus
  wrapping (L282-285, L307); widths int8/int16/int32 for 32-bit elements and
  int16/int32/int64 for 64-bit elements (`_SmallTypes`, L169-176); most common
  delta with ties broken to the LARGEST delta (L290).
* LightUSD `src/next/c/crate-int.c` `lu_crate_decode_delta_u32` (L108-165) /
  `lu_crate_decode_delta_u64` (L230-290) agree on all of the above on the decode
  side (`(code_byte >> ((i % 4) * 2)) & 3`, L128/L252; wrapping prefix sum,
  L161/L284). LightUSD's encoder `src/next/c/crate-delta-encode.c`
  `lu_crate_encode_delta_u32` (code placement L310, header L286) breaks ties to
  the SMALLEST delta (L12, `dh_most_common` L159). The spec fixes no tie-break;
  both choices decode identically because the common value is in the header.
  (`EncodeIntegers` in `src/next/crate/crate-format.cc` L218 is the unrelated
  legacy common-prefix integer format, not this coding.)

Every definition is executable and structurally recursive, so the worked
example and the boundary cases are checked by `decide`.

Spec issues found while formalizing are tagged with `LEAN-4` numbers in the `SPEC-ISSUE` registry.
-/

namespace UsdSpec.Crate

/-! ## 2-bit codes. SPEC: L11640-11649. -/

/-- The four codepoints. SPEC: L11644-11649 (`00`, `01`, `10`, `11`). -/
inductive Code
  | common   -- 00: use the common value
  | quarter  -- 01: quarter-width signed integer
  | half     -- 10: half-width signed integer
  | full     -- 11: full-width signed integer
  deriving DecidableEq, Repr, BEq, Inhabited

namespace Code

def toBits : Code → UInt8
  | common => 0
  | quarter => 1
  | half => 2
  | full => 3

/-- Read a code from the low two bits of a byte (`(b >> 2i) & 3` in the C). -/
def ofBits (b : UInt8) : Code :=
  if b &&& 3 = 0 then common
  else if b &&& 3 = 1 then quarter
  else if b &&& 3 = 2 then half
  else full

/-- Pack four codes into one byte, element `i` in bits `2i..2i+1`
(OpenUSD `_EncodeNHelper`: `codeByte |= (code << (2 * i))`). -/
def packByte (a b c d : Code) : UInt8 :=
  a.toBits ||| (b.toBits <<< 2) ||| (c.toBits <<< 4) ||| (d.toBits <<< 6)

theorem ofBits_packByte_0 (a b c d : Code) : ofBits (packByte a b c d) = a := by
  cases a <;> cases b <;> cases c <;> cases d <;> decide
theorem ofBits_packByte_1 (a b c d : Code) : ofBits (packByte a b c d >>> 2) = b := by
  cases a <;> cases b <;> cases c <;> cases d <;> decide
theorem ofBits_packByte_2 (a b c d : Code) : ofBits (packByte a b c d >>> 4) = c := by
  cases a <;> cases b <;> cases c <;> cases d <;> decide
theorem ofBits_packByte_3 (a b c d : Code) : ofBits (packByte a b c d >>> 6) = d := by
  cases a <;> cases b <;> cases c <;> cases d <;> decide

end Code

/-- Pack a code list into `⌈n/4⌉` bytes; a trailing partial byte is padded with
zero bits (= `common` codes) in its high bits. SPEC: L11651-11659 ("Padding …
Zeroed bits"). -/
def packCodes : List Code → List UInt8
  | a :: b :: c :: d :: rest => Code.packByte a b c d :: packCodes rest
  | [a, b, c] => [Code.packByte a b c .common]
  | [a, b] => [Code.packByte a b .common .common]
  | [a] => [Code.packByte a .common .common .common]
  | [] => []

/-- Unpack exactly `n` codes, consuming `⌈n/4⌉` bytes; returns the remaining bytes. -/
def unpackCodes : Nat → List UInt8 → Option (List Code × List UInt8)
  | 0, bs => some ([], bs)
  | 1, b :: bs => some ([Code.ofBits b], bs)
  | 2, b :: bs => some ([Code.ofBits b, Code.ofBits (b >>> 2)], bs)
  | 3, b :: bs => some ([Code.ofBits b, Code.ofBits (b >>> 2), Code.ofBits (b >>> 4)], bs)
  | n + 4, b :: bs =>
    match unpackCodes n bs with
    | none => none
    | some (ks, rest) =>
      some (Code.ofBits b :: Code.ofBits (b >>> 2) :: Code.ofBits (b >>> 4)
              :: Code.ofBits (b >>> 6) :: ks, rest)
  | _ + 1, [] => none

/-- Code bytes: `⌈n/4⌉ = (n+3)/4` (OpenUSD: `(numInts * 2 + 7) / 8`). -/
theorem packCodes_length (ks : List Code) : (packCodes ks).length = (ks.length + 3) / 4 := by
  induction ks using packCodes.induct with
  | case1 a b c d rest ih => simp [packCodes, ih]; omega
  | case2 a b c => simp [packCodes]
  | case3 a b => simp [packCodes]
  | case4 a => simp [packCodes]
  | case5 => simp [packCodes]

theorem unpackCodes_packCodes (ks : List Code) (rest : List UInt8) :
    unpackCodes ks.length (packCodes ks ++ rest) = some (ks, rest) := by
  induction ks using packCodes.induct with
  | case1 a b c d rest' ih =>
    simp [packCodes, unpackCodes, ih, Code.ofBits_packByte_0, Code.ofBits_packByte_1,
      Code.ofBits_packByte_2, Code.ofBits_packByte_3]
  | case2 a b c =>
    simp [packCodes, unpackCodes, Code.ofBits_packByte_0, Code.ofBits_packByte_1,
      Code.ofBits_packByte_2]
  | case3 a b =>
    simp [packCodes, unpackCodes, Code.ofBits_packByte_0, Code.ofBits_packByte_1]
  | case4 a => simp [packCodes, unpackCodes, Code.ofBits_packByte_0]
  | case5 => simp [packCodes, unpackCodes]

/-! ## Little-endian fixed-width values (OpenUSD `_WriteBits` / `_ReadBits`, i.e.
`memcpy` of the native little-endian representation). -/

def le8 (u : UInt8) : List UInt8 := [u]
def le16 (u : UInt16) : List UInt8 := [u.toUInt8, (u >>> 8).toUInt8]
def le32 (u : UInt32) : List UInt8 :=
  [u.toUInt8, (u >>> 8).toUInt8, (u >>> 16).toUInt8, (u >>> 24).toUInt8]
def le64 (u : UInt64) : List UInt8 :=
  [u.toUInt8, (u >>> 8).toUInt8, (u >>> 16).toUInt8, (u >>> 24).toUInt8,
   (u >>> 32).toUInt8, (u >>> 40).toUInt8, (u >>> 48).toUInt8, (u >>> 56).toUInt8]

def rd8 : List UInt8 → Option (UInt8 × List UInt8)
  | b :: r => some (b, r)
  | [] => none
def rd16 : List UInt8 → Option (UInt16 × List UInt8)
  | b0 :: b1 :: r => some (b0.toUInt16 ||| (b1.toUInt16 <<< 8), r)
  | _ => none
def rd32 : List UInt8 → Option (UInt32 × List UInt8)
  | b0 :: b1 :: b2 :: b3 :: r =>
    some (b0.toUInt32 ||| (b1.toUInt32 <<< 8) ||| (b2.toUInt32 <<< 16) ||| (b3.toUInt32 <<< 24), r)
  | _ => none
def rd64 : List UInt8 → Option (UInt64 × List UInt8)
  | b0 :: b1 :: b2 :: b3 :: b4 :: b5 :: b6 :: b7 :: r =>
    some (b0.toUInt64 ||| (b1.toUInt64 <<< 8) ||| (b2.toUInt64 <<< 16) ||| (b3.toUInt64 <<< 24)
          ||| (b4.toUInt64 <<< 32) ||| (b5.toUInt64 <<< 40) ||| (b6.toUInt64 <<< 48)
          ||| (b7.toUInt64 <<< 56), r)
  | _ => none

theorem rd8_le8 (u : UInt8) (r : List UInt8) : rd8 (le8 u ++ r) = some (u, r) := rfl
theorem rd16_le16 (u : UInt16) (r : List UInt8) : rd16 (le16 u ++ r) = some (u, r) := by
  simp only [le16, rd16, List.cons_append, List.nil_append, Option.some.injEq, Prod.mk.injEq,
    and_true]
  bv_decide
theorem rd32_le32 (u : UInt32) (r : List UInt8) : rd32 (le32 u ++ r) = some (u, r) := by
  simp only [le32, rd32, List.cons_append, List.nil_append, Option.some.injEq, Prod.mk.injEq,
    and_true]
  bv_decide
theorem rd64_le64 (u : UInt64) (r : List UInt8) : rd64 (le64 u ++ r) = some (u, r) := by
  simp only [le64, rd64, List.cons_append, List.nil_append, Option.some.injEq, Prod.mk.injEq,
    and_true]
  bv_decide

/-! ## Width-generic layout

The spec's table (L11644-11649) is the only thing that differs between the
32-bit and 64-bit variants, so the common value, per-delta coding, and the
roundtrip obligations are bundled in a `Layout` and the sequence-level theorems
are proved once. -/

/-- Everything width-specific, together with the two facts the roundtrip needs. -/
structure Layout (α : Type) [Add α] [Sub α] where
  /-- Common value: "Signed Integer of same size" (L11655), little-endian. -/
  commonBytes : α → List UInt8
  readCommon : List UInt8 → Option (α × List UInt8)
  /-- `encodeDelta common d` : the code for `d` and its encoded bytes (L11640-11649). -/
  encodeDelta : α → α → Code × List UInt8
  decodeDelta : α → Code → List UInt8 → Option (α × List UInt8)
  /-- Byte width of each code (0 for `common`). -/
  width : Code → Nat
  commonWidth : Nat
  readCommon_commonBytes : ∀ c rest, readCommon (commonBytes c ++ rest) = some (c, rest)
  decodeDelta_encodeDelta : ∀ c d rest,
    decodeDelta c (encodeDelta c d).1 ((encodeDelta c d).2 ++ rest) = some (d, rest)
  encodeDelta_length : ∀ c d, (encodeDelta c d).2.length = width (encodeDelta c d).1
  commonBytes_length : ∀ c, (commonBytes c).length = commonWidth
  add_sub_cancel : ∀ p x : α, p + (x - p) = x

variable {α : Type} [Add α] [Sub α]

/-! ### Delta pass. SPEC: L11614-11623; "If no previous value exists, it starts at 0" (L11642). -/

/-- `x[i] - x[i-1]`, with `prev` standing for `x[-1]`. -/
def toDeltasFrom (prev : α) : List α → List α
  | [] => []
  | x :: xs => (x - prev) :: toDeltasFrom x xs

/-- Prefix sums (the decoder's `prevVal += delta`). -/
def fromDeltasFrom (prev : α) : List α → List α
  | [] => []
  | d :: ds => (prev + d) :: fromDeltasFrom (prev + d) ds

omit [Add α] in
theorem toDeltasFrom_length (p : α) (xs : List α) : (toDeltasFrom p xs).length = xs.length := by
  induction xs generalizing p with
  | nil => rfl
  | cons x xs ih => simp [toDeltasFrom, ih]

theorem fromDeltasFrom_toDeltasFrom (L : Layout α) (p : α) (xs : List α) :
    fromDeltasFrom p (toDeltasFrom p xs) = xs := by
  induction xs generalizing p with
  | nil => rfl
  | cons x xs ih => simp [toDeltasFrom, fromDeltasFrom, L.add_sub_cancel, ih]

/-! ### Most common delta. SPEC: L11630-11631.

OpenUSD `_EncodeIntegers` scans once, replacing the candidate when a count
becomes strictly larger, or equal with a larger value; the result is the
lexicographic maximum of `(count, value)`. The spec fixes no tie-break
(LightUSD picks the smallest instead); the choice does not affect decoding. -/

def mostCommon [DecidableEq α] [LT α] [DecidableLT α] (ds : List α) (zero : α) : α :=
  ds.foldl (fun best d =>
    if ds.count d > ds.count best ∨ (ds.count d = ds.count best ∧ best < d) then d else best) zero

/-- A candidate dominates another by frequency, then by value. -/
def CommonBest [DecidableEq α] [LE α] (ds : List α) (best x : α) : Prop :=
  ds.count x ≤ ds.count best ∧ (ds.count x = ds.count best → x ≤ best)

omit [Add α] [Sub α] in
private theorem commonBest_trans [LinearOrder α] [DecidableEq α] (ds : List α) {a b c : α}
    (hab : CommonBest ds a b) (hbc : CommonBest ds b c) : CommonBest ds a c := by
  obtain ⟨h1, h2⟩ := hab
  obtain ⟨h3, h4⟩ := hbc
  exact ⟨by omega, fun h => le_trans (h4 (by omega)) (h2 (by omega))⟩

omit [Add α] [Sub α] in
private theorem mostCommon_step [LinearOrder α] [DecidableEq α] (ds : List α) (b d : α) :
    let r := if ds.count d > ds.count b ∨ (ds.count d = ds.count b ∧ b < d) then d else b
    CommonBest ds r b ∧ CommonBest ds r d := by
  dsimp
  split
  · rename_i h
    constructor
    · constructor
      · omega
      · intro he
        rcases h with h | ⟨_, h⟩
        · omega
        · exact le_of_lt h
    · exact ⟨Nat.le_refl _, fun _ => le_refl _⟩
  · rename_i h
    constructor
    · exact ⟨Nat.le_refl _, fun _ => le_refl _⟩
    · constructor
      · omega
      · intro he
        exact le_of_not_gt (fun hb => h (Or.inr ⟨he, hb⟩))

omit [Add α] [Sub α] in
private theorem mostCommon_fold [LinearOrder α] [DecidableEq α] (ds xs : List α) (b : α) :
    let r := xs.foldl (fun best d =>
      if ds.count d > ds.count best ∨ (ds.count d = ds.count best ∧ best < d) then d else best) b
    CommonBest ds r b ∧ (∀ x ∈ xs, CommonBest ds r x) ∧ (r = b ∨ r ∈ xs) := by
  induction xs generalizing b with
  | nil => simp [CommonBest]
  | cons d xs ih =>
    dsimp only [List.foldl_cons]
    let next := if ds.count d > ds.count b ∨ (ds.count d = ds.count b ∧ b < d) then d else b
    obtain ⟨hb, hd⟩ := mostCommon_step ds b d
    obtain ⟨hr, hx, hm⟩ := ih next
    refine ⟨commonBest_trans ds hr hb, ?_, ?_⟩
    · intro x h
      rcases List.mem_cons.mp h with rfl | h
      · exact commonBest_trans ds hr hd
      · exact hx x h
    · rcases hm with h | h
      · rw [h]
        dsimp [next]
        split
        · exact Or.inr (by simp)
        · exact Or.inl rfl
      · exact Or.inr (List.mem_cons_of_mem _ h)

omit [Add α] [Sub α] in
/-- The selected delta has maximal frequency; ties choose the largest value.
The empty-input result is the supplied zero. No frequency claim relies on examples. -/
theorem mostCommon_spec [LinearOrder α] [DecidableEq α] (ds : List α) (zero : α) :
    (∀ x ∈ ds, CommonBest ds (mostCommon ds zero) x) ∧
    (mostCommon ds zero = zero ∨ mostCommon ds zero ∈ ds) := by
  obtain ⟨_, hx, hm⟩ := mostCommon_fold ds ds zero
  exact ⟨hx, hm⟩

omit [Add α] [Sub α] in
theorem mostCommon_nil [DecidableEq α] [LT α] [DecidableLT α] (zero : α) :
    mostCommon [] zero = zero := rfl

omit [Add α] [Sub α] in
/-- A nonempty input always selects one of its actual deltas. -/
theorem mostCommon_mem [LinearOrder α] [DecidableEq α] (ds : List α) (zero : α)
    (hne : ds ≠ []) : mostCommon ds zero ∈ ds := by
  obtain ⟨hx, hm⟩ := mostCommon_spec ds zero
  rcases hm with h | h
  · by_contra hz
    have hz0 : ds.count zero = 0 := List.count_eq_zero.mpr (by simpa [h] using hz)
    obtain ⟨x, xs, rfl⟩ := List.exists_cons_of_ne_nil hne
    have hh := (hx x (by simp)).1
    rw [h, hz0] at hh
    simp at hh
  · exact h

/-! ### Coding a delta sequence -/

/-- All codes, and all value bytes concatenated in element order (L11657-11658). -/
def encodeDeltas (L : Layout α) (c : α) : List α → List Code × List UInt8
  | [] => ([], [])
  | d :: ds =>
    let e := L.encodeDelta c d
    let r := encodeDeltas L c ds
    (e.1 :: r.1, e.2 ++ r.2)

def decodeDeltas (L : Layout α) (c : α) : List Code → List UInt8 → Option (List α × List UInt8)
  | [], bs => some ([], bs)
  | k :: ks, bs =>
    match L.decodeDelta c k bs with
    | none => none
    | some (d, r) =>
      match decodeDeltas L c ks r with
      | none => none
      | some (ds, r') => some (d :: ds, r')

theorem encodeDeltas_codes_length (L : Layout α) (c : α) (ds : List α) :
    (encodeDeltas L c ds).1.length = ds.length := by
  induction ds with
  | nil => rfl
  | cons d ds ih => simp [encodeDeltas, ih]

/-- The value bytes are exactly the sum of the per-code widths. -/
theorem encodeDeltas_values_length (L : Layout α) (c : α) (ds : List α) :
    (encodeDeltas L c ds).2.length = ((encodeDeltas L c ds).1.map L.width).sum := by
  induction ds with
  | nil => rfl
  | cons d ds ih => simp [encodeDeltas, ih, L.encodeDelta_length]

theorem decodeDeltas_encodeDeltas (L : Layout α) (c : α) (ds : List α) (rest : List UInt8) :
    decodeDeltas L c (encodeDeltas L c ds).1 ((encodeDeltas L c ds).2 ++ rest)
      = some (ds, rest) := by
  induction ds with
  | nil => rfl
  | cons d ds ih =>
    simp only [encodeDeltas, List.append_assoc, decodeDeltas, L.decodeDelta_encodeDelta, ih]

/-! ### The byte stream (before LZ4). SPEC: L11651-11659 layout table. -/

/-- `[common][codes ⌈n/4⌉ bytes, zero-padded][values]`; the empty array encodes to
nothing (OpenUSD: `if (numInts == 0) return 0;`). -/
def encode [DecidableEq α] [LT α] [DecidableLT α] (L : Layout α) (zero : α) (xs : List α) :
    List UInt8 :=
  match xs with
  | [] => []
  | _ :: _ =>
    let ds := toDeltasFrom zero xs
    let c := mostCommon ds zero
    let e := encodeDeltas L c ds
    L.commonBytes c ++ packCodes e.1 ++ e.2

/-- Decode `n` integers. Trailing bytes are ignored, as in both reference decoders. -/
def decode (L : Layout α) (zero : α) (n : Nat) (bs : List UInt8) : Option (List α) :=
  match n with
  | 0 => some []
  | _ + 1 =>
    match L.readCommon bs with
    | none => none
    | some (c, r1) =>
      match unpackCodes n r1 with
      | none => none
      | some (ks, r2) =>
        match decodeDeltas L c ks r2 with
        | none => none
        | some (ds, _) => some (fromDeltasFrom zero ds)

/-- Generic roundtrip. -/
theorem decode_encode [DecidableEq α] [LT α] [DecidableLT α] (L : Layout α) (zero : α)
    (xs : List α) : decode L zero xs.length (encode L zero xs) = some xs := by
  cases xs with
  | nil => rfl
  | cons x xs =>
    simp only [encode, List.length_cons]
    have hn : xs.length + 1 = (encodeDeltas L (mostCommon (toDeltasFrom zero (x :: xs)) zero)
        (toDeltasFrom zero (x :: xs))).1.length := by
      rw [encodeDeltas_codes_length, toDeltasFrom_length, List.length_cons]
    simp only [decode, List.append_assoc, L.readCommon_commonBytes]
    rw [hn, unpackCodes_packCodes]
    have h := decodeDeltas_encodeDeltas L (mostCommon (toDeltasFrom zero (x :: xs)) zero)
      (toDeltasFrom zero (x :: xs)) []
    rw [List.append_nil] at h
    simp only [h, fromDeltasFrom_toDeltasFrom L]

/-- Generic length formula: common + `⌈n/4⌉` code bytes + Σ widths. -/
theorem encode_length [DecidableEq α] [LT α] [DecidableLT α] (L : Layout α) (zero : α)
    (xs : List α) (hne : xs ≠ []) :
    let ds := toDeltasFrom zero xs
    let e := encodeDeltas L (mostCommon ds zero) ds
    (encode L zero xs).length = L.commonWidth + (xs.length + 3) / 4 + (e.1.map L.width).sum := by
  cases xs with
  | nil => exact absurd rfl hne
  | cons x xs =>
    simp only [encode, List.length_append, L.commonBytes_length, packCodes_length,
      encodeDeltas_codes_length, toDeltasFrom_length, encodeDeltas_values_length]

/-! ## 32-bit elements. SPEC: L11644-11649 (Int8 / Int16 / full) with 32-bit common value. -/

/-- Narrowest code that represents `d` exactly (OpenUSD `getCode`: common first,
then `[-128,127]`, then `[-32768,32767]`, else full). -/
def encodeDelta32 (c d : Int32) : Code × List UInt8 :=
  if d = c then (.common, [])
  else if -128 ≤ d ∧ d ≤ 127 then (.quarter, le8 d.toInt8.toUInt8)
  else if -32768 ≤ d ∧ d ≤ 32767 then (.half, le16 d.toInt16.toUInt16)
  else (.full, le32 d.toUInt32)

def decodeDelta32 (c : Int32) : Code → List UInt8 → Option (Int32 × List UInt8)
  | .common, bs => some (c, bs)
  | .quarter, bs =>
    match rd8 bs with
    | none => none
    | some (u, r) => some (u.toInt8.toInt32, r)
  | .half, bs =>
    match rd16 bs with
    | none => none
    | some (u, r) => some (u.toInt16.toInt32, r)
  | .full, bs =>
    match rd32 bs with
    | none => none
    | some (u, r) => some (u.toInt32, r)

def Code.width32 : Code → Nat
  | common => 0
  | quarter => 1
  | half => 2
  | full => 4

theorem decodeDelta32_encodeDelta32 (c d : Int32) (rest : List UInt8) :
    decodeDelta32 c (encodeDelta32 c d).1 ((encodeDelta32 c d).2 ++ rest) = some (d, rest) := by
  unfold encodeDelta32
  split_ifs with h0 h1 h2
  · subst h0; rfl
  · simp only [decodeDelta32, rd8_le8, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  · simp only [decodeDelta32, rd16_le16, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  · simp only [decodeDelta32, rd32_le32, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide

theorem encodeDelta32_length (c d : Int32) :
    (encodeDelta32 c d).2.length = Code.width32 (encodeDelta32 c d).1 := by
  unfold encodeDelta32; split_ifs <;> rfl

def layout32 : Layout Int32 where
  commonBytes c := le32 c.toUInt32
  readCommon bs :=
    match rd32 bs with
    | none => none
    | some (u, r) => some (u.toInt32, r)
  encodeDelta := encodeDelta32
  decodeDelta := decodeDelta32
  width := Code.width32
  commonWidth := 4
  readCommon_commonBytes c rest := by
    simp only [rd32_le32, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  decodeDelta_encodeDelta := decodeDelta32_encodeDelta32
  encodeDelta_length := encodeDelta32_length
  commonBytes_length _ := rfl
  add_sub_cancel p x := by bv_decide

def encode32 : List Int32 → List UInt8 := encode layout32 0
def decode32 (n : Nat) : List UInt8 → Option (List Int32) := decode layout32 0 n

/-- (I1) 32-bit roundtrip. -/
theorem decode32_encode32 (xs : List Int32) : decode32 xs.length (encode32 xs) = some xs :=
  decode_encode layout32 0 xs

/-- (I2) 32-bit length: `4 + ⌈n/4⌉ + Σ widths` (widths 0/1/2/4 by code). -/
theorem encode32_length (xs : List Int32) (hne : xs ≠ []) :
    let ds := toDeltasFrom 0 xs
    let e := encodeDeltas layout32 (mostCommon ds 0) ds
    (encode32 xs).length = 4 + (xs.length + 3) / 4 + (e.1.map Code.width32).sum :=
  encode_length layout32 0 xs hne

/-! ## 64-bit elements. SPEC: L11644-11649 (Int16 / Int32 / full) with 64-bit common value. -/

def encodeDelta64 (c d : Int64) : Code × List UInt8 :=
  if d = c then (.common, [])
  else if -32768 ≤ d ∧ d ≤ 32767 then (.quarter, le16 d.toInt16.toUInt16)
  else if -2147483648 ≤ d ∧ d ≤ 2147483647 then (.half, le32 d.toInt32.toUInt32)
  else (.full, le64 d.toUInt64)

def decodeDelta64 (c : Int64) : Code → List UInt8 → Option (Int64 × List UInt8)
  | .common, bs => some (c, bs)
  | .quarter, bs =>
    match rd16 bs with
    | none => none
    | some (u, r) => some (u.toInt16.toInt64, r)
  | .half, bs =>
    match rd32 bs with
    | none => none
    | some (u, r) => some (u.toInt32.toInt64, r)
  | .full, bs =>
    match rd64 bs with
    | none => none
    | some (u, r) => some (u.toInt64, r)

def Code.width64 : Code → Nat
  | common => 0
  | quarter => 2
  | half => 4
  | full => 8

theorem decodeDelta64_encodeDelta64 (c d : Int64) (rest : List UInt8) :
    decodeDelta64 c (encodeDelta64 c d).1 ((encodeDelta64 c d).2 ++ rest) = some (d, rest) := by
  unfold encodeDelta64
  split_ifs with h0 h1 h2
  · subst h0; rfl
  · simp only [decodeDelta64, rd16_le16, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  · simp only [decodeDelta64, rd32_le32, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  · simp only [decodeDelta64, rd64_le64, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide

theorem encodeDelta64_length (c d : Int64) :
    (encodeDelta64 c d).2.length = Code.width64 (encodeDelta64 c d).1 := by
  unfold encodeDelta64; split_ifs <;> rfl

def layout64 : Layout Int64 where
  commonBytes c := le64 c.toUInt64
  readCommon bs :=
    match rd64 bs with
    | none => none
    | some (u, r) => some (u.toInt64, r)
  encodeDelta := encodeDelta64
  decodeDelta := decodeDelta64
  width := Code.width64
  commonWidth := 8
  readCommon_commonBytes c rest := by
    simp only [rd64_le64, Option.some.injEq, Prod.mk.injEq, and_true]
    bv_decide
  decodeDelta_encodeDelta := decodeDelta64_encodeDelta64
  encodeDelta_length := encodeDelta64_length
  commonBytes_length _ := rfl
  add_sub_cancel p x := by bv_decide

def encode64 : List Int64 → List UInt8 := encode layout64 0
def decode64 (n : Nat) : List UInt8 → Option (List Int64) := decode layout64 0 n

/-- (I1, 64-bit) roundtrip. -/
theorem decode64_encode64 (xs : List Int64) : decode64 xs.length (encode64 xs) = some xs :=
  decode_encode layout64 0 xs

/-- (I2, 64-bit) length: `8 + ⌈n/4⌉ + Σ widths` (widths 0/2/4/8 by code). -/
theorem encode64_length (xs : List Int64) (hne : xs ≠ []) :
    let ds := toDeltasFrom 0 xs
    let e := encodeDeltas layout64 (mostCommon ds 0) ds
    (encode64 xs).length = 8 + (xs.length + 3) / 4 + (e.1.map Code.width64).sum :=
  encode_length layout64 0 xs hne

/-! ## (I3) The spec's worked example. SPEC: L11618-11623, L11630-11631, L11663-11667.

Input (L11619) `[123, 124, 125, 100125, 100125, 100126, 100126]`, differences
(L11622) `[123, 1, 1, 100000, 0, 1, 0]`, common value `1` (L11631), and output
(L11667) `[int32(1) 01 00 00 11 01 00 01 XX int8(123) int32(100000) int8(0) int8(0)]`.
-/

/-- Delta pass of the example. SPEC: L11619-L11623. -/
example : toDeltasFrom (0 : Int32) [123, 124, 125, 100125, 100125, 100126, 100126]
    = [123, 1, 1, 100000, 0, 1, 0] := by decide

/-- Common value of the example is `1`. SPEC: L11630-11631. -/
example : mostCommon ([123, 1, 1, 100000, 0, 1, 0] : List Int32) 0 = 1 := by decide

/-- The codes of the example, in element order, are exactly the spec's
`01 00 00 11 01 00 01`. SPEC: L11667. -/
example : (encodeDeltas layout32 1 [123, 1, 1, 100000, 0, 1, 0]).1
    = [.quarter, .common, .common, .full, .quarter, .common, .quarter] := by decide

/-- The example's full byte stream, with the input ending in `100126` (as the
difference array L11622 and the output's trailing `int8(0)` require):
`int32(1)` = `01 00 00 00`; code bytes `C1 11` (= `0b11_00_00_01`, `0b00_01_00_01`:
element 0 in the low bits, `XX` padding = the zero high bits of the second byte);
then `int8(123)`, `int32(100000)` = `A0 86 01 00`, `int8(0)`, `int8(0)`. 13 bytes. -/
example : encode32 [123, 124, 125, 100125, 100125, 100126, 100126]
    = [1, 0, 0, 0, 0xC1, 0x11, 123, 0xA0, 0x86, 0x01, 0x00, 0, 0] := by decide

/-- SPEC-ISSUE: LEAN-401 — the `input` line of the output listing (L11665) ends in
`10026` whereas L11619 and the difference array L11622 use `100126`. With `10026`
the last difference is `10026 - 100126 = -90100`, which needs a full-width code,
so the listed output (`… 01 … int8(0)` as the last element) is NOT the encoding
of the listed input: the last code becomes `11` and the last value `int32(-90100)`
= `0C A0 FE FF`. -/
example : toDeltasFrom (0 : Int32) [123, 124, 125, 100125, 100125, 100126, 10026]
    = [123, 1, 1, 100000, 0, 1, -90100] := by decide
example : encode32 [123, 124, 125, 100125, 100125, 100126, 10026]
    = [1, 0, 0, 0, 0xC1, 0x31, 123, 0xA0, 0x86, 0x01, 0x00, 0, 0x0C, 0xA0, 0xFE, 0xFF] := by
  decide

/-- SPEC-ISSUE: LEAN-402 — L11640 says each difference is "converted to a code
point and a value relative to the previous value", which reads as a second delta
pass over the difference array. The reference implementations (OpenUSD
`_EncodeNHelper`, LightUSD `lu_crate_encode_delta_u32`) encode each difference
itself, and the spec's own output (`int32(100000)`, not `99999`) confirms it: a
second pass would give `[123, -122, 0, 99999, -100000, 1, -1]` and a different
stream. -/
example : toDeltasFrom (0 : Int32) [123, 1, 1, 100000, 0, 1, 0]
    = [123, -122, 0, 99999, -100000, 1, -1] := by decide
example : (encodeDeltas layout32 1 [123, 1, 1, 100000, 0, 1, 0]).2
    = [123, 0xA0, 0x86, 0x01, 0x00, 0, 0] := by decide

/-! ## (I4) Boundary cases -/

/-- Empty array: no bytes at all. -/
example : encode32 [] = [] := by decide
example : decode32 0 [] = some [] := by decide
example : encode64 [] = [] := by decide

/-- Single element `7`: delta `7` is the (only, hence most common) value, so the
one code is `00` and nothing follows the header: 5 bytes. -/
example : encode32 [7] = [7, 0, 0, 0, 0] := by decide
example : encode64 [7] = [7, 0, 0, 0, 0, 0, 0, 0, 0] := by decide

/-- All-equal array `[5,5,5,5]`: deltas `[5,0,0,0]`, common `0`; the first element
is `01 int8(5)`, the rest `00`. Only an array whose every delta is the common
value (e.g. all zeros, or an arithmetic progression) is all-`00`. -/
example : encode32 [5, 5, 5, 5] = [0, 0, 0, 0, 0b00_00_00_01, 5] := by decide
example : encode32 [0, 0, 0, 0, 0] = [0, 0, 0, 0, 0, 0] := by decide
example : encode32 [10, 20, 30, 40] = [10, 0, 0, 0, 0] := by decide

/-- int8 boundaries: deltas `127` and `-128` take `01`; `128` and `-129` take `10`. -/
example : encodeDelta32 0 127 = (.quarter, [0x7F]) := by decide
example : encodeDelta32 0 (-128) = (.quarter, [0x80]) := by decide
example : encodeDelta32 0 128 = (.half, [0x80, 0x00]) := by decide
example : encodeDelta32 0 (-129) = (.half, [0x7F, 0xFF]) := by decide

/-- int16 boundaries: `32767` / `-32768` take `10`; `32768` / `-32769` take `11`. -/
example : encodeDelta32 0 32767 = (.half, [0xFF, 0x7F]) := by decide
example : encodeDelta32 0 (-32768) = (.half, [0x00, 0x80]) := by decide
example : encodeDelta32 0 32768 = (.full, [0x00, 0x80, 0x00, 0x00]) := by decide
example : encodeDelta32 0 (-32769) = (.full, [0xFF, 0x7F, 0xFF, 0xFF]) := by decide

/-- Full width: `2^31-1` and `-2^31`. -/
example : encodeDelta32 0 2147483647 = (.full, [0xFF, 0xFF, 0xFF, 0x7F]) := by decide
example : encodeDelta32 0 (-2147483648) = (.full, [0x00, 0x00, 0x00, 0x80]) := by decide
example : encode32 [2147483647] = [0xFF, 0xFF, 0xFF, 0x7F, 0] := by decide
example : encode32 [-2147483648] = [0x00, 0x00, 0x00, 0x80, 0] := by decide

/-- Wrapping delta: `[2^31-1, -2^31]` has difference `-2^32+1 ≡ 1 (mod 2^32)`
(OpenUSD computes the delta in `int32`; LightUSD's decoders add with wrapping).
Both deltas occur once, so the tie-break picks the larger, `2^31-1`, as the
common value and the second element codes as `01 int8(1)`; the roundtrip holds
by `decode32_encode32`. -/
example : toDeltasFrom (0 : Int32) [2147483647, -2147483648] = [2147483647, 1] := by decide
example : encode32 [2147483647, -2147483648]
    = [0xFF, 0xFF, 0xFF, 0x7F, 0b00_00_01_00, 1] := by decide
example : decode32 2 [0xFF, 0xFF, 0xFF, 0x7F, 0b00_00_01_00, 1]
    = some [2147483647, -2147483648] := by decide

/-- A mixed array exercising every code in one byte: deltas `[1, 1000, 100000, 1]`. -/
example : encode32 [1, 1001, 101001, 101002]
    = [1, 0, 0, 0, 0b00_11_10_00, 0xE8, 0x03, 0xA0, 0x86, 0x01, 0x00] := by decide
example : decode32 4 [1, 0, 0, 0, 0b00_11_10_00, 0xE8, 0x03, 0xA0, 0x86, 0x01, 0x00]
    = some [1, 1001, 101001, 101002] := by decide

/-- Tie-break: deltas `[3, 3, 5, 5]` → OpenUSD picks the LARGER value `5`. -/
example : mostCommon ([3, 3, 5, 5] : List Int32) 0 = 5 := by decide
example : mostCommon ([5, 5, 3, 3] : List Int32) 0 = 5 := by decide

/-- 64-bit boundaries: int16 → `01`, int32 → `10`, beyond → `11`. -/
example : encodeDelta64 0 32767 = (.quarter, [0xFF, 0x7F]) := by decide
example : encodeDelta64 0 (-32768) = (.quarter, [0x00, 0x80]) := by decide
example : encodeDelta64 0 32768 = (.half, [0x00, 0x80, 0x00, 0x00]) := by decide
example : encodeDelta64 0 2147483647 = (.half, [0xFF, 0xFF, 0xFF, 0x7F]) := by decide
example : encodeDelta64 0 (-2147483648) = (.half, [0x00, 0x00, 0x00, 0x80]) := by decide
example : encodeDelta64 0 2147483648 = (.full, [0, 0, 0, 0x80, 0, 0, 0, 0]) := by decide
example : encodeDelta64 0 9223372036854775807
    = (.full, [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F]) := by decide
example : encodeDelta64 0 (-9223372036854775808)
    = (.full, [0, 0, 0, 0, 0, 0, 0, 0x80]) := by decide

/-- Malformed input is rejected rather than misread: too few code bytes or
missing value bytes. -/
example : decode32 1 [1, 0, 0] = none := by decide
example : decode32 1 [1, 0, 0, 0] = none := by decide
example : decode32 1 [1, 0, 0, 0, 0b01] = none := by decide
example : decode32 1 [1, 0, 0, 0, 0b11, 1, 2] = none := by decide

end UsdSpec.Crate
