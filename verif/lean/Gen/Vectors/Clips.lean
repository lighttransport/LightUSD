import UsdSpec.ValueResolution.Clips
import Gen.Inc

/-!
Value-clip vectors.

* `LEAN_CLIP_TIME(id, active, times, stage_t, expected_clip_index, expected_clip_time)`:
  `active` = `stage:index;…`, `times` = `stage:clip;…` (empty string = no
  `times` authored), all rationals `num/den`; `expected_clip_index` is the
  `assetIndex` of the active entry (`activeIndex`), `expected_clip_time` the
  timing-curve value (`clipTime`, incl. jumps and the v1.1 clamp).
* `LEAN_CLIP_TEMPLATE(id, template, start, end, stride, active_offset, asset_paths, active, times)`:
  `active_offset` `-` = unauthored; `asset_paths` `;`-separated; `active` /
  `times` encoded as above. Template expansion assumes every derived file
  resolves (OpenUSD skips missing files, SPEC-ISSUE LEAN-766). Templates are
  dot-separated (`name.###.usd`, `name.###.##.usd`), the only form OpenUSD
  accepts (SPEC-ISSUE LEAN-762).
-/
namespace Gen.ClipVec
open UsdSpec.Clips

def q (r : ℚ) : String := s!"{r.num}/{r.den}"

def encA (l : List (ℚ × ℕ)) : String := ";".intercalate (l.map fun (s, i) => s!"{q s}:{i}")
def encT (l : List (ℚ × ℚ)) : String := ";".intercalate (l.map fun (s, c) => s!"{q s}:{q c}")

/-- (active, times) cases; the first four are the spec's own examples
(L6709-6711 / L6951-6958 / L6969-6990; L7235-7236 jump; L7246-7248 loop; L7280-7281 clip
 times; L7293-7295 clip looping — rows of that last case in the clamped region
 [0,10) differ from OpenUSD at the value level, SPEC-ISSUE LEAN-769). -/
def cases : List (List (ℚ × ℕ) × List (ℚ × ℚ)) :=
  [ ([(0, 0), (1, 1), (2, 2)], [(0, 1), (1, 2), (2, 3)]),
    ([(0, 0), (10, 1)], [(0, 0), (10, 10), (10, 25), (20, 35)]),
    ([(0, 0)], [(0, 0), (25, 25), (25, 0), (50, 25)]),
    ([(0, 0), (10, 1)], [(0, 5), (10, 0)]),
    ([(0, 0), (10, 1)], [(10, 0), (30, 20), (30, 0), (50, 20)]),
    ([(-4, 2), (-1/2, 0), (3, 1)], [(-4, 8), (0, 0), (6, 3)]),
    ([(0, 0), (10, 1)], []),
    ([(1/4, 3), (5/2, 1), (7, 2)], [(0, 0), (2, 4), (2, -4), (6, 4), (6, 8)]) ]

def queries (a : List (ℚ × ℕ)) (t : List (ℚ × ℚ)) : List ℚ :=
  let ss := a.map (·.1) ++ t.map (·.1)
  let lo := (ss.foldl min 0) - 3
  let hi := (ss.foldl max 0) + 3
  let n := ((hi - lo) * 4).floor.toNat
  ((List.range (n + 1)).map fun (j : ℕ) => lo + (j : ℚ) / 4) ++ ss.map (· + 1/8)

def timeRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for (a, t) in cases do
    for s in queries a t do
      let idx := (activeIndex a s).getD 0
      out := out ++ row "LEAN_CLIP_TIME" [toString id, cstr (encA a), cstr (encT t), cstr (q s),
        toString idx, cstr (q (clipTime t s))]
      id := id + 1
  return out

def tplString (tp : Template) : String :=
  let hashes (n : ℕ) := String.ofList (List.replicate n '#')
  tp.pre ++ hashes tp.intWidth ++ (match tp.fracWidth with | some w => "." ++ hashes w | none => "") ++ tp.post

def templates : List Template :=
  [ ⟨"clip.", 3, none, ".usd", 101, 103, 1, some (1/2)⟩,      -- L6868-6878
    ⟨"clip.", 3, none, ".usd", 101, 103, 1, some (-1/2)⟩,
    ⟨"path/clipname.", 1, none, ".usd", 12, 25, 6, none⟩,     -- L6858-6866
    ⟨"path/clipname.", 3, none, ".usd", 12, 12, 1, none⟩,     -- L6843-6847
    ⟨"./quad.", 3, none, ".usda", 0, 2, 1, none⟩,             -- L6889-6909 (dot form)
    ⟨"a/b.", 4, none, ".usda", 0, 10, 5/2, some 1⟩,
    ⟨"f.", 3, none, ".usd", 25/2, 27/2, 1, none⟩,             -- fractional start, integer template
    ⟨"g.", 3, some 2, ".usd", 1/2, 4, 3/2, none⟩,
    ⟨"g.", 2, some 3, ".usd", 1, 2, 1/4, some (1/8)⟩,
    ⟨"h.", 2, none, ".usd", 0, 3, 1, some 1⟩ ]

def templateRows : String := Id.run do
  let mut out := ""
  let mut id := 0
  for tp in templates do
    let cs := tp.expand
    out := out ++ row "LEAN_CLIP_TEMPLATE" [toString id, cstr (tplString tp), cstr (q tp.start),
      cstr (q tp.stop), cstr (q tp.stride),
      cstr (match tp.activeOffset with | some o => q o | none => "-"),
      cstr (";".intercalate cs.assetPaths), cstr (encA cs.active), cstr (encT cs.times)]
    id := id + 1
  return out

def files : List (String × String) :=
  [ ("clip-vectors.inc", header "Gen.Vectors.Clips" ++
      "// LEAN_CLIP_TIME(id, active, times, stage_t, expected_clip_index, expected_clip_time):\n" ++
      "//   active = stage:index;...  times = stage:clip;... (\"\" = unauthored -> identity), num/den.\n" ++
      "//   SPEC: core_spec.md L6941-6994 (active, timing curve, clamp), L7222-7236 (jumps).\n" ++
      timeRows ++
      "// LEAN_CLIP_TEMPLATE(id, template, start, end, stride, active_offset, asset_paths, active, times):\n" ++
      "//   active_offset '-' = unauthored. SPEC: L6823-6915. Assumes every derived file resolves.\n" ++
      templateRows) ]

end Gen.ClipVec
