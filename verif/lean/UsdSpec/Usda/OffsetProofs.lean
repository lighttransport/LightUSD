import UsdSpec.Usda.GrammarProofs
/-!
# USDA offset, reference and sublayer round-trip proofs

SPEC: core_spec.md L10057, L10849-10861, L11176-11183.
Extends GrammarProofs.lean without changing executable parsers or printers.
The four offset states, three nonempty optional-reference states, and arbitrary
reference/sublayer lists recover exact fields and following input. Character-
count fuel is proved sufficient rather than assumed by a parser-result premise.

WellFormed here means printer/parser admissibility in the existing subset:
number scanner lexemes, delimiter exclusion, and at least one reference field.
It does not assert semantic numeric conversion, finite/nonzero scale, URI
validity, or USD path validity. Those are separate validation obligations.
The whole-AST round trip, recursive prim/dictionary and sampled-value cases
are proved in LayerProofs, PrimProofs, RecursiveMetadataProofs and SampleProofs;
these are the reusable component proofs.
-/
namespace UsdSpec.Usda

@[simp] theorem num_space (rest : List Char) : num (' ' :: rest) = num rest := by rfl
@[simp] theorem ident_space (rest : List Char) : ident (' ' :: rest) = ident rest := by rfl
@[simp] theorem lit_space (s : String) (rest : List Char) : lit s (' ' :: rest) = lit s rest := by rfl

def Offset.WellFormed (o : Offset) : Prop :=
  (∀ n ∈ o.offset.toList, NumLexeme n) ∧ (∀ n ∈ o.scale.toList, NumLexeme n)

theorem offsetItems_offset (f : Nat) (o : Offset) (v : String) (rest : List Char)
    (valid : NumLexeme v) :
    offsetItems (f + 2) o ("offset = ".toList ++ v.toList ++ ')' :: rest) =
      some ({o with offset := some v}, rest) := by
  have number := num_print v ')' rest valid (by decide)
  change (do
    let (n, r) ← num (' ' :: (v.toList ++ ')' :: rest))
    offsetItems (f + 1) {o with offset := some n} r) = _
  rw [num_space, number]
  rfl

theorem offsetItems_scale (f : Nat) (o : Offset) (v : String) (rest : List Char)
    (valid : NumLexeme v) :
    offsetItems (f + 2) o ("scale = ".toList ++ v.toList ++ ')' :: rest) =
      some ({o with scale := some v}, rest) := by
  have number := num_print v ')' rest valid (by decide)
  change (do
    let (n, r) ← num (' ' :: (v.toList ++ ')' :: rest))
    offsetItems (f + 1) {o with scale := some n} r) = _
  rw [num_space, number]
  rfl

theorem offsetItems_both (f : Nat) (o : Offset) (a b : String) (rest : List Char)
    (ha : NumLexeme a) (hb : NumLexeme b) :
    offsetItems (f + 3) o ("offset = ".toList ++ a.toList ++ "; scale = ".toList ++ b.toList ++ ')' :: rest) =
      some ({offset := some a, scale := some b}, rest) := by
  have number := num_print a ';' (" scale = ".toList ++ b.toList ++ ')' :: rest) ha (by decide)
  change (do
    let (n, r) ← num (' ' :: (a.toList ++ ';' :: " scale = ".toList ++ b.toList ++ ')' :: rest))
    offsetItems (f + 2) {o with offset := some n} r) = _
  rw [num_space]
  simp only [List.append_assoc, List.cons_append] at *
  rw [number]
  change offsetItems (f + 2) {o with offset := some a}
    ("scale = ".toList ++ b.toList ++ ')' :: rest) = _
  simpa using offsetItems_scale f {o with offset := some a} b rest hb

theorem optOffset_offset (v : String) (rest : List Char) (valid : NumLexeme v) :
    optOffset (" (offset = ".toList ++ v.toList ++ ')' :: rest) =
      some ({offset := some v}, rest) := by
  let fuel := (" (offset = ".toList ++ v.toList ++ ')' :: rest).length + 1
  have enough : 2 ≤ fuel := by simp [fuel]
  have fuel_eq : fuel = (fuel - 2) + 2 := by omega
  change offsetItems fuel {} ("offset = ".toList ++ v.toList ++ ')' :: rest) = _
  rw [fuel_eq]
  exact offsetItems_offset (fuel - 2) {} v rest valid

theorem optOffset_scale (v : String) (rest : List Char) (valid : NumLexeme v) :
    optOffset (" (scale = ".toList ++ v.toList ++ ')' :: rest) =
      some ({scale := some v}, rest) := by
  let fuel := (" (scale = ".toList ++ v.toList ++ ')' :: rest).length + 1
  have enough : 2 ≤ fuel := by simp [fuel]
  have fuel_eq : fuel = (fuel - 2) + 2 := by omega
  change offsetItems fuel {} ("scale = ".toList ++ v.toList ++ ')' :: rest) = _
  rw [fuel_eq]
  exact offsetItems_scale (fuel - 2) {} v rest valid

theorem optOffset_both (a b : String) (rest : List Char)
    (ha : NumLexeme a) (hb : NumLexeme b) :
    optOffset (" (offset = ".toList ++ a.toList ++ "; scale = ".toList ++ b.toList ++ ')' :: rest) =
      some ({offset := some a, scale := some b}, rest) := by
  let fuel := (" (offset = ".toList ++ a.toList ++ "; scale = ".toList ++ b.toList ++ ')' :: rest).length + 1
  have enough : 3 ≤ fuel := by simp [fuel]
  have fuel_eq : fuel = (fuel - 3) + 3 := by omega
  change offsetItems fuel {} ("offset = ".toList ++ a.toList ++ "; scale = ".toList ++ b.toList ++ ')' :: rest) = _
  rw [fuel_eq]
  exact offsetItems_both (fuel - 3) {} a b rest ha hb

theorem optOffset_print (o : Offset) (rest : List Char) (valid : o.WellFormed) :
    optOffset (o.print.toList ++ ',' :: rest) = some (o, ',' :: rest) := by
  rcases o with ⟨offset, scale⟩
  cases offset with
  | none =>
    cases scale with
    | none => rfl
    | some s =>
      simp [Offset.WellFormed] at valid
      simpa [Offset.print, String.toList_append, List.append_assoc] using
        optOffset_scale s (',' :: rest) valid
  | some o =>
    cases scale with
    | none =>
      simp [Offset.WellFormed] at valid
      simpa [Offset.print, String.toList_append, List.append_assoc] using
        optOffset_offset o (',' :: rest) valid
    | some s =>
      simp [Offset.WellFormed] at valid
      simpa [Offset.print, String.toList_append, List.append_assoc] using
        optOffset_both o s (',' :: rest) valid.1 valid.2

def SubLayer.WellFormed (s : SubLayer) : Prop :=
  (∀ c ∈ s.asset.toList, c ≠ '@') ∧ s.off.WellFormed

theorem subLayer_print (s : SubLayer) (rest : List Char) (valid : s.WellFormed) :
    subLayer (s.print.toList ++ ',' :: rest) = some (s, ',' :: rest) := by
  have asset_value := asset_print s.asset (s.off.print.toList ++ ',' :: rest) valid.1
  simp only [List.cons_append] at asset_value
  simp only [SubLayer.print, String.toList_append, List.append_assoc,
    show "@".toList = ['@'] from rfl, List.cons_append, List.nil_append, subLayer]
  rw [asset_value]
  change (optOffset (s.off.print.toList ++ ',' :: rest)).bind
    (fun result => some (SubLayer.mk s.asset result.1, result.2)) = some (s, ',' :: rest)
  rw [optOffset_print s.off rest valid.2]
  cases s
  rfl

def Reference.WellFormed (r : Reference) : Prop :=
  (r.asset.isSome = true ∨ r.prim.isSome = true) ∧
  (∀ a ∈ r.asset.toList, ∀ c ∈ a.toList, c ≠ '@') ∧
  (∀ p ∈ r.prim.toList, ∀ c ∈ p.toList, c ≠ '>') ∧ r.off.WellFormed

theorem pathRef_offset_none (o : Offset) (rest : List Char) :
    pathRef (o.print.toList ++ ',' :: rest) = none := by
  cases o with
  | mk a b => cases a <;> cases b <;>
    simp [Offset.print, String.toList_append, List.append_assoc,
      pathRef, delimited, ws, isWs]

theorem reference_print (r : Reference) (rest : List Char) (valid : r.WellFormed) :
    reference (r.print.toList ++ ',' :: rest) = some (r, ',' :: rest) := by
  rcases r with ⟨asset, prim, off⟩
  cases asset with
  | none =>
    cases prim with
    | none => simp [Reference.WellFormed] at valid
    | some p =>
      simp [Reference.WellFormed] at valid
      have path_value := pathRef_print p (off.print.toList ++ ',' :: rest) valid.1
      simp only [List.cons_append] at path_value
      simp only [Reference.print, Option.map_none, Option.getD_none, Option.map_some,
        Option.getD_some, String.empty_append, String.toList_append,
        show "<".toList = ['<'] from rfl, show ">".toList = ['>'] from rfl,
        List.cons_append, List.nil_append, List.append_assoc, reference]
      have no_asset : UsdSpec.Usda.asset ('<' :: (p.toList ++ '>' :: (off.print.toList ++ ',' :: rest))) = none := by rfl
      rw [no_asset, path_value]
      simp only [Option.isNone_some, Bool.and_false, Bool.false_eq_true, ↓reduceIte]
      rw [optOffset_print off rest valid.2]
      rfl
  | some a =>
    cases prim with
    | none =>
      simp [Reference.WellFormed] at valid
      have asset_value := asset_print a (off.print.toList ++ ',' :: rest) valid.1
      simp only [List.cons_append] at asset_value
      simp only [Reference.print, Option.map_none, Option.getD_none, Option.map_some,
        Option.getD_some, String.append_empty, String.toList_append,
        show "@".toList = ['@'] from rfl, List.cons_append, List.nil_append,
        List.append_assoc, reference]
      rw [asset_value, pathRef_offset_none]
      simp only [Option.isNone_some, Bool.false_and, Bool.false_eq_true, ↓reduceIte]
      rw [optOffset_print off rest valid.2]
      rfl
    | some p =>
      simp [Reference.WellFormed] at valid
      have path_value := pathRef_print p (off.print.toList ++ ',' :: rest) valid.2.1
      have asset_value := asset_print a ('<' :: p.toList ++ '>' :: off.print.toList ++ ',' :: rest) valid.1
      simp only [List.cons_append, List.append_assoc] at asset_value path_value
      simp only [Reference.print, Option.map_some, Option.getD_some,
        String.toList_append, show "@".toList = ['@'] from rfl,
        show "<".toList = ['<'] from rfl, show ">".toList = ['>'] from rfl,
        List.cons_append, List.nil_append, List.append_assoc, reference]
      rw [asset_value, path_value]
      simp only [Option.isNone_some, Bool.false_and, Bool.false_eq_true, ↓reduceIte]
      rw [optOffset_print off rest valid.2.2]
      rfl

@[simp] theorem ws_idempotent (cs : List Char) : ws (ws cs) = ws cs := by
  induction cs with
  | nil => rfl
  | cons c cs ih => cases h : isWs c <;> simp_all [ws]

@[simp] theorem asset_ws (cs : List Char) : asset (ws cs) = asset cs := by
  simp only [asset, delimited, ws_idempotent]

@[simp] theorem pathRef_ws (cs : List Char) : pathRef (ws cs) = pathRef cs := by
  simp only [pathRef, delimited, ws_idempotent]

theorem subLayer_ws (cs : List Char) : subLayer (ws cs) = subLayer cs := by
  simp only [subLayer, asset_ws]

theorem reference_ws (cs : List Char) : reference (ws cs) = reference cs := by
  unfold reference
  rw [asset_ws]
  cases h : asset cs with
  | some pair => rfl
  | none =>
    simp only [pathRef_ws]
    cases hp : pathRef cs <;> rfl

theorem reference_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    reference (padding ++ cs) = reference cs := by
  rw [← reference_ws (padding ++ cs)]
  have strip : ws (padding ++ cs) = ws cs := dropWhile_prefix isWs padding cs pad
  rw [strip, reference_ws]

theorem subLayer_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    subLayer (padding ++ cs) = subLayer cs := by
  rw [← subLayer_ws (padding ++ cs)]
  have strip : ws (padding ++ cs) = ws cs := dropWhile_prefix isWs padding cs pad
  rw [strip, subLayer_ws]

theorem newline_ind_ws (n : Nat) : ∀ c ∈ '\n' :: (ind n).toList, isWs c = true := by
  intro c hc
  simp only [List.mem_cons, ind, String.toList_ofList, List.mem_replicate] at hc
  rcases hc with rfl | ⟨_, rfl⟩ <;> rfl

theorem subLayer_print_start (s : SubLayer) :
    ∃ c cs, ws s.print.toList = c :: cs ∧ c ≠ ']' := by
  refine ⟨'@', s.print.toList.tail, ?_, by decide⟩
  simp [SubLayer.print, String.toList_append, show "@".toList = ['@'] from rfl, ws, isWs]

theorem reference_print_start (r : Reference) (valid : r.WellFormed) :
    ∃ c cs, ws r.print.toList = c :: cs ∧ c ≠ ']' := by
  rcases r with ⟨a, p, o⟩
  cases a with
  | some a =>
    refine ⟨'@', (Reference.print ⟨some a, p, o⟩).toList.tail, ?_, by decide⟩
    simp [Reference.print, String.toList_append, show "@".toList = ['@'] from rfl, ws, isWs]
  | none =>
    cases p with
    | none => simp [Reference.WellFormed] at valid
    | some p =>
      refine ⟨'<', (Reference.print ⟨none, some p, o⟩).toList.tail, ?_, by decide⟩
      simp [Reference.print, String.toList_append, show "<".toList = ['<'] from rfl, ws, isWs]

theorem subLayer_bracket (n : Nat) (xs : List SubLayer) (rest : List Char)
    (valid : ∀ s ∈ xs, s.WellFormed) :
    listOrOne subLayer ((bracket n (xs.map SubLayer.print)).toList ++ rest) = some (xs, rest) := by
  apply listOrOne_bracket subLayer SubLayer.print SubLayer.WellFormed n
  · intro s valid rest
    have strip := subLayer_padding ('\n' :: (ind (n + 1)).toList)
      (s.print.toList ++ ',' :: rest) (newline_ind_ws (n + 1))
    simpa only [String.toList_append, List.cons_append, List.append_assoc] using
      strip.trans (subLayer_print s rest valid)
  · intro s valid
    simpa only [String.toList_append, ws_ind] using subLayer_print_start s
  · exact valid

theorem reference_bracket (n : Nat) (xs : List Reference) (rest : List Char)
    (valid : ∀ r ∈ xs, r.WellFormed) :
    listOrOne reference ((bracket n (xs.map Reference.print)).toList ++ rest) = some (xs, rest) := by
  apply listOrOne_bracket reference Reference.print Reference.WellFormed n
  · intro r valid rest
    have strip := reference_padding ('\n' :: (ind (n + 1)).toList)
      (r.print.toList ++ ',' :: rest) (newline_ind_ws (n + 1))
    simpa only [String.toList_append, List.cons_append, List.append_assoc] using
      strip.trans (reference_print r rest valid)
  · intro r valid
    simpa only [String.toList_append, ws_ind] using reference_print_start r valid
  · exact valid
end UsdSpec.Usda
