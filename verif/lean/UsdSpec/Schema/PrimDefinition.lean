import Mathlib.Data.List.Basic

/-!
# The prim definition (AOUSD USD Core Specification v1.1.0, §11)

SPEC: core_spec.md L8737-8809 (`build_prim_definition`,
`compose_prim_definition`), L8811-8888 (composed prim definition, examples),
L8890-8950 (fallback value resolution, schema orderings L8922 / L8949);
schema definitions used by the examples: L8380-8410 (`Foo`, `Bar`),
L8535-8560 (`FooBar`, `BarBaz`), L8677-8690 (`BazFoo`), inclusion rules
L8614-8646.

The pseudocode is transcribed as executable Lean with fuel (`build` returns
`none` when the fuel runs out, which is how a non-terminating recursion shows).
The unstated helpers are filled in as the prose describes them:

* `get_properties_from_schema_type`: the schema's properties plus those of its
  typed ancestors (`inherits`, L8380-8430), with `<instance_name>` replaced by
  the instance name for multiple-apply schemas (L8516-8528);
* `get_built_in_inclusion_list` / `get_auto_applies_inclusion_list`: the
  declared lists, with a multiple-apply schema named by type receiving the
  defining schema's instance name (L8635-8638) and one named by instance being
  prefixed with it (L8639-8641);
* auto-applies are read as the pseudocode and the examples read them (pulled in
  by the defining schema); the contradiction with their definition is
  `issues/aousd/06-auto-applies-direction.md` and is not re-recorded;
* `compose_prim_definition` copies only `default` and `hidden` into an
  existing property — `issues/aousd/07-prim-definition-field-merge.md` (also the
  `propeerties` typo, L8801); kept as written.

Confirmed literal-pseudocode findings, SPEC-ISSUE: LEAN-311:
1. schema lists are duplicated: `build_prim_definition` appends the inclusions
   to `schemas` (L8769) and `compose_prim_definition` appends the included
   definition's `schemas`, which starts with the same name (L8791,
   L8761) — `l8922_schemas`;
2. the L8949 ordering omits `BarBaz:otherInstance`, which `BazFoo`'s built-in
   `BarBaz` (L8677-8690) brings in exactly as `BarBaz:bazfooinstance` is
   brought in by the L8706-8711 example — `l8949_schemas`;
3. the recursion has no cycle guard: the comment says "build its prim
   definition (if not already built)" (L8772-8774) but nothing records what was
   built; two schemas that include each other diverge — `cycle_diverges`.
   This theorem concerns the literal model without an implicit cache; it is not
   a claim that OpenUSD diverges. Clean v26.11.1 schemaRegistry.cpp explicitly
   tracks Building/Complete states and skips cyclic inclusions, while
   primDefinition.cpp deduplicates applied schema family/instance/version.
   Stock built-in definition controls have unique expanded schema lists; they
   do not recreate the hypothetical FooBar schemas in a runtime plugin.
-/

namespace UsdSpec.Schema.PrimDefinition

abbrev Fields := List (String × String)

/-- A property-name part: literal text or the `<instance_name>` placeholder. -/
inductive NamePart
  | lit (s : String)
  | inst
  deriving DecidableEq, Repr

structure Property where
  name : List NamePart
  fields : Fields
  deriving DecidableEq, Repr

/-- A schema as named on a prim / in an inclusion list: base name and optional
instance name (`BarBaz:myInstance` = `⟨"BarBaz", some "myInstance"⟩`, L8524-8528). -/
abbrev SchemaRef := String × Option String

structure SchemaDef where
  name : String
  isMultiple : Bool := false
  inherits : Option String := none
  properties : List Property := []
  overrides : List Property := []
  builtIns : List SchemaRef := []
  autoApplies : List SchemaRef := []
  deriving DecidableEq, Repr

abbrev Registry := List SchemaDef

structure PrimDef where
  schemas : List SchemaRef
  properties : List Property
  deriving DecidableEq, Repr

def lookup (reg : Registry) (base : String) : Option SchemaDef := reg.find? (·.name == base)

/-- Replace `<instance_name>`. -/
def instProp (inst : Option String) (p : Property) : Property :=
  match inst with
  | some i => { p with name := p.name.map fun e => match e with | .inst => .lit i | e => e }
  | none => p

/-- `get_properties_from_schema_type` (own properties first, then typed ancestors). -/
def propsOf (reg : Registry) : Nat → SchemaRef → List Property
  | 0, _ => []
  | f + 1, (b, inst) =>
    match lookup reg b with
    | none => []
    | some d =>
      d.properties.map (instProp inst) ++ (d.inherits.map (fun t => propsOf reg f (t, none)) |>.getD [])

def overridesOf (reg : Registry) (s : SchemaRef) : List Property :=
  let (b, inst) := s
  ((lookup reg b).map (·.overrides) |>.getD []).map (instProp inst)

/-- Inclusion naming for multiple-apply targets (L8630-8646). -/
def resolveIncl (reg : Registry) (definingInst : Option String) (incl : SchemaRef) : SchemaRef :=
  let (b, i) := incl
  let multiple := ((lookup reg b).map (·.isMultiple)).getD false
  match multiple, definingInst, i with
  | true, some di, none => (b, some di)
  | true, some di, some j => (b, some (di ++ ":" ++ j))
  | _, _, _ => incl

def inclusions (reg : Registry) (s : SchemaRef) : List SchemaRef :=
  let (b, inst) := s
  match lookup reg b with
  | none => []
  | some d =>
    let definingInst := if d.isMultiple then inst else none
    (d.builtIns ++ d.autoApplies).map (resolveIncl reg definingInst)

def hasField (fs : Fields) (k : String) : Bool := fs.any (·.1 == k)
def getField (fs : Fields) (k : String) : Option String := (fs.find? (·.1 == k)).map (·.2)
def setField (fs : Fields) (k : String) : Option String → Fields
  | some v => if hasField fs k then fs.map (fun e => if e.1 == k then (k, v) else e) else fs ++ [(k, v)]
  | none => fs.filter (·.1 != k)

def findProp (ps : List Property) (n : List NamePart) : Option Property := ps.find? (·.name == n)

def updateProp (ps : List Property) (n : List NamePart) (g : Property → Property) : List Property :=
  ps.map fun p => if p.name == n then g p else p

/-- `compose_prim_definition` (L8789-8809), as written. -/
def composeDef (s w : PrimDef) : PrimDef :=
  let schemas := s.schemas ++ w.schemas
  let props := w.properties.foldl (fun ps wp =>
    let ps := if (findProp ps wp.name).isSome then ps else ps ++ [wp]
    ["default", "hidden"].foldl (fun ps f =>
      match findProp ps wp.name with
      | some sp => if hasField sp.fields f then ps
                   else updateProp ps wp.name fun p => { p with fields := setField p.fields f (getField wp.fields f) }
      | none => ps) ps) s.properties
  { schemas, properties := props }

/-- Override properties (L8778-8785): every field but variability/typeName. -/
def overrideField (o : Property) (ps : List Property) (kv : String × String) : List Property :=
  if kv.1 == "variability" || kv.1 == "typeName" then ps
  else updateProp ps o.name fun p => { p with fields := setField p.fields kv.1 (some kv.2) }

def applyOverrides (pd : PrimDef) (ovs : List Property) : PrimDef :=
  { pd with properties := ovs.foldl (fun ps o => o.fields.foldl (overrideField o) ps) pd.properties }

/-- `build_prim_definition` (L8755-8787), fuel-bounded. -/
def build (reg : Registry) : Nat → SchemaRef → Option PrimDef
  | 0, _ => none
  | f + 1, s => do
    let incl := inclusions reg s
    let pd₀ : PrimDef := { schemas := [s] ++ incl, properties := propsOf reg (f + 1) s }
    let pd ← incl.foldlM (fun pd a => do let w ← build reg f a; pure (composeDef pd w)) pd₀
    pure (applyOverrides pd (overridesOf reg s))

/-- The composed prim's definition (L8811-8818): authored properties, then
each schema (typed first, then applied in order) composed as weaker. -/
def primDef (reg : Registry) (fuel : Nat) (authored : List Property) (schemas : List SchemaRef) :
    Option PrimDef :=
  schemas.foldlM (fun pd s => do let w ← build reg fuel s; pure (composeDef pd w))
    { schemas := [], properties := authored }

/-- Fallback for field `k` of property `n` (L8890-8925): read off the composed
definition (the strongest schema providing the field wins). -/
def fallback (pd : PrimDef) (n : List NamePart) (k : String) : Option String :=
  (findProp pd.properties n).bind fun p => getField p.fields k

/-! ## The spec's schemas and examples -/

section Examples

def prop (n : List NamePart) (t d : String) : Property := ⟨n, [("typeName", t), ("default", d)]⟩
def L (s : String) : List NamePart := [.lit s]

def Foo : SchemaDef := { name := "Foo", properties := [prop (L "fooprop") "int" "0"] }
def Bar : SchemaDef := { name := "Bar", inherits := some "Foo", properties := [prop (L "barprop") "float" "1.0"] }
def BarBaz : SchemaDef :=
  { name := "BarBaz", isMultiple := true, properties := [prop [.inst, .lit ":barbazprop"] "string" "\"\""] }
def BazFoo : SchemaDef :=
  { name := "BazFoo", isMultiple := true, properties := [prop [.lit "bazfooprop:", .inst] "int" "1"],
    builtIns := [("BarBaz", none)] }
/-- `FooBar` with the L8856-8871 inclusion. -/
def FooBar₁ : SchemaDef :=
  { name := "FooBar", properties := [prop (L "foobarprop") "half" "0.5"], builtIns := [("BarBaz", some "myInstance")] }
/-- `FooBar` with the L8930-8947 inclusions. -/
def FooBar₂ : SchemaDef := { FooBar₁ with autoApplies := [("BazFoo", some "otherInstance")] }
/-- `FooBar` with the L8650-8662 inclusion. -/
def FooBar₀ : SchemaDef :=
  { name := "FooBar", properties := [prop (L "foobarprop") "half" "0.5"], autoApplies := [("BazFoo", some "bazfooinstance")] }

def reg₀ : Registry := [Foo, Bar, FooBar₀, BarBaz, BazFoo]
def reg₁ : Registry := [Foo, Bar, FooBar₁, BarBaz, BazFoo]
def reg₂ : Registry := [Foo, Bar, FooBar₂, BarBaz, BazFoo]

/-- `def Bar "myFoo" (apiSchemas = ["FooBar"]) { double myFoo:prop = 5.0 }` (L8830-8838). -/
def myFoo : List Property := [⟨L "myFoo:prop", [("typeName", "double"), ("default", "5.0")]⟩]

def dedup (l : List SchemaRef) : List SchemaRef := l.foldl (fun acc x => if x ∈ acc then acc else acc ++ [x]) []

/-- The five properties of L8874-8881 come out right. -/
theorem l8874_properties :
    ((primDef reg₁ 6 myFoo [("Bar", none), ("FooBar", none)]).map fun pd => pd.properties.map (·.name)) =
      some [L "myFoo:prop", L "barprop", L "fooprop", L "foobarprop", [.lit "myInstance", .lit ":barbazprop"]] := by decide +kernel

/-- Finding 1 (LEAN-311): the schema list is `[Bar, FooBar, BarBaz:myInstance,
BarBaz:myInstance]`, not the L8922 `[Bar, FooBar, BarBaz:myInstance]`; only a
de-duplication (not in the pseudocode) recovers the spec's list. -/
theorem l8922_schemas :
    (primDef reg₁ 6 myFoo [("Bar", none), ("FooBar", none)]).map (·.schemas) =
      some [("Bar", none), ("FooBar", none), ("BarBaz", some "myInstance"), ("BarBaz", some "myInstance")] ∧
    (primDef reg₁ 6 myFoo [("Bar", none), ("FooBar", none)]).map (dedup ·.schemas) =
      some [("Bar", none), ("FooBar", none), ("BarBaz", some "myInstance")] := by decide +kernel

/-- The L8706-8711 example: `FooBar` (auto-applies `BazFoo:bazfooinstance`) gives
`FooBar, BazFoo:bazfooinstance, BarBaz:bazfooinstance` (after de-duplication). -/
theorem l8706_schemas :
    (primDef reg₀ 6 [] [("FooBar", none)]).map (dedup ·.schemas) =
      some [("FooBar", none), ("BazFoo", some "bazfooinstance"), ("BarBaz", some "bazfooinstance")] := by decide +kernel

/-- Finding 2 (LEAN-311): with the same `BazFoo`, the L8949 list must contain
`BarBaz:otherInstance`; the spec's `[Bar, FooBar, BarBaz:myInstance,
BazFoo:otherInstance]` omits it. -/
theorem l8949_schemas :
    (primDef reg₂ 6 myFoo [("Bar", none), ("FooBar", none)]).map (dedup ·.schemas) =
      some [("Bar", none), ("FooBar", none), ("BarBaz", some "myInstance"), ("BazFoo", some "otherInstance"), ("BarBaz", some "otherInstance")] ∧
    (primDef reg₂ 6 myFoo [("Bar", none), ("FooBar", none)]).map (dedup ·.schemas) ≠
      some [("Bar", none), ("FooBar", none), ("BarBaz", some "myInstance"), ("BazFoo", some "otherInstance")] := by decide +kernel

/-- Fallback order (L8890-8925): the typed schema's opinion beats an applied one. -/
def Strong : SchemaDef := { name := "Strong", properties := [prop (L "p") "int" "1"] }
def WeakAPI : SchemaDef := { name := "WeakAPI", properties := [⟨L "p", [("typeName", "int"), ("default", "2"), ("hidden", "true")]⟩] }

theorem fallback_order :
    ((primDef [Strong, WeakAPI] 4 [] [("Strong", none), ("WeakAPI", none)]).bind fun pd => fallback pd (L "p") "default") = some "1" ∧
    ((primDef [Strong, WeakAPI] 4 [] [("Strong", none), ("WeakAPI", none)]).bind fun pd => fallback pd (L "p") "hidden") = some "true" ∧
    ((primDef [Strong, WeakAPI] 4 [] [("WeakAPI", none), ("Strong", none)]).bind fun pd => fallback pd (L "p") "default") = some "2" := by
  decide +kernel

/-- Finding 3 (LEAN-311): mutual built-ins diverge for every fuel. -/
def CycA : SchemaDef := { name := "CycA", builtIns := [("CycB", none)] }
def CycB : SchemaDef := { name := "CycB", builtIns := [("CycA", none)] }
def regCyc : Registry := [CycA, CycB]

theorem cycle_diverges : ∀ n, build regCyc n ("CycA", none) = none ∧ build regCyc n ("CycB", none) = none := by
  intro n
  induction n with
  | zero => exact ⟨rfl, rfl⟩
  | succ n ih =>
    have hA : inclusions regCyc ("CycA", none) = [("CycB", none)] := by decide +kernel
    have hB : inclusions regCyc ("CycB", none) = [("CycA", none)] := by decide +kernel
    constructor
    · simp [build, hA, ih.2]
    · simp [build, hB, ih.1]

end Examples

end UsdSpec.Schema.PrimDefinition
