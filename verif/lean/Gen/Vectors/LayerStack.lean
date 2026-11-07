import UsdSpec.Composition.PrimIndex
import Lean.Data.Json

namespace Gen.LayerStackVec
open Lean UsdSpec.Composition.PrimIndex

def layer (name : String) (children : List String) : Layer :=
  { name, sublayers := children, specs := [sp (P ["P"])] }

def twoChoices : List (List String) :=
  [[], ["root.usda"], ["a.usda"], ["root.usda", "a.usda"], ["a.usda", "root.usda"]]

def threeChoices : List (List String) :=
  [[], ["root.usda"], ["a.usda"], ["b.usda"]]

def chainName (n : Nat) : String := if n == 0 then "root.usda" else "l" ++ toString n ++ ".usda"

def graphs : List (String × Layers) := Id.run do
  let mut out := []
  for (i, r) in enum twoChoices do
    for (j, a) in enum twoChoices do
      out := out ++ [(s!"two-r{i}-a{j}", [layer "root.usda" r, layer "a.usda" a])]
  for (i, r) in enum threeChoices do
    for (j, a) in enum threeChoices do
      for (k, b) in enum threeChoices do
        out := out ++ [(s!"three-r{i}-a{j}-b{k}",
          [layer "root.usda" r, layer "a.usda" a, layer "b.usda" b])]
  out := out ++ [
    ("diamond", [layer "root.usda" ["a.usda", "b.usda"],
      layer "a.usda" ["c.usda"], layer "b.usda" ["c.usda"], layer "c.usda" []]),
    ("diamond-cycle", [layer "root.usda" ["a.usda", "b.usda"],
      layer "a.usda" ["c.usda"], layer "b.usda" ["c.usda"], layer "c.usda" ["a.usda"]]),
    ("missing-sibling", [layer "root.usda" ["missing.usda", "a.usda"], layer "a.usda" []]),
    ("cycle-and-missing", [layer "root.usda" ["root.usda", "missing.usda"]]),
    ("deep-25-layers", (List.range 25).map fun n =>
      layer (chainName n) (if n == 24 then [] else [chainName (n+1)]))]
  return out

def layerText (l : Layer) (marker : Nat) : String :=
  "#usda 1.0\n" ++
  (if l.sublayers.isEmpty then "" else
    "(subLayers = [" ++ ", ".intercalate (l.sublayers.map fun n => "@" ++ n ++ "@") ++ "])\n") ++
  "def \"P\" { int marker = " ++ toString marker ++ "\n}\n"

def issueJson : LayerStackIssue → Json
  | .missing name => Json.mkObj [("kind", toJson "missing"), ("layer", toJson name)]
  | .cycle name => Json.mkObj [("kind", toJson "cycle"), ("layer", toJson name)]

def siteJson (site : UsdSpec.Strength.Site) : Json :=
  Json.mkObj [("layer", toJson site.layer), ("path", toJson site.path)]

def vectors : Json := Json.mkObj [
  ("generator", toJson "Gen.Vectors.LayerStack"),
  ("cases", Json.arr (graphs.map fun (id, ls) =>
    let checked := layerStackChecked ls "root.usda"
    Json.mkObj [("id", toJson id),
      ("files", Json.mkObj ((enum ls).map fun (i, l) => (l.name, toJson (layerText l i)))),
      ("layers", toJson checked.layers),
      ("issues", Json.arr (checked.issues.map issueJson).toArray),
      ("expected", Json.arr ((compose ls "root.usda" 16 (P ["P"])).map siteJson).toArray)]).toArray)]

def files : List (String × String) := [("layer-stack-vectors.json", vectors.pretty ++ "\n")]
end Gen.LayerStackVec
