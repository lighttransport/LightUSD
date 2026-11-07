import UsdSpec.Usda.SampleProofs
/-! Whitespace-aware list inverses used by metadata assembly. These lemmas
retain the item parser's lexical admissibility hypotheses; they do not establish
semantic arc validity or a complete metadata / layer AST inverse. Arc statement
and arbitrary-length arc metadata block inverses are proved below; customData
dictionaries remain outside ArcWellFormed; they are covered by
RecursiveMetadataProofs. -/
namespace UsdSpec.Usda

theorem listOrOne_bracket_padding {α : Type} (item : P α) (print : α → String)
    (Valid : α → Prop) (n : Nat)
    (parse_item : ∀ x, Valid x → ∀ rest,
      item ('\n' :: (ind (n + 1) ++ print x).toList ++ ',' :: rest) = some (x, ',' :: rest))
    (starts_value : ∀ x, Valid x → ∃ c cs,
      ws ((ind (n + 1) ++ print x).toList) = c :: cs ∧ c ≠ ']')
    (xs : List α) (padding rest : List Char)
    (pad_ws : ∀ c ∈ padding, isWs c = true) (valid : ∀ x ∈ xs, Valid x) :
    listOrOne item (padding ++ (bracket n (xs.map print)).toList ++ rest) = some (xs, rest) := by
  by_cases empty : xs = []
  · subst xs
    have strip : ws (padding ++ '[' :: ']' :: rest) = '[' :: ']' :: rest := by
      unfold ws
      rw [dropWhile_prefix isWs padding ('[' :: ']' :: rest) pad_ws]
      rfl
    simp only [List.map_nil, bracket, List.isEmpty_nil, ite_true, show "[]".toList = ['[', ']'] from rfl, List.cons_append, List.nil_append, List.append_assoc]
    change listOrOne item (padding ++ '[' :: ']' :: rest) = some ([], rest)
    simp only [listOrOne, ident, peek, lit, strip]
    simp [ws, isWs, isIdStart, listUntil, peek, lit,
      show "[".toList = ['['] from rfl, show "]".toList = [']'] from rfl,
      show "[".length = 1 from rfl, show "]".length = 1 from rfl]
  · simp only [List.append_assoc]
    rw [bracket_chars print n xs rest empty]
    let emit := fun x => (ind (n + 1) ++ print x).toList
    let closing := (ind n).toList
    let tail := closing ++ ']' :: rest
    have strip : ws (padding ++ '[' :: '\n' :: commaBody emit xs tail) =
        '[' :: '\n' :: commaBody emit xs tail := by
      unfold ws
      rw [dropWhile_prefix isWs padding _ pad_ws]
      rfl
    dsimp only [emit, tail, closing] at strip
    simp only [listOrOne, ident, peek, lit, strip]
    simp only [show isIdStart '[' = false from rfl,
      show "[".toList = ['['] from rfl, show "[".length = 1 from rfl]
    change listUntil item ((padding ++ '[' :: '\n' :: commaBody emit xs tail).length + 1) []
      ('\n' :: commaBody emit xs tail) = some (xs, rest)
    have close_ws : ∀ c ∈ closing, isWs c = true := by
      intro c hc
      simp only [closing, ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    have length_bound := commaBody_length_ge emit xs tail
    have result := listUntil_print item emit Valid parse_item starts_value xs [] closing rest
      ((padding ++ '[' :: '\n' :: commaBody emit xs tail).length + 1) close_ws
      (by simp only [List.length_append, List.length_cons]; omega) valid
    simpa only [List.reverse_nil, List.nil_append] using result
theorem pathRef_bracket_padding (n : Nat) (xs : List String) (padding rest : List Char)
    (pad_ws : ∀ c ∈ padding, isWs c = true)
    (safe : ∀ s ∈ xs, ∀ c ∈ s.toList, c ≠ '>') :
    listOrOne pathRef (padding ++ (bracket n (xs.map (fun s => "<" ++ s ++ ">"))).toList ++ rest) =
      some (xs, rest) := by
  apply listOrOne_bracket_padding pathRef (fun s => "<" ++ s ++ ">")
    (fun s => ∀ c ∈ s.toList, c ≠ '>') n
  · intro s valid rest
    simpa only [pathRef, delimited, String.toList_append,
      show "<".toList = ['<'] from rfl, show ">".toList = ['>'] from rfl,
      List.cons_append, List.nil_append, List.append_assoc, ws_newline, ws_ind]
      using pathRef_print s (',' :: rest) valid
  · intro s valid
    refine ⟨'<', s.toList ++ ['>'], ?_, by decide⟩
    simp [String.toList_append, ws_ind, ws_cons,
      show "<".toList = ['<'] from rfl, show ">".toList = ['>'] from rfl,
      show isWs '<' = false from rfl]
  · exact pad_ws
  · exact safe
theorem reference_bracket_padding (n : Nat) (xs : List Reference) (padding rest : List Char)
    (pad_ws : ∀ c ∈ padding, isWs c = true)
    (valid : ∀ r ∈ xs, r.WellFormed) :
    listOrOne reference (padding ++ (bracket n (xs.map Reference.print)).toList ++ rest) = some (xs, rest) := by
  apply listOrOne_bracket_padding reference Reference.print Reference.WellFormed n
  · intro r valid rest
    have strip := reference_padding ('\n' :: (ind (n + 1)).toList)
      (r.print.toList ++ ',' :: rest) (newline_ind_ws (n + 1))
    simpa only [String.toList_append, List.cons_append, List.append_assoc] using
      strip.trans (reference_print r rest valid)
  · intro r valid
    simpa only [String.toList_append, ws_ind] using reference_print_start r valid
  · exact pad_ws
  · exact valid

theorem primMeta_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    primMeta (padding ++ cs) = primMeta cs := by
  simp only [primMeta, qual, ident_padding padding cs pad]
  split <;> simp [ident_padding padding cs pad]

theorem primMeta_inherits_prefix (qu : Qual) (n : Nat) (tail : List Char) :
    primMeta ((ind n ++ qu.print ++ "inherits = ").toList ++ tail) =
      (listOrOne pathRef (' ' :: tail)).bind (fun result => some (.inherits qu result.1, result.2)) := by
  simp only [String.toList_append, List.append_assoc]
  rw [primMeta_padding (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  cases qu <;>
    simp [primMeta, qual, Qual.print, ident, ws, isWs, isIdStart, isIdCont, lit, show "=".length = 1 from rfl]
theorem primMeta_specializes_prefix (qu : Qual) (n : Nat) (tail : List Char) :
    primMeta ((ind n ++ qu.print ++ "specializes = ").toList ++ tail) =
      (listOrOne pathRef (' ' :: tail)).bind (fun result => some (.specializes qu result.1, result.2)) := by
  simp only [String.toList_append, List.append_assoc]
  rw [primMeta_padding (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  cases qu <;>
    simp [primMeta, qual, Qual.print, ident, ws, isWs, isIdStart, isIdCont, lit, show "=".length = 1 from rfl]
theorem primMeta_references_prefix (qu : Qual) (n : Nat) (tail : List Char) :
    primMeta ((ind n ++ qu.print ++ "references = ").toList ++ tail) =
      (listOrOne reference (' ' :: tail)).bind (fun result => some (.references qu result.1, result.2)) := by
  simp only [String.toList_append, List.append_assoc]
  rw [primMeta_padding (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  cases qu <;>
    simp [primMeta, qual, Qual.print, ident, ws, isWs, isIdStart, isIdCont, lit, show "=".length = 1 from rfl]

theorem primMeta_inherits_print (qu : Qual) (n : Nat) (xs : List String) (rest : List Char)
    (valid : ∀ s ∈ xs, ∀ c ∈ s.toList, c ≠ '>') :
    primMeta (((PrimMeta.inherits qu xs).print n).toList ++ rest) =
      some (.inherits qu xs, rest) := by
  have front := primMeta_inherits_prefix qu n ((bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at front
  change primMeta (((ind n ++ qu.print ++ "inherits = ") ++ bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest) = _
  simp only [String.toList_append, List.append_assoc]
  rw [front]
  have values := pathRef_bracket_padding n xs [' '] rest (by simp [isWs]) valid
  change listOrOne pathRef (' ' :: ((bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest)) = some (xs, rest) at values
  rw [values]
  rfl

theorem primMeta_specializes_print (qu : Qual) (n : Nat) (xs : List String) (rest : List Char)
    (valid : ∀ s ∈ xs, ∀ c ∈ s.toList, c ≠ '>') :
    primMeta (((PrimMeta.specializes qu xs).print n).toList ++ rest) =
      some (.specializes qu xs, rest) := by
  have front := primMeta_specializes_prefix qu n ((bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at front
  change primMeta (((ind n ++ qu.print ++ "specializes = ") ++ bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest) = _
  simp only [String.toList_append, List.append_assoc]
  rw [front]
  have values := pathRef_bracket_padding n xs [' '] rest (by simp [isWs]) valid
  change listOrOne pathRef (' ' :: ((bracket n (xs.map (fun p => "<" ++ p ++ ">"))).toList ++ rest)) = some (xs, rest) at values
  rw [values]
  rfl

theorem primMeta_references_print (qu : Qual) (n : Nat) (xs : List Reference) (rest : List Char)
    (valid : ∀ r ∈ xs, r.WellFormed) :
    primMeta (((PrimMeta.references qu xs).print n).toList ++ rest) =
      some (.references qu xs, rest) := by
  have front := primMeta_references_prefix qu n ((bracket n (xs.map Reference.print)).toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at front
  change primMeta (((ind n ++ qu.print ++ "references = ") ++ bracket n (xs.map Reference.print)).toList ++ rest) = _
  simp only [String.toList_append, List.append_assoc]
  rw [front]
  have values := reference_bracket_padding n xs [' '] rest (by simp [isWs]) valid
  change listOrOne reference (' ' :: ((bracket n (xs.map Reference.print)).toList ++ rest)) = some (xs, rest) at values
  rw [values]
  rfl

/-- Lexical admissibility of arc metadata in the operational subset.
This predicate makes no assertion about composition validity. -/
def PrimMeta.ArcWellFormed : PrimMeta → Prop
  | .inherits _ xs | .specializes _ xs => ∀ s ∈ xs, ∀ c ∈ s.toList, c ≠ '>'
  | .references _ xs => ∀ r ∈ xs, r.WellFormed
  | .customData _ => False

theorem primMeta_arc_print (m : PrimMeta) (n : Nat) (rest : List Char)
    (valid : m.ArcWellFormed) :
    primMeta ((m.print n).toList ++ rest) = some (m, rest) := by
  cases m with
  | inherits qu xs => exact primMeta_inherits_print qu n xs rest valid
  | specializes qu xs => exact primMeta_specializes_print qu n xs rest valid
  | references qu xs => exact primMeta_references_print qu n xs rest valid
  | customData d => exact False.elim valid

theorem primMeta_arc_not_close (m : PrimMeta) (n : Nat) (rest : List Char)
    (valid : m.ArcWellFormed) : peek ((m.print n).toList ++ rest) ≠ some ')' := by
  cases m with
  | customData d => exact False.elim valid
  | inherits qu xs | specializes qu xs | references qu xs =>
    simp only [PrimMeta.print, String.toList_append, List.append_assoc, peek, ws_ind]
    cases qu <;> simp [Qual.print, ws, isWs]

def metaBody (n : Nat) (xs : List PrimMeta) : List Char :=
  xs.flatMap (fun m => (m.print n).toList ++ ['\n'])

theorem primMetas_arcs (n : Nat) (xs acc : List PrimMeta) (padding closing rest : List Char)
    (fuel : Nat) (pad_ws : ∀ c ∈ padding, isWs c = true)
    (close_ws : ∀ c ∈ closing, isWs c = true)
    (valid : ∀ m ∈ xs, m.ArcWellFormed) (enough : xs.length + 1 ≤ fuel) :
    primMetas fuel acc (padding ++ metaBody n xs ++ closing ++ ')' :: rest) =
      some (acc.reverse ++ xs, rest) := by
  induction xs generalizing acc fuel padding with
  | nil =>
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have strip : ws (padding ++ closing ++ ')' :: rest) = ')' :: rest := by
        unfold ws
        rw [List.append_assoc, dropWhile_prefix isWs padding _ pad_ws,
          dropWhile_prefix isWs closing _ close_ws]
        rfl
      simp only [List.append_assoc] at strip
      simp [metaBody, primMetas, peek, lit, strip, show ")".length = 1 from rfl]
  | cons m xs ih =>
    have head_valid := valid m (by simp)
    have tail_valid : ∀ x ∈ xs, x.ArcWellFormed := fun x hx => valid x (by simp [hx])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      let tail := '\n' :: (metaBody n xs ++ (closing ++ ')' :: rest))
      have parsed := (primMeta_padding padding ((m.print n).toList ++ tail) pad_ws).trans
        (primMeta_arc_print m n tail head_valid)
      have not_close := primMeta_arc_not_close m n tail head_valid
      have strip : peek (padding ++ ((m.print n).toList ++ tail)) =
          peek ((m.print n).toList ++ tail) := by
        unfold peek ws
        rw [dropWhile_prefix isWs padding _ pad_ws]
      simp only [metaBody, List.flatMap_cons, List.append_assoc, List.cons_append, List.nil_append]
      change primMetas (fuel + 1) acc (padding ++ ((m.print n).toList ++ tail)) = _
      rw [primMetas]
      simp only [strip, beq_iff_eq, not_close, ↓reduceIte, parsed]
      have next := ih (m :: acc) ['\n'] fuel (by simp [isWs]) tail_valid (by simp at enough; omega)
      simpa [tail, List.reverse_cons, List.append_assoc] using next

theorem metaBody_length_ge (n : Nat) (xs : List PrimMeta) : xs.length ≤ (metaBody n xs).length := by
  induction xs with
  | nil => simp [metaBody]
  | cons m xs ih =>
    simp only [metaBody, List.flatMap_cons, List.length_append, List.length_cons, List.length_nil] at *
    omega

theorem metaBody_text (n : Nat) (xs : List PrimMeta) :
    (String.join (xs.map (fun m => m.print n ++ "\n"))).toList = metaBody n xs := by
  simp [String.toList_join, List.flatMap_map, String.toList_append,
    metaBody, show "\n".toList = ['\n'] from rfl]

theorem primMetas_arcs_text (n : Nat) (xs : List PrimMeta) (rest : List Char)
    (valid : ∀ m ∈ xs, m.ArcWellFormed) :
    let cs := '\n' ::
      (String.join (xs.map (fun m => m.print (n + 1) ++ "\n")) ++ ind n ++ ")").toList ++ rest
    primMetas (cs.length + 1) [] cs = some (xs, rest) := by
  dsimp only
  simp only [String.toList_append, metaBody_text, show ")".toList = [')'] from rfl,
    List.append_assoc, List.cons_append, List.nil_append]
  have count := metaBody_length_ge (n + 1) xs
  have closing : ∀ c ∈ (ind n).toList, isWs c = true := by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl
  have result := primMetas_arcs (n + 1) xs [] ['\n'] (ind n).toList rest
    (('\n' :: (metaBody (n + 1) xs ++ (ind n).toList ++ ')' :: rest)).length + 1)
    (by simp [isWs]) closing valid (by simp only [List.length_cons, List.length_append]; omega)
  simpa only [List.cons_append, List.nil_append, List.reverse_nil, List.nil_append,
    List.append_assoc] using result
end UsdSpec.Usda
