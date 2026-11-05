import Mathlib.Data.List.Basic
import Mathlib.Tactic.IntervalCases
import UsdSpec.Path.Element
/-!
# Schema families and versions (AOUSD USD Core Specification v1.1.0, §11.2)

SPEC: core_spec.md L8240-8306 (Schema Families and Versioning).

The PEG (L8252-8270):
```
AsciiStart <- [A-Za-z_]
AsciiContinue <- [A-Za-z0-9_]
SchemaName <- Family ('_' Version)?
Family     <- (AsciiStart) (!('_' Version !AsciiContinue) AsciiContinue)*
Version    <- [0-9]+
```
`Family` consumes its first character unconditionally and then stops at the first
position whose remaining text is exactly `_` followed by digits to the end of the
name (the `!AsciiContinue` lookahead means "nothing follows", since a schema name is
made of `AsciiContinue` characters only). `parseFamilyVersion` implements exactly
this: the name is a valid family only if it is `AsciiStart AsciiContinue*`; a
trailing `_[0-9]+` with at least one character before the `_` is the version.
The spec's table (L8285-8294) is checked row by row in the kernel.

The version is returned as `Option Nat` rather than the task's `Nat` so that the
table's "(none)" rows are distinguishable from `Foo_0`; see the discussion of
version `0` and leading zeros (`Foo_01` parses to version `1` and re-prints as
`Foo_1`) in issues/aousd/02-schema-family-underscore-digit-name.md, which also
covers `_1` (an unversioned family `_1` under the PEG, an empty family under the
prose L8276-8283 and OpenUSD) and `Foo_1_2` (family `Foo_1`, which itself ends in
`_<digits>`, so "family never ends in `_<digits>`" is *not* a theorem of this
grammar). Those are already filed there and are not re-recorded as `SPEC-ISSUE`
tags here.
-/

namespace UsdSpec.Schema

/-- `AsciiStart <- [A-Za-z_]`. SPEC: L8252-8254. -/
def isAsciiStart (c : Char) : Bool := ('A' ≤ c && c ≤ 'Z') || ('a' ≤ c && c ≤ 'z') || c == '_'

/-- `AsciiContinue <- [A-Za-z0-9_]`. SPEC: L8256-8258. -/
def isAsciiContinue (c : Char) : Bool := isAsciiStart c || c.isDigit

/-- A syntactically valid schema name / family: `AsciiStart AsciiContinue*`. -/
def isSchemaNameList : List Char → Bool
  | [] => false
  | c :: cs => isAsciiStart c && cs.all isAsciiContinue

def isSchemaName (s : String) : Bool := isSchemaNameList s.toList

/-- Value of a *reversed* digit string (least significant digit first). -/
def valueRev : List Char → Nat
  | [] => 0
  | d :: ds => (d.toNat - '0'.toNat) + 10 * valueRev ds

/-- Split a reversed name into (reversed family, reversed version digits). The
version is present iff the name ends in `_[0-9]+` with a nonempty family before the
`_` (SPEC: L8264-8266, the `Family` rule always consumes its first character). -/
def splitVersionRev (rev : List Char) : List Char × Option (List Char) :=
  match rev.takeWhile Char.isDigit, rev.dropWhile Char.isDigit with
  | d :: ds, u :: f :: fs => if u == '_' then (f :: fs, some (d :: ds)) else (rev, none)
  | _, _ => (rev, none)

/-- Decompose a schema name into family and optional version. SPEC: L8252-8283.
`none` when the name is not `AsciiStart AsciiContinue*`. -/
def parseFamilyVersion (s : String) : Option (String × Option Nat) :=
  if isSchemaNameList s.toList then
    let (famRev, ver) := splitVersionRev s.toList.reverse
    some (String.ofList famRev.reverse, ver.map valueRev)
  else none

def digitChar : Nat → Char
  | 0 => '0' | 1 => '1' | 2 => '2' | 3 => '3' | 4 => '4'
  | 5 => '5' | 6 => '6' | 7 => '7' | 8 => '8' | _ => '9'

/-- Decimal digits of `n`, least significant first; `fuel > n` suffices. -/
def digitsRev : Nat → Nat → List Char
  | 0, _ => []
  | fuel + 1, n => if n < 10 then [digitChar n] else digitChar (n % 10) :: digitsRev fuel (n / 10)

/-- Canonical decimal text of `n` (no leading zeros). -/
def natChars (n : Nat) : List Char := (digitsRev (n + 1) n).reverse

/-- Print a family with an optional version: `Family` or `Family_Version`. SPEC: L8260-8262. -/
def printFamilyVersion (family : String) (version : Option Nat) : String :=
  match version with
  | none => family
  | some n => String.ofList (family.toList ++ '_' :: natChars n)

/-- "Within a schema family, a versioned schema must not inherit from another schema
in the same family." SPEC: L8300-8306. `true` when the inheritance is allowed by
this rule (unparseable names impose no constraint here). -/
def versionedMayInherit (child parent : String) : Bool :=
  match parseFamilyVersion child, parseFamilyVersion parent with
  | some (f, some _), some (g, _) => f != g
  | _, _ => true

/-! ## Worked examples: the decomposition table L8285-8294, decided in the kernel -/

example : parseFamilyVersion "Mesh" = some ("Mesh", none) := by decide +kernel
example : parseFamilyVersion "Cylinder_1" = some ("Cylinder", some 1) := by decide +kernel
example : parseFamilyVersion "GaussianSplatting_2" = some ("GaussianSplatting", some 2) := by decide +kernel
example : parseFamilyVersion "Gaussian_Splatting" = some ("Gaussian_Splatting", none) := by decide +kernel
example : parseFamilyVersion "Gaussian_Splatting_3" = some ("Gaussian_Splatting", some 3) := by decide +kernel
example : parseFamilyVersion "MySchema_" = some ("MySchema_", none) := by decide +kernel

/-- Cases from issues/aousd/02-schema-family-underscore-digit-name.md: `_1` is the
unversioned family `_1` under the PEG (OpenUSD: empty family, version 1);
`Foo_1_2` has family `Foo_1`; `Foo_0` is versioned with version 0; `Foo_01` loses
its leading zero. -/
example : parseFamilyVersion "_1" = some ("_1", none) := by decide +kernel
example : parseFamilyVersion "Foo_1_2" = some ("Foo_1", some 2) := by decide +kernel
example : parseFamilyVersion "Foo_0" = some ("Foo", some 0) := by decide +kernel
example : parseFamilyVersion "Foo_01" = some ("Foo", some 1) := by decide +kernel
example : printFamilyVersion "Foo" (some 1) = "Foo_1" := by decide +kernel
example : printFamilyVersion "Foo" none = "Foo" := by decide +kernel
example : printFamilyVersion "A" (some 4294967295) = "A_4294967295" := by decide +kernel
/-- Names outside the grammar. -/
example : parseFamilyVersion "" = none := by decide +kernel
example : parseFamilyVersion "1Mesh" = none := by decide +kernel
example : parseFamilyVersion "Mesh-1" = none := by decide +kernel
example : parseFamilyVersion "Mésh" = none := by decide +kernel

/-- SPEC: L8300-8306. `Cylinder_2` must not inherit from `Cylinder_1` or `Cylinder`,
but may inherit from `Gprim`; the rule only constrains *versioned* children. -/
example : versionedMayInherit "Cylinder_2" "Cylinder_1" = false := by decide +kernel
example : versionedMayInherit "Cylinder_2" "Cylinder" = false := by decide +kernel
example : versionedMayInherit "Cylinder_2" "Gprim" = true := by decide +kernel
example : versionedMayInherit "Cylinder" "Gprim" = true := by decide +kernel
example : versionedMayInherit "Cylinder" "Cylinder_1" = true := by decide +kernel
example : versionedMayInherit "Gaussian_Splatting_3" "Gaussian_Splatting" = false := by decide +kernel

/-! ## Theorems -/

theorem digitChar_isDigit (d : Nat) (hd : d < 10) : (digitChar d).isDigit = true := by
  interval_cases d <;> decide

theorem digitChar_val (d : Nat) (hd : d < 10) : (digitChar d).toNat - 48 = d := by
  interval_cases d <;> decide

theorem digitChar_not_underscore (d : Nat) (hd : d < 10) : (digitChar d == '_') = false := by
  interval_cases d <;> decide

theorem digitChar_asciiContinue (d : Nat) (hd : d < 10) : isAsciiContinue (digitChar d) = true := by
  interval_cases d <;> decide

theorem digitsRev_all_digit (fuel : Nat) : ∀ n, ∀ c ∈ digitsRev fuel n, c.isDigit = true := by
  induction fuel with
  | zero => intro n c hc; simp [digitsRev] at hc
  | succ fuel ih =>
    intro n c hc
    simp only [digitsRev] at hc
    split at hc
    · simp at hc; subst hc; exact digitChar_isDigit n ‹_›
    · simp at hc
      rcases hc with rfl | hc
      · exact digitChar_isDigit _ (Nat.mod_lt _ (by omega))
      · exact ih _ c hc

theorem digitsRev_ne_nil (fuel n : Nat) (h : 0 < fuel) : digitsRev fuel n ≠ [] := by
  cases fuel with
  | zero => omega
  | succ fuel => simp only [digitsRev]; split <;> simp

theorem valueRev_digitsRev (fuel : Nat) : ∀ n, n < fuel → valueRev (digitsRev fuel n) = n := by
  induction fuel with
  | zero => intro n h; omega
  | succ fuel ih =>
    intro n h
    simp only [digitsRev]
    split
    · simp only [valueRev, show '0'.toNat = 48 from rfl, Nat.mul_zero, Nat.add_zero]
      exact digitChar_val n ‹_›
    · have : n / 10 < fuel := by omega
      simp only [valueRev, ih (n / 10) this, show '0'.toNat = 48 from rfl,
        digitChar_val (n % 10) (Nat.mod_lt _ (by omega))]
      omega

theorem isAsciiContinue_of_isDigit {c : Char} (h : c.isDigit = true) : isAsciiContinue c = true := by
  simp [isAsciiContinue, h]

theorem isAsciiContinue_of_isAsciiStart {c : Char} (h : isAsciiStart c = true) : isAsciiContinue c = true := by
  simp [isAsciiContinue, h]

theorem isDigit_underscore : ('_'.isDigit) = false := by decide

theorem dropWhile_eq_nil_of_all {p : Char → Bool} {l : List Char} (h : ∀ a ∈ l, p a = true) :
    l.dropWhile p = [] := by
  induction l with
  | nil => rfl
  | cons a l ih => simp [h a (by simp), ih (fun b hb => h b (by simp [hb]))]

/-- The printed `Family_Version` is a valid schema name when the family is. -/
theorem isSchemaName_print (family : String) (hf : isSchemaName family = true) (n : Nat) :
    isSchemaName (printFamilyVersion family (some n)) = true := by
  unfold isSchemaName at *
  simp only [printFamilyVersion, String.toList_ofList]
  unfold isSchemaNameList at hf
  cases h : family.toList with
  | nil => simp [h] at hf
  | cons c cs =>
    rw [h] at hf
    simp only [Bool.and_eq_true, List.all_eq_true] at hf
    simp only [List.cons_append, isSchemaNameList, hf.1, Bool.true_and, List.all_eq_true,
      List.mem_append, List.mem_cons]
    intro x hx
    rcases hx with hx | rfl | hx
    · exact hf.2 x hx
    · decide
    · simp only [natChars, List.mem_reverse] at hx
      exact isAsciiContinue_of_isDigit (digitsRev_all_digit _ _ x hx)

/-- **Round trip (versioned).** For every valid family and every version,
`parseFamilyVersion (printFamilyVersion f (some n)) = some (f, some n)`.
No restriction on `f` beyond validity: a family like `Foo_1` followed by `_2`
decomposes back to (`Foo_1`, 2), as the PEG's "last `_digits`" rule dictates. -/
theorem parse_print_versioned (family : String) (hf : isSchemaName family = true) (n : Nat) :
    parseFamilyVersion (printFamilyVersion family (some n)) = some (family, some n) := by
  have hv := isSchemaName_print family hf n
  unfold isSchemaName at hv
  unfold parseFamilyVersion
  simp only [hv, ↓reduceIte]
  simp only [printFamilyVersion, String.toList_ofList, List.reverse_append, List.reverse_cons,
    List.append_assoc, List.singleton_append, natChars, List.reverse_reverse]
  unfold isSchemaName isSchemaNameList at hf
  obtain ⟨c, cs, h⟩ : ∃ c cs, family.toList = c :: cs := by
    cases h : family.toList with
    | nil => simp [h] at hf
    | cons c cs => exact ⟨c, cs, rfl⟩
  have hall : ∀ x ∈ digitsRev (n + 1) n, Char.isDigit x = true := digitsRev_all_digit _ _
  have hne := digitsRev_ne_nil (n + 1) n (by omega)
  have htake : (digitsRev (n + 1) n ++ '_' :: family.toList.reverse).takeWhile Char.isDigit
      = digitsRev (n + 1) n := by
    rw [List.takeWhile_append_of_pos hall]; simp [isDigit_underscore]
  have hdrop : (digitsRev (n + 1) n ++ '_' :: family.toList.reverse).dropWhile Char.isDigit
      = '_' :: family.toList.reverse := by
    rw [List.dropWhile_append]
    have : (digitsRev (n + 1) n).dropWhile Char.isDigit = [] := dropWhile_eq_nil_of_all hall
    simp [this, isDigit_underscore]
  simp only [splitVersionRev, htake, hdrop]
  rw [h, List.reverse_cons]
  obtain ⟨d, ds, hd⟩ : ∃ d ds, digitsRev (n + 1) n = d :: ds := by
    cases hd : digitsRev (n + 1) n with
    | nil => exact absurd hd hne
    | cons d ds => exact ⟨d, ds, rfl⟩
  obtain ⟨f, fs, hf'⟩ : ∃ f fs, cs.reverse ++ [c] = f :: fs := by
    cases hc : cs.reverse ++ [c] with
    | nil => simp at hc
    | cons f fs => exact ⟨f, fs, rfl⟩
  rw [hd, hf']
  simp only [beq_self_eq_true, ↓reduceIte, Option.map_some, Option.some.injEq, Prod.mk.injEq]
  refine ⟨?_, ?_⟩
  · rw [← hf', ← List.reverse_cons, List.reverse_reverse, ← h, String.ofList_toList]
  · rw [← hd, valueRev_digitsRev (n + 1) n (by omega)]

/-- The family of any successful parse is itself a valid schema name
(`AsciiStart AsciiContinue*`, hence also a path-grammar `Identifier` since every
ASCII letter is `XID_Start` and every ASCII digit is `XID_Continue`). -/
theorem family_isSchemaName (s f : String) (v : Option Nat) (h : parseFamilyVersion s = some (f, v)) :
    isSchemaName f = true := by
  unfold parseFamilyVersion at h
  split at h
  · rename_i hs
    simp only [Option.some.injEq, Prod.mk.injEq] at h
    obtain ⟨hf, _⟩ := h
    subst hf
    unfold isSchemaName
    rw [String.toList_ofList]
    unfold isSchemaNameList at hs
    cases hl : s.toList with
    | nil => simp [hl] at hs
    | cons c cs =>
      rw [hl] at hs
      simp only [Bool.and_eq_true, List.all_eq_true] at hs
      -- the family is a prefix of `c :: cs`, computed through the reversed split
      unfold splitVersionRev
      simp only [List.reverse_cons]
      have hwhole : isSchemaNameList (cs.reverse ++ [c]).reverse = true := by
        simp only [List.reverse_append, List.reverse_reverse, List.reverse_cons, List.reverse_nil,
          List.nil_append, List.singleton_append, isSchemaNameList, Bool.and_eq_true, List.all_eq_true]
        exact hs
      split
      · rename_i d ds u f fs hT hD
        split
        · have h1 : u :: f :: fs <:+ cs.reverse ++ [c] := by
            have := List.dropWhile_suffix (p := Char.isDigit) (l := cs.reverse ++ [c])
            rwa [hD] at this
          have h2 : f :: fs <:+ cs.reverse ++ [c] := List.IsSuffix.trans ⟨[u], rfl⟩ h1
          have h3 : (f :: fs).reverse <+: (cs.reverse ++ [c]).reverse := List.reverse_prefix.mpr h2
          simp only [List.reverse_append, List.reverse_reverse, List.reverse_cons, List.reverse_nil,
            List.nil_append, List.singleton_append] at h3
          obtain ⟨t, ht⟩ := h3
          cases hr : (fs.reverse ++ [f]) with
          | nil => simp at hr
          | cons g gs =>
            rw [hr] at ht
            simp only [List.cons_append, List.cons.injEq] at ht
            obtain ⟨rfl, ht⟩ := ht
            rw [List.reverse_cons, hr]
            simp only [isSchemaNameList, hs.1, Bool.true_and, List.all_eq_true]
            intro x hx
            exact hs.2 x (by rw [← ht]; simp [hx])
        · exact hwhole
      · exact hwhole
  · exact absurd h (by simp)

/-- Unversioned round trip: a name that parses as an unversioned family prints to
itself (`printFamilyVersion f none = f` definitionally). -/
theorem parse_print_unversioned (f : String) (hu : parseFamilyVersion f = some (f, none)) :
    parseFamilyVersion (printFamilyVersion f none) = some (f, none) := hu

/-- Two versions of one family may not inherit from each other. SPEC: L8300-8306. -/
theorem versionedMayInherit_same_family (f : String) (hf : isSchemaName f = true) (n m : Nat) :
    versionedMayInherit (printFamilyVersion f (some n)) (printFamilyVersion f (some m)) = false := by
  unfold versionedMayInherit
  rw [parse_print_versioned f hf n, parse_print_versioned f hf m]
  simp

/-- A versioned schema may not inherit from the unversioned schema of its family.
SPEC: L8303-8306 (`Cylinder_2` must not inherit from `Cylinder`). -/
theorem versionedMayInherit_unversioned_parent (f : String) (hf : isSchemaName f = true)
    (hu : parseFamilyVersion f = some (f, none)) (n : Nat) :
    versionedMayInherit (printFamilyVersion f (some n)) f = false := by
  unfold versionedMayInherit
  rw [parse_print_versioned f hf n, hu]
  simp

/-- The rule never constrains an unversioned child. -/
theorem versionedMayInherit_of_unversioned (c p f : String) (h : parseFamilyVersion c = some (f, none)) :
    versionedMayInherit c p = true := by
  unfold versionedMayInherit
  rw [h]

end UsdSpec.Schema
