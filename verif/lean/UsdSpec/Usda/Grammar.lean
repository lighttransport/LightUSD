/-!
# USDA text format — subset parser and printer (milestone M4)

SPEC: core_spec.md L9569-11375 (text format PEG). This module covers the
productions below and nothing else; the full 166-rule PEG is milestone M5.

| rule | spec line | covered as |
|---|---|---|
| `LayerHeader` | L11204 | `#usda 1.0` only (`parseHeader`) |
| `LayerSpec` | L11212 | header, optional `LayerMetadata`, `PrimSpec`* |
| `LayerMetadata`/`MetadataBlock<T>` | L11201, L10406 | `subLayers` only |
| `SublayerMetadata`/`SublayerItem` | L11183, L11176 | asset + `(offset = n; scale = n)` |
| `LayerOffset` | L10057 | `offset` / `scale` |
| `PrimSpec`/`PrimSpecifier`/`PrimTypeName` | L10955, L10949, L10921 | full |
| `PrimMetadata` | L10909 | `references`, `inherits`, `specializes`, `customData` |
| `ListOp` | L10377 | `delete` / `append` / `prepend` (absent = explicit) |
| `ReferencesMetadata`/`ReferenceList`/`ReferenceListItem` | L10861, L10854, L10849 | asset? path? `(offset; scale)` |
| `InheritsMetadata`/`SpecializesMetadata`/`InheritsOrSpecializesList` | L10835, L10831, L10824 | full |
| `DictionaryValue`/`DictionaryValueItem`/`DictionaryKey` | L10158, L10153, L10144 | `int`, `string`, nested `dictionary` |
| `AttributeSpec` + `SpecialAttributeType` | L10466, L10454 | `double name = n`, `double name.timeSamples = { n: n, }` |
| `TimeSample`/`TimeSampleMap` | L10187, L10192 | `Number: Number` |

Simplifications (documented, not spec deviations): strings are `"..."` without escapes
or `"`/`\\`/newline; numbers are kept as their lexeme `-?[0-9]+(.[0-9]+)?` (exact, no
`Float`); `None` lists parse as `[]`; whitespace (incl. newlines and `;` statement
separators of `Sequence<T>`, L10084) is skipped everywhere a padding rule appears; no
comments. Single-item `ReferenceList`/`InheritsOrSpecializesList` without brackets is
accepted; the printer always brackets.

Grammar fact recorded while formalizing (SPEC-ISSUE: LEAN-801, candidate): `ListValue`
(L10123) requires a non-empty `List<ListItem>`, so `ListOpValue` (L10383) cannot be
`[]`; e.g. `prepend apiSchemas = []` is not in the language. pxr 26.11's parser also
rejects it (`parse error matching ... ListOf<ListValueItem>`), so this is consistent
with the reference but means an authored empty prepend/append token list op has no
text form, while `ReferenceList`/`InheritsOrSpecializesList` (L10824, L10854) do admit
`[]`.

`parse (print x) = some x` is checked by `#guard` on examples (including the text
pxr 26.11 `Sdf.Layer.ExportToString` produced for an equivalent layer); the list-op
keyword and specifier productions are proven; the general theorem is not.
-/

namespace UsdSpec.Usda

/-! ## AST -/

inductive Specifier | def_ | over | class_
  deriving DecidableEq, Repr

/-- `ListOp` keyword (L10377); `explicit` = no keyword. -/
inductive Qual | explicit | prepend | append | delete
  deriving DecidableEq, Repr

/-- `Number` lexeme. -/
abbrev Num := String

structure Offset where
  offset : Option Num := none
  scale  : Option Num := none
  deriving DecidableEq, Repr

structure SubLayer where
  asset : String
  off   : Offset := {}
  deriving DecidableEq, Repr

structure Reference where
  asset : Option String
  prim  : Option String     -- PathRef contents
  off   : Offset := {}
  deriving DecidableEq, Repr

inductive DVal where
  | int (n : Int)
  | str (s : String)
  | dict (items : List (String × DVal))
  deriving Repr

inductive PrimMeta where
  | references (q : Qual) (items : List Reference)
  | inherits (q : Qual) (paths : List String)
  | specializes (q : Qual) (paths : List String)
  | customData (items : List (String × DVal))
  deriving Repr

inductive Prim where
  | mk (spec : Specifier) (type : Option String) (name : String) (mds : List PrimMeta)
       (items : List (Sum (String × Sum Num (List (Num × Num))) Prim))
  deriving Repr

structure Layer where
  subLayers : Option (List SubLayer)
  prims : List Prim
  deriving Repr

/-! ## Printer (pxr-style canonical layout) -/

def ind (n : Nat) : String := String.ofList (List.replicate (4 * n) ' ')
def q (s : String) : String := "\"" ++ s ++ "\""

def Specifier.print : Specifier → String
  | .def_ => "def" | .over => "over" | .class_ => "class"

def Qual.print : Qual → String
  | .explicit => "" | .prepend => "prepend " | .append => "append " | .delete => "delete "

def Offset.print (o : Offset) : String :=
  let items := (o.offset.map ("offset = " ++ ·)).toList ++ (o.scale.map ("scale = " ++ ·)).toList
  if items.isEmpty then "" else " (" ++ "; ".intercalate items ++ ")"

def Reference.print (r : Reference) : String :=
  (r.asset.map (fun a => "@" ++ a ++ "@")).getD "" ++ (r.prim.map (fun p => "<" ++ p ++ ">")).getD "" ++
    r.off.print

def bracket (n : Nat) (xs : List String) : String :=
  if xs.isEmpty then "[]" else
  "[\n" ++ String.join (xs.map (fun x => ind (n + 1) ++ x ++ ",\n")) ++ ind n ++ "]"

mutual
def DVal.printItem (n : Nat) : String × DVal → String
  | (k, .int i) => ind n ++ "int " ++ k ++ " = " ++ toString i ++ "\n"
  | (k, .str s) => ind n ++ "string " ++ k ++ " = " ++ q s ++ "\n"
  | (k, .dict d) => ind n ++ "dictionary " ++ k ++ " = " ++ DVal.printDict n d ++ "\n"
def DVal.printDict (n : Nat) : List (String × DVal) → String
  | [] => "{\n" ++ ind n ++ "}"
  | items => "{\n" ++ DVal.printItems (n + 1) items ++ ind n ++ "}"
def DVal.printItems (n : Nat) : List (String × DVal) → String
  | [] => ""
  | i :: is => DVal.printItem n i ++ DVal.printItems n is
end

def PrimMeta.print (n : Nat) : PrimMeta → String
  | .references qu rs => ind n ++ qu.print ++ "references = " ++ bracket n (rs.map Reference.print)
  | .inherits qu ps => ind n ++ qu.print ++ "inherits = " ++ bracket n (ps.map (fun p => "<" ++ p ++ ">"))
  | .specializes qu ps => ind n ++ qu.print ++ "specializes = " ++ bracket n (ps.map (fun p => "<" ++ p ++ ">"))
  | .customData d => ind n ++ "customData = " ++ DVal.printDict n d

def printAttr (n : Nat) : String × Sum Num (List (Num × Num)) → String
  | (nm, .inl v) => ind n ++ "double " ++ nm ++ " = " ++ v ++ "\n"
  | (nm, .inr ts) => ind n ++ "double " ++ nm ++ ".timeSamples = {\n" ++
      String.join (ts.map (fun (t, v) => ind (n + 1) ++ t ++ ": " ++ v ++ ",\n")) ++ ind n ++ "}\n"

mutual
def Prim.print (n : Nat) : Prim → String
  | .mk sp ty nm mds items =>
    ind n ++ sp.print ++ " " ++ (ty.map (· ++ " ")).getD "" ++ q nm ++
    (if mds.isEmpty then "" else
      " (\n" ++ String.join (mds.map (fun m => PrimMeta.print (n + 1) m ++ "\n")) ++ ind n ++ ")") ++
    "\n" ++ ind n ++ "{\n" ++ Prim.printItems (n + 1) items ++ ind n ++ "}\n"
def Prim.printItems (n : Nat) : List (Sum (String × Sum Num (List (Num × Num))) Prim) → String
  | [] => ""
  | .inl a :: is => printAttr n a ++ Prim.printItems n is
  | .inr p :: is => Prim.print n p ++ Prim.printItems n is
end

def SubLayer.print (s : SubLayer) : String := "@" ++ s.asset ++ "@" ++ s.off.print

def Layer.print (l : Layer) : String :=
  "#usda 1.0\n" ++
  (match l.subLayers with
   | none => ""
   | some ss => "(\n" ++ ind 1 ++ "subLayers = " ++ bracket 1 (ss.map SubLayer.print) ++ "\n)\n") ++
  String.join (l.prims.map (fun p => "\n" ++ Prim.print 0 p))

/-! ## Parser (recursive descent on `List Char`, fuel-bounded) -/

abbrev P (α : Type) := List Char → Option (α × List Char)

def isWs (c : Char) : Bool := c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';'
def ws : List Char → List Char := List.dropWhile isWs

def lit (s : String) : P Unit := fun cs =>
  let cs := ws cs
  if s.toList.isPrefixOf cs then some ((), cs.drop s.length) else none

def isIdStart (c : Char) : Bool := c.isAlpha || c == '_'
def isIdCont (c : Char) : Bool := c.isAlphanum || c == '_' || c == ':'

def ident : P String := fun cs =>
  match ws cs with
  | c :: r => if isIdStart c then
      let t := r.takeWhile isIdCont
      some (String.ofList (c :: t), r.drop t.length) else none
  | [] => none

/-- Text between `o` and the next `c`. -/
def delimited (o c : Char) : P String := fun cs =>
  match ws cs with
  | x :: r => if x == o then
      let t := r.takeWhile (· != c)
      match r.drop t.length with
      | y :: r' => if y == c then some (String.ofList t, r') else none
      | [] => none
    else none
  | [] => none

def str : P String := delimited '"' '"'
def asset : P String := delimited '@' '@'
def pathRef : P String := delimited '<' '>'

def peek (cs : List Char) : Option Char := (ws cs).head?

def isNumChar (c : Char) : Bool := c.isDigit || c == '.' || c == '-' || c == 'e' || c == '+'

def num : P Num := fun cs =>
  let cs := ws cs
  let t := cs.takeWhile isNumChar
  if t.isEmpty then none else some (String.ofList t, cs.drop t.length)

def int : P Int := fun cs => do
  let (s, r) ← num cs
  let i ← s.toInt?
  some (i, r)

/-- `ListOp` keyword, or none. Consumes the keyword only when followed by more input. -/
def qual : P Qual := fun cs =>
  match ident cs with
  | some ("prepend", r) => some (.prepend, r)
  | some ("append", r) => some (.append, r)
  | some ("delete", r) => some (.delete, r)
  | _ => some (.explicit, cs)

def specifier : P Specifier := fun cs =>
  match ident cs with
  | some ("def", r) => some (.def_, r)
  | some ("over", r) => some (.over, r)
  | some ("class", r) => some (.class_, r)
  | _ => none

/-- `( offset = n; scale = n )` items (L10057), possibly empty / absent. -/
def offsetItems : Nat → Offset → P Offset
  | 0, _ => fun _ => none
  | f + 1, o => fun cs =>
    match ident cs with
    | some ("offset", r) => do let ((), r) ← lit "=" r; let (n, r) ← num r; offsetItems f { o with offset := some n } r
    | some ("scale", r) => do let ((), r) ← lit "=" r; let (n, r) ← num r; offsetItems f { o with scale := some n } r
    | _ => do let ((), r) ← lit ")" cs; some (o, r)

def optOffset : P Offset := fun cs =>
  if peek cs == some '(' then do
    let ((), r) ← lit "(" cs
    offsetItems (cs.length + 1) {} r
  else some ({}, cs)

/-- Comma list until `]`, with optional trailing comma (`List<T>`, L10079). -/
def listUntil {α} (item : P α) : Nat → List α → P (List α)
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if peek cs == some ']' then do let ((), r) ← lit "]" cs; some (acc.reverse, r)
    else do
      let (x, r) ← item cs
      let r := if peek r == some ',' then (ws r).drop 1 else r
      listUntil item f (x :: acc) r

/-- `None` / single item / `[ List<T>? ]`. -/
def listOrOne {α} (item : P α) : P (List α) := fun cs =>
  match ident cs with
  | some ("None", r) => some ([], r)
  | _ =>
    if peek cs == some '[' then do
      let ((), r) ← lit "[" cs
      listUntil item (cs.length + 1) [] r
    else do let (x, r) ← item cs; some ([x], r)

def reference : P Reference := fun cs => do
  let (a, r) := match asset cs with | some (a, r) => (some a, r) | none => (none, cs)
  let (p, r) := match pathRef r with | some (p, r) => (some p, r) | none => (none, r)
  if a.isNone && p.isNone then none
  let (o, r) ← optOffset r
  some (⟨a, p, o⟩, r)

def subLayer : P SubLayer := fun cs => do
  let (a, r) ← asset cs
  let (o, r) ← optOffset r
  some (⟨a, o⟩, r)

mutual
def dictItems : Nat → List (String × DVal) → P (List (String × DVal))
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if peek cs == some '}' then do let ((), r) ← lit "}" cs; some (acc.reverse, r)
    else do
      let (ty, r) ← ident cs
      let (k, r) ← (match str r with | some x => some x | none => ident r)
      let ((), r) ← lit "=" r
      let (v, r) ← (match ty with
        | "int" => do let (i, r) ← int r; some (DVal.int i, r)
        | "string" => do let (s, r) ← str r; some (DVal.str s, r)
        | "dictionary" => do let (d, r) ← dict f r; some (DVal.dict d, r)
        | _ => none)
      dictItems f ((k, v) :: acc) r
def dict : Nat → P (List (String × DVal))
  | 0 => fun _ => none
  | f + 1 => fun cs => do let ((), r) ← lit "{" cs; dictItems f [] r
end

def primMeta : P PrimMeta := fun cs => do
  let (qu, r) ← qual cs
  let (k, r) ← ident r
  let ((), r) ← lit "=" r
  match k, qu with
  | "references", _ => do let (xs, r) ← listOrOne reference r; some (.references qu xs, r)
  | "inherits", _ => do let (xs, r) ← listOrOne pathRef r; some (.inherits qu xs, r)
  | "specializes", _ => do let (xs, r) ← listOrOne pathRef r; some (.specializes qu xs, r)
  | "customData", .explicit => do let (d, r) ← dict (r.length + 1) r; some (.customData d, r)
  | _, _ => none

def primMetas : Nat → List PrimMeta → P (List PrimMeta)
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if peek cs == some ')' then do let ((), r) ← lit ")" cs; some (acc.reverse, r)
    else do let (m, r) ← primMeta cs; primMetas f (m :: acc) r

def samples : Nat → List (Num × Num) → P (List (Num × Num))
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if peek cs == some '}' then do let ((), r) ← lit "}" cs; some (acc.reverse, r)
    else do
      let (t, r) ← num cs
      let ((), r) ← lit ":" r
      let (v, r) ← num r
      let r := if peek r == some ',' then (ws r).drop 1 else r
      samples f ((t, v) :: acc) r

/-- `double name = n` / `double name.timeSamples = { ... }` (after `double`). -/
def attrRest : P (String × Sum Num (List (Num × Num))) := fun cs => do
  let (nm, r) ← ident cs
  if ".timeSamples".toList.isPrefixOf r then
    let r := r.drop ".timeSamples".length
    let ((), r) ← lit "=" r
    let ((), r) ← lit "{" r
    let (ts, r) ← samples (r.length + 1) [] r
    some ((nm, .inr ts), r)
  else
    let ((), r) ← lit "=" r
    let (v, r) ← num r
    some ((nm, .inl v), r)

mutual
def prim : Nat → P Prim
  | 0 => fun _ => none
  | f + 1 => fun cs => do
    let (sp, r) ← specifier cs
    let (ty, r) ← (if peek r == some '"' then some (none, r) else do
      let (t, r) ← ident r; some (some t, r))
    let (nm, r) ← str r
    let (mds, r) ← (if peek r == some '(' then do
        let ((), r) ← lit "(" r; primMetas (r.length + 1) [] r
      else some ([], r))
    let ((), r) ← lit "{" r
    let (items, r) ← primItems f [] r
    some (.mk sp ty nm mds items, r)
def primItems : Nat → List (Sum (String × Sum Num (List (Num × Num))) Prim) →
    P (List (Sum (String × Sum Num (List (Num × Num))) Prim))
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if peek cs == some '}' then do let ((), r) ← lit "}" cs; some (acc.reverse, r)
    else match ident cs with
      | some ("double", r) => do let (a, r) ← attrRest r; primItems f (.inl a :: acc) r
      | _ => do let (p, r) ← prim f cs; primItems f (.inr p :: acc) r
end

def prims : Nat → List Prim → P (List Prim)
  | 0, _ => fun _ => none
  | f + 1, acc => fun cs =>
    if ws cs = [] then some (acc.reverse, []) else do
      let (p, r) ← prim (cs.length + 1) cs
      prims f (p :: acc) r

/-- `LayerHeader` (L11204), version `1.0` only. -/
def parseHeader : P Unit := fun cs => do
  let ((), r) ← lit "#usda" cs
  lit "1.0" r

def parseLayer (s : String) : Option Layer := do
  let cs := s.toList
  let ((), r) ← parseHeader cs
  let (subs, r) ← (if peek r == some '(' then do
      let ((), r) ← lit "(" r
      match ident r with
      | some ("subLayers", r) => do
        let ((), r) ← lit "=" r
        let ((), r) ← lit "[" r
        let (ss, r) ← listUntil subLayer (r.length + 1) [] r
        let ((), r) ← lit ")" r
        some (some ss, r)
      | _ => do let ((), r) ← lit ")" r; some (some [], r)
    else some (none, r))
  let (ps, r) ← prims (r.length + 1) [] r
  if ws r = [] then some ⟨subs, ps⟩ else none

/-! ## Decidable equality on the AST (structural, via printing is not used) -/

mutual
def DVal.beq : DVal → DVal → Bool
  | .int a, .int b => a == b
  | .str a, .str b => a == b
  | .dict a, .dict b => DVal.beqItems a b
  | _, _ => false
def DVal.beqItems : List (String × DVal) → List (String × DVal) → Bool
  | [], [] => true
  | (k, v) :: as, (k', v') :: bs => k == k' && DVal.beq v v' && DVal.beqItems as bs
  | _, _ => false
end

def PrimMeta.beq : PrimMeta → PrimMeta → Bool
  | .references q1 a, .references q2 b => q1 == q2 && a == b
  | .inherits q1 a, .inherits q2 b => q1 == q2 && a == b
  | .specializes q1 a, .specializes q2 b => q1 == q2 && a == b
  | .customData a, .customData b => DVal.beqItems a b
  | _, _ => false

mutual
def Prim.beq : Prim → Prim → Bool
  | .mk s1 t1 n1 m1 i1, .mk s2 t2 n2 m2 i2 =>
    s1 == s2 && t1 == t2 && n1 == n2 && m1.length == m2.length &&
    (m1.zip m2).all (fun (a, b) => PrimMeta.beq a b) && Prim.beqItems i1 i2
def Prim.beqItems : List (Sum (String × Sum Num (List (Num × Num))) Prim) →
    List (Sum (String × Sum Num (List (Num × Num))) Prim) → Bool
  | [], [] => true
  | .inl a :: as, .inl b :: bs => a == b && Prim.beqItems as bs
  | .inr a :: as, .inr b :: bs => Prim.beq a b && Prim.beqItems as bs
  | _, _ => false
end

def Layer.beq (a b : Layer) : Bool :=
  a.subLayers == b.subLayers && a.prims.length == b.prims.length &&
  (a.prims.zip b.prims).all (fun (x, y) => Prim.beq x y)

/-- The round-trip property, as a Boolean check. -/
def roundtrips (l : Layer) : Bool :=
  match parseLayer l.print with
  | some l' => Layer.beq l l'
  | none => false

/-! ## Proven pieces -/

theorem qual_print (qu : Qual) (rest : List Char) (hq : qu ≠ .explicit) :
    qual (qu.print.toList ++ 'r' :: rest) = some (qu, ' ' :: 'r' :: rest) := by
  cases qu <;> simp_all [qual, ident, ws, isWs, isIdStart, isIdCont, Qual.print] <;> decide

theorem specifier_print (s : Specifier) (rest : List Char) :
    specifier (s.print.toList ++ ' ' :: '"' :: rest) = some (s, ' ' :: '"' :: rest) := by
  cases s <;> rfl

/-! ## Examples -/

def exLayer : Layer where
  subLayers := some [⟨"a.usda", ⟨some "2", some "0.5"⟩⟩, ⟨"b.usda", {}⟩]
  prims := [
    .mk .def_ (some "Xform") "World"
      [.references .prepend [⟨some "a.usda", some "/P", ⟨some "2", some "0.5"⟩⟩, ⟨none, some "/Q", {}⟩],
       .references .delete [⟨some "c.usda", none, {}⟩],
       .inherits .explicit ["/_class_A"],
       .specializes .append ["/S1", "/S2"],
       .customData [("k", .dict [("n", .int (-1)), ("s", .str "x y")]), ("z", .int 3)]]
      [.inl ("x", .inl "1.5"), .inl ("x", .inr [("0", "2"), ("1.5", "-3")]),
       .inr (.mk .over none "Child" [] [.inr (.mk .class_ (some "Scope") "G" [] [])])],
    .mk .class_ none "_class_A" [.inherits .delete []] []]

#guard roundtrips exLayer
#guard roundtrips ⟨none, []⟩
#guard roundtrips ⟨some [], [.mk .def_ none "A" [.customData []] []]⟩

/-- Verbatim `Sdf.Layer.ExportToString()` output of pxr 26.11 for a layer built through
the Sdf API (scratch run). -/
def pxrText : String :=
"#usda 1.0
(
    subLayers = [
        @a.usda@ (offset = 2; scale = 0.5)
    ]
)

def \"B\" (
    customData = {
        dictionary k = {
            int n = 1
            string s = \"x\"
        }
    }
    delete references = </Q>
    prepend references = @a.usda@</P> (offset = 2; scale = 0.5)
)
{
    double x = 1.5
    double x.timeSamples = {
        0: 2,
        1.5: -3,
    }
}
"

def pxrAst : Layer where
  subLayers := some [⟨"a.usda", ⟨some "2", some "0.5"⟩⟩]
  prims := [.mk .def_ none "B"
    [.customData [("k", .dict [("n", .int 1), ("s", .str "x")])],
     .references .delete [⟨none, some "/Q", {}⟩],
     .references .prepend [⟨some "a.usda", some "/P", ⟨some "2", some "0.5"⟩⟩]]
    [.inl ("x", .inl "1.5"), .inl ("x", .inr [("0", "2"), ("1.5", "-3")])]]

#guard (parseLayer pxrText).map (Layer.beq pxrAst) == some true
#guard roundtrips pxrAst

-- Rejections: wrong header, unterminated prim, unsupported dictionary value type.
#guard (parseLayer "#usda 2.0\n").isNone
#guard (parseLayer "#usda 1.0\ndef \"A\" {\n").isNone
#guard (parseLayer "#usda 1.0\ndef \"A\" (\n  customData = { float f = 1 }\n) {}\n").isNone

end UsdSpec.Usda
