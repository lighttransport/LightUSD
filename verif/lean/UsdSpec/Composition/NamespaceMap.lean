import Mathlib.Data.List.Basic
import Mathlib.Data.List.Infix

/-!
# Namespace mappings (AOUSD USD Core Specification v1.1.0, §10.5)

SPEC: core_spec.md L5375-5419 (Namespace Mappings), with the per-arc mapping
paragraphs: references L4413-4448, inherits L4655-4674, variants L4813-4817,
relocates L4919-4975.

A path is modelled as the list of prim-name components of an absolute prim
path (`[]` is the pseudo-root `/`). `UsdSpec/Path/Element.lean` exists but its
paths carry property/variant elements and the XID tables; a mapping only ever
replaces prim-path prefixes (L5390-5395), so the minimal model suffices.

A mapping is a list of `(source, target)` prefix pairs (L5389-5395). `mapPath`
is the literal rule: replace the *longest* matching source prefix (the "most
specific" pair, as pxr `_GetBestSourceMatch`); no match = no mapping
(L5397-5400).

Findings:
* SPEC-ISSUE: LEAN-511 — the literal rule is not a bijection (L5384), not even
  injective, on the spec's own internal-reference example
  (`mapPath_internalRef_not_injective`). pxr (`PcpMapFunction::_Map`,
  pxr/usd/pcp/mapFunction.cpp:777-809) adds a round-trip check; `mapChecked`
  models it and *is* a partial bijection (`mapChecked_inverse`). Without the
  check, injectivity holds when targets are prefix-free (`mapPath_inverse`),
  and fails without that hypothesis (`mapPath_inverse_fails`).
* SPEC-ISSUE: LEAN-512 — the reference and inherit examples write the pair as
  `(target, source)` (`[(/A, /B)]` for `/A` referencing `/B`, L4426, L4446,
  L4672) although §10.5 defines pairs as `(source, target)` and says `/B` is
  the source (L5380-5382); the relocates example (L4950-4951) uses
  `(source, target)`.
* SPEC-ISSUE: LEAN-513 — the relocates example result `[(/Ref,/A),(/Ref/B,/A/C)]`
  (L4949-4951) is the composition only if the "additional mapping"
  `[(/A/B,/A/C)]` implicitly contains the identity; as defined (L4924-4929) it
  does not (`compose_relocates_example`, `compose_relocates_example_noIdentity`).
-/

namespace UsdSpec.NamespaceMap

/-- Absolute prim path as name components; `[]` is `/`. -/
abbrev PrimPath := List String

/-- `(source, target)` prefix pairs. SPEC: L5389-5395. -/
abbrev NsMap := List (PrimPath × PrimPath)

/-- Most specific pair whose source prefixes `p`; on equal length the earlier
pair wins. -/
def best : NsMap → PrimPath → Option (PrimPath × PrimPath)
  | [], _ => none
  | a :: rest, p =>
    if a.1 <+: p then
      match best rest p with
      | some b => if b.1.length ≤ a.1.length then some a else some b
      | none => some a
    else best rest p

/-- Literal mapping rule. SPEC: core_spec.md L5389-5400 (Namespace Mappings). -/
def mapPath (m : NsMap) (p : PrimPath) : Option PrimPath :=
  (best m p).map fun a => a.2 ++ p.drop a.1.length

/-- Inversion: swap every pair. SPEC: L5384-5385. -/
def inverse (m : NsMap) : NsMap := m.map Prod.swap

/-- pxr `PcpMapFunction::_Map`: the literal rule plus the round-trip check that
the result maps back to the input (mapFunction.cpp:777-809). -/
def mapChecked (m : NsMap) (p : PrimPath) : Option PrimPath :=
  match mapPath m p with
  | some q => if mapPath (inverse m) q = some p then some q else none
  | none => none

/-- Composition `compose f g` = apply `g` then `f` (SPEC: L5385-5387): each
`g` pair with its target mapped by `f`, plus every `f` pair strictly refining
a `g` target, pulled back to `g`'s source (pxr `PcpMapFunction::Compose`). -/
def compose (f g : NsMap) : NsMap :=
  g.filterMap (fun a => (mapPath f a.2).map (a.1, ·)) ++
  g.flatMap (fun a => f.filterMap (fun b =>
    if a.2 <+: b.1 ∧ a.2 ≠ b.1 then some (a.1 ++ b.1.drop a.2.length, b.2) else none))

/-! ## Basic lemmas -/

theorem inverse_inverse (m : NsMap) : inverse (inverse m) = m := by
  simp [inverse, List.map_map]

theorem best_spec {m : NsMap} {p : PrimPath} {c} (h : best m p = some c) :
    c ∈ m ∧ c.1 <+: p := by
  induction m with
  | nil => simp [best] at h
  | cons a rest ih =>
    simp only [best] at h
    split_ifs at h with ha
    · split at h
      · rename_i b hb
        split_ifs at h <;> simp_all
      · simp_all
    · exact ⟨List.mem_cons_of_mem _ (ih h).1, (ih h).2⟩

theorem best_eq_none {m : NsMap} {p : PrimPath} (h : ∀ a ∈ m, ¬ a.1 <+: p) :
    best m p = none := by
  induction m with
  | nil => rfl
  | cons a rest ih =>
    simp only [best, h a (by simp), ite_false]
    exact ih (fun b hb => h b (by simp [hb]))

/-- If `c` is the only matching pair, `best` picks it. -/
theorem best_eq_of_unique {m : NsMap} {p : PrimPath} {c} (hc : c ∈ m) (hcp : c.1 <+: p)
    (hu : ∀ a ∈ m, a.1 <+: p → a = c) : best m p = some c := by
  induction m with
  | nil => simp at hc
  | cons a rest ih =>
    simp only [best]
    by_cases ha : a.1 <+: p
    · have hac := hu a (by simp) ha
      subst hac
      simp only [ha, ite_true]
      cases hb : best rest p with
      | none => rfl
      | some b =>
        have := best_spec hb
        have hbe := hu b (by simp [this.1]) this.2
        subst hbe; simp
    · simp only [ha, ite_false]
      have hcr : c ∈ rest := by
        rcases List.mem_cons.mp hc with h | h
        · subst h; exact absurd hcp ha
        · exact h
      exact ih hcr (fun b hb => hu b (by simp [hb]))

theorem best_congr {m : NsMap} {x y : PrimPath} (h : ∀ a ∈ m, (a.1 <+: x ↔ a.1 <+: y)) :
    best m x = best m y := by
  induction m with
  | nil => rfl
  | cons a rest ih =>
    have ih' := ih (fun b hb => h b (by simp [hb]))
    simp only [best, ih', h a (by simp)]

theorem prefix_comparable {a b p : PrimPath} (ha : a <+: p) (hb : b <+: p) :
    a <+: b ∨ b <+: a := by
  rcases le_total a.length b.length with h | h
  · exact Or.inl (List.prefix_of_prefix_length_le ha hb h)
  · exact Or.inr (List.prefix_of_prefix_length_le hb ha h)

theorem drop_of_prefix {s p : PrimPath} (h : s <+: p) : s ++ p.drop s.length = p := by
  obtain ⟨r, rfl⟩ := h; simp

/-! ## Inversion -/

/-- Targets are prefix-free: no pair's target is a prefix of a different pair's
target (in particular no root identity `(/,/)` beside another pair). -/
def TargetsPrefixFree (m : NsMap) : Prop := ∀ a ∈ m, ∀ b ∈ m, a.2 <+: b.2 → a = b

/-- Sources are prefix-free. -/
def SourcesPrefixFree (m : NsMap) : Prop := ∀ a ∈ m, ∀ b ∈ m, a.1 <+: b.1 → a = b

/-- Partial injectivity of the literal rule under prefix-free targets.
SPEC: L5384-5385 ("they can be inverted"). -/
theorem mapPath_inverse {m : NsMap} (hm : TargetsPrefixFree m) {p q : PrimPath}
    (h : mapPath m p = some q) : mapPath (inverse m) q = some p := by
  unfold mapPath at h
  cases hb : best m p with
  | none => simp [hb] at h
  | some c =>
    simp only [hb, Option.map_some, Option.some.injEq] at h
    obtain ⟨hcm, hcp⟩ := best_spec hb
    have htq : c.2 <+: q := h ▸ List.prefix_append _ _
    have hinv : best (inverse m) q = some c.swap := by
      apply best_eq_of_unique (List.mem_map.mpr ⟨c, hcm, rfl⟩) htq
      intro a ha haq
      simp only [List.mem_map] at ha
      obtain ⟨a', ha', rfl⟩ := ha
      rcases prefix_comparable haq htq with h1 | h1
      · rw [hm a' ha' c hcm h1]
      · rw [hm c hcm a' ha' h1]
    simp only [mapPath, hinv, Option.map_some, Prod.fst_swap, Prod.snd_swap]
    subst h
    simp [drop_of_prefix hcp]

/-- Without prefix-free targets, inversion fails: `/A/Y ↦ /X/Y ↦ /B`. -/
theorem mapPath_inverse_fails :
    let m : NsMap := [(["A"], ["X"]), (["B"], ["X", "Y"])]
    mapPath m ["A", "Y"] = some ["X", "Y"] ∧ mapPath (inverse m) ["X", "Y"] = some ["B"] := by
  decide

/-- The round-trip-checked mapping is a partial bijection with no hypothesis. -/
theorem mapChecked_inverse (m : NsMap) {p q : PrimPath} (h : mapChecked m p = some q) :
    mapChecked (inverse m) q = some p := by
  unfold mapChecked at h ⊢
  split at h
  · rename_i q' hq'
    split_ifs at h with hr
    cases h
    simp [hr, inverse_inverse, hq']
  · simp at h

/-- SPEC-ISSUE: LEAN-511 — the spec's internal-reference mapping (pairs written
source→target, see LEAN-512) maps both `/A` and `/B` to `/A`: not injective,
so not the "bijection" of L5384. pxr blocks `/A` (`mapChecked`). -/
theorem mapPath_internalRef_not_injective :
    let m : NsMap := [(["B"], ["A"]), ([], [])]
    mapPath m ["A"] = some ["A"] ∧ mapPath m ["B"] = some ["A"] ∧
    mapChecked m ["A"] = none ∧ mapChecked m ["B"] = some ["A"] ∧
    mapChecked m ["C"] = some ["C"] := by
  decide

/-! ## Composition -/

theorem mapPath_of_best_none {m : NsMap} {p : PrimPath} (h : best m p = none) :
    mapPath m p = none := by simp [mapPath, h]

theorem mapPath_of_best_some {m : NsMap} {p : PrimPath} {c} (h : best m p = some c) :
    mapPath m p = some (c.2 ++ p.drop c.1.length) := by simp [mapPath, h]

theorem mem_simpleCompose {f g : NsMap} {a : PrimPath × PrimPath}
    (h : a ∈ g.filterMap (fun a => (mapPath f a.2).map (a.1, ·))) :
    ∃ a' ∈ g, a'.1 = a.1 ∧ mapPath f a'.2 = some a.2 := by
  obtain ⟨a', ha', hm⟩ := List.mem_filterMap.mp h
  cases hx : mapPath f a'.2 with
  | none => rw [hx] at hm; cases hm
  | some x => rw [hx] at hm; cases hm; exact ⟨a', ha', rfl, hx⟩

/-- `compose_apply` under: `g` sources prefix-free (one `g` pair per path) and
no `f` source strictly below a `g` target (then the refinement part is empty). -/
theorem compose_apply {f g : NsMap} (hg : SourcesPrefixFree g)
    (hfg : ∀ a ∈ g, ∀ b ∈ f, a.2 <+: b.1 → b.1 = a.2) (p : PrimPath) :
    mapPath (compose f g) p = (mapPath g p).bind (mapPath f) := by
  have hcomp : compose f g = g.filterMap (fun a => (mapPath f a.2).map (a.1, ·)) := by
    unfold compose
    have : g.flatMap (fun a => f.filterMap (fun b =>
        if a.2 <+: b.1 ∧ a.2 ≠ b.1 then some (a.1 ++ b.1.drop a.2.length, b.2) else none)) = [] := by
      simp only [List.flatMap_eq_nil_iff, List.filterMap_eq_nil_iff]
      intro a ha b hb
      have := hfg a ha b hb
      split_ifs with hc
      · exact absurd (this hc.1).symm hc.2
      · rfl
    rw [this, List.append_nil]
  rw [hcomp]
  by_cases hex : ∃ c ∈ g, c.1 <+: p
  · obtain ⟨c, hc, hcp⟩ := hex
    have hu : ∀ a ∈ g, a.1 <+: p → a = c := by
      intro a ha hap
      rcases prefix_comparable hap hcp with h1 | h1
      · exact hg a ha c hc h1
      · exact (hg c hc a ha h1).symm
    have hbg : best g p = some c := best_eq_of_unique hc hcp hu
    -- f's choice on `c.2 ++ r` equals its choice on `c.2`
    have hfc : ∀ r, best f (c.2 ++ r) = best f c.2 := by
      intro r
      apply best_congr
      intro b hb
      constructor
      · intro h1
        rcases prefix_comparable h1 (List.prefix_append c.2 r) with h2 | h2
        · exact h2
        · rw [hfg c hc b hb h2]
      · intro h1; exact h1.trans (List.prefix_append _ _)
    have hmf : ∀ r, mapPath f (c.2 ++ r) = (mapPath f c.2).map (· ++ r) := by
      intro r
      simp only [mapPath, hfc r]
      cases hb : best f c.2 with
      | none => rfl
      | some b =>
        have := (best_spec hb).2
        obtain ⟨u, hu⟩ := this
        simp [← hu, List.drop_append]
    have rhs : (mapPath g p).bind (mapPath f) = (mapPath f c.2).map (· ++ p.drop c.1.length) := by
      rw [show mapPath g p = some (c.2 ++ p.drop c.1.length) by simp [mapPath, hbg]]
      simp only [Option.bind_some]; exact hmf _
    rw [rhs]
    cases hm : mapPath f c.2 with
    | none =>
      simp only [Option.map_none]
      apply mapPath_of_best_none
      apply best_eq_none
      intro a ha hap
      obtain ⟨a', ha', h1, h2⟩ := mem_simpleCompose ha
      have := hu a' ha' (h1 ▸ hap)
      subst this; rw [hm] at h2; cases h2
    | some t =>
      simp only [Option.map_some]
      apply mapPath_of_best_some (c := (c.1, t))
      apply best_eq_of_unique (c := (c.1, t)) _ hcp
      · intro a ha hap
        obtain ⟨a', ha', h1, h2⟩ := mem_simpleCompose ha
        have := hu a' ha' (h1 ▸ hap)
        subst this; rw [hm] at h2; cases h2
        exact Prod.ext h1.symm rfl
      · simp only [List.mem_filterMap]; exact ⟨c, hc, by simp [hm]⟩
  · simp only [not_exists, not_and] at hex
    have hbg : best g p = none := best_eq_none hex
    rw [mapPath_of_best_none hbg, Option.bind_none]
    apply mapPath_of_best_none
    apply best_eq_none
    intro a ha hap
    obtain ⟨a', ha', h1, _⟩ := mem_simpleCompose ha
    exact hex a' ha' (h1 ▸ hap)

/-! ## Spec examples (SPEC: L5402-5419 and the per-arc paragraphs) -/

/-- L5404-5410: `[(/A, /B)]`. -/
example : mapPath [(["A"], ["B"])] ["A"] = some ["B"] ∧
    mapPath [(["A"], ["B"])] ["A", "Child"] = some ["B", "Child"] ∧
    mapPath [(["A"], ["B"])] ["C"] = none := by decide

/-- L5412-5418: `[(/A, /B), (/, /)]`. -/
example : mapPath [(["A"], ["B"]), ([], [])] ["A"] = some ["B"] ∧
    mapPath [(["A"], ["B"]), ([], [])] ["A", "Child"] = some ["B", "Child"] ∧
    mapPath [(["A"], ["B"]), ([], [])] ["C"] = some ["C"] := by decide

/-- References L4413-4426 (`/A` references `@ref@</B>`): source `/B`, target
`/A` (L5380-5382), so source→target is `[(/B, /A)]`; the spec prints
`[(/A, /B)]` (SPEC-ISSUE: LEAN-512). -/
example : mapPath [(["B"], ["A"])] ["B", "Child"] = some ["A", "Child"] := by decide

/-- Variants do not remap namespace (L4813-4817): the identity mapping. -/
example : mapPath [([], [])] ["A", "B"] = some ["A", "B"] := by decide

/-- Relocates L4949-4951: reference `[(/Ref, /A)]` composed with the relocates
mapping `[(/A/B, /A/C)]` plus the identity gives the spec's result. -/
theorem compose_relocates_example :
    compose [([], []), (["A", "B"], ["A", "C"])] [(["Ref"], ["A"])] =
      [(["Ref"], ["A"]), (["Ref", "B"], ["A", "C"])] := by decide

/-- SPEC-ISSUE: LEAN-513 — without the identity, the composition drops `(/Ref, /A)`. -/
theorem compose_relocates_example_noIdentity :
    compose [(["A", "B"], ["A", "C"])] [(["Ref"], ["A"])] = [(["Ref", "B"], ["A", "C"])] := by
  decide

end UsdSpec.NamespaceMap
