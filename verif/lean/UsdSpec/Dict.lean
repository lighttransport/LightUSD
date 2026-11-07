import Mathlib.Data.List.Basic
import Mathlib.Data.List.Nodup
/-!
# Dictionaries (AOUSD USD Core Specification v1.1.0, §6.6.2)

SPEC: core_spec.md L1221-1269 (Dictionaries; Combining `∪`), L6200-6236
(Value resolution of dictionary fields).

A dictionary is "an unordered map of strings to heterogenous values" (L1223-1224)
whose values may themselves be dictionaries (L1228-1232, "nested"). The
combining operator `∪` (L1241-1269) is transcribed bullet-for-bullet as `merge`;
every definition is executable (`decide` on the spec's worked example).

Representation. The spec's dictionary is unordered; here a dictionary is an
association list `Dict α := List (String × DVal α)`, which is what every
serialization and every implementation actually carries. Uniqueness of keys
("each key maps to one and only one value", L1224-1225) is the `WellFormed`
predicate (`Nodup` keys, recursively), carried as a hypothesis where needed
and never as a subtype — the same discipline as `ListOp.lean`.

Entry order of `merge S W` (not fixed by the spec; L1223 "unordered"): the
entries of the stronger `S` in their order (with values replaced by the
recursively combined value where both sides hold a dictionary), followed by
the entries of `W` whose keys are not in `S`, in `W`'s order. This is the
order LightUSD's `MergeWeakerDictionary` produces
(`src/next/composition/composition-merge-helpers.cc:62-77`).

Spec issues found while formalizing are tagged `SPEC-ISSUE: LEAN-NNN`:

* `LEAN-101` — `∪` is **not associative** (contrary to L1243-1244 "closed and
  associative"), not even up to entry order: see `merge_not_assoc`. It is
  associative on shape-compatible operands (`merge_assoc_of_compat`), and the
  *key sequence* is always associative (`keys_merge_assoc`).
* `LEAN-102` — because of LEAN-101 the fold direction of the value-resolution
  rule L6200-6206 matters and the spec does not state one; `resolveDict`
  (accumulate strongest-first, `((S ∪ W₁) ∪ W₂)`, the LightUSD order) and
  `resolveDictR` (`S ∪ (W₁ ∪ W₂)`) differ: `resolveDict_ne_resolveDictR`.
-/

namespace UsdSpec

/-- A dictionary value: a scalar / dimensioned / array leaf of some value type
`α`, or a nested dictionary. SPEC: L1226-1232 (value types), L1234 ("nested"). -/
inductive DVal (α : Type)
  | leaf (v : α)
  | dict (entries : List (String × DVal α))
  deriving Repr, Inhabited

namespace DVal

mutual
/-- Decidable equality (the `deriving` handler does not cover nested inductives;
written out by mutual structural recursion so that `decide` evaluates it). -/
def decEq {α : Type} [DecidableEq α] : (a b : DVal α) → Decidable (a = b)
  | .leaf u, .leaf v =>
      if h : u = v then isTrue (by rw [h]) else isFalse (by intro e; cases e; exact h rfl)
  | .leaf _, .dict _ => isFalse (by intro e; cases e)
  | .dict _, .leaf _ => isFalse (by intro e; cases e)
  | .dict s, .dict w =>
      match decEqEntries s w with
      | isTrue h => isTrue (by rw [h])
      | isFalse h => isFalse (by intro e; cases e; exact h rfl)
def decEqEntries {α : Type} [DecidableEq α] :
    (s w : List (String × DVal α)) → Decidable (s = w)
  | [], [] => isTrue rfl
  | [], _ :: _ => isFalse (by intro e; cases e)
  | _ :: _, [] => isFalse (by intro e; cases e)
  | (k, v) :: s, (k', v') :: w =>
      if hk : k = k' then
        match decEq v v' with
        | isTrue hv =>
          match decEqEntries s w with
          | isTrue hs => isTrue (by rw [hk, hv, hs])
          | isFalse hs => isFalse (by intro e; cases e; exact hs rfl)
        | isFalse hv => isFalse (by intro e; cases e; exact hv rfl)
      else isFalse (by intro e; cases e; exact hk rfl)
end

instance {α : Type} [DecidableEq α] : DecidableEq (DVal α) := decEq

end DVal

/-- A dictionary: an association list from keys to values. SPEC: L1223-1225. -/
abbrev Dict (α : Type) := List (String × DVal α)

namespace Dict

variable {α : Type}

/-- The key sequence of a dictionary. -/
def keys (d : Dict α) : List String := d.map Prod.fst

@[simp] theorem keys_nil : keys ([] : Dict α) = [] := rfl
@[simp] theorem keys_cons (k : String) (v : DVal α) (rest : Dict α) :
    keys ((k, v) :: rest) = k :: keys rest := rfl
theorem mem_keys_iff (k : String) (d : Dict α) : k ∈ keys d ↔ ∃ v, (k, v) ∈ d := by
  simp [keys]

/-- First-match lookup (the only match for a `WellFormed` dictionary). -/
def lookup (k : String) : Dict α → Option (DVal α)
  | [] => none
  | (k', v) :: rest => if k' = k then some v else lookup k rest

/-- Lookup along a key path through nested dictionaries. `lookupPath [] d` is
`d` itself. -/
def lookupPath : List String → Dict α → Option (DVal α)
  | [], d => some (.dict d)
  | k :: rest, d =>
      match rest, lookup k d with
      | [], r => r
      | _ :: _, some (.dict d') => lookupPath rest d'
      | _ :: _, _ => none

mutual
/-- Combining `∪`, the stronger side's entries. SPEC: L1241-1269.
`mergeStrong S W` produces the first three bullets of L1246-1253 (one entry per
entry of `S`, in `S`'s order); `merge` appends the fourth bullet (entries of `W`
not keyed in `S`). -/
def mergeStrong : Dict α → Dict α → Dict α
  | [], _ => []
  | (k, v) :: rest, w => (k, mergeVal v (lookup k w)) :: mergeStrong rest w
/-- The per-key rule of `∪`: when both values are dictionaries they are combined
recursively (L1251-1252), otherwise the stronger value `v` is kept (L1249-1250). -/
def mergeVal : DVal α → Option (DVal α) → DVal α
  | .dict sv, some (.dict wv) =>
      .dict (mergeStrong sv wv ++ wv.filter (fun kv => !decide (kv.1 ∈ keys sv)))
  | v, _ => v
end

/-- Combining `∪`. SPEC: L1241-1269: `mergeStrong S W` (bullets 1-3 of L1246-1252)
followed by the entries of `W` whose keys are not in `S` (bullet 4, L1254). -/
def merge (s w : Dict α) : Dict α :=
  mergeStrong s w ++ w.filter (fun kv => !decide (kv.1 ∈ keys s))

@[inherit_doc merge] infixl:65 " ∪ᵈ " => merge

/-- Keys are unique, recursively. SPEC: L1224-1225 ("one and only one value"). -/
inductive WellFormed : DVal α → Prop
  | leaf (v : α) : WellFormed (.leaf v)
  | dict (es : Dict α) (hnd : (keys es).Nodup)
      (hall : ∀ kv ∈ es, WellFormed kv.2) : WellFormed (.dict es)

/-- `WellFormed` for a top-level dictionary. -/
def WF (d : Dict α) : Prop := WellFormed (.dict d)

/-- Shape compatibility: at every key held by both, both values are leaves or
both are dictionaries (recursively). This is the hypothesis under which `∪`
is associative (`merge_assoc_of_compat`). -/
inductive Compat : DVal α → DVal α → Prop
  | leaf (u v : α) : Compat (.leaf u) (.leaf v)
  | dict (s w : Dict α)
      (h : ∀ k vs vw, lookup k s = some vs → lookup k w = some vw → Compat vs vw) :
      Compat (.dict s) (.dict w)

/-- Value resolution of a dictionary field over a strength-ordered list of
opinions (strongest first). SPEC: L6200-6206: "Composition provides a strength
ordering of dictionary value opinions and value resolution ... combines them
according to those combining rule[s]". Accumulates from the strongest:
`resolveDict [S, W₁, W₂] = (S ∪ W₁) ∪ W₂` — the order LightUSD uses
(`MergeWeakerDictionary(stronger_so_far, weaker)`). -/
def resolveDict (stack : List (Dict α)) : Dict α :=
  stack.foldl merge []

/-- The other association, `S ∪ (W₁ ∪ W₂)` (`foldr`). Equals `resolveDict` on
shape-compatible stacks (`resolveDict_eq_resolveDictR_of_compat`), not in
general (`resolveDict_ne_resolveDictR`, SPEC-ISSUE LEAN-102). -/
def resolveDictR (stack : List (Dict α)) : Dict α :=
  stack.foldr merge []

/-! ## Equations -/

theorem mergeVal_dict_dict (sv wv : Dict α) :
    mergeVal (.dict sv) (some (.dict wv)) = .dict (merge sv wv) := rfl

theorem mergeVal_leaf (v : α) (o : Option (DVal α)) : mergeVal (.leaf v) o = .leaf v := by
  cases o with
  | none => rfl
  | some w => cases w <;> rfl

theorem mergeVal_none (v : DVal α) : mergeVal v none = v := by
  cases v <;> rfl

theorem mergeVal_some_leaf (v : DVal α) (w : α) : mergeVal v (some (.leaf w)) = v := by
  cases v <;> rfl

theorem mergeStrong_nil (w : Dict α) : mergeStrong [] w = [] := rfl
theorem mergeStrong_cons (k : String) (v : DVal α) (rest w : Dict α) :
    mergeStrong ((k, v) :: rest) w = (k, mergeVal v (lookup k w)) :: mergeStrong rest w := rfl

/-! ## Worked example from the spec, checked by `decide`.

SPEC: L6208-6236: a `customData = { bool a = true }` opinion and a
`customData = { bool b = false }` opinion resolve to `{ bool a = true; bool b = false }`.
The spec does not say which opinion is stronger (the `over` is listed second);
both orders give the same key set, and in the LightUSD entry order the stronger
opinion's entries come first. -/
example : resolveDict [[("a", DVal.leaf true)], [("b", DVal.leaf false)]]
    = [("a", DVal.leaf true), ("b", DVal.leaf false)] := by decide

example : resolveDict [[("b", DVal.leaf false)], [("a", DVal.leaf true)]]
    = [("b", DVal.leaf false), ("a", DVal.leaf true)] := by decide

/-- The four bullets of L1246-1253 on a small example: `x` only in `S`, `y` in
both (stronger wins, L1249-1250), `z` in both as dictionaries (combined,
L1251-1252), `w` only in `W` (L1254). -/
example :
    merge [("x", DVal.leaf 1), ("y", DVal.leaf 2), ("z", DVal.dict [("p", DVal.leaf 3)])]
          [("w", DVal.leaf 9), ("y", DVal.leaf 8), ("z", DVal.dict [("q", DVal.leaf 7)])]
    = [("x", DVal.leaf 1), ("y", DVal.leaf 2),
       ("z", DVal.dict [("p", DVal.leaf 3), ("q", DVal.leaf 7)]), ("w", DVal.leaf 9)] := by
  decide

/-! ## Lookup / keys lemmas -/

theorem lookup_eq_none_iff (k : String) (d : Dict α) : lookup k d = none ↔ k ∉ keys d := by
  induction d with
  | nil => simp [lookup, keys]
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    by_cases h : k' = k
    · subst h; simp [lookup, keys]
    · simp [lookup, keys, h, ih, Ne.symm h]

theorem lookup_append (k : String) (d₁ d₂ : Dict α) :
    lookup k (d₁ ++ d₂) = (lookup k d₁).or (lookup k d₂) := by
  induction d₁ with
  | nil => simp [lookup]
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    by_cases h : k' = k <;> simp [lookup, h, ih]

theorem lookup_filter_notMem (k : String) (w : Dict α) (ks : List String) :
    lookup k (w.filter (fun kv => !decide (kv.1 ∈ ks))) =
      if k ∈ ks then none else lookup k w := by
  induction w with
  | nil => simp [lookup]
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    by_cases hk : k' ∈ ks
    · by_cases h : k' = k
      · subst h; rw [List.filter_cons_of_neg (by simpa using hk), ih]; simp [hk]
      · rw [List.filter_cons_of_neg (by simpa using hk), ih]
        simp [lookup, h]
    · by_cases h : k' = k
      · subst h; rw [List.filter_cons_of_pos (by simpa using hk)]; simp [lookup, hk]
      · rw [List.filter_cons_of_pos (by simpa using hk)]; simp [lookup, h, ih]

theorem lookup_mergeStrong (k : String) (s w : Dict α) :
    lookup k (mergeStrong s w) = (lookup k s).map (fun v => mergeVal v (lookup k w)) := by
  induction s with
  | nil => simp [mergeStrong_nil, lookup]
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    by_cases h : k' = k
    · subst h; simp [mergeStrong_cons, lookup]
    · simp [mergeStrong_cons, lookup, h, ih]

theorem keys_append (d₁ d₂ : Dict α) : keys (d₁ ++ d₂) = keys d₁ ++ keys d₂ := by
  simp [keys]

theorem keys_filter_notMem (w : Dict α) (ks : List String) :
    keys (w.filter (fun kv => !decide (kv.1 ∈ ks))) =
      (keys w).filter (fun x => !decide (x ∈ ks)) := by
  induction w with
  | nil => rfl
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    by_cases hk : k' ∈ ks
    · rw [List.filter_cons_of_neg (by simpa using hk), keys_cons,
        List.filter_cons_of_neg (by simpa using hk), ih]
    · rw [List.filter_cons_of_pos (by simpa using hk), keys_cons, keys_cons,
        List.filter_cons_of_pos (by simpa using hk), ih]

theorem keys_mergeStrong (s w : Dict α) : keys (mergeStrong s w) = keys s := by
  induction s with
  | nil => rfl
  | cons kv rest ih =>
    obtain ⟨k', v⟩ := kv
    rw [mergeStrong_cons, keys_cons, keys_cons, ih]

/-- The key sequence of `S ∪ W`: `S`'s keys, then `W`'s keys not in `S`. -/
theorem keys_merge (s w : Dict α) :
    keys (s ∪ᵈ w) = keys s ++ (keys w).filter (fun x => !decide (x ∈ keys s)) := by
  rw [merge, keys_append, keys_mergeStrong, keys_filter_notMem]

theorem mem_keys_merge (k : String) (s w : Dict α) :
    k ∈ keys (s ∪ᵈ w) ↔ k ∈ keys s ∨ k ∈ keys w := by
  rw [keys_merge]; simp only [List.mem_append, List.mem_filter]
  by_cases hs : k ∈ keys s <;> simp [hs]

/-- (D5) Lookup characterization of `∪`. SPEC: L1246-1253, one bullet per case. -/
theorem lookup_merge (k : String) (s w : Dict α) :
    lookup k (s ∪ᵈ w) =
      match lookup k s, lookup k w with
      | some (.dict sv), some (.dict wv) => some (.dict (sv ∪ᵈ wv))
      | some v, _ => some v
      | none, r => r := by
  unfold merge
  rw [lookup_append, lookup_mergeStrong, lookup_filter_notMem]
  by_cases hk : k ∈ keys s
  · have : lookup k s ≠ none := by rw [Ne, lookup_eq_none_iff]; exact not_not.mpr hk
    obtain ⟨v, hv⟩ := Option.ne_none_iff_exists'.mp this
    rw [hv]
    cases v with
    | leaf x => simp [hk, mergeVal_leaf]
    | dict sv =>
      cases hw : lookup k w with
      | none => simp [hk, mergeVal_none]
      | some wv => cases wv <;> simp [hk, mergeVal_dict_dict, mergeVal_some_leaf, merge]
  · have hs : lookup k s = none := (lookup_eq_none_iff k s).mpr hk
    rw [hs]; simp [hk]

/-- `lookup_merge` with the stronger side a `dict` at `k`. -/
theorem lookup_merge_dict_left {k : String} {s w : Dict α} {sv : Dict α}
    (hs : lookup k s = some (.dict sv)) :
    lookup k (s ∪ᵈ w) =
      some (match lookup k w with
            | some (.dict wv) => .dict (sv ∪ᵈ wv)
            | _ => .dict sv) := by
  rw [lookup_merge, hs]
  cases lookup k w with
  | none => rfl
  | some wv => cases wv <;> rfl

/-! ## (D3) The empty dictionary is a two-sided unit. -/

theorem mergeStrong_nil_right (s : Dict α) : mergeStrong s [] = s := by
  induction s with
  | nil => rfl
  | cons kv rest ih =>
    obtain ⟨k, v⟩ := kv
    simp [mergeStrong_cons, lookup, mergeVal_none, ih]

theorem merge_nil_left (w : Dict α) : ([] : Dict α) ∪ᵈ w = w := by
  simp [merge, mergeStrong_nil, keys]

theorem merge_nil_right (s : Dict α) : s ∪ᵈ [] = s := by
  simp [merge, mergeStrong_nil_right]

/-! ## (D2) Well-formedness is preserved. -/

theorem WF.nodup {d : Dict α} (h : WF d) : (keys d).Nodup := by
  cases h with | dict _ hnd _ => exact hnd

theorem WF.of_mem {d : Dict α} (h : WF d) {kv : String × DVal α} (hm : kv ∈ d) :
    WellFormed kv.2 := by
  cases h with | dict _ _ hall => exact hall kv hm

theorem WF.of_lookup {d : Dict α} (h : WF d) {k : String} {v : DVal α}
    (hl : lookup k d = some v) : WellFormed v := by
  induction d with
  | nil => simp [lookup] at hl
  | cons kv rest ih =>
    obtain ⟨k', v'⟩ := kv
    by_cases hk : k' = k
    · subst hk; simp [lookup] at hl; subst hl; exact h.of_mem (List.mem_cons_self)
    · simp [lookup, hk] at hl
      cases h with
      | dict _ hnd hall =>
        exact ih (WellFormed.dict rest (List.Nodup.of_cons hnd)
          (fun kv hm => hall kv (List.mem_cons_of_mem _ hm))) hl

theorem WF.cons_iff {k : String} {v : DVal α} {rest : Dict α} :
    WF ((k, v) :: rest) ↔ k ∉ keys rest ∧ WellFormed v ∧ WF rest := by
  constructor
  · intro h
    cases h with
    | dict _ hnd hall =>
      simp only [keys, List.map_cons, List.nodup_cons] at hnd
      exact ⟨hnd.1, hall _ List.mem_cons_self,
        WellFormed.dict rest hnd.2 (fun kv hm => hall kv (List.mem_cons_of_mem _ hm))⟩
  · rintro ⟨hk, hv, hrest⟩
    cases hrest with
    | dict _ hnd hall =>
      refine WellFormed.dict _ ?_ ?_
      · simp only [keys, List.map_cons, List.nodup_cons]; exact ⟨hk, hnd⟩
      · intro kv hm
        rcases List.mem_cons.mp hm with rfl | hm
        · exact hv
        · exact hall kv hm

theorem WF.nil : WF ([] : Dict α) := WellFormed.dict [] List.nodup_nil (by simp)

/-- In a `WellFormed` dictionary, lookup of an entry's key returns that entry. -/
theorem lookup_of_mem_of_wf {d : Dict α} (h : WF d) {k : String} {v : DVal α}
    (hm : (k, v) ∈ d) : lookup k d = some v := by
  induction d with
  | nil => simp at hm
  | cons kv rest ih =>
    obtain ⟨k', v'⟩ := kv
    rw [WF.cons_iff] at h
    rcases List.mem_cons.mp hm with heq | hm'
    · cases heq; simp [lookup]
    · have hne : k' ≠ k := by
        rintro rfl
        exact h.1 (List.mem_map.mpr ⟨(k', v), hm', rfl⟩)
      simp [lookup, hne, ih h.2.2 hm']

/-- Every entry of `mergeStrong s w` comes from an entry of `s` via `mergeVal`. -/
theorem mem_mergeStrong {kv : String × DVal α} {s w : Dict α} (hm : kv ∈ mergeStrong s w) :
    ∃ v, (kv.1, v) ∈ s ∧ kv.2 = mergeVal v (lookup kv.1 w) := by
  induction s with
  | nil => simp [mergeStrong_nil] at hm
  | cons kv' rest ih =>
    obtain ⟨k', v'⟩ := kv'
    rw [mergeStrong_cons] at hm
    rcases List.mem_cons.mp hm with rfl | hm'
    · exact ⟨v', List.mem_cons_self, rfl⟩
    · obtain ⟨v, hv, heq⟩ := ih hm'
      exact ⟨v, List.mem_cons_of_mem _ hv, heq⟩

/-- (D2) `∪` preserves well-formedness. -/
theorem merge_wf : ∀ {s : Dict α}, WF s → ∀ {w : Dict α}, WF w → WF (s ∪ᵈ w) := by
  -- strong induction on the stronger dictionary via its `WellFormed` derivation
  suffices H : ∀ a : DVal α, WellFormed a → ∀ s : Dict α, a = .dict s →
      ∀ w : Dict α, WF w → WF (s ∪ᵈ w) by
    intro s hs w hw; exact H _ hs s rfl w hw
  intro a ha
  induction ha with
  | leaf v => intro s h; cases h
  | dict es hnd hall ih =>
    intro s hs w hw
    cases hs
    refine WellFormed.dict _ ?_ ?_
    · rw [keys_merge]
      refine List.Nodup.append hnd (List.Nodup.filter _ hw.nodup) ?_
      intro k hk hk'
      exact absurd hk (by simpa using (List.mem_filter.mp hk').2)
    · intro kv hm
      unfold merge at hm
      rcases List.mem_append.mp hm with hm | hm
      · obtain ⟨v, hv, heq⟩ := mem_mergeStrong hm
        rw [heq]
        cases hl : lookup kv.1 w with
        | none => rw [mergeVal_none]; exact hall _ hv
        | some wv =>
          cases v with
          | leaf x => rw [mergeVal_leaf]; exact WellFormed.leaf x
          | dict sv =>
            cases wv with
            | leaf y => rw [mergeVal_some_leaf]; exact hall _ hv
            | dict wv =>
              rw [mergeVal_dict_dict]
              exact ih (kv.1, .dict sv) hv sv rfl wv (hw.of_lookup hl)
      · exact hw.of_mem (List.mem_filter.mp hm).1

/-! ## (D4) Idempotence -/

theorem merge_self : ∀ {s : Dict α}, WF s → s ∪ᵈ s = s := by
  suffices H : ∀ a : DVal α, WellFormed a → ∀ s : Dict α, a = .dict s → s ∪ᵈ s = s by
    intro s hs; exact H _ hs s rfl
  intro a ha
  induction ha with
  | leaf v => intro s h; cases h
  | dict es hnd hall ih =>
    intro s hs
    cases hs
    have hwf : WF es := WellFormed.dict es hnd hall
    unfold merge
    have h2 : es.filter (fun kv => !decide (kv.1 ∈ keys es)) = [] := by
      rw [List.filter_eq_nil_iff]
      intro kv hm
      have : kv.1 ∈ keys es := (mem_keys_iff _ _).mpr ⟨kv.2, hm⟩
      simp [this]
    rw [h2, List.append_nil]
    -- `mergeStrong es es = es`: each entry looks itself up
    have key : ∀ (l : Dict α), (∀ kv ∈ l, kv ∈ es) → mergeStrong l es = l := by
      intro l
      induction l with
      | nil => intro _; rfl
      | cons kv rest ihr =>
        intro hsub
        obtain ⟨k, v⟩ := kv
        rw [mergeStrong_cons, ihr (fun kv h => hsub kv (List.mem_cons_of_mem _ h))]
        rw [lookup_of_mem_of_wf hwf (hsub _ List.mem_cons_self)]
        cases v with
        | leaf x => rw [mergeVal_leaf]
        | dict sv =>
          rw [mergeVal_dict_dict, ih (k, .dict sv) (hsub _ List.mem_cons_self) sv rfl]
    exact key es (fun _ h => h)

/-! ## Extensionality for well-formed dictionaries -/

/-- Two well-formed dictionaries with the same key sequence and the same lookups
are equal. -/
theorem ext_of_keys_lookup : ∀ {d₁ d₂ : Dict α}, WF d₁ → keys d₁ = keys d₂ →
    (∀ k, lookup k d₁ = lookup k d₂) → d₁ = d₂ := by
  intro d₁
  induction d₁ with
  | nil =>
    intro d₂ _ hk _
    cases d₂ with
    | nil => rfl
    | cons _ _ => simp [keys] at hk
  | cons kv rest ih =>
    intro d₂ h hk hl
    obtain ⟨k, v⟩ := kv
    cases d₂ with
    | nil => simp [keys] at hk
    | cons kv' rest' =>
      obtain ⟨k', v'⟩ := kv'
      simp only [keys_cons, List.cons.injEq] at hk
      obtain ⟨rfl, hkr⟩ := hk
      rw [WF.cons_iff] at h
      have hv : v = v' := by
        have := hl k; simpa [lookup] using this
      subst hv
      congr 1
      refine ih h.2.2 hkr ?_
      intro k''
      by_cases hkk : k = k''
      · subst hkk
        rw [(lookup_eq_none_iff k rest).mpr h.1,
          (lookup_eq_none_iff k rest').mpr (hkr ▸ h.1)]
      · have := hl k''; simpa [lookup, hkk] using this

/-! ## (D1) Associativity — SPEC-ISSUE: LEAN-101

The spec claims (L1243-1244) that `∪` is "a closed and associative operation".
Closure holds by construction. Associativity is **false**: a leaf in the middle
operand hides the weaker dictionary from the stronger one in `(a ∪ b) ∪ c` but
hides the stronger from the weaker in `a ∪ (b ∪ c)`:

    a = { x = {} }, b = { x = 1 }, c = { x = { y = 2 } }
    (a ∪ b) ∪ c = { x = { y = 2 } }     (a ∪ b = a; then both dictionaries, combined)
    a ∪ (b ∪ c) = { x = {} }           (b ∪ c = b; then a's dictionary wins over the leaf)

The two results differ in the *value* at key path `[x]`, so no choice of entry
order repairs the claim. What does hold:

* the key sequence is associative unconditionally (`keys_merge_assoc`);
* `∪` is associative when the operands are pairwise shape-compatible
  (`merge_assoc_of_compat`: at every shared key path both sides are leaves or
  both are dictionaries) — in particular on dictionaries that never mix a
  leaf and a sub-dictionary under one key.

SPEC-ISSUE: LEAN-101 — L1243-1244 "Combining of two dictionaries ... is a
closed and associative operation": associativity fails (`merge_not_assoc`);
true only under `Compat` (`merge_assoc_of_compat`). -/

/-- Counterexample to associativity (SPEC-ISSUE LEAN-101), on well-formed
operands, observed through `lookupPath [x]`. -/
theorem merge_not_assoc :
    ∃ a b c : Dict Nat, WF a ∧ WF b ∧ WF c ∧
      (a ∪ᵈ b) ∪ᵈ c ≠ a ∪ᵈ (b ∪ᵈ c) ∧
      lookupPath ["x"] ((a ∪ᵈ b) ∪ᵈ c) ≠ lookupPath ["x"] (a ∪ᵈ (b ∪ᵈ c)) := by
  refine ⟨[("x", .dict [])], [("x", .leaf 1)], [("x", .dict [("y", .leaf 2)])],
    ?_, ?_, ?_, by decide, by decide⟩
  · exact WF.cons_iff.mpr ⟨by simp [keys], WellFormed.dict [] List.nodup_nil (by simp), WF.nil⟩
  · exact WF.cons_iff.mpr ⟨by simp [keys], WellFormed.leaf 1, WF.nil⟩
  · exact WF.cons_iff.mpr ⟨by simp [keys],
      WF.cons_iff.mpr ⟨by simp [keys], WellFormed.leaf 2, WF.nil⟩, WF.nil⟩

/-- The key sequence of `∪` is associative, for every entry order question the
spec's "unordered" (L1223) leaves open. -/
theorem keys_merge_assoc (a b c : Dict α) :
    keys ((a ∪ᵈ b) ∪ᵈ c) = keys (a ∪ᵈ (b ∪ᵈ c)) := by
  simp only [keys_merge, List.filter_append, List.append_assoc, List.filter_filter]
  congr 2
  apply List.filter_congr
  intro x _
  by_cases ha : x ∈ keys a <;> by_cases hb : x ∈ keys b <;> simp [ha, hb]

theorem Compat.lookup {s w : Dict α} (h : Compat (.dict s) (.dict w)) {k : String}
    {vs vw : DVal α} (hs : lookup k s = some vs) (hw : lookup k w = some vw) : Compat vs vw := by
  cases h with | dict _ _ h => exact h k vs vw hs hw

/-- (D1, true form) `∪` is associative on pairwise shape-compatible, well-formed
dictionaries. -/
theorem merge_assoc_of_compat : ∀ {a : Dict α}, WF a → ∀ {b c : Dict α}, WF b → WF c →
    Compat (.dict a) (.dict b) → Compat (.dict a) (.dict c) → Compat (.dict b) (.dict c) →
    (a ∪ᵈ b) ∪ᵈ c = a ∪ᵈ (b ∪ᵈ c) := by
  suffices H : ∀ x : DVal α, WellFormed x → ∀ a : Dict α, x = .dict a →
      ∀ b c : Dict α, WF b → WF c →
      Compat (.dict a) (.dict b) → Compat (.dict a) (.dict c) → Compat (.dict b) (.dict c) →
      (a ∪ᵈ b) ∪ᵈ c = a ∪ᵈ (b ∪ᵈ c) by
    intro a ha b c hb hc hab hac hbc; exact H _ ha a rfl b c hb hc hab hac hbc
  intro x hx
  induction hx with
  | leaf v => intro a h; cases h
  | dict es hnd hall ih =>
    intro a ha b c hb hc hab hac hbc
    cases ha
    have hwa : WF es := WellFormed.dict es hnd hall
    apply ext_of_keys_lookup (merge_wf (merge_wf hwa hb) hc) (keys_merge_assoc es b c)
    intro k
    rw [lookup_merge, lookup_merge, lookup_merge, lookup_merge]
    cases hla : lookup k es with
    | none =>
      cases lookup k b with
      | none => rfl
      | some vb => cases vb <;> cases lookup k c with | none => rfl | some vc => cases vc <;> rfl
    | some va =>
      cases va with
      | leaf x =>
        cases hlb : lookup k b with
        | none => cases hlc : lookup k c with | none => rfl | some vc => cases vc <;> rfl
        | some vb =>
          cases vb with
          | leaf y => cases hlc : lookup k c with | none => rfl | some vc => cases vc <;> rfl
          | dict _ => cases hab.lookup hla hlb
      | dict sa =>
        cases hlb : lookup k b with
        | none =>
          cases hlc : lookup k c with
          | none => rfl
          | some vc => cases vc <;> rfl
        | some vb =>
          cases vb with
          | leaf y => cases hab.lookup hla hlb
          | dict sb =>
            cases hlc : lookup k c with
            | none => rfl
            | some vc =>
              cases vc with
              | leaf z => cases hac.lookup hla hlc
              | dict sc =>
                show some (DVal.dict ((sa ∪ᵈ sb) ∪ᵈ sc)) = some (DVal.dict (sa ∪ᵈ (sb ∪ᵈ sc)))
                have hmem : (k, DVal.dict sa) ∈ es := by
                  -- recover membership from lookup
                  clear ih hall hnd hwa hab hac hbc hlb hlc
                  induction es with
                  | nil => simp [lookup] at hla
                  | cons kv rest ihr =>
                    obtain ⟨k', v'⟩ := kv
                    by_cases hk : k' = k
                    · subst hk; simp [lookup] at hla; subst hla; exact List.mem_cons_self
                    · simp [lookup, hk] at hla; exact List.mem_cons_of_mem _ (ihr hla)
                rw [ih (k, .dict sa) hmem sa rfl sb sc (hb.of_lookup hlb) (hc.of_lookup hlc)
                  (hab.lookup hla hlb) (hac.lookup hla hlc) (hbc.lookup hlb hlc)]

/-! ## Value resolution (L6200-6206) as a fold -/

theorem resolveDict_nil : resolveDict ([] : List (Dict α)) = [] := rfl

/-- `resolveDict` is the iterated binary merge, strongest first: the running
result (strongest so far) is combined over the next weaker opinion. -/
theorem resolveDict_cons (d : Dict α) (rest : List (Dict α)) :
    resolveDict (d :: rest) = rest.foldl merge d := by
  simp [resolveDict, merge_nil_left]

theorem resolveDict_two (a b : Dict α) : resolveDict [a, b] = a ∪ᵈ b := by
  simp [resolveDict, merge_nil_left]

theorem resolveDict_three (a b c : Dict α) : resolveDict [a, b, c] = (a ∪ᵈ b) ∪ᵈ c := by
  simp [resolveDict, merge_nil_left]

theorem resolveDictR_cons (d : Dict α) (rest : List (Dict α)) :
    resolveDictR (d :: rest) = d ∪ᵈ resolveDictR rest := rfl

theorem resolveDictR_three (a b c : Dict α) : resolveDictR [a, b, c] = a ∪ᵈ (b ∪ᵈ c) := by
  simp [resolveDictR, merge_nil_right]

/-- SPEC-ISSUE: LEAN-102 — L6200-6206 says opinions are "combine[d] ... according
to those combining rule[s]" without fixing an association; by LEAN-101 the two
associations differ. LightUSD (and `resolveDict`) accumulate strongest-first. -/
theorem resolveDict_ne_resolveDictR :
    ∃ stack : List (Dict Nat), resolveDict stack ≠ resolveDictR stack :=
  ⟨[[("x", .dict [])], [("x", .leaf 1)], [("x", .dict [("y", .leaf 2)])]], by decide⟩

theorem Compat.symm : ∀ {x y : DVal α}, Compat x y → Compat y x := by
  intro x y h
  induction h with
  | leaf u v => exact Compat.leaf v u
  | dict s w _ ih => exact Compat.dict w s (fun k vw vs hw hs => ih k vs vw hs hw)

theorem Compat.nil_right (s : Dict α) : Compat (.dict s) (.dict []) :=
  Compat.dict s [] (fun _ _ _ _ h => by cases h)

/-- `Compat` is preserved by `∪` on the left: `a ~ c → b ~ c → (a ∪ b) ~ c`. -/
theorem compat_merge_left : ∀ {x z : DVal α}, Compat x z → ∀ {a c : Dict α},
    x = .dict a → z = .dict c → ∀ b : Dict α, Compat (.dict b) (.dict c) →
    Compat (.dict (a ∪ᵈ b)) (.dict c) := by
  intro x z hxz
  induction hxz with
  | leaf u v => intro a c h; cases h
  | dict s w h ih =>
    intro a c ha hc b hbc
    cases ha; cases hc
    refine Compat.dict _ _ ?_
    intro k v vc hv hvc
    rw [lookup_merge] at hv
    revert hv
    cases hla : lookup k s with
    | none =>
      cases hlb : lookup k b with
      | none => intro h'; cases h'
      | some vb =>
        cases vb <;> intro h' <;> simp only [Option.some.injEq] at h' <;> subst h' <;>
          exact hbc.lookup hlb hvc
    | some va =>
      cases va with
      | leaf x =>
        cases lookup k b with
        | none => intro h'; simp only [Option.some.injEq] at h'; subst h'; exact h k _ _ hla hvc
        | some vb =>
          cases vb <;> intro h' <;> simp only [Option.some.injEq] at h' <;> subst h' <;>
            exact h k _ _ hla hvc
      | dict sa =>
        cases hlb : lookup k b with
        | none => intro h'; simp only [Option.some.injEq] at h'; subst h'; exact h k _ _ hla hvc
        | some vb =>
          cases vb with
          | leaf y => intro h'; simp only [Option.some.injEq] at h'; subst h'; exact h k _ _ hla hvc
          | dict sb =>
            intro h'; simp only [Option.some.injEq] at h'; subst h'
            cases vc with
            | leaf z => cases h k _ _ hla hvc
            | dict sc => exact ih k _ _ hla hvc rfl rfl sb (hbc.lookup hlb hvc)

/-- `Compat` is preserved by `∪` on the right: `a ~ b → a ~ c → a ~ (b ∪ c)`. -/
theorem compat_merge_right {a b c : Dict α} (hab : Compat (.dict a) (.dict b))
    (hac : Compat (.dict a) (.dict c)) : Compat (.dict a) (.dict (b ∪ᵈ c)) :=
  (compat_merge_left hab.symm rfl rfl c hac.symm).symm

theorem foldr_merge_wf (rest : List (Dict α)) (h : ∀ e ∈ rest, WF e) :
    WF (rest.foldr merge []) := by
  induction rest with
  | nil => exact WF.nil
  | cons b rest ih =>
    exact merge_wf (h b List.mem_cons_self) (ih (fun e he => h e (List.mem_cons_of_mem _ he)))

theorem compat_foldr_merge (x : Dict α) (rest : List (Dict α))
    (h : ∀ e ∈ rest, Compat (.dict x) (.dict e)) :
    Compat (.dict x) (.dict (rest.foldr merge [])) := by
  induction rest with
  | nil => exact Compat.nil_right x
  | cons b rest ih =>
    exact compat_merge_right (h b List.mem_cons_self)
      (ih (fun e he => h e (List.mem_cons_of_mem _ he)))

/-- Both folds agree on a pairwise shape-compatible, well-formed stack
(the hypothesis under which LEAN-101/LEAN-102 do not bite). -/
theorem resolveDict_eq_resolveDictR_of_compat (stack : List (Dict α))
    (hwf : ∀ d ∈ stack, WF d)
    (hc : ∀ d₁ ∈ stack, ∀ d₂ ∈ stack, Compat (.dict d₁) (.dict d₂)) :
    resolveDict stack = resolveDictR stack := by
  have main : ∀ (rest : List (Dict α)) (d : Dict α), WF d → (∀ e ∈ rest, WF e) →
      (∀ e ∈ rest, Compat (.dict d) (.dict e)) →
      (∀ e₁ ∈ rest, ∀ e₂ ∈ rest, Compat (.dict e₁) (.dict e₂)) →
      rest.foldl merge d = d ∪ᵈ rest.foldr merge [] := by
    intro rest
    induction rest with
    | nil => intro d _ _ _ _; simp [merge_nil_right]
    | cons b rest ih =>
      intro d hd hwf hdc hcc
      have hb : WF b := hwf b List.mem_cons_self
      have hwf' : ∀ e ∈ rest, WF e := fun e he => hwf e (List.mem_cons_of_mem _ he)
      have hcc' : ∀ e₁ ∈ rest, ∀ e₂ ∈ rest, Compat (.dict e₁) (.dict e₂) :=
        fun e₁ h₁ e₂ h₂ => hcc e₁ (List.mem_cons_of_mem _ h₁) e₂ (List.mem_cons_of_mem _ h₂)
      have hbc : ∀ e ∈ rest, Compat (.dict b) (.dict e) :=
        fun e he => hcc b List.mem_cons_self e (List.mem_cons_of_mem _ he)
      have hdc' : ∀ e ∈ rest, Compat (.dict d) (.dict e) :=
        fun e he => hdc e (List.mem_cons_of_mem _ he)
      simp only [List.foldl_cons, List.foldr_cons]
      rw [ih (d ∪ᵈ b) (merge_wf hd hb) hwf'
        (fun e he => compat_merge_left (hdc' e he) rfl rfl b (hbc e he)) hcc']
      exact merge_assoc_of_compat hd hb (foldr_merge_wf rest hwf') (hdc b List.mem_cons_self)
        (compat_foldr_merge d rest hdc') (compat_foldr_merge b rest hbc)
  cases stack with
  | nil => rfl
  | cons d rest =>
    rw [resolveDict_cons, resolveDictR_cons]
    exact main rest d (hwf d List.mem_cons_self)
      (fun e he => hwf e (List.mem_cons_of_mem _ he))
      (fun e he => hc d List.mem_cons_self e (List.mem_cons_of_mem _ he))
      (fun e₁ h₁ e₂ h₂ => hc e₁ (List.mem_cons_of_mem _ h₁) e₂ (List.mem_cons_of_mem _ h₂))

end Dict
end UsdSpec
