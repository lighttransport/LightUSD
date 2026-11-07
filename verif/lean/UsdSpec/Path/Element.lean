import Mathlib.Data.List.Basic
import UsdSpec.Unicode.XidTable
/-!
# Path elements and paths (AOUSD USD Core Specification v1.1.0, ch. 8)

SPEC: core_spec.md L3319-3511 (8.1 Introduction), L1805-1837 (6.9 Names, identifier
grammar), L3617-3647 (8.4 Grammar Definition, identifier classes).

A path is an absolute/relative flag, a count of leading parent specifiers (`..`,
L3375-3383: only allowed as the starting elements), and a list of elements
(prim names, a terminal property name with optional namespaces, variant
selections). The spec's legacy mapper/expression/relational-attribute elements
(L3809-3892) are deliberately not modelled: the spec itself leaves their
semantics unspecified (L3814-3817).

All string processing is done on `List Char` (Unicode code points); the spec says
identifier equivalence is code-point equivalence (L3631-3637). Executable
definitions are `Bool`-valued; `Prop` forms are `… = true`. Concrete facts about
the XID tables are decided with `decide +kernel` (the elaborator's `decide` hits
its recursion limit on the ~700-row tables; the kernel evaluates them directly).

Spec issues found while formalizing are tagged `SPEC-ISSUE: LEAN-NNN`.
-/

namespace UsdSpec

open UsdSpec.Unicode

/-! ## Identifiers (SPEC: L1805-1837, L3650-3670) -/

/-- `IdentifierStart <- [XID_Start] / Underscore`. SPEC: L3660-3663. -/
def isIdentifierStart (c : Char) : Bool := c == '_' || isXidStart c

/-- `IdentifierContinue <- [XID_Continue]`. SPEC: L3665-3667. -/
def isIdentifierContinue (c : Char) : Bool := isXidContinue c

/-- `Identifier <- IdentifierStart (IdentifierContinue)*` on code points. SPEC: L3669-3671,
also `PrimName` (L1820-1822) and `VariantSetName` (L1824-1826). -/
def isIdentifierList : List Char → Bool
  | [] => false
  | c :: cs => isIdentifierStart c && cs.all isIdentifierContinue

def isIdentifier (s : String) : Bool := isIdentifierList s.toList

/-- `VariantContinue <- IdentifierContinue / VerticalBar / Hyphen`. SPEC: L3681-3685. -/
def isVariantContinue (c : Char) : Bool := isXidContinue c || c == '|' || c == '-'

/-- `VariantName <- (Dot (VariantContinue)*) / (VariantContinue)+`. SPEC: L3687-3690
(and L1828-1830). A variant name may start with a digit (L3743-3748). -/
def isVariantNameList : List Char → Bool
  | [] => false
  | c :: cs => if c == '.' then cs.all isVariantContinue else (c :: cs).all isVariantContinue

def isVariantName (s : String) : Bool := isVariantNameList s.toList

/-- Split on a separator code point (never produces an empty list; `[]` ↦ `[[]]`).
Used instead of `String.splitOn`, which does not reduce in the kernel. -/
def splitOnChar (sep : Char) : List Char → List (List Char)
  | [] => [[]]
  | c :: cs =>
    match splitOnChar sep cs with
    | [] => [[]]
    | part :: parts => if c == sep then [] :: part :: parts else (c :: part) :: parts

/-- `PropertyName <- Identifier (Colon Identifier)*`. SPEC: L3719-3721 (and L1832-1834).
Every `:`-separated part is an identifier, so the separator is medial and never
repeated (L3804-3807 rejected examples). -/
def isPropertyNameList (cs : List Char) : Bool :=
  (splitOnChar ':' cs).all isIdentifierList

def isPropertyName (s : String) : Bool := isPropertyNameList s.toList

/-! ## Elements -/

/-- One element of a path. SPEC: L3319-3511.
* `prim name`: a prim (spec) name, L3330-3332.
* `prop namespaces base`: a property name `ns₁:…:nsₖ:base`, L3335-3337, L3719-3721.
* `variant set name`: a variant selection `{set=name}`, L3459-3465; `name = ""`
  selects the variant set spec itself, L3503-3506. -/
inductive PathElement
  | prim (name : String)
  | prop (namespaces : List String) (base : String)
  | variant (set : String) (name : String)
  deriving DecidableEq, Repr, Inhabited

namespace PathElement

def isPrim : PathElement → Bool
  | .prim _ => true
  | _ => false

def isProp : PathElement → Bool
  | .prop _ _ => true
  | _ => false

def isVariant : PathElement → Bool
  | .variant _ _ => true
  | _ => false

/-- The full property name with namespaces joined by `:`. SPEC: L3719-3721. -/
def propFullName (namespaces : List String) (base : String) : String :=
  ":".intercalate (namespaces ++ [base])

/-- The element's own text as it appears in a path (no separator). -/
def text : PathElement → String
  | .prim n => n
  | .prop ns b => propFullName ns b
  | .variant s n => "{" ++ s ++ "=" ++ n ++ "}"

/-- The element's name: prim name, full property name, or the variant *set* name
for a variant selection. -/
def name : PathElement → String
  | .prim n => n
  | .prop ns b => propFullName ns b
  | .variant s _ => s

/-- Element-level well-formedness: every name obeys the grammar of its spec form
(SPEC: L1805-1837). -/
def isWellFormed : PathElement → Bool
  | .prim n => isIdentifier n
  | .prop ns b => ns.all isIdentifier && isIdentifier b
  | .variant s n => isIdentifier s && (n == "" || isVariantName n)

end PathElement

/-! ## Paths -/

/-- A path. SPEC: L3346-3383.
* `absolute = true`: rooted at the absolute root `/` (L3351-3353); then `parents = 0`.
* `parents`: number of leading `..` parent specifiers (L3361-3364, L3375-3379).
* `elems`: the elements. `⟨false, 0, []⟩` is the reflexive relative path `.`
  (L3365-3367); `⟨true, 0, []⟩` is the absolute root path. -/
structure Path where
  absolute : Bool
  parents : Nat
  elems : List PathElement
  deriving DecidableEq, Repr, Inhabited

namespace Path

/-- The absolute root path `/`. SPEC: L3351-3353, L3778. -/
def absoluteRoot : Path := ⟨true, 0, []⟩

/-- The reflexive relative path `.`. SPEC: L3365-3367, L3779. -/
def reflexiveRelative : Path := ⟨false, 0, []⟩

def isAbsolute (p : Path) : Bool := p.absolute
def isAbsoluteRootPath (p : Path) : Bool := p.absolute && p.parents == 0 && p.elems.isEmpty

/-- The last element, if any. -/
def lastElem (p : Path) : Option PathElement := p.elems.getLast?

/-- A prim path ends in a prim name. SPEC: L3339-3344 ("prim path"). The absolute
root is not a prim path (it is the pseudo-root, L3778). -/
def isPrimPath (p : Path) : Bool :=
  match p.lastElem with
  | some (.prim _) => true
  | _ => false

/-- A property path ends in a property name. SPEC: L3339-3344, L3424-3425
("a property is a terminal leaf in a path"). -/
def isPropertyPath (p : Path) : Bool :=
  match p.lastElem with
  | some (.prop _ _) => true
  | _ => false

/-- A path ending in a variant selection (addresses a variant spec, or the variant
set spec when the selection is empty). SPEC: L3459-3465, L3503-3506. -/
def isVariantSelectionPath (p : Path) : Bool :=
  match p.lastElem with
  | some (.variant _ _) => true
  | _ => false

/-- The name of the last element (`""` for `/`, `.` and `..`-only paths). -/
def name (p : Path) : String :=
  match p.lastElem with
  | some e => e.name
  | none => ""

/-- The parent path. `/` has no parent (nothing may address ancestors of the
absolute root, L3368-3374); the parent of a relative path with no elements adds
one more `..` (L3361-3364); otherwise the last element is dropped. -/
def parent (p : Path) : Option Path :=
  match p.elems with
  | [] => if p.absolute then none else some ⟨false, p.parents + 1, []⟩
  | _ :: _ => some { p with elems := p.elems.dropLast }

/-- Append a child prim name. SPEC: L3385-3388. -/
def appendChild (p : Path) (n : String) : Path := { p with elems := p.elems ++ [.prim n] }

/-- Append a property (with namespaces). SPEC: L3421-3425. -/
def appendProperty (p : Path) (namespaces : List String) (base : String) : Path :=
  { p with elems := p.elems ++ [.prop namespaces base] }

/-- Append a variant selection `{set=name}`. SPEC: L3459-3465. -/
def appendVariantSelection (p : Path) (set name : String) : Path :=
  { p with elems := p.elems ++ [.variant set name] }

/-- `p` is a prefix of `q`: same rooting, and `p`'s elements are a prefix of `q`'s.
Needed by namespace mappings (spec ch. 10). -/
def isPrefixOf (p q : Path) : Bool :=
  p.absolute == q.absolute && p.parents == q.parents && p.elems.isPrefixOf q.elems

/-- Propositional form of `isPrefixOf`. -/
def HasPrefix (p q : Path) : Prop :=
  p.absolute = q.absolute ∧ p.parents = q.parents ∧ p.elems <+: q.elems

instance : DecidableRel HasPrefix := fun _ _ => by unfold HasPrefix; infer_instance

/-! ### Well-formedness (what the grammar of L3617-3737 can produce) -/

/-- Shape of the element list after a leading prim: prims and variant selections in
any order, optionally terminated by exactly one property (L3424-3425). -/
def primTailShape : List PathElement → Bool
  | [] => true
  | .prim _ :: rest => primTailShape rest
  | .variant _ _ :: rest => primTailShape rest
  | [.prop _ _] => true
  | .prop _ _ :: _ :: _ => false

/-- Shape of a whole element list: empty, a lone property (relative paths only,
`.prop`, L3367-3369 / L3780), or prim-first (`PrimElements` starts with
`PrimName`, L3712-3717). -/
def elemsShape (absolute : Bool) : List PathElement → Bool
  | [] => true
  | [.prop _ _] => !absolute
  | .prim _ :: rest => primTailShape rest
  | .prop _ _ :: _ :: _ => false
  | .variant _ _ :: _ => false

/-- Executable well-formedness: absolute paths have no `..` (L3368-3374); the element
shape is grammatical; every element's name is valid. -/
def isWellFormed (p : Path) : Bool :=
  (!p.absolute || p.parents == 0) && elemsShape p.absolute p.elems
    && p.elems.all PathElement.isWellFormed

def WellFormed (p : Path) : Prop := p.isWellFormed = true

instance : DecidablePred WellFormed := fun _ => by unfold WellFormed; infer_instance

/-! ## Theorems -/

theorem isPrefixOf_iff (p q : Path) : isPrefixOf p q = true ↔ HasPrefix p q := by
  simp [isPrefixOf, HasPrefix, List.isPrefixOf_iff_prefix, and_assoc]

theorem hasPrefix_refl (p : Path) : HasPrefix p p := ⟨rfl, rfl, List.prefix_refl _⟩

theorem hasPrefix_trans {p q r : Path} (h₁ : HasPrefix p q) (h₂ : HasPrefix q r) :
    HasPrefix p r :=
  ⟨h₁.1.trans h₂.1, h₁.2.1.trans h₂.2.1, h₁.2.2.trans h₂.2.2⟩

theorem hasPrefix_antisymm {p q : Path} (h₁ : HasPrefix p q) (h₂ : HasPrefix q p) : p = q := by
  obtain ⟨a, b, c⟩ := h₁
  obtain ⟨_, _, c'⟩ := h₂
  have : p.elems = q.elems := c.eq_of_length (Nat.le_antisymm c.length_le c'.length_le)
  cases p; cases q
  simp_all

theorem isPrefixOf_refl (p : Path) : isPrefixOf p p = true :=
  (isPrefixOf_iff p p).mpr (hasPrefix_refl p)

theorem isPrefixOf_trans {p q r : Path} (h₁ : isPrefixOf p q = true) (h₂ : isPrefixOf q r = true) :
    isPrefixOf p r = true :=
  (isPrefixOf_iff p r).mpr (hasPrefix_trans ((isPrefixOf_iff p q).mp h₁) ((isPrefixOf_iff q r).mp h₂))

theorem isPrefixOf_antisymm {p q : Path} (h₁ : isPrefixOf p q = true) (h₂ : isPrefixOf q p = true) :
    p = q :=
  hasPrefix_antisymm ((isPrefixOf_iff p q).mp h₁) ((isPrefixOf_iff q p).mp h₂)

/-- The parent of an appended child is the original path. SPEC: L3385-3388. -/
theorem parent_appendChild (p : Path) (n : String) : (p.appendChild n).parent = some p := by
  unfold appendChild parent
  cases h : p.elems ++ [PathElement.prim n] with
  | nil => simp at h
  | cons e rest =>
    simp only [Option.some.injEq]
    rw [← h, List.dropLast_concat]

theorem parent_appendProperty (p : Path) (ns : List String) (b : String) :
    (p.appendProperty ns b).parent = some p := by
  unfold appendProperty parent
  cases h : p.elems ++ [PathElement.prop ns b] with
  | nil => simp at h
  | cons e rest =>
    simp only [Option.some.injEq]
    rw [← h, List.dropLast_concat]

theorem appendChild_isPrimPath (p : Path) (n : String) : (p.appendChild n).isPrimPath = true := by
  simp [appendChild, isPrimPath, lastElem]

theorem appendProperty_isPropertyPath (p : Path) (ns : List String) (b : String) :
    (p.appendProperty ns b).isPropertyPath = true := by
  simp [appendProperty, isPropertyPath, lastElem]

/-- A path is a prefix of its own children. -/
theorem hasPrefix_appendChild (p : Path) (n : String) : HasPrefix p (p.appendChild n) :=
  ⟨rfl, rfl, List.prefix_append _ _⟩

/-! ## Worked examples from the spec, decided mechanically -/

/-- `/World/Foo/Baz/Foobar`. SPEC: L3400-3401. -/
example : (absoluteRoot.appendChild "World" |>.appendChild "Foo" |>.appendChild "Baz"
    |>.appendChild "Foobar").isPrimPath = true := by decide

/-- `/World/Foo/Baz/Foobar.foobarprop1` is a property path with parent
`/World/Foo/Baz/Foobar`. SPEC: L3419-3420. -/
example :
    let foobar := absoluteRoot.appendChild "World" |>.appendChild "Foo" |>.appendChild "Baz"
      |>.appendChild "Foobar"
    (foobar.appendProperty [] "foobarprop1").isPropertyPath = true
      ∧ (foobar.appendProperty [] "foobarprop1").parent = some foobar := by decide

/-- `/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1`. SPEC: L3452. -/
example : (absoluteRoot.appendChild "World" |>.appendChild "Foo" |>.appendChild "Baz"
    |>.appendVariantSelection "VariantSet1" "VarFoobar" |>.appendChild "Foobar"
    |>.appendProperty [] "foobarprop1").isWellFormed = true := by decide +kernel

/-- Identifier validity (SPEC: L1815-1837, L3631-3637): `_` may start an identifier,
digits may not; `München` is an identifier; reserved characters are not. -/
example : isIdentifier "_foo" = true := by decide +kernel
example : isIdentifier "foo1" = true := by decide +kernel
example : isIdentifier "1foo" = false := by decide +kernel
example : isIdentifier "" = false := by decide +kernel
example : isIdentifier "München" = true := by decide +kernel
example : isIdentifier "New York" = false := by decide +kernel
example : isIdentifier "a:b" = false := by decide +kernel
example : isPropertyName "primvars:color" = true := by decide +kernel
example : isPropertyName "namespace:" = false := by decide +kernel
example : isPropertyName ":property" = false := by decide +kernel
example : isPropertyName "namespace::property" = false := by decide +kernel
example : isPropertyName "abc:123" = false := by decide +kernel
/-- Variant names may start with a digit or a dot and contain `|` and `-`. SPEC: L3687-3690. -/
example : isVariantName "5thAvenue" = true := by decide +kernel
example : isVariantName ".hidden" = true := by decide +kernel
example : isVariantName "a|b-c" = true := by decide +kernel
example : isVariantName "" = false := by decide +kernel
example : isVariantName "a=b" = false := by decide +kernel

/-- The absolute root has no parent; the parent of `.` is `..`; `../..` is the parent
of `..`. SPEC: L3368-3374, L3787-3788. -/
example : absoluteRoot.parent = none := by decide
example : reflexiveRelative.parent = some ⟨false, 1, []⟩ := by decide
example : (Path.mk false 1 []).parent = some ⟨false, 2, []⟩ := by decide

/-- `/.prop` is not well-formed (`PrimFirstPathElements` must start with a prim name,
L3727-3729), while `../.points` is (L3783). -/
example : (Path.mk true 0 [.prop [] "prop"]).isWellFormed = false := by decide +kernel
example : (Path.mk false 1 [.prop [] "points"]).isWellFormed = true := by decide +kernel

end Path
end UsdSpec
