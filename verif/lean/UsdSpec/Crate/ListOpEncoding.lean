import UsdSpec.Crate.IntegerCoding
import UsdSpec.ListOp
import UsdSpec.ListOp.Legacy
/-!
# Crate list-op encoding (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L12558-12636 (List Operations).

Header bits (L12581-12592): Make Explicit `0x1`, Add Explicit Items `0x2`, Add Items
`0x4`, Delete Items `0x8`, Reorder Items `0x10`, Prepend Items `0x20`, Append Items
`0x40` — identical to OpenUSD `crateFile.cpp` `_ListOpHeader::_Bits` (L481-488).
Wire order of the item vectors (L12594-12602): explicit, added, prepended, appended,
deleted, ordered — identical to OpenUSD `_Writer::Write(SdfListOp<T>)` (L1581-1587)
and the reader (L1147-1156), and to LightUSD `kInt64ListOpBits = {2,4,32,64,8,16}`
(`src/next/layer/int64-listop.hh` L19) and `crate-reader-decode.cc` L315-330.
So the spec's order is right (no issue for the order).

Each vector: `u64 count` then `count` uncompressed elements (L12604-12612). Elements
are modelled as `UInt64` words (Int64/UInt64 list ops; token/string/path list ops
carry u32 indices — width is a parameter of the real format, not of the framing).

What the spec leaves open (SPEC-ISSUE: LEAN-414): the meaning of header byte
combinations a canonical writer never emits. OpenUSD's reader applies the
`SdfListOp` setters in wire order, and every setter switches the op's mode and
clears all lists when the mode changes (`listOp.cpp` `_SetExplicit` L378-389,
`SetExplicitItems` L303-308 sets explicit, `SetAddedItems` etc. set non-explicit).
So `0x02` alone reads as an explicit op, and `0x22` (explicit items + prepended)
reads as a composable op whose explicit items are silently dropped. LightUSD's
`Int64ListOp::is_explicit()` is `(bits & 3) != 0`. `pxrRead` models the OpenUSD
reader.
-/

namespace UsdSpec.Crate.ListOpEncoding
open UsdSpec.Crate

/-- The six wire vectors plus the explicit flag. -/
structure CrateListOp where
  isExplicit : Bool
  explicit   : List UInt64
  added      : List UInt64
  prepended  : List UInt64
  appended   : List UInt64
  deleted    : List UInt64
  ordered    : List UInt64
  deriving DecidableEq, Repr, Inhabited

def bIsExplicit : UInt8 := 0x01
def bExplicit : UInt8 := 0x02
def bAdded : UInt8 := 0x04
def bDeleted : UInt8 := 0x08
def bOrdered : UInt8 := 0x10
def bPrepended : UInt8 := 0x20
def bAppended : UInt8 := 0x40

/-- Wire order. Matches LightUSD `kInt64ListOpBits`. -/
def wireBits : List UInt8 := [bExplicit, bAdded, bPrepended, bAppended, bDeleted, bOrdered]
theorem wireBits_eq_lightusd : wireBits.map UInt8.toNat = [2, 4, 32, 64, 8, 16] := by decide

def flag (b : Bool) (m : UInt8) : UInt8 := if b then m else 0

/-- OpenUSD `_ListOpHeader(op)` (L493-501). -/
def header (o : CrateListOp) : UInt8 :=
  flag o.isExplicit bIsExplicit ||| flag (!o.explicit.isEmpty) bExplicit |||
  flag (!o.added.isEmpty) bAdded ||| flag (!o.prepended.isEmpty) bPrepended |||
  flag (!o.appended.isEmpty) bAppended ||| flag (!o.deleted.isEmpty) bDeleted |||
  flag (!o.ordered.isEmpty) bOrdered

def has (h m : UInt8) : Bool := h &&& m != 0

def encodeVec (xs : List UInt64) : List UInt8 :=
  le64 (UInt64.ofNat xs.length) ++ xs.flatMap le64

def readN : Nat → List UInt8 → Option (List UInt64 × List UInt8)
  | 0, r => some ([], r)
  | k + 1, r => do
    let (x, r) ← rd64 r
    let (xs, r) ← readN k r
    some (x :: xs, r)

def decodeVec (bs : List UInt8) : Option (List UInt64 × List UInt8) := do
  let (n, r) ← rd64 bs
  readN n.toNat r

/-- Encode one optional vector: present iff non-empty (OpenUSD writes a vector only
when its header bit is set). -/
def encOpt (xs : List UInt64) : List UInt8 := if xs.isEmpty then [] else encodeVec xs

def decOpt (present : Bool) (bs : List UInt8) : Option (List UInt64 × List UInt8) :=
  if present then decodeVec bs else some ([], bs)

def encode (o : CrateListOp) : List UInt8 :=
  header o :: (encOpt o.explicit ++ encOpt o.added ++ encOpt o.prepended ++
    encOpt o.appended ++ encOpt o.deleted ++ encOpt o.ordered)

def decode : List UInt8 → Option (CrateListOp × List UInt8)
  | [] => none
  | h :: r => do
    let (e, r) ← decOpt (has h bExplicit) r
    let (a, r) ← decOpt (has h bAdded) r
    let (p, r) ← decOpt (has h bPrepended) r
    let (ap, r) ← decOpt (has h bAppended) r
    let (d, r) ← decOpt (has h bDeleted) r
    let (o, r) ← decOpt (has h bOrdered) r
    some (⟨has h bIsExplicit, e, a, p, ap, d, o⟩, r)

theorem readN_flatMap (xs : List UInt64) (r : List UInt8) :
    readN xs.length (xs.flatMap le64 ++ r) = some (xs, r) := by
  induction xs with
  | nil => rfl
  | cons x xs ih =>
    simp only [List.length_cons, List.flatMap_cons, List.append_assoc, readN]
    rw [rd64_le64]; simp only [Option.bind_eq_bind, Option.bind_some]; rw [ih]; rfl

/-- Lists shorter than 2^64 (always true of real data). -/
def Small (xs : List UInt64) : Prop := xs.length < 2 ^ 64

theorem decodeVec_encodeVec (xs : List UInt64) (r : List UInt8) (h : Small xs) :
    decodeVec (encodeVec xs ++ r) = some (xs, r) := by
  simp only [decodeVec, encodeVec, List.append_assoc]
  rw [rd64_le64]; simp only [Option.bind_eq_bind, Option.bind_some]
  have : (UInt64.ofNat xs.length).toNat = xs.length := by simp [Small] at h ⊢; omega
  rw [this, readN_flatMap]

def hdr (x e a p ap d o : Bool) : UInt8 :=
  flag x bIsExplicit ||| flag e bExplicit ||| flag a bAdded ||| flag p bPrepended |||
  flag ap bAppended ||| flag d bDeleted ||| flag o bOrdered

theorem has_hdr (x e a p ap d o : Bool) :
    has (hdr x e a p ap d o) bIsExplicit = x ∧ has (hdr x e a p ap d o) bExplicit = e ∧
    has (hdr x e a p ap d o) bAdded = a ∧ has (hdr x e a p ap d o) bPrepended = p ∧
    has (hdr x e a p ap d o) bAppended = ap ∧ has (hdr x e a p ap d o) bDeleted = d ∧
    has (hdr x e a p ap d o) bOrdered = o := by
  cases x <;> cases e <;> cases a <;> cases p <;> cases ap <;> cases d <;> cases o <;> decide

theorem has_header (o : CrateListOp) :
    has (header o) bIsExplicit = o.isExplicit ∧
    has (header o) bExplicit = !o.explicit.isEmpty ∧
    has (header o) bAdded = !o.added.isEmpty ∧
    has (header o) bPrepended = !o.prepended.isEmpty ∧
    has (header o) bAppended = !o.appended.isEmpty ∧
    has (header o) bDeleted = !o.deleted.isEmpty ∧
    has (header o) bOrdered = !o.ordered.isEmpty :=
  has_hdr _ _ _ _ _ _ _

theorem decOpt_encOpt (xs : List UInt64) (r : List UInt8) (h : Small xs) :
    decOpt (!xs.isEmpty) (encOpt xs ++ r) = some (xs, r) := by
  cases xs with
  | nil => rfl
  | cons x xs =>
    simp only [decOpt, encOpt, List.isEmpty_cons, Bool.not_false, ↓reduceIte,
      Bool.false_eq_true]
    exact decodeVec_encodeVec _ r h

def CrateListOp.Small (o : CrateListOp) : Prop :=
  ListOpEncoding.Small o.explicit ∧ ListOpEncoding.Small o.added ∧
  ListOpEncoding.Small o.prepended ∧ ListOpEncoding.Small o.appended ∧
  ListOpEncoding.Small o.deleted ∧ ListOpEncoding.Small o.ordered

/-- Roundtrip of the wire format, for every `CrateListOp`. -/
theorem decode_encode (o : CrateListOp) (r : List UInt8) (hs : o.Small) :
    decode (encode o ++ r) = some (o, r) := by
  obtain ⟨h1, h2, h3, h4, h5, h6⟩ := hs
  have hhd := has_header o
  obtain ⟨x1, x2, x3, x4, x5, x6, x7⟩ := hhd
  obtain ⟨x, e, a, p, ap, d, od⟩ := o
  simp only at h1 h2 h3 h4 h5 h6 x1 x2 x3 x4 x5 x6 x7
  simp only [encode, List.cons_append, List.append_assoc, decode]
  rw [x2, decOpt_encOpt e _ h1]; simp only [Option.bind_eq_bind, Option.bind_some]
  rw [x3, decOpt_encOpt a _ h2]; simp only [Option.bind_some]
  rw [x4, decOpt_encOpt p _ h3]; simp only [Option.bind_some]
  rw [x5, decOpt_encOpt ap _ h4]; simp only [Option.bind_some]
  rw [x6, decOpt_encOpt d _ h5]; simp only [Option.bind_some]
  rw [x7, decOpt_encOpt od _ h6]; simp only [Option.bind_some]
  rw [x1]

/-! ## Bridge to the spec list-op model `UsdSpec.ListOp` (L1271-1708) -/

def ofSpec : UsdSpec.ListOp UInt64 → CrateListOp
  | .explicit items => ⟨true, items, [], [], [], [], []⟩
  | .comp c => ⟨false, [], [], c.prepended, c.appended, c.deleted, []⟩

/-- Legacy `added` / `ordered` are dropped (no normative semantics, L12630-12634). -/
def toSpec (o : CrateListOp) : UsdSpec.ListOp UInt64 :=
  if o.isExplicit then .explicit o.explicit else .comp ⟨o.deleted, o.prepended, o.appended⟩

theorem toSpec_ofSpec (l : UsdSpec.ListOp UInt64) : toSpec (ofSpec l) = l := by
  cases l <;> rfl

def SpecSmall : UsdSpec.ListOp UInt64 → Prop
  | .explicit items => Small items
  | .comp c => Small c.deleted ∧ Small c.prepended ∧ Small c.appended

theorem ofSpec_small (l : UsdSpec.ListOp UInt64) (h : SpecSmall l) : (ofSpec l).Small := by
  have z : Small [] := by simp [Small]
  cases l with
  | explicit items => exact ⟨h, z, z, z, z, z⟩
  | comp c => obtain ⟨a, b, c⟩ := h; exact ⟨z, z, b, c, a, z⟩

/-- Spec list ops round-trip through the crate encoding. -/
theorem spec_roundtrip (l : UsdSpec.ListOp UInt64) (r : List UInt8) (h : SpecSmall l) :
    (decode (encode (ofSpec l) ++ r)).map (fun p => (toSpec p.1, p.2)) = some (l, r) := by
  rw [decode_encode _ r (ofSpec_small l h)]; simp [toSpec_ofSpec]

def ofLegacy (o : UsdSpec.ListOp.LegacyOps UInt64) : CrateListOp :=
  ⟨false, [], o.added, o.core.prepended, o.core.appended, o.core.deleted, o.ordered⟩

def toLegacy (o : CrateListOp) : UsdSpec.ListOp.LegacyOps UInt64 :=
  ⟨⟨o.deleted, o.prepended, o.appended⟩, o.added, o.ordered⟩

theorem legacy_roundtrip (o : UsdSpec.ListOp.LegacyOps UInt64) (r : List UInt8)
    (h : (ofLegacy o).Small) :
    (decode (encode (ofLegacy o) ++ r)).map (fun p => (toLegacy p.1, p.2)) = some (o, r) := by
  rw [decode_encode _ r h]; rfl

/-! ## OpenUSD reader semantics on arbitrary headers (SPEC-ISSUE: LEAN-414) -/

/-- OpenUSD `_Reader::Read<SdfListOp<T>>` (L1145-1157): `ClearAndMakeExplicit` if bit 1,
then each present vector via its setter; a setter whose mode differs from the
current one clears every list first. -/
def pxrRead (o : CrateListOp) (h : UInt8) : CrateListOp :=
  let empty (x : Bool) : CrateListOp := ⟨x, [], [], [], [], [], []⟩
  let setMode (s : CrateListOp) (x : Bool) := if s.isExplicit = x then s else empty x
  let s := if has h bIsExplicit then empty true else empty false
  let s := if has h bExplicit then { setMode s true with explicit := o.explicit } else s
  let s := if has h bAdded then { setMode s false with added := o.added } else s
  let s := if has h bPrepended then { setMode s false with prepended := o.prepended } else s
  let s := if has h bAppended then { setMode s false with appended := o.appended } else s
  let s := if has h bDeleted then { setMode s false with deleted := o.deleted } else s
  if has h bOrdered then { setMode s false with ordered := o.ordered } else s

/-- On canonical (writer-produced) ops the OpenUSD reader agrees with `decode`. -/
theorem pxrRead_canonical_explicit (items : List UInt64) (hne : items ≠ []) :
    pxrRead (ofSpec (.explicit items)) (header (ofSpec (.explicit items))) =
      ofSpec (.explicit items) := by
  cases items with
  | nil => exact absurd rfl hne
  | cons x xs =>
    simp [pxrRead, ofSpec, header, has, flag, bIsExplicit, bExplicit, bAdded, bDeleted,
      bOrdered, bPrepended, bAppended]
    have h1 : ((1 : UInt8) ||| 2) = 3 := by decide
    simp only [h1, show (3 : UInt8) &&& 16 = 0 by decide, show (3 : UInt8) &&& 8 = 0 by decide,
      show (3 : UInt8) &&& 64 = 0 by decide, show (3 : UInt8) &&& 32 = 0 by decide,
      show (3 : UInt8) &&& 4 = 0 by decide, show ¬ ((3 : UInt8) &&& 2 = 0) by decide,
      show ¬ ((3 : UInt8) &&& 1 = 0) by decide, ↓reduceIte]

/-- `0x02` (explicit items, no Make-Explicit bit) is read by OpenUSD as explicit. -/
theorem pxr_0x02_is_explicit :
    (pxrRead ⟨false, [7], [], [], [], [], []⟩ 0x02).isExplicit = true := by decide

/-- `0x22`: OpenUSD drops the explicit items when it meets the prepended vector;
the spec text gives no rule. -/
theorem pxr_0x22_drops_explicit :
    pxrRead ⟨false, [7], [], [9], [], [], []⟩ 0x22 = ⟨false, [], [], [9], [], [], []⟩ := by
  decide

/-- pxr `references = None` / `[]` (explicit, empty) is header `0x01` and nothing else. -/
theorem explicit_empty_bytes : encode (ofSpec (.explicit [])) = [0x01] := by decide

theorem example_prepend_bytes :
    encode (ofSpec (.comp ⟨[3], [1, 2], []⟩)) =
      [0x28] ++ le64 2 ++ le64 1 ++ le64 2 ++ le64 1 ++ le64 3 := by decide

end UsdSpec.Crate.ListOpEncoding
