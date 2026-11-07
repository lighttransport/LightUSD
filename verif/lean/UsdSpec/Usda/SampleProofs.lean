import UsdSpec.Usda.OffsetProofs
/-!
# USDA sample-map and attribute-body round-trip proofs

SPEC: core_spec.md L10187-10195, L10454-10471.
Extends the existing operational subset without changing executable parsers or
printers. Sample maps of arbitrary length recover exact lexeme pairs and their
authored order. Local character-count fuel is sufficient. Default-value and
.timeSamples attribute statements advance the actual primItems parser, and
attribute-only bodies of arbitrary length recover their complete item lists.

The explicit admissibility predicates concern the subset's identifier/numeric
scanners. They do not assert full XID/Number PEG validity, finite values, sorted
or unique timestamps, or interpolation/value-resolution semantics. Nested prims,
metadata/dictionaries and whole-layer recovery are proved in PrimProofs,
RecursiveMetadataProofs and LayerProofs; composition is not claimed here.
-/
namespace UsdSpec.Usda

def SampleWellFormed (s : Num × Num) : Prop := NumLexeme s.1 ∧ NumLexeme s.2

theorem num_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    num (padding ++ cs) = num cs := by
  unfold num
  have strip : ws (padding ++ cs) = ws cs := dropWhile_prefix isWs padding cs pad
  rw [strip]

theorem sample_time_num (n : Nat) (t v : Num) (rest : List Char) (valid : NumLexeme t) :
    num ('\n' :: (ind (n + 1) ++ t ++ ": " ++ v).toList ++ ',' :: rest) =
      some (t, ':' :: ' ' :: (v.toList ++ ',' :: rest)) := by
  have strip := num_padding ('\n' :: (ind (n + 1)).toList)
    (t.toList ++ ':' :: ' ' :: (v.toList ++ ',' :: rest)) (newline_ind_ws (n + 1))
  have number := num_print t ':' (' ' :: (v.toList ++ ',' :: rest)) valid (by decide)
  simpa only [String.toList_append, show ": ".toList = [':', ' '] from rfl,
    List.cons_append, List.nil_append, List.append_assoc] using strip.trans number

theorem sample_peek_not_close (n : Nat) (t v : Num) (rest : List Char) (valid : NumLexeme t) :
    peek ('\n' :: (ind (n + 1) ++ t ++ ": " ++ v).toList ++ ',' :: rest) ≠ some '}' := by
  rcases valid with ⟨c, cs, spell, head, cont⟩
  have first := num_first_not_ws c head
  have not_close : c ≠ '}' := by intro h; subst c; simp [isNumChar] at head
  simpa only [String.toList_append, List.cons_append, List.append_assoc,
    peek, ws_newline, ws_ind, spell, List.cons_append, ws_cons _ _ first,
    List.head?_cons, ne_eq, Option.some.injEq] using not_close

theorem samples_print (n : Nat) (xs acc : List (Num × Num)) (rest : List Char) (fuel : Nat)
    (valid : ∀ s ∈ xs, SampleWellFormed s) (enough : xs.length + 1 ≤ fuel) :
    samples fuel acc ('\n' :: commaBody (fun s => (ind (n + 1) ++ s.1 ++ ": " ++ s.2).toList)
      xs ((ind n).toList ++ '}' :: rest)) = some (acc.reverse ++ xs, rest) := by
  revert valid
  induction xs generalizing acc fuel with
  | nil =>
    intro valid
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have strip : ws ('\n' :: ((ind n).toList ++ '}' :: rest)) = '}' :: rest := by
        rw [ws_newline, ws_ind]
        rfl
      simp [commaBody, samples, peek, lit, strip, show "}".length = 1 from rfl]
  | cons s xs ih =>
    intro valid
    have vh := valid s (by simp)
    have vt : ∀ p ∈ xs, SampleWellFormed p := fun p hp => valid p (by simp [hp])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      let tail := '\n' :: commaBody (fun s : Num × Num => (ind (n + 1) ++ s.1 ++ ": " ++ s.2).toList)
        xs ((ind n).toList ++ '}' :: rest)
      have no_close := sample_peek_not_close n s.1 s.2 tail vh.1
      have time_value := sample_time_num n s.1 s.2 tail vh.1
      simp only [List.cons_append] at no_close time_value
      have sample_value := num_print s.2 ',' tail vh.2 (by decide)
      simp only [commaBody]
      rw [samples]
      simp only [beq_iff_eq]
      rw [ite_eq_right (by exact no_close), time_value]
      change (do
        let (v, r) ← num (' ' :: (s.2.toList ++ ',' :: tail))
        samples fuel ((s.1, v) :: acc) (if peek r = some ',' then (ws r).drop 1 else r)) = _
      rw [num_space, sample_value]
      change samples fuel (s :: acc) tail = _
      have step := ih (s :: acc) fuel (by simpa using enough) vt
      simpa [tail, List.reverse_cons, List.append_assoc] using step

theorem sample_rows_chars (n : Nat) (xs : List (Num × Num)) (rest : List Char) :
    (String.join (xs.map (fun s => ind (n + 1) ++ s.1 ++ ": " ++ s.2 ++ ",\n")) ++ ind n ++ "}").toList ++ rest =
    commaBody (fun s => (ind (n + 1) ++ s.1 ++ ": " ++ s.2).toList) xs ((ind n).toList ++ '}' :: rest) := by
  simp [commaBody_flatMap, String.toList_join, List.flatMap_map,
    String.toList_append, List.append_assoc,
    show ",\n".toList = [',', '\n'] from rfl, show "}".toList = ['}'] from rfl]

theorem samples_print_text (n : Nat) (xs : List (Num × Num)) (rest : List Char)
    (valid : ∀ s ∈ xs, SampleWellFormed s) :
    let text := String.join (xs.map (fun s => ind (n + 1) ++ s.1 ++ ": " ++ s.2 ++ ",\n")) ++ ind n ++ "}"
    let input := '\n' :: text.toList ++ rest
    samples (input.length + 1) [] input = some (xs, rest) := by
  dsimp only
  simp only [List.cons_append]
  rw [sample_rows_chars]
  let emit := fun s : Num × Num => (ind (n + 1) ++ s.1 ++ ": " ++ s.2).toList
  let tail := (ind n).toList ++ '}' :: rest
  have bound := commaBody_length_ge emit xs tail
  have enough : xs.length + 1 ≤ ('\n' :: commaBody emit xs tail).length + 1 := by
    simp only [List.length_cons]
    omega
  simpa only [List.reverse_nil, List.nil_append] using
    samples_print n xs [] rest (('\n' :: commaBody emit xs tail).length + 1) valid enough

theorem attrRest_value (name value : String) (rest : List Char)
    (hn : IdentLexeme name) (hv : NumLexeme value) :
    attrRest (' ' :: name.toList ++ " = ".toList ++ value.toList ++ '\n' :: rest) =
      some ((name, Sum.inl value), '\n' :: rest) := by
  have identifier := ident_print name ' ' ("= ".toList ++ value.toList ++ '\n' :: rest) hn (by decide)
  have number := num_print value '\n' rest hv (by decide)
  simp only [List.append_assoc, List.cons_append] at *
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    show "= ".toList = ['=', ' '] from rfl] at *
  simp only [List.cons_append, List.nil_append] at *
  unfold attrRest
  rw [ident_space, identifier]
  change (do
    let (v, r) ← num (' ' :: (value.toList ++ '\n' :: rest))
    some ((name, Sum.inl v), r)) = _
  rw [num_space, number]
  rfl

theorem attrRest_samples (n : Nat) (name : String) (xs : List (Num × Num)) (rest : List Char)
    (hn : IdentLexeme name) (valid : ∀ s ∈ xs, SampleWellFormed s) :
    let text := String.join (xs.map (fun s => ind (n + 1) ++ s.1 ++ ": " ++ s.2 ++ ",\n")) ++ ind n ++ "}"
    attrRest (' ' :: name.toList ++ ".timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest) =
      some ((name, Sum.inr xs), '\n' :: rest) := by
  dsimp only
  let text := String.join (xs.map (fun s => ind (n + 1) ++ s.1 ++ ": " ++ s.2 ++ ",\n")) ++ ind n ++ "}"
  have identifier := ident_print name '.' ("timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest) hn (by decide)
  have parsed := samples_print_text n xs ('\n' :: rest) valid
  simp only [List.append_assoc, List.cons_append,
    show ".timeSamples = {\n".toList = '.' :: "timeSamples = {\n".toList from rfl] at *
  unfold attrRest
  rw [ident_space, identifier]
  change (samples (('\n' :: text.toList ++ '\n' :: rest).length + 1) []
    ('\n' :: text.toList ++ '\n' :: rest)).bind
    (fun result => some ((name, Sum.inr result.1), result.2)) = _
  simp only [List.cons_append]
  rw [parsed]
  rfl

theorem ident_double (n : Nat) (rest : List Char) :
    ident ((ind n ++ "double ").toList ++ rest) = some ("double", ' ' :: rest) := by
  simp only [String.toList_append, List.append_assoc, ident, ws_ind]
  rfl

theorem peek_double (n : Nat) (rest : List Char) :
    peek ((ind n ++ "double ").toList ++ rest) = some 'd' := by
  simp only [String.toList_append, List.append_assoc, peek, ws_ind]
  rfl

def AttrWellFormed (a : String × Sum Num (List (Num × Num))) : Prop :=
  IdentLexeme a.1 ∧ match a.2 with
    | Sum.inl value => NumLexeme value
    | Sum.inr xs => ∀ s ∈ xs, SampleWellFormed s

theorem ident_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    ident (padding ++ cs) = ident cs := by
  unfold ident
  have strip : ws (padding ++ cs) = ws cs := dropWhile_prefix isWs padding cs pad
  rw [strip]

theorem peek_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    peek (padding ++ cs) = peek cs := by
  unfold peek
  have strip : ws (padding ++ cs) = ws cs := dropWhile_prefix isWs padding cs pad
  rw [strip]

theorem primItems_printAttr (n fuel : Nat)
    (a : String × Sum Num (List (Num × Num)))
    (acc : List (Sum (String × Sum Num (List (Num × Num))) Prim)) (rest padding : List Char)
    (valid : AttrWellFormed a) (pad : ∀ c ∈ padding, isWs c = true) :
    primItems (fuel + 1) acc (padding ++ (printAttr n a).toList ++ rest) =
      primItems fuel (Sum.inl a :: acc) ('\n' :: rest) := by
  rcases a with ⟨name, value⟩
  cases value with
  | inl value =>
    have parsed := attrRest_value name value rest valid.1 valid.2
    simp only [List.cons_append, List.append_assoc] at parsed
    have header := ident_double n (name.toList ++ " = ".toList ++ value.toList ++ '\n' :: rest)
    have peek_header := peek_double n (name.toList ++ " = ".toList ++ value.toList ++ '\n' :: rest)
    have frame : (printAttr n (name, Sum.inl value)).toList ++ rest =
        (ind n ++ "double ").toList ++ (name.toList ++ " = ".toList ++ value.toList ++ '\n' :: rest) := by
      simp [printAttr, String.toList_append, List.append_assoc]
    simp only [List.append_assoc]
    rw [frame, primItems]
    dsimp only
    rw [peek_padding padding _ pad]
    simp only [peek_header, show (some 'd' == some '}') = false from rfl, Bool.false_eq_true, ↓reduceIte]
    rw [ident_padding padding _ pad, header]
    change (attrRest (' ' :: name.toList ++ " = ".toList ++ value.toList ++ '\n' :: rest)).bind
      (fun result => primItems fuel (Sum.inl result.1 :: acc) result.2) = _
    simp only [List.cons_append, List.append_assoc]
    rw [parsed]
    rfl
  | inr xs =>
    let text := String.join (xs.map (fun s => ind (n + 1) ++ s.1 ++ ": " ++ s.2 ++ ",\n")) ++ ind n ++ "}"
    have parsed := attrRest_samples n name xs rest valid.1 valid.2
    simp only [List.cons_append, List.append_assoc] at parsed
    have header := ident_double n (name.toList ++ ".timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest)
    have peek_header := peek_double n (name.toList ++ ".timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest)
    have frame : (printAttr n (name, Sum.inr xs)).toList ++ rest =
        (ind n ++ "double ").toList ++ (name.toList ++ ".timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest) := by
      simp [printAttr, text, String.toList_append, List.append_assoc]
    simp only [List.append_assoc]
    rw [frame, primItems]
    dsimp only
    rw [peek_padding padding _ pad]
    simp only [peek_header, show (some 'd' == some '}') = false from rfl, Bool.false_eq_true, ↓reduceIte]
    rw [ident_padding padding _ pad, header]
    change (attrRest (' ' :: name.toList ++ ".timeSamples = {\n".toList ++ text.toList ++ '\n' :: rest)).bind
      (fun result => primItems fuel (Sum.inl result.1 :: acc) result.2) = _
    simp only [List.cons_append, List.append_assoc]
    rw [parsed]
    rfl

theorem primItems_attrs (n : Nat) (xs : List (String × Sum Num (List (Num × Num))))
    (acc : List (Sum (String × Sum Num (List (Num × Num))) Prim))
    (padding closing rest : List Char) (fuel : Nat)
    (valid : ∀ a ∈ xs, AttrWellFormed a)
    (pad : ∀ c ∈ padding, isWs c = true) (close_ws : ∀ c ∈ closing, isWs c = true)
    (enough : xs.length + 1 ≤ fuel) :
    primItems fuel acc (padding ++ (Prim.printItems n (xs.map Sum.inl)).toList ++ closing ++ '}' :: rest) =
      some (acc.reverse ++ xs.map Sum.inl, rest) := by
  revert valid pad
  induction xs generalizing acc padding fuel with
  | nil =>
    intro valid pad
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have both : ∀ c ∈ padding ++ closing, isWs c = true := by
        intro c hc
        rcases List.mem_append.mp hc with hp | hc
        · exact pad c hp
        · exact close_ws c hc
      have strip : ws (padding ++ closing ++ '}' :: rest) = '}' :: rest := by
        unfold ws
        rw [dropWhile_prefix isWs (padding ++ closing) ('}' :: rest) both]
        rfl
      simp only [List.append_assoc] at strip
      simp [Prim.printItems, primItems, peek, lit, strip, List.append_assoc,
        show "}".length = 1 from rfl]
  | cons a xs ih =>
    intro valid pad
    have vh := valid a (by simp)
    have vt : ∀ b ∈ xs, AttrWellFormed b := fun b hb => valid b (by simp [hb])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      simp only [List.map_cons, Prim.printItems, String.toList_append, List.append_assoc]
      have advance := primItems_printAttr n fuel a acc
        ((Prim.printItems n (xs.map Sum.inl)).toList ++ closing ++ '}' :: rest) padding vh pad
      simp only [List.append_assoc] at advance
      rw [advance]
      have next := ih (Sum.inl a :: acc) ['\n'] fuel (by simpa using enough) vt
        (by intro c hc; simp only [List.mem_singleton] at hc; subst c; rfl)
      simpa [List.reverse_cons, List.append_assoc] using next
end UsdSpec.Usda
