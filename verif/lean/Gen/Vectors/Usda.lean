import UsdSpec.Usda.Rules
import Gen.Inc

/-!
USDA PEG acceptance vectors (milestone M5).

Row: `LEAN_USDA(id, source, text, peg_literal, peg_repaired, expect, note)`

* `source`: a repo-relative file (`tests/usda/...`; `text` is `""`) or `inline:<name>`
  (the layer is `text`).
* `peg_literal`: the spec PEG exactly as written (`UsdSpec.Usda.Peg.usda`, entry
  `LayerSpec`) consumes the whole input. `peg_repaired`: the same with `repairs`
  (SPEC-ISSUE LEAN-802, LEAN-805..810).
* `expect`, for LightUSD `LoadUSDAFromString` with
  `LoadOptions.parse_options.strict_aousd_conformance = true`:
  `1` must succeed (repaired PEG accepts; inline rows: pxr 26.11 also accepts),
  `0` must fail (repaired PEG and pxr 26.11 both reject),
  `-1` no expectation (PEG and pxr disagree: legacy syntax or a pxr semantic error).
  Never test against `peg_literal`: its extra rejections are spec defects.

File verdicts were recorded by the acceptance harness (`lake env lean --run` over this
grammar); inline verdicts are computed here by `UsdSpec.Usda.Peg.accepts`.
-/
namespace Gen.UsdaVec
open UsdSpec.Usda.Peg

structure FileRow where
  id : String
  path : String
  literal : Bool
  repaired : Bool
  expect : Int
  note : String

/-- Recorded harness verdicts for the corpus files. -/
def fileRows : List FileRow := [
  ⟨"alpha-billboard-sidedness", "tests/usda/alpha-billboard-sidedness.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd-defaultprim-reference", "tests/usda/aousd-defaultprim-reference.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd-defaultprim-target", "tests/usda/aousd-defaultprim-target.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd-namespace-order", "tests/usda/aousd-namespace-order.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd-unknown-property-metadata", "tests/usda/aousd-unknown-property-metadata.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd11-instancing-ancestral-arcs-asset", "tests/usda/aousd11-instancing-ancestral-arcs-asset.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ancestral-arcs-group_1", "tests/usda/aousd11-instancing-ancestral-arcs-group_1.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ancestral-arcs-group_2", "tests/usda/aousd11-instancing-ancestral-arcs-group_2.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ancestral-arcs", "tests/usda/aousd11-instancing-ancestral-arcs.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd11-instancing-ignored-arcs-asset", "tests/usda/aousd11-instancing-ignored-arcs-asset.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ignored-arcs-extra", "tests/usda/aousd11-instancing-ignored-arcs-extra.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ignored-arcs", "tests/usda/aousd11-instancing-ignored-arcs.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd11-instancing-ignored-opinions-asset", "tests/usda/aousd11-instancing-ignored-opinions-asset.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ignored-opinions-asset_ref", "tests/usda/aousd11-instancing-ignored-opinions-asset_ref.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-ignored-opinions", "tests/usda/aousd11-instancing-ignored-opinions.usda", true, true, 1, "accepted as written"⟩,
  ⟨"aousd11-instancing-implied-arcs-shared-asset", "tests/usda/aousd11-instancing-implied-arcs-shared-asset.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-implied-arcs-shared-group_1", "tests/usda/aousd11-instancing-implied-arcs-shared-group_1.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-implied-arcs-shared-group_2", "tests/usda/aousd11-instancing-implied-arcs-shared-group_2.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"aousd11-instancing-implied-arcs-shared", "tests/usda/aousd11-instancing-implied-arcs-shared.usda", true, true, 1, "accepted as written"⟩,
  ⟨"ar-anchoring", "tests/usda/ar-anchoring.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"array-comments-int", "tests/usda/array-comments-int.usda", true, true, 1, "accepted as written"⟩,
  ⟨"array-newline-000", "tests/usda/array-newline-000.usda", false, true, 1, "literal grammar rejects (LEAN-805); repaired accepts"⟩,
  ⟨"attr-metadataum-displayGroup", "tests/usda/attr-metadataum-displayGroup.usda", false, true, 1, "literal grammar rejects (LEAN-805); repaired accepts"⟩,
  ⟨"blendshape-001", "tests/usda/blendshape-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"c-core-relationships", "tests/usda/c-core-relationships.usda", true, true, 1, "accepted as written"⟩,
  ⟨"colorspace-materialx-config-texture-render", "tests/usda/colorspace-materialx-config-texture-render.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"customattr-001", "tests/usda/customattr-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"dedup-global-001", "tests/usda/dedup-global-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"dome-light-1", "tests/usda/dome-light-1.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"feat-livrps-inherits-main", "tests/usda/feat-livrps-inherits-main.usda", true, true, 1, "accepted as written"⟩,
  ⟨"feat-variant-chain-main", "tests/usda/feat-variant-chain-main.usda", true, true, 1, "accepted as written"⟩,
  ⟨"geometry-optimize-authored-semantics-001", "tests/usda/geometry-optimize-authored-semantics-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"geommesh-002", "tests/usda/geommesh-002.usda", false, true, 1, "literal grammar rejects (LEAN-805); repaired accepts"⟩,
  ⟨"inlined-intvec-001", "tests/usda/inlined-intvec-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"interpolation-attribute-003", "tests/usda/interpolation-attribute-003.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"lusdrender-mixed-flat-curve", "tests/usda/lusdrender-mixed-flat-curve.usda", false, true, 1, "literal grammar rejects (LEAN-805); repaired accepts"⟩,
  ⟨"lusdview-materialx-cold-promotion", "tests/usda/lusdview-materialx-cold-promotion.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"lusdview-stacked-glass", "tests/usda/lusdview-stacked-glass.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"material-binding-none-001", "tests/usda/material-binding-none-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"material-optimize-unique-001", "tests/usda/material-optimize-unique-001.usda", false, true, 1, "literal grammar rejects (LEAN-807); repaired accepts"⟩,
  ⟨"multi-attribute-005", "tests/usda/multi-attribute-005.usda", false, true, 1, "literal grammar rejects (LEAN-807); repaired accepts"⟩,
  ⟨"payload-000", "tests/usda/payload-000.usda", true, true, 1, "accepted as written"⟩,
  ⟨"physics-collision-group-004", "tests/usda/physics-collision-group-004.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"physics-mjc-joint-authorship-001", "tests/usda/physics-mjc-joint-authorship-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"physics-schema-defaults-001", "tests/usda/physics-schema-defaults-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"prim-meta-add-variantSets-000", "tests/usda/prim-meta-add-variantSets-000.usda", false, false, -1, "legacy syntax (`add` list op, L11443); not normative"⟩,
  ⟨"references-006", "tests/usda/references-006.usda", true, true, 1, "accepted as written"⟩,
  ⟨"rel-array-001", "tests/usda/rel-array-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"rotate-001", "tests/usda/rotate-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"shader-opacityMode-000", "tests/usda/shader-opacityMode-000.usda", true, true, 1, "accepted as written"⟩,
  ⟨"shader-varname-connect-001", "tests/usda/shader-varname-connect-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"specializes-001", "tests/usda/specializes-001.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"spectral-ior-001", "tests/usda/spectral-ior-001.usda", false, true, 1, "literal grammar rejects (LEAN-805); repaired accepts"⟩,
  ⟨"spectral-stage-metadata-002", "tests/usda/spectral-stage-metadata-002.usda", false, true, 1, "literal grammar rejects (LEAN-806); repaired accepts"⟩,
  ⟨"stage-meta-only-001", "tests/usda/stage-meta-only-001.usda", true, true, 1, "accepted as written"⟩,
  ⟨"string-escape-004", "tests/usda/string-escape-004.usda", false, true, 1, "literal grammar rejects (LEAN-809/810); repaired accepts"⟩,
  ⟨"sublayers-006", "tests/usda/sublayers-006.usda", true, true, 1, "accepted as written"⟩,
  ⟨"timesamples-array-bool-001", "tests/usda/timesamples-array-bool-001.usda", false, true, 1, "literal grammar rejects (LEAN-807); repaired accepts"⟩
]

/-- Constructed snippets: (name, text, pxr 26.11 usdcat accepts). -/
def snippets : List (String × String × Bool) := [
  ("autoease_name", "#usda 1.0\ndef \"a\" {\n  float autoEase = 1\n}\n", true),
  ("close_same_line", "#usda 1.0\ndef \"a\" {\n  def \"b\" {\n  }}\n", false),
  ("customdata", "#usda 1.0\ndef \"a\" (\n  customData = {int x = 1}\n)\n{\n}\n", true),
  ("empty_listop", "#usda 1.0\ndef \"a\" (\n  prepend apiSchemas = []\n)\n{\n}\n", false),
  ("kw_attrname", "#usda 1.0\ndef \"a\" {\n  token def = \"x\"\n}\n", true),
  ("listop_noassign", "#usda 1.0\n(\n  append rel myRel\n)\n", false),
  ("ml_newline", "#usda 1.0\ndef \"a\" {\n  string s = \"\"\"a\nb\"\"\"\n}\n", true),
  ("noeol", "#usda 1.0\ndef \"a\" {\n}", true),
  ("prop_close_same_line", "#usda 1.0\ndef \"a\" {\n  int x = 1 }\n", false),
  ("rel_default", "#usda 1.0\ndef \"a\" {\n  rel r.default = </a>\n}\n", true),
  ("tab_in_string", "#usda 1.0\ndef \"a\" {\n  string s = \"a\tb\"\n}\n", true),
  ("trailing_dot", "#usda 1.0\ndef \"a\" {\n  float x = 1.\n}\n", true),
  ("varcomma", "#usda 1.0\n\ndef \"Ref\" (\n    variantSets = [\"v\"]\n    variants = {\n        string v = \"y\"\n    }\n)\n{\n    variantSet \"v\" = {\n        \"x\" {\n            def \"X\"\n            {\n            }\n        },\n        \"y\" {\n            def \"Y\"\n            {\n            }\n        }\n    }\n}\n", false),
  ("varying", "#usda 1.0\ndef \"a\" {\n  varying float x = 1\n}\n", false)
]

/-- pxr 26.11 usdcat verdict for each spec example in `specExamples`. -/
def specPxr : List (String × Bool) := [
  ("spec-L10322", true),
  ("spec-L10345", false),
  ("spec-L10978", true),
  ("spec-L11020", true),
  ("spec-L11073", true),
  ("spec-L11243", false),
  ("spec-L11354", true),
  ("spec-L4305", true),
  ("spec-L4322", true),
  ("spec-L4338", true),
  ("spec-L4348", true),
  ("spec-L4421", true),
  ("spec-L4438", true),
  ("spec-L4464", true),
  ("spec-L4484", true),
  ("spec-L4496", true),
  ("spec-L4506", true),
  ("spec-L4557", true),
  ("spec-L4577", true),
  ("spec-L4589", true),
  ("spec-L4599", true),
  ("spec-L4664", true),
  ("spec-L4680", true),
  ("spec-L4692", false),
  ("spec-L4741", true),
  ("spec-L4753", false),
  ("spec-L4829", true),
  ("spec-L4844", false),
  ("spec-L4934", true),
  ("spec-L4952", true),
  ("spec-L4973", true),
  ("spec-L4993", true),
  ("spec-L5086", true),
  ("spec-L5102", true),
  ("spec-L5112", true),
  ("spec-L5132", true),
  ("spec-L5152", true),
  ("spec-L5189", true),
  ("spec-L5206", true),
  ("spec-L5219", true),
  ("spec-L5239", true),
  ("spec-L5260", true),
  ("spec-L5288", false),
  ("spec-L5320", true),
  ("spec-L5332", false),
  ("spec-L5552", true),
  ("spec-L5568", true),
  ("spec-L5583", true),
  ("spec-L5633", true),
  ("spec-L5646", true),
  ("spec-L5665", true),
  ("spec-L5684", true),
  ("spec-L5709", true),
  ("spec-L5728", true),
  ("spec-L5751", true),
  ("spec-L5774", true),
  ("spec-L6472", true),
  ("spec-L6515", true),
  ("spec-L6545", false),
  ("spec-L6698", true),
  ("spec-L6736", true),
  ("spec-L6759", true),
  ("spec-L6783", true),
  ("spec-L6810", true),
  ("spec-L6927", true),
  ("spec-L7125", true),
  ("spec-L7155", true),
  ("spec-L7409", true),
  ("spec-L7427", true),
  ("spec-L7492", true),
  ("spec-L8418", true),
  ("spec-L8580", true),
  ("spec-L8698", true),
  ("spec-L8831", true)
]

def fuel : Nat := 1000000

def expectOf (rep pxr : Bool) : Int := if rep && pxr then 1 else if !rep && !pxr then 0 else -1

def noteOf (lit rep pxr : Bool) : String :=
  if lit && rep && pxr then "accepted as written"
  else if rep && pxr then "literal PEG rejects (spec defect LEAN-805..810); repaired accepts"
  else if !rep && !pxr then "rejected by the PEG and by pxr"
  else if rep then "PEG accepts, pxr rejects (semantic error, not grammar)"
  else "PEG rejects, pxr accepts"

def inlineRow (name text : String) (pxr : Bool) : String :=
  let lit := accepts usda "LayerSpec" text fuel
  let rep := accepts usdaRepaired "LayerSpec" text fuel
  Gen.row "LEAN_USDA" [Gen.cstr name, Gen.cstr ("inline:" ++ name), Gen.cstr text, toString lit,
    toString rep, toString (expectOf rep pxr), Gen.cstr (noteOf lit rep pxr)]

def content : String := Id.run do
  let mut s := Gen.header "Gen/Vectors/Usda.lean"
  s := s ++ "// LEAN_USDA(id, source, text, peg_literal, peg_repaired, expect, note)\n"
  s := s ++ "// expect: 1 strict LoadUSDAFromString must succeed, 0 must fail, -1 no expectation.\n"
  for r in fileRows do
    s := s ++ Gen.row "LEAN_USDA" [Gen.cstr r.id, Gen.cstr r.path, Gen.cstr "", toString r.literal,
      toString r.repaired, toString r.expect, Gen.cstr r.note]
  for (n, t) in specExamples do
    s := s ++ inlineRow n t ((specPxr.lookup n).getD true)
  for (n, t, pxr) in snippets do
    s := s ++ inlineRow n t pxr
  return s

def files : List (String × String) := [("usda-peg-vectors.inc", content)]

end Gen.UsdaVec
