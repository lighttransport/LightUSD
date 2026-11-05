import Mathlib.Data.Rat.Floor
import Mathlib.Tactic

/-!
# Spline evaluation (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L2006-2162 (Cubic Splines Specialized Type: data model),
L6582-6606 (Spline Evaluation in value resolution), L7634-8096 (§12 Spline:
interpolation, tangents, AutoEase, continuity, looping, inner loops,
non-monotonic curves / anti-regression).

Everything is over `ℚ`. A Bezier segment is the parametric cubic
`(time(u), value(u))`, `u ∈ [0,1]`, with control points
`(t₀, t₀+w₀, t₁−w₁, t₁)` / `(v₀, v₀+s₀w₀, v₁⁻−s₁w₁, v₁⁻)` (slope form, L7682-7688;
`v₁⁻` is the next knot's pre-value, L7795-7801). Evaluating at a *time* needs
`time(u) = t`, which has no exact rational solution in general, so:

* structural theorems are proved for the parametric form (`Seg.val_zero`,
  `Seg.val_one`, `bez_taylor`, the regression characterization);
* *time-linear* segments (both tangent widths = segment/3, `bez_timeLinear`) and
  Hermite segments (`hermite_eq_bez`) are evaluated exactly;
* other Bezier segments go through Keep Ratio (with a rational under-
  approximation of `√(ab)`, so the shortening stops at or short of the limit,
  like OpenUSD's padding, issue 03) and a fuelled bisection on `time(u) = t`
  that provably brackets the root (`bisect_bracket`, `bisect_width`).

Theorems: SP1 `Seg.val_zero`/`Seg.val_one`/`Seg.time_zero`/`Seg.time_one`;
SP2 `held_is_const_bez`, `linear_is_timeLinear_bez`; SP3 `nonRegressive_iff`,
`contain_nonRegressive`, `nonRegressive_scale`, `keepRatio_boundary`,
`keepRatio_maximal`, `keepRatio_equal_widths`; SP4 `loop_repeat_period`,
`loop_reset_period`, `loop_oscillate_period`, `loop_oscillate_mirror`,
`loop_base`; SP5 `postSlope_linear_extends`, `bez_end_slope`;
SP6 `evalBasic_post_held`; SP7 `shift_spacing`, `shift_time`, `shift_value`.

Modelling choices fixed by OpenUSD v26.11 probes (scratch `m5s/c1.json`,
`c2.json`; see ISSUES.md LEAN-741..748):
* segments are left-closed `[tᵢ, tᵢ₊₁)`; the post-extrapolation region is
  `[t_last, ∞)` (pxr: `post: none` has no value *at* the last knot, `loop reset`
  gives the first knot's value there) — LEAN-745;
* loops: `n = ⌊(t−t_first)/P⌋`, `τ = t − nP`; repeat adds `n·(v_last − v_first)`,
  oscillate evaluates odd copies at `t_first + t_last − τ` from the left
  (pxr flips the pre/post side) — matches every pxr probe;
* inner loops expand into knot copies (`expandInner`), then the basic spline is
  evaluated (pxr `inner`, `innerheld` probes);
* knots without a `post` interpolation default to `held` (data model L2092; pxr
  agrees).

Spec issues (rows in ISSUES.md):
* SPEC-ISSUE: LEAN-741 — AutoEase pseudocode (L7717-7766) blends toward
  `nextKnot.value` and zeroes both slopes when a neighbour is missing; OpenUSD
  uses the next knot's pre-value and leaves the off-the-end tangent untouched
  (`autoEase_nextDual_ne`, `autoEase_noPrev_ne`).
* SPEC-ISSUE: LEAN-742 — "evaluation … does not consult the algorithm field"
  (L2050-2052, L7709-7712), but OpenUSD re-solves `autoEase` tangents of
  authored knots, so the stored slope/width do not determine the value.
* SPEC-ISSUE: LEAN-743 — held/linear pre-extrapolation from a dual-valued first
  knot: spec "value of the terminal knot", OpenUSD the pre-value.
* SPEC-ISSUE: LEAN-744 — linear extrapolation slope with a dual-valued
  terminal knot: spec takes the segment slope, OpenUSD uses 0.
* SPEC-ISSUE: LEAN-745 — which side owns `t_last` (and the closing inner-loop
  knot) under `post: none` / `loop reset` is unspecified.
* SPEC-ISSUE: LEAN-746 — linear extrapolation of a single-knot spline has no
  segment to take the slope from.
* SPEC-ISSUE: LEAN-747 — oscillate does not say reflected copies take left
  limits (pre-values, held values) at mirrored knots.
* SPEC-ISSUE: LEAN-748 — inner-loop copy semantics (cumulative valueOffset,
  closing knot, dropped authored knots) are only implied; L7991-7992 typos.
-/

namespace UsdSpec.Spline
set_option linter.dupNamespace false

/-- SPEC: L2013-2020 (`EnumCurveType`). -/
inductive CurveType
  | bezier
  | hermite
  deriving DecidableEq, Repr

/-- SPEC: L2022-2040 (`EnumInterpolationMode`); `none` = value block. -/
inductive Interp
  | none
  | held
  | linear
  | curve
  deriving DecidableEq, Repr

/-- SPEC: L2109-2135 (`EnumExtrapolationMode`), L7820-7845. -/
inductive Extrap
  | none
  | held
  | linear
  | sloped (s : ℚ)
  | loopRepeat
  | loopReset
  | loopOscillate
  deriving DecidableEq, Repr

/-- A knot. SPEC: L2080-2107 (`SplineKnot`). A non-dual knot has
`preValue = value` (L7795-7801). Tangent algorithms are not stored: evaluation
"does not consult the algorithm field" (L2050-2052, L7711-7712; but see
SPEC-ISSUE: LEAN-742). Default `interp` is `held` (L2092). -/
structure Knot where
  time : ℚ
  value : ℚ
  preValue : ℚ
  preW : ℚ := 0
  preS : ℚ := 0
  postW : ℚ := 0
  postS : ℚ := 0
  interp : Interp := .held
  deriving DecidableEq, Repr

def Knot.dual (k : Knot) : Bool := decide (k.preValue ≠ k.value)

/-- SPEC: L2137-2148 (`LoopParameters`), L7962-8016. -/
structure LoopParams where
  protoStart : ℚ
  protoEnd : ℚ
  numPre : ℕ
  numPost : ℕ
  valueOffset : ℚ
  deriving DecidableEq, Repr

/-- SPEC: L2150-2162 (`Spline`). -/
structure Spline where
  curve : CurveType := .bezier
  pre : Extrap := .held
  post : Extrap := .held
  loop : Option LoopParams := none
  knots : List Knot
  deriving DecidableEq, Repr

/-! ## Cubic Bezier in one coordinate -/

/-- Cubic Bernstein form. -/
def bez (p0 p1 p2 p3 u : ℚ) : ℚ :=
  (1 - u) ^ 3 * p0 + 3 * (1 - u) ^ 2 * u * p1 + 3 * (1 - u) * u ^ 2 * p2 + u ^ 3 * p3

/-- First derivative. -/
def bezD (p0 p1 p2 p3 u : ℚ) : ℚ :=
  3 * ((1 - u) ^ 2 * (p1 - p0) + 2 * (1 - u) * u * (p2 - p1) + u ^ 2 * (p3 - p2))

def bezDD (p0 p1 p2 p3 u : ℚ) : ℚ :=
  6 * ((1 - u) * (p2 - 2 * p1 + p0) + u * (p3 - 2 * p2 + p1))

def bezDDD (p0 p1 p2 p3 : ℚ) : ℚ := 6 * (p3 - 3 * p2 + 3 * p1 - p0)

/-- Exact Taylor expansion: `bezD` is the derivative of `bez`. -/
theorem bez_taylor (p0 p1 p2 p3 u h : ℚ) :
    bez p0 p1 p2 p3 (u + h) - bez p0 p1 p2 p3 u =
      h * bezD p0 p1 p2 p3 u + h ^ 2 / 2 * bezDD p0 p1 p2 p3 u + h ^ 3 / 6 * bezDDD p0 p1 p2 p3 := by
  unfold bez bezD bezDD bezDDD; ring

theorem bez_zero (p0 p1 p2 p3 : ℚ) : bez p0 p1 p2 p3 0 = p0 := by unfold bez; ring
theorem bez_one (p0 p1 p2 p3 : ℚ) : bez p0 p1 p2 p3 1 = p3 := by unfold bez; ring

/-- Evenly spaced control points give a linear map: a Bezier segment whose two
tangent widths are a third of the segment is *time-linear*. -/
theorem bez_timeLinear (p0 p3 u : ℚ) :
    bez p0 (p0 + (p3 - p0) / 3) (p3 - (p3 - p0) / 3) p3 u = p0 + u * (p3 - p0) := by
  unfold bez; ring

theorem bez_const (c u : ℚ) : bez c c c c u = c := by unfold bez; ring

/-! ## Segments -/

/-- A curve segment between two knots, tangents in slope form. -/
structure Seg where
  t0 : ℚ
  v0 : ℚ
  t1 : ℚ
  v1 : ℚ
  w0 : ℚ
  s0 : ℚ
  w1 : ℚ
  s1 : ℚ
  deriving DecidableEq, Repr

def Seg.time (g : Seg) (u : ℚ) : ℚ := bez g.t0 (g.t0 + g.w0) (g.t1 - g.w1) g.t1 u
def Seg.val (g : Seg) (u : ℚ) : ℚ := bez g.v0 (g.v0 + g.s0 * g.w0) (g.v1 - g.s1 * g.w1) g.v1 u

/-- SP1. SPEC: L2004-2010 "points through which the curve passes". -/
theorem Seg.val_zero (g : Seg) : g.val 0 = g.v0 := bez_zero ..
theorem Seg.val_one (g : Seg) : g.val 1 = g.v1 := bez_one ..
theorem Seg.time_zero (g : Seg) : g.time 0 = g.t0 := bez_zero ..
theorem Seg.time_one (g : Seg) : g.time 1 = g.t1 := bez_one ..

/-- SP5 (curve): the value/time derivative ratio at the end of a segment is
the incoming tangent slope (needs a non-zero width). -/
theorem bez_end_slope (g : Seg) (hw : g.w1 ≠ 0) :
    bezD g.v0 (g.v0 + g.s0 * g.w0) (g.v1 - g.s1 * g.w1) g.v1 1 /
      bezD g.t0 (g.t0 + g.w0) (g.t1 - g.w1) g.t1 1 = g.s1 := by
  unfold bezD; rw [div_eq_iff (by norm_num [hw])]; ring

theorem bez_start_slope (g : Seg) (hw : g.w0 ≠ 0) :
    bezD g.v0 (g.v0 + g.s0 * g.w0) (g.v1 - g.s1 * g.w1) g.v1 0 /
      bezD g.t0 (g.t0 + g.w0) (g.t1 - g.w1) g.t1 0 = g.s0 := by
  unfold bezD; rw [div_eq_iff (by norm_num [hw])]; ring

/-- Time-linear segment: `time(u) = t₀ + u·(t₁−t₀)`, so the value at time `t`
is `val ((t−t₀)/(t₁−t₀))` exactly. -/
theorem Seg.time_timeLinear (g : Seg) (h0 : g.w0 = (g.t1 - g.t0) / 3)
    (h1 : g.w1 = (g.t1 - g.t0) / 3) (u : ℚ) : g.time u = g.t0 + u * (g.t1 - g.t0) := by
  unfold Seg.time; rw [h0, h1]; exact bez_timeLinear ..

theorem Seg.time_at (g : Seg) (h0 : g.w0 = (g.t1 - g.t0) / 3)
    (h1 : g.w1 = (g.t1 - g.t0) / 3) (hd : g.t1 - g.t0 ≠ 0) (t : ℚ) :
    g.time ((t - g.t0) / (g.t1 - g.t0)) = t := by
  rw [g.time_timeLinear h0 h1]; field_simp; ring

/-- Hermite basis with tangent slopes `m₀, m₁` over duration `d`. -/
def hermite (v0 m0 v1 m1 d u : ℚ) : ℚ :=
  (2 * u ^ 3 - 3 * u ^ 2 + 1) * v0 + (u ^ 3 - 2 * u ^ 2 + u) * d * m0 +
    (-2 * u ^ 3 + 3 * u ^ 2) * v1 + (u ^ 3 - u ^ 2) * d * m1

/-- Hermite = Bezier with both widths a third of the segment (OpenUSD
`_UpdateTangent` forces Hermite widths to 1/3, knotData.cpp:107-113). -/
theorem hermite_eq_bez (t0 t1 v0 s0 v1 s1 u : ℚ) :
    hermite v0 s0 v1 s1 (t1 - t0) u =
      (Seg.mk t0 v0 t1 v1 ((t1 - t0) / 3) s0 ((t1 - t0) / 3) s1).val u := by
  unfold hermite Seg.val bez; ring

/-- SP2 held: a held segment is the degenerate curve with all value control
points at `v₀`. SPEC: L7665-7666 "held constant until the next knot". -/
theorem held_is_const_bez (t0 t1 v0 w0 w1 u : ℚ) :
    (Seg.mk t0 v0 t1 v0 w0 0 w1 0).val u = v0 := by
  unfold Seg.val; simp [bez_const]

/-- SP2 linear: a linear segment is the time-linear curve with both slopes
equal to the chord slope. SPEC: L7668-7669. -/
theorem linear_is_timeLinear_bez (t0 t1 v0 v1 u : ℚ) (hd : t1 - t0 ≠ 0) :
    (Seg.mk t0 v0 t1 v1 ((t1 - t0) / 3) ((v1 - v0) / (t1 - t0)) ((t1 - t0) / 3)
      ((v1 - v0) / (t1 - t0))).val u = v0 + u * (v1 - v0) := by
  unfold Seg.val bez; field_simp; ring

/-! ## SP3: regression and Keep Ratio

SPEC: L8023-8096. Normalize a segment to duration 1 with tangent widths
`a = w₀/d`, `b = w₁/d` (after the backward-facing rule, L8054-8056:
negative widths count as 0). The time curve is `bez 0 a (1−b) 1`, whose
derivative is `3·tq a b u`. -/

/-- `time'(u)/3` in Bernstein form. -/
def tq (a b u : ℚ) : ℚ := a * (1 - u) ^ 2 + 2 * (1 - a - b) * u * (1 - u) + b * u ^ 2

theorem bezD_time_eq (a b u : ℚ) : bezD 0 a (1 - b) 1 u = 3 * tq a b u := by
  unfold bezD tq; ring

/-- The denormalized form: derivative of the segment's time curve. -/
theorem Seg.timeD_eq (g : Seg) (hd : g.t1 - g.t0 ≠ 0) (u : ℚ) :
    bezD g.t0 (g.t0 + g.w0) (g.t1 - g.w1) g.t1 u =
      3 * (g.t1 - g.t0) * tq (g.w0 / (g.t1 - g.t0)) (g.w1 / (g.t1 - g.t0)) u := by
  unfold bezD tq; field_simp; ring

/-- Non-regressive (time non-decreasing on `[0,1]`), closed form. -/
def NonRegressive (a b : ℚ) : Prop := a + b ≤ 1 ∨ (1 - a - b) ^ 2 ≤ a * b

instance (a b : ℚ) : Decidable (NonRegressive a b) := by unfold NonRegressive; infer_instance

/-- SP3 (characterization). For non-negative normalized widths, the time
curve is monotone on `[0,1]` iff `NonRegressive a b`. Matches issue 03's
derivation and OpenUSD `Ts_RegressionPreventer` (regressionPreventer.cpp). -/
theorem nonRegressive_iff {a b : ℚ} (ha : 0 ≤ a) (hb : 0 ≤ b) :
    (∀ u, 0 ≤ u → u ≤ 1 → 0 ≤ tq a b u) ↔ NonRegressive a b := by
  constructor
  · intro h
    by_contra hn
    unfold NonRegressive at hn
    push Not at hn
    obtain ⟨h1, h2⟩ := hn
    rcases eq_or_lt_of_le ha with ha0 | hapos
    · -- a = 0: then (1-b)^2 > 0 means b ≠ 1 ... pick y = a+b-1 > 0 side
      subst ha0
      -- B = 1 - b < 0; take x = b + 1, y = b - 1 (both > 0), u = y/(x+y)
      have hb1 : 1 < b := by linarith
      have hu := h ((b - 1) / (2 * b)) (by positivity) (by
        rw [div_le_one (by linarith)]; linarith)
      unfold tq at hu
      have : (0:ℚ) * (1 - (b - 1) / (2 * b)) ^ 2 +
          2 * (1 - 0 - b) * ((b - 1) / (2 * b)) * (1 - (b - 1) / (2 * b)) +
          b * ((b - 1) / (2 * b)) ^ 2 = -((b - 1) ^ 2 * (b + 2)) / (4 * b ^ 2) := by
        field_simp; ring
      rw [this] at hu
      have hneg : -((b - 1) ^ 2 * (b + 2)) / (4 * b ^ 2) < 0 := by
        apply div_neg_of_neg_of_pos
        · have : 0 < (b - 1) ^ 2 * (b + 1) := by positivity
          linarith
        · positivity
      linarith
    · -- a > 0, B = 1-a-b < 0, B² > ab: u = a / (a - B) = a / (2a + b - 1)
      set B := 1 - a - b with hB
      have hBn : B < 0 := by linarith
      have hden : 0 < a - B := by linarith
      have hu := h (a / (a - B)) (by positivity) (by
        rw [div_le_one hden]; linarith)
      unfold tq at hu
      rw [← hB] at hu
      have : a * (1 - a / (a - B)) ^ 2 + 2 * B * (a / (a - B)) * (1 - a / (a - B)) +
          b * (a / (a - B)) ^ 2 = a * (a * b - B ^ 2) / (a - B) ^ 2 := by
        field_simp; ring
      rw [this] at hu
      have : a * (a * b - B ^ 2) / (a - B) ^ 2 < 0 := by
        apply div_neg_of_neg_of_pos
        · have : a * b - B ^ 2 < 0 := by rw [hB]; nlinarith
          nlinarith
        · positivity
      linarith
  · intro h u hu0 hu1
    unfold tq
    have hx : 0 ≤ 1 - u := by linarith
    rcases h with h | h
    · have : 0 ≤ 1 - a - b := by linarith
      positivity
    · -- A x² + 2Bxy + C y² ≥ 0 when B² ≤ AC, A,C ≥ 0
      set x := 1 - u
      set B := 1 - a - b
      rcases eq_or_lt_of_le ha with ha0 | hapos
      · subst ha0
        have : B = 0 := by nlinarith [sq_nonneg B]
        rw [this]; positivity
      · have key : a * (a * x ^ 2 + 2 * B * u * x + b * u ^ 2) =
            (a * x + B * u) ^ 2 + (a * b - B ^ 2) * u ^ 2 := by ring
        have : 0 ≤ a * (a * x ^ 2 + 2 * B * u * x + b * u ^ 2) := by
          rw [key]; have : 0 ≤ a * b - B ^ 2 := by linarith
          positivity
        have := nonneg_of_mul_nonneg_right (by linarith [this] : 0 ≤ a * (a * x ^ 2 + 2 * B * u * x + b * u ^ 2)) hapos
        nlinarith [this]

/-- SPEC L8047-8050 CLAIM: "If both tangents of a segment remain within the
segment interval (i.e., neither tangent crosses the opposite knot in time),
the segment is guaranteed to be non-regressive." True (the **Contain**
strategy is sound). -/
theorem contain_nonRegressive {a b : ℚ} (ha : 0 ≤ a) (hb : 0 ≤ b) (ha1 : a ≤ 1) (hb1 : b ≤ 1) :
    NonRegressive a b := by
  unfold NonRegressive
  by_cases h : a + b ≤ 1
  · exact Or.inl h
  · right
    -- with x = 1-a, y = 1-b: ab - (a+b-1)² = (1-x-y)(x+y) + xy ≥ 0
    nlinarith [mul_nonneg (sub_nonneg.2 ha1) (sub_nonneg.2 hb1)]

/-- Shortening both tangents by a common factor `s ∈ [0,1]` preserves
non-regression: the non-regressive scales form a down-closed interval, so
"shorten until the limit is reached" (L8066) is well defined. -/
theorem nonRegressive_scale {a b s : ℚ} (ha : 0 ≤ a) (hb : 0 ≤ b) (hs0 : 0 ≤ s) (hs1 : s ≤ 1)
    (h : NonRegressive a b) : NonRegressive (s * a) (s * b) := by
  unfold NonRegressive at *
  by_cases hc : s * a + s * b ≤ 1
  · exact Or.inl hc
  · right
    push Not at hc
    rcases h with h | h
    · nlinarith
    · have hc1 : 1 < a + b := by nlinarith
      have e1 : 0 ≤ s * a + s * b - 1 := by linarith
      have e2 : s * a + s * b - 1 ≤ s * (a + b - 1) := by nlinarith
      have : (s * a + s * b - 1) ^ 2 ≤ (s * (a + b - 1)) ^ 2 := by
        apply pow_le_pow_left₀ e1 e2
      have h' : (a + b - 1) ^ 2 ≤ a * b := by nlinarith
      nlinarith [mul_le_mul_of_nonneg_left h' (sq_nonneg s)]

/-- SP3 Keep Ratio limit. `r = √(ab)` (a witness, `r ≥ 0`, `r² = ab`). For a
regressive segment the scale `s* = 1/(a+b−r)` lands exactly on the boundary
`(1 − s(a+b))² = s²ab`, is non-regressive, and shortens (`0 < s* < 1`).
SPEC: L8066, L8088-8094; issue 03 (exact limit `min(1, 1/(a+b−√(ab)))`). -/
theorem keepRatio_boundary {a b r : ℚ} (ha : 0 ≤ a) (hb : 0 ≤ b) (hr : 0 ≤ r)
    (hrr : r * r = a * b) (hreg : ¬ NonRegressive a b) :
    let s := 1 / (a + b - r)
    (1 - s * (a + b)) ^ 2 = s ^ 2 * (a * b) ∧ NonRegressive (s * a) (s * b) ∧ 0 < s ∧ s < 1 := by
  intro s
  unfold NonRegressive at hreg
  push Not at hreg
  obtain ⟨h1, h2⟩ := hreg
  have hr2 : r < a + b - 1 := by
    by_contra hc; push Not at hc
    have : (a + b - 1) ^ 2 ≤ r ^ 2 := by
      apply pow_le_pow_left₀ (by linarith) hc
    nlinarith
  have hpos : 0 < a + b - r := by linarith
  have hb' : (1 - s * (a + b)) ^ 2 = s ^ 2 * (a * b) := by
    simp only [s]; rw [← hrr]; field_simp; ring
  refine ⟨hb', ?_, by positivity, ?_⟩
  · right; nlinarith [hb']
  · simp only [s]; rw [div_lt_one hpos]; linarith

/-- Maximality: any strictly larger common scale is regressive. -/
theorem keepRatio_maximal {a b r s' : ℚ} (_ha : 0 ≤ a) (_hb : 0 ≤ b) (hr : 0 ≤ r)
    (hrr : r * r = a * b) (hab : 0 < a + b)
    (hs : 1 / (a + b - r) < s') : ¬ NonRegressive (s' * a) (s' * b) := by
  have hrle : r ≤ (a + b) / 2 := by nlinarith [sq_nonneg (a - b), sq_nonneg (r - (a + b) / 2)]
  have hpos : 0 < a + b - r := by linarith
  have hs' : 1 < s' * (a + b - r) := by
    rw [div_lt_iff₀ hpos] at hs; linarith
  have hs'0 : 0 < s' := by
    have : 0 < 1 / (a + b - r) := by positivity
    linarith
  unfold NonRegressive; push Not
  have hgap : s' * r < s' * (a + b) - 1 := by linarith
  have hsr : 0 ≤ s' * r := by positivity
  constructor
  · nlinarith
  · have : (s' * r) ^ 2 < (s' * (a + b) - 1) ^ 2 := by
      apply pow_lt_pow_left₀ hgap hsr (by norm_num)
    have e1 : (s' * r) ^ 2 = s' * a * (s' * b) := by
      rw [mul_pow, sq r, hrr]; ring
    have e2 : (1 - s' * a - s' * b) ^ 2 = (s' * (a + b) - 1) ^ 2 := by ring
    rw [e1] at this; rw [e2]; linarith

/-- For equal regressive widths the limit makes each tangent exactly as wide
as the interval (issue 03). -/
theorem keepRatio_equal_widths {a : ℚ} (ha : 0 < a) : 1 / (a + a - a) * a = 1 := by
  field_simp; ring

/-- Ratio preservation is by construction (common factor). -/
theorem keepRatio_ratio (a b s : ℚ) : (s * a) * b = (s * b) * a := by ring

/-! ### Fuelled bisection -/

/-- Bisection on `f u = t` over `[lo, hi]`. -/
def bisect (f : ℚ → ℚ) (t : ℚ) : ℕ → ℚ → ℚ → ℚ × ℚ
  | 0, lo, hi => (lo, hi)
  | n + 1, lo, hi =>
    let m := (lo + hi) / 2
    if f m ≤ t then bisect f t n m hi else bisect f t n lo m

/-- The bracket invariant `f lo ≤ t ≤ f hi` is preserved (no monotonicity
needed); with a monotone `f` this brackets the root. -/
theorem bisect_bracket (f : ℚ → ℚ) (t : ℚ) :
    ∀ (n : ℕ) (lo hi : ℚ), f lo ≤ t → t ≤ f hi →
      f (bisect f t n lo hi).1 ≤ t ∧ t ≤ f (bisect f t n lo hi).2 := by
  intro n
  induction n with
  | zero => intro lo hi h1 h2; exact ⟨h1, h2⟩
  | succ n ih =>
    intro lo hi h1 h2
    simp only [bisect]
    split_ifs with h
    · exact ih _ _ h h2
    · exact ih _ _ h1 (le_of_lt (lt_of_not_ge h))

theorem bisect_width (f : ℚ → ℚ) (t : ℚ) :
    ∀ (n : ℕ) (lo hi : ℚ),
      (bisect f t n lo hi).2 - (bisect f t n lo hi).1 = (hi - lo) / 2 ^ n := by
  intro n
  induction n with
  | zero => intro lo hi; simp [bisect]
  | succ n ih =>
    intro lo hi
    simp only [bisect]
    split_ifs <;> rw [ih] <;> field_simp <;> ring

/-- Lower rational approximation of `√x` (bisection on `y² ≤ x`). -/
def sqrtLo (x : ℚ) (fuel : ℕ) : ℚ := (bisect (fun y => y * y) x fuel 0 (max 1 x)).1

/-! ## Segment and spline evaluation -/

/-- Backward-facing tangents are zero-length at evaluation. SPEC: L8054-8056. -/
def clampW (w : ℚ) : ℚ := max w 0

def fuel : ℕ := 64

/-- Curve segment value at time `t` (`k0.time ≤ t ≤ k1.time`). Hermite widths
are `d/3`. Time-linear segments are exact; otherwise Keep Ratio then bisection
(approximate). SPEC: L7671-7673, L8088-8096. -/
def curveSegValue (ct : CurveType) (k0 k1 : Knot) (t : ℚ) : ℚ :=
  let d := k1.time - k0.time
  let w0 := match ct with | .hermite => d / 3 | .bezier => clampW k0.postW
  let w1 := match ct with | .hermite => d / 3 | .bezier => clampW k1.preW
  let g : Seg := ⟨k0.time, k0.value, k1.time, k1.preValue, w0, k0.postS, w1, k1.preS⟩
  if w0 = d / 3 ∧ w1 = d / 3 then g.val ((t - k0.time) / d)
  else
    let a := w0 / d
    let b := w1 / d
    let s : ℚ := if NonRegressive a b then 1 else 1 / (a + b - sqrtLo (a * b) fuel)
    let g' : Seg := { g with w0 := s * w0, w1 := s * w1 }
    let p := bisect g'.time t fuel 0 1
    g'.val ((p.1 + p.2) / 2)

/-- Exactness: on a time-linear (or Hermite) segment the evaluated value is the
parametric value at the unique `u` with `time(u) = t`. -/
theorem curveSegValue_hermite (k0 k1 : Knot) (t : ℚ) :
    curveSegValue .hermite k0 k1 t =
      hermite k0.value k0.postS k1.preValue k1.preS (k1.time - k0.time)
        ((t - k0.time) / (k1.time - k0.time)) := by
  simp [curveSegValue, hermite_eq_bez]

/-- Segment value per interpolation mode, for `t ∈ [k0.time, k1.time]`; the
segment ends at the next knot's pre-value (L7795-7801). SPEC: L7652-7673. -/
def segValue (ct : CurveType) (k0 k1 : Knot) (t : ℚ) : Option ℚ :=
  match k0.interp with
  | .none => none
  | .held => some k0.value
  | .linear => some (k0.value + (t - k0.time) / (k1.time - k0.time) * (k1.preValue - k0.value))
  | .curve => some (curveSegValue ct k0 k1 t)

/-- Segment search from the right: `a.time ≤ t < b.time`. -/
def findR : List Knot → ℚ → Option (Knot × Knot)
  | a :: b :: rest, t => if a.time ≤ t ∧ t < b.time then some (a, b) else findR (b :: rest) t
  | _, _ => none

/-- Segment search from the left: `a.time < t ≤ b.time` (left limits). -/
def findL : List Knot → ℚ → Option (Knot × Knot)
  | a :: b :: rest, t => if a.time < t ∧ t ≤ b.time then some (a, b) else findL (b :: rest) t
  | _, _ => none

def baseR (sp : Spline) (t : ℚ) : Option ℚ :=
  (findR sp.knots t).bind fun p => segValue sp.curve p.1 p.2 t

def baseL (sp : Spline) (t : ℚ) : Option ℚ :=
  (findL sp.knots t).bind fun p => segValue sp.curve p.1 p.2 t

def lastTwo : List Knot → Option (Knot × Knot)
  | [a, b] => some (a, b)
  | _ :: b :: c :: rest => lastTwo (b :: c :: rest)
  | _ => none

/-- Linear pre-extrapolation slope: from the first segment. SPEC: L7878-7903. -/
def preSlope (sp : Spline) : ℚ :=
  match sp.knots with
  | a :: b :: _ =>
    match a.interp with
    | .curve => a.postS
    | .linear => (b.preValue - a.value) / (b.time - a.time)
    | _ => 0
  | _ => 0

/-- Linear post-extrapolation slope: from the last segment. SPEC: L7878-7903.
A single knot has no segment: slope 0 (SPEC-ISSUE: LEAN-746). -/
def postSlope (sp : Spline) : ℚ :=
  match lastTwo sp.knots with
  | some (p, l) =>
    match p.interp with
    | .curve => l.preS
    | .linear => (l.preValue - p.value) / (l.time - p.time)
    | _ => 0
  | none => 0

/-- Extrapolating loops. `P = t_last − t_first`, `n = ⌊(t − t_first)/P⌋`,
`τ = t − nP ∈ [t_first, t_last)`. SPEC: L7811-7845, L7922-7960. -/
def loopEval (mode : Extrap) (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ) : Option ℚ :=
  let P := l.time - f.time
  let n : ℤ := ⌊(t - f.time) / P⌋
  let τ := t - n * P
  match mode with
  | .loopRepeat => (bR τ).map (· + n * (l.value - f.value))
  | .loopReset => bR τ
  | .loopOscillate => if n % 2 = 0 then bR τ else bL (f.time + l.time - τ)
  | _ => none

/-- One extrapolation region (`isPre`: `t < t_first`; else `t ≥ t_last`).
SPEC: L7811-7960. -/
def extrapEval (sp : Spline) (isPre : Bool) (mode : Extrap) (f l : Knot) (t : ℚ) : Option ℚ :=
  match mode with
  | .none => none
  | .held => some (if isPre then f.value else l.value)
  | .linear => some (if isPre then f.value + preSlope sp * (t - f.time)
                     else l.value + postSlope sp * (t - l.time))
  | .sloped s => some (if isPre then f.value + s * (t - f.time) else l.value + s * (t - l.time))
  | m => if l.time = f.time then some (if isPre then f.value else l.value)
         else loopEval m f l (baseR sp) (baseL sp) t

/-- Basic-form evaluation (no inner loop). `none` = no value (value block,
`none` extrapolation, or no knots). -/
def evalBasic (sp : Spline) (t : ℚ) : Option ℚ :=
  match sp.knots.head?, sp.knots.getLast? with
  | some f, some l =>
    if t < f.time then extrapEval sp true sp.pre f l t
    else if l.time ≤ t then extrapEval sp false sp.post f l t
    else baseR sp t
  | _, _ => none

/-! ### Inner loops (SPEC: L7962-8016) -/

/-- A knot copy shifted by `i` prototype lengths, value offset `i·valueOffset`. -/
def shift (lp : LoopParams) (i : ℤ) (k : Knot) : Knot :=
  { k with time := k.time + i * (lp.protoEnd - lp.protoStart),
           value := k.value + i * lp.valueOffset,
           preValue := k.preValue + i * lp.valueOffset }

/-- Expand an inner loop: prototype knots `[protoStart, protoEnd)` copied
`numPre` times before and `numPost` times after, a closing copy of the
`protoStart` knot, authored knots inside the looped interval dropped. -/
def expandInner (lp : LoopParams) (ks : List Knot) : List Knot :=
  let L := lp.protoEnd - lp.protoStart
  let proto := ks.filter fun k => decide (lp.protoStart ≤ k.time ∧ k.time < lp.protoEnd)
  let lo := lp.protoStart - lp.numPre * L
  let hi := lp.protoEnd + lp.numPost * L
  let copies := (List.range (lp.numPre + lp.numPost + 1)).flatMap fun j =>
    proto.map (shift lp ((j : ℤ) - lp.numPre))
  let closing := match proto.head? with
    | some k => [shift lp ((lp.numPost : ℤ) + 1) k]
    | none => []
  ks.filter (fun k => decide (k.time < lo)) ++ copies ++ closing ++ ks.filter (fun k => decide (hi < k.time))

/-- Full evaluation. SPEC: L6595-6604 (inner loop → basic form → extrapolation). -/
def eval (sp : Spline) (t : ℚ) : Option ℚ :=
  match sp.loop with
  | none => evalBasic sp t
  | some lp =>
    if lp.protoStart < lp.protoEnd then
      evalBasic { sp with loop := none, knots := expandInner lp sp.knots } t
    else evalBasic sp t

/-- SP7: copies preserve knot spacing. -/
theorem shift_spacing (lp : LoopParams) (i : ℤ) (a b : Knot) :
    (shift lp i a).time - (shift lp i b).time = a.time - b.time := by
  simp only [shift]; ring

theorem shift_time (lp : LoopParams) (i : ℤ) (k : Knot) :
    (shift lp i k).time = k.time + i * (lp.protoEnd - lp.protoStart) := rfl

/-- SP7: the `i`-th copy carries `i·valueOffset` (L8005-8016 example with −5). -/
theorem shift_value (lp : LoopParams) (i : ℤ) (k : Knot) :
    (shift lp i k).value = k.value + i * lp.valueOffset ∧
      (shift lp i k).preValue = k.preValue + i * lp.valueOffset := ⟨rfl, rfl⟩

theorem shift_shift (lp : LoopParams) (i j : ℤ) (k : Knot) :
    shift lp i (shift lp j k) = shift lp (i + j) k := by
  simp only [shift]; congr 1 <;> push_cast <;> ring

/-! ### SP4 loops -/

theorem loop_floor_succ {P x : ℚ} (hP : P ≠ 0) : ⌊(x + P) / P⌋ = ⌊x / P⌋ + 1 := by
  rw [show (x + P) / P = x / P + 1 by field_simp]; exact Int.floor_add_one _

/-- SP4 repeat: one period later the value is offset by `v_last − v_first`.
SPEC L7937-7944 CLAIM: "appends the curve's last key's value to the value of
the first key's original curve". -/
theorem loop_repeat_period (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ) (hP : l.time - f.time ≠ 0) :
    loopEval .loopRepeat f l bR bL (t + (l.time - f.time)) =
      (loopEval .loopRepeat f l bR bL t).map (· + (l.value - f.value)) := by
  simp only [loopEval]
  rw [show t + (l.time - f.time) - f.time = (t - f.time) + (l.time - f.time) by ring,
    loop_floor_succ hP]
  rw [Option.map_map]
  have : t + (l.time - f.time) - ((⌊(t - f.time) / (l.time - f.time)⌋ + 1 : ℤ) : ℚ) * (l.time - f.time)
      = t - (⌊(t - f.time) / (l.time - f.time)⌋ : ℚ) * (l.time - f.time) := by push_cast; ring
  rw [this]
  congr 1; funext x; simp only [Function.comp]; push_cast; ring

/-- SP4 reset: exact repetition (L7951-7955). -/
theorem loop_reset_period (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ) (hP : l.time - f.time ≠ 0) :
    loopEval .loopReset f l bR bL (t + (l.time - f.time)) = loopEval .loopReset f l bR bL t := by
  simp only [loopEval]
  rw [show t + (l.time - f.time) - f.time = (t - f.time) + (l.time - f.time) by ring,
    loop_floor_succ hP]
  congr 1; push_cast; ring

/-- SP4 oscillate: period `2P`. -/
theorem loop_oscillate_period (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ) (hP : l.time - f.time ≠ 0) :
    loopEval .loopOscillate f l bR bL (t + 2 * (l.time - f.time)) =
      loopEval .loopOscillate f l bR bL t := by
  simp only [loopEval]
  have h2 : ⌊(t + 2 * (l.time - f.time) - f.time) / (l.time - f.time)⌋ =
      ⌊(t - f.time) / (l.time - f.time)⌋ + 2 := by
    rw [show (t + 2 * (l.time - f.time) - f.time) / (l.time - f.time) =
      (t - f.time) / (l.time - f.time) + (2 : ℤ) by push_cast; field_simp; ring]
    exact Int.floor_add_intCast _ _
  rw [h2]
  have hp : (⌊(t - f.time) / (l.time - f.time)⌋ + 2) % 2 = ⌊(t - f.time) / (l.time - f.time)⌋ % 2 := by
    omega
  rw [hp]
  have : t + 2 * (l.time - f.time) - ((⌊(t - f.time) / (l.time - f.time)⌋ + 2 : ℤ) : ℚ) * (l.time - f.time)
      = t - (⌊(t - f.time) / (l.time - f.time)⌋ : ℚ) * (l.time - f.time) := by push_cast; ring
  rw [this]

/-- SP4 oscillate mirror: the first copy after the knots is the curve reflected
about `t_last` (evaluated from the left). SPEC: L7922-7926 "reversing its
values, and therefore shape, with each cycle". -/
theorem loop_oscillate_mirror (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ)
    (hP : 0 < l.time - f.time) (h1 : l.time ≤ t) (h2 : t < l.time + (l.time - f.time)) :
    loopEval .loopOscillate f l bR bL t = bL (2 * l.time - t) := by
  simp only [loopEval]
  have hn : ⌊(t - f.time) / (l.time - f.time)⌋ = 1 := by
    rw [Int.floor_eq_iff]; constructor
    · rw [le_div_iff₀ hP]; push_cast; linarith
    · rw [div_lt_iff₀ hP]; push_cast; linarith
  rw [hn]; simp; congr 1; ring

/-- Inside the knot range a loop is the basic curve (`n = 0`). -/
theorem loop_base (m : Extrap) (f l : Knot) (bR bL : ℚ → Option ℚ) (t : ℚ)
    (hP : 0 < l.time - f.time) (h1 : f.time ≤ t) (h2 : t < l.time)
    (hm : m = .loopRepeat ∨ m = .loopReset ∨ m = .loopOscillate) :
    loopEval m f l bR bL t = bR t := by
  have hn : ⌊(t - f.time) / (l.time - f.time)⌋ = 0 := by
    rw [Int.floor_eq_iff]; constructor
    · push_cast; apply div_nonneg <;> linarith
    · rw [div_lt_iff₀ hP]; push_cast; linarith
  rcases hm with rfl | rfl | rfl <;> simp [loopEval, hn]

/-! ### SP5 / SP6 extrapolation -/

/-- SP5: linear post-extrapolation continues the last linear segment's line
(non-dual terminal knot). SPEC: L7891-7903; v1.1 change "slope from the
non-extrapolated side". -/
theorem postSlope_linear_extends (sp : Spline) (p l : Knot) (hl : lastTwo sp.knots = some (p, l))
    (hi : p.interp = .linear) (hnd : l.preValue = l.value) (hd : l.time - p.time ≠ 0) (t : ℚ) :
    segValue sp.curve p l t = some (l.value + postSlope sp * (t - l.time)) := by
  simp only [segValue, hi, postSlope, hl, Option.some.injEq]
  rw [hnd]; field_simp; ring

/-- SP6: held post-extrapolation is constant at the last knot's value.
SPEC: L7863-7870. -/
theorem evalBasic_post_held (sp : Spline) (f l : Knot) (hf : sp.knots.head? = some f)
    (hl : sp.knots.getLast? = some l) (hfl : f.time ≤ l.time) (hpost : sp.post = .held)
    (t : ℚ) (ht : l.time ≤ t) : evalBasic sp t = some l.value := by
  have : ¬ t < f.time := not_lt.2 (le_trans hfl ht)
  simp [evalBasic, hf, hl, this, ht, extrapEval, hpost]

/-- SP6 (pre side): held pre-extrapolation is constant at the first knot's
value (spec reading; pxr uses the pre-value, SPEC-ISSUE: LEAN-743). -/
theorem evalBasic_pre_held (sp : Spline) (f l : Knot) (hf : sp.knots.head? = some f)
    (hl : sp.knots.getLast? = some l) (hpre : sp.pre = .held) (t : ℚ) (ht : t < f.time) :
    evalBasic sp t = some f.value := by
  simp [evalBasic, hf, hl, ht, extrapEval, hpre]

/-! ## AutoEase (SPEC: L7704-7767)

`autoEaseSpec` transcribes the pseudocode literally; `autoEasePxr` is OpenUSD
v26.11 `Ts_TypedKnotData::_UpdateTangentAutoEase` (knotData.cpp:132-262).
Result: `(preTanSlope, preTanWidth, postTanSlope, postTanWidth)`. -/

def aeBlend (prev k next : Knot) (nextValue : ℚ) : ℚ :=
  let prevSlope := (k.value - prev.value) / (k.time - prev.time)
  let nextSlope := (nextValue - k.value) / (next.time - k.time)
  if prevSlope * nextSlope ≤ 0 then 0
  else
    let f := (k.time - prev.time) / (next.time - prev.time)
    let u := f - 1 / 2
    let g := 1 / 2 + u * (1 / 2 + 2 * u * u)
    let slope := (1 - g) * prevSlope + g * nextSlope
    if nextSlope > 0 then min slope (min (3 * nextSlope) (3 * prevSlope))
    else max slope (max (3 * nextSlope) (3 * prevSlope))

/-- Literal pseudocode, L7717-7766. SPEC-ISSUE: LEAN-741. -/
def autoEaseSpec (prev : Option Knot) (k : Knot) (next : Option Knot) : ℚ × ℚ × ℚ × ℚ :=
  let preW := match prev with | some p => (k.time - p.time) / 3 | none => k.preW
  let postW := match next with | some n => (n.time - k.time) / 3 | none => k.postW
  match prev, next with
  | some p, some n =>
    if k.dual || k.interp = .none || p.interp = .none then (0, preW, 0, postW)
    else let s := aeBlend p k n n.value; (s, preW, s, postW)
  | _, _ => (0, preW, 0, postW)

/-- OpenUSD: an off-the-end tangent is left untouched; the next knot's
*pre-value* is the blend target. -/
def autoEasePxr (prev : Option Knot) (k : Knot) (next : Option Knot) : ℚ × ℚ × ℚ × ℚ :=
  let disc := k.dual || k.interp = .none ||
    (match prev with | some p => decide (p.interp = .none) | none => true) || next.isNone
  let s := match prev, next with
    | some p, some n => if disc then 0 else aeBlend p k n n.preValue
    | _, _ => 0
  let pre := match prev with | some p => (s, (k.time - p.time) / 3) | none => (k.preS, k.preW)
  let post := match next with | some n => (s, (n.time - k.time) / 3) | none => (k.postS, k.postW)
  (pre.1, pre.2, post.1, post.2)

def aeK (t v : ℚ) (pv : ℚ := v) : Knot :=
  { time := t, value := v, preValue := pv, preW := 5, preS := 9, postW := 5, postS := 9, interp := .curve }

/-- pxr `UpdateTangents` probe "mono": (0,0),(1,1),(4,10) → slope 1.6875. -/
theorem autoEase_mono :
    autoEaseSpec (some (aeK 0 0)) (aeK 1 1) (some (aeK 4 10)) = (27/16, 1/3, 27/16, 1) ∧
    autoEasePxr (some (aeK 0 0)) (aeK 1 1) (some (aeK 4 10)) = (27/16, 1/3, 27/16, 1) := by
  decide +kernel

/-- pxr probe "mono-asym": the `3·prevSlope` clamp binds → slope 1. -/
theorem autoEase_clamp :
    autoEasePxr (some (aeK 0 0)) (aeK 3 1) (some (aeK 4 10)) = (1, 1, 1, 1/3) ∧
    autoEaseSpec (some (aeK 0 0)) (aeK 3 1) (some (aeK 4 10)) = (1, 1, 1, 1/3) := by
  decide +kernel

/-- SPEC-ISSUE: LEAN-741 (a): dual-valued next knot. Pseudocode blends toward
`nextKnot.value`; pxr toward `GetPreValue()` (37/48 = 0.770833…, probe
"nextdual"). -/
theorem autoEase_nextDual_ne :
    autoEaseSpec (some (aeK 0 0)) (aeK 1 1) (some (aeK 4 10 2)) = (27/16, 1/3, 27/16, 1) ∧
    autoEasePxr (some (aeK 0 0)) (aeK 1 1) (some (aeK 4 10 2)) = (37/48, 1/3, 37/48, 1) := by
  decide +kernel

/-- SPEC-ISSUE: LEAN-741 (b): missing neighbour. Pseudocode zeroes *both*
slopes; pxr leaves the off-the-end tangent (slope 9, width 5) untouched (probe
"noprev"). -/
theorem autoEase_noPrev_ne :
    autoEaseSpec none (aeK 1 1) (some (aeK 4 10)) = (0, 5, 0, 1) ∧
    autoEasePxr none (aeK 1 1) (some (aeK 4 10)) = (9, 5, 0, 1) := by
  decide +kernel

/-! ## Evaluation examples (all cross-checked against pxr v26.11, probes c1/c2) -/

def kn (t v : ℚ) (i : Interp := .linear) : Knot := { time := t, value := v, preValue := v, interp := i }

def triK : List Knot := [kn 0 0, kn 2 4, kn 4 1]

/-- pxr "repeat": t=5 → 3, t=−1 → 1.5, t=8 → 2. -/
theorem example_repeat :
    eval { pre := .loopRepeat, post := .loopRepeat, knots := triK } 5 = some 3 ∧
    eval { pre := .loopRepeat, post := .loopRepeat, knots := triK } (-1) = some (3/2) ∧
    eval { pre := .loopRepeat, post := .loopRepeat, knots := triK } 8 = some 2 := by
  decide +kernel

/-- pxr "reset": the join belongs to the next copy (t=4 → 0, LEAN-745). -/
theorem example_reset :
    eval { pre := .loopReset, post := .loopReset, knots := triK } 4 = some 0 ∧
    eval { pre := .loopReset, post := .loopReset, knots := triK } 7 = some (5/2) := by
  decide +kernel

/-- pxr "osc": t=5 → 2.5, t=12 → 1, t=−3 → 2.5. -/
theorem example_oscillate :
    eval { pre := .loopOscillate, post := .loopOscillate, knots := triK } 5 = some (5/2) ∧
    eval { pre := .loopOscillate, post := .loopOscillate, knots := triK } 12 = some 1 ∧
    eval { pre := .loopOscillate, post := .loopOscillate, knots := triK } (-3) = some (5/2) := by
  decide +kernel

/-- pxr "inner": `loop: (0, 4, 1, 2, 10)` over 0:0, 2:4, 4:1, 6:100. -/
theorem example_inner :
    let sp : Spline := { loop := some ⟨0, 4, 1, 2, 10⟩, knots := triK ++ [kn 6 100] }
    eval sp (-1) = some (-3) ∧ eval sp 5 = some 12 ∧ eval sp 13 = some 30 ∧ eval sp (-5) = some (-10) := by
  decide +kernel

/-- pxr "linexcurve" (Hermite): t=2 → 5, linear extrapolation uses the tangent
slopes on the inside: t=−2 → −5, t=6 → 3. -/
theorem example_hermite :
    let sp : Spline := { curve := .hermite, pre := .linear, post := .linear, knots := [⟨0, 1, 1, 0, 0, 0, 3, .curve⟩, ⟨4, 5, 5, 0, -1, 0, 0, .curve⟩] }
    eval sp 2 = some 5 ∧ eval sp (-2) = some (-5) ∧ eval sp 6 = some 3 := by
  decide +kernel

end UsdSpec.Spline
