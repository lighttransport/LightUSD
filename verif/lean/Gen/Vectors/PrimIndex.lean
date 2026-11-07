import UsdSpec.Composition.PrimIndex
import UsdSpec.Composition.PrimIndexInvariants
import Lean.Data.Json

/-! Cross-layer arc-list edits. Expected stacks are computed by compose, not
by a second list-edit implementation in the native test harness. -/
namespace Gen.PrimIndexVec
open Lean UsdSpec UsdSpec.Composition.PrimIndex

def mapOp {α β : Type} (f : α → β) : ListOp α → ListOp β
  | .explicit xs => .explicit (xs.map f)
  | .comp c => .comp ⟨c.deleted.map f, c.prepended.map f, c.appended.map f⟩

def strongOps : List (Option (ListOp String)) :=
  [none, some (.explicit []), some (.explicit ["C", "D"]),
   some (.comp ⟨["A"], [], []⟩), some (.comp ⟨[], ["C", "A"], []⟩),
   some (.comp ⟨[], [], ["B", "C"]⟩), some (.comp ⟨["A"], ["D"], ["B"]⟩),
   some ListOp.default]

def weakOps : List (Option (ListOp String)) :=
  [none, some (.explicit ["A", "B"]), some (.comp ⟨[], ["B", "A"], []⟩),
   some (.explicit [])]

def fields : List String := ["references", "payload", "inherits", "specializes"]
def names : List String := ["A", "B", "C", "D"]

def modelSpec (field : String) (op : Option (ListOp String)) : PrimSpec :=
  let paths := op.map (mapOp fun name => P [name])
  let refs := op.map (mapOp fun name => (some "asset.usda", P [name]))
  { path := P ["P"]
    referenceEdits := if field == "references" then refs else none
    payloadEdits := if field == "payload" then refs else none
    inheritEdits := if field == "inherits" then paths else none
    specializeEdits := if field == "specializes" then paths else none }

def target (field name : String) : String :=
  if field == "references" || field == "payload" then "@asset.usda@</" ++ name ++ ">"
  else "</" ++ name ++ ">"

def items (field : String) (xs : List String) : String :=
  "[" ++ ", ".intercalate (xs.map (target field)) ++ "]"

def authorOp (field : String) : Option (ListOp String) → String
  | none => ""
  | some (.explicit xs) => "  " ++ field ++ " = " ++ items field xs ++ "\n"
  | some (.comp c) =>
    (if c.deleted.isEmpty then "" else "  delete " ++ field ++ " = " ++ items field c.deleted ++ "\n") ++
    (if c.prepended.isEmpty then "" else "  prepend " ++ field ++ " = " ++ items field c.prepended ++ "\n") ++
    (if c.appended.isEmpty then "" else "  append " ++ field ++ " = " ++ items field c.appended ++ "\n")

def primText (field : String) (op : Option (ListOp String)) : String :=
  let authored := authorOp field op
  "def \"P\"" ++ (if authored.isEmpty then "" else " (\n" ++ authored ++ ")") ++ " {}\n"

def targetsText (specifier : String) : String :=
  "".intercalate ((enum names).map fun (i, name) =>
    specifier ++ " \"" ++ name ++ "\" { int marker = " ++ toString (i+1) ++
      "\n  def \"Child\" { int marker = " ++ toString (i+1) ++ "\n  }\n}\n")

def targetSpecs : List PrimSpec := names.flatMap fun name =>
  [sp (P [name]), sp (P [name, "Child"])]

def siteJson (site : UsdSpec.Strength.Site) : Json :=
  Json.mkObj [("layer", toJson site.layer), ("path", toJson site.path)]

def vectors : Json := Id.run do
  let mut cases : Array Json := #[]
  for field in fields do
    for (si, strong) in enum strongOps do
      for (wi, weak) in enum weakOps do
        for loadPayloads in [true, false] do
          let localClasses := if field == "inherits" || field == "specializes"
            then targetSpecs else []
          let layers : Layers :=
            [{ name := "root.usda", sublayers := ["weak.usda"],
               specs := modelSpec field strong :: localClasses },
             { name := "weak.usda", specs := [modelSpec field weak] },
             { name := "asset.usda", specs := targetSpecs }]
          let expected := composeWithLoadPolicy loadPayloads layers "root.usda" 16 (P ["P"])
          let child := composeWithLoadPolicy loadPayloads layers "root.usda" 16 (P ["P", "Child"])
          let classes := if localClasses.isEmpty then "" else targetsText "class"
          cases := cases.push (Json.mkObj [
            ("id", toJson (field ++ "-s" ++ toString si ++ "-w" ++ toString wi ++
              (if loadPayloads then "-loaded" else "-deferred"))),
            ("query", toJson "/P"),
            ("loadPayloads", toJson loadPayloads),
            ("root", toJson ("#usda 1.0\n(subLayers = [@weak.usda@])\n" ++
              classes ++ primText field strong)),
            ("weak", toJson ("#usda 1.0\n" ++ primText field weak)),
            ("asset", toJson ("#usda 1.0\n" ++ targetsText "def")),
            ("expected", Json.arr (expected.map siteJson).toArray),
            ("expectedChild", Json.arr (child.map siteJson).toArray)])
  return Json.mkObj [("generator", toJson "Gen.Vectors.PrimIndex"),
    ("scope", toJson "layered arc edits, ancestral children, global payload loading"),
    ("cases", Json.arr cases)]


/-! ## Mechanism cases: implied classes, variant selection provenance, payload
deferral. The expected Prim Stack is `compose` from the Lean model. -/

def mechSpec (path : Path) (f : PrimSpec → PrimSpec := id) : PrimSpec := f (sp path)

structure Mech where
  id : String
  query : String
  loadPayloads : Bool
  files : List (String × String)   -- name, usda text
  layers : Layers
  path : Path
  impliedArcs : Bool := false
  relocates : List UsdSpec.Relocates.Relocate := []

def mechCases : List Mech :=
  -- Implied inherit through a reference, with and without a root class override.
  [true, false].map (fun override =>
    let rootClass := if override then "class \"C\" {}\n" else ""
    { id := "implied-inherit-" ++ (if override then "override" else "plain")
      query := "/Inst", loadPayloads := true,
      files := [("root.usda", "#usda 1.0\n" ++ rootClass ++
                  "def \"Inst\" (references = @ref.usda@</Ref>) {}\n"),
                ("ref.usda", "#usda 1.0\nclass \"C\" {}\ndef \"Ref\" (inherits = </C>) {}\n")]
      layers := [{ name := "root.usda",
                   specs := (if override then [sp (P ["C"])] else []) ++
                     [{ path := P ["Inst"], references := [(some "ref.usda", P ["Ref"])] }] },
                 { name := "ref.usda",
                   specs := [sp (P ["C"]), { path := P ["Ref"], inherits := [P ["C"]] }] }]
      path := P ["Inst"] }) ++
  -- Implied class whose root-stack declaration inherits another class.
  [{ id := "implied-chain", query := "/Inst", loadPayloads := true,
     files := [("root.usda", "#usda 1.0\nclass \"D\" {}\nclass \"C\" (inherits = </D>) {}\n" ++
                  "def \"Inst\" (references = @ref.usda@</Ref>) {}\n"),
               ("ref.usda", "#usda 1.0\nclass \"C\" {}\ndef \"Ref\" (inherits = </C>) {}\n")]
     layers := impliedChainL, path := P ["Inst"], impliedArcs := true : Mech }] ++
  -- Relocation: destination receives the source's arc-derived opinions; the
  -- root-stack opinion at the old source path is suppressed, and the source
  -- path itself no longer exists.
  [("dest", P ["Root", "B"]), ("source", P ["Root", "A"])].map (fun (name, q) =>
    { id := "relocate-" ++ name, query := if name == "dest" then "/Root/B" else "/Root/A",
      loadPayloads := true,
      files := [("root.usda", "#usda 1.0\n(\n  relocates = { </Root/A>: </Root/B> }\n)\n" ++
                  "def \"Root\" (references = @ref.usda@</R>)\n{\n  over \"A\"\n  {\n    int x = 1\n  }\n" ++
                  "  over \"B\"\n  {\n    int y = 2\n  }\n}\n"),
                ("ref.usda", "#usda 1.0\ndef \"R\"\n{\n  def \"A\"\n  {\n    int z = 3\n  }\n}\n")]
      layers := [{ name := "root.usda",
                   specs := [{ path := P ["Root"], references := [(some "ref.usda", P ["R"])] },
                             sp (P ["Root", "A"]), sp (P ["Root", "B"])] },
                 { name := "ref.usda", specs := [sp (P ["R"]), sp (P ["R", "A"])] }]
      path := q, relocates := [⟨["Root", "A"], some ["Root", "B"]⟩] }) ++
  -- Variant-context cycle: the selected option references its own host prim.
  [{ id := "variant-cycle", query := "/P", loadPayloads := true,
     files := [("root.usda", "#usda 1.0\ndef \"P\" (\n  variantSets = \"v\"\n  variants = {string v = \"a\"}\n)\n{\n" ++
                  "  variantSet \"v\" = {\n    \"a\" (\n      references = </P>\n    ) {\n    }\n  }\n}\n")]
     layers := [{ name := "root.usda",
                  specs := [{ path := P ["P"], variantSets := ["v"], selections := [("v", "a")] },
                            { path := [.prim "P", .variant "v" "a"], references := [(none, P ["P"])] }] }]
     path := P ["P"] : Mech }] ++
  -- Payload target contributes only when loaded.
  [true, false].map (fun load =>
    { id := "payload-" ++ (if load then "loaded" else "deferred")
      query := "/P", loadPayloads := load,
      files := [("root.usda", "#usda 1.0\ndef \"P\" (payload = @pay.usda@</T>) {}\n"),
                ("pay.usda", "#usda 1.0\ndef \"T\" {}\n")]
      layers := [{ name := "root.usda",
                   specs := [{ path := P ["P"], payloads := [(some "pay.usda", P ["T"])] }] },
                 { name := "pay.usda", specs := [sp (P ["T"])] }]
      path := P ["P"] }) 

  ++
  -- Strongest authored selection wins (provenance); weak layer's differs.
  [("strong", "a", "b"), ("weak-only", "", "b")].map (fun (name, strongSel, weakSel) =>
    let vs := "  variantSet \"v\" = {\n    \"a\" {}\n    \"b\" {}\n  }\n"
    let sel := fun (x : String) => " (\n  variantSets = \"v\"\n" ++
      (if x.isEmpty then "" else "  variants = {string v = \"" ++ x ++ "\"}\n") ++ ")"
    let mk := fun (x : String) => "def \"P\"" ++ sel x ++ " {\n" ++ vs ++ "}\n"
    let sels := fun (x : String) => if x.isEmpty then [] else [("v", x)]
    let chosen := if strongSel.isEmpty then weakSel else strongSel
    { id := "variant-selection-" ++ name
      query := "/P", loadPayloads := true,
      files := [("root.usda", "#usda 1.0\n(subLayers = [@weak.usda@])\n" ++ mk strongSel),
                ("weak.usda", "#usda 1.0\n" ++ mk weakSel)]
      layers := [{ name := "root.usda", sublayers := ["weak.usda"],
                   specs := [{ path := P ["P"], variantSets := ["v"], selections := sels strongSel },
                             sp [.prim "P", .variant "v" chosen]] },
                 { name := "weak.usda",
                   specs := [{ path := P ["P"], variantSets := ["v"], selections := sels weakSel },
                             sp [.prim "P", .variant "v" chosen]] }]
      path := P ["P"] })

def mechJson (m : Mech) : Json :=
  let expected := if !m.relocates.isEmpty then
      (if relocatedAway m.relocates m.path then [] else composeAtDestination m.relocates m.layers "root.usda" m.path)
    else if m.impliedArcs then composeImplied m.layers "root.usda" m.path
    else composeWithLoadPolicy m.loadPayloads m.layers "root.usda" 16 m.path
  Json.mkObj [("id", toJson m.id), ("query", toJson m.query),
    ("loadPayloads", toJson m.loadPayloads),
    ("files", Json.mkObj (m.files.map fun (n, t) => (n, toJson t))),
    ("expected", Json.arr (expected.map siteJson).toArray)]

def mechVectors : Json :=
  Json.mkObj [("generator", toJson "Gen.Vectors.PrimIndex"),
    ("scope", toJson "implied inherit through reference, payload deferral"),
    ("cases", Json.arr (mechCases.map mechJson).toArray)]

def files : List (String × String) := [("prim-index-vectors.json", vectors.pretty ++ "\n"),
  ("prim-index-mechanism-vectors.json", mechVectors.pretty ++ "\n")]
end Gen.PrimIndexVec
