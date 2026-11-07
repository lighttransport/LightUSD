import UsdSpec.Usda.Grammar
/-!
# USDA subset lexical and list round-trip proofs

SPEC: core_spec.md L10057-10084, L10153-10192, L10377, L10824-10861.
These results concern the existing operational subset in Grammar.lean. They
preserve its definitions and prove exact parser results with arbitrary trailing
input, whitespace indentation, and unbounded list lengths.

IdentLexeme describes the subset lexer rather than the complete Unicode XID
production. NumLexeme describes scanner characters, not semantic Number PEG
validity or numeric conversion. Delimiter safety is syntactic, not validation
of an asset identifier or USD object path. These distinctions are required for
the whole-AST well-formedness and round-trip proofs (LayerProofs).

The generic list theorem states its item inverse and lexical-start assumptions
explicitly. pathRef_bracket discharges them for the actual path-reference
printer used by inherits/specializes; it does not assume that list result.
-/
namespace UsdSpec.Usda

theorem takeWhile_prefix {α : Type} (p : α → Bool) (front : List α) (stop : α) (rest : List α)
    (all : ∀ c ∈ front, p c = true) (boundary : p stop = false) :
    (front ++ stop :: rest).takeWhile p = front := by
  induction front with
  | nil => simp [boundary]
  | cons c cs ih =>
    have head := all c (by simp)
    have tail : ∀ x ∈ cs, p x = true := fun x hx => all x (by simp [hx])
    simp [head, ih tail]

theorem dropWhile_prefix {α : Type} (p : α → Bool) (front rest : List α)
    (all : ∀ c ∈ front, p c = true) :
    (front ++ rest).dropWhile p = rest.dropWhile p := by
  induction front with
  | nil => rfl
  | cons c cs ih =>
    have head := all c (by simp)
    have tail : ∀ x ∈ cs, p x = true := fun x hx => all x (by simp [hx])
    simp [head, ih tail]

theorem ws_cons (c : Char) (rest : List Char) (h : isWs c = false) :
    ws (c :: rest) = c :: rest := by simp [ws, h]

theorem ws_ind (n : Nat) (rest : List Char) : ws ((ind n).toList ++ rest) = ws rest := by
  apply dropWhile_prefix
  intro c hc
  simp only [ind, String.toList_ofList, List.mem_replicate] at hc
  rcases hc with ⟨_, rfl⟩
  rfl

theorem ident_first_not_ws (c : Char) (hc : isIdStart c = true) : isWs c = false := by
  cases hw : isWs c with
  | false => rfl
  | true =>
    simp only [isWs, Bool.or_eq_true, beq_iff_eq] at hw
    rcases hw with (((rfl | rfl) | rfl) | rfl) | rfl <;> simp [isIdStart] at hc

theorem ident_echo (c : Char) (cs : List Char) (stop : Char) (rest : List Char)
    (start : isIdStart c = true) (cont : ∀ x ∈ cs, isIdCont x = true)
    (boundary : isIdCont stop = false) :
    ident (c :: cs ++ stop :: rest) = some (String.ofList (c :: cs), stop :: rest) := by
  have first := ident_first_not_ws c start
  have take := takeWhile_prefix isIdCont cs stop rest cont boundary
  simp [ident, ws_cons _ _ first, start, take]

theorem num_first_not_ws (c : Char) (hc : isNumChar c = true) : isWs c = false := by
  cases hw : isWs c with
  | false => rfl
  | true =>
    simp only [isWs, Bool.or_eq_true, beq_iff_eq] at hw
    rcases hw with (((rfl | rfl) | rfl) | rfl) | rfl <;> simp [isNumChar] at hc

theorem num_echo (c : Char) (cs : List Char) (stop : Char) (rest : List Char)
    (head : isNumChar c = true) (cont : ∀ x ∈ cs, isNumChar x = true)
    (boundary : isNumChar stop = false) :
    num (c :: cs ++ stop :: rest) = some (String.ofList (c :: cs), stop :: rest) := by
  have first := num_first_not_ws c head
  have all : ∀ x ∈ c :: cs, isNumChar x = true := by simp_all
  have take := takeWhile_prefix isNumChar (c :: cs) stop rest all boundary
  simp only [List.cons_append] at take
  simp [num, ws_cons _ _ first, take]

theorem delimited_echo (opener closer : Char) (payload rest : List Char)
    (not_ws : isWs opener = false) (no_close : ∀ c ∈ payload, c ≠ closer) :
    delimited opener closer (opener :: payload ++ closer :: rest) =
      some (String.ofList payload, rest) := by
  have take := takeWhile_prefix (fun c => c != closer) payload closer rest
    (by simpa using no_close) (by simp)
  simp [delimited, ws_cons _ _ not_ws, take]

/-- Syntactic identifier domain of this subset lexer, not full Unicode XID. -/
def IdentLexeme (s : String) : Prop := ∃ c cs, s.toList = c :: cs ∧
  isIdStart c = true ∧ ∀ x ∈ cs, isIdCont x = true

/-- Lexeme consumed by the model's numeric scanner. This alone does not assert
acceptance by the normative Number PEG or semantic numeric conversion. -/
def NumLexeme (s : String) : Prop := ∃ c cs, s.toList = c :: cs ∧
  isNumChar c = true ∧ ∀ x ∈ cs, isNumChar x = true

theorem ident_print (s : String) (stop : Char) (rest : List Char)
    (valid : IdentLexeme s) (boundary : isIdCont stop = false) :
    ident (s.toList ++ stop :: rest) = some (s, stop :: rest) := by
  rcases valid with ⟨c, cs, spell, start, cont⟩
  have proof := ident_echo c cs stop rest start cont boundary
  simpa only [← spell, String.ofList_toList] using proof

theorem num_print (s : String) (stop : Char) (rest : List Char)
    (valid : NumLexeme s) (boundary : isNumChar stop = false) :
    num (s.toList ++ stop :: rest) = some (s, stop :: rest) := by
  rcases valid with ⟨c, cs, spell, start, cont⟩
  have proof := num_echo c cs stop rest start cont boundary
  simpa only [← spell, String.ofList_toList] using proof

theorem asset_print (s : String) (rest : List Char)
    (safe : ∀ c ∈ s.toList, c ≠ '@') :
    asset (('@' :: s.toList) ++ '@' :: rest) = some (s, rest) := by
  simpa [asset] using delimited_echo '@' '@' s.toList rest rfl safe

theorem pathRef_print (s : String) (rest : List Char)
    (safe : ∀ c ∈ s.toList, c ≠ '>') :
    pathRef (('<' :: s.toList) ++ '>' :: rest) = some (s, rest) := by
  simpa [pathRef] using delimited_echo '<' '>' s.toList rest rfl safe

theorem specifier_print_general (s : Specifier) (rest : List Char) :
    specifier (s.print.toList ++ ' ' :: rest) = some (s, ' ' :: rest) := by
  cases s <;> rfl

def commaBody {α : Type} (emit : α → List Char) : List α → List Char → List Char
  | [], tail => tail
  | x :: xs, tail => emit x ++ ',' :: '\n' :: commaBody emit xs tail

theorem ws_newline (cs : List Char) : ws ('\n' :: cs) = ws cs := by rfl

theorem ws_append_nonempty (front rest : List Char) (h : ws front ≠ []) :
    ws (front ++ rest) = ws front ++ rest := by
  unfold ws at *
  rw [List.dropWhile_append]
  have nonempty : (front.dropWhile isWs).isEmpty = false := by
    cases hs : front.dropWhile isWs <;> simp_all
  simp only [nonempty, Bool.false_eq_true, ↓reduceIte]

theorem listUntil_print {α : Type} (item : P α) (emit : α → List Char)
    (Valid : α → Prop)
    (parse_item : ∀ (x : α), Valid x → ∀ (rest : List Char),
      item ('\n' :: emit x ++ ',' :: rest) = some (x, ',' :: rest))
    (starts_value : ∀ x, Valid x → ∃ c cs, ws (emit x) = c :: cs ∧ c ≠ ']')
    (xs acc : List α) (padding rest : List Char) (fuel : Nat)
    (pad_ws : ∀ c ∈ padding, isWs c = true)
    (enough : xs.length + 1 ≤ fuel) (items_valid : ∀ x ∈ xs, Valid x) :
    listUntil item fuel acc ('\n' :: commaBody emit xs (padding ++ ']' :: rest)) =
      some (acc.reverse ++ xs, rest) := by
  simp only [List.cons_append] at parse_item
  revert items_valid
  induction xs generalizing fuel acc with
  | nil =>
    intro items_valid
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have strip : ws ('\n' :: (padding ++ ']' :: rest)) = ']' :: rest := by
        simp only [ws, List.dropWhile_cons, isWs, beq_self_eq_true, Bool.or_true,
          Bool.true_or, ↓reduceIte]
        rw [dropWhile_prefix isWs padding (']' :: rest) pad_ws]
        rfl
      simp [commaBody, listUntil, peek, lit, strip, show "]".length = 1 from rfl]
  | cons x xs ih =>
    intro items_valid
    have valid_head := items_valid x (by simp)
    have valid_tail : ∀ y ∈ xs, Valid y := fun y hy => items_valid y (by simp [hy])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have step := ih (x :: acc) fuel (by simpa using enough) valid_tail
      rcases starts_value x valid_head with ⟨c, cs, start, not_close⟩
      have nonempty : ws (emit x) ≠ [] := by rw [start]; simp
      have peek_start : peek ('\n' :: (emit x ++ ',' :: ('\n' :: commaBody emit xs (padding ++ ']' :: rest)))) ≠ some ']' := by
        simpa only [List.cons_append, peek, ws_newline,
          ws_append_nonempty _ _ nonempty, start, List.head?_cons, Option.some.injEq, ne_eq]
          using not_close
      simp only [commaBody]
      rw [listUntil]
      simp only [beq_iff_eq]
      rw [ite_eq_right (by exact peek_start), parse_item x valid_head]
      change listUntil item fuel (x :: acc) ('\n' :: commaBody emit xs (padding ++ ']' :: rest)) = _
      simpa [List.reverse_cons, List.append_assoc] using step


theorem commaBody_flatMap {α : Type} (emit : α → List Char) (xs : List α) (tail : List Char) :
    commaBody emit xs tail = xs.flatMap (fun x => emit x ++ [',', '\n']) ++ tail := by
  induction xs with
  | nil => rfl
  | cons x xs ih => simp [commaBody, ih, List.append_assoc]

theorem commaBody_length_ge {α : Type} (emit : α → List Char) (xs : List α) (tail : List Char) :
    xs.length ≤ (commaBody emit xs tail).length := by
  induction xs with
  | nil => simp
  | cons x xs ih => simp only [commaBody, List.length_append, List.length_cons] at *; omega

theorem bracket_chars {α : Type} (print : α → String) (n : Nat) (xs : List α) (rest : List Char)
    (nonempty : xs ≠ []) :
    (bracket n (xs.map print)).toList ++ rest =
      '[' :: '\n' :: commaBody (fun x => (ind (n + 1) ++ print x).toList) xs
        ((ind n).toList ++ ']' :: rest) := by
  cases xs with
  | nil => contradiction
  | cons x xs =>
    simp [bracket, commaBody_flatMap, String.toList_join, List.flatMap_map,
      Function.comp_def, String.toList_append, List.append_assoc,
      show ("[\n").toList = ['[', '\n'] from rfl,
      show (",\n").toList = [',', '\n'] from rfl,
      show ("]").toList = [']'] from rfl]

theorem listOrOne_bracket {α : Type} (item : P α) (print : α → String)
    (Valid : α → Prop) (n : Nat)
    (parse_item : ∀ x, Valid x → ∀ rest,
      item ('\n' :: (ind (n + 1) ++ print x).toList ++ ',' :: rest) = some (x, ',' :: rest))
    (starts_value : ∀ x, Valid x → ∃ c cs,
      ws ((ind (n + 1) ++ print x).toList) = c :: cs ∧ c ≠ ']')
    (xs : List α) (rest : List Char) (valid : ∀ x ∈ xs, Valid x) :
    listOrOne item ((bracket n (xs.map print)).toList ++ rest) = some (xs, rest) := by
  by_cases empty : xs = []
  · subst xs
    simp [bracket, listOrOne, ident, ws, isWs, isIdStart, peek, lit, listUntil,
      show "[".length = 1 from rfl, show "]".length = 1 from rfl]
  · rw [bracket_chars print n xs rest empty]
    let emit := fun x => (ind (n + 1) ++ print x).toList
    let pad := (ind n).toList
    let tail := pad ++ ']' :: rest
    change listUntil item (('[' :: '\n' :: commaBody emit xs tail).length + 1) []
      ('\n' :: commaBody emit xs tail) = some (xs, rest)
    have pad_ws : ∀ c ∈ pad, isWs c = true := by
      intro c hc
      simp only [pad, ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    have length_bound := commaBody_length_ge emit xs tail
    have result := listUntil_print item emit Valid parse_item starts_value xs [] pad rest
      (('[' :: '\n' :: commaBody emit xs tail).length + 1) pad_ws
      (by simp only [List.length_cons]; omega) valid
    simpa only [List.reverse_nil, List.nil_append] using result

theorem pathRef_bracket (n : Nat) (xs : List String) (rest : List Char)
    (safe : ∀ s ∈ xs, ∀ c ∈ s.toList, c ≠ '>') :
    listOrOne pathRef ((bracket n (xs.map (fun s => "<" ++ s ++ ">"))).toList ++ rest) =
      some (xs, rest) := by
  apply listOrOne_bracket pathRef (fun s => "<" ++ s ++ ">")
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
  · exact safe
end UsdSpec.Usda
