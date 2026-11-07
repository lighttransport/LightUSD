import Mathlib.Data.Rat.Defs
import UsdSpec.ValueResolution.Metadata

/-!
# Attribute value resolution: source priority (AOUSD USD Core Specification v1.1.0, §12.2)

SPEC: core_spec.md L6270-6345 (Attribute Resolution), L6348-6443 (python
`get_value`), L6492-6503 (Time Based), L6582-6590 (Spline Evaluation),
L7006-7011 (Value Resolution of Clip Values).

The spec states the priority of the value sources (`timeSamples`, `spline`,
`default`, value `clips`) three times, differently:

* L6325-6336: "If time based evaluation is considered then the strongest opinion
  of the following metadata fields are consulted (in the order given):
  `timeSamples`, `spline`, `default`, `clips`. This happens on each spec
  contributing to composition."
* L6492-6503 (Time Based): "1. Time Samples 2. Spline Evaluation 3. Value clips
  … Time Samples have higher priority over *Spline evaluation* or *Value clips*."
  (no `default`).
* L6586-6590: "`timeSamples` take precedence over splines, and splines take
  precedence over `default` and value clips."
* L7006-7011: "The clip data is just weaker than the `Local` … data of the
  anchoring layer." — a position *in the layer order*, not in a field order.

Each opinion of the strength-ordered stack (strongest first) is abstracted to
the value each source yields at the query time (`none` = not authored / no
value). Clips are attached to the opinion whose layer anchors the clip set.

Readings formalized:
* `perSpec` — L6334 "on each spec": walk specs strongest first, inside a spec
  `timeSamples > spline > default > clips`;
* `anchored` — L7006-7011: each spec contributes its local value, then the
  clips it anchors (definitionally the same as `perSpec`: `perSpec_eq_anchored`);
* `perFieldA` — L6326-6332 read as "the strongest opinion of `timeSamples`
  across the stack, else of `spline`, else `default`, else `clips`";
* `perFieldB` — L6492-6503: `timeSamples`, `spline`, `clips` across the stack,
  `default` only when no time-varying source exists.

`readings_disagree` shows by `decide` where they differ. OpenUSD 26.11.1
(pxr python, layer stacks `w`, `x`, `y`, `z`) agrees with `perSpec`/`anchored`
on all of them. SPEC-ISSUE: LEAN-702.
Not modelled: value blocks / fallback (L6313-6316, L7364-7372) and the
uniform-attribute clip exclusion (~L6304-6309).
-/

namespace UsdSpec.ValueResolution.Attribute

open UsdSpec.ValueResolution.Metadata (strongest strongest_append)

/-- One contributing spec, evaluated at the query time. -/
structure Opinion where
  timeSamples : Option ℚ := none
  spline : Option ℚ := none
  default : Option ℚ := none
  /-- value of a clip set anchored in this spec's layer (on the owning prim or
  an ancestor), at the query time -/
  clips : Option ℚ := none
  deriving DecidableEq, Repr

inductive Query
  | defaultTime
  | numeric
  deriving DecidableEq, Repr

/-- Local (non-clip) value of a spec at a numeric time: `timeSamples > spline > default`. -/
def Opinion.localValue (o : Opinion) : Option ℚ := o.timeSamples <|> o.spline <|> o.default

/-- Default-time query: the strongest `default` (L6338-6340). Common to all readings. -/
def atDefault (os : List Opinion) : Option ℚ := strongest (os.map (·.default))

/-- L6325-6336, per spec. -/
def perSpec (os : List Opinion) : Option ℚ :=
  strongest (os.map fun o => o.timeSamples <|> o.spline <|> o.default <|> o.clips)

/-- L7006-7011, clips just weaker than the anchoring layer's Local data. -/
def anchored (os : List Opinion) : Option ℚ :=
  strongest (os.flatMap fun o => [o.localValue, o.clips])

/-- L6326-6332 read per field across the whole stack. -/
def perFieldA (os : List Opinion) : Option ℚ :=
  strongest (os.map (·.timeSamples)) <|> strongest (os.map (·.spline)) <|>
    strongest (os.map (·.default)) <|> strongest (os.map (·.clips))

/-- L6492-6503 (time-varying sources first), then `default` (L6484-6487). -/
def perFieldB (os : List Opinion) : Option ℚ :=
  strongest (os.map (·.timeSamples)) <|> strongest (os.map (·.spline)) <|>
    strongest (os.map (·.clips)) <|> strongest (os.map (·.default))

def resolve (q : Query) (r : List Opinion → Option ℚ) (os : List Opinion) : Option ℚ :=
  match q with
  | .defaultTime => atDefault os
  | .numeric => r os

theorem perSpec_eq_anchored (os : List Opinion) : perSpec os = anchored os := by
  induction os with
  | nil => rfl
  | cons o os ih =>
    simp only [perSpec, anchored, List.map_cons, List.flatMap_cons, strongest_append] at *
    simp only [strongest] at *
    rw [ih]
    simp only [Opinion.localValue]
    cases o.timeSamples <;> cases o.spline <;> cases o.default <;> cases o.clips <;> rfl

/-- On a single spec all four readings agree (the field order is the same;
the readings differ only in how fields of *different* specs interleave). -/
theorem readings_agree_single (o : Opinion) :
    perSpec [o] = perFieldA [o] ∧ (o.default = none → perSpec [o] = perFieldB [o]) := by
  obtain ⟨a, b, c, d⟩ := o
  cases a <;> cases b <;> cases c <;> cases d <;> simp [perSpec, perFieldA, perFieldB, strongest]

/-! ## Distinguishing stacks (the pxr test layers, values at stage time 5)

* `w`: root sublayers `[w1, w2]`; `w1` (stronger) authors `a = 5`, `w2` authors
  `a.timeSamples = {0: 1, 10: 2}` (1.5 at time 5). pxr: `Get(5) = 5.0`
  (`ResolveInfoSourceDefault`).
* `x`: `x1` authors `a = 5`, the weaker `x2` anchors a clip set whose clip has
  `a.timeSamples = {0:1, 10:2}`. pxr: `5.0` (Default).
* `y`: the stronger `y1` anchors the clips, the weaker `y2` authors `a = 5`.
  pxr: `1.5` (`ResolveInfoSourceValueClips`).
* `z`: one layer authors both `a = 5` and the clips. pxr: `5.0` (Default). -/

def stackW : List Opinion := [{ default := some 5 }, { timeSamples := some (3/2) }]
def stackX : List Opinion := [{ default := some 5 }, { clips := some (3/2) }]
def stackY : List Opinion := [{ clips := some (3/2) }, { default := some 5 }]
def stackZ : List Opinion := [{ default := some 5, clips := some (3/2) }]

/-- OpenUSD 26.11.1 results for the four stacks (see module doc). -/
def pxrResult : List (Option ℚ) := [some 5, some 5, some (3/2), some 5]

def readingResults (r : List Opinion → Option ℚ) : List (Option ℚ) :=
  [stackW, stackX, stackY, stackZ].map r

/-- SPEC-ISSUE: LEAN-702 — the three stated source orders are pairwise
inconsistent; only the per-spec / clip-anchoring reading matches OpenUSD.
`perFieldA` (L6326-6332 across the stack) is wrong on `w` and `y`; `perFieldB`
(L6492-6503) is wrong on `w`, `x` and `z`. -/
theorem readings_disagree :
    readingResults perSpec = pxrResult ∧
    readingResults anchored = pxrResult ∧
    readingResults perFieldA = [some (3/2), some 5, some 5, some 5] ∧
    readingResults perFieldB = [some (3/2), some (3/2), some (3/2), some (3/2)] := by
  refine ⟨?_, ?_, ?_, ?_⟩ <;>
    simp [readingResults, pxrResult, perSpec, anchored, perFieldA, perFieldB, strongest,
      Opinion.localValue, stackW, stackX, stackY, stackZ]

/-! ## The python `get_value` (L6412-6443), modelled where it parses

SPEC-ISSUE: LEAN-703 — defects of the reference pseudo-implementation (see
ISSUES.md). The model `pyGetValue` repairs only the syntax: it reads a single
spec, consults `timeSamples` then `default` then the fallback; spline and clip
values are ignored, so an attribute authored only with a spline resolves to the
fallback (contrary to L6582-6590). -/

def pyGetValue (fallback : ℚ) (q : Query) (o : Opinion) : ℚ :=
  match q with
  | .defaultTime => o.default.getD fallback
  | .numeric => (o.timeSamples <|> o.default).getD fallback

theorem pyGetValue_ignores_spline :
    pyGetValue 0 .numeric { spline := some 7 } = 0 ∧
      perSpec [{ spline := some 7 }] = some 7 := by
  simp [pyGetValue, perSpec, strongest]

theorem pyGetValue_ignores_clips :
    pyGetValue 0 .numeric { clips := some 7 } = 0 ∧
      perSpec [{ clips := some 7 }] = some 7 := by
  simp [pyGetValue, perSpec, strongest]

end UsdSpec.ValueResolution.Attribute
