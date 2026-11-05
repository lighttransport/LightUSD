import Mathlib.Data.List.Basic
import UsdSpec.Path.Element
/-!
# Path grammar: parser, printer, round trip (AOUSD USD Core Specification v1.1.0, §8.3-8.5)

SPEC: core_spec.md L3573-3616 (8.3 Path Grammar, conventions), L3617-3737 (8.4 Grammar
Definition), L3738-3772 (reserved characters), L3773-3807 (8.5 Examples). The legacy
compatibility rules (L3809-3892) are not modelled.

Two things are formalized:

1. `Peg.accepts`: a *literal* transcription of the PEG (ordered choice, greedy
   repetition, no backtracking into a succeeded alternative), as a recognizer. It
   exists to check what the rules as written actually accept. It is NOT the parser
   implementations should use: see LEAN-202/203/206 below.

2. `parsePath` / `printPath`: the parser and printer this formalization (and the
   LightUSD implementation it mirrors) use. They follow the *examples* table
   (L3773-3807) and the prose of §8.1 wherever the PEG text disagrees with them:
   prims and variant selections may be mixed freely (`/a{v=x}b/c`), `..` may repeat
   (`../..`), and a path has at most one property. `parse_print` proves
   `parsePath (printPath p) = some p` for every well-formed `p`.

Whitespace inside variant selections (L3786) is accepted by the parser and never
produced by the printer, so `/City{ selection = NewYork }` parses to the same
`Path` as `/City{selection=NewYork}`.

Spec issues:

SPEC-ISSUE: LEAN-202 — `PrimElements` (L3712-3717) cannot produce the spec's own
variant paths. The rule is
```
PrimElements <- PrimName (&(ForwardSlash PrimName) ForwardSlash PrimName)* (VariantSelections)? /
                PrimName (&(VariantSelections PrimName) VariantSelections PrimName)* (VariantSelections)?
```
Read as a context-free grammar, the first alternative only produces `/`-separated
prim names (optionally ending in variant selections) and the second only
`{…}`-separated ones, so a path mixing both, e.g. `/a{v=x}b/c` or `/a/b{v=x}c`, is
derivable from neither. Read as a PEG (the spec's declared notation, L3575-3577,
with `/` = ordered choice, L3603-3604), it is worse: the first alternative succeeds
whenever `PrimName` matches, because `(…)*` and `(…)?` never fail, so the second
alternative is unreachable and `/a{v=x}b` is not accepted either — the spec's own
example `/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1` (L3478-3479)
is rejected by its grammar (`Peg.accepts` decides this below). The intended
language (§8.1 prose, examples L3459-3511) is `PrimName (ForwardSlash PrimName /
VariantSelections PrimName / VariantSelections)*`-like; `parsePath` implements that.

SPEC-ISSUE: LEAN-203 — Property-count asymmetry. `PathElements <- PrimElements
(PropElements)* / !(PrimElements) PropElements` (L3727-3729) allows any number of
`.prop` suffixes on a relative path (`a.b.c`), while `PrimFirstPathElements <-
PrimElements (PropElements)?` (L3732-3734), used for absolute paths, allows at most
one, and L3449-3450 says a property is a terminal leaf. `Peg.accepts "a.b.c"` is
`true` and `Peg.accepts "/a.b.c"` is `false`. `parsePath` allows one property in
both cases.

SPEC-ISSUE: LEAN-206 — `Path <- … / Dot Dot (ForwardSlash PathElements)? / …`
(L3736-3738) admits exactly one `..`: after consuming `..`, the optional group
needs `PathElements` after the `/`, and `..` is not a `PathElements`, so for
`../..` the group fails, leaving `/..` unconsumed. The examples table lists
`../..` and `../.points` (L3782-3783) as valid and L3380-3383 calls
`../../ParentSibilng/Cousin` legal. `Peg.accepts "../.."` is `false`;
`parsePath` accepts any number of leading `..`.

SPEC-ISSUE: LEAN-207 — The introductory example `/Root/Child/Grandchild{size=1}/GreatGrandchild`
(L3335) puts a `/` between a variant selection and the next prim name. L3497-3500
("there is no `/` separator between a variant selector and … a prim spec") and
both `PrimElements` alternatives (L3712-3717) forbid that, and OpenUSD's `SdfPath`
rejects the string. Compare the correct form `/World/Foo/Baz{VariantSet1=VarFoobaz}Foobaz`
at L3484. `parsePath` and `Peg.accepts` both reject it.
-/

namespace UsdSpec.PathGrammar

open UsdSpec

/-! ## Character classes (SPEC: L3649-3658) -/

def isSpace (c : Char) : Bool := c == ' ' || c == '\t'

/-- Reserved characters. SPEC: L3752-3758. -/
def isReserved (c : Char) : Bool :=
  c == '.' || c == '/' || c == '{' || c == '}' || c == ':' || c == '='

/-! ## Lexers on code-point lists -/

/-- `Identifier`: returns the identifier and the rest. SPEC: L3669-3671. -/
def lexIdent (cs : List Char) : Option (String × List Char) :=
  match cs with
  | [] => none
  | c :: cs' =>
    if isIdentifierStart c then
      some (String.ofList (c :: cs'.takeWhile isIdentifierContinue), cs'.dropWhile isIdentifierContinue)
    else none

/-- `VariantName`, possibly empty (used for `(VariantName)?`). SPEC: L3687-3690. -/
def lexVariantName (cs : List Char) : String × List Char :=
  match cs with
  | [] => ("", [])
  | c :: cs' =>
    if c == '.' then (String.ofList ('.' :: cs'.takeWhile isVariantContinue), cs'.dropWhile isVariantContinue)
    else (String.ofList (cs.takeWhile isVariantContinue), cs.dropWhile isVariantContinue)

/-- `(Space)?` -/
def skipSpaceOpt (cs : List Char) : List Char :=
  match cs with
  | [] => []
  | c :: cs' => if isSpace c then cs' else cs

/-- `(Space)*` -/
def skipSpaces (cs : List Char) : List Char := cs.dropWhile isSpace

/-- `VariantSelection <- '{' Space? VariantSetName Assignment VariantName? Space? '}'`.
SPEC: L3692-3695 (`Assignment <- Space* '=' Space*`, L3656-3658). -/
def lexVariantSelection (cs : List Char) : Option (PathElement × List Char) :=
  match cs with
  | [] => none
  | c :: cs' =>
    if c == '{' then
      match lexIdent (skipSpaceOpt cs') with
      | none => none
      | some (set, cs₁) =>
        match skipSpaces cs₁ with
        | [] => none
        | e :: cs₂ =>
          if e == '=' then
            let (name, cs₃) := lexVariantName (skipSpaces cs₂)
            match skipSpaceOpt cs₃ with
            | [] => none
            | r :: cs₄ => if r == '}' then some (.variant set name, cs₄) else none
          else none
    else none

/-- `(Colon Identifier)*` with `fuel` bounding the loop. -/
def lexNamespaces (fuel : Nat) (acc : List String) (cs : List Char) : List String × List Char :=
  match fuel with
  | 0 => (acc, cs)
  | fuel + 1 =>
    match cs with
    | [] => (acc, cs)
    | c :: cs' =>
      if c == ':' then
        match lexIdent cs' with
        | some (n, cs'') => lexNamespaces fuel (acc ++ [n]) cs''
        | none => (acc, cs)
      else (acc, cs)

/-- `PropElements <- Dot PropertyName` (the `.` already consumed). SPEC: L3719-3725. -/
def lexProp (cs : List Char) : Option (PathElement × List Char) :=
  match lexIdent cs with
  | none => none
  | some (first, cs') =>
    let (more, rest) := lexNamespaces cs'.length [] cs'
    match (first :: more).getLast? with
    | some base => some (.prop (first :: more).dropLast base, rest)
    | none => none

/-! ## The parser -/

/-- Parse the elements after a leading prim or variant selection. `acc` is the
elements so far (its last element is a prim or a variant selection). -/
def parseTail (fuel : Nat) (acc : List PathElement) (cs : List Char) : Option (List PathElement) :=
  match fuel with
  | 0 => if cs.isEmpty then some acc else none
  | fuel + 1 =>
    match cs with
    | [] => some acc
    | c :: cs' =>
      if c == '/' then
        match acc.getLast? with
        | some (.prim _) =>
          match lexIdent cs' with
          | some (n, rest) => parseTail fuel (acc ++ [.prim n]) rest
          | none => none
        | _ => none
      else if c == '{' then
        match lexVariantSelection cs with
        | some (v, rest) => parseTail fuel (acc ++ [v]) rest
        | none => none
      else if c == '.' then
        match lexProp cs' with
        | some (p, rest) => if rest.isEmpty then some (acc ++ [p]) else none
        | none => none
      else
        match acc.getLast? with
        | some (.variant _ _) =>
          match lexIdent cs with
          | some (n, rest) => parseTail fuel (acc ++ [.prim n]) rest
          | none => none
        | _ => none

/-- `PrimName …` elements (prim-first). -/
def parsePrimFirst (cs : List Char) : Option (List PathElement) :=
  match lexIdent cs with
  | some (n, rest) => parseTail rest.length [.prim n] rest
  | none => none

/-- `PathElements`: prim-first, or a lone `.property`. SPEC: L3727-3729. -/
def parseElemsRel (cs : List Char) : Option (List PathElement) :=
  match cs with
  | [] => none
  | c :: cs' =>
    if c == '.' then
      match lexProp cs' with
      | some (p, rest) => if rest.isEmpty then some [p] else none
      | none => none
    else parsePrimFirst cs

/-- Consume `(/..)*` after a leading `..`; stops at the first `/` not followed by `..`. -/
def countParents (fuel n : Nat) (cs : List Char) : Nat × List Char :=
  match fuel with
  | 0 => (n, cs)
  | fuel + 1 =>
    match cs with
    | a :: b :: c :: rest =>
      if a == '/' && b == '.' && c == '.' then countParents fuel (n + 1) rest else (n, cs)
    | _ => (n, cs)

/-- `Path` on code points. SPEC: L3736-3741 plus the deviations listed in the module
docstring (LEAN-202/203/206). -/
def parsePathList (cs : List Char) : Option Path :=
  match cs with
  | [] => none
  | c :: cs' =>
    if c == '/' then
      if cs'.isEmpty then some Path.absoluteRoot
      else (parsePrimFirst cs').map fun es => ⟨true, 0, es⟩
    else if c == '.' then
      match cs' with
      | [] => some Path.reflexiveRelative
      | d :: cs'' =>
        if d == '.' then
          let (n, rest) := countParents cs''.length 1 cs''
          match rest with
          | [] => some ⟨false, n, []⟩
          | s :: rest' =>
            if s == '/' then (parseElemsRel rest').map fun es => ⟨false, n, es⟩ else none
        else (parseElemsRel cs).map fun es => ⟨false, 0, es⟩
    else (parsePrimFirst cs).map fun es => ⟨false, 0, es⟩

/-- Parse a textual path. `none` for every string the grammar rejects. -/
def parsePath (s : String) : Option Path := parsePathList s.toList

/-! ## The printer -/

/-- `:`-joined identifiers on code points. -/
def joinColon : List String → List Char
  | [] => []
  | f :: more => f.toList ++ more.flatMap (fun n => ':' :: n.toList)

/-- Text of a property name on code points: `ns₁:…:nsₖ:base`. -/
def propChars (ns : List String) (base : String) : List Char := joinColon (ns ++ [base])

/-- Text of an element without its separator. -/
def elemChars : PathElement → List Char
  | .prim n => n.toList
  | .prop ns b => propChars ns b
  | .variant s n => '{' :: s.toList ++ '=' :: n.toList ++ ['}']

/-- Separator between a previous element and the next: `/` between prims, `.` before
a property, nothing around variant selections (L3497-3500). -/
def sepChars : PathElement → PathElement → List Char
  | .prim _, .prim _ => ['/']
  | _, .prop _ _ => ['.']
  | _, _ => []

def printRest (prev : PathElement) : List PathElement → List Char
  | [] => []
  | e :: rest => sepChars prev e ++ elemChars e ++ printRest e rest

/-- Elements of a path, the first one prefixed by `.` if it is a property. -/
def printElems : List PathElement → List Char
  | [] => []
  | e :: rest => (if e.isProp then ['.'] else []) ++ elemChars e ++ printRest e rest

/-- `(/..)*` -/
def dotsTail : Nat → List Char
  | 0 => []
  | k + 1 => '/' :: '.' :: '.' :: dotsTail k

def printPathList (p : Path) : List Char :=
  if p.absolute then '/' :: printElems p.elems
  else
    match p.parents with
    | 0 => if p.elems.isEmpty then ['.'] else printElems p.elems
    | k + 1 => '.' :: '.' :: dotsTail k ++ (if p.elems.isEmpty then [] else '/' :: printElems p.elems)

/-- Print a path in the grammar's textual form. -/
def printPath (p : Path) : String := String.ofList (printPathList p)

/-! ## Literal PEG recognizer (SPEC: L3649-3741, transcribed rule for rule) -/

namespace Peg

/-- Greedy `(f)*`: apply `f` while it succeeds. `fuel` bounds the loop. -/
def star (f : List Char → Option (List Char)) : Nat → List Char → List Char
  | 0, cs => cs
  | fuel + 1, cs => match f cs with
    | some cs' => star f fuel cs'
    | none => cs

/-- `(f)?` -/
def opt (f : List Char → Option (List Char)) (cs : List Char) : List Char := (f cs).getD cs

/-- `Identifier <- IdentifierStart (IdentifierContinue)*`, L3669-3671. -/
def identifier : List Char → Option (List Char)
  | [] => none
  | c :: cs => if isIdentifierStart c then some (cs.dropWhile isIdentifierContinue) else none

def primName := identifier
def variantSetName := identifier

/-- `VariantName <- (Dot (VariantContinue)*) / (VariantContinue)+`, L3687-3690. -/
def variantName : List Char → Option (List Char)
  | [] => none
  | c :: cs =>
    if c == '.' then some (cs.dropWhile isVariantContinue)
    else if isVariantContinue c then some (cs.dropWhile isVariantContinue) else none

def space : List Char → Option (List Char)
  | c :: cs => if isSpace c then some cs else none
  | [] => none

/-- `Assignment <- (Space)* Equals (Space)*`, L3656-3658. -/
def assignment (cs : List Char) : Option (List Char) :=
  match star space cs.length cs with
  | c :: rest => if c == '=' then some (star space rest.length rest) else none
  | [] => none

/-- `VariantSelection`, L3692-3695. -/
def variantSelection : List Char → Option (List Char)
  | c :: cs =>
    if c == '{' then do
      let cs ← variantSetName (opt space cs)
      let cs ← assignment cs
      let cs := opt variantName cs
      match opt space cs with
      | r :: rest => if r == '}' then some rest else none
      | [] => none
    else none
  | [] => none

/-- `VariantSelections <- (VariantSelection)+`, L3697-3699. -/
def variantSelections (cs : List Char) : Option (List Char) := do
  let cs' ← variantSelection cs
  some (star variantSelection cs'.length cs')

/-- `&(ForwardSlash PrimName) ForwardSlash PrimName` (the lookahead is redundant). -/
def slashPrimName : List Char → Option (List Char)
  | c :: cs => if c == '/' then primName cs else none
  | [] => none

/-- `&(VariantSelections PrimName) VariantSelections PrimName`. -/
def variantsPrimName (cs : List Char) : Option (List Char) := do
  let cs ← variantSelections cs
  primName cs

/-- `PrimElements`, L3712-3717: ordered choice of the two alternatives. The first
alternative succeeds whenever `PrimName` does, so the second is unreachable. -/
def primElements (cs : List Char) : Option (List Char) :=
  (do
    let cs ← primName cs
    let cs := star slashPrimName cs.length cs
    some (opt variantSelections cs))
  <|>
  (do
    let cs ← primName cs
    let cs := star variantsPrimName cs.length cs
    some (opt variantSelections cs))

/-- `PropertyName <- Identifier (Colon Identifier)*`, L3719-3721. -/
def propertyName (cs : List Char) : Option (List Char) := do
  let cs ← identifier cs
  some (star (fun cs => match cs with
    | c :: cs' => if c == ':' then identifier cs' else none
    | [] => none) cs.length cs)

/-- `PropElements <- Dot PropertyName`, L3723-3725. -/
def propElements : List Char → Option (List Char)
  | c :: cs => if c == '.' then propertyName cs else none
  | [] => none

/-- `PathElements <- PrimElements (PropElements)* / !(PrimElements) PropElements`, L3727-3729. -/
def pathElements (cs : List Char) : Option (List Char) :=
  (do
    let cs ← primElements cs
    some (star propElements cs.length cs))
  <|>
  (match primElements cs with
    | some _ => none
    | none => propElements cs)

/-- `PrimFirstPathElements <- PrimElements (PropElements)?`, L3732-3734. -/
def primFirstPathElements (cs : List Char) : Option (List Char) := do
  let cs ← primElements cs
  some (opt propElements cs)

/-- `Path`, L3736-3741. -/
def path (cs : List Char) : Option (List Char) :=
  (match cs with
    | c :: cs' => if c == '/' then some (opt primFirstPathElements cs') else none
    | [] => none)
  <|>
  (match cs with
    | a :: b :: cs' =>
      if a == '.' && b == '.' then
        some (opt (fun cs => match cs with
          | c :: cs'' => if c == '/' then pathElements cs'' else none
          | [] => none) cs')
      else none
    | _ => none)
  <|> pathElements cs
  <|> (match cs with
    | c :: cs' => if c == '.' then some cs' else none
    | [] => none)

/-- The literal grammar accepts `s` when `Path` consumes all of it. -/
def accepts (s : String) : Bool := path s.toList == some []

end Peg

/-! ## Examples (SPEC: L3773-3807 and the paths of §8.1), decided in the kernel -/

section Examples
open Path PathElement

/-- Valid paths table, L3776-3789. -/
example : parsePath "/" = some absoluteRoot := by decide +kernel
example : parsePath "." = some reflexiveRelative := by decide +kernel
example : parsePath ".property" = some ⟨false, 0, [.prop [] "property"]⟩ := by decide +kernel
example : parsePath ".." = some ⟨false, 1, []⟩ := by decide +kernel
example : parsePath "../.." = some ⟨false, 2, []⟩ := by decide +kernel
example : parsePath "../.points" = some ⟨false, 1, [.prop [] "points"]⟩ := by decide +kernel
example : parsePath "Descendant" = some ⟨false, 0, [.prim "Descendant"]⟩ := by decide +kernel
example : parsePath "/City{ selection = NewYork }"
    = some ⟨true, 0, [.prim "City", .variant "selection" "NewYork"]⟩ := by decide +kernel
example : parsePath "/City{ selection = NewYork }" = parsePath "/City{selection=NewYork}" := by
  decide +kernel
example : parsePath "/City/Street{selection=5thAvenue}"
    = some ⟨true, 0, [.prim "City", .prim "Street", .variant "selection" "5thAvenue"]⟩ := by
  decide +kernel
example : parsePath "/City/Street{selection=}"
    = some ⟨true, 0, [.prim "City", .prim "Street", .variant "selection" ""]⟩ := by decide +kernel

/-- Invalid paths table, L3791-3807. -/
example : parsePath "" = none := by decide +kernel
example : parsePath "./Descendant/Prim" = none := by decide +kernel
example : parsePath "/New York/New York" = none := by decide +kernel
example : parsePath "/abc/123" = none := by decide +kernel
example : parsePath "/City/Street{=}" = none := by decide +kernel
example : parsePath "/abc{123=}" = none := by decide +kernel
example : parsePath "/Root/" = none := by decide +kernel
example : parsePath "/Root//Child" = none := by decide +kernel
example : parsePath "/Prim/namespace:" = none := by decide +kernel
example : parsePath "/Prim.:property" = none := by decide +kernel
example : parsePath "/Prim.namespace::property" = none := by decide +kernel
example : parsePath "/Prim.abc:123" = none := by decide +kernel

/-- §8.1 worked paths: L3330-3337, L3360-3364, L3380-3383, L3400-3405, L3439-3444,
L3478-3484, L3496, L3509-3511. -/
example : parsePath "/Root/Child/Grandchild"
    = some ⟨true, 0, [.prim "Root", .prim "Child", .prim "Grandchild"]⟩ := by decide +kernel
example : parsePath "/Root/Child/Grandchild.visibility"
    = some ⟨true, 0, [.prim "Root", .prim "Child", .prim "Grandchild", .prop [] "visibility"]⟩ := by
  decide +kernel
/-- LEAN-207: the L3335 example is not a path. -/
theorem spec_intro_variant_example_rejected :
    parsePath "/Root/Child/Grandchild{size=1}/GreatGrandchild" = none
    ∧ Peg.accepts "/Root/Child/Grandchild{size=1}/GreatGrandchild" = false := by decide +kernel
example : parsePath "/Parent/Child{color=r}.primvars:color"
    = some ⟨true, 0, [.prim "Parent", .prim "Child", .variant "color" "r",
        .prop ["primvars"] "color"]⟩ := by decide +kernel
example : parsePath "Child/Grandchild" = some ⟨false, 0, [.prim "Child", .prim "Grandchild"]⟩ := by
  decide +kernel
example : parsePath "../Sibling/Child/Grandchild"
    = some ⟨false, 1, [.prim "Sibling", .prim "Child", .prim "Grandchild"]⟩ := by decide +kernel
example : parsePath "../../Descendant" = some ⟨false, 2, [.prim "Descendant"]⟩ := by decide +kernel
example : parsePath "../Sibling/Child/.." = none := by decide +kernel
example : parsePath "../../ParentSibilng/Cousin"
    = some ⟨false, 2, [.prim "ParentSibilng", .prim "Cousin"]⟩ := by decide +kernel
example : parsePath "/World/Foo/Baz/Foobar"
    = some ⟨true, 0, [.prim "World", .prim "Foo", .prim "Baz", .prim "Foobar"]⟩ := by decide +kernel
example : parsePath "/World/Bar/BarBaz"
    = some ⟨true, 0, [.prim "World", .prim "Bar", .prim "BarBaz"]⟩ := by decide +kernel
example : parsePath "../../Foo/Baz/Foobar"
    = some ⟨false, 2, [.prim "Foo", .prim "Baz", .prim "Foobar"]⟩ := by decide +kernel
example : parsePath "/World/Foo/Baz/Foobar.foobarprop1"
    = some ⟨true, 0, [.prim "World", .prim "Foo", .prim "Baz", .prim "Foobar",
        .prop [] "foobarprop1"]⟩ := by decide +kernel
example : parsePath "../../Foo/Baz/Foobar.foobarprop1"
    = some ⟨false, 2, [.prim "Foo", .prim "Baz", .prim "Foobar", .prop [] "foobarprop1"]⟩ := by
  decide +kernel
example : parsePath ".foobarprop1" = some ⟨false, 0, [.prop [] "foobarprop1"]⟩ := by decide +kernel
example : parsePath "Baz/Foobar.foobarprop1"
    = some ⟨false, 0, [.prim "Baz", .prim "Foobar", .prop [] "foobarprop1"]⟩ := by decide +kernel
example : parsePath "/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1"
    = some ⟨true, 0, [.prim "World", .prim "Foo", .prim "Baz", .variant "VariantSet1" "VarFoobar",
        .prim "Foobar", .prop [] "foobarprop1"]⟩ := by decide +kernel
example : parsePath "/World/Foo/Baz{VariantSet1=VarFoobaz}Foobaz"
    = some ⟨true, 0, [.prim "World", .prim "Foo", .prim "Baz", .variant "VariantSet1" "VarFoobaz",
        .prim "Foobaz"]⟩ := by decide +kernel
example : parsePath "/World/Bar{VariantSet2=VarBarFoo}{VariantSet3=VarBarBar}BarFoo"
    = some ⟨true, 0, [.prim "World", .prim "Bar", .variant "VariantSet2" "VarBarFoo",
        .variant "VariantSet3" "VarBarBar", .prim "BarFoo"]⟩ := by decide +kernel
example : parsePath "/a{x=}" = some ⟨true, 0, [.prim "a", .variant "x" ""]⟩ := by decide +kernel

/-- Mixed separators (LEAN-202): accepted by `parsePath`, rejected by the literal PEG. -/
example : parsePath "/a{v=x}b/c"
    = some ⟨true, 0, [.prim "a", .variant "v" "x", .prim "b", .prim "c"]⟩ := by decide +kernel
example : parsePath "/a/b{v=x}c"
    = some ⟨true, 0, [.prim "a", .prim "b", .variant "v" "x", .prim "c"]⟩ := by decide +kernel
/-- A `/` directly after a variant selection is not allowed (neither PEG alternative
has it; L3497-3500). -/
example : parsePath "/a{v=x}/b" = none := by decide +kernel
/-- Only one property, and it is terminal (LEAN-203, L3449-3450). -/
example : parsePath "a.b.c" = none := by decide +kernel
example : parsePath "/a.b.c" = none := by decide +kernel
example : parsePath "/a.b/c" = none := by decide +kernel
example : parsePath "/a.b{v=x}" = none := by decide +kernel

/-- What the PEG text literally accepts (`Peg.accepts`): the three issues. -/
theorem peg_rejects_mixed_separators :
    Peg.accepts "/a{v=x}b/c" = false ∧ Peg.accepts "/a/b{v=x}c" = false
    ∧ Peg.accepts "/a{v=x}b" = false
    ∧ Peg.accepts "/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1" = false := by
  decide +kernel
theorem peg_property_asymmetry : Peg.accepts "a.b.c" = true ∧ Peg.accepts "/a.b.c" = false := by
  decide +kernel
theorem peg_rejects_double_dotdot :
    Peg.accepts "../.." = false ∧ Peg.accepts "../../ParentSibilng/Cousin" = false
    ∧ Peg.accepts ".." = true ∧ Peg.accepts "../.points" = true := by
  decide +kernel
/-- Sanity: the literal PEG agrees with `parsePath` on the examples tables where no
issue applies. -/
example : Peg.accepts "/" ∧ Peg.accepts "." ∧ Peg.accepts ".property" ∧ Peg.accepts "Descendant"
    ∧ Peg.accepts "/City{ selection = NewYork }" ∧ Peg.accepts "/City/Street{selection=5thAvenue}"
    ∧ Peg.accepts "/City/Street{selection=}" ∧ Peg.accepts "/a/b{v=x}"
    ∧ Peg.accepts "/Parent/Child{color=r}.primvars:color" := by decide +kernel
example : !Peg.accepts "" ∧ !Peg.accepts "./Descendant/Prim" ∧ !Peg.accepts "/New York/New York"
    ∧ !Peg.accepts "/abc/123" ∧ !Peg.accepts "/City/Street{=}" ∧ !Peg.accepts "/abc{123=}"
    ∧ !Peg.accepts "/Root/" ∧ !Peg.accepts "/Root//Child" ∧ !Peg.accepts "/Prim/namespace:"
    ∧ !Peg.accepts "/Prim.:property" ∧ !Peg.accepts "/Prim.namespace::property"
    ∧ !Peg.accepts "/Prim.abc:123" := by decide +kernel

/-- Printer examples. -/
example : printPath ⟨true, 0, [.prim "World", .prim "Foo", .prim "Baz", .variant "VariantSet1" "VarFoobar",
    .prim "Foobar", .prop [] "foobarprop1"]⟩ = "/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1" := by
  decide +kernel
example : printPath ⟨false, 2, [.prim "Foo", .prop ["primvars"] "color"]⟩ = "../../Foo.primvars:color" := by
  decide +kernel
example : printPath absoluteRoot = "/" ∧ printPath reflexiveRelative = "." ∧ printPath ⟨false, 2, []⟩ = "../.." := by
  decide +kernel

end Examples

/-! ## Round trip: `parsePath (printPath p) = some p` for well-formed `p` -/

section RoundTrip
open Path PathElement

/-! ### Concrete facts about the character classes, decided in the kernel -/

theorem cont_slash : isIdentifierContinue '/' = false := by decide +kernel
theorem cont_lbrace : isIdentifierContinue '{' = false := by decide +kernel
theorem cont_dot : isIdentifierContinue '.' = false := by decide +kernel
theorem cont_eq : isIdentifierContinue '=' = false := by decide +kernel
theorem cont_colon : isIdentifierContinue ':' = false := by decide +kernel
theorem start_slash : isIdentifierStart '/' = false := by decide +kernel
theorem start_lbrace : isIdentifierStart '{' = false := by decide +kernel
theorem start_dot : isIdentifierStart '.' = false := by decide +kernel
theorem start_space : isIdentifierStart ' ' = false := by decide +kernel
theorem start_tab : isIdentifierStart '\t' = false := by decide +kernel
theorem vcont_rbrace : isVariantContinue '}' = false := by decide +kernel
theorem vcont_space : isVariantContinue ' ' = false := by decide +kernel
theorem vcont_tab : isVariantContinue '\t' = false := by decide +kernel

theorem isSpace_eq_false_of_start {c : Char} (h : isIdentifierStart c = true) : isSpace c = false := by
  unfold isSpace
  by_cases h1 : c = ' '
  · subst h1; simp [start_space] at h
  · by_cases h2 : c = '\t'
    · subst h2; simp [start_tab] at h
    · simp [h1, h2]

theorem isSpace_eq_false_of_vcont {c : Char} (h : isVariantContinue c = true) : isSpace c = false := by
  unfold isSpace
  by_cases h1 : c = ' '
  · subst h1; simp [vcont_space] at h
  · by_cases h2 : c = '\t'
    · subst h2; simp [vcont_tab] at h
    · simp [h1, h2]

theorem start_ne_slash {c : Char} (h : isIdentifierStart c = true) : (c == '/') = false := by
  by_cases e : c = '/'
  · subst e; simp [start_slash] at h
  · simp [e]
theorem start_ne_lbrace {c : Char} (h : isIdentifierStart c = true) : (c == '{') = false := by
  by_cases e : c = '{'
  · subst e; simp [start_lbrace] at h
  · simp [e]
theorem start_ne_dot {c : Char} (h : isIdentifierStart c = true) : (c == '.') = false := by
  by_cases e : c = '.'
  · subst e; simp [start_dot] at h
  · simp [e]

/-! ### `takeWhile` / `dropWhile` on an all-true prefix -/

theorem takeWhile_eq_self_of_all {p : Char → Bool} {l : List Char} (h : ∀ a ∈ l, p a = true) :
    l.takeWhile p = l := by
  induction l with
  | nil => rfl
  | cons a l ih => simp [h a (by simp), ih (fun b hb => h b (by simp [hb]))]

theorem dropWhile_eq_nil_of_all {p : Char → Bool} {l : List Char} (h : ∀ a ∈ l, p a = true) :
    l.dropWhile p = [] := by
  induction l with
  | nil => rfl
  | cons a l ih => simp [h a (by simp), ih (fun b hb => h b (by simp [hb]))]

/-- A list whose head (if any) fails `p` is untouched by `takeWhile`/`dropWhile`. -/
def HeadFails (p : Char → Bool) (l : List Char) : Prop := ∀ c, l.head? = some c → p c = false

theorem takeWhile_eq_nil_of_headFails {p : Char → Bool} {l : List Char} (h : HeadFails p l) :
    l.takeWhile p = [] := by
  cases l with
  | nil => rfl
  | cons a l => simp [h a rfl]

theorem dropWhile_eq_self_of_headFails {p : Char → Bool} {l : List Char} (h : HeadFails p l) :
    l.dropWhile p = l := by
  cases l with
  | nil => rfl
  | cons a l => simp [h a rfl]

theorem takeWhile_append_all {p : Char → Bool} {l r : List Char} (hl : ∀ a ∈ l, p a = true)
    (hr : HeadFails p r) : (l ++ r).takeWhile p = l := by
  rw [List.takeWhile_append_of_pos hl, takeWhile_eq_nil_of_headFails hr, List.append_nil]

theorem dropWhile_append_all {p : Char → Bool} {l r : List Char} (hl : ∀ a ∈ l, p a = true)
    (hr : HeadFails p r) : (l ++ r).dropWhile p = r := by
  rw [List.dropWhile_append, dropWhile_eq_nil_of_all hl]
  simp [dropWhile_eq_self_of_headFails hr]

theorem headFails_cons {p : Char → Bool} {c : Char} {l : List Char} (h : p c = false) :
    HeadFails p (c :: l) := fun d hd => by simp at hd; rw [← hd]; exact h

/-! ### Lexer lemmas -/

/-- An identifier followed by anything that cannot continue it lexes exactly. -/
theorem lexIdent_append (n : String) (hn : isIdentifier n = true) (rest : List Char)
    (hr : HeadFails isIdentifierContinue rest) : lexIdent (n.toList ++ rest) = some (n, rest) := by
  unfold isIdentifier isIdentifierList at hn
  cases h : n.toList with
  | nil => simp [h] at hn
  | cons c cs =>
    rw [h] at hn
    simp only [Bool.and_eq_true, List.all_eq_true] at hn
    simp only [List.cons_append, lexIdent, hn.1, ↓reduceIte, Option.some.injEq, Prod.mk.injEq]
    rw [takeWhile_append_all hn.2 hr, dropWhile_append_all hn.2 hr, ← h, String.ofList_toList]
    exact ⟨rfl, rfl⟩

theorem toList_ne_nil_of_isIdentifier {n : String} (hn : isIdentifier n = true) : n.toList ≠ [] := by
  intro h; simp [isIdentifier, isIdentifierList, h] at hn

theorem head_start_of_isIdentifier {n : String} (hn : isIdentifier n = true) {c : Char} {cs : List Char}
    (h : n.toList = c :: cs) : isIdentifierStart c = true := by
  simp [isIdentifier, isIdentifierList, h] at hn; exact hn.1

theorem exists_cons_of_isIdentifier {n : String} (hn : isIdentifier n = true) :
    ∃ c cs, n.toList = c :: cs ∧ isIdentifierStart c = true := by
  cases h : n.toList with
  | nil => exact absurd h (toList_ne_nil_of_isIdentifier hn)
  | cons c cs => exact ⟨c, cs, rfl, head_start_of_isIdentifier hn h⟩

theorem headFails_flatMap_colon (more : List String) :
    HeadFails isIdentifierContinue (more.flatMap (fun n => ':' :: n.toList)) := by
  intro c hc
  cases more with
  | nil => simp at hc
  | cons m ms => simp at hc; rw [← hc]; exact cont_colon

theorem lexNamespaces_flatMap (ids : List String) (hids : ∀ n ∈ ids, isIdentifier n = true)
    (acc : List String) (fuel : Nat) (hf : ids.length ≤ fuel) :
    lexNamespaces fuel acc (ids.flatMap (fun n => ':' :: n.toList)) = (acc ++ ids, []) := by
  induction ids generalizing acc fuel with
  | nil => cases fuel <;> simp [lexNamespaces]
  | cons n more ih =>
    cases fuel with
    | zero => simp at hf
    | succ fuel =>
      have hn : isIdentifier n = true := hids n (by simp)
      simp only [List.flatMap_cons, List.cons_append, lexNamespaces, beq_self_eq_true, ↓reduceIte]
      rw [lexIdent_append n hn _ (headFails_flatMap_colon more)]
      simp only
      rw [ih (fun x hx => hids x (by simp [hx])) (acc ++ [n]) fuel (by simpa using hf)]
      simp

theorem length_le_flatMap_colon (ids : List String) :
    ids.length ≤ (ids.flatMap (fun n => ':' :: n.toList)).length := by
  induction ids with
  | nil => simp
  | cons n more ih =>
    simp only [List.flatMap_cons, List.length_cons, List.length_append]
    omega

/-- A `:`-joined nonempty identifier list lexes to the property with its last part as base. -/
theorem lexProp_joinColon (ids : List String) (hne : ids ≠ []) (hids : ∀ n ∈ ids, isIdentifier n = true) :
    lexProp (joinColon ids) = some (.prop ids.dropLast (ids.getLast hne), []) := by
  cases ids with
  | nil => exact absurd rfl hne
  | cons f more =>
    have hf : isIdentifier f = true := hids f (by simp)
    simp only [joinColon, lexProp]
    rw [lexIdent_append f hf _ (headFails_flatMap_colon more)]
    simp only
    rw [lexNamespaces_flatMap more (fun x hx => hids x (by simp [hx])) [] _ (length_le_flatMap_colon more)]
    simp [List.getLast?_eq_some_getLast]

theorem lexProp_propChars (ns : List String) (b : String) (hns : ∀ n ∈ ns, isIdentifier n = true)
    (hb : isIdentifier b = true) : lexProp (propChars ns b) = some (.prop ns b, []) := by
  unfold propChars
  rw [lexProp_joinColon (ns ++ [b]) (by simp)
    (fun n hn => by simp at hn; rcases hn with hn | rfl; exact hns n hn; exact hb)]
  simp

/-- The first code point of a property text is an identifier start. -/
theorem propChars_head (ns : List String) (b : String) (hns : ∀ n ∈ ns, isIdentifier n = true)
    (hb : isIdentifier b = true) : ∃ c cs, propChars ns b = c :: cs ∧ isIdentifierStart c = true := by
  unfold propChars
  cases ns with
  | nil =>
    obtain ⟨c, cs, h, hc⟩ := exists_cons_of_isIdentifier hb
    exact ⟨c, cs ++ ([] : List String).flatMap (fun n => ':' :: n.toList), by simp [joinColon, h], hc⟩
  | cons f more =>
    obtain ⟨c, cs, h, hc⟩ := exists_cons_of_isIdentifier (hns f (by simp))
    exact ⟨c, cs ++ (more ++ [b]).flatMap (fun n => ':' :: n.toList), by simp [joinColon, h], hc⟩

theorem skipSpaces_of_headFails {l : List Char} (h : HeadFails isSpace l) : skipSpaces l = l :=
  dropWhile_eq_self_of_headFails h

theorem skipSpaceOpt_of_headFails {l : List Char} (h : HeadFails isSpace l) : skipSpaceOpt l = l := by
  cases l with
  | nil => rfl
  | cons a l => simp [skipSpaceOpt, h a rfl]

theorem lexVariantName_append (n : String) (hn : n = "" ∨ isVariantName n = true) (rest : List Char)
    (hr : HeadFails isVariantContinue rest) (hr' : HeadFails (fun c => c == '.') rest) :
    lexVariantName (n.toList ++ rest) = (n, rest) := by
  rcases hn with rfl | hn
  · cases rest with
    | nil => rfl
    | cons r rs =>
      have := hr r rfl
      have h2 := hr' r rfl
      simp [lexVariantName, h2, this]
  · unfold isVariantName isVariantNameList at hn
    cases h : n.toList with
    | nil => simp [h] at hn
    | cons c cs =>
      rw [h] at hn
      by_cases hc : c = '.'
      · subst hc
        simp only [beq_self_eq_true, ↓reduceIte, List.all_eq_true] at hn
        simp only [List.cons_append, lexVariantName, beq_self_eq_true, ↓reduceIte, Prod.mk.injEq]
        rw [takeWhile_append_all hn hr, dropWhile_append_all hn hr, ← h, String.ofList_toList]
        exact ⟨rfl, rfl⟩
      · have hc' : (c == '.') = false := by simp [hc]
        simp only [hc', Bool.false_eq_true, ↓reduceIte, List.all_eq_true] at hn
        simp only [List.cons_append, lexVariantName, hc', Bool.false_eq_true, ↓reduceIte, Prod.mk.injEq]
        rw [← List.cons_append, takeWhile_append_all hn hr, dropWhile_append_all hn hr, ← h,
          String.ofList_toList]
        exact ⟨rfl, rfl⟩

theorem lexVariantSelection_append (s n : String) (hs : isIdentifier s = true)
    (hn : n = "" ∨ isVariantName n = true) (rest : List Char) :
    lexVariantSelection ('{' :: (s.toList ++ '=' :: (n.toList ++ '}' :: rest)))
      = some (.variant s n, rest) := by
  have hsp : HeadFails isSpace (s.toList ++ '=' :: (n.toList ++ '}' :: rest)) := by
    intro c hc
    obtain ⟨d, ds, h, hd⟩ := exists_cons_of_isIdentifier hs
    rw [h] at hc; simp at hc; rw [← hc]
    exact isSpace_eq_false_of_start hd
  have hname_sp : HeadFails isSpace (n.toList ++ '}' :: rest) := by
    intro c hc
    rcases hn with rfl | hn
    · simp at hc; rw [← hc]; decide
    · unfold isVariantName isVariantNameList at hn
      cases h : n.toList with
      | nil => simp [h] at hn
      | cons d ds =>
        rw [h] at hc; simp at hc; rw [← hc]; rw [h] at hn
        by_cases hd : d = '.'
        · subst hd; decide
        · simp [hd] at hn; exact isSpace_eq_false_of_vcont hn.1
  have hr1 : HeadFails isVariantContinue ('}' :: rest) := headFails_cons vcont_rbrace
  have hr2 : HeadFails (fun c => c == '.') ('}' :: rest) := headFails_cons (by decide)
  simp only [lexVariantSelection, beq_self_eq_true, ↓reduceIte]
  rw [skipSpaceOpt_of_headFails hsp]
  rw [lexIdent_append s hs _ (headFails_cons cont_eq)]
  simp only [skipSpaces, List.dropWhile_cons, show isSpace '=' = false from rfl, Bool.false_eq_true,
    ↓reduceIte, beq_self_eq_true]
  rw [← skipSpaces, skipSpaces_of_headFails hname_sp, lexVariantName_append n hn _ hr1 hr2]
  simp [skipSpaceOpt, show isSpace '}' = false by decide]

/-! ### Printer shape lemmas -/

theorem elemChars_ne_nil (e : PathElement) (he : e.isWellFormed = true) : elemChars e ≠ [] := by
  cases e with
  | prim n => exact toList_ne_nil_of_isIdentifier (by simpa [PathElement.isWellFormed] using he)
  | prop ns b =>
    have hwf : (∀ n ∈ ns, isIdentifier n = true) ∧ isIdentifier b = true := by
      simpa [PathElement.isWellFormed, List.all_eq_true] using he
    obtain ⟨c, cs, h, _⟩ := propChars_head ns b hwf.1 hwf.2
    simp [elemChars, h]
  | variant s n => simp [elemChars]

/-- After a prim or variant, the printed rest starts with a non-continue character,
unless it is a bare prim name right after a variant selection. -/
theorem printRest_headFails (prev : PathElement) (hprev : prev.isProp = false) (rest : List PathElement) :
    HeadFails isIdentifierContinue (printRest prev rest) ∨
      (∃ n rest', rest = .prim n :: rest' ∧ prev.isVariant = true) := by
  cases rest with
  | nil => left; intro c hc; simp [printRest] at hc
  | cons e rest' =>
    cases e with
    | prim n =>
      cases prev with
      | prim m => left; intro c hc; simp [printRest, sepChars] at hc; rw [← hc]; exact cont_slash
      | prop ns b => simp [PathElement.isProp] at hprev
      | variant s v => right; exact ⟨n, rest', rfl, rfl⟩
    | prop ns b =>
      left; intro c hc
      cases prev <;> simp [printRest, sepChars] at hc <;> rw [← hc] <;> exact cont_dot
    | variant s v =>
      left; intro c hc
      cases prev <;> simp [printRest, sepChars, elemChars] at hc <;> rw [← hc] <;> exact cont_lbrace

theorem printRest_headFails_of_prim (m : String) (rest : List PathElement) :
    HeadFails isIdentifierContinue (printRest (.prim m) rest) := by
  rcases printRest_headFails (.prim m) rfl rest with h | ⟨_, _, _, h⟩
  · exact h
  · simp [PathElement.isVariant] at h

theorem length_le_printRest (rest : List PathElement) : ∀ prev : PathElement,
    rest.all PathElement.isWellFormed = true → rest.length ≤ (printRest prev rest).length := by
  induction rest with
  | nil => intro _ _; simp [printRest]
  | cons e rest ih =>
    intro prev hwf
    simp only [List.all_cons, Bool.and_eq_true] at hwf
    have h1 := ih e hwf.2
    have h2 : 0 < (elemChars e).length := List.length_pos_iff.mpr (elemChars_ne_nil e hwf.1)
    simp only [printRest, List.length_append, List.length_cons]
    omega

/-! ### The parser consumes what the printer produces -/

theorem parseTail_printRest (rest : List PathElement) : ∀ (prev : PathElement) (acc : List PathElement)
    (fuel : Nat), prev.isProp = false → primTailShape rest = true →
    rest.all PathElement.isWellFormed = true → acc.getLast? = some prev → rest.length ≤ fuel →
    parseTail fuel acc (printRest prev rest) = some (acc ++ rest) := by
  induction rest with
  | nil => intro prev acc fuel _ _ _ _ _; cases fuel <;> simp [printRest, parseTail]
  | cons e rest ih =>
    intro prev acc fuel hprev hshape hwf hacc hf
    cases fuel with
    | zero => simp at hf
    | succ fuel =>
      simp only [List.all_cons, Bool.and_eq_true] at hwf
      obtain ⟨hwe, hwrest⟩ := hwf
      have hf' : rest.length ≤ fuel := by simpa using hf
      cases e with
      | prim n =>
        have hn : isIdentifier n = true := by simpa [PathElement.isWellFormed] using hwe
        have hshape' : primTailShape rest = true := by simpa [primTailShape] using hshape
        have hR := printRest_headFails_of_prim n rest
        obtain ⟨c, cs, hc, hstart⟩ := exists_cons_of_isIdentifier hn
        cases prev with
        | prop _ _ => simp [PathElement.isProp] at hprev
        | prim m =>
          simp only [printRest, sepChars, elemChars, List.append_assoc, List.cons_append]
          simp only [parseTail, beq_self_eq_true, ↓reduceIte, hacc, List.nil_append]
          rw [lexIdent_append n hn _ hR]
          simp only
          have := ih (PathElement.prim n) (acc ++ [PathElement.prim n]) fuel rfl hshape' hwrest (by simp) hf'
          rw [this]
          simp
        | variant s v =>
          simp only [printRest, sepChars, elemChars, List.nil_append]
          rw [hc, List.cons_append]
          simp only [parseTail, start_ne_slash hstart, start_ne_lbrace hstart, start_ne_dot hstart,
            Bool.false_eq_true, ↓reduceIte, hacc]
          rw [← List.cons_append, ← hc, lexIdent_append n hn _ hR]
          simp only
          have := ih (PathElement.prim n) (acc ++ [PathElement.prim n]) fuel rfl hshape' hwrest (by simp) hf'
          rw [this]
          simp
      | variant s v =>
        have hsv : isIdentifier s = true ∧ (v = "" ∨ isVariantName v = true) := by
          simp only [PathElement.isWellFormed, Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq] at hwe
          exact hwe
        have hshape' : primTailShape rest = true := by simpa [primTailShape] using hshape
        have hsep : sepChars prev (.variant s v) = [] := by cases prev <;> rfl
        simp only [printRest, hsep, elemChars, List.nil_append, List.cons_append, List.append_assoc]
        simp only [parseTail, show ('{' == '/') = false from rfl, show ('{' == '{') = true from rfl,
          Bool.false_eq_true, ↓reduceIte]
        rw [lexVariantSelection_append s v hsv.1 hsv.2]
        simp only
        have := ih (PathElement.variant s v) (acc ++ [PathElement.variant s v]) fuel rfl hshape' hwrest
          (by simp) hf'
        rw [this]
        simp
      | prop ns b =>
        have hrest : rest = [] := by
          cases rest with
          | nil => rfl
          | cons _ _ => simp [primTailShape] at hshape
        subst hrest
        have hwf : (∀ n ∈ ns, isIdentifier n = true) ∧ isIdentifier b = true := by
          simpa [PathElement.isWellFormed, List.all_eq_true] using hwe
        have hsep : sepChars prev (.prop ns b) = ['.'] := by cases prev <;> rfl
        simp only [printRest, hsep, elemChars, List.singleton_append, List.append_nil]
        simp only [parseTail, show ('.' == '/') = false from rfl, show ('.' == '{') = false from rfl,
          show ('.' == '.') = true from rfl, Bool.false_eq_true, ↓reduceIte]
        rw [lexProp_propChars ns b hwf.1 hwf.2]
        simp

theorem printElems_prim (n : String) (rest : List PathElement) :
    printElems (.prim n :: rest) = n.toList ++ printRest (.prim n) rest := rfl

theorem printElems_prop (ns : List String) (b : String) :
    printElems [.prop ns b] = '.' :: propChars ns b := by
  simp [printElems, elemChars, printRest, PathElement.isProp]

theorem parsePrimFirst_printElems (n : String) (rest : List PathElement) (hn : isIdentifier n = true)
    (hshape : primTailShape rest = true) (hwf : rest.all PathElement.isWellFormed = true) :
    parsePrimFirst (printElems (.prim n :: rest)) = some (.prim n :: rest) := by
  rw [printElems_prim]
  unfold parsePrimFirst
  rw [lexIdent_append n hn _ (printRest_headFails_of_prim n rest)]
  simp only
  exact parseTail_printRest rest (.prim n) [.prim n] _ rfl hshape hwf rfl
    (length_le_printRest rest (.prim n) hwf)

theorem parseElemsRel_printElems (elems : List PathElement) (hne : elems ≠ [])
    (hshape : elemsShape false elems = true) (hwf : elems.all PathElement.isWellFormed = true) :
    parseElemsRel (printElems elems) = some elems := by
  cases elems with
  | nil => exact absurd rfl hne
  | cons e rest =>
    simp only [List.all_cons, Bool.and_eq_true] at hwf
    cases e with
    | prim n =>
      have hn : isIdentifier n = true := by simpa [PathElement.isWellFormed] using hwf.1
      have hshape' : primTailShape rest = true := by simpa [elemsShape] using hshape
      have hp := parsePrimFirst_printElems n rest hn hshape' hwf.2
      obtain ⟨c, cs, hc, hstart⟩ := exists_cons_of_isIdentifier hn
      rw [printElems_prim] at hp ⊢
      rw [hc, List.cons_append] at hp ⊢
      simp only [parseElemsRel, start_ne_dot hstart, Bool.false_eq_true, ↓reduceIte]
      exact hp
    | prop ns b =>
      have hrest : rest = [] := by
        cases rest with
        | nil => rfl
        | cons _ _ => simp [elemsShape] at hshape
      subst hrest
      have hwf' : (∀ n ∈ ns, isIdentifier n = true) ∧ isIdentifier b = true := by
        simpa [PathElement.isWellFormed, List.all_eq_true] using hwf.1
      rw [printElems_prop]
      simp only [parseElemsRel, beq_self_eq_true, ↓reduceIte]
      rw [lexProp_propChars ns b hwf'.1 hwf'.2]
      simp
    | variant _ _ => simp [elemsShape] at hshape

/-- `tail` does not start another `/..` step. -/
def NotParentStep (tail : List Char) : Prop :=
  ∀ a b c r, tail = a :: b :: c :: r → (a == '/' && b == '.' && c == '.') = false

theorem countParents_dotsTail (k : Nat) : ∀ (n fuel : Nat) (tail : List Char), k ≤ fuel →
    NotParentStep tail → countParents fuel n (dotsTail k ++ tail) = (n + k, tail) := by
  induction k with
  | zero =>
    intro n fuel tail _ ht
    cases fuel with
    | zero => simp [countParents, dotsTail]
    | succ f =>
      rcases tail with _ | ⟨a, _ | ⟨b, _ | ⟨c, r⟩⟩⟩
      · rfl
      · rfl
      · rfl
      · simp [dotsTail, countParents, ht a b c r rfl]
  | succ k ih =>
    intro n fuel tail hf ht
    cases fuel with
    | zero => omega
    | succ f =>
      simp only [dotsTail, List.cons_append, countParents, beq_self_eq_true, Bool.and_self, ↓reduceIte]
      rw [ih (n + 1) f tail (by omega) ht]
      simp only [Prod.mk.injEq, and_true]
      omega

theorem dotsTail_length (k : Nat) : (dotsTail k).length = 3 * k := by
  induction k with
  | zero => rfl
  | succ k ih => simp [dotsTail, ih]; omega

theorem notParentStep_printElems (elems : List PathElement) (hne : elems ≠ [])
    (hshape : elemsShape false elems = true) (hwf : elems.all PathElement.isWellFormed = true) :
    NotParentStep ('/' :: printElems elems) := by
  cases elems with
  | nil => exact absurd rfl hne
  | cons e rest =>
    simp only [List.all_cons, Bool.and_eq_true] at hwf
    cases e with
    | prim n =>
      have hn : isIdentifier n = true := by simpa [PathElement.isWellFormed] using hwf.1
      obtain ⟨c, cs, hc, hstart⟩ := exists_cons_of_isIdentifier hn
      intro a b d r h
      rw [printElems_prim, hc, List.cons_append] at h
      simp only [List.cons.injEq] at h
      obtain ⟨rfl, rfl, _⟩ := h
      simp [start_ne_dot hstart]
    | prop ns b =>
      have hrest : rest = [] := by
        cases rest with
        | nil => rfl
        | cons _ _ => simp [elemsShape] at hshape
      subst hrest
      have hwf' : (∀ n ∈ ns, isIdentifier n = true) ∧ isIdentifier b = true := by
        simpa [PathElement.isWellFormed, List.all_eq_true] using hwf.1
      obtain ⟨c, cs, hc, hstart⟩ := propChars_head ns b hwf'.1 hwf'.2
      intro a b' d r h
      rw [printElems_prop, hc] at h
      simp only [List.cons.injEq] at h
      obtain ⟨rfl, rfl, rfl, _⟩ := h
      simp [start_ne_dot hstart]
    | variant _ _ => simp [elemsShape] at hshape

/-- **Round trip.** Every well-formed path prints to a string the parser reads back. -/
theorem parse_print (p : Path) (h : p.WellFormed) : parsePath (printPath p) = some p := by
  unfold parsePath printPath
  rw [String.toList_ofList]
  obtain ⟨absolute, parents, elems⟩ := p
  unfold Path.WellFormed Path.isWellFormed at h
  simp only [Bool.and_eq_true] at h
  obtain ⟨⟨hpar, hshape⟩, hwf⟩ := h
  cases absolute with
  | true =>
    have hp0 : parents = 0 := by simpa using hpar
    subst hp0
    cases elems with
    | nil => simp [printPathList, printElems, parsePathList, Path.absoluteRoot]
    | cons e rest =>
      cases e with
      | prim n =>
        simp only [List.all_cons, Bool.and_eq_true] at hwf
        have hn : isIdentifier n = true := by simpa [PathElement.isWellFormed] using hwf.1
        have hshape' : primTailShape rest = true := by simpa [elemsShape] using hshape
        have hp := parsePrimFirst_printElems n rest hn hshape' hwf.2
        obtain ⟨c, cs, hc, hstart⟩ := exists_cons_of_isIdentifier hn
        simp only [printPathList, ↓reduceIte]
        rw [printElems_prim] at hp ⊢
        rw [hc, List.cons_append] at hp ⊢
        simp only [parsePathList, beq_self_eq_true, ↓reduceIte, List.isEmpty_cons, Bool.false_eq_true]
        rw [hp]
        rfl
      | prop _ _ =>
        cases rest <;> simp [elemsShape] at hshape
      | variant _ _ => simp [elemsShape] at hshape
  | false =>
    cases parents with
    | zero =>
      cases elems with
      | nil => simp [printPathList, parsePathList, Path.reflexiveRelative]
      | cons e rest =>
        have hne : e :: rest ≠ [] := by simp
        have hrel := parseElemsRel_printElems (e :: rest) hne hshape hwf
        simp only [printPathList, Bool.false_eq_true, ↓reduceIte, List.isEmpty_cons]
        cases e with
        | prim n =>
          simp only [List.all_cons, Bool.and_eq_true] at hwf
          have hn : isIdentifier n = true := by simpa [PathElement.isWellFormed] using hwf.1
          obtain ⟨c, cs, hc, hstart⟩ := exists_cons_of_isIdentifier hn
          rw [printElems_prim] at hrel ⊢
          rw [hc, List.cons_append] at hrel ⊢
          simp only [parseElemsRel, start_ne_dot hstart, Bool.false_eq_true, ↓reduceIte] at hrel
          simp only [parsePathList, start_ne_slash hstart, start_ne_dot hstart, Bool.false_eq_true,
            ↓reduceIte]
          rw [hrel]
          rfl
        | prop ns b =>
          have hrest : rest = [] := by
            cases rest with
            | nil => rfl
            | cons _ _ => simp [elemsShape] at hshape
          subst hrest
          simp only [List.all_cons, List.all_nil, Bool.and_true] at hwf
          have hwf' : (∀ n ∈ ns, isIdentifier n = true) ∧ isIdentifier b = true := by
            simpa [PathElement.isWellFormed, List.all_eq_true] using hwf
          obtain ⟨c, cs, hc, hstart⟩ := propChars_head ns b hwf'.1 hwf'.2
          rw [printElems_prop] at hrel ⊢
          rw [hc] at hrel ⊢
          simp only [parsePathList, show ('.' == '/') = false from rfl, beq_self_eq_true,
            start_ne_dot hstart, Bool.false_eq_true, ↓reduceIte]
          rw [hrel]
          rfl
        | variant _ _ => simp [elemsShape] at hshape
    | succ k =>
      simp only [printPathList, Bool.false_eq_true, ↓reduceIte]
      cases elems with
      | nil =>
        simp only [List.isEmpty_nil, ↓reduceIte, List.append_nil]
        simp only [parsePathList, show ('.' == '/') = false from rfl, beq_self_eq_true,
          Bool.false_eq_true, ↓reduceIte]
        have hc := countParents_dotsTail k 1 (dotsTail k).length [] (by rw [dotsTail_length]; omega)
          (fun a b c r h => by simp at h)
        rw [List.append_nil] at hc
        rw [hc]
        simp only [Option.some.injEq, Path.mk.injEq, true_and, and_true]
        omega
      | cons e rest =>
        have hne : e :: rest ≠ [] := by simp
        have hrel := parseElemsRel_printElems (e :: rest) hne hshape hwf
        have hnps := notParentStep_printElems (e :: rest) hne hshape hwf
        simp only [List.isEmpty_cons, Bool.false_eq_true, ↓reduceIte, List.cons_append]
        simp only [parsePathList, show ('.' == '/') = false from rfl, beq_self_eq_true,
          Bool.false_eq_true, ↓reduceIte]
        have hc := countParents_dotsTail k 1 (dotsTail k ++ '/' :: printElems (e :: rest)).length
          ('/' :: printElems (e :: rest)) (by simp [dotsTail_length]; omega) hnps
        rw [hc]
        simp only [beq_self_eq_true, ↓reduceIte]
        rw [hrel]
        simp only [Option.map_some, Option.some.injEq, Path.mk.injEq, true_and, and_true]
        omega

/-- Corollary on strings: printing is injective on well-formed paths. -/
theorem printPath_injective {p q : Path} (hp : p.WellFormed) (hq : q.WellFormed)
    (h : printPath p = printPath q) : p = q := by
  have := parse_print p hp
  rw [h, parse_print q hq] at this
  exact (Option.some.injEq _ _).mp this |>.symm

end RoundTrip

end UsdSpec.PathGrammar
