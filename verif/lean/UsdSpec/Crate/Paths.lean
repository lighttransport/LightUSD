import Mathlib.Tactic.Ring
/-!
# Crate PATHS section (AOUSD USD Core Specification v1.1.0)

SPEC: core_spec.md L11881-11984 (Paths Section, Path Construction Algorithm).

Models the three integer arrays *after* integer decoding (see
`UsdSpec.Crate.IntegerCoding`): `pathIndexes : u32`, `elementTokenIndexes : i32`,
`jumps : i32`, one `Row` per encoded path.

Reference semantics, OpenUSD `pxr/usd/sdf/crateFile.cpp`:
* writer `_BuildCompressedPathDataRecursive` (L3300-3360) over the paths sorted by
  `SdfPath::operator<` (preorder; `_WritePaths` L3153-3167):
  `jump = curIndex - thisIndex` (child AND sibling; the sibling follows the whole child
  subtree), `0` = sibling only, `-1` = child only, `-2` = leaf. The next row after a
  node with a child is always its first child; after a sibling-only node, its sibling.
  Element token index is negated for prim property paths (`IsPrimPropertyPath`).
* reader `_BuildDecompressedPathsImpl` (L4043-4115): `hasChild = jump > 0 || jump == -1`,
  `hasSibling = jump >= 0`; the child continues at `thisIndex + 1` with parent = this
  path, the sibling at `thisIndex + jump` (or `+1` when only a sibling) with the same
  parent. The root row has an empty parent and ignores its element token.

Byte-level cross-check (pxr 26.11 `usdcat`, scratch `t.usda` → `t.usdc`, PATHS section
LZ4- and integer-decoded by a standalone script): the layer
`/A{/A/B{.x,.y},/A/C},/D` produced exactly `pxrRows` below.

SPEC-ISSUE: LEAN-413 — Step 5 (L11980-11982) re-runs the algorithm at `X + jump` for
every jump `≥ 0`; for `jump = 0` that is `X` itself, so the literal algorithm never
terminates on any file with a sibling-only row (`specAlg_diverges_on_pxr`). The jump
table (L11943-11948) also omits that `> 0` means child *and* sibling and that `0`
means the sibling is the next row.

SPEC-ISSUE: LEAN-416 — the section starts with TWO u64 counts in OpenUSD
(`_ReadPaths` L3887 `_paths.resize(Read<uint64_t>())` = total path-table size, then
`_ReadCompressedPaths` L3983 = number of encoded rows; the empty path is never
encoded, `_WritePaths` L3157). The spec table (L11898-11903) has one. Observed: a
layer with `references = @a.usda@` (empty prim path) writes `3, 2`, path indexes
`[0, 2]`.
-/

namespace UsdSpec.Crate.Paths

/-- One decoded row. -/
structure Row where
  pathIndex : Nat
  token     : Int   -- > 0 prim element, < 0 property name (L11924-11927)
  jump      : Int
  deriving DecidableEq, Repr

/-- A path tree node: element token (signed as on the wire), its path index, children
(in `SdfPath` order). -/
inductive Node where
  | mk (token : Int) (pathIndex : Nat) (children : List Node)
  deriving Repr

/-- A path as the list of signed element tokens from the root (root = `[]`). -/
abbrev Path := List Int

/-! ## Encoder (OpenUSD `_BuildCompressedPathDataRecursive`) -/

mutual
def encNode : Node → Bool → List Row
  | .mk t p cs, sib =>
    let sub := encForest cs
    let j : Int := match cs.isEmpty, sib with
      | false, true => (sub.length : Int) + 1
      | true, true => 0
      | false, false => -1
      | true, false => -2
    ⟨p, t, j⟩ :: sub
def encForest : List Node → List Row
  | [] => []
  | n :: rest => encNode n (!rest.isEmpty) ++ encForest rest
end

/-- The root has no sibling. -/
def encode (root : Node) : List Row := encNode root false

-- Expected output: `(pathIndex, path)` in preorder.
mutual
def pathsNode : Path → Node → List (Nat × Path)
  | par, .mk t p cs => (p, par ++ [t]) :: pathsForest (par ++ [t]) cs
def pathsForest : Path → List Node → List (Nat × Path)
  | _, [] => []
  | par, n :: rest => pathsNode par n ++ pathsForest par rest
end

/-- Root path is `[]` regardless of its token. -/
def paths : Node → List (Nat × Path)
  | .mk _ p cs => (p, []) :: pathsForest [] cs

/-! ## Decoder (OpenUSD `_BuildDecompressedPathsImpl`), with fuel -/

/-- `parent = none` is the empty parent (only for the root row). -/
def decodeAt (rows : List Row) : Nat → Nat → Option Path → List (Nat × Path) →
    Option (List (Nat × Path))
  | 0, _, _, _ => none
  | fuel + 1, cur, parent, acc =>
    match rows[cur]? with
    | none => none
    | some r =>
      let path : Path := match parent with
        | none => []
        | some p => p ++ [r.token]
      let acc := acc ++ [(r.pathIndex, path)]
      let hasChild := decide (r.jump > 0) || r.jump == -1
      let hasSib := decide (r.jump ≥ 0)
      if hasChild && hasSib then
        match decodeAt rows fuel (cur + 1) (some path) acc with
        | none => none
        | some acc => decodeAt rows fuel (cur + r.jump.toNat) parent acc
      else if hasChild then decodeAt rows fuel (cur + 1) (some path) acc
      else if hasSib then decodeAt rows fuel (cur + 1) parent acc
      else some acc

def decode (rows : List Row) (fuel : Nat) : Option (List (Nat × Path)) :=
  decodeAt rows fuel 0 none []

/-- Fuel monotonicity: more fuel never changes a successful result. -/
theorem decodeAt_mono (rows : List Row) :
    ∀ (f k cur : Nat) (par : Option Path) (acc res : List (Nat × Path)),
      decodeAt rows f cur par acc = some res → decodeAt rows (f + k) cur par acc = some res := by
  intro f
  induction f with
  | zero => intro k cur par acc res h; simp [decodeAt] at h
  | succ f ih =>
    intro k cur par acc res h
    rw [show f + 1 + k = (f + k) + 1 by omega]
    simp only [decodeAt] at h ⊢
    cases hr : rows[cur]? with
    | none => simp [hr] at h
    | some r =>
      simp only [hr] at h ⊢
      by_cases c1 : ((decide (r.jump > 0) || r.jump == -1) && decide (r.jump ≥ 0)) = true
      · simp only [c1, ↓reduceIte] at h ⊢
        split at h
        · exact absurd h (by simp)
        · rename_i acc' h1
          rw [ih k _ _ _ _ h1]; exact ih k _ _ _ _ h
      · simp only [c1, Bool.false_eq_true, ↓reduceIte] at h ⊢
        by_cases c2 : (decide (r.jump > 0) || r.jump == -1) = true
        · simp only [c2, ↓reduceIte] at h ⊢; exact ih k _ _ _ _ h
        · simp only [c2, Bool.false_eq_true, ↓reduceIte] at h ⊢
          by_cases c3 : decide (r.jump ≥ 0) = true
          · simp only [c3, ↓reduceIte] at h ⊢; exact ih k _ _ _ _ h
          · simp only [c3, Bool.false_eq_true, ↓reduceIte] at h ⊢; exact h

/-! ## The spec's literal algorithm (L11950-11984), with fuel

Read as: process row `X` (steps 1-2); jump (step 3); stop if leaf (step 4); if
`jump ≥ 0` recurse "from the start with the starting index as X plus the jump value"
(step 5) — `resetParent` selects whether "from the start" also resets the parent path
to empty (L11961) or keeps it; then parent := output[X] (step 6) and continue at X+1.
`none` = fuel exhausted (or out-of-range). -/
def specAlg (rows : List Row) (resetParent : Bool) : Nat → Nat → Option Path →
    List (Nat × Path) → Option (List (Nat × Path))
  | 0, _, _, _ => none
  | fuel + 1, x, parent, acc =>
    match rows[x]? with
    | none => none
    | some r =>
      let path : Path := match parent with
        | none => []
        | some p => p ++ [r.token]
      let acc := acc ++ [(r.pathIndex, path)]
      if r.jump = -2 then some acc
      else
        let acc? := if r.jump ≥ 0 then
            specAlg rows resetParent fuel (x + r.jump.toNat)
              (if resetParent then none else parent) acc
          else some acc
        match acc? with
        | none => none
        | some acc => specAlg rows resetParent fuel (x + 1) (some path) acc

/-! ## Examples -/

/-- Tokens of the pxr sample: A=1, D=2, B=8, C=9, x=11, y=12 (TOKENS section). -/
def sampleTree : Node :=
  .mk 4 0 [.mk 1 1 [.mk 8 2 [.mk (-11) 5 [], .mk (-12) 6 []], .mk 9 3 []], .mk 2 4 []]

/-- Rows read from the pxr 26.11 file (pathIndexes / elementTokenIndexes / jumps). -/
def pxrRows : List Row :=
  [⟨0, 4, -1⟩, ⟨1, 1, 5⟩, ⟨2, 8, 3⟩, ⟨5, -11, 0⟩, ⟨6, -12, -2⟩, ⟨3, 9, -2⟩, ⟨4, 2, -2⟩]

/-- Our encoder reproduces pxr's bytes. -/
theorem encode_sampleTree : encode sampleTree = pxrRows := by decide

theorem decode_pxrRows : decode pxrRows 8 = some (paths sampleTree) := by decide

theorem paths_sampleTree :
    paths sampleTree =
      [(0, []), (1, [1]), (2, [1, 8]), (5, [1, 8, -11]), (6, [1, 8, -12]), (3, [1, 9]),
       (4, [2])] := by decide

/-- Root-only layer. -/
theorem decode_root : decode (encode (.mk 4 0 [])) 1 = some [(0, [])] := by decide

/-- 3-level tree, wide at every level, properties at two depths. -/
def tree3 : Node :=
  .mk 0 0 [
    .mk 1 1 [.mk (-7) 2 [], .mk 2 3 [.mk 3 4 [.mk (-7) 5 []], .mk 4 6 []], .mk 5 7 []],
    .mk 6 8 [.mk 3 9 [.mk (-8) 10 [], .mk (-9) 11 []]],
    .mk 10 12 []]

theorem roundtrip_tree3 : decode (encode tree3) 13 = some (paths tree3) := by decide

theorem encode_tree3 :
    (encode tree3).map (·.jump) = [-1, 7, 0, 4, 2, -2, -2, -2, 4, -1, 0, -2, -2] := by decide

/-- A deep chain (every row child-only) — needs fuel = number of rows. -/
def chain : Nat → Node
  | 0 => .mk 1 0 []
  | n + 1 => .mk (n + 2) (n + 1) [chain n]

theorem roundtrip_chain : decode (encode (.mk 0 100 [chain 6])) 8 =
    some (paths (.mk 0 100 [chain 6])) := by decide

/-- Fuel is consumed per row: one fewer than the row count fails on the chain. -/
theorem chain_fuel_tight : decode (encode (.mk 0 100 [chain 6])) 7 = none := by decide

/-- SPEC-ISSUE: LEAN-413 — the literal algorithm exhausts any fuel on the pxr rows
(row 3 has jump 0), under both readings of "from the start", while the OpenUSD
reader finishes with 8. -/
theorem specAlg_diverges_on_pxr :
    specAlg pxrRows false 64 0 none [] = none ∧ specAlg pxrRows true 64 0 none [] = none := by
  decide

/-- Without jump-0 rows the literal algorithm terminates. Keeping the parent ("from
the start" = restart the loop only) yields the right `(index, path)` pairs (order is
irrelevant: the output array is indexed by path index); resetting the parent to empty
(L11961 "Start with an `empty` Parent Path") makes `/D` come out as `/` (`(3, [])`). -/
def noZero : Node := .mk 0 0 [.mk 1 1 [.mk 2 2 []], .mk 3 3 []]
theorem noZero_rows : (encode noZero).map (·.jump) = [-1, 2, -2, -2] := by decide
theorem noZero_pxr : decode (encode noZero) 4 = some [(0, []), (1, [1]), (2, [1, 2]), (3, [3])] := by
  decide
theorem noZero_spec_keep :
    specAlg (encode noZero) false 16 0 none [] =
      some [(0, []), (1, [1]), (3, [3]), (2, [1, 2])] := by decide
theorem noZero_spec_reset :
    specAlg (encode noZero) true 16 0 none [] = some [(0, []), (1, [1]), (3, []), (2, [1, 2])] := by
  decide

/-! ## General correctness and fuel bounds -/

/-- Prefix framing: the first encoded row is found independently of surrounding rows. -/
@[simp] theorem row_at_prefix (pre : List Row) (r : Row) (tail : List Row) :
    (pre ++ r :: tail)[pre.length]? = some r := by simp

theorem encForest_nonempty (ns : List Node) (hn : ns ≠ []) :
    0 < (encForest ns).length := by
  cases ns with
  | nil => contradiction
  | cons n ns => cases n; simp [encForest, encNode]

/-- General forest round trip with arbitrary prefix/suffix framing and accumulated
output. All jump forms are covered, with fuel bounded by encoded row count. -/
theorem decodeAt_encForest (fuel : Nat) :
    ∀ (ns : List Node) (pre post : List Row) (par : Path) (acc : List (Nat × Path)),
    ns ≠ [] → (encForest ns).length ≤ fuel →
    decodeAt (pre ++ encForest ns ++ post) fuel pre.length (some par) acc =
      some (acc ++ pathsForest par ns) := by
  induction fuel with
  | zero =>
    intro ns pre post par acc hn hf
    have := encForest_nonempty ns hn
    omega
  | succ fuel ih =>
    intro ns pre post par acc hn hf
    cases ns with
    | nil => contradiction
    | cons node rest =>
      cases node with
      | mk t p cs =>
        by_cases hc : cs = []
        · subst cs
          cases rest with
          | nil => simp [encForest, encNode, decodeAt, pathsForest, pathsNode]
          | cons next rest =>
            have bound : (encForest (next :: rest)).length ≤ fuel := by
              simpa [encForest, encNode] using hf
            have step := ih (next :: rest) (pre ++ [⟨p, t, 0⟩]) post par
              (acc ++ [(p, par ++ [t])]) (by simp) bound
            simpa [encForest, encNode, decodeAt, pathsForest, pathsNode,
              List.append_assoc] using step
        · have hempty : cs.isEmpty = false := by cases cs <;> simp_all
          cases rest with
          | nil =>
            have bound : (encForest cs).length ≤ fuel := by
              simpa [encForest, encNode, hempty] using hf
            have step := ih cs (pre ++ [⟨p, t, -1⟩]) post (par ++ [t])
              (acc ++ [(p, par ++ [t])]) hc bound
            simpa [encForest, encNode, decodeAt, pathsForest, pathsNode,
              hempty, List.append_assoc] using step
          | cons next rest =>
            let r : Row := ⟨p, t, (encForest cs).length + 1⟩
            have child_bound : (encForest cs).length ≤ fuel := by
              simp [encForest, encNode, hempty] at hf
              omega
            have sib_bound : (encForest (next :: rest)).length ≤ fuel := by
              simp only [encForest, encNode, List.length_append, List.length_cons] at hf ⊢
              omega
            have child := ih cs (pre ++ [r]) (encForest (next :: rest) ++ post)
              (par ++ [t]) (acc ++ [(p, par ++ [t])]) hc child_bound
            have sibling := ih (next :: rest) (pre ++ [r] ++ encForest cs) post par
              ((acc ++ [(p, par ++ [t])]) ++ pathsForest (par ++ [t]) cs)
              (by simp) sib_bound
            have positive : r.jump > 0 := by dsimp [r]; omega
            have jump_nat : r.jump.toNat = (encForest cs).length + 1 := by simp [r]
            have encoded : encForest (.mk t p cs :: next :: rest) =
                r :: (encForest cs ++ encForest (next :: rest)) := by
              simp [encForest, encNode, hempty, r]
            rw [encoded]
            simp only [List.cons_append, List.append_assoc]
            rw [decodeAt]
            simp only [row_at_prefix]
            have cond : ((decide (r.jump > 0) || r.jump == -1) &&
                decide (r.jump ≥ 0)) = true := by
              simp only [Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq,
                beq_iff_eq]
              exact ⟨Or.inl positive, by omega⟩
            simp only [cond, ↓reduceIte]
            have child' : decodeAt
                (pre ++ r :: (encForest cs ++ (encForest (next :: rest) ++ post)))
                fuel (pre.length + 1) (some (par ++ [t]))
                (acc ++ [(p, par ++ [t])]) =
                some ((acc ++ [(p, par ++ [t])]) ++ pathsForest (par ++ [t]) cs) := by
              simpa only [List.append_assoc, List.singleton_append, List.cons_append,
                List.length_append, List.length_singleton, List.nil_append] using child
            rw [child']
            simpa [jump_nat, List.append_assoc, List.cons_append, pathsForest, pathsNode,
              List.length_append, Nat.add_assoc, Nat.add_comm, Nat.add_left_comm] using sibling

/-- General tree round trip. Root tokens are ignored, as in the decoder; signed
element tokens and path indices are otherwise unrestricted by this theorem. -/
theorem decode_encode_of_le (root : Node) (fuel : Nat)
    (hf : (encode root).length ≤ fuel) : decode (encode root) fuel = some (paths root) := by
  cases root with
  | mk t p cs =>
    cases fuel with
    | zero => simp [encode, encNode] at hf
    | succ fuel =>
      by_cases hc : cs = []
      · subst cs
        simp [decode, encode, encNode, encForest, decodeAt, paths, pathsForest]
      · have hempty : cs.isEmpty = false := by cases cs <;> simp_all
        have bound : (encForest cs).length ≤ fuel := by
          simpa [encode, encNode, hempty] using hf
        have step := decodeAt_encForest fuel cs [⟨p, t, -1⟩] [] [] [(p, [])] hc bound
        simpa [decode, encode, encNode, encForest, decodeAt, paths, hempty] using step

theorem decode_encode (root : Node) :
    decode (encode root) (encode root).length = some (paths root) :=
  decode_encode_of_le root _ (Nat.le_refl _)

theorem decodeAt_chain_insufficient (n : Nat) :
    ∀ (fuel : Nat) (pre post : List Row) (par : Option Path) (acc : List (Nat × Path)),
    fuel ≤ n → decodeAt (pre ++ encode (chain n) ++ post) fuel pre.length par acc = none := by
  induction n with
  | zero =>
    intro fuel pre post par acc hf
    have hzero : fuel = 0 := by omega
    subst fuel
    rfl
  | succ n ih =>
    intro fuel pre post par acc hf
    cases fuel with
    | zero => rfl
    | succ fuel =>
      let path : Path := match par with | none => [] | some p => p ++ [(n : Int) + 2]
      have step := ih fuel (pre ++ [⟨n + 1, n + 2, -1⟩]) post (some path)
        (acc ++ [(n + 1, path)]) (by omega)
      cases par <;>
        simpa [chain, encode, encForest, encNode, decodeAt, path, List.append_assoc] using step

theorem chain_fuel_insufficient (n fuel : Nat) (hf : fuel ≤ n) :
    decode (encode (chain n)) fuel = none := by
  simpa [decode] using decodeAt_chain_insufficient n fuel [] [] none [] hf

mutual
theorem pathsNode_indices : ∀ (n : Node) (par : Path) (sib : Bool),
    (pathsNode par n).map Prod.fst = (encNode n sib).map Row.pathIndex
  | .mk t p cs, par, sib => by
    simp only [pathsNode, encNode, List.map_cons, pathsForest_indices]
theorem pathsForest_indices : ∀ (ns : List Node) (par : Path),
    (pathsForest par ns).map Prod.fst = (encForest ns).map Row.pathIndex
  | [], _ => rfl
  | n :: rest, par => by
    simp only [pathsForest, encForest, List.map_append,
      pathsNode_indices n par (!rest.isEmpty), pathsForest_indices]
end

theorem paths_indices (root : Node) :
    (paths root).map Prod.fst = (encode root).map Row.pathIndex := by
  cases root
  simp only [paths, encode, encNode, List.map_cons, pathsForest_indices]

/-- Sparse indices are preserved verbatim and in encoded order: there is no
contiguous-index assumption. This is an event-list result, not a validation of
binary table bounds, uniqueness, or the separately encoded total path count. -/
theorem decode_encode_indices (root : Node) (fuel : Nat)
    (hf : (encode root).length ≤ fuel) :
    (decode (encode root) fuel).map (List.map Prod.fst) =
      some ((encode root).map Row.pathIndex) := by
  rw [decode_encode_of_le root fuel hf]
  simp only [Option.map_some, paths_indices]

theorem encode_chain_length (n : Nat) : (encode (chain n)).length = n + 1 := by
  induction n with
  | zero => rfl
  | succ n ih => simpa [chain, encode, encNode, encForest] using congrArg Nat.succ ih

/-- The row-count fuel upper bound is tight for chains of every length. -/
theorem chain_fuel_minimal (n fuel : Nat) (result : List (Nat × Path))
    (h : decode (encode (chain n)) fuel = some result) : n + 1 ≤ fuel := by
  by_contra bad
  have insufficient := chain_fuel_insufficient n fuel (by omega)
  rw [insufficient] at h
  contradiction

end UsdSpec.Crate.Paths
