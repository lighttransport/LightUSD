import Mathlib.Data.List.Chain
import UsdSpec.Unicode.NormTables
import UsdSpec.Unicode.XidTable

/-!
# Unicode canonical normalization (NFD / NFC) and what the spec says about it

SPEC: core_spec.md L1805-1837 (6.9 Names: "Spec names are UTF-8 encoded
identifiers using the `XID_Start` and `XID_Continue` classes defined by the
Unicode Standard"), L3620-3647 (8 Path grammar, XID classes "used in their
default form"), L3516-3531 (Element ordering: "ASCII letters shall be case
folded …", "Unicode characters shall be ordered according to their code
points") and L3903-3909:

CLAIM (L3903-3909): "**Normalization**: Normalization is out of scope for the
Path grammar. This means that Unicode characters that would normalize to
equivalent under a particular normalization form (e.g., NKFD, etc.) are treated
as distinct and hence, the paths containing those characters would be treated
as distinct paths. For example, the strings “IX” (**U+0049 U+0058**) and “Ⅸ”
(**U+2168**) are distinct strings when normalization forms are not applied."

So the spec does *not* require normalization: paths compare by code points.
The case folding in element ordering is ASCII-only, so no Unicode case-folding
table is needed. This module formalizes NFD/NFC (UAX #15: recursive canonical
decomposition, canonical ordering, canonical composition, Hangul algorithmic
(de)composition) to state precisely what the non-normalization rule means:
canonically equivalent but distinct names (`cafe_nfc ≠ cafe_nfd` although
`nfc cafe_nfd = cafe_nfc`).

SPEC-ISSUE: LEAN-211 — the "normalization is out of scope" rule is stated only
under the Path grammar's runtime considerations (L3903-3909); §6.9 Names
(L1805-1817), which defines sibling-name uniqueness, does not repeat it, so
two sibling prims `café` (NFC) and `café` (NFD) are unique names that render
identically. OpenUSD 26.11 and LightUSD agree: distinct `Sdf.Path`s, both valid
identifiers (`Sdf.Path.IsValidIdentifier` / `lu_is_valid_identifier`), two
distinct children after composition and flatten.
SPEC-ISSUE: LEAN-212 — "NKFD" (L3905) is not a normalization form; NFKD is meant.
The example `IX` vs `Ⅸ` is a *compatibility* (NFKC/NFKD) equivalence, not a
canonical one: NFC keeps `Ⅸ` (`nfc_roman_nine`).
-/

namespace UsdSpec.Unicode.Norm

/-! ## Table lookups (binary search, fuel-bounded) -/

/-- First index in `[lo, hi)` whose key is `≥ k` (keys sorted ascending). -/
def lowerBound {α : Type} (tbl : Array α) (key : α → Nat) (k : Nat) : Nat :=
  let rec go (lo hi : Nat) : Nat → Nat
    | 0 => lo
    | fuel + 1 =>
      if lo < hi then
        let mid := (lo + hi) / 2
        match tbl[mid]? with
        | some e => if key e < k then go (mid + 1) hi fuel else go lo mid fuel
        | none => lo
      else lo
  go 0 tbl.size tbl.size

/-- Canonical combining class. -/
def ccc (c : UInt32) : Nat :=
  let i := lowerBound cccRanges (fun e => e.2.1.toNat) c.toNat
  match cccRanges[i]? with
  | some (lo, _, v) => if lo ≤ c then v else 0
  | none => 0

/-- One-level canonical mapping, if any (Hangul excluded). -/
def decompOne (c : UInt32) : Option (List UInt32) :=
  let i := lowerBound canonDecomp (fun e => e.1.toNat) c.toNat
  match canonDecomp[i]? with
  | some (k, a, b) => if k = c then some (if b = 0 then [a] else [a, b]) else none
  | none => none

/-- Primary composite of a pair, if any (Hangul excluded). -/
def composeTbl (a b : UInt32) : Option UInt32 :=
  let k := a.toNat * 2 ^ 21 + b.toNat
  let i := lowerBound compositions (fun e => e.1.toNat) k
  match compositions[i]? with
  | some (k', c) => if k'.toNat = k then some c else none
  | none => none

/-! ## Hangul (Unicode §3.12) -/

def sBase : Nat := 0xAC00
def lBase : Nat := 0x1100
def vBase : Nat := 0x1161
def tBase : Nat := 0x11A7
def lCount : Nat := 19
def vCount : Nat := 21
def tCount : Nat := 28
def nCount : Nat := vCount * tCount
def sCount : Nat := lCount * nCount

def hangulDecomp (c : UInt32) : Option (List UInt32) :=
  let s := c.toNat
  if sBase ≤ s ∧ s < sBase + sCount then
    let si := s - sBase
    let l := lBase + si / nCount
    let v := vBase + (si % nCount) / tCount
    let t := tBase + si % tCount
    some (if si % tCount = 0 then [l.toUInt32, v.toUInt32] else [l.toUInt32, v.toUInt32, t.toUInt32])
  else none

def hangulCompose (a b : UInt32) : Option UInt32 :=
  let x := a.toNat; let y := b.toNat
  if lBase ≤ x ∧ x < lBase + lCount ∧ vBase ≤ y ∧ y < vBase + vCount then
    some (sBase + ((x - lBase) * vCount + (y - vBase)) * tCount).toUInt32
  else if sBase ≤ x ∧ x < sBase + sCount ∧ (x - sBase) % tCount = 0 ∧ tBase < y ∧ y < tBase + tCount then
    some (x + (y - tBase)).toUInt32
  else none

def compose (a b : UInt32) : Option UInt32 :=
  match hangulCompose a b with
  | some c => some c
  | none => composeTbl a b

/-! ## NFD -/

/-- Full canonical decomposition of one character (depth ≤ `fuel`; canonical
mappings nest at most 3 deep, so 8 is ample). -/
def decompFull : Nat → UInt32 → List UInt32
  | 0, c => [c]
  | f + 1, c =>
    match hangulDecomp c with
    | some cs => cs
    | none =>
      match decompOne c with
      | some cs => cs.flatMap (decompFull f)
      | none => [c]

/-- Canonical ordering step on a *reversed* accumulator: a non-starter moves
before (to the left of) preceding characters of strictly greater class. -/
def insertRev {α : Type} (k : α → Nat) (x : α) : List α → List α
  | [] => [x]
  | y :: ys => if k x ≠ 0 ∧ k x < k y then y :: insertRev k x ys else x :: y :: ys

def reorderAcc {α : Type} (k : α → Nat) : List α → List α → List α
  | acc, [] => acc.reverse
  | acc, x :: xs => reorderAcc k (insertRev k x acc) xs

/-- Canonical Ordering Algorithm (UAX #15 D109): stable sort of each run of
non-starters by combining class. -/
def canonicalOrder {α : Type} (k : α → Nat) (xs : List α) : List α := reorderAcc k [] xs

def nfdCps (cs : List UInt32) : List UInt32 := canonicalOrder ccc (cs.flatMap (decompFull 8))

/-! ## NFC: Canonical Composition Algorithm (UAX #15 D117) -/

/-- `out` is the output so far; `starter` the index in `out` of the last
starter (if any); `last` the class of the last character appended (256 when it
blocks everything, i.e. the text began with a non-starter). -/
def composeGo (out : Array UInt32) (starter : Option Nat) (last : Nat) : List UInt32 → Array UInt32
  | [] => out
  | c :: cs =>
    let cc := ccc c
    let composed : Option UInt32 :=
      match starter with
      | some si =>
        if last < cc ∨ last = 0 then
          match out[si]? with
          | some s => compose s c
          | none => none
        else none
      | none => none
    match composed, starter with
    | some p, some si => composeGo (out.set! si p) starter last cs
    | _, _ =>
      if cc = 0 then composeGo (out.push c) (some out.size) 0 cs
      else composeGo (out.push c) starter cc cs

def composeCps : List UInt32 → List UInt32
  | [] => []
  | c :: cs =>
    let cc := ccc c
    (composeGo #[c] (if cc = 0 then some 0 else none) (if cc = 0 then 0 else 256) cs).toList

def nfcCps (cs : List UInt32) : List UInt32 := composeCps (nfdCps cs)

def cps (s : String) : List UInt32 := s.toList.map (·.val)

def ofCps (cs : List UInt32) : String :=
  String.ofList (cs.map fun c => if h : c.toNat.isValidChar then Char.ofNatAux c.toNat h else '�')

def nfd (s : String) : String := ofCps (nfdCps (cps s))
def nfc (s : String) : String := ofCps (nfcCps (cps s))

/-! ## UN1 (canonical ordering): `canonicalOrder` is idempotent, for any class function -/

section Ordering
variable {α : Type} (k : α → Nat)

/-- Canonical order: no adjacent `a b` with `ccc a > ccc b > 0`. -/
def COrd (a b : α) : Prop := ¬ (0 < k b ∧ k b < k a)

theorem insertRev_head (x : α) (l : List α) :
    (insertRev k x l).head? = some x ∨ (insertRev k x l).head? = l.head? := by
  cases l with
  | nil => simp [insertRev]
  | cons y ys =>
    simp only [insertRev]; split <;> simp

/-- Insertion preserves canonical order of the reversed accumulator. -/
theorem insertRev_chain (x : α) :
    ∀ l : List α, List.IsChain (fun u v => COrd k v u) l →
      List.IsChain (fun u v => COrd k v u) (insertRev k x l) := by
  intro l
  induction l with
  | nil => intro _; simp [insertRev]
  | cons y ys ih =>
    intro h
    simp only [insertRev]
    split
    · rename_i hxy
      have hys : List.IsChain (fun u v => COrd k v u) ys := h.tail
      have ih' := ih hys
      rcases hi : insertRev k x ys with _ | ⟨z, zs⟩
      · simp
      · rw [hi] at ih'
        refine List.IsChain.cons_cons ?_ ih'
        rcases insertRev_head k x ys with hh | hh <;> rw [hi] at hh <;> simp at hh
        · subst hh; unfold COrd; omega
        · cases ys with
          | nil => simp at hh
          | cons w ws =>
            simp at hh; subst hh
            exact (List.isChain_cons_cons.1 h).1
    · rename_i hxy
      exact List.IsChain.cons_cons (by unfold COrd; omega) h

theorem reorderAcc_chain :
    ∀ (xs acc : List α), List.IsChain (fun u v => COrd k v u) acc →
      List.IsChain (COrd k) (reorderAcc k acc xs) := by
  intro xs
  induction xs with
  | nil => intro acc h; simpa [reorderAcc, List.isChain_reverse] using h
  | cons x xs ih => intro acc h; exact ih _ (insertRev_chain k x acc h)

/-- The output of the canonical ordering algorithm is canonically ordered. -/
theorem canonicalOrder_ordered (xs : List α) : List.IsChain (COrd k) (canonicalOrder k xs) :=
  reorderAcc_chain k xs [] (by simp)

theorem reorderAcc_of_ordered :
    ∀ (xs acc : List α), List.IsChain (COrd k) (acc.reverse ++ xs) →
      reorderAcc k acc xs = acc.reverse ++ xs := by
  intro xs
  induction xs with
  | nil => intro acc _; simp [reorderAcc]
  | cons x xs ih =>
    intro acc h
    have hins : insertRev k x acc = x :: acc := by
      cases acc with
      | nil => rfl
      | cons y ys =>
        have := (List.isChain_append.1 h).2.2 y (by simp) x (by simp)
        simp only [insertRev]
        rw [if_neg (by unfold COrd at this; omega)]
    simp only [reorderAcc, hins]
    rw [ih (x :: acc) (by simpa using h)]
    simp

/-- UN1 (ordering part): the canonical ordering algorithm is idempotent. -/
theorem canonicalOrder_idem (xs : List α) :
    canonicalOrder k (canonicalOrder k xs) = canonicalOrder k xs := by
  have := reorderAcc_of_ordered k (canonicalOrder k xs) [] (by simpa using canonicalOrder_ordered k xs)
  simpa [canonicalOrder] using this

end Ordering

/-! ## Examples (decided by the kernel) -/

def cafeNfc : String := "café"
def cafeNfd : String := "café"

/-- Canonically equivalent, distinct strings — the spec keeps them distinct (L3903-3909). -/
theorem cafe_distinct : cafeNfc ≠ cafeNfd := by decide
example : nfc cafeNfd = cafeNfc := by decide +kernel
example : nfd cafeNfc = cafeNfd := by decide +kernel

/-- `IX` vs `Ⅸ` (L3907-3909) is compatibility, not canonical, equivalence. -/
theorem nfc_roman_nine : nfc "Ⅸ" = "Ⅸ" := by decide +kernel

/-- Hangul: 한 = U+D55C ↔ ᄒ ᅡ ᆫ. -/
example : nfd "한" = "한" := by decide +kernel
example : nfc "한" = "한" := by decide +kernel

/-- Singletons: Ω OHM SIGN and Å ANGSTROM SIGN decompose and never recompose. -/
example : nfc "Ω" = "Ω" ∧ nfc "Å" = "Å" := by decide +kernel

/-- Canonical reordering: dot below (220) before acute-above (230), and
composition across a non-blocking mark: q + ̣ + ̇ stays; ṩ composes. -/
example : nfd "ṩ" = "ṩ" := by decide +kernel
example : nfc "ṩ" = "ṩ" := by decide +kernel

/-- Composition exclusion: U+0958 DEVANAGARI QA decomposes, never recomposes. -/
example : nfc "क़" = "क़" := by decide +kernel

/-- Leading non-starter is never composed onto. -/
example : nfc "́e" = "́e" := by decide +kernel

/-- UN1 on a corpus: NFC and NFD are idempotent, and `nfd ∘ nfc = nfd`. -/
def corpus : List String :=
  [ "", "abc", "café", "café", "Ångström", "Å", "Å", "Ω",
    "한국어", "한", "각", "각",
    "ṩ", "ẛ̣", "q̣̇", "क़", "́e", "é́",
    "ą́", "Ǻ", "Ⅸ", "xְֱ", "̈́", "ཱི", "_prim_ü" ]

theorem nfc_idem_corpus : corpus.all (fun s => nfc (nfc s) == nfc s) = true := by decide +kernel
theorem nfd_idem_corpus : corpus.all (fun s => nfd (nfd s) == nfd s) = true := by decide +kernel
theorem nfd_nfc_corpus : corpus.all (fun s => nfd (nfc s) == nfd s) = true := by decide +kernel

/-! ## UN2: XID identifier validity under NFC / NFD on examples

UAX #31 makes `XID_Start`/`XID_Continue` closed under NFKC, and with Python's
unicodedata 16.0 an exhaustive check over every code point `c` (as `c` and as
`a·c`) finds no valid identifier whose NFC/NFD/NFKC/NFKD is invalid. The
examples below check this with LightUSD's XID table (`UsdSpec.Unicode`). -/

def isIdent (s : String) : Bool :=
  match s.toList with
  | [] => false
  | c :: cs => (c == '_' || UsdSpec.Unicode.isXidStart c) && cs.all UsdSpec.Unicode.isXidContinue

def identCorpus : List String :=
  [ "café", "café", "Å", "Å", "Å", "Ω", "한", "한",
    "ṩ", "Ⅸ", "_́", "xक़", "Ǻ" ]

theorem ident_nfc_corpus :
    identCorpus.all (fun s => isIdent s == isIdent (nfc s) && isIdent s == isIdent (nfd s)) = true := by
  decide +kernel

/-- …and all of them are valid identifiers in both forms. -/
theorem ident_corpus_valid : identCorpus.all isIdent = true := by decide +kernel

/-- U+037A GREEK YPOGEGRAMMENI is excluded from XID because its NFKC form begins
with a space; it is not a valid identifier continuation (OpenUSD agrees). -/
theorem ypogegrammeni_not_ident : isIdent "aͺ" = false := by decide +kernel

end UsdSpec.Unicode.Norm
