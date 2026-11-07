import UsdSpec.ValueResolution.Spline
import Mathlib.Analysis.Calculus.Deriv.MeanValue
import Mathlib.Analysis.Calculus.Deriv.Polynomial
import Mathlib.Algebra.Polynomial.Roots
import Mathlib.Order.Interval.Set.Infinite
import Mathlib.Topology.Order.IntermediateValue
import Mathlib.Analysis.SpecialFunctions.Pow.Real
/-!
# Certified spline bisection value error

Bounds the value polynomial on `[0,1]` and connects the rational bisection
actually used by spline evaluation to a real time-curve root. Real roots need
not be rational. The error decreases by a factor of two per bisection step.
Keep Ratio safety yields a strictly monotone time curve and a unique real
root, so the bound applies to the value of the *rationally corrected* segment
(Keep Ratio with a square-root under-approximation); the distance to the exact
Keep Ratio limit (L8088-8095) is not bounded.
No floating-point rounding bound is asserted for the native C++ evaluator.
-/

namespace UsdSpec.Spline.Bounds

def bezier (p0 p1 p2 p3 u : ℝ) : ℝ :=
  (1-u)^3*p0 + 3*(1-u)^2*u*p1 + 3*(1-u)*u^2*p2 + u^3*p3

def valueBound (p0 p1 p2 p3 : ℝ) : ℝ :=
  |3*(p1-p0)| + 2*|3*(p0-2*p1+p2)| + 3*|-p0+3*p1-3*p2+p3|

theorem square_diff_bound {u v : ℝ} (hu0 : 0 ≤ u) (hu1 : u ≤ 1)
    (hv0 : 0 ≤ v) (hv1 : v ≤ 1) : |u^2-v^2| ≤ 2*|u-v| := by
  have he : u^2-v^2 = (u-v)*(u+v) := by ring
  rw [he, abs_mul, abs_of_nonneg (by linarith : 0 ≤ u+v)]
  nlinarith [mul_le_mul_of_nonneg_left (show u+v ≤ 2 by linarith) (abs_nonneg (u-v))]

theorem cube_diff_bound {u v : ℝ} (hu0 : 0 ≤ u) (hu1 : u ≤ 1)
    (hv0 : 0 ≤ v) (hv1 : v ≤ 1) : |u^3-v^3| ≤ 3*|u-v| := by
  have hu2 : u^2 ≤ 1 := by nlinarith [mul_nonneg hu0 (sub_nonneg.mpr hu1)]
  have hv2 : v^2 ≤ 1 := by nlinarith [mul_nonneg hv0 (sub_nonneg.mpr hv1)]
  have huv : u*v ≤ 1 := by nlinarith [mul_le_mul hu1 hv1 hv0 (by norm_num : (0:ℝ) ≤ 1)]
  have he : u^3-v^3 = (u-v)*(u^2+u*v+v^2) := by ring
  rw [he, abs_mul, abs_of_nonneg (by positivity : 0 ≤ u^2+u*v+v^2)]
  nlinarith [mul_le_mul_of_nonneg_left (show u^2+u*v+v^2 ≤ 3 by linarith) (abs_nonneg (u-v))]

/-- An explicit control-point bound for the cubic on the full unit interval. -/
theorem bezier_lipschitz (p0 p1 p2 p3 : ℝ) {u v : ℝ}
    (hu0 : 0 ≤ u) (hu1 : u ≤ 1) (hv0 : 0 ≤ v) (hv1 : v ≤ 1) :
    |bezier p0 p1 p2 p3 u - bezier p0 p1 p2 p3 v| ≤
      valueBound p0 p1 p2 p3 * |u-v| := by
  let a := 3*(p1-p0)
  let b := 3*(p0-2*p1+p2)
  let c := -p0+3*p1-3*p2+p3
  have he : bezier p0 p1 p2 p3 u - bezier p0 p1 p2 p3 v =
      a*(u-v)+b*(u^2-v^2)+c*(u^3-v^3) := by simp only [bezier, a, b, c]; ring
  rw [he]
  have htriangle := le_trans (abs_add_le (a*(u-v)+b*(u^2-v^2)) (c*(u^3-v^3)))
    (add_le_add (abs_add_le (a*(u-v)) (b*(u^2-v^2))) (le_refl |c*(u^3-v^3)|))
  simp only [abs_mul] at htriangle
  have hb := mul_le_mul_of_nonneg_left (square_diff_bound hu0 hu1 hv0 hv1) (abs_nonneg b)
  have hc := mul_le_mul_of_nonneg_left (cube_diff_bound hu0 hu1 hv0 hv1) (abs_nonneg c)
  change _ ≤ (|a| + 2*|b| + 3*|c|)*|u-v|
  nlinarith

theorem bezier_cast (p0 p1 p2 p3 u : ℚ) :
    ((bez p0 p1 p2 p3 u : ℚ) : ℝ) =
      bezier (p0 : ℝ) (p1 : ℝ) (p2 : ℝ) (p3 : ℝ) (u : ℝ) := by
  simp [bez, bezier]

def timeReal (g : Seg) (u : ℝ) : ℝ :=
  bezier (g.t0 : ℝ) ((g.t0 + g.w0 : ℚ) : ℝ)
    ((g.t1 - g.w1 : ℚ) : ℝ) (g.t1 : ℝ) u

def valReal (g : Seg) (u : ℝ) : ℝ :=
  bezier (g.v0 : ℝ) ((g.v0 + g.s0*g.w0 : ℚ) : ℝ)
    ((g.v1 - g.s1*g.w1 : ℚ) : ℝ) (g.v1 : ℝ) u

def segmentValueBound (g : Seg) : ℝ :=
  valueBound (g.v0 : ℝ) ((g.v0 + g.s0*g.w0 : ℚ) : ℝ)
    ((g.v1 - g.s1*g.w1 : ℚ) : ℝ) (g.v1 : ℝ)

theorem timeReal_cast (g : Seg) (u : ℚ) : timeReal g (u : ℝ) = (g.time u : ℝ) := by
  simpa [timeReal, Seg.time] using (bezier_cast g.t0 (g.t0+g.w0) (g.t1-g.w1) g.t1 u).symm

theorem valReal_cast (g : Seg) (u : ℚ) : valReal g (u : ℝ) = (g.val u : ℝ) := by
  simpa [valReal, Seg.val] using (bezier_cast g.v0 (g.v0+g.s0*g.w0) (g.v1-g.s1*g.w1) g.v1 u).symm

/-- For any fuel and target time inside the knot interval, bisection selects a
real root in its final bracket. The midpoint value is within the explicit
control-point bound divided by `2^(n+1)` of that root's value. This statement
requires no rational-root assumption. -/
theorem bisect_value_error (g : Seg) (t : ℚ) (ht0 : g.t0 ≤ t) (ht1 : t ≤ g.t1) (n : ℕ) :
    let p := bisect g.time t n 0 1
    ∃ u : ℝ, u ∈ Set.Icc (p.1 : ℝ) (p.2 : ℝ) ∧ timeReal g u = (t : ℝ) ∧
      |(g.val ((p.1+p.2)/2) : ℝ) - valReal g u| ≤ segmentValueBound g / 2^(n+1) := by
  dsimp only
  let p := bisect g.time t n 0 1
  obtain ⟨hlo, horder, hhi⟩ := bisect_bounds g.time t n 0 1 (by norm_num)
  obtain ⟨hleft, hright⟩ := bisect_bracket g.time t n 0 1
    (by simpa [Seg.time_zero] using ht0) (by simpa [Seg.time_one] using ht1)
  have hwidth := bisect_width g.time t n 0 1
  change 0 ≤ p.1 at hlo
  change p.1 ≤ p.2 at horder
  change p.2 ≤ 1 at hhi
  change g.time p.1 ≤ t at hleft
  change t ≤ g.time p.2 at hright
  have hc : ContinuousOn (timeReal g) (Set.Icc (p.1 : ℝ) (p.2 : ℝ)) := by
    unfold timeReal bezier
    fun_prop
  obtain ⟨u, hu, htime⟩ := intermediate_value_Icc (by exact_mod_cast horder) hc
    (show (t : ℝ) ∈ Set.Icc (timeReal g (p.1 : ℝ)) (timeReal g (p.2 : ℝ)) by
      rw [timeReal_cast, timeReal_cast]
      constructor <;> exact_mod_cast ‹_›)
  refine ⟨u, hu, htime, ?_⟩
  have h0 : (0 : ℝ) ≤ (p.1 : ℝ) := by exact_mod_cast hlo
  have h1 : (p.2 : ℝ) ≤ (1 : ℝ) := by exact_mod_cast hhi
  let m : ℚ := (p.1+p.2)/2
  have hm0 : (0 : ℝ) ≤ (m : ℝ) := by dsimp [m]; push_cast; linarith [hu.1, hu.2]
  have hm1 : (m : ℝ) ≤ (1 : ℝ) := by dsimp [m]; push_cast; linarith [hu.1, hu.2]
  have hdist : |(m : ℝ)-u| ≤ 1 / 2^(n+1) := by
    have hwq : p.2-p.1 = 1/(2:ℚ)^n := by simpa [p] using hwidth
    have hw : (p.2 : ℝ) - (p.1 : ℝ) = 1 / (2:ℝ)^n := by
      have h := congrArg (fun x : ℚ => (x : ℝ)) hwq
      push_cast at h
      exact h
    have hrad : (1:ℝ)/2^(n+1) = ((p.2 : ℝ)-(p.1 : ℝ))/2 := by
      rw [pow_succ, div_mul_eq_div_div, ← hw]
    rw [hrad]
    apply abs_le.mpr
    dsimp [m]
    push_cast
    constructor <;> linarith [hu.1, hu.2]
  have hlip := bezier_lipschitz (g.v0 : ℝ) ((g.v0+g.s0*g.w0 : ℚ) : ℝ)
    ((g.v1-g.s1*g.w1 : ℚ) : ℝ) (g.v1 : ℝ) hm0 hm1
    (le_trans h0 hu.1) (le_trans hu.2 h1)
  change |valReal g (m : ℝ) - valReal g u| ≤ segmentValueBound g * |(m : ℝ)-u| at hlip
  change |(g.val m : ℝ) - valReal g u| ≤ _
  rw [← valReal_cast]
  have hk : 0 ≤ segmentValueBound g := by unfold segmentValueBound valueBound; positivity
  have h := mul_le_mul_of_nonneg_left hdist hk
  simpa [div_eq_mul_inv] using le_trans hlip h

open Polynomial

noncomputable def poly (p0 p1 p2 p3 : ℝ) : Polynomial ℝ :=
  C p0 + C (3*(p1-p0))*X + C (3*(p0-2*p1+p2))*X^2 + C (-p0+3*p1-3*p2+p3)*X^3

theorem poly_eval (p0 p1 p2 p3 u : ℝ) : (poly p0 p1 p2 p3).eval u = bezier p0 p1 p2 p3 u := by
  simp [poly, bezier]
  ring

theorem bezier_hasDerivAt (p0 p1 p2 p3 u : ℝ) :
    HasDerivAt (bezier p0 p1 p2 p3)
      (3*((1-u)^2*(p1-p0)+2*(1-u)*u*(p2-p1)+u^2*(p3-p2))) u := by
  have h := (poly p0 p1 p2 p3).hasDerivAt u
  simp_rw [poly_eval] at h
  convert h using 1
  simp [poly]
  ring

/-- A nonconstant monotone cubic is strictly monotone: a flat interval would
supply infinitely many roots of its difference from a constant polynomial. -/
theorem bezier_strictMono_of_monotone (p0 p1 p2 p3 : ℝ) (hd : p0 ≠ p3)
    (hmono : MonotoneOn (bezier p0 p1 p2 p3) (Set.Icc 0 1)) :
    StrictMonoOn (bezier p0 p1 p2 p3) (Set.Icc 0 1) := by
  intro a ha b hb hab
  have hle := hmono ha hb (le_of_lt hab)
  by_contra hnot
  have heq : bezier p0 p1 p2 p3 a = bezier p0 p1 p2 p3 b := by linarith
  have hflat : ∀ x ∈ Set.Icc a b, bezier p0 p1 p2 p3 x = bezier p0 p1 p2 p3 a := by
    intro x hx
    have hx01 : x ∈ Set.Icc (0:ℝ) 1 := ⟨le_trans ha.1 hx.1, le_trans hx.2 hb.2⟩
    have h1 := hmono ha hx01 hx.1
    have h2 := hmono hx01 hb hx.2
    linarith
  have hinfinite : Set.Infinite {x | (poly p0 p1 p2 p3).eval x =
      (C (bezier p0 p1 p2 p3 a)).eval x} := by
    apply (Set.Icc_infinite hab).mono
    intro x hx
    simpa [poly_eval] using hflat x hx
  have hp := Polynomial.eq_of_infinite_eval_eq (poly p0 p1 p2 p3)
    (C (bezier p0 p1 p2 p3 a)) hinfinite
  have h0 := congrArg (Polynomial.eval (0:ℝ)) hp
  have h1 := congrArg (Polynomial.eval (1:ℝ)) hp
  simp [poly] at h0 h1
  apply hd
  linarith


theorem tq_real_nonneg {a b : ℝ} (ha : 0 ≤ a) (hb : 0 ≤ b)
    (h : a+b ≤ 1 ∨ (1-a-b)^2 ≤ a*b) {u : ℝ} (hu0 : 0 ≤ u) (hu1 : u ≤ 1) :
    0 ≤ a*(1-u)^2+2*(1-a-b)*u*(1-u)+b*u^2 := by
  have hx : 0 ≤ 1-u := by linarith
  rcases h with h | h
  · have : 0 ≤ 1-a-b := by linarith
    positivity
  · set x := 1-u
    set B := 1-a-b
    rcases eq_or_lt_of_le ha with ha0 | hapos
    · subst ha0
      have : B = 0 := by nlinarith [sq_nonneg B]
      rw [this]
      positivity
    · have key : a*(a*x^2+2*B*u*x+b*u^2) =
          (a*x+B*u)^2+(a*b-B^2)*u^2 := by ring
      have hp : 0 ≤ a*(a*x^2+2*B*u*x+b*u^2) := by
        rw [key]
        have : 0 ≤ a*b-B^2 := by linarith
        positivity
      have := nonneg_of_mul_nonneg_right hp hapos
      nlinarith

theorem timeReal_monotone (g : Seg) (hd : 0 < g.t1-g.t0)
    (hw0 : 0 ≤ g.w0) (hw1 : 0 ≤ g.w1)
    (hnr : NonRegressive (g.w0/(g.t1-g.t0)) (g.w1/(g.t1-g.t0))) :
    MonotoneOn (timeReal g) (Set.Icc 0 1) := by
  have hdR : 0 < (g.t1 : ℝ)-(g.t0 : ℝ) := by exact_mod_cast hd
  have hdne : (g.t1 : ℝ)-(g.t0 : ℝ) ≠ 0 := ne_of_gt hdR
  have hnon : (g.w0/(g.t1-g.t0) : ℚ) + g.w1/(g.t1-g.t0) ≤ 1 ∨
      (1-g.w0/(g.t1-g.t0)-g.w1/(g.t1-g.t0))^2 ≤
        (g.w0/(g.t1-g.t0))*(g.w1/(g.t1-g.t0)) := hnr
  have hnonR : (g.w0 : ℝ)/((g.t1 : ℝ)-g.t0) + (g.w1 : ℝ)/((g.t1 : ℝ)-g.t0) ≤ 1 ∨
      (1-(g.w0 : ℝ)/((g.t1 : ℝ)-g.t0)-(g.w1 : ℝ)/((g.t1 : ℝ)-g.t0))^2 ≤
        ((g.w0 : ℝ)/((g.t1 : ℝ)-g.t0))*((g.w1 : ℝ)/((g.t1 : ℝ)-g.t0)) := by
    exact_mod_cast hnon
  have hderiv (u : ℝ) := bezier_hasDerivAt (g.t0 : ℝ) ((g.t0+g.w0 : ℚ) : ℝ)
    ((g.t1-g.w1 : ℚ) : ℝ) (g.t1 : ℝ) u
  apply monotoneOn_of_deriv_nonneg (convex_Icc (0:ℝ) 1)
  · unfold timeReal bezier
    fun_prop
  · intro u _
    exact (hderiv u).differentiableAt.differentiableWithinAt
  · intro u hu
    have hu01 : u ∈ Set.Icc (0:ℝ) 1 := interior_subset hu
    change 0 ≤ deriv (bezier (g.t0 : ℝ) ((g.t0+g.w0 : ℚ) : ℝ)
      ((g.t1-g.w1 : ℚ) : ℝ) (g.t1 : ℝ)) u
    rw [(hderiv u).deriv]
    have he : 3*((1-u)^2*(((g.t0+g.w0 : ℚ) : ℝ)-g.t0)+
        2*(1-u)*u*(((g.t1-g.w1 : ℚ) : ℝ)-((g.t0+g.w0 : ℚ) : ℝ))+
        u^2*((g.t1 : ℝ)-((g.t1-g.w1 : ℚ) : ℝ))) =
        3*((g.t1 : ℝ)-g.t0)*(
          ((g.w0 : ℝ)/((g.t1 : ℝ)-g.t0))*(1-u)^2+
          2*(1-(g.w0 : ℝ)/((g.t1 : ℝ)-g.t0)-(g.w1 : ℝ)/((g.t1 : ℝ)-g.t0))*u*(1-u)+
          ((g.w1 : ℝ)/((g.t1 : ℝ)-g.t0))*u^2) := by
      push_cast
      field_simp [hdne]
      ring
    rw [he]
    apply mul_nonneg (by positivity)
    apply tq_real_nonneg _ _ hnonR hu01.1 hu01.2
    · exact div_nonneg (by exact_mod_cast hw0) (le_of_lt hdR)
    · exact div_nonneg (by exact_mod_cast hw1) (le_of_lt hdR)

/-- Non-regression and positive duration imply a unique time parameter, even
at the Keep Ratio boundary where the derivative can vanish at isolated points. -/
theorem timeReal_strictMono (g : Seg) (hd : 0 < g.t1-g.t0)
    (hw0 : 0 ≤ g.w0) (hw1 : 0 ≤ g.w1)
    (hnr : NonRegressive (g.w0/(g.t1-g.t0)) (g.w1/(g.t1-g.t0))) :
    StrictMonoOn (timeReal g) (Set.Icc 0 1) := by
  apply bezier_strictMono_of_monotone
  · have h : g.t0 < g.t1 := by linarith
    have hr : (g.t0 : ℝ) < (g.t1 : ℝ) := by exact_mod_cast h
    exact ne_of_lt hr
  · exact timeReal_monotone g hd hw0 hw1 hnr


/-- Error against any real root of a valid non-regressive segment, rather
than only the particular root obtained by the intermediate-value argument. -/
theorem bisect_value_error_for_root (g : Seg) (hd : 0 < g.t1-g.t0)
    (hw0 : 0 ≤ g.w0) (hw1 : 0 ≤ g.w1)
    (hnr : NonRegressive (g.w0/(g.t1-g.t0)) (g.w1/(g.t1-g.t0)))
    (t : ℚ) (ht0 : g.t0 ≤ t) (ht1 : t ≤ g.t1) (n : ℕ)
    (v : ℝ) (hv : v ∈ Set.Icc 0 1) (hvtime : timeReal g v = (t:ℝ)) :
    let p := bisect g.time t n 0 1
    |(g.val ((p.1+p.2)/2) : ℝ) - valReal g v| ≤ segmentValueBound g / 2^(n+1) := by
  obtain ⟨u, hu, hutime, herror⟩ := bisect_value_error g t ht0 ht1 n
  obtain ⟨hlo, _, hhi⟩ := bisect_bounds g.time t n 0 1 (by norm_num)
  have hu01 : u ∈ Set.Icc (0:ℝ) 1 := by
    constructor
    · exact le_trans (by exact_mod_cast hlo) hu.1
    · exact le_trans hu.2 (by exact_mod_cast hhi)
  have heq : v = u := (timeReal_strictMono g hd hw0 hw1 hnr).injOn hv hu01
    (by rw [hvtime, hutime])
  simpa only [heq] using herror

/-- The certified value-error bound for the corrected segment and midpoint
calculation actually used by curveSegValue, at arbitrary fuel. -/
theorem keepRatio_value_error (g : Seg) (hd : 0 < g.t1-g.t0)
    (hw0 : 0 ≤ g.w0) (hw1 : 0 ≤ g.w1)
    (t : ℚ) (ht0 : g.t0 ≤ t) (ht1 : t ≤ g.t1) (n : ℕ)
    (u : ℝ) (hu : u ∈ Set.Icc 0 1) (hutime : timeReal (g.keepRatio n) u = (t:ℝ)) :
    let corrected := g.keepRatio n
    let p := bisect corrected.time t n 0 1
    |(corrected.val ((p.1+p.2)/2) : ℝ) - valReal corrected u| ≤
      segmentValueBound corrected / 2^(n+1) := by
  obtain ⟨h0, _, h1, _, hnr⟩ := g.keepRatio_safe hd hw0 hw1 n
  apply bisect_value_error_for_root (g.keepRatio n) hd h0 h1 hnr t ht0 ht1 n u hu hutime


/-- The root used by the error bound exists and is unique on the full unit
interval for every positive-duration corrected segment and in-range time. -/
theorem keepRatio_root_unique (g : Seg) (hd : 0 < g.t1-g.t0)
    (hw0 : 0 ≤ g.w0) (hw1 : 0 ≤ g.w1)
    (t : ℚ) (ht0 : g.t0 ≤ t) (ht1 : t ≤ g.t1) (n : ℕ) :
    ∃! u : ℝ, u ∈ Set.Icc 0 1 ∧ timeReal (g.keepRatio n) u = (t:ℝ) := by
  obtain ⟨h0, _, h1, _, hnr⟩ := g.keepRatio_safe hd hw0 hw1 n
  obtain ⟨u, hu, hutime, _⟩ := bisect_value_error (g.keepRatio n) t ht0 ht1 0
  have hu01 : u ∈ Set.Icc (0:ℝ) 1 := by simpa [bisect] using hu
  refine ⟨u, ⟨hu01, hutime⟩, ?_⟩
  intro v hv
  exact (timeReal_strictMono (g.keepRatio n) hd h0 h1 hnr).injOn hv.1 hu01
    (by rw [hv.2, hutime])

end UsdSpec.Spline.Bounds
