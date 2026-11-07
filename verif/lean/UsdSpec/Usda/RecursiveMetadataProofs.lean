import UsdSpec.Usda.DictionaryProofs
/-!
# Recursive dictionary and complete subset metadata recovery

No depth, entry-count, integer-magnitude or string-length bound is imposed.
EntriesWellFormed_all proves every dictionary tree admissible, including arbitrary
String keys at every depth; PrimMeta.WellFormed retains existing delimiter and
offset hypotheses for arcs. These are lexical printer admissibility conditions, not composition or
normative full-PEG validity. The actual recursive parser is recovered with a
conservative finite dictCost; printed length dominates that bound. The actual
primMeta and primMetas inverses include nested customData and mixed arc edits.
Nested prims and whole-Layer AST recovery are proved in PrimProofs and LayerProofs.
-/
namespace UsdSpec.Usda
/-- Conservative parser fuel, counting every nested container and entry. -/
def dictCost : List (String × DVal) → Nat
  | [] => 1
  | (_, .dict nested) :: rest => dictCost nested + dictCost rest + 2
  | _ :: rest => dictCost rest + 1

def EntriesWellFormed : List (String × DVal) → Prop
  | [] => True
  | (_, .dict nested) :: rest => EntriesWellFormed nested ∧ EntriesWellFormed rest
  | (_, _) :: rest => EntriesWellFormed rest
/-- Every dictionary key/value tree is supported; there is no lexical key restriction. -/
theorem EntriesWellFormed_all (xs : List (String × DVal)) : EntriesWellFormed xs := by
  cases xs with
  | nil => simp [EntriesWellFormed]
  | cons entry rest =>
    rcases entry with ⟨key, value⟩
    cases value with
    | int _ | str _ => simpa only [EntriesWellFormed] using EntriesWellFormed_all rest
    | dict nested =>
      simp only [EntriesWellFormed]
      exact ⟨EntriesWellFormed_all nested, EntriesWellFormed_all rest⟩
termination_by sizeOf xs

theorem dictItems_dict_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char) (valid : IdentLexeme key) :
    dictItems (fuel + 1) acc (("dictionary " ++ key ++ " = ").toList ++ rest) =
      (do let (i, r) ← dict fuel (' ' :: rest); dictItems fuel ((key, .dict i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("dictionary ".toList ++ (key.toList ++ (" = ".toList ++ rest))) =
      some ("dictionary", ' ' :: (key.toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("dictionary ".toList ++ (key.toList ++ (" = ".toList ++ rest))) = some 'd' := rfl
  rw [dictItems]
  simp only [peekv, show (some 'd' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  change ((match str (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest)) with
    | some x => some x | none => ident (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest))).bind
      (fun kr => (lit "=" kr.2).bind (fun eqr =>
        ((dict fuel eqr.2).bind (fun ir => some (DVal.dict ir.1, ir.2))).bind
          (fun vr => dictItems fuel ((kr.1, vr.1) :: acc) vr.2)))) = _
  have no_string : str (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest)) = none :=
    (str_padding [' '] _ (by simp [isWs])).trans (str_ident_none key _ valid)
  have identifier : ident (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest)) =
      some (key, ' ' :: '=' :: ' ' :: rest) := by
    simpa only [ident_space] using ident_print key ' ' ('=' :: ' ' :: rest) valid (by decide)
  rw [no_string, identifier]
  cases h : dict fuel (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]


theorem dictItems_dict_quoted_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char)  :
    dictItems (fuel + 1) acc (("dictionary " ++ q key ++ " = ").toList ++ rest) =
      (do let (i, r) ← dict fuel (' ' :: rest); dictItems fuel ((key, .dict i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("dictionary ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) =
      some ("dictionary", ' ' :: ((q key).toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("dictionary ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) = some 'd' := rfl
  rw [dictItems]
  simp only [peekv, show (some 'd' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  have string_key : str (' ' :: ((q key).toList ++ ' ' :: '=' :: ' ' :: rest)) =
      some (key, ' ' :: '=' :: ' ' :: rest) :=
    (str_padding [' '] _ (by simp [isWs])).trans (str_print key _)
  simp only [Bind.bind, Option.bind_some]
  rw [string_key]
  cases h : dict fuel (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]


theorem dictItems_dict_step (n fuel : Nat) (acc : List (String × DVal))
    (key : String) (nested : List (String × DVal)) (rest : List Char)
    (child : dict fuel ((DVal.printDict n nested).toList ++ '\n' :: rest) =
      some (nested, '\n' :: rest)) :
    dictItems (fuel + 1) acc ((DVal.printItem n (key, .dict nested)).toList ++ rest) =
      dictItems fuel ((key, .dict nested) :: acc) ('\n' :: rest) := by
  simp only [DVal.printItem, String.toList_append, List.append_assoc]
  rw [dictItems_padding (fuel + 1) acc (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  have header := dictItems_dict_quoted_prefix fuel acc key ((DVal.printDict n nested ++ "\n").toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at header
  rw [header]
  have strip := dict_padding fuel [' '] ((DVal.printDict n nested).toList ++ '\n' :: rest)
    (by simp [isWs])
  simp only [List.cons_append, List.nil_append] at strip
  simp only [show "\n".toList = ['\n'] from rfl, List.cons_append, List.nil_append]
  rw [strip, child]
  rfl

theorem dictItems_nested (n : Nat) (xs acc : List (String × DVal))
    (padding closing rest : List Char) (fuel : Nat)
    (pad_ws : ∀ c ∈ padding, isWs c = true) (close_ws : ∀ c ∈ closing, isWs c = true)
    (valid : EntriesWellFormed xs) (enough : dictCost xs ≤ fuel) :
    dictItems fuel acc (padding ++ (DVal.printItems n xs).toList ++ closing ++ '}' :: rest) =
      some (acc.reverse ++ xs, rest) := by
  cases fuel with
  | zero =>
    cases xs with
    | nil => simp [dictCost] at enough
    | cons entry xs =>
      rcases entry with ⟨key, value⟩
      cases value <;> simp only [dictCost] at enough <;> omega
  | succ fuel =>
    cases xs with
    | nil =>
      have strip : ws (padding ++ (closing ++ '}' :: rest)) = '}' :: rest := by
        unfold ws
        rw [dropWhile_prefix isWs padding _ pad_ws, dropWhile_prefix isWs closing _ close_ws]
        rfl
      simp [DVal.printItems, dictItems, peek, lit, strip, List.append_assoc,
        show "}".length = 1 from rfl]
    | cons entry xs =>
      rcases entry with ⟨key, value⟩
      simp only [DVal.printItems, String.toList_append, List.append_assoc]
      rw [dictItems_padding (fuel + 1) acc padding _ pad_ws]
      cases value with
      | int i =>
        simp only [EntriesWellFormed] at valid
        have tail_valid : EntriesWellFormed xs := valid
        have tail_enough : dictCost xs ≤ fuel := by simp only [dictCost] at enough; omega
        rw [dictItems_int_step n fuel acc key i _]
        have next := dictItems_nested n xs ((key, .int i) :: acc) ['\n'] closing rest fuel
          (by simp [isWs]) close_ws tail_valid tail_enough
        simpa [List.reverse_cons, List.append_assoc] using next
      | str value =>
        simp only [EntriesWellFormed] at valid
        have tail_valid : EntriesWellFormed xs := valid
        have tail_enough : dictCost xs ≤ fuel := by simp only [dictCost] at enough; omega
        rw [dictItems_str_step n fuel acc key value _]
        have next := dictItems_nested n xs ((key, .str value) :: acc) ['\n'] closing rest fuel
          (by simp [isWs]) close_ws tail_valid tail_enough
        simpa [List.reverse_cons, List.append_assoc] using next
      | dict nested =>
        simp only [EntriesWellFormed] at valid
        have child_valid : EntriesWellFormed nested := valid.1
        have tail_valid : EntriesWellFormed xs := valid.2
        cases fuel with
        | zero => simp only [dictCost] at enough; omega
        | succ fuel =>
          have child_enough : dictCost nested ≤ fuel := by simp only [dictCost] at enough; omega
          have tail_enough : dictCost xs ≤ fuel + 1 := by simp only [dictCost] at enough; omega
          have indent : ∀ c ∈ (ind n).toList, isWs c = true := by
            intro c hc
            simp only [ind, String.toList_ofList, List.mem_replicate] at hc
            rcases hc with ⟨_, rfl⟩
            rfl
          let tail := (DVal.printItems n xs).toList ++ (closing ++ '}' :: rest)
          have child_items := dictItems_nested (n + 1) nested [] ['\n'] (ind n).toList
            ('\n' :: tail) fuel (by simp [isWs]) indent child_valid child_enough
          have child : dict (fuel + 1) ((DVal.printDict n nested).toList ++ '\n' :: tail) =
              some (nested, '\n' :: tail) := by
            rw [dict_print_chars]
            change dictItems fuel [] ('\n' :: ((DVal.printItems (n + 1) nested).toList ++
              (ind n).toList ++ '}' :: '\n' :: tail)) = _
            simpa only [List.cons_append, List.nil_append, List.reverse_nil,
              List.append_assoc] using child_items
          rw [dictItems_dict_step n (fuel + 1) acc key nested tail child]
          have next := dictItems_nested n xs ((key, .dict nested) :: acc) ['\n'] closing rest
            (fuel + 1) (by simp [isWs]) close_ws tail_valid tail_enough
          simpa [tail, List.reverse_cons, List.append_assoc] using next
termination_by fuel

theorem dict_print_length (n : Nat) (xs : List (String × DVal)) :
    (DVal.printDict n xs).length = (DVal.printItems (n + 1) xs).length + (ind n).length + 3 := by
  cases xs <;> simp [DVal.printDict, DVal.printItems, String.length_append,
    show "{\n".length = 2 from rfl, show "}".length = 1 from rfl] <;> omega

theorem dictCost_print_bound (n : Nat) (xs : List (String × DVal)) :
    dictCost xs ≤ (DVal.printItems n xs).length + 1 := by
  cases xs with
  | nil => simp [dictCost, DVal.printItems]
  | cons entry xs =>
    rcases entry with ⟨key, value⟩
    have tail := dictCost_print_bound n xs
    cases value with
    | int i | str value =>
      simp only [dictCost, DVal.printItems, DVal.printItem, String.length_append,
        show "\n".length = 1 from rfl]
      omega
    | dict nested =>
      have child := dictCost_print_bound (n + 1) nested
      simp only [dictCost, DVal.printItems, DVal.printItem, String.length_append,
        dict_print_length, show "\n".length = 1 from rfl,
        show "dictionary ".length = 11 from rfl]
      omega
termination_by sizeOf xs

theorem dict_nested (n : Nat) (xs : List (String × DVal)) (rest : List Char)
    (fuel : Nat) (valid : EntriesWellFormed xs) (enough : dictCost xs + 1 ≤ fuel) :
    dict fuel ((DVal.printDict n xs).toList ++ rest) = some (xs, rest) := by
  rw [dict_print_chars]
  cases fuel with
  | zero => omega
  | succ fuel =>
    change dictItems fuel [] ('\n' :: ((DVal.printItems (n + 1) xs).toList ++ (ind n).toList ++ '}' :: rest)) = _
    have closing : ∀ c ∈ (ind n).toList, isWs c = true := by
      intro c hc
      simp only [ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    have result := dictItems_nested (n + 1) xs [] ['\n'] (ind n).toList rest fuel
      (by simp [isWs]) closing valid (by omega)
    simpa only [List.cons_append, List.nil_append, List.reverse_nil, List.append_assoc] using result

theorem primMeta_customData_nested_print (n : Nat) (xs : List (String × DVal))
    (rest : List Char) (valid : EntriesWellFormed xs) :
    primMeta (((PrimMeta.customData xs).print n).toList ++ rest) = some (.customData xs, rest) := by
  have front := primMeta_customData_prefix n ((DVal.printDict n xs).toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at front
  simp only [PrimMeta.print, String.toList_append, List.append_assoc]
  rw [front]
  have count := dictCost_print_bound (n + 1) xs
  have enough : dictCost xs + 1 ≤ (' ' :: ((DVal.printDict n xs).toList ++ rest)).length + 1 := by
    simp only [List.length_cons, List.length_append, String.length_toList, dict_print_length]
    omega
  have strip := dict_padding ((' ' :: ((DVal.printDict n xs).toList ++ rest)).length + 1)
    [' '] ((DVal.printDict n xs).toList ++ rest) (by simp [isWs])
  simp only [List.cons_append, List.nil_append] at strip
  rw [strip, dict_nested n xs rest _ valid enough]
  rfl

/-- Lexical admissibility for every metadata constructor in this subset. -/
def PrimMeta.WellFormed : PrimMeta → Prop
  | .customData xs => EntriesWellFormed xs
  | m => m.ArcWellFormed

theorem primMeta_print (m : PrimMeta) (n : Nat) (rest : List Char) (valid : m.WellFormed) :
    primMeta ((m.print n).toList ++ rest) = some (m, rest) := by
  cases m with
  | customData xs => exact primMeta_customData_nested_print n xs rest valid
  | references qu xs => exact primMeta_references_print qu n xs rest valid
  | inherits qu xs => exact primMeta_inherits_print qu n xs rest valid
  | specializes qu xs => exact primMeta_specializes_print qu n xs rest valid

theorem primMeta_print_not_close (m : PrimMeta) (n : Nat) (rest : List Char)
    (valid : m.WellFormed) : peek ((m.print n).toList ++ rest) ≠ some ')' := by
  cases m with
  | customData xs =>
    simp only [PrimMeta.print, String.toList_append, List.append_assoc, peek, ws_ind]
    simp [ws, isWs]
  | references qu xs => exact primMeta_arc_not_close (.references qu xs) n rest valid
  | inherits qu xs => exact primMeta_arc_not_close (.inherits qu xs) n rest valid
  | specializes qu xs => exact primMeta_arc_not_close (.specializes qu xs) n rest valid
theorem primMetas_print (n : Nat) (xs acc : List PrimMeta) (padding closing rest : List Char)
    (fuel : Nat) (pad_ws : ∀ c ∈ padding, isWs c = true)
    (close_ws : ∀ c ∈ closing, isWs c = true)
    (valid : ∀ m ∈ xs, m.WellFormed) (enough : xs.length + 1 ≤ fuel) :
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
    have tail_valid : ∀ x ∈ xs, x.WellFormed := fun x hx => valid x (by simp [hx])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      let tail := '\n' :: (metaBody n xs ++ (closing ++ ')' :: rest))
      have parsed := (primMeta_padding padding ((m.print n).toList ++ tail) pad_ws).trans
        (primMeta_print m n tail head_valid)
      have not_close := primMeta_print_not_close m n tail head_valid
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

theorem primMetas_print_text (n : Nat) (xs : List PrimMeta) (rest : List Char)
    (valid : ∀ m ∈ xs, m.WellFormed) :
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
  have result := primMetas_print (n + 1) xs [] ['\n'] (ind n).toList rest
    (('\n' :: (metaBody (n + 1) xs ++ (ind n).toList ++ ')' :: rest)).length + 1)
    (by simp [isWs]) closing valid (by simp only [List.length_cons, List.length_append]; omega)
  simpa only [List.cons_append, List.nil_append, List.reverse_nil, List.nil_append,
    List.append_assoc] using result
end UsdSpec.Usda
