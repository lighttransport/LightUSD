import Mathlib.Data.List.Basic
import UsdSpec.Path.Element
/-!
# Element ordering (AOUSD USD Core Specification v1.1.0, §8.2)

SPEC: core_spec.md L3513-3570 (8.2 Element Ordering).

`elemCompare` is the normative "dictionary" order on element names (L3515-3530):

* `_` before all ASCII letters and digits (L3517);
* ASCII letters compared case-folded (L3518-3519), a case-only difference putting
  upper case first (L3520-3521);
* ASCII digits before ASCII letters (L3522);
* maximal digit runs compared numerically, leading zeros skipped (L3523-3524), with
  the leading-zero count as a tie-break (L3525-3528);
* everything else (non-ASCII) by code point (L3529-3530).

The implementation tokenizes a name into characters and digit runs, maps each
token to a *primary* key (what the bullets compare first) and a *secondary* key
(the case / leading-zero tie-break), and compares lexicographically on the primary
key sequence, then on the secondary key sequence. Both tie-breaks are therefore
deferred to the end of the string, exactly as OpenUSD's `TfDictionaryLessThan`
(pxr/base/tf/stringUtils.cpp, `_LessImpl`: "End of both -- use the first mismatch
to discriminate") does, and as the spec's own worked example requires
(`LEAN-201`). Because the result is a lexicographic order on key sequences,
reflexivity, antisymmetry and transitivity are all proved below.

SPEC-ISSUE: LEAN-201 — Leading-zero tie-break: prose vs worked example.
The prose (L3525-3528) says that when two digit groups are numerically equal
"the group with the lower number of leading '0' values shall be ordered first",
which reads as an immediate decision at that group. The worked example
(L3532-3557) lists `foo00001bar` (four leading zeros) *before*
`foo001bar001abc` (two leading zeros): the immediate reading would order
`foo001bar001abc` first. The example is only consistent with deferring the
leading-zero tie-break until the rest of the strings compare equal (as
`TfDictionaryLessThan` does; `a1b` vs `a01a` sorts `a01a` first there). This
file implements the deferred semantics; `example_leadingZero_deferred` below
pins the distinguishing case.

SPEC-ISSUE: LEAN-204 — The worked example's sorted list (L3545-3557) contradicts
the prose rule "ASCII numbers shall be ordered before ASCII letters" (L3522): it
places `foobar` (L3552) before `foo00001bar` (L3553), although at the first
difference the digit `0` meets the letter `b`, and it places `Foobar`/`foobar`
after `foo_bar` but before all `foo0…` names. Under the prose rules — and under
OpenUSD's `TfDictionaryLessThan`, checked against OpenUSD 26.x — the sorted
order is
`_foobar a0 ab aü foo_bar foo00001bar foo001bar001abc foo001bar002abc
foo0001bar0002xyz Foobar foobar`
(OpenUSD additionally moves `foo_bar` after the `foo0…` names, see LEAN-205).
No per-character rule can produce the listed order, since the same list also
puts `a0` before `ab` (digit before letter). `example_spec_list_not_sorted` shows
that the listed order is not sorted under the prose rules, and
`example_sorted` gives the order they do produce.

SPEC-ISSUE: LEAN-205 — `_` versus digits: spec vs OpenUSD. L3517 orders `_`
before all ASCII letters *and numbers*; `TfDictionaryLessThan` only applies its
"underscore first" trick when both bytes are in the letter zone (`>= 0x40`),
so in OpenUSD a digit sorts before `_` (`0a < _a`, `foo00001bar < foo_bar`).
This file follows the spec (`example_underscore_before_digit`). Implementations
that want OpenUSD-identical child ordering must deviate from L3517.
-/

namespace UsdSpec.ElemOrder

/-- A token of a name: a single non-digit code point, or a maximal run of ASCII
digits with its numeric value and its count of leading zeros. SPEC: L3523-3528. -/
inductive Token
  | chr (c : Char)
  | num (value zeros : Nat)
  deriving DecidableEq, Repr

/-- Fold one digit into a (value, leading-zero count) accumulator. -/
def digitStep (v z : Nat) (c : Char) : Nat × Nat :=
  let d := c.toNat - '0'.toNat
  if v == 0 && d == 0 then (0, z + 1) else (10 * v + d, z)

def tokenizeAux : List Char → Option (Nat × Nat) → List Token
  | [], none => []
  | [], some (v, z) => [.num v z]
  | c :: cs, none =>
    if c.isDigit then tokenizeAux cs (some (digitStep 0 0 c)) else .chr c :: tokenizeAux cs none
  | c :: cs, some (v, z) =>
    if c.isDigit then tokenizeAux cs (some (digitStep v z c))
    else .num v z :: .chr c :: tokenizeAux cs none

/-- Split a name into characters and maximal digit runs. -/
def tokenize (cs : List Char) : List Token := tokenizeAux cs none

def isAsciiLetter (c : Char) : Bool := ('A' ≤ c && c ≤ 'Z') || ('a' ≤ c && c ≤ 'z')

/-- Primary key of a token, as two naturals compared lexicographically:
`_` ↦ `(0,0)` (L3517); digit run ↦ `(1, value)` (L3522-3524); any other code
point ↦ `(2, case-folded code point)` (L3518-3519, L3529-3530). -/
def primary : Token → List Nat
  | .chr c => if c == '_' then [0, 0] else [2, if isAsciiLetter c then c.toLower.toNat else c.toNat]
  | .num v _ => [1, v]

/-- Secondary (tie-break) key: the raw code point (upper case first, L3520-3521) or
the leading-zero count (fewer first, L3525-3528). -/
def secondary : Token → Nat
  | .chr c => c.toNat
  | .num _ z => z

def primaryKeys (cs : List Char) : List Nat := (tokenize cs).flatMap primary
def secondaryKeys (cs : List Char) : List Nat := (tokenize cs).map secondary

/-- Lexicographic comparison of key sequences; a proper prefix is smaller. -/
def lexCompare : List Nat → List Nat → Ordering
  | [], [] => .eq
  | [], _ :: _ => .lt
  | _ :: _, [] => .gt
  | a :: as, b :: bs => (compare a b).then (lexCompare as bs)

/-- Element-name comparison on code points. SPEC: L3515-3530. -/
def elemCompareList (a b : List Char) : Ordering :=
  (lexCompare (primaryKeys a) (primaryKeys b)).then (lexCompare (secondaryKeys a) (secondaryKeys b))

/-- Element-name comparison. SPEC: L3515-3530. -/
def elemCompare (a b : String) : Ordering := elemCompareList a.toList b.toList

/-- Kind rank of a child element: properties before prims before variant sets.
SPEC: L3559-3561. -/
def kindRank : PathElement → Nat
  | .prop _ _ => 0
  | .prim _ => 1
  | .variant _ _ => 2

/-- Ordering of sibling children: by kind (L3559-3561), then by name; variant
selections by set name, then variant name. SPEC: L3559-3570. -/
def childCompare (a b : PathElement) : Ordering :=
  (compare (kindRank a) (kindRank b)).then
    (match a, b with
      | .variant s n, .variant s' n' => (elemCompare s s').then (elemCompare n n')
      | a, b => elemCompare a.name b.name)

/-! ## Theorems: `lexCompare` is a linear order on key sequences -/

theorem lexCompare_self (l : List Nat) : lexCompare l l = .eq := by
  induction l with
  | nil => rfl
  | cons a as ih => simp [lexCompare, ih]

theorem lexCompare_swap (l m : List Nat) : lexCompare m l = (lexCompare l m).swap := by
  induction l generalizing m with
  | nil => cases m <;> rfl
  | cons a as ih =>
    cases m with
    | nil => rfl
    | cons b bs =>
      show (compare b a).then (lexCompare bs as) = ((compare a b).then (lexCompare as bs)).swap
      rw [Ordering.swap_then, Nat.compare_swap, ih]

theorem lexCompare_eq_iff (l m : List Nat) : lexCompare l m = .eq ↔ l = m := by
  induction l generalizing m with
  | nil => cases m <;> simp [lexCompare]
  | cons a as ih =>
    cases m with
    | nil => simp [lexCompare]
    | cons b bs => simp [lexCompare, Ordering.then_eq_eq, ih]

theorem lexCompare_lt_trans {l m n : List Nat} (h₁ : lexCompare l m = .lt)
    (h₂ : lexCompare m n = .lt) : lexCompare l n = .lt := by
  induction l generalizing m n with
  | nil =>
    cases m with
    | nil => simp [lexCompare] at h₁
    | cons b bs => cases n with
      | nil => simp [lexCompare] at h₂
      | cons c cs => rfl
  | cons a as ih =>
    cases m with
    | nil => simp [lexCompare] at h₁
    | cons b bs =>
      cases n with
      | nil => simp [lexCompare] at h₂
      | cons c cs =>
        simp only [lexCompare, Ordering.then_eq_lt, Nat.compare_eq_lt, Nat.compare_eq_eq] at h₁ h₂ ⊢
        rcases h₁ with h₁ | ⟨rfl, h₁⟩ <;> rcases h₂ with h₂ | ⟨rfl, h₂⟩
        · exact Or.inl (Nat.lt_trans h₁ h₂)
        · exact Or.inl h₁
        · exact Or.inl h₂
        · exact Or.inr ⟨rfl, ih h₁ h₂⟩

/-! ## Theorems: `elemCompare` -/

theorem elemCompareList_refl (a : List Char) : elemCompareList a a = .eq := by
  simp [elemCompareList, lexCompare_self]

theorem elemCompareList_swap (a b : List Char) :
    elemCompareList b a = (elemCompareList a b).swap := by
  unfold elemCompareList
  rw [Ordering.swap_then, lexCompare_swap (primaryKeys a), lexCompare_swap (secondaryKeys a)]

theorem elemCompareList_lt_trans {a b c : List Char} (h₁ : elemCompareList a b = .lt)
    (h₂ : elemCompareList b c = .lt) : elemCompareList a c = .lt := by
  unfold elemCompareList at *
  rw [Ordering.then_eq_lt] at h₁ h₂ ⊢
  rcases h₁ with h₁ | ⟨h₁, h₁'⟩ <;> rcases h₂ with h₂ | ⟨h₂, h₂'⟩
  · exact Or.inl (lexCompare_lt_trans h₁ h₂)
  · rw [lexCompare_eq_iff] at h₂; rw [h₂] at h₁; exact Or.inl h₁
  · rw [lexCompare_eq_iff] at h₁; rw [← h₁] at h₂; exact Or.inl h₂
  · rw [lexCompare_eq_iff] at h₁ h₂
    exact Or.inr ⟨by rw [lexCompare_eq_iff, h₁, h₂], lexCompare_lt_trans h₁' h₂'⟩

/-- `elemCompare a a = .eq`. -/
theorem elemCompare_refl (a : String) : elemCompare a a = .eq := elemCompareList_refl _

/-- Swapping the arguments swaps the result. -/
theorem elemCompare_swap_eq (a b : String) : elemCompare b a = (elemCompare a b).swap :=
  elemCompareList_swap _ _

/-- Antisymmetry: `a < b ↔ b > a`. -/
theorem elemCompare_swap (a b : String) : elemCompare a b = .lt ↔ elemCompare b a = .gt := by
  rw [elemCompare_swap_eq]; cases elemCompare a b <;> simp

/-- Transitivity of the strict order. -/
theorem elemCompare_trans {a b c : String} (h₁ : elemCompare a b = .lt)
    (h₂ : elemCompare b c = .lt) : elemCompare a c = .lt :=
  elemCompareList_lt_trans h₁ h₂

/-- Totality: exactly one of `lt`, `eq`, `gt` holds, and `eq` is symmetric. -/
theorem elemCompare_eq_symm (a b : String) : elemCompare a b = .eq ↔ elemCompare b a = .eq := by
  rw [elemCompare_swap_eq]; cases elemCompare a b <;> simp

/-- `eq` is a congruence for the order (needed for sorting stability arguments). -/
theorem elemCompare_eq_trans {a b c : String} (h₁ : elemCompare a b = .eq)
    (h₂ : elemCompare b c = .eq) : elemCompare a c = .eq := by
  unfold elemCompare elemCompareList at *
  rw [Ordering.then_eq_eq, lexCompare_eq_iff, lexCompare_eq_iff] at *
  exact ⟨h₁.1.trans h₂.1, h₁.2.trans h₂.2⟩

/-- `elemCompare a b = .eq` exactly when the key sequences agree. Injectivity of the
keys back to the string (so that `.eq ↔ a = b`) is not proved here; it needs
`detokenize ∘ tokenize = id`, which is left as a TODO. -/
theorem elemCompare_eq_iff_keys (a b : String) :
    elemCompare a b = .eq ↔ primaryKeys a.toList = primaryKeys b.toList
      ∧ secondaryKeys a.toList = secondaryKeys b.toList := by
  unfold elemCompare elemCompareList
  rw [Ordering.then_eq_eq, lexCompare_eq_iff, lexCompare_eq_iff]

theorem childCompare_refl (e : PathElement) : childCompare e e = .eq := by
  cases e <;> simp [childCompare, elemCompare_refl]

/-! ## The worked example (SPEC: L3532-3557) -/

/-- The input list of L3534-3544. -/
def exampleNames : List String :=
  ["foobar", "Foobar", "_foobar", "foo_bar", "foo001bar001abc", "foo001bar002abc",
   "foo0001bar0002xyz", "foo00001bar", "a0", "aü", "ab"]

/-- The order the spec lists at L3547-3557 (not sorted under its own rules, LEAN-204). -/
def specListedOrder : List String :=
  ["_foobar", "a0", "ab", "aü", "foo_bar", "Foobar", "foobar", "foo00001bar",
   "foo001bar001abc", "foo001bar002abc", "foo0001bar0002xyz"]

/-- The order the prose rules (L3515-3530) produce. -/
def derivedOrder : List String :=
  ["_foobar", "a0", "ab", "aü", "foo_bar", "foo00001bar", "foo001bar001abc",
   "foo001bar002abc", "foo0001bar0002xyz", "Foobar", "foobar"]

def isSorted (l : List String) : Bool := l.Pairwise (fun a b => elemCompare a b = .lt)

/-- Structural insertion sort (kernel-reducible, unlike the well-founded `List.mergeSort`). -/
def insertSorted (x : String) : List String → List String
  | [] => [x]
  | y :: ys => if (elemCompare x y).isLE then x :: y :: ys else y :: insertSorted x ys

def sortNames : List String → List String
  | [] => []
  | x :: xs => insertSorted x (sortNames xs)

/-- Sorting the spec's input with `elemCompare` gives `derivedOrder`. -/
theorem example_sorted : sortNames exampleNames = derivedOrder := by decide +kernel

/-- The same with the library sort (`native_decide`: `List.mergeSort` is defined by
well-founded recursion and does not reduce in the kernel). -/
theorem example_sorted_mergeSort :
    exampleNames.mergeSort (fun a b => (elemCompare a b).isLE) = derivedOrder := by
  native_decide

/-- The spec's listed order is not sorted under its own prose rules: the adjacent
pair `foobar`, `foo00001bar` (L3552-3553) is inverted (digit before letter, L3522). -/
theorem example_spec_list_not_sorted :
    isSorted specListedOrder = false ∧ elemCompare "foo00001bar" "foobar" = .lt := by
  decide +kernel

/-- Every adjacent pair of `derivedOrder` is strictly increasing, and `derivedOrder` is a
permutation of the input. -/
theorem example_derived_sorted : isSorted derivedOrder = true ∧ derivedOrder.Perm exampleNames := by
  refine ⟨by decide +kernel, ?_⟩
  decide +kernel

/-- Rows that the prose and the listed order agree on (L3517-3521). -/
example : elemCompare "_foobar" "a0" = .lt := by decide +kernel
example : elemCompare "a0" "ab" = .lt := by decide +kernel
example : elemCompare "ab" "aü" = .lt := by decide +kernel
example : elemCompare "foo_bar" "Foobar" = .lt := by decide +kernel
example : elemCompare "Foobar" "foobar" = .lt := by decide +kernel
example : elemCompare "foo001bar001abc" "foo001bar002abc" = .lt := by decide +kernel
example : elemCompare "foo001bar002abc" "foo0001bar0002xyz" = .lt := by decide +kernel
/-- LEAN-201: `foo00001bar` before `foo001bar001abc` as listed (L3553-3554), which
only the deferred tie-break produces. -/
example : elemCompare "foo00001bar" "foo001bar001abc" = .lt := by decide +kernel

/-- LEAN-201 distinguishing case: immediate tie-break would put `a1b` first; the
deferred one (spec example, OpenUSD) puts `a01a` first. Equal digit runs with a
pure leading-zero difference still order fewer zeros first (L3525-3528). -/
theorem example_leadingZero_deferred :
    elemCompare "a01a" "a1b" = .lt ∧ elemCompare "a1" "a01" = .lt ∧ elemCompare "a01" "a1" = .gt := by
  decide +kernel

/-- LEAN-205: `_` before digits per L3517 (OpenUSD orders the digit first). -/
theorem example_underscore_before_digit :
    elemCompare "_a" "0a" = .lt ∧ elemCompare "foo_bar" "foo00001bar" = .lt := by decide +kernel

/-- Digit runs compare numerically, not textually (L3523-3524). -/
example : elemCompare "a9" "a10" = .lt := by decide +kernel
example : elemCompare "a010" "a9" = .gt := by decide +kernel

/-- Children of different forms: `/a.b` < `/a/b` < `/a{b=}`. SPEC: L3563-3570. -/
example : childCompare (.prop [] "b") (.prim "b") = .lt
    ∧ childCompare (.prim "b") (.variant "b" "") = .lt
    ∧ childCompare (.prop [] "b") (.variant "b" "") = .lt := by decide +kernel

end UsdSpec.ElemOrder
