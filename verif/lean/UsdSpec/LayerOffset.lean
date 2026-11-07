import Mathlib.Data.Rat.Defs
import Mathlib.Tactic.Ring
import Mathlib.Tactic.FieldSimp
import Mathlib.Tactic.NormNum
import Mathlib.Tactic.Positivity
import Mathlib.Tactic.Linarith
/-!
# Layer offsets and time codes per second (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L4271-4298 (§10.3.1 Sublayer Time Offsets), L4449-4458
(Reference Time Offsets), L5428-5510 (§11 Time Codes Per Second), L6505-6537
(§12 Layer Offset and Scale in value resolution).

Time ordinates are modelled in `ℚ` (Mathlib `Rat`): every identity below is an
identity of the real-number algebra `offset + scale * t` and is proved by `ring` /
`field_simp`, so it transfers to any field; the implementation's `double` only
adds rounding.

Spec issues found while formalizing are tagged `SPEC-ISSUE: LEAN-NNN`:

* `LEAN-111` — direction of the layer offset in the §12 example (L6523-6525)
  versus §11 (L5436-5440): see `section Lean111`.
* `LEAN-112` — the "stack-level description" note (L5461-5467) claims the
  recursive and closed forms "produce the same effective offsets"; this holds
  for the `scale` component (`cumulative_scale_closed_form`) but not for the
  `offset` component (`stackLevel_offset_ne_recursive`).
* `LEAN-113` — §10.3.1 L4285-4288 says the concatenation adds "the `offset` of
  the *child*", §11 L5483-5487 says "the parent's `offset` is added"; only the
  latter is the composition of the affine time maps (`composeLiteral_ne`).
-/

namespace UsdSpec

/-- A layer offset: `offset` and `scale`. "By default, the offset is 0 and the
scale is 1.0." SPEC: L4273-4276. -/
@[ext]
structure LayerOffset where
  offset : ℚ
  scale : ℚ
  deriving DecidableEq, Repr

namespace LayerOffset

/-- The default layer offset. SPEC: L4275-4276. -/
def identity : LayerOffset := ⟨0, 1⟩

/-- Concatenation `parent ∘ child`. SPEC: L5483-5487: "the parent's `scale` is
applied to the child's `offset` and `scale`, and then the parent's `offset` is
added" (and L4282-4288, see SPEC-ISSUE LEAN-113). This is the composition of
the affine time maps (`apply_compose`) and what LightUSD does
(`src/next/pcp/namespace-mapping.hh:110-112`). -/
def compose (parent child : LayerOffset) : LayerOffset :=
  ⟨parent.offset + parent.scale * child.offset, parent.scale * child.scale⟩

/-- The *literal* text of L4285-4288: "applying the `scale` of the parent layer
to the `offset` and `scale` of the child and then adding the `offset` of the
**child** to the result". Kept only to state SPEC-ISSUE LEAN-113. -/
def composeLiteral (parent child : LayerOffset) : LayerOffset :=
  ⟨parent.scale * child.offset + child.offset, parent.scale * child.scale⟩

/-- Apply a layer offset to a time ordinate: `offset + scale * t`.
SPEC: L5436-5440: the effective offset "converts [a layer's] time ordinates
into the root layer stack's time scale", i.e. `apply` maps layer time to
root/stage time. -/
def apply (o : LayerOffset) (t : ℚ) : ℚ := o.offset + o.scale * t

/-- The inverse offset (needs `scale ≠ 0`): maps stage time back to layer time.
SPEC: L5439-5440 "remap their queries into each layer's time space". -/
def inverse (o : LayerOffset) (_h : o.scale ≠ 0) : LayerOffset :=
  ⟨-o.offset / o.scale, 1 / o.scale⟩

/-- A valid layer offset has positive scale. SPEC: L4295-4298, L4455-4458,
L6527-6531 ("`scale` must be a positive none zero value"). -/
def valid (o : LayerOffset) : Prop := 0 < o.scale

instance : DecidablePred valid := fun o => by unfold valid; infer_instance

/-- Error handling for `scale ≤ 0`: "it is a composition error and that
sublayer offset is treated as though it had the default offset and scale
values". SPEC: L4295-4298 (sublayers), L4455-4458 (references). -/
def sanitize (o : LayerOffset) : LayerOffset := if 0 < o.scale then o else identity

/-! ## (O1) Monoid laws -/

theorem compose_assoc (a b c : LayerOffset) :
    compose (compose a b) c = compose a (compose b c) := by
  ext <;> simp only [compose] <;> ring

theorem identity_compose (o : LayerOffset) : compose identity o = o := by
  ext <;> simp [compose, identity]

theorem compose_identity (o : LayerOffset) : compose o identity = o := by
  ext <;> simp [compose, identity]

/-- SPEC-ISSUE: LEAN-113 — L4285-4288 ("adding the `offset` of the child")
contradicts L5483-5487 ("the parent's `offset` is added"). The literal
§10.3.1 reading is not the composition of the time maps and loses the parent's
offset entirely: with `parent = ⟨10, 1⟩`, `child = ⟨0, 1⟩` (an un-offset
sublayer of an offset sublayer) it yields `⟨0, 1⟩`, not `⟨10, 1⟩`. -/
theorem composeLiteral_ne :
    composeLiteral ⟨10, 1⟩ ⟨0, 1⟩ = ⟨0, 1⟩ ∧ compose ⟨10, 1⟩ ⟨0, 1⟩ = ⟨10, 1⟩ ∧
    (composeLiteral ⟨10, 1⟩ ⟨0, 1⟩).apply 0 ≠ (⟨10, 1⟩ : LayerOffset).apply ((⟨0, 1⟩ : LayerOffset).apply 0) := by
  refine ⟨?_, ?_, ?_⟩ <;> norm_num [composeLiteral, compose, apply, LayerOffset.ext_iff]

/-! ## (O2) `compose` is composition of the affine maps -/

theorem apply_compose (a b : LayerOffset) (t : ℚ) :
    (compose a b).apply t = a.apply (b.apply t) := by
  simp only [compose, apply]; ring

theorem apply_identity (t : ℚ) : identity.apply t = t := by
  simp [apply, identity]

/-! ## (O3) Validity is closed under composition -/

theorem valid_identity : valid identity := by
  simp [valid, identity]

theorem valid_compose {a b : LayerOffset} (ha : valid a) (hb : valid b) :
    valid (compose a b) := by
  unfold valid compose at *; exact mul_pos ha hb

theorem valid_sanitize (o : LayerOffset) : valid (sanitize o) := by
  unfold sanitize
  split_ifs with h
  · exact h
  · exact valid_identity

theorem sanitize_of_valid {o : LayerOffset} (h : valid o) : sanitize o = o := by
  have hs : 0 < o.scale := h
  simp [sanitize, hs]

theorem valid_scale_ne_zero {o : LayerOffset} (h : valid o) : o.scale ≠ 0 :=
  ne_of_gt h

/-! ## (O4) Inverse round trips -/

theorem apply_inverse (o : LayerOffset) (h : o.scale ≠ 0) (t : ℚ) :
    o.apply ((o.inverse h).apply t) = t := by
  simp only [apply, inverse]
  field_simp
  ring

theorem inverse_apply (o : LayerOffset) (h : o.scale ≠ 0) (t : ℚ) :
    (o.inverse h).apply (o.apply t) = t := by
  simp only [apply, inverse]
  field_simp
  ring

theorem compose_inverse (o : LayerOffset) (h : o.scale ≠ 0) :
    compose o (o.inverse h) = identity := by
  ext
  · simp only [compose, inverse, identity]; field_simp; ring
  · simp only [compose, inverse, identity]; field_simp

theorem inverse_compose (o : LayerOffset) (h : o.scale ≠ 0) :
    compose (o.inverse h) o = identity := by
  ext
  · simp only [compose, inverse, identity]; field_simp; ring
  · simp only [compose, inverse, identity]; field_simp

theorem valid_inverse {o : LayerOffset} (h : valid o) : valid (o.inverse (valid_scale_ne_zero h)) := by
  unfold valid inverse at *; simp only; positivity

end LayerOffset

/-! ## Time codes per second (§11) -/

/-- Effective timecodes-per-second of a layer: `timeCodesPerSecond` if authored,
else `framesPerSecond` if authored, else `24`. SPEC: L5442-5453. -/
def effectiveTcps (tcps fps : Option ℚ) : ℚ := tcps.getD (fps.getD 24)

theorem effectiveTcps_none_none : effectiveTcps none none = 24 := rfl
theorem effectiveTcps_some (t : ℚ) (fps : Option ℚ) : effectiveTcps (some t) fps = t := rfl
theorem effectiveTcps_none_some (f : ℚ) : effectiveTcps none (some f) = f := rfl

/-- Sublayer offset adjustment: only the `scale` is multiplied by
`parentTcps / subTcps`; the `offset` is unchanged. SPEC: L5469-5483
(`adjusted = (offset, scale * f)`). -/
def sublayerAdjust (parentTcps subTcps : ℚ) (o : LayerOffset) : LayerOffset :=
  ⟨o.offset, o.scale * (parentTcps / subTcps)⟩

/-- Reference / payload offset adjustment: only the `scale` is multiplied by
`targetTcps / sourceStackTcps`, where `targetTcps` is that of the layer holding
the arc and `sourceStackTcps` that of the referenced layer stack's root layer.
SPEC: L5495-5509. -/
def referenceAdjust (targetTcps sourceStackTcps : ℚ) (o : LayerOffset) : LayerOffset :=
  ⟨o.offset, o.scale * (targetTcps / sourceStackTcps)⟩

theorem sublayerAdjust_eq_referenceAdjust (p s : ℚ) (o : LayerOffset) :
    sublayerAdjust p s o = referenceAdjust p s o := rfl

/-- The adjustment is the same thing as post-composing a pure scale. -/
theorem sublayerAdjust_eq_compose (p s : ℚ) (o : LayerOffset) :
    sublayerAdjust p s o = LayerOffset.compose o ⟨0, p / s⟩ := by
  ext <;> simp [sublayerAdjust, LayerOffset.compose]

/-- With equal timecodes-per-second the adjustment is the identity (so skipping
it when `parent_tcps == sub_tcps`, as LightUSD does, is sound). -/
theorem sublayerAdjust_self (t : ℚ) (ht : t ≠ 0) (o : LayerOffset) :
    sublayerAdjust t t o = o := by
  ext <;> simp [sublayerAdjust, div_self ht]

/-- Positive tcps keep a valid offset valid (the "composition error" note of
L5491-5493 cannot fire when both tcps are positive). -/
theorem valid_sublayerAdjust {p s : ℚ} (hp : 0 < p) (hs : 0 < s) {o : LayerOffset}
    (ho : LayerOffset.valid o) : LayerOffset.valid (sublayerAdjust p s o) := by
  unfold LayerOffset.valid sublayerAdjust at *; simp only; positivity

/-! ## (O5) LightUSD's order matches the spec

LightUSD (`src/next/pcp/cache-layers.cc:312,352`) computes

    sub_cum = cum_offset.Compose(LayerOffset{offset, scale});          -- L312
    sub_cum = sub_cum.Compose(LayerOffset{0.0, parent_tcps / sub_tcps}); -- L352

i.e. `compose (compose cum child) ⟨0, f⟩`, while the spec (L5483-5487) says to
first adjust the child (`sublayerAdjust`) and then compose it under the
parent's effective offset: `compose cum (sublayerAdjust p s child)`.
These are equal. -/
theorem lightusd_order_eq_spec (cum child : LayerOffset) (f : ℚ) :
    LayerOffset.compose (LayerOffset.compose cum child) ⟨0, f⟩
      = LayerOffset.compose cum ⟨child.offset, child.scale * f⟩ := by
  ext <;> simp only [LayerOffset.compose] <;> ring

theorem lightusd_order_eq_sublayerAdjust (cum child : LayerOffset) (p s : ℚ) :
    LayerOffset.compose (LayerOffset.compose cum child) ⟨0, p / s⟩
      = LayerOffset.compose cum (sublayerAdjust p s child) :=
  lightusd_order_eq_spec cum child (p / s)

/-! ## (O6) Recursive versus closed-form TCPS (§11, L5455-5467)

A sublayer chain is a list of links from the root layer's direct sublayer
downward; each link carries the sublayer's effective tcps and its authored
sublayer offset (already sanitized). The root layer has effective tcps
`rootTcps` and the identity effective offset. -/

/-- One link of a sublayer chain. -/
structure ChainLink where
  tcps : ℚ
  authored : LayerOffset
  deriving DecidableEq, Repr

/-- The recursive (normative, L5466) formulation. `cumulativeFrom cum p links`:
`cum` is the parent's effective offset, `p` the parent's effective tcps
(L5488-5489: "the sublayer's effective timecodes-per-second becomes the parent
value for its children"). -/
def cumulativeFrom (cum : LayerOffset) (parentTcps : ℚ) : List ChainLink → LayerOffset
  | [] => cum
  | l :: rest =>
      cumulativeFrom (LayerOffset.compose cum (sublayerAdjust parentTcps l.tcps l.authored))
        l.tcps rest

/-- Effective offset of the leaf of a chain under a root layer with `rootTcps`. -/
def cumulative (rootTcps : ℚ) (links : List ChainLink) : LayerOffset :=
  cumulativeFrom LayerOffset.identity rootTcps links

/-- The effective tcps of the leaf layer of a chain (`rootTcps` for the empty chain). -/
def leafTcps (rootTcps : ℚ) : List ChainLink → ℚ
  | [] => rootTcps
  | l :: rest => leafTcps l.tcps rest

/-- The authored offsets composed without any tcps adjustment. -/
def authoredCumulative (links : List ChainLink) : LayerOffset :=
  links.foldl (fun cum l => LayerOffset.compose cum l.authored) LayerOffset.identity

theorem cumulativeFrom_scale (cum : LayerOffset) (p : ℚ) (hp : p ≠ 0) (links : List ChainLink)
    (h : ∀ l ∈ links, l.tcps ≠ 0) :
    (cumulativeFrom cum p links).scale
      = cum.scale * (links.map (fun l => l.authored.scale)).prod * (p / leafTcps p links) := by
  induction links generalizing cum p with
  | nil => simp [cumulativeFrom, leafTcps, div_self hp]
  | cons l rest ih =>
    have hl : l.tcps ≠ 0 := h l List.mem_cons_self
    rw [cumulativeFrom, leafTcps, List.map_cons, List.prod_cons,
      ih _ _ hl (fun l' hl' => h l' (List.mem_cons_of_mem _ hl'))]
    simp only [LayerOffset.compose, sublayerAdjust]
    have key : p / l.tcps * (l.tcps / leafTcps l.tcps rest) = p / leafTcps l.tcps rest := by
      rw [div_mul_div_comm, mul_comm p, mul_div_mul_left _ _ hl]
    rw [← key]; ring

/-- (O6) The scale of the recursive formulation telescopes to the closed form of
the note at L5461-5465: the product of the authored scales times
`rootTcps / leafTcps`. -/
theorem cumulative_scale_closed_form (rootTcps : ℚ) (hr : rootTcps ≠ 0) (links : List ChainLink)
    (h : ∀ l ∈ links, l.tcps ≠ 0) :
    (cumulative rootTcps links).scale
      = (links.map (fun l => l.authored.scale)).prod * (rootTcps / leafTcps rootTcps links) := by
  unfold cumulative
  rw [cumulativeFrom_scale _ _ hr _ h]
  simp [LayerOffset.identity]

theorem authoredCumulative_scale (links : List ChainLink) :
    (authoredCumulative links).scale = (links.map (fun l => l.authored.scale)).prod := by
  unfold authoredCumulative
  suffices H : ∀ cum : LayerOffset,
      (links.foldl (fun cum l => LayerOffset.compose cum l.authored) cum).scale
        = cum.scale * (links.map (fun l => l.authored.scale)).prod by
    rw [H]; simp [LayerOffset.identity]
  induction links with
  | nil => intro cum; simp
  | cons l rest ih =>
    intro cum
    rw [List.foldl_cons, ih, List.map_cons, List.prod_cons]
    simp only [LayerOffset.compose]; ring

/-- The stack-level closed form of the note (L5461-5465): the authored offsets
composed, then the stack-level factor `rootTcps / leafTcps` applied as a pure
scale (either side). Its scale agrees with the recursive form. -/
theorem stackLevel_scale_eq_recursive (rootTcps : ℚ) (hr : rootTcps ≠ 0) (links : List ChainLink)
    (h : ∀ l ∈ links, l.tcps ≠ 0) :
    (LayerOffset.compose (authoredCumulative links) ⟨0, rootTcps / leafTcps rootTcps links⟩).scale
      = (cumulative rootTcps links).scale := by
  rw [cumulative_scale_closed_form _ hr _ h]
  simp [LayerOffset.compose, authoredCumulative_scale]

/-- SPEC-ISSUE: LEAN-112 — L5461-5465 claims the per-sublayer adjustments
"produce the same effective offsets as computing the layer stack's
timecodes-per-second divided by a layer's timecodes-per-second for each layer
and composing the result with the layer's accumulated offset". For the
`offset` component this is false as soon as a sublayer at a different tcps has
its own sublayer with a non-zero offset. Root at 24 tcps; sublayer `A` at 48
tcps with authored `(offset 10, scale 1)`; `A`'s sublayer `B` at 48 tcps with
authored `(offset 5, scale 1)`:

* recursive (normative, L5466): after `A`: `⟨10, 1/2⟩`; after `B`:
  `compose ⟨10, 1/2⟩ ⟨5, 1⟩ = ⟨12.5, 1/2⟩`;
* stack-level: authored composition `⟨15, 1⟩`, factor `24/48 = 1/2`:
  `compose ⟨15, 1⟩ ⟨0, 1/2⟩ = ⟨15, 1/2⟩` or `compose ⟨0, 1/2⟩ ⟨15, 1⟩ = ⟨7.5, 1/2⟩`.

The scales agree (`stackLevel_scale_eq_recursive`); the offsets do not. -/
theorem stackLevel_offset_ne_recursive :
    let links : List ChainLink := [⟨48, ⟨10, 1⟩⟩, ⟨48, ⟨5, 1⟩⟩]
    cumulative 24 links = ⟨25 / 2, 1 / 2⟩
    ∧ LayerOffset.compose (authoredCumulative links) ⟨0, 24 / leafTcps 24 links⟩ = ⟨15, 1 / 2⟩
    ∧ LayerOffset.compose ⟨0, 24 / leafTcps 24 links⟩ (authoredCumulative links) = ⟨15 / 2, 1 / 2⟩
    ∧ (cumulative 24 links).offset
        ≠ (LayerOffset.compose (authoredCumulative links) ⟨0, 24 / leafTcps 24 links⟩).offset
    ∧ (cumulative 24 links).offset
        ≠ (LayerOffset.compose ⟨0, 24 / leafTcps 24 links⟩ (authoredCumulative links)).offset := by
  refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
    simp only [cumulative, cumulativeFrom, leafTcps, authoredCumulative, List.foldl_cons,
      List.foldl_nil, LayerOffset.compose, LayerOffset.identity, sublayerAdjust] <;>
    norm_num [LayerOffset.ext_iff]

/-! ## (O7) The §12 worked example — SPEC-ISSUE: LEAN-111

SPEC: L6514-6525. A sublayer is included with `(offset = 10; scale = 0.5)` and
the spec says: "if the current `timeCode` is 12 the time sample will be
calculated `((12 * 0.5) + 10) = 16`, 16 being the new `timeCode` for sampling
the animation in the sublayer" (L6523-6525).

Two readings, both formalized below; this file does not decide between them.

* Reading A (the §12 arithmetic): the stage/query time `12` is pushed *forward*
  through the sublayer's offset, `apply ⟨10, 1/2⟩ 12 = 16`, and `16` is the
  time at which the sublayer is sampled. Under this reading `apply` maps
  stage time to layer time.
* Reading B (§11, L5436-5440): the effective offset "converts [a layer's] time
  ordinates into the root layer stack's time scale" and clients "use the
  effective layer offset to remap their queries into each layer's time space",
  i.e. `apply` maps layer time to stage time and a query at stage time `12`
  samples the sublayer at the *inverse* image `(12 - 10) / 0.5 = 4`. Under this
  reading the number `16` of the example is the *stage* time at which the
  sublayer's sample authored at layer time `12` is seen (`apply ⟨10, 1/2⟩ 12 = 16`),
  not a sublayer sampling time.

SPEC-ISSUE: LEAN-111 — direction of layer offset in the §12 example: L6523-6525
computes the sublayer sampling time as the forward image `offset + scale * t`
of the stage time (`16`), whereas L5436-5440 makes the effective offset the
layer→root map, under which the sublayer sampling time for stage time 12 is the
inverse image `4` and `16` is the *stage* time of the sublayer's sample at 12.
The two readings are mutually inconsistent for every offset with
`(offset, scale) ≠ (0, 1)`; only one can be normative. -/
section Lean111

/-- Reading A: the example's arithmetic, `(12 * 0.5) + 10 = 16`. -/
example : (LayerOffset.mk 10 (1 / 2)).apply 12 = 16 := by
  norm_num [LayerOffset.apply]

/-- Reading B: querying stage time 12 through the inverse map gives layer time 4. -/
example : ((LayerOffset.mk 10 (1 / 2)).inverse (by norm_num)).apply 12 = 4 := by
  norm_num [LayerOffset.apply, LayerOffset.inverse]

/-- Reading B, the other way round: the sublayer's sample at layer time 12 is
seen at stage time 16 — the example's number, with the roles of the two times
exchanged. -/
example : (LayerOffset.mk 10 (1 / 2)).apply 12 = 16 ∧
    ((LayerOffset.mk 10 (1 / 2)).inverse (by norm_num)).apply 16 = 12 := by
  norm_num [LayerOffset.apply, LayerOffset.inverse]

/-- For a valid offset the two readings agree at every time iff the offset is
the identity: the example's direction question cannot be dodged for any
non-trivial offset. (Without validity the time reversal `scale = -1` is an
involution and also agrees.) -/
theorem readings_agree_iff (o : LayerOffset) (hv : o.valid) :
    (∀ t, o.apply t = (o.inverse (LayerOffset.valid_scale_ne_zero hv)).apply t)
      ↔ o = LayerOffset.identity := by
  have h : o.scale ≠ 0 := LayerOffset.valid_scale_ne_zero hv
  constructor
  · intro H
    have h0 := H 0
    have h1 := H 1
    simp only [LayerOffset.apply, LayerOffset.inverse, mul_zero, add_zero, mul_one] at h0 h1
    have e : o.scale = 1 / o.scale := by linarith
    have hsq : o.scale * o.scale = 1 := by
      field_simp at e
      linarith
    rcases mul_self_eq_one_iff.mp hsq with hs | hs
    · rw [hs] at h0
      have ho : o.offset = 0 := by
        simp only [div_one] at h0
        linarith
      ext <;> simp [LayerOffset.identity, ho, hs]
    · exfalso
      have : (0 : ℚ) < o.scale := hv
      rw [hs] at this
      norm_num at this
  · rintro rfl t
    simp [LayerOffset.apply, LayerOffset.inverse, LayerOffset.identity]

end Lean111

/-! ## Reference offsets compose with the layer-stack offsets (L4288-4291) -/

/-- "composition arcs like references may apply an additional time offset that
concatenates with the layer offsets in the layer stack" (L4288-4291): applying
a reference offset on top of a layer stack's effective offset is one more
`compose`, and the result is again an affine map. -/
theorem apply_reference_over_stack (ref stack : LayerOffset) (t : ℚ) :
    (LayerOffset.compose ref stack).apply t = ref.apply (stack.apply t) :=
  LayerOffset.apply_compose ref stack t

end UsdSpec
