import UsdSpec.Path.Grammar
import UsdSpec.Schema.Family
import Gen.Inc

/-!
Path and schema-name vectors.

* `LEAN_PATH_VALID(id, str, valid, printed)`: `parsePath str` succeeds iff
  `valid`; `printed` is `printPath` of the result (canonical form). Checked
  against `Path::Parse(str).is_valid()` and its string form.
* `LEAN_SCHEMA_FAMILY(id, name, family, has_version, version)`: Lean
  `parseFamilyVersion`. Checked against `ParseSchemaFamilyAndVersion`.
-/
namespace Gen.PathSchemaVec
open UsdSpec

def pathSamples : List String :=
  [ "/", ".", "..", "../..", "../../A/B", "/A", "/A/B", "/A/B.c", "/A.b", "/A.b:c:d",
    "A", "A/B", "A.b", ".b", "../A.b", "/A{v=x}", "/A{v=x}B", "/A{v=x}B/C", "/A/B{v=x}C",
    "/A{v=x}/B", "/A{v=}", "/A{v=x}{w=y}", "/A{v=x.y-z|w}", "/A{ v = x }", "/A.b.c", "a.b.c",
    "/A/", "//A", "/A//B", "/1A", "/_A", "/A_1", "/München", "/A-B", "/A.b:", "/A.:b",
    "/A{v=x}.p", "/Root/Child/Grandchild{size=1}/GreatGrandchild",
    "/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1", "", " ", "/A B", "/A..",
    "/A/..", "/A/../B", "../", "/..", "/.", "/A.b[/C]" ]

def schemaSamples : List String :=
  [ "Foo", "Foo_1", "Foo_12", "Foo_0", "Foo_01", "Foo_1_2", "_1", "Foo_", "Foo_1a",
    "Gaussian_Splatting_3", "CollectionAPI", "MaterialBindingAPI_1", "A", "a_b_3",
    "Foo__2", "_Foo", "_Foo_7", "X_4294967295" ]

def pathRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for s in pathSamples do
    let (ok, pr) := match PathGrammar.parsePath s with
      | some p => ("1", PathGrammar.printPath p)
      | none => ("0", "")
    out := out ++ row "LEAN_PATH_VALID" [toString id, cstr s, ok, cstr pr]
    id := id + 1
  return out

def schemaRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for s in schemaSamples do
    let (fam, hv, v) := match Schema.parseFamilyVersion s with
      | some (f, some n) => (f, "1", toString n)
      | some (f, none) => (f, "0", "0")
      | none => ("", "0", "0")
    out := out ++ row "LEAN_SCHEMA_FAMILY" [toString id, cstr s, cstr fam, hv, v]
    id := id + 1
  return out

def files : List (String × String) :=
  [ ("path-valid-vectors.inc", header "Gen.Vectors.PathSchema" ++
      "// LEAN_PATH_VALID(id, str, valid, printed): UsdSpec.PathGrammar.parsePath (ch 8 PEG, L3617-3772,\n" ++
      "// with the LEAN-202/206 corrections: mixed '/' + '{v=x}' and repeated '..' accepted)\n" ++ pathRows),
    ("schema-family-vectors.inc", header "Gen.Vectors.PathSchema" ++
      "// LEAN_SCHEMA_FAMILY(id, name, family, has_version, version): parseFamilyVersion (L8252-8294)\n" ++
      schemaRows) ]

end Gen.PathSchemaVec
