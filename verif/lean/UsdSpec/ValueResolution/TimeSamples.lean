import Mathlib.Data.Rat.Defs
import Mathlib.Algebra.Order.Field.Rat
import Mathlib.Tactic.Linarith
import Mathlib.Tactic.FieldSimp

/-!
# Time-sample evaluation (AOUSD USD Core Specification v1.1.0, §12)

SPEC: core_spec.md L6539-6580 (Time Samples), L7571-7598 (Held),
L7599-7632 (Linear), L7364-7446 (Blocked Attributes / per-sample blocks).

Times and values are exact rationals. A sample's value is `Option ℚ`, `none`
being an authored value block (`1: None`, L7408-7417). The sample list is
sorted strictly ascending by time (`Sorted`, the spec sorts before searching,
L6406-6410 / L6562-6566, and a `timeSamples` dictionary has one value per time).

`eval i s t : Option (Option ℚ)`:
* `none`        — no samples authored (resolution falls through to weaker sources);
* `some none`   — the governing sample is a block: no value, resolution stops;
* `some (some v)` — the value.

Rules transcribed:
* before the first / after the last sample the end value is held
  ("For time sample queries prior to the start time or after the end time,
  **Held** interpolation is always used", L6578-6580; L7578-7580, L7617-7618);
* exact hit returns the sample;
* held: lower bracket (L7573-7575);
* linear: `v₀ + (t−t₀)/(t₁−t₀)·(v₁−v₀)` (`_linear_interpolate`, L6362-6364);
* blocks inside a linear segment: a blocked lower bracket blocks the interval;
  a blocked upper bracket cannot be interpolated into, so the lower value is
  held up to the block time. The spec text does not spell out the
  mixed-block segment; this is the OpenUSD (`Usd_LinearInterpolator` /
  `_GetBracketingTimeSamples` + held fallback) and LightUSD
  (`TimeInterpolator::InterpolateWithOffsets`, `src/next/types/interpolation.hh`)
  behaviour, recorded here as a modelling choice.

Only linearly interpolable values are modelled (ℚ stands for a scalar
component); L7616-7617 ("types that are not supported by linear ... behave as
if Held") is the `held` mode.
-/

namespace UsdSpec.ValueResolution.TimeSamples

/-- Interpolation mode. SPEC: L7571-7632. -/
inductive Interp
  | held
  | linear
  deriving DecidableEq, Repr

/-- `(time, value)`, value `none` = per-sample value block (L7408-7417). -/
abbrev Sample := ℚ × Option ℚ
abbrev Samples := List Sample

/-- Strictly ascending sample times. -/
def Sorted (s : Samples) : Prop := s.Pairwise (fun a b => a.1 < b.1)

/-- Linear interpolation. SPEC: L6362-6364 (`_linear_interpolate`). -/
def lerp (t₀ v₀ t₁ v₁ t : ℚ) : ℚ := v₀ + (t - t₀) / (t₁ - t₀) * (v₁ - v₀)

/-- The value on the half-open segment `[p.1, q.1)` (`p.1 ≤ t < q.1`). -/
def interpSeg (i : Interp) (p q : Sample) (t : ℚ) : Option ℚ :=
  if t = p.1 then p.2 else
  match i with
  | .held => p.2
  | .linear =>
    match p.2, q.2 with
    | some a, some b => some (lerp p.1 a q.1 b t)
    | some a, none => some a      -- blocked upper bracket: hold the lower value
    | none, _ => none             -- blocked lower bracket governs the interval

/-- Walk with the previous sample `p` (`t > p.1` or exact), remaining samples. -/
def evalAux (i : Interp) (t : ℚ) : Sample → Samples → Option ℚ
  | p, [] => p.2                                   -- after the last sample: hold
  | p, q :: rest => if t < q.1 then interpSeg i p q t else evalAux i t q rest

/-- Evaluate time samples at `t`. SPEC: L6366-6402 (`_interpolate`), L6578-6580. -/
def eval (i : Interp) (s : Samples) (t : ℚ) : Option (Option ℚ) :=
  match s with
  | [] => none
  | p :: rest => some (if t ≤ p.1 then p.2 else evalAux i t p rest)

/-! ## Equations -/

/-- Skipping past samples at or before `t`. -/
theorem evalAux_skip (i : Interp) (t : ℚ) (a p : Sample) (rest : Samples) :
    ∀ pre : Samples, (∀ x ∈ pre ++ [p], x.1 ≤ t) →
      evalAux i t a (pre ++ p :: rest) = evalAux i t p rest := by
  intro pre
  induction pre generalizing a with
  | nil =>
    intro h
    have hp : p.1 ≤ t := h p (by simp)
    simp [evalAux, not_lt.mpr hp]
  | cons b pre ih =>
    intro h
    have hb : b.1 ≤ t := h b (by simp)
    simp only [List.cons_append, evalAux, not_lt.mpr hb, ite_false]
    exact ih b (fun x hx => h x (by simp_all))

/-- At an authored sample time the sample is returned (value or block).
CLAIM: "if time_data[self.TIME] == time: return time_data[self.DATA].value" (L6383-6384). -/
theorem eval_at_sample (i : Interp) {s : Samples} (hs : Sorted s) {t : ℚ} {v : Option ℚ}
    (hm : (t, v) ∈ s) : eval i s t = some v := by
  -- auxiliary statement on the walk
  have aux : ∀ (p : Sample) (rest : Samples), Sorted (p :: rest) → (t, v) ∈ p :: rest →
      p.1 ≤ t → evalAux i t p rest = v := by
    intro p rest
    induction rest generalizing p with
    | nil => intro _ h _; simp at h; subst h; rfl
    | cons q rest ih =>
      intro hs hm hpt
      simp only [Sorted, List.pairwise_cons, List.mem_cons] at hs
      obtain ⟨hp, hq⟩ := hs
      simp only [evalAux]
      by_cases htq : t < q.1
      · rw [if_pos htq]
        rcases List.mem_cons.mp hm with h | h
        · subst h; simp [interpSeg]
        · exfalso
          rcases List.mem_cons.mp h with h' | h'
          · subst h'; exact lt_irrefl _ htq
          · have := hq.1 _ h'; simp at this; linarith
      · rw [if_neg htq]
        rcases List.mem_cons.mp hm with h | h
        · subst h; exfalso; have := hp q (by simp); simp at this; linarith
        · exact ih q (by simpa [Sorted] using hq) h (not_lt.mp htq)
  cases s with
  | nil => simp at hm
  | cons p rest =>
    simp only [eval]
    by_cases htp : t ≤ p.1
    · rw [if_pos htp]
      rcases List.mem_cons.mp hm with h | h
      · subst h; rfl
      · exfalso
        have := (List.pairwise_cons.mp hs).1 _ h; simp at this; linarith
    · rw [if_neg htp]
      congr 1
      exact aux p rest hs hm (le_of_lt (not_le.mp htp))

/-- A per-sample block at an authored time blocks the attribute at that time
(L7408-7417). -/
theorem eval_block_at_sample (i : Interp) {s : Samples} (hs : Sorted s) {t : ℚ}
    (hm : (t, none) ∈ s) : eval i s t = some none := eval_at_sample i hs hm

/-- Inside a bracket `p.1 ≤ t < q.1` of adjacent samples, evaluation is the
segment rule. -/
theorem eval_bracket (i : Interp) (pre post : Samples) (p q : Sample)
    (hs : Sorted (pre ++ p :: q :: post)) {t : ℚ} (hp : p.1 ≤ t) (hq : t < q.1) :
    eval i (pre ++ p :: q :: post) t = some (interpSeg i p q t) := by
  have hpre : ∀ x ∈ pre, x.1 < p.1 := by
    intro x hx
    have := (List.pairwise_append.mp hs).2.2 x hx p (by simp)
    exact this
  cases pre with
  | nil =>
    simp only [List.nil_append, eval]
    by_cases htp : t ≤ p.1
    · have : t = p.1 := le_antisymm htp hp
      simp [interpSeg, this]
    · simp [htp, evalAux, hq]
  | cons x pre =>
    have hx : x.1 < t := lt_of_lt_of_le (hpre x (by simp)) hp
    simp only [List.cons_append, eval, not_le.mpr hx, ite_false]
    rw [evalAux_skip i t x p (q :: post) pre (fun y hy => by
      rcases List.mem_append.mp hy with h | h
      · exact le_of_lt (lt_of_lt_of_le (hpre y (by simp [h])) hp)
      · simp at h; subst h; exact hp)]
    simp [evalAux, hq]

/-- Held = lower bracket. CLAIM: "The value is held to the nearest previous value
at the authored time" (L7573-7574). -/
theorem eval_held_bracket (pre post : Samples) (p q : Sample)
    (hs : Sorted (pre ++ p :: q :: post)) {t : ℚ} (hp : p.1 ≤ t) (hq : t < q.1) :
    eval .held (pre ++ p :: q :: post) t = some p.2 := by
  rw [eval_bracket .held pre post p q hs hp hq]; simp [interpSeg]

/-- Out of range, before: the first sample is held (L6578-6580, L7579). -/
theorem eval_before (i : Interp) (p : Sample) (rest : Samples) {t : ℚ} (h : t ≤ p.1) :
    eval i (p :: rest) t = some p.2 := by simp [eval, h]

/-- Out of range, after: the last sample is held (L6578-6580, L7617). -/
theorem eval_after (i : Interp) (pre : Samples) (p : Sample) (hs : Sorted (pre ++ [p]))
    {t : ℚ} (h : p.1 ≤ t) : eval i (pre ++ [p]) t = some p.2 := by
  have hpre : ∀ x ∈ pre, x.1 < p.1 := fun x hx =>
    (List.pairwise_append.mp hs).2.2 x hx p (by simp)
  cases pre with
  | nil =>
    simp only [List.nil_append, eval]
    by_cases htp : t ≤ p.1 <;> simp [htp, evalAux]
  | cons x pre =>
    have hx : x.1 < t := lt_of_lt_of_le (hpre x (by simp)) h
    simp only [List.cons_append, eval, not_le.mpr hx, ite_false]
    have := evalAux_skip i t x p [] pre (fun y hy => by
      rcases List.mem_append.mp hy with h' | h'
      · exact le_of_lt (lt_of_lt_of_le (hpre y (by simp [h'])) h)
      · simp at h'; subst h'; exact h)
    rw [this]; rfl

/-- Out-of-range evaluation is clamping: it equals evaluation at the clamped time. -/
theorem eval_clamp (i : Interp) (pre : Samples) (p q : Sample) (post : Samples)
    (_hs : Sorted (p :: (pre ++ q :: post))) {t : ℚ} :
    (t ≤ p.1 → eval i (p :: (pre ++ q :: post)) t = eval i (p :: (pre ++ q :: post)) p.1) := by
  intro h; simp [eval, h]

/-- Linear at a sample time returns the sample (special case of `eval_at_sample`). -/
theorem linear_at_sample {s : Samples} (hs : Sorted s) {t v : ℚ} (hm : (t, some v) ∈ s) :
    eval .linear s t = some (some v) := eval_at_sample .linear hs hm

/-- `lerp` hits the end points. -/
theorem lerp_left (t₀ v₀ t₁ v₁ : ℚ) : lerp t₀ v₀ t₁ v₁ t₀ = v₀ := by simp [lerp]
theorem lerp_right (t₀ v₀ t₁ v₁ : ℚ) (h : t₀ ≠ t₁) : lerp t₀ v₀ t₁ v₁ t₁ = v₁ := by
  have : t₁ - t₀ ≠ 0 := sub_ne_zero.mpr (Ne.symm h)
  simp [lerp, div_self this]

/-- The interpolation parameter lies in `[0,1]` on the segment. -/
theorem lerp_param_mem {t₀ t₁ t : ℚ} (h₀ : t₀ ≤ t) (h₁ : t ≤ t₁) (hlt : t₀ < t₁) :
    0 ≤ (t - t₀) / (t₁ - t₀) ∧ (t - t₀) / (t₁ - t₀) ≤ 1 := by
  have hd : 0 < t₁ - t₀ := sub_pos.mpr hlt
  refine ⟨div_nonneg (by linarith) (le_of_lt hd), ?_⟩
  rw [div_le_one₀ hd]; linarith

/-- Linear between two samples lies between them. -/
theorem lerp_between {t₀ t₁ t v₀ v₁ : ℚ} (h₀ : t₀ ≤ t) (h₁ : t ≤ t₁) (hlt : t₀ < t₁) :
    min v₀ v₁ ≤ lerp t₀ v₀ t₁ v₁ t ∧ lerp t₀ v₀ t₁ v₁ t ≤ max v₀ v₁ := by
  obtain ⟨l0, l1⟩ := lerp_param_mem h₀ h₁ hlt
  set l := (t - t₀) / (t₁ - t₀)
  simp only [lerp]
  rcases le_total v₀ v₁ with h | h
  · rw [min_eq_left h, max_eq_right h]
    constructor <;> nlinarith
  · rw [min_eq_right h, max_eq_left h]
    constructor <;> nlinarith

/-- Linear is monotone on a segment with `v₀ ≤ v₁` (the 2-point case). -/
theorem lerp_mono {t₀ t₁ v₀ v₁ : ℚ} (hlt : t₀ < t₁) (hv : v₀ ≤ v₁) {t t' : ℚ} (htt : t ≤ t') :
    lerp t₀ v₀ t₁ v₁ t ≤ lerp t₀ v₀ t₁ v₁ t' := by
  have hd : 0 < t₁ - t₀ := sub_pos.mpr hlt
  simp only [lerp]
  have : (t - t₀) / (t₁ - t₀) ≤ (t' - t₀) / (t₁ - t₀) :=
    div_le_div_of_nonneg_right (by linarith) (le_of_lt hd)
  nlinarith

/-- Linear between two adjacent unblocked samples is between them. -/
theorem eval_linear_between (pre post : Samples) (t₀ t₁ v₀ v₁ : ℚ)
    (hs : Sorted (pre ++ (t₀, some v₀) :: (t₁, some v₁) :: post)) {t : ℚ}
    (hp : t₀ ≤ t) (hq : t < t₁) :
    ∃ v, eval .linear (pre ++ (t₀, some v₀) :: (t₁, some v₁) :: post) t = some (some v) ∧
      min v₀ v₁ ≤ v ∧ v ≤ max v₀ v₁ := by
  rw [eval_bracket .linear pre post _ _ hs hp hq]
  by_cases ht : t = t₀
  · refine ⟨v₀, by simp [interpSeg, ht], min_le_left _ _, le_max_left _ _⟩
  · refine ⟨lerp t₀ v₀ t₁ v₁ t, by simp [interpSeg, ht], ?_⟩
    exact lerp_between hp (le_of_lt hq) (lt_of_le_of_lt hp hq)

/-! ## Spec examples -/

/-- L7408-7417: `1: None, 2: 2.5`. -/
def blockEx : Samples := [(1, none), (2, some (5/2))]
/-- L7577-7591: `100:10, 200:20`, held. -/
def heldEx : Samples := [(100, some 10), (200, some 20)]
example : eval .held heldEx 150 = some (some 10) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .held heldEx 199 = some (some 10) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .held heldEx 50 = some (some 10) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .held heldEx 250 = some (some 20) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .linear heldEx 150 = some (some 15) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx]
example : eval .linear blockEx 1 = some none := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .linear blockEx (3/2) = some none := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .linear blockEx 0 = some none := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
example : eval .linear blockEx 2 = some (some (5/2)) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]
/-- Blocked upper bracket: hold the lower value up to the block. -/
example : eval .linear [(0, some 1), (2, none)] 1 = some (some 1) := by
  norm_num [eval, evalAux, interpSeg, lerp, heldEx, blockEx]

end UsdSpec.ValueResolution.TimeSamples
