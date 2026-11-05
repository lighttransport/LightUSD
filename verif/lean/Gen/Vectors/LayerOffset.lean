import UsdSpec.LayerOffset
import Gen.Inc

/-!
Layer-offset / TCPS vectors.

* `LEAN_OFFSET(id, o1, s1, o2, s2, eo, es)`: `compose ⟨o1,s1⟩ ⟨o2,s2⟩ = ⟨eo,es⟩`
  (checked against `pcp::LayerOffset::Compose`).
* `LEAN_TCPS_CHAIN(id, root_tcps, links, eo, es)`: a sublayer chain root → … → leaf,
  `links` = `tcps:offset:scale;…` (tcps `-` = unauthored → fallback 24). The
  leaf authors samples at 0 and 1; composed stage times must be `eo` and
  `eo + es` (`cumulative`, the normative recursive formulation L5466).

All rationals are dyadic and printed as `num/den`, so C++ doubles are exact.
-/
namespace Gen.OffsetVec
open UsdSpec UsdSpec.LayerOffset

def q (r : ℚ) : String := s!"{r.num}/{r.den}"

def offs : List ℚ := [0, 1, -2, 3/2, 10]
def scales : List ℚ := [1, 2, 1/2, 1/4]

def composeRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for o1 in offs do for s1 in scales do for o2 in offs do for s2 in scales do
    let r := compose ⟨o1, s1⟩ ⟨o2, s2⟩
    out := out ++ row "LEAN_OFFSET" [toString id, cstr (q o1), cstr (q s1), cstr (q o2), cstr (q s2),
      cstr (q r.offset), cstr (q r.scale)]
    id := id + 1
  return out

def tcpsChoices : List (Option ℚ) := [none, some 24, some 48, some 12, some 96]

/-- Chains of length 1–3. Every link authors an offset; tcps may be unauthored. -/
def chains : List (Option ℚ × List (Option ℚ × LayerOffset)) := Id.run do
  let links : List (Option ℚ × LayerOffset) :=
    (tcpsChoices.flatMap fun t => [(t, ⟨0, 1⟩), (t, ⟨10, 1⟩), (t, ⟨3/2, 2⟩), (t, ⟨-2, 1/2⟩)])
  let mut out := []
  for r in tcpsChoices do
    for l1 in links do
      out := out ++ [(r, [l1])]
  let la := links.toArray
  let mut g : Lcg := ⟨0x7C95⟩
  for _ in [0:240] do
    let (g1, a) := g.next; let (g2, b) := g1.next; let (g3, c) := g2.next; let (g4, d) := g3.next
    g := g4
    let n := 2 + d % 2
    let ls := [la.getD (a % la.size) (none, ⟨0, 1⟩), la.getD (b % la.size) (none, ⟨0, 1⟩), la.getD (c % la.size) (none, ⟨0, 1⟩)].take n
    out := out ++ [(tcpsChoices.getD (d % 5) none, ls)]
  return out

def tcpsRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for (rt, ls) in chains do
    let root := effectiveTcps rt none
    let r := cumulative root (ls.map fun (t, o) => ⟨effectiveTcps t none, o⟩)
    let enc := ";".intercalate (ls.map fun (t, o) =>
      s!"{match t with | some v => q v | none => "-"}:{q o.offset}:{q o.scale}")
    out := out ++ row "LEAN_TCPS_CHAIN" [toString id,
      cstr (match rt with | some v => q v | none => "-"), cstr enc, cstr (q r.offset), cstr (q r.scale)]
    id := id + 1
  return out

def files : List (String × String) :=
  [ ("layer-offset-vectors.inc", header "Gen.Vectors.LayerOffset" ++
      "// LEAN_OFFSET(id, o1, s1, o2, s2, eo, es): compose (o1,s1) (o2,s2) = (eo,es), L4282-4288/L5483-5487\n" ++
      composeRows),
    ("tcps-chain-vectors.inc", header "Gen.Vectors.LayerOffset" ++
      "// LEAN_TCPS_CHAIN(id, root_tcps, links, eo, es): links = tcps:offset:scale;... root->leaf,\n" ++
      "// '-' = timeCodesPerSecond unauthored (fallback 24, L5442-5453); leaf samples at 0,1\n" ++
      "// land at stage times eo, eo+es (recursive formulation, L5466-5509).\n" ++ tcpsRows) ]

end Gen.OffsetVec
