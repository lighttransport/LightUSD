import UsdSpec.Crate.IntegerCoding
/-!
# Crate header, bootstrap, table of contents (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L11691-11788 (Header, Crate Versions, Bootstrap, Table of Contents).

Byte layout pinned from OpenUSD `pxr/usd/sdf/crateFile.h` `_BootStrap` (L479-486:
`uint8_t ident[8]; uint8_t version[8]; int64_t tocOffset; int64_t _reserved[8];`,
i.e. **88** bytes), `_Section` (L488-493: `char name[16]; int64_t start, size;`),
and `crateFile.cpp` `_ReadBootStrap` (L3534: files shorter than
`sizeof(_BootStrap)` are rejected). A pxr 26.11 `usdcat` output was inspected
byte-for-byte (scratch `t.usdc`, 791 bytes): version `00 08 00`, `tocOffset = 591`
at byte 16, bytes 24..87 zero, first section `TOKENS` at 136, TOC = 8 + 6·32 bytes
ending exactly at EOF (`pxrSample` below).

The reserved length is a parameter: the spec says 8 bytes (L11760-11761), OpenUSD
writes 64 (SPEC-ISSUE: LEAN-415).
-/

namespace UsdSpec.Crate.Header
open UsdSpec.Crate

/-- `PXR-USDC`. SPEC: L11698-11699. -/
def ident : List UInt8 := [0x50, 0x58, 0x52, 0x2D, 0x55, 0x53, 0x44, 0x43]

/-- Split off exactly `n` bytes. -/
def takeExact (n : Nat) (l : List UInt8) : Option (List UInt8 × List UInt8) :=
  if n ≤ l.length then some (l.take n, l.drop n) else none

theorem takeExact_append (xs r : List UInt8) :
    takeExact xs.length (xs ++ r) = some (xs, r) := by
  simp [takeExact]

theorem takeExact_append' (n : Nat) (xs r : List UInt8) (h : xs.length = n) :
    takeExact n (xs ++ r) = some (xs, r) := by
  subst h; exact takeExact_append xs r

/-! ## Versions. SPEC: L11709-11748.

SPEC-ISSUE: LEAN-411 — the version table (L11744) says 0.4.0 "Compressed the Table
of Contents Sections", but the TOC is always raw 32-byte entries (L11768-11782);
OpenUSD's history reads "0.4.0: Compressed structural sections" (crateFile.cpp L408). -/

structure Version where
  major : UInt8
  minor : UInt8
  patch : UInt8
  deriving DecidableEq, Repr, Inhabited

namespace Version
def key (v : Version) : Nat × Nat × Nat := (v.major.toNat, v.minor.toNat, v.patch.toNat)
/-- Lexicographic order on the triple. -/
def le (a b : Version) : Bool :=
  a.major.toNat < b.major.toNat ||
  (a.major == b.major && (a.minor.toNat < b.minor.toNat ||
    (a.minor == b.minor && a.patch.toNat ≤ b.patch.toNat)))
end Version

/-- SPEC: L11717-11718 "The minimum crate version on write must be `0.8.0`". -/
def minWrite : Version := ⟨0, 8, 0⟩
/-- SPEC: L11718-11719 "... and the maximum must be `0.13.0`". -/
def maxWrite : Version := ⟨0, 13, 0⟩
/-- SPEC: L11714-11715 "must support the latest crate version in this specification for
reading" — the latest row of the table L11733-11748 is `0.13.0`. -/
def latest : Version := ⟨0, 13, 0⟩

/-- A version a conforming writer may emit. SPEC: L11722-11723. -/
def writeOk (v : Version) : Bool := minWrite.le v && v.le maxWrite

/-- Versions a conforming reader must accept: everything a conforming writer may
produce. The spec only states "latest" (L11714-11715) plus "do not need to support
versions prior to the minimum" (L11723-11725); since writers are allowed every version
in the window, reading only `latest` would not interoperate. -/
def readRequired (v : Version) : Bool := writeOk v

theorem writeOk_min : writeOk minWrite = true := by decide
theorem writeOk_max : writeOk maxWrite = true := by decide
theorem writeOk_latest : writeOk latest = true := by decide
theorem not_writeOk_0_7 : writeOk ⟨0, 7, 0⟩ = false := by decide
/-- OpenUSD 26.11 itself can write 0.14.0 (ArrayEdit) .. 0.16.0 (GfDuration)
(`crateFile.cpp` L393-397), outside the spec window. -/
theorem not_writeOk_0_14 : writeOk ⟨0, 14, 0⟩ = false := by decide
/-- pxr 26.11 `usdcat` default output version. -/
theorem writeOk_pxr_default : writeOk ⟨0, 8, 0⟩ = true := by decide

/-! ## Header + bootstrap. SPEC: L11693-11703, L11750-11760. -/

structure Bootstrap where
  version  : Version
  /-- 5 unused header bytes (L11703; "recommended to be zero", L11706-11708). -/
  unused   : List UInt8
  /-- SPEC: L11759 "Unsigned 64 bit integer" (OpenUSD: `int64_t`). -/
  tocOffset : UInt64
  /-- Reserved bootstrap bytes: 8 per spec (L11760), 64 per OpenUSD. -/
  reserved : List UInt8
  deriving DecidableEq, Repr

/-- Length of the reserved block. -/
def specReservedLen : Nat := 8
def pxrReservedLen : Nat := 64

def Bootstrap.encode (b : Bootstrap) : List UInt8 :=
  ident ++ [b.version.major, b.version.minor, b.version.patch] ++ b.unused ++
    le64 b.tocOffset ++ b.reserved

def Bootstrap.decode (reservedLen : Nat) (bs : List UInt8) :
    Option (Bootstrap × List UInt8) := do
  let (id, r) ← takeExact 8 bs
  if id ≠ ident then none
  let (v, r) ← takeExact 3 r
  let (un, r) ← takeExact 5 r
  let (toc, r) ← rd64 r
  let (res, r) ← takeExact reservedLen r
  match v with
  | [a, b, c] => some (⟨⟨a, b, c⟩, un, toc, res⟩, r)
  | _ => none

/-- Total bootstrap size under a reserved length: 8 + 3 + 5 + 8 + reserved. -/
def bootstrapLen (reservedLen : Nat) : Nat := 24 + reservedLen

theorem encode_length (b : Bootstrap) (hu : b.unused.length = 5) :
    b.encode.length = bootstrapLen b.reserved.length := by
  simp [Bootstrap.encode, ident, le64, bootstrapLen, hu]; omega

/-- Roundtrip. -/
theorem decode_encode (b : Bootstrap) (r : List UInt8)
    (hu : b.unused.length = 5) :
    Bootstrap.decode b.reserved.length (b.encode ++ r) = some (b, r) := by
  obtain ⟨⟨ma, mi, pa⟩, un, toc, res⟩ := b
  simp only at hu
  have e : Bootstrap.encode ⟨⟨ma, mi, pa⟩, un, toc, res⟩ ++ r =
      ident ++ ([ma, mi, pa] ++ (un ++ (le64 toc ++ (res ++ r)))) := by
    simp [Bootstrap.encode]
  rw [e]
  simp only [Bootstrap.decode]
  rw [takeExact_append' 8 ident _ rfl]
  simp only [Option.bind_eq_bind, Option.bind_some, ne_eq, not_true_eq_false, ↓reduceIte]
  rw [takeExact_append' 3 [ma, mi, pa] _ rfl]
  simp only [Option.bind_some]
  rw [takeExact_append' 5 un _ hu]
  simp only [Option.bind_some]
  rw [rd64_le64]
  simp only [Option.bind_some]
  rw [takeExact_append]
  rfl

/-- SPEC-ISSUE: LEAN-415 — spec bootstrap is 8+3+5+8+8 = 32 bytes, OpenUSD's is 88. -/
theorem spec_vs_pxr_bootstrapLen :
    bootstrapLen specReservedLen = 32 ∧ bootstrapLen pxrReservedLen = 88 := by decide

/-! ## Table of contents. SPEC: L11764-11787. -/

structure Section where
  /-- 16 bytes, NUL-terminated ASCII (L11780). -/
  name  : List UInt8
  start : UInt64
  size  : UInt64
  deriving DecidableEq, Repr

def Section.encode (s : Section) : List UInt8 := s.name ++ le64 s.start ++ le64 s.size

def Section.decode (bs : List UInt8) : Option (Section × List UInt8) := do
  let (n, r) ← takeExact 16 bs
  let (st, r) ← rd64 r
  let (sz, r) ← rd64 r
  some (⟨n, st, sz⟩, r)

theorem Section.encode_length (s : Section) (h : s.name.length = 16) :
    s.encode.length = 32 := by
  simp [Section.encode, le64, h]

theorem Section.decode_encode (s : Section) (r : List UInt8) (h : s.name.length = 16) :
    Section.decode (s.encode ++ r) = some (s, r) := by
  obtain ⟨n, st, sz⟩ := s
  have e : Section.encode ⟨n, st, sz⟩ ++ r = n ++ (le64 st ++ (le64 sz ++ r)) := by
    simp [Section.encode]
  rw [e]; simp only [Section.decode]
  rw [takeExact_append' 16 n _ h]
  simp only [Option.bind_eq_bind, Option.bind_some]
  rw [rd64_le64]; simp only [Option.bind_some]
  rw [rd64_le64]; rfl

/-- Section name from an ASCII string: bytes then NUL padding to 16 (≤ 15 chars). -/
def nameBytes (s : String) : List UInt8 :=
  let b := s.toList.map (fun c => c.toNat.toUInt8)
  b ++ List.replicate (16 - b.length) 0

def encodeSections : List Section → List UInt8
  | [] => []
  | s :: ss => s.encode ++ encodeSections ss

def decodeSections : Nat → List UInt8 → Option (List Section × List UInt8)
  | 0, r => some ([], r)
  | k + 1, r => do
    let (s, r) ← Section.decode r
    let (ss, r) ← decodeSections k r
    some (s :: ss, r)

def encodeToc (ss : List Section) : List UInt8 :=
  le64 (UInt64.ofNat ss.length) ++ encodeSections ss

def decodeToc (bs : List UInt8) : Option (List Section × List UInt8) := do
  let (n, r) ← rd64 bs
  decodeSections n.toNat r

theorem decodeSections_encode (ss : List Section) (r : List UInt8)
    (h : ∀ s ∈ ss, s.name.length = 16) :
    decodeSections ss.length (encodeSections ss ++ r) = some (ss, r) := by
  induction ss with
  | nil => rfl
  | cons s ss ih =>
    simp only [List.length_cons, encodeSections, List.append_assoc, decodeSections]
    rw [Section.decode_encode s _ (h s (by simp))]
    simp only [Option.bind_eq_bind, Option.bind_some]
    rw [ih (fun t ht => h t (by simp [ht]))]
    rfl

theorem decodeToc_encodeToc (ss : List Section) (r : List UInt8)
    (h : ∀ s ∈ ss, s.name.length = 16) (hn : ss.length < 2 ^ 64) :
    decodeToc (encodeToc ss ++ r) = some (ss, r) := by
  simp only [decodeToc, encodeToc, List.append_assoc]
  rw [rd64_le64]
  simp only [Option.bind_eq_bind, Option.bind_some]
  have : (UInt64.ofNat ss.length).toNat = ss.length := by
    simp; omega
  rw [this]
  exact decodeSections_encode ss r h

theorem encodeToc_length (ss : List Section) (h : ∀ s ∈ ss, s.name.length = 16) :
    (encodeToc ss).length = 8 + 32 * ss.length := by
  simp only [encodeToc, List.length_append]
  have : (encodeSections ss).length = 32 * ss.length := by
    induction ss with
    | nil => rfl
    | cons s ss ih =>
      simp only [encodeSections, List.length_append, List.length_cons]
      rw [Section.encode_length s (h s (by simp)), ih (fun t ht => h t (by simp [ht]))]
      omega
  rw [this]; simp only [le64, List.length_cons, List.length_nil]

/-! ## Well-formedness. SPEC: L11766-11767 "The Table of Contents must be at the end of
the file." Sections are byte ranges `[start, start+size)`. -/

def Section.lo (s : Section) : Nat := s.start.toNat
def Section.hi (s : Section) : Nat := s.start.toNat + s.size.toNat

def disjoint (a b : Section) : Bool := a.hi ≤ b.lo || b.hi ≤ a.lo

def pairwiseDisjoint : List Section → Bool
  | [] => true
  | s :: ss => ss.all (disjoint s) && pairwiseDisjoint ss

/-- Well-formed TOC for a file of `fileSize` bytes with bootstrap length `boot`:
names NUL-terminated within 16 bytes, every section after the bootstrap and before
the TOC, sections pairwise non-overlapping, TOC ending exactly at end of file. The spec requires only that the TOC is at end
of file; the other conjuncts are OpenUSD-layout sanity checks. -/
def wellFormedToc (boot fileSize : Nat) (tocOffset : UInt64) (ss : List Section) : Bool :=
  ss.all (fun s => s.name.length == 16 && s.name.getLast? == some 0) &&
  ss.all (fun s => boot ≤ s.lo && s.hi ≤ tocOffset.toNat) &&
  pairwiseDisjoint ss &&
  boot ≤ tocOffset.toNat &&
  tocOffset.toNat + 8 + 32 * ss.length == fileSize

/-- The structural layout of pxr 26.11 `usdcat` output for a 7-path layer (791 bytes). -/
def pxrSample : List Section :=
  [⟨nameBytes "TOKENS", 136, 138⟩, ⟨nameBytes "STRINGS", 274, 8⟩,
   ⟨nameBytes "FIELDS", 282, 122⟩, ⟨nameBytes "FIELDSETS", 404, 44⟩,
   ⟨nameBytes "PATHS", 448, 78⟩, ⟨nameBytes "SPECS", 526, 65⟩]

theorem pxrSample_wellFormed : wellFormedToc 88 791 591 pxrSample = true := by decide

/-- A TOC that is not at the end of the file is rejected. -/
theorem trailing_garbage_not_wellFormed : wellFormedToc 88 792 591 pxrSample = false := by
  decide

/-- Overlap is rejected. -/
theorem overlap_not_wellFormed :
    wellFormedToc 88 791 591
      [⟨nameBytes "TOKENS", 136, 139⟩, ⟨nameBytes "STRINGS", 274, 8⟩] = false := by decide

/-- SPEC-ISSUE: LEAN-415 — the smallest layout the spec's 32-byte bootstrap admits
(no sections, TOC at 32: 40 bytes) is well-formed for the spec, but OpenUSD rejects
any file shorter than `sizeof(_BootStrap) = 88` (`crateFile.cpp` L3534). -/
theorem spec_minimal_file :
    wellFormedToc (bootstrapLen specReservedLen) 40 32 [] = true ∧
    40 < bootstrapLen pxrReservedLen := by decide

/-- The first 88 bytes of the pxr sample decode as a bootstrap with 64 reserved bytes. -/
def pxrPrefix : List UInt8 :=
  ident ++ [0, 8, 0] ++ List.replicate 5 0 ++ le64 591 ++ List.replicate 64 0

theorem pxrPrefix_decode :
    (Bootstrap.decode pxrReservedLen pxrPrefix).map (·.1.tocOffset) = some 591 := by decide

/-- With the spec's 8 reserved bytes the same prefix decodes, but leaves 56 bytes of the
reserved block to be misread as section data. -/
theorem pxrPrefix_decode_spec :
    (Bootstrap.decode specReservedLen pxrPrefix).map (·.2.length) = some 56 := by decide

end UsdSpec.Crate.Header
