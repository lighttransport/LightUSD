import Mathlib.Data.Rat.Floor
import Mathlib.Tactic

/-!
# Value clips (AOUSD USD Core Specification v1.1.0, §12 Value Clips)

SPEC: core_spec.md L6607-7308. Modelled: the explicit clip-set fields
(`assetPaths`, `active`, `times`), template expansion (`templateAssetPath`,
`templateStartTime`, `templateEndTime`, `templateStride`,
`templateActiveOffset`), active-clip selection (L6941-6958), the stage→clip
timing curve (L6962-6994) with jump discontinuities (L7222-7236) and clamping.
`primPath`, `manifestAssetPath` and `interpolateMissingClipValues` only steer
the value lookup *inside* a clip and are carried as data.

Times are exact rationals; stage-time lists are assumed sorted by stage time
(L6967 "sorted by stageTime"; OpenUSD sorts both `active` and `times` itself —
probe "unsorted").

Theorems: CL1 `templateTimes_spec`, `templateTimes_maximal`,
`example_offset_spec` (L6868-6878 by computation), `example_stride_spec`
(L6858-6866), `example_quad_ne_spec` (LEAN-761); CL2 `activePos_mono`,
`activeIndex_leftClosed`, `activeIndex_before`, `activeIndex_after`;
CL3 `clipTime_jump_left`, `clipTime_jump_at`; CL4 `clipTime_clamp_before`,
`clipTime_clamp_after`; CL5 `clipTime_affine`, `clipTime_continuous_at`.

Spec issues (rows in ISSUES.md):
* SPEC-ISSUE: LEAN-761 — template example resolves start 0..2 to
  `quad_001…003`; the rules give `000…002` (`example_quad_ne_spec`).
* SPEC-ISSUE: LEAN-762 — template example `string templateAssetPath =
  [@./quad_###.usda@]` is ill-typed and its `quad_###` group is not a
  dot-separated token (OpenUSD rejects it); L7247-7275 `[@./clip1.usd@. clip_2.usd]`.
* SPEC-ISSUE: LEAN-763 — key names `interpolateMissingClipsValue` (L6662) and
  `manifestPath` (L6657) vs `interpolateMissingClipValues` (L7213) and
  `manifestAssetPath` (examples, OpenUSD).
* SPEC-ISSUE: LEAN-764 — `templateActiveOffset` knots: placement for negative
  offsets and for an end time off the stride grid only follows from OpenUSD.
* SPEC-ISSUE: LEAN-765 — "start time … truncated": only the file name is.
* SPEC-ISSUE: LEAN-766 — unresolvable template paths are skipped by OpenUSD.
* SPEC-ISSUE: LEAN-767 — `active` sortedness / duplicate stage times.
* SPEC-ISSUE: LEAN-768 — no `times` authored → identity mapping (unstated).
* SPEC-ISSUE: LEAN-769 — in clamped regions OpenUSD interpolates values across
  clip boundaries instead of querying the active clip at the clamped time.
-/

namespace UsdSpec.Clips

/-- Explicit clip-set metadata. SPEC: L6648-6690. -/
structure ClipSet where
  assetPaths : List String
  /-- `(stageTime, assetIndex)` pairs, L6684-6687. -/
  active : List (ℚ × ℕ)
  /-- `(stageTime, clipTime)` pairs, L6688-6691. -/
  times : List (ℚ × ℚ)
  primPath : String := ""
  manifestAssetPath : String := ""
  interpolateMissingClipValues : Bool := false
  deriving DecidableEq, Repr

/-! ## Active clip (SPEC: L6941-6958) -/

/-- "A `[stageTime, assetIndex]` entry indicates that the clip … is active from
time stageTime up to the stageTime of the next entry … The first clip … is also
considered active for all *earlier* times, and the last clip … for all *later*
times." Intervals are left-closed (the example: quad_2 "active from time 1"). -/
def activeIndex : List (ℚ × ℕ) → ℚ → Option ℕ
  | [], _ => none
  | [e], _ => some e.2
  | e :: f :: rest, t => if f.1 ≤ t then activeIndex (f :: rest) t else some e.2

/-- Position of the selected entry in `active`. -/
def activePos : List (ℚ × ℕ) → ℚ → ℕ
  | [], _ => 0
  | [_], _ => 0
  | _ :: f :: rest, t => if f.1 ≤ t then activePos (f :: rest) t + 1 else 0

def SortedS {β} (l : List (ℚ × β)) : Prop := l.Pairwise fun a b => a.1 ≤ b.1

/-- CL2: the selected entry moves forward monotonically with stage time (no
sortedness needed for the position; the index column itself is monotone when
authored non-decreasing). -/
theorem activePos_mono : ∀ (l : List (ℚ × ℕ)) {t t' : ℚ}, t ≤ t' → activePos l t ≤ activePos l t'
  | [], _, _, _ => le_refl _
  | [_], _, _, _ => le_refl _
  | e :: f :: rest, t, t', h => by
    simp only [activePos]
    by_cases h1 : f.1 ≤ t
    · have h2 : f.1 ≤ t' := le_trans h1 h
      simp only [h1, h2, ite_true]
      exact Nat.succ_le_succ (activePos_mono (f :: rest) h)
    · simp only [h1, ite_false]; exact Nat.zero_le _

/-- CL2 left-closed intervals: on `[e.stage, f.stage)` entry `e` is active. -/
theorem activeIndex_leftClosed (e f : ℚ × ℕ) (rest : List (ℚ × ℕ)) (t : ℚ) (h : t < f.1) :
    activeIndex (e :: f :: rest) t = some e.2 := by
  simp [activeIndex, not_le.2 h]

theorem activeIndex_at_next (e f : ℚ × ℕ) (rest : List (ℚ × ℕ)) (t : ℚ) (h : f.1 ≤ t) :
    activeIndex (e :: f :: rest) t = activeIndex (f :: rest) t := by
  simp [activeIndex, h]

/-- CL2: earlier than every entry → the first clip. -/
theorem activeIndex_before (e : ℚ × ℕ) (rest : List (ℚ × ℕ)) (hs : SortedS (e :: rest)) (t : ℚ)
    (h : t < e.1) : activeIndex (e :: rest) t = some e.2 := by
  cases rest with
  | nil => rfl
  | cons f rest =>
    have : e.1 ≤ f.1 := (List.pairwise_cons.1 hs).1 f (by simp)
    exact activeIndex_leftClosed e f rest t (lt_of_lt_of_le h this)

/-- CL2: at or after the last entry → the last clip. -/
theorem activeIndex_after : ∀ (l : List (ℚ × ℕ)) (e : ℚ × ℕ) (_ : SortedS (l ++ [e])) (t : ℚ),
    e.1 ≤ t → activeIndex (l ++ [e]) t = some e.2
  | [], e, _, t, _ => rfl
  | [a], e, hs, t, h => by simp [activeIndex, h]
  | a :: b :: rest, e, hs, t, h => by
    have hb : b.1 ≤ e.1 := by
      have := (List.pairwise_cons.1 (List.pairwise_cons.1 hs).2).1
      exact this e (by simp)
    show activeIndex (a :: b :: (rest ++ [e])) t = some e.2
    rw [activeIndex_at_next a b (rest ++ [e]) t (le_trans hb h)]
    exact activeIndex_after (b :: rest) e (List.pairwise_cons.1 hs).2 t h

/-! ## Timing curve (SPEC: L6962-6994, L7222-7236) -/

def lerp (s0 c0 s1 c1 t : ℚ) : ℚ := c0 + (t - s0) / (s1 - s0) * (c1 - c0)

/-- Walk the timing curve from a segment start `(s0, c0)` with `t ≥ s0`.
A segment `(s0,c0)→(s1,c1)` covers `[s0, s1)`; at a duplicate stage time the
next segment starts, so the right-most entry owns the jump time. -/
def walk : List (ℚ × ℚ) → ℚ → ℚ
  | [], t => t
  | [(_, c)], _ => c
  | (s0, c0) :: (s1, c1) :: rest, t =>
    if t < s1 then lerp s0 c0 s1 c1 t else walk ((s1, c1) :: rest) t

/-- Stage time → clip time. Before the first entry the first clip time,
after the last the last clip time (L6983-6990, v1.1 clamp); no `times` →
identity (SPEC-ISSUE: LEAN-768: the spec does not say; OpenUSD and LightUSD use
identity). -/
def clipTime (l : List (ℚ × ℚ)) (t : ℚ) : ℚ :=
  match l with
  | [] => t
  | (s0, c0) :: _ => if t < s0 then c0 else walk l t

/-- CL4 before the first entry. CLAIM L6983-6985: "If the queried stage time is
before the first entry …, the clip time is clamped to the clip time of the
first entry." -/
theorem clipTime_clamp_before (s0 c0 : ℚ) (rest : List (ℚ × ℚ)) (t : ℚ) (h : t < s0) :
    clipTime ((s0, c0) :: rest) t = c0 := by
  simp [clipTime, h]

theorem walk_after : ∀ (l : List (ℚ × ℚ)) (e : ℚ × ℚ) (_ : SortedS (l ++ [e])) (t : ℚ),
    e.1 ≤ t → walk (l ++ [e]) t = e.2
  | [], e, _, t, _ => rfl
  | [a], e, hs, t, h => by
    have : ¬ t < e.1 := not_lt.2 h
    simp [walk, this]
  | a :: b :: rest, e, hs, t, h => by
    have hb : b.1 ≤ e.1 := (List.pairwise_cons.1 (List.pairwise_cons.1 hs).2).1 e (by simp)
    have : ¬ t < b.1 := not_lt.2 (le_trans hb h)
    show walk (a :: b :: (rest ++ [e])) t = e.2
    simp only [walk, this, ite_false]
    exact walk_after (b :: rest) e (List.pairwise_cons.1 hs).2 t h

/-- CL4 after the last entry. CLAIM L6985-6987. -/
theorem clipTime_clamp_after (l : List (ℚ × ℚ)) (e : ℚ × ℚ) (hs : SortedS (l ++ [e])) (t : ℚ)
    (h : e.1 ≤ t) : clipTime (l ++ [e]) t = e.2 := by
  cases l with
  | nil =>
    simp only [List.nil_append, clipTime]
    simp [not_lt.2 h, walk]
  | cons a rest =>
    have ha : a.1 ≤ e.1 := (List.pairwise_cons.1 hs).1 e (by simp)
    have : ¬ t < a.1 := not_lt.2 (le_trans ha h)
    show (match (a :: (rest ++ [e])) with | [] => t | (s0, c0) :: _ => if t < s0 then c0 else walk _ t) = e.2
    simp only [this, ite_false]
    exact walk_after (a :: rest) e hs t h

/-- CL5 affine within a segment: "times between these entries will be
linearly interpolated" (L6973-6978). -/
theorem clipTime_affine (s0 c0 s1 c1 : ℚ) (rest : List (ℚ × ℚ)) (t : ℚ) (h0 : s0 ≤ t) (h1 : t < s1) :
    clipTime ((s0, c0) :: (s1, c1) :: rest) t = c0 + (t - s0) / (s1 - s0) * (c1 - c0) := by
  simp [clipTime, not_lt.2 h0, walk, h1, lerp]

/-- CL5 continuity at a non-jump entry: the left limit of the segment ending
at `(s1, c1)` equals the value at `s1`, which is `c1` when the next entry is
strictly later. -/
theorem clipTime_continuous_at (s0 c0 s1 c1 s2 c2 : ℚ) (rest : List (ℚ × ℚ)) (h01 : s0 < s1)
    (h12 : s1 < s2) :
    lerp s0 c0 s1 c1 s1 = c1 ∧ clipTime ((s0, c0) :: (s1, c1) :: (s2, c2) :: rest) s1 = c1 := by
  have hne : s1 - s0 ≠ 0 := by linarith
  refine ⟨by unfold lerp; field_simp; ring, ?_⟩
  simp [clipTime, not_lt.2 (le_of_lt h01), walk, h12, lerp]

/-- CL3 jump, left side: on `[s0, s)` the curve heads toward the left-most
entry `(s, a)` (its limit at `s` is `a`). CLAIM L7224-7226: "The clip time in
the left-most entry is used for time mappings up to the specified stage time". -/
theorem clipTime_jump_left (s0 c0 s a b s2 c2 : ℚ) (rest : List (ℚ × ℚ)) (t : ℚ)
    (h0 : s0 ≤ t) (h1 : t < s) (hs : s0 < s) :
    clipTime ((s0, c0) :: (s, a) :: (s, b) :: (s2, c2) :: rest) t = lerp s0 c0 s a t ∧
      lerp s0 c0 s a s = a := by
  have hne : s - s0 ≠ 0 := by linarith
  refine ⟨by simp [clipTime, not_lt.2 h0, walk, h1], by unfold lerp; field_simp; ring⟩

/-- CL3 jump, at and after: "the clip time in the right-most entry is used for
time mappings at that stage time and afterward" (L7226-7227). -/
theorem clipTime_jump_at (s0 c0 s a b s2 c2 : ℚ) (rest : List (ℚ × ℚ)) (t : ℚ)
    (hs : s0 ≤ s) (h0 : s ≤ t) (h1 : t < s2) :
    clipTime ((s0, c0) :: (s, a) :: (s, b) :: (s2, c2) :: rest) t = lerp s b s2 c2 t ∧
      lerp s b s2 c2 s = b := by
  have : ¬ t < s0 := not_lt.2 (le_trans hs h0)
  have h' : ¬ t < s := not_lt.2 h0
  refine ⟨by simp [clipTime, this, walk, h', h1], by simp [lerp]⟩

/-- The L7229-7241 example (L7235-7236): `[(0,0),(10,10),(10,25),(20,35)]`. -/
theorem example_jump :
    clipTime [(0, 0), (10, 10), (10, 25), (20, 35)] 5 = 5 ∧
    clipTime [(0, 0), (10, 10), (10, 25), (20, 35)] 10 = 25 ∧
    clipTime [(0, 0), (10, 10), (10, 25), (20, 35)] 15 = 30 ∧
    clipTime [(0, 0), (10, 10), (10, 25), (20, 35)] 20 = 35 ∧
    clipTime [(0, 0), (10, 10), (10, 25), (20, 35)] 25 = 35 := by
  norm_num [clipTime, walk, lerp]

/-- The L6969-6990 example `[(0,1),(1,2),(2,3)]`: 1.5 → 2.5, −1 → 1, 5 → 3. -/
theorem example_times :
    clipTime [(0, 1), (1, 2), (2, 3)] (3/2) = 5/2 ∧ clipTime [(0, 1), (1, 2), (2, 3)] (-1) = 1 ∧
    clipTime [(0, 1), (1, 2), (2, 3)] 5 = 3 := by
  norm_num [clipTime, walk, lerp]

/-! ## Templates (SPEC: L6823-6915) -/

/-- `templateAssetPath` split around its `#` groups: `prefix`, integer width,
optional fractional width (two groups "must be adjacent, separated by a dot",
L6834-6838), `suffix`. -/
structure Template where
  pre : String
  intWidth : ℕ
  fracWidth : Option ℕ
  post : String
  start : ℚ
  stop : ℚ
  stride : ℚ
  activeOffset : Option ℚ := none
  deriving DecidableEq, Repr

/-- Number of strides after the start: `⌊(end − start)/stride⌋`. -/
def Template.count (tp : Template) : ℕ := (⌊(tp.stop - tp.start) / tp.stride⌋).toNat

/-- Derived stage times `start, start+stride, … ≤ end` (end inclusive, L6858-6866:
12, 18, 24 for end 25). The start is *not* truncated: only the file name is
(SPEC-ISSUE: LEAN-765). -/
def Template.times (tp : Template) : List ℚ :=
  (List.range (tp.count + 1)).map fun (i : ℕ) => tp.start + (i : ℚ) * tp.stride

def padNat (w n : ℕ) : String :=
  let s := toString n
  String.ofList (List.replicate (w - s.length) '0') ++ s

/-- File name for a non-negative stage time: truncated integer part padded to
`intWidth`; fractional part to `fracWidth` digits (truncated; the vectors only
use exactly representable fractions). L6839-6857. -/
def Template.path (tp : Template) (t : ℚ) : String :=
  let ip := (⌊t⌋).toNat
  match tp.fracWidth with
  | none => tp.pre ++ padNat tp.intWidth ip ++ tp.post
  | some w =>
    let fp := (⌊(t - ⌊t⌋) * 10 ^ w⌋).toNat
    tp.pre ++ padNat tp.intWidth ip ++ "." ++ padNat w fp ++ tp.post

/-- Template → explicit metadata. For each derived time `t` with index `n`:
`times (t, t)` and `active (t + offset, n)` (L6907-6915); with an offset, two
extra `times` knots at `start − |offset|` and `end + |offset|` (L6868-6885).
The `end + |offset|` knot uses `end`, not the last derived time (OpenUSD
clipSetDefinition.cpp:286-293). Every file is assumed to resolve (OpenUSD
skips unresolvable ones, SPEC-ISSUE: LEAN-766). -/
def Template.expand (tp : Template) : ClipSet :=
  let ts := tp.times
  let o := tp.activeOffset.getD 0
  let core := ts.map fun t => (t, t)
  let times := match tp.activeOffset with
    | some o => [(tp.start - |o|, tp.start - |o|)] ++ core ++ [(tp.stop + |o|, tp.stop + |o|)]
    | none => core
  { assetPaths := ts.map tp.path,
    active := (List.range ts.length).zip ts |>.map fun (n, t) => (t + o, n),
    times := times }

/-- The offset must not exceed the stride in magnitude (L6884-6885). -/
def Template.valid (tp : Template) : Prop :=
  0 < tp.stride ∧ tp.start ≤ tp.stop ∧ ∀ o, tp.activeOffset = some o → |o| ≤ tp.stride

/-- CL1: every derived time is `start + i·stride` and lies in `[start, end]`. -/
theorem templateTimes_spec (tp : Template) (hv : tp.valid) (t : ℚ) (ht : t ∈ tp.times) :
    ∃ i : ℕ, t = tp.start + i * tp.stride ∧ tp.start ≤ t ∧ t ≤ tp.stop := by
  obtain ⟨hs, hle, -⟩ := hv
  unfold Template.times at ht
  obtain ⟨i, hi, rfl⟩ := List.mem_map.1 ht
  have hi := List.mem_range.1 hi
  refine ⟨i, rfl, by have : (0:ℚ) ≤ i * tp.stride := by positivity
                     linarith, ?_⟩
  have hc : (i : ℤ) ≤ ⌊(tp.stop - tp.start) / tp.stride⌋ := by
    have : i ≤ tp.count := Nat.lt_succ_iff.1 hi
    unfold Template.count at this
    have h0 : 0 ≤ ⌊(tp.stop - tp.start) / tp.stride⌋ :=
      Int.floor_nonneg.2 (div_nonneg (by linarith) (le_of_lt hs))
    omega
  have := (Int.le_floor.1 hc)
  rw [le_div_iff₀ hs] at this
  push_cast at this
  linarith

/-- CL1 maximality: the next stride would pass `end`. -/
theorem templateTimes_maximal (tp : Template) (hv : tp.valid) :
    tp.stop < tp.start + ((tp.count + 1 : ℕ) : ℚ) * tp.stride := by
  obtain ⟨hs, hle, -⟩ := hv
  have h0 : 0 ≤ ⌊(tp.stop - tp.start) / tp.stride⌋ :=
    Int.floor_nonneg.2 (div_nonneg (by linarith) (le_of_lt hs))
  have hlt := Int.lt_floor_add_one ((tp.stop - tp.start) / tp.stride)
  have hc : ((tp.count : ℕ) : ℚ) = ((⌊(tp.stop - tp.start) / tp.stride⌋ : ℤ) : ℚ) := by
    unfold Template.count; exact_mod_cast Int.toNat_of_nonneg h0
  rw [div_lt_iff₀ hs] at hlt
  push_cast; rw [hc]; nlinarith

theorem templateTimes_length (tp : Template) : tp.times.length = tp.count + 1 := by
  simp [Template.times]

/-- L6868-6878 example: start 101, end 103, stride 1, offset 0.5 gives exactly
the listed `times` and `active` (OpenUSD agrees, probe "tpl-offset"). -/
theorem example_offset_spec :
    let tp : Template := ⟨"clip.", 3, none, ".usd", 101, 103, 1, some (1/2)⟩
    tp.expand.times = [(201/2, 201/2), (101, 101), (102, 102), (103, 103), (207/2, 207/2)] ∧
    tp.expand.active = [(203/2, 0), (205/2, 1), (207/2, 2)] := by
  decide +kernel

/-- L6858-6866 example: start 12, end 25, `path/clipname.#.usd`, stride 6. -/
theorem example_stride_spec :
    (({ pre := "path/clipname.", intWidth := 1, fracWidth := none, post := ".usd",
        start := 12, stop := 25, stride := 6 } : Template).expand.assetPaths) =
      ["path/clipname.12.usd", "path/clipname.18.usd", "path/clipname.24.usd"] := by
  decide +kernel

/-- L6843-6847 example: `path/clipname.###.usd`, start 12 → `path/clipname.012.usd`. -/
theorem example_padding_spec :
    (({ pre := "path/clipname.", intWidth := 3, fracWidth := none, post := ".usd",
        start := 12, stop := 12, stride := 1 } : Template).expand.assetPaths) =
      ["path/clipname.012.usd"] := by
  decide +kernel

/-- SPEC-ISSUE: LEAN-761: the L6889-6909 example (start 0, end 2, stride 1)
says the files resolve to `quad_001…quad_003`; the template rules give
`quad_000…quad_002` (OpenUSD: q.000/q.001/q.002, probe "tpl-dot"). -/
theorem example_quad_ne_spec :
    let tp : Template := ⟨"./quad_", 3, none, ".usda", 0, 2, 1, none⟩
    tp.expand.assetPaths = ["./quad_000.usda", "./quad_001.usda", "./quad_002.usda"] ∧
    tp.expand.assetPaths ≠ ["./quad_001.usda", "./quad_002.usda", "./quad_003.usda"] := by
  decide +kernel

/-- Template-derived active entries are sorted and `times` is `(t, t)`, so the
timing curve is the identity on `[start, end]` (CL1 + CL5). -/
theorem template_identity (tp : Template) (hv : tp.valid) (t : ℚ) (ht : t ∈ tp.times) :
    (t, t) ∈ tp.expand.times := by
  simp only [Template.expand]
  cases tp.activeOffset <;> simp [ht]

end UsdSpec.Clips
