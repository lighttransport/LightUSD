import UsdSpec.ValueResolution.Spline
import Gen.Inc

/-!
Spline evaluation vectors.

`LEAN_SPLINE(id, text, pre, post, t, expected)`:
* `text` — a complete USDA spline value block (`{ … }`, core_spec.md
  L10196-10301 grammar) accepted by LightUSD `ParseSplineText` and by OpenUSD
  (`double v.spline = <text>`); every knot authors `post <interp>` explicitly;
* `pre`, `post` — the extrapolation tokens used in `text` (informational);
* `t` — query time as `num/den`;
* `expected` — `num/den`, or `none` when the spline has no value at `t`
  (value block / `none` extrapolation).

Only exactly evaluable configurations are emitted: held/linear segments,
Hermite segments and *time-linear* Bezier segments (both widths = segment/3),
under every extrapolation mode including loops and inner loops. All inputs are
dyadic so C++ doubles are exact. Dual-valued *terminal* knots are avoided
under held/linear extrapolation (SPEC-ISSUE LEAN-743/744).
-/
namespace Gen.SplineVec
open UsdSpec.Spline

def q (r : ℚ) : String := s!"{r.num}/{r.den}"

/-- Exact decimal for a dyadic rational. -/
def dec (r : ℚ) : String := Id.run do
  let mut k := 0
  let mut d := r.den
  while d % 2 == 0 && d > 1 do
    d := d / 2; k := k + 1
  if d != 1 then return s!"{r.num}/{r.den}"  -- never emitted (generator only uses dyadics)
  let n : Int := r.num * (5 : Int) ^ k
  let neg := n < 0
  let a := n.natAbs
  let s := toString a
  let s := if s.length ≤ k then String.ofList (List.replicate (k + 1 - s.length) '0') ++ s else s
  let ip := s.take (s.length - k)
  let fp := (s.drop (s.length - k)).toString
  let fp := (fp.toList.reverse.dropWhile (· == '0')).reverse
  let body := if fp.isEmpty then ip.toString else ip.toString ++ "." ++ String.ofList fp
  return (if neg then "-" else "") ++ body

def isDyadic (r : ℚ) : Bool := Id.run do
  let mut d := r.den
  while d % 2 == 0 && d > 1 do d := d / 2
  return d == 1

def extrapTok : Extrap → String
  | .none => "none"
  | .held => "held"
  | .linear => "linear"
  | .sloped s => s!"sloped({dec s})"
  | .loopRepeat => "loop repeat"
  | .loopReset => "loop reset"
  | .loopOscillate => "loop oscillate"

def interpTok : Interp → String
  | .none => "none" | .held => "held" | .linear => "linear" | .curve => "curve"

def knotText (ct : CurveType) (k : Knot) : String :=
  let v := if k.preValue = k.value then dec k.value else s!"{dec k.preValue} & {dec k.value}"
  let tan (w s : ℚ) := match ct with | .bezier => s!"({dec w}, {dec s})" | .hermite => s!"({dec s})"
  let pre := if k.preW = 0 ∧ k.preS = 0 then "" else s!"; pre {tan k.preW k.preS}"
  let post := match k.interp with
    | .curve => s!"; post curve {tan k.postW k.postS}"
    | i => s!"; post {interpTok i}"
  s!"{dec k.time}: {v}{pre}{post}"

def splineText (sp : Spline) : String :=
  let ct := match sp.curve with | .bezier => "bezier" | .hermite => "hermite"
  let loop := match sp.loop with
    | some lp => s!" loop: ({dec lp.protoStart}, {dec lp.protoEnd}, {lp.numPre}, {lp.numPost}, {dec lp.valueOffset}),"
    | none => ""
  "{ " ++ ct ++ ", pre: " ++ extrapTok sp.pre ++ ", post: " ++ extrapTok sp.post ++ "," ++ loop ++
    String.join (sp.knots.map fun k => " " ++ knotText sp.curve k ++ ",") ++ " }"

/-- Every Bezier curve segment is time-linear (exact evaluation). -/
def exactOK (sp : Spline) : Bool :=
  let ks := match sp.loop with | some lp => expandInner lp sp.knots | none => sp.knots
  let rec go : List Knot → Bool
    | a :: b :: rest =>
      let d := b.time - a.time
      let ok := match sp.curve, a.interp with
        | .bezier, .curve => decide (clampW a.postW = d / 3 ∧ clampW b.preW = d / 3)
        | _, _ => true
      ok && go (b :: rest)
    | _ => true
  go ks

def kn (t v : ℚ) (i : Interp := .linear) (pv : ℚ := v) : Knot :=
  { time := t, value := v, preValue := pv, interp := i }
def kc (t v preW preS postW postS : ℚ) (i : Interp := .curve) : Knot :=
  ⟨t, v, v, preW, preS, postW, postS, i⟩

/-- Base knot sets: (name, curve, knots, loop). -/
def bases : List (CurveType × List Knot × Option LoopParams) :=
  [ (.bezier, [kn 0 0, kn 2 4, kn 4 1], none),
    (.bezier, [kn 0 1 .held, kn 1 3, kn 2 5 .linear 2, kc 4 0 0 0 1 (1/2), kc 7 2 1 (-1) 0 0 .linear], none),
    (.hermite, [kc 0 1 0 0 0 3, kc 4 5 0 (-1) 0 (1/2), kn 8 2], none),
    (.bezier, [kc 0 0 0 0 1 2, kc 3 6 1 0 (1/2) (-1), kc (9/2) 3 (1/2) 4 0 0 .held, kn 6 1], none),
    (.bezier, [kn 0 1, kn 2 3 .none, kn 4 5, kn 6 0 .held, kn 8 2], none),
    (.bezier, [kn 0 0, kn 2 4, kn 4 1, kn 6 100], some ⟨0, 4, 1, 2, 10⟩),
    (.bezier, [kn 0 0, kn 2 4, kn 4 1], some ⟨0, 3, 0, 1, 10⟩),
    (.hermite, [kc 0 0 0 0 0 1, kc 2 2 0 (-1) 0 0 .linear, kn 4 1], some ⟨0, 4, 1, 1, -5⟩) ]

def modes : List Extrap :=
  [.none, .held, .linear, .sloped (1/2), .sloped (-2), .loopRepeat, .loopReset, .loopOscillate]

def queries (f l : ℚ) : List ℚ :=
  let P := if l = f then 1 else l - f
  ((List.range 49).map fun j => f - 2 * P + (j : ℚ) * P / 8) ++ [f - 1/8, l + 1/8]

def rows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for (ct, ks, lp) in bases do
    for pre in modes do
      -- pre and post regions are evaluated independently: one mode per side pair
      for post in [pre] do
        let sp : Spline := { curve := ct, pre := pre, post := post, loop := lp, knots := ks }
        if !exactOK sp then continue
        let eks := match lp with | some p => expandInner p ks | none => ks
        let (f, l) := (eks.head?.map (·.time) |>.getD 0, eks.getLast?.map (·.time) |>.getD 0)
        for t in queries f l do
          -- pxr: with an inner loop, `post: none` still yields the closing
          -- knot's value at its own time (SPEC-ISSUE LEAN-745); unspecified, skip.
          if lp.isSome && post == .none && t == l then continue
          let e := match eval sp t with | some v => q v | none => "none"
          out := out ++ row "LEAN_SPLINE" [toString id, cstr (splineText sp), cstr (extrapTok pre),
            cstr (extrapTok post), cstr (q t), cstr e]
          id := id + 1
  return out

def files : List (String × String) :=
  [ ("spline-vectors.inc", header "Gen.Vectors.Spline" ++
      "// LEAN_SPLINE(id, text, pre, post, t, expected): EvaluateSplineData(ParseSplineText(text), t)\n" ++
      "// == expected (num/den) or no value (\"none\"). SPEC: core_spec.md L7634-8096, L2006-2162.\n" ++
      "// Only exact configurations (held/linear, Hermite, time-linear Bezier, all extrapolations,\n" ++
      "// inner loops). Cross-checked against OpenUSD v26.11 Ts (UsdAttribute::Get).\n" ++ rows) ]

end Gen.SplineVec
