import Mathlib.Data.List.Basic
/-!
# LZ4 block format and crate chunk framing (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L11576-11598 (Binary File Format → Compression → LZ4 Compression).
CLAIM: "LZ4 compression is described by the LZ4 library at <https://lz4.org/> and
is not expanded here, unless specifics must be provided." (L11580-11582)

The spec delegates the *block* format to the external LZ4 block specification
(lz4/doc/lz4_Block_format.md); only the chunk framing is spelled out, and its
field widths are wrong — `issues/aousd/01-lz4-chunk-framing-erratum.md` (the
framing used here is OpenUSD's `TfFastCompression`, pxr/base/tf/fastCompression.cpp,
1-byte chunk count, int32 LE per-chunk *compressed* size).

## LZ4 block format (external spec), as modelled

A block is a sequence of *sequences*. Each sequence is
`token · [literal-length ext] · literals · offset(2 bytes LE) · [match-length ext]`;
the token's high nibble is the literal length, the low nibble the match length
minus the minimum match 4; a nibble of 15 is continued by bytes, each `255`
adding 255 and continuing, the first byte `< 255` adding itself and stopping.
The match copies `matchlength` bytes starting `offset` bytes back in the
output, byte by byte, so the copy may overlap the bytes it produces (RLE).
The last sequence is literals only: the block ends right after its literals.

The decoder is split into `parse` (bytes → `Block`, structural, fuel = input
length) and `exec` (`Block` → bytes). Because the functions are total over
lists, "never reads past the input" is automatic: running out of input yields
`none` (e.g. `parse_truncated_literals`); output bounds are stated as length
lemmas (`exec_length`, `decompressUpTo_length_le`, `decompressExact_length`).

The end-of-block parsing restrictions of the LZ4 block spec ("the last 5 bytes
are always literals", "the last match must start at least 12 bytes before the
end of the block") are *encoder* obligations; the reference decoder
`LZ4_decompress_safe` relies on them for its fast path and rejects some
violating streams. They are modelled by `endRulesOk`, which both reference
compressors satisfy (literals-only trivially; greedy by construction of its
search window — checked by `decide` on examples, and every generated vector
(Gen/Vectors/Lz4.lean) is accepted by the real `LZ4_decompress_safe`).

SPEC-ISSUE: LEAN-421 — the per-chunk size field is "the size of the chunk in
bytes" (L11590-11592) without saying compressed or uncompressed; OpenUSD writes
and reads the *compressed* size. LightUSD's multi-chunk writer writes the
*uncompressed* size (src/next/c/crate-blob.c `lu_crate_blob_compress`) while its
reader treats it as compressed — exactly this misreading.
SPEC-ISSUE: LEAN-422 — nothing fixes the per-chunk uncompressed size
(OpenUSD: `LZ4_MAX_INPUT_SIZE = 0x7E000000`), the chunk-count limit (OpenUSD
writer ≤ 127, `GetMaxInputSize() = 127 * LZ4_MAX_INPUT_SIZE`; the reader reads
the count through a `char` and so is signed on most ABIs), nor how an empty
buffer is framed (OpenUSD: `[0x00, 0x00]`, LightUSD: `[0x00]`).
-/

namespace UsdSpec.Crate.Lz4

abbrev Bytes := List UInt8

/-! ## Length fields -/

/-- Continuation bytes for an extended length `n` (the part above the nibble's 15):
`⌊n/255⌋` bytes `255` then `n mod 255`. SPEC: LZ4 block format, "Token". -/
def encExt (n : Nat) : Bytes := List.replicate (n / 255) 255 ++ [UInt8.ofNat (n % 255)]

/-- Read continuation bytes, accumulating into `acc`. -/
def readExt : Nat → Bytes → Option (Nat × Bytes)
  | _, [] => none
  | acc, b :: bs => if b = 255 then readExt (acc + 255) bs else some (acc + b.toNat, bs)

/-- Bytes following the token for a length `n` stored in a nibble. -/
def lenExt (n : Nat) : Bytes := if 15 ≤ n then encExt (n - 15) else []

/-- Decode a length given its nibble value. -/
def readLen (nib : Nat) (bs : Bytes) : Option (Nat × Bytes) :=
  if nib = 15 then readExt 15 bs else some (nib, bs)

/-- The token byte: high nibble literal length, low nibble match length − 4. -/
def token (l m : Nat) : UInt8 := UInt8.ofNat (16 * min l 15 + min m 15)

/-! ## Blocks -/

/-- A sequence with a match. `mlen` is the actual match length (≥ 4). -/
structure Seq where
  lits : Bytes
  offset : Nat
  mlen : Nat
  deriving DecidableEq, Repr

/-- A block: sequences with matches, then the final literals-only sequence. -/
structure Block where
  seqs : List Seq
  last : Bytes
  deriving DecidableEq, Repr

def serSeq (s : Seq) : Bytes :=
  token s.lits.length (s.mlen - 4) :: (lenExt s.lits.length ++ s.lits ++
    [UInt8.ofNat (s.offset % 256), UInt8.ofNat (s.offset / 256)] ++ lenExt (s.mlen - 4))

def serLast (l : Bytes) : Bytes := token l.length 0 :: (lenExt l.length ++ l)

def serSeqs : List Seq → Bytes → Bytes
  | [], l => serLast l
  | s :: ss, l => serSeq s ++ serSeqs ss l

def serialize (b : Block) : Bytes := serSeqs b.seqs b.last

/-- Parse a block (fuel = remaining sequences budget; each sequence consumes ≥ 1 byte). -/
def parse : Nat → Bytes → Option Block
  | 0, _ => none
  | _ + 1, [] => none
  | f + 1, t :: bs =>
    match readLen (t.toNat / 16) bs with
    | none => none
    | some (ll, bs) =>
      if bs.length < ll then none else
      let lits := bs.take ll
      match bs.drop ll with
      | [] => some ⟨[], lits⟩
      | [_] => none
      | lo :: hi :: bs =>
        match readLen (t.toNat % 16) bs with
        | none => none
        | some (ml, bs) =>
          match parse f bs with
          | none => none
          | some b => some ⟨⟨lits, lo.toNat + 256 * hi.toNat, ml + 4⟩ :: b.seqs, b.last⟩

/-- Overlapping match copy: append `n` bytes, each the byte `d` back. -/
def copyMatch (out : Bytes) (d : Nat) : Nat → Bytes
  | 0 => out
  | n + 1 => copyMatch (out ++ [out.getD (out.length - d) 0]) d n

/-- Execute the sequences. Offset 0 or an offset reaching before the start of the
output is invalid (no external dictionary). -/
def exec (out : Bytes) : List Seq → Bytes → Option Bytes
  | [], l => some (out ++ l)
  | s :: ss, l =>
    let o := out ++ s.lits
    if s.offset = 0 ∨ o.length < s.offset then none
    else exec (copyMatch o s.offset s.mlen) ss l

def decompress (bs : Bytes) : Option Bytes :=
  match parse bs.length bs with
  | none => none
  | some b => exec [] b.seqs b.last

/-- `LZ4_decompress_safe` with an exact declared size (crate sections know it). -/
def decompressExact (n : Nat) (bs : Bytes) : Option Bytes :=
  match decompress bs with
  | some o => if o.length = n then some o else none
  | none => none

/-- Capacity-bounded decode: output longer than `cap` is an error. -/
def decompressUpTo (cap : Nat) (bs : Bytes) : Option Bytes :=
  match decompress bs with
  | some o => if o.length ≤ cap then some o else none
  | none => none

/-! ## Encoder obligations (LZ4 block spec "End of block conditions") -/

def seqsOutLen : List Seq → Nat
  | [] => 0
  | s :: ss => s.lits.length + s.mlen + seqsOutLen ss

/-- Every match starts ≥ 12 bytes before the end and ends ≥ 5 bytes before it. -/
def endRulesAux (total : Nat) : Nat → List Seq → Bool
  | _, [] => true
  | pos, s :: ss =>
    let mstart := pos + s.lits.length
    decide (mstart + 12 ≤ total) && decide (mstart + s.mlen + 5 ≤ total) &&
      endRulesAux total (mstart + s.mlen) ss

def endRulesOk (b : Block) : Bool :=
  endRulesAux (seqsOutLen b.seqs + b.last.length) 0 b.seqs

/-! ## Reference compressors -/

/-- Literals only: one final sequence. -/
def compressLiteralsOnly (xs : Bytes) : Bytes := serialize ⟨[], xs⟩

/-- Sound match check: `1 ≤ d ≤ i`, `d ≤ 65535`, `i + L ≤ |xs|`, and the `L`
bytes at `i` equal the bytes `d` back (overlap allowed). -/
def checkMatch (xs : Bytes) (i d L : Nat) : Bool :=
  decide (1 ≤ d) && decide (d ≤ i) && decide (d ≤ 65535) && decide (4 ≤ L) &&
    decide (i + L ≤ xs.length) &&
    (List.range L).all fun k => xs.getD (i + k) 0 == xs.getD (i - d + k) 0

/-- Length of the match at `i` with distance `d`, capped at `cap`. -/
def matchLen (a : Array UInt8) (i d : Nat) : Nat → Nat → Nat
  | 0, k => k
  | c + 1, k => if a.getD (i + k) 0 == a.getD (i - d + k) 0 then matchLen a i d c (k + 1) else k

/-- Longest match among distances `1..min(i, W)` (ties: smallest distance). -/
def bestMatch (a : Array UInt8) (i W cap : Nat) : Nat × Nat :=
  (List.range (min i W)).foldl (fun (best : Nat × Nat) j =>
    let d := j + 1
    let L := matchLen a i d cap 0
    if best.2 < L then (d, L) else best) (0, 0)

/-- Greedy matcher. A match is searched only where the LZ4 end rules allow one
(`i + 12 ≤ n`, match end `≤ n − 5`); it is re-validated by `checkMatch`, so the
correctness proof is independent of `bestMatch`. -/
def greedyGo (xs : Bytes) (a : Array UInt8) (W : Nat) : Nat → Nat → Nat → Block
  | 0, _, lit => ⟨[], xs.drop lit⟩
  | f + 1, i, lit =>
    if i + 12 ≤ xs.length then
      let m := bestMatch a i W (xs.length - 5 - i)
      if checkMatch xs i m.1 m.2 then
        let b := greedyGo xs a W f (i + m.2) (i + m.2)
        ⟨⟨(xs.drop lit).take (i - lit), m.1, m.2⟩ :: b.seqs, b.last⟩
      else greedyGo xs a W f (i + 1) lit
    else ⟨[], xs.drop lit⟩

def greedyBlock (W : Nat) (xs : Bytes) : Block := greedyGo xs xs.toArray W xs.length 0 0

/-- Greedy compressor with window `W` (≤ 65535 is enforced by `checkMatch`). -/
def compressGreedy (W : Nat) (xs : Bytes) : Bytes := serialize (greedyBlock W xs)

/-! ## Length-field lemmas -/

theorem toNat_ofNat_mod (n : Nat) : (UInt8.ofNat n).toNat = n % 256 := rfl

theorem toNat_ofNat_lt {n : Nat} (h : n < 256) : (UInt8.ofNat n).toNat = n := by
  rw [toNat_ofNat_mod, Nat.mod_eq_of_lt h]

theorem readExt_rep (k acc : Nat) (r : UInt8) (hr : r ≠ 255) (rest : Bytes) :
    readExt acc (List.replicate k 255 ++ r :: rest) = some (acc + 255 * k + r.toNat, rest) := by
  induction k generalizing acc with
  | zero => simp [readExt, hr]
  | succ k ih =>
    simp only [List.replicate_succ, List.cons_append, readExt, ite_true]
    rw [ih]; congr 2; omega

theorem readExt_encExt (acc n : Nat) (rest : Bytes) :
    readExt acc (encExt n ++ rest) = some (acc + n, rest) := by
  have h1 : n % 255 < 255 := Nat.mod_lt _ (by omega)
  have hr : (UInt8.ofNat (n % 255)).toNat = n % 255 := toNat_ofNat_lt (by omega)
  have hne : UInt8.ofNat (n % 255) ≠ 255 := by
    intro h; rw [h] at hr; simp at hr; omega
  unfold encExt
  rw [List.append_assoc, List.singleton_append, readExt_rep _ _ _ hne, hr]
  congr 2; have := Nat.div_add_mod n 255; omega

theorem readLen_lenExt (n : Nat) (rest : Bytes) :
    readLen (min n 15) (lenExt n ++ rest) = some (n, rest) := by
  unfold readLen lenExt
  by_cases h : 15 ≤ n
  · simp only [h, ite_true, Nat.min_eq_right h]
    rw [readExt_encExt]; congr 2; omega
  · have : min n 15 = n := Nat.min_eq_left (by omega)
    simp [h, this]; omega

theorem token_hi (l m : Nat) : (token l m).toNat / 16 = min l 15 := by
  unfold token; rw [toNat_ofNat_lt (by omega)]; omega

theorem token_lo (l m : Nat) : (token l m).toNat % 16 = min m 15 := by
  unfold token; rw [toNat_ofNat_lt (by omega)]; omega

/-! ## Parse ∘ serialize -/

/-- Sequence validity: offset in `1..65535`, match length ≥ 4. -/
def ValidSeq (s : Seq) : Prop := 1 ≤ s.offset ∧ s.offset ≤ 65535 ∧ 4 ≤ s.mlen

theorem serSeqs_length (ss : List Seq) (l : Bytes) : ss.length + 1 ≤ (serSeqs ss l).length := by
  induction ss with
  | nil => simp [serSeqs, serLast]
  | cons s ss ih => simp [serSeqs, serSeq]; omega

theorem parse_serSeqs (ss : List Seq) (l : Bytes) (hv : ∀ s ∈ ss, ValidSeq s) :
    ∀ f, ss.length + 1 ≤ f → parse f (serSeqs ss l) = some ⟨ss, l⟩ := by
  induction ss with
  | nil =>
    intro f hf
    obtain ⟨f, rfl⟩ : ∃ g, f = g + 1 := ⟨f - 1, by simp at hf; omega⟩
    simp only [serSeqs, serLast, parse, token_hi]
    rw [readLen_lenExt]
    simp
  | cons s ss ih =>
    intro f hf
    obtain ⟨f, rfl⟩ : ∃ g, f = g + 1 := ⟨f - 1, by simp at hf; omega⟩
    obtain ⟨h1, h2, h3⟩ := hv s (by simp)
    have ih' := ih (fun t ht => hv t (by simp [ht])) f (by simp at hf; omega)
    simp only [serSeqs, serSeq, List.cons_append, parse, token_hi, List.append_assoc]
    rw [readLen_lenExt]
    simp only [List.length_append, List.length_cons, List.length_nil]
    rw [if_neg (by omega)]
    simp only [List.cons_append, List.nil_append, List.drop_left', List.take_left']
    rw [token_lo, readLen_lenExt]
    simp only [ih']
    have ho : (UInt8.ofNat (s.offset % 256)).toNat + 256 * (UInt8.ofNat (s.offset / 256)).toNat
        = s.offset := by
      rw [toNat_ofNat_lt (by omega), toNat_ofNat_lt (by omega)]; omega
    simp only [ho, Option.some.injEq, Block.mk.injEq, List.cons.injEq, and_true]
    cases s; simp at h3 ⊢; omega

theorem parse_serialize (b : Block) (hv : ∀ s ∈ b.seqs, ValidSeq s) :
    parse (serialize b).length (serialize b) = some b :=
  parse_serSeqs b.seqs b.last hv _ (serSeqs_length _ _)

/-! ## Exec -/

theorem getD_take {xs : Bytes} {i j : Nat} (h : j < i) : (xs.take i).getD j 0 = xs.getD j 0 := by
  simp [List.getD_eq_getElem?_getD, List.getElem?_take, h]

theorem take_succ_getD {xs : Bytes} {i : Nat} (h : i < xs.length) :
    xs.take i ++ [xs.getD i 0] = xs.take (i + 1) := by
  rw [List.take_succ]; simp [List.getD_eq_getElem?_getD, List.getElem?_eq_getElem h]

/-- An overlapping copy of a validated match reproduces the input. -/
theorem copyMatch_take (xs : Bytes) (d : Nat) :
    ∀ L i, 1 ≤ d → d ≤ i → i + L ≤ xs.length →
      (∀ k < L, xs.getD (i + k) 0 = xs.getD (i - d + k) 0) →
      copyMatch (xs.take i) d L = xs.take (i + L) := by
  intro L
  induction L with
  | zero => intro i _ _ _ _; rfl
  | succ L ih =>
    intro i h1 h2 h3 hk
    have hi : (xs.take i).length = i := by simp; omega
    simp only [copyMatch, hi]
    rw [getD_take (by omega)]
    have e0 := hk 0 (by omega)
    simp only [Nat.add_zero] at e0
    rw [← e0, take_succ_getD (by omega), ih (i + 1) h1 (by omega) (by omega)]
    · congr 1; omega
    · intro k hkL
      have := hk (k + 1) (by omega)
      rw [show i + 1 + k = i + (k + 1) by omega, show i + 1 - d + k = i - d + (k + 1) by omega]
      exact this

theorem checkMatch_spec {xs : Bytes} {i d L : Nat} (h : checkMatch xs i d L = true) :
    1 ≤ d ∧ d ≤ i ∧ d ≤ 65535 ∧ 4 ≤ L ∧ i + L ≤ xs.length ∧
      ∀ k < L, xs.getD (i + k) 0 = xs.getD (i - d + k) 0 := by
  simp only [checkMatch, Bool.and_eq_true, decide_eq_true_eq, List.all_eq_true,
    List.mem_range, beq_iff_eq] at h
  obtain ⟨⟨⟨⟨⟨a, b⟩, c⟩, e⟩, f⟩, g⟩ := h
  exact ⟨a, b, c, e, f, g⟩

/-! ## General greedy end-of-block obligations -/

private theorem matchLen_le (a : Array UInt8) (i d cap k : Nat) :
    matchLen a i d cap k ≤ k + cap := by
  induction cap generalizing k with
  | zero => simp [matchLen]
  | succ cap ih =>
    simp only [matchLen]
    split
    · have := ih (k + 1); omega
    · omega

private theorem bestMatch_le (a : Array UInt8) (i W cap : Nat) :
    (bestMatch a i W cap).2 ≤ cap := by
  have aux : ∀ (xs : List Nat) (best : Nat × Nat), best.2 ≤ cap →
      (xs.foldl (fun best j =>
        let d := j + 1
        let L := matchLen a i d cap 0
        if best.2 < L then (d, L) else best) best).2 ≤ cap := by
    intro xs
    induction xs with
    | nil => intro best h; exact h
    | cons j xs ih =>
      intro best h
      simp only [List.foldl_cons]
      split
      · exact ih _ (by simpa using matchLen_le a i (j + 1) cap 0)
      · exact ih _ h
  exact aux _ (0, 0) (Nat.zero_le _)

/-- The matcher emits exactly the unconsumed suffix, for any fuel/window. -/
theorem greedyGo_outLen (xs : Bytes) (a : Array UInt8) (W : Nat) :
    ∀ f i lit, lit ≤ i → i ≤ xs.length →
      seqsOutLen (greedyGo xs a W f i lit).seqs +
        (greedyGo xs a W f i lit).last.length + lit = xs.length := by
  intro f
  induction f with
  | zero => intro i lit hl hi; simp [greedyGo, seqsOutLen]; omega
  | succ f ih =>
    intro i lit hl hi
    simp only [greedyGo]
    split
    · split
      · rename_i hn hc
        have hm := (checkMatch_spec hc).2.2.2.2.1
        have hr := ih (i + (bestMatch a i W (xs.length - 5 - i)).2)
          (i + (bestMatch a i W (xs.length - 5 - i)).2) (Nat.le_refl _) hm
        simp only [seqsOutLen, List.length_take, List.length_drop]
        rw [Nat.min_eq_left (by omega)]
        omega
      · rename_i hn _
        exact ih (i + 1) lit (by omega) (by omega)
    · simp [seqsOutLen]; omega

/-- Every emitted match satisfies both LZ4 end-of-block distance rules. -/
theorem greedyGo_endRules (xs : Bytes) (a : Array UInt8) (W : Nat) :
    ∀ f i lit, lit ≤ i → i ≤ xs.length →
      endRulesAux xs.length lit (greedyGo xs a W f i lit).seqs = true := by
  intro f
  induction f with
  | zero => intro i lit _ _; rfl
  | succ f ih =>
    intro i lit hl hi
    simp only [greedyGo]
    split
    · split
      · rename_i hn hc
        have hm := (checkMatch_spec hc).2.2.2.2.1
        have hb := bestMatch_le a i W (xs.length - 5 - i)
        have hlen : ((xs.drop lit).take (i - lit)).length = i - lit := by
          simp only [List.length_take, List.length_drop]
          exact Nat.min_eq_left (by omega)
        simp only [endRulesAux, hlen]
        have hpos : lit + (i - lit) = i := by omega
        rw [hpos]
        simp only [decide_eq_true_eq, Bool.and_eq_true]
        exact ⟨⟨hn, by omega⟩, ih _ _ (Nat.le_refl _) hm⟩
      · rename_i hn _
        exact ih (i + 1) lit (by omega) (by omega)
    · rfl

/-- LZ4 end rules for every input, including short and empty literal blocks. -/
theorem greedyBlock_endRulesOk (W : Nat) (xs : Bytes) :
    endRulesOk (greedyBlock W xs) = true := by
  have hlen := greedyGo_outLen xs xs.toArray W xs.length 0 0 (by omega) (by omega)
  simp only [Nat.add_zero] at hlen
  unfold endRulesOk greedyBlock
  rw [hlen]
  exact greedyGo_endRules xs xs.toArray W xs.length 0 0 (by omega) (by omega)

theorem greedyGo_valid (xs : Bytes) (a : Array UInt8) (W : Nat) :
    ∀ f i lit, ∀ s ∈ (greedyGo xs a W f i lit).seqs, ValidSeq s := by
  intro f
  induction f with
  | zero => intro i lit s hs; simp [greedyGo] at hs
  | succ f ih =>
    intro i lit s hs
    simp only [greedyGo] at hs
    split at hs
    · split at hs
      · rename_i hc
        obtain ⟨h1, -, h2, h3, -⟩ := checkMatch_spec hc
        simp only [List.mem_cons] at hs
        rcases hs with rfl | hs
        · exact ⟨h1, h2, h3⟩
        · exact ih _ _ s hs
      · exact ih _ _ s hs
    · simp at hs

theorem greedyGo_exec (xs : Bytes) (a : Array UInt8) (W : Nat) :
    ∀ f i lit, lit ≤ i → i ≤ xs.length →
      exec (xs.take lit) (greedyGo xs a W f i lit).seqs (greedyGo xs a W f i lit).last
        = some xs := by
  intro f
  induction f with
  | zero => intro i lit _ _; simp [greedyGo, exec]
  | succ f ih =>
    intro i lit hl hi
    simp only [greedyGo]
    split
    · split
      · rename_i hn hc
        obtain ⟨h1, h2, h3, h4, h5, h6⟩ := checkMatch_spec hc
        simp only [exec]
        have hcat : xs.take lit ++ (xs.drop lit).take (i - lit) = xs.take i := by
          rw [← List.take_add]; congr 1; omega
        rw [hcat]
        have hlen : (xs.take i).length = i := by simp; omega
        simp only [hlen]
        rw [if_neg (by omega), copyMatch_take xs _ _ _ h1 h2 h5 h6]
        exact ih _ _ (Nat.le_refl _) h5
      · rename_i hn _
        exact ih _ _ (by omega) (by omega)
    · simp [exec]

/-- LZ1: literals-only roundtrip. -/
theorem decompress_compressLiteralsOnly (xs : Bytes) :
    decompress (compressLiteralsOnly xs) = some xs := by
  unfold decompress compressLiteralsOnly
  rw [parse_serialize ⟨[], xs⟩ (by simp)]
  simp [exec]

/-- LZ2: greedy-matcher roundtrip, for every window size. -/
theorem decompress_compressGreedy (W : Nat) (xs : Bytes) :
    decompress (compressGreedy W xs) = some xs := by
  unfold decompress compressGreedy greedyBlock
  rw [parse_serialize _ (greedyGo_valid _ _ _ _ _ _)]
  simpa using greedyGo_exec xs xs.toArray W xs.length 0 0 (Nat.le_refl _) (Nat.zero_le _)

/-! ## LZ3: decoder bounds -/

theorem copyMatch_length (out : Bytes) (d n : Nat) : (copyMatch out d n).length = out.length + n := by
  induction n generalizing out with
  | zero => rfl
  | succ n ih => simp [copyMatch, ih]; omega

/-- The output length is exactly the literal plus match lengths (nothing else is written). -/
theorem exec_length (ss : List Seq) : ∀ (out l o : Bytes), exec out ss l = some o →
    o.length = out.length + seqsOutLen ss + l.length := by
  induction ss with
  | nil => intro out l o h; simp [exec] at h; subst h; simp [seqsOutLen]
  | cons s ss ih =>
    intro out l o h
    simp only [exec] at h
    split at h
    · simp at h
    · have := ih _ _ _ h; simp [copyMatch_length] at this; simp [seqsOutLen]; omega

theorem decompressUpTo_length_le {cap : Nat} {bs o : Bytes} (h : decompressUpTo cap bs = some o) :
    o.length ≤ cap := by
  unfold decompressUpTo at h
  split at h
  · split at h
    · simp at h; subst h; assumption
    · simp at h
  · simp at h

theorem decompressExact_length {n : Nat} {bs o : Bytes} (h : decompressExact n bs = some o) :
    o.length = n := by
  unfold decompressExact at h
  split at h
  · split at h
    · simp at h; subst h; assumption
    · simp at h
  · simp at h

theorem decompress_nil : decompress [] = none := rfl

/-- Running out of input inside a literal run is an error, never a read past the end. -/
theorem parse_truncated_literals (f : Nat) (rest : Bytes) (h : rest.length < 3) :
    parse (f + 1) (token 3 0 :: rest) = none := by
  simp [parse, token_hi, readLen, h]

/-! ## Crate chunk framing (OpenUSD `TfFastCompression`) -/

/-- OpenUSD `LZ4_MAX_INPUT_SIZE` (pxr/base/tf/pxrLZ4/lz4.h:182): the uncompressed
size of every chunk but the last. -/
def lz4MaxInputSize : Nat := 0x7E000000

/-- Split into chunks of `k` bytes (the last may be shorter). -/
def chunksGo (k : Nat) : Nat → Bytes → List Bytes
  | 0, _ => []
  | _ + 1, [] => []
  | f + 1, x :: xs => (x :: xs).take k :: chunksGo k f ((x :: xs).drop k)

def chunks (k : Nat) (xs : Bytes) : List Bytes := chunksGo k xs.length xs

/-- int32 little-endian. -/
def le32 (n : Nat) : Bytes :=
  [UInt8.ofNat n, UInt8.ofNat (n / 256), UInt8.ofNat (n / 65536), UInt8.ofNat (n / 16777216)]

def frameChunk (comp : Bytes → Bytes) (c : Bytes) : Bytes := le32 (comp c).length ++ comp c

/-- `TfFastCompression::CompressToBuffer` generalized over the chunk size `k`
(OpenUSD: `k = lz4MaxInputSize`). -/
def frame (k : Nat) (comp : Bytes → Bytes) (xs : Bytes) : Bytes :=
  if xs.length ≤ k then 0 :: comp xs
  else UInt8.ofNat (chunks k xs).length :: (chunks k xs).flatMap (frameChunk comp)

/-- Read a little-endian int32; a negative or zero size is an error. -/
def readSize : Bytes → Option (Nat × Bytes)
  | b0 :: b1 :: b2 :: b3 :: rest =>
    let v := b0.toNat + 256 * b1.toNat + 65536 * b2.toNat + 16777216 * b3.toNat
    if v = 0 ∨ 2 ^ 31 ≤ v then none else some (v, rest)
  | _ => none

/-- Per-chunk decode; trailing bytes after the last chunk are ignored (as in
OpenUSD and LightUSD). -/
def unchunks (dec : Bytes → Option Bytes) : Nat → Bytes → Option Bytes
  | 0, _ => some []
  | n + 1, bs =>
    match readSize bs with
    | none => none
    | some (s, rest) =>
      if rest.length < s then none else
      match dec (rest.take s), unchunks dec n (rest.drop s) with
      | some a, some b => some (a ++ b)
      | _, _ => none

/-- `TfFastCompression::DecompressFromBuffer`. -/
def unframe (dec : Bytes → Option Bytes) : Bytes → Option Bytes
  | [] => none
  | c :: rest => if c = 0 then dec rest else unchunks dec c.toNat rest

theorem readSize_le32 (n : Nat) (h0 : 0 < n) (h : n < 2 ^ 31) (rest : Bytes) :
    readSize (le32 n ++ rest) = some (n, rest) := by
  simp only [le32, List.cons_append, List.nil_append, readSize, toNat_ofNat_mod]
  rw [if_neg (by omega)]
  congr 2; omega

theorem chunksGo_flatten (k : Nat) (hk : 0 < k) :
    ∀ f xs, xs.length ≤ f → (chunksGo k f xs).flatten = xs := by
  intro f
  induction f with
  | zero => intro xs h; simp at h; subst h; rfl
  | succ f ih =>
    intro xs h
    cases xs with
    | nil => rfl
    | cons x xs =>
      simp only [chunksGo, List.flatten_cons]
      rw [ih _ (by simp at h ⊢; omega)]
      exact List.take_append_drop _ _

theorem chunksGo_ne_nil (k : Nat) (hk : 0 < k) :
    ∀ f xs, ∀ c ∈ chunksGo k f xs, c ≠ [] := by
  intro f
  induction f with
  | zero => intro xs c h; simp [chunksGo] at h
  | succ f ih =>
    intro xs c h
    cases xs with
    | nil => simp [chunksGo] at h
    | cons x xs =>
      simp only [chunksGo, List.mem_cons] at h
      rcases h with rfl | h
      · cases k with
        | zero => omega
        | succ k => simp
      · exact ih _ c h

theorem chunksGo_le (k : Nat) :
    ∀ f xs, ∀ c ∈ chunksGo k f xs, c.length ≤ k := by
  intro f
  induction f with
  | zero => intro xs c h; simp [chunksGo] at h
  | succ f ih =>
    intro xs c h
    cases xs with
    | nil => simp [chunksGo] at h
    | cons x xs =>
      simp only [chunksGo, List.mem_cons] at h
      rcases h with rfl | h
      · simp only [List.length_take]; omega
      · exact ih _ c h

theorem chunksGo_length (k : Nat) (hk : 0 < k) :
    ∀ f xs, (chunksGo k f xs).length * k < xs.length + k := by
  intro f
  induction f with
  | zero => intro xs; simp [chunksGo]; omega
  | succ f ih =>
    intro xs
    cases xs with
    | nil => simp [chunksGo]; omega
    | cons x xs =>
      simp only [chunksGo, List.length_cons, Nat.succ_mul]
      have := ih ((x :: xs).drop k)
      simp only [List.length_drop, List.length_cons] at this
      by_cases hle : xs.length + 1 ≤ k
      · rw [show xs.length + 1 - k = 0 by omega] at this
        have : (chunksGo k f (List.drop k (x :: xs))).length = 0 := by
          rcases Nat.eq_zero_or_pos (chunksGo k f (List.drop k (x :: xs))).length with h | h
          · exact h
          · exfalso; have := Nat.mul_le_mul_right k h; omega
        rw [this]; omega
      · omega

theorem unchunks_flatMap (dec : Bytes → Option Bytes) (comp : Bytes → Bytes)
    (hrt : ∀ ys, dec (comp ys) = some ys)
    (cs : List Bytes)
    (hsz : ∀ c ∈ cs, 0 < (comp c).length ∧ (comp c).length < 2 ^ 31) : ∀ trailing,
      unchunks dec cs.length (cs.flatMap (frameChunk comp) ++ trailing) = some cs.flatten := by
  induction cs with
  | nil => intro _; rfl
  | cons c cs ih =>
    intro tr
    obtain ⟨h0, h1⟩ := hsz c (by simp)
    simp only [List.flatMap_cons, List.length_cons, unchunks, frameChunk, List.append_assoc]
    rw [readSize_le32 _ h0 h1]
    simp only [List.length_append, List.take_left', List.drop_left']
    rw [if_neg (by omega), hrt, ih (fun d hd => hsz d (by simp [hd]))]
    simp

/-- LZ4-FRAME: the chunk framing roundtrips over any block compressor with a
roundtripping decoder, for any chunk size `k > 0` and at most 255 chunks.
OpenUSD's writer instance: `k = lz4MaxInputSize`, ≤ 127 chunks. -/
theorem unframe_frame (k : Nat) (hk : 0 < k) (dec : Bytes → Option Bytes) (comp : Bytes → Bytes)
    (hrt : ∀ ys, dec (comp ys) = some ys)
    (hsz : ∀ ys, ys ≠ [] → ys.length ≤ k → 0 < (comp ys).length ∧ (comp ys).length < 2 ^ 31)
    (xs : Bytes) (hn : xs.length ≤ 255 * k) :
    unframe dec (frame k comp xs) = some xs := by
  unfold frame
  split
  · simp [unframe, hrt]
  · rename_i hgt
    have hlen := chunksGo_length k hk xs.length xs
    have hc : (chunks k xs).length < 256 := by
      unfold chunks
      by_contra hc
      have := Nat.mul_le_mul_right k (show 256 ≤ (chunksGo k xs.length xs).length by omega)
      omega
    have hc1 : 1 ≤ (chunks k xs).length := by
      unfold chunks
      cases xs with
      | nil => simp at hgt
      | cons x xs => simp [chunksGo]
    have ht : (UInt8.ofNat (chunks k xs).length).toNat = (chunks k xs).length := toNat_ofNat_lt hc
    have hne0 : UInt8.ofNat (chunks k xs).length ≠ 0 := by
      intro h; rw [h] at ht; simp at ht; omega
    simp only [unframe, hne0, ite_false, ht]
    have := unchunks_flatMap dec comp hrt (chunks k xs)
      (fun c hc => hsz c (chunksGo_ne_nil k hk _ _ c hc) (chunksGo_le k _ _ c hc)) []
    rw [List.append_nil] at this
    rw [this]; unfold chunks; rw [chunksGo_flatten k hk _ _ (Nat.le_refl _)]

theorem encExt_length (n : Nat) : (encExt n).length = n / 255 + 1 := by simp [encExt]

theorem lenExt_length_le (n : Nat) : (lenExt n).length ≤ n / 255 + 1 := by
  unfold lenExt; split
  · rw [encExt_length]; have := Nat.div_le_div_right (c := 255) (Nat.sub_le n 15); omega
  · simp

/-- Size of the literals-only block: `1 + |ext| + n`. -/
theorem compressLiteralsOnly_length (xs : Bytes) :
    0 < (compressLiteralsOnly xs).length ∧
      (compressLiteralsOnly xs).length ≤ xs.length + xs.length / 255 + 2 := by
  simp only [compressLiteralsOnly, serialize, serSeqs, serLast, List.length_cons, List.length_append]
  have := lenExt_length_le xs.length; omega

/-- Framed literals-only roundtrip for any chunk size up to OpenUSD's. -/
theorem unframe_frame_literalsOnly (k : Nat) (hk : 0 < k) (hk2 : k ≤ lz4MaxInputSize)
    (xs : Bytes) (hn : xs.length ≤ 255 * k) :
    unframe decompress (frame k compressLiteralsOnly xs) = some xs := by
  apply unframe_frame k hk decompress compressLiteralsOnly decompress_compressLiteralsOnly _ xs hn
  intro ys _ hy
  have := compressLiteralsOnly_length ys
  unfold lz4MaxInputSize at hk2
  refine ⟨this.1, ?_⟩
  have : ys.length / 255 ≤ k / 255 := Nat.div_le_div_right hy
  omega

/-! ## Examples (by `decide`) -/

/-- Overlapping match = run-length: `a` then a match of 15 at distance 1, then 5 literals. -/
example : decompress [0x1B, 0x61, 0x01, 0x00, 0x50, 0x62, 0x62, 0x62, 0x62, 0x62]
    = some (List.replicate 16 0x61 ++ List.replicate 5 0x62) := by decide

/-- Offset 0 is rejected; an offset reaching before the output start is rejected. -/
example : decompress [0x10, 0x61, 0x00, 0x00, 0x00] = none := by decide
example : decompress [0x10, 0x61, 0x02, 0x00, 0x00] = none := by decide

/-- Empty block: one token `0x00`. -/
example : decompress [0x00] = some [] := by decide
example : compressLiteralsOnly [] = [0x00] := by decide

/-- Literal length 15 needs one extension byte `0`; 270 = 15 + 255 + 0 needs `255, 0`. -/
example : (compressLiteralsOnly (List.replicate 15 7)).take 2 = [0xF0, 0x00] := by decide
set_option maxRecDepth 20000 in
example : (compressLiteralsOnly (List.replicate 270 7)).take 3 = [0xF0, 0xFF, 0x00] := by decide

def sampleAbc : Bytes := (List.range 40).map fun i => UInt8.ofNat (97 + i % 3)

/-- The greedy matcher finds the period-3 match and respects the end rules. -/
example : compressGreedy 16 sampleAbc
    = [0x3F, 0x61, 0x62, 0x63, 0x03, 0x00, 0x0D, 0x50, 0x63, 0x61, 0x62, 0x63, 0x61] := by decide
example : endRulesOk (greedyBlock 16 sampleAbc) = true := by decide
example : endRulesOk (greedyBlock 16 (List.replicate 64 0)) = true := by decide
example : decompress (compressGreedy 16 sampleAbc) = some sampleAbc := by decide

end UsdSpec.Crate.Lz4
