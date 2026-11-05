import UsdSpec.ValueResolution.TimeSamples
import Gen.Inc

/-!
Value-resolution vectors for held / linear time samples.

* `LEAN_VR(id, interp, samples, query_time, expected)`:
  - `interp`: `0` held, `1` linear;
  - `samples`: `"t:v;t:v;…"` sorted by time, `t` and `v` printed `num/den`,
    `v = None` an authored per-sample value block (L7408-7417);
  - `query_time`: `num/den`;
  - `expected`: `"None"` (the governing sample is a block: no value, resolution
    stops) or `num/den`.

Expected values are `UsdSpec.ValueResolution.TimeSamples.eval` (L6539-6580,
L7571-7632): out-of-range queries hold the end samples, held = lower bracket,
linear = `v₀ + (t−t₀)/(t₁−t₀)·(v₁−v₀)`, a blocked lower bracket blocks the
segment and a blocked upper bracket holds the lower value. All rationals are
dyadic, so C++ doubles are exact.
-/
namespace Gen.VrVec
open UsdSpec.ValueResolution.TimeSamples

def q (r : ℚ) : String := s!"{r.num}/{r.den}"

def ov : Option ℚ → String
  | some v => q v
  | none => "None"

def sampleSets : List Samples :=
  [ [(0, some 1)],
    [(1, none)],
    [(0, some 6), (1, some 8), (10, some (-1))],          -- L6355
    [(1, some 0), (5, some (13/4)), (10, some (25/4))],     -- L6553-6557 shape, dyadic values
    [(100, some 10), (200, some 20)],                     -- L7577-7591
    [(1, none), (2, some (5/2))],                         -- L7408-7417
    [(1, some 2), (2, some (5/2))],                       -- L7395-7401
    [(0, some 1), (2, none), (4, some 3)],
    [(0, none), (2, some 1), (4, none)],
    [(-3/2, some (1/4)), (0, some (-1/2)), (1/4, some 4), (6, some 4), (8, none), (9, some (-3/8))],
    [(0, some 0), (1, some 1024), (3, some (-1024)), (7, some (1/1024))] ]

/-- Queries: every sample time, the midpoints and quarter points of each
segment, and one unit before / after the range. -/
def queries (s : Samples) : List ℚ :=
  let ts := s.map (·.1)
  let segs := (ts.zip ts.tail).flatMap fun (a, b) => [(3 * a + b) / 4, (a + b) / 2, (a + 3 * b) / 4]
  match ts.head?, ts.getLast? with
  | some f, some l => [f - 1] ++ ts ++ segs ++ [l + 1]
  | _, _ => []

def encode (s : Samples) : String := ";".intercalate (s.map fun (t, v) => s!"{q t}:{ov v}")

def rows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for s in sampleSets do
    for (i, iv) in [(0, Interp.held), (1, Interp.linear)] do
      for t in queries s do
        match eval iv s t with
        | some r =>
          out := out ++ row "LEAN_VR" [toString id, toString i, cstr (encode s), cstr (q t), cstr (ov r)]
          id := id + 1
        | none => pure ()
  return out

def files : List (String × String) :=
  [ ("vr-lean-cases.inc", header "Gen.Vectors.TimeSamples" ++
      "// LEAN_VR(id, interp /*0 held 1 linear*/, \"t:v;t:v;...\" /* v may be None (value block) */,\n" ++
      "//         query_time, expected /* \"None\" (blocked) or num/den */)\n" ++
      "// Time-sample evaluation, L6539-6580 / L7364-7446 / L7571-7632: end samples held\n" ++
      "// out of range; blocked lower bracket blocks the segment; blocked upper bracket holds.\n" ++
      rows) ]

end Gen.VrVec
