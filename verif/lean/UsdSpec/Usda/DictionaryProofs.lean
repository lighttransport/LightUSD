import UsdSpec.Usda.MetadataProofs
import Std.Data.String.ToInt
/-!
Dictionary codec and parser assembly proofs for the operational USDA subset.
Integer printing is inverted for every Int, without a magnitude bound. Keys and
string values use the general quoted-string inverse, with arbitrary String keys.
Bare-key prefix lemmas remain available for identifier source syntax. PrimitiveEntryWellFormed deliberately excludes nested
dictionaries; RecursiveMetadataProofs and LayerProofs establish recursive and
whole supported-AST recovery.
The flat customData theorem below includes the actual metadata parser and its
input-length fuel; it makes no claim about nested dictionaries.
-/
namespace UsdSpec.Usda
theorem numLexeme_nat_repr (n : Nat) : NumLexeme n.repr := by
  have digits : ∀ c ∈ n.repr.toList, isNumChar c = true := by
    intro c hc
    have digit := Nat.isDigit_of_mem_toDigits (b := 10) (by decide) (by decide)
      (by simpa only [Nat.toList_repr] using hc)
    simp [isNumChar, digit]
  have nonempty : n.repr.toList ≠ [] := by simpa only [Nat.toList_repr] using (Nat.toDigits_ne_nil (n := n) (b := 10))
  cases h : n.repr.toList with
  | nil => exact False.elim (nonempty h)
  | cons c cs =>
    refine ⟨c, cs, h, ?_, ?_⟩
    · exact digits c (by simp [h])
    · intro x hx; exact digits x (by simp [h, hx])

theorem numLexeme_int_string (i : Int) : NumLexeme (toString i) := by
  rw [Int.toString_eq_repr]
  cases i with
  | ofNat n => exact numLexeme_nat_repr n
  | negSucc n =>
    refine ⟨'-', (n + 1).repr.toList, ?_, by decide, ?_⟩
    · simp [Int.repr, String.toList_append]
    · intro c hc
      have digit := Nat.isDigit_of_mem_toDigits (b := 10) (by decide) (by decide)
        (by simpa only [Nat.toList_repr] using hc)
      simp [isNumChar, digit]

theorem int_print (i : Int) (rest : List Char) :
    int ((toString i).toList ++ '\n' :: rest) = some (i, '\n' :: rest) := by
  have value := num_print (toString i) '\n' rest (numLexeme_int_string i) (by decide)
  unfold int
  rw [value]
  simp [Int.toString_eq_repr]

theorem str_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    str (padding ++ cs) = str cs := by
  unfold str ws
  rw [dropWhile_prefix isWs padding cs pad]

theorem str_ident_none (key : String) (rest : List Char) (valid : IdentLexeme key) :
    str (key.toList ++ rest) = none := by
  rcases valid with ⟨c, cs, spell, start, cont⟩
  have first := ident_first_not_ws c start
  have no_quote : c ≠ '"' := by intro h; subst c; simp [isIdStart] at start
  simp only [str, spell, List.cons_append, ws_cons _ _ first]
  cases c; simp_all [parseQuotedChars]

theorem dict_key_print (key : String) (rest : List Char) (valid : IdentLexeme key) :
    (match str (' ' :: (key.toList ++ ' ' :: rest)) with
     | some x => some x | none => ident (' ' :: (key.toList ++ ' ' :: rest))) =
       some (key, ' ' :: rest) := by
  have not_string := str_ident_none key (' ' :: rest) valid
  have identifier := ident_print key ' ' rest valid (by decide)
  have strip : str (' ' :: (key.toList ++ ' ' :: rest)) = str (key.toList ++ ' ' :: rest) :=
    str_padding [' '] _ (by simp [isWs])
  rw [strip, not_string]
  simpa only [ident_space] using identifier

theorem lit_padding (value : String) (padding cs : List Char)
    (pad : ∀ c ∈ padding, isWs c = true) : lit value (padding ++ cs) = lit value cs := by
  unfold lit ws
  rw [dropWhile_prefix isWs padding cs pad]

theorem dictItems_padding (fuel : Nat) (acc : List (String × DVal))
    (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    dictItems fuel acc (padding ++ cs) = dictItems fuel acc cs := by
  cases fuel <;> simp [dictItems, peek_padding padding cs pad,
    ident_padding padding cs pad, lit_padding "}" padding cs pad]

theorem str_print (value : String) (rest : List Char) :
    str ((q value).toList ++ rest) = some (value, rest) := by
  have strip : ws ((q value).toList ++ rest) = (q value).toList ++ rest := by
    simp only [q, quoteString, String.toList_ofList, List.cons_append]
    exact ws_cons _ _ (by decide)
  unfold str
  rw [strip]
  exact parseQuotedChars_quoteString value rest

theorem dictItems_int_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char) (valid : IdentLexeme key) :
    dictItems (fuel + 1) acc (("int " ++ key ++ " = ").toList ++ rest) =
      (do let (i, r) ← int (' ' :: rest); dictItems fuel ((key, .int i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("int ".toList ++ (key.toList ++ (" = ".toList ++ rest))) =
      some ("int", ' ' :: (key.toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("int ".toList ++ (key.toList ++ (" = ".toList ++ rest))) = some 'i' := rfl
  rw [dictItems]
  simp only [peekv, show (some 'i' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  have key_parse := dict_key_print key ('=' :: ' ' :: rest) valid
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  change ((match str (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest)) with
    | some x => some x | none => ident (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest))).bind
      (fun kr => (lit "=" kr.2).bind (fun eqr =>
        ((int eqr.2).bind (fun ir => some (DVal.int ir.1, ir.2))).bind
          (fun vr => dictItems fuel ((kr.1, vr.1) :: acc) vr.2)))) = _
  rw [key_parse]
  cases h : int (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]

theorem dictItems_int_quoted_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char)  :
    dictItems (fuel + 1) acc (("int " ++ q key ++ " = ").toList ++ rest) =
      (do let (i, r) ← int (' ' :: rest); dictItems fuel ((key, .int i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("int ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) =
      some ("int", ' ' :: ((q key).toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("int ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) = some 'i' := rfl
  rw [dictItems]
  simp only [peekv, show (some 'i' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  have string_key : str (' ' :: ((q key).toList ++ ' ' :: '=' :: ' ' :: rest)) =
      some (key, ' ' :: '=' :: ' ' :: rest) :=
    (str_padding [' '] _ (by simp [isWs])).trans (str_print key _)
  simp only [Bind.bind, Option.bind_some]
  rw [string_key]
  cases h : int (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]

theorem dictItems_str_quoted_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char)  :
    dictItems (fuel + 1) acc (("string " ++ q key ++ " = ").toList ++ rest) =
      (do let (i, r) ← str (' ' :: rest); dictItems fuel ((key, .str i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("string ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) =
      some ("string", ' ' :: ((q key).toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("string ".toList ++ ((q key).toList ++ (" = ".toList ++ rest))) = some 's' := rfl
  rw [dictItems]
  simp only [peekv, show (some 's' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  have string_key : str (' ' :: ((q key).toList ++ ' ' :: '=' :: ' ' :: rest)) =
      some (key, ' ' :: '=' :: ' ' :: rest) :=
    (str_padding [' '] _ (by simp [isWs])).trans (str_print key _)
  simp only [Bind.bind, Option.bind_some]
  rw [string_key]
  cases h : str (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]

theorem dictItems_int_step (n fuel : Nat) (acc : List (String × DVal))
    (key : String) (i : Int) (rest : List Char)  :
    dictItems (fuel + 1) acc ((DVal.printItem n (key, .int i)).toList ++ rest) =
      dictItems fuel ((key, .int i) :: acc) ('\n' :: rest) := by
  simp only [DVal.printItem, String.toList_append, List.append_assoc]
  rw [dictItems_padding (fuel + 1) acc (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  have header := dictItems_int_quoted_prefix fuel acc key ((toString i ++ "\n").toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at header
  rw [header]
  have number := int_print i rest
  have strip : int (' ' :: ((toString i).toList ++ '\n' :: rest)) =
      int ((toString i).toList ++ '\n' :: rest) := by rfl
  simp only [show "\n".toList = ['\n'] from rfl, List.cons_append, List.nil_append]
  rw [strip, number]
  rfl

theorem dictItems_str_prefix (fuel : Nat) (acc : List (String × DVal))
    (key : String) (rest : List Char) (valid : IdentLexeme key) :
    dictItems (fuel + 1) acc (("string " ++ key ++ " = ").toList ++ rest) =
      (do let (i, r) ← str (' ' :: rest); dictItems fuel ((key, .str i) :: acc) r) := by
  simp only [String.toList_append, List.append_assoc]
  have token : ident ("string ".toList ++ (key.toList ++ (" = ".toList ++ rest))) =
      some ("string", ' ' :: (key.toList ++ (" = ".toList ++ rest))) := rfl
  have peekv : peek ("string ".toList ++ (key.toList ++ (" = ".toList ++ rest))) = some 's' := rfl
  rw [dictItems]
  simp only [peekv, show (some 's' == some '}') = false from rfl, Bool.false_eq_true,
    ↓reduceIte, token]
  have key_parse := dict_key_print key ('=' :: ' ' :: rest) valid
  simp only [show " = ".toList = [' ', '=', ' '] from rfl,
    List.cons_append, List.nil_append]
  change ((match str (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest)) with
    | some x => some x | none => ident (' ' :: (key.toList ++ ' ' :: '=' :: ' ' :: rest))).bind
      (fun kr => (lit "=" kr.2).bind (fun eqr =>
        ((str eqr.2).bind (fun ir => some (DVal.str ir.1, ir.2))).bind
          (fun vr => dictItems fuel ((kr.1, vr.1) :: acc) vr.2)))) = _
  rw [key_parse]
  cases h : str (' ' :: rest) <;> simp [lit, ws, isWs, show "=".length = 1 from rfl, h]

theorem dictItems_str_step (n fuel : Nat) (acc : List (String × DVal))
    (key : String) (i : String) (rest : List Char)  :
    dictItems (fuel + 1) acc ((DVal.printItem n (key, .str i)).toList ++ rest) =
      dictItems fuel ((key, .str i) :: acc) ('\n' :: rest) := by
  simp only [DVal.printItem, String.toList_append, List.append_assoc]
  rw [dictItems_padding (fuel + 1) acc (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  have header := dictItems_str_quoted_prefix fuel acc key ((q i ++ "\n").toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at header
  rw [header]
  have number := str_print i ('\n' :: rest)
  have strip : str (' ' :: ((q i).toList ++ '\n' :: rest)) =
      str ((q i).toList ++ '\n' :: rest) := by rfl
  simp only [show "\n".toList = ['\n'] from rfl, List.cons_append, List.nil_append]
  rw [strip, number]
  rfl

def PrimitiveEntryWellFormed (entry : String × DVal) : Prop :=
  match entry.2 with | .int _ | .str _ => True | .dict _ => False

theorem dictItems_primitives (n : Nat) (xs acc : List (String × DVal))
    (padding closing rest : List Char) (fuel : Nat)
    (pad_ws : ∀ c ∈ padding, isWs c = true) (close_ws : ∀ c ∈ closing, isWs c = true)
    (valid : ∀ entry ∈ xs, PrimitiveEntryWellFormed entry) (enough : xs.length + 1 ≤ fuel) :
    dictItems fuel acc (padding ++ (DVal.printItems n xs).toList ++ closing ++ '}' :: rest) =
      some (acc.reverse ++ xs, rest) := by
  induction xs generalizing acc padding fuel with
  | nil =>
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have strip : ws (padding ++ (closing ++ '}' :: rest)) = '}' :: rest := by
        unfold ws
        rw [dropWhile_prefix isWs padding _ pad_ws, dropWhile_prefix isWs closing _ close_ws]
        rfl
      simp [DVal.printItems, dictItems, peek, lit, strip, List.append_assoc,
        show "}".length = 1 from rfl]
  | cons entry xs ih =>
    have head_valid := valid entry (by simp)
    have tail_valid : ∀ e ∈ xs, PrimitiveEntryWellFormed e := fun e he => valid e (by simp [he])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      rcases entry with ⟨key, value⟩
      have stepfuel : xs.length + 1 ≤ fuel := by simp at enough; omega
      simp only [DVal.printItems, String.toList_append, List.append_assoc]
      rw [dictItems_padding (fuel + 1) acc padding _ pad_ws]
      cases value with
      | dict d => exact False.elim head_valid
      | int i =>
        rw [dictItems_int_step n fuel acc key i _]
        have next := ih ((key, .int i) :: acc) ['\n'] fuel (by simp [isWs]) tail_valid stepfuel
        simpa [List.reverse_cons, List.append_assoc] using next
      | str value =>
        rw [dictItems_str_step n fuel acc key value _]
        have next := ih ((key, .str value) :: acc) ['\n'] fuel (by simp [isWs]) tail_valid stepfuel
        simpa [List.reverse_cons, List.append_assoc] using next

theorem dict_print_chars (n : Nat) (xs : List (String × DVal)) (rest : List Char) :
    (DVal.printDict n xs).toList ++ rest =
      '{' :: '\n' :: ((DVal.printItems (n + 1) xs).toList ++ (ind n).toList ++ '}' :: rest) := by
  cases xs <;> simp [DVal.printDict, DVal.printItems, String.toList_append, List.append_assoc,
    show "{\n".toList = ['{', '\n'] from rfl, show "}".toList = ['}'] from rfl]

theorem dict_primitives (n : Nat) (xs : List (String × DVal)) (rest : List Char)
    (fuel : Nat) (valid : ∀ entry ∈ xs, PrimitiveEntryWellFormed entry)
    (enough : xs.length + 2 ≤ fuel) :
    dict fuel ((DVal.printDict n xs).toList ++ rest) = some (xs, rest) := by
  rw [dict_print_chars]
  cases fuel with
  | zero => simp at enough
  | succ fuel =>
    change dictItems fuel [] ('\n' :: ((DVal.printItems (n + 1) xs).toList ++ (ind n).toList ++ '}' :: rest)) = _
    have closing : ∀ c ∈ (ind n).toList, isWs c = true := by
      intro c hc
      simp only [ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    have result := dictItems_primitives (n + 1) xs [] ['\n'] (ind n).toList rest fuel
      (by simp [isWs]) closing valid (by omega)
    simpa only [List.cons_append, List.nil_append, List.reverse_nil, List.append_assoc] using result

theorem dictItems_length_ge (n : Nat) (xs : List (String × DVal)) :
    xs.length ≤ (DVal.printItems n xs).length := by
  induction xs with
  | nil => simp
  | cons entry xs ih =>
    rcases entry with ⟨key, value⟩
    cases value <;> simp only [DVal.printItems, DVal.printItem, String.length_append,
      List.length_cons, show "\n".length = 1 from rfl] <;> omega

theorem primMeta_customData_prefix (n : Nat) (tail : List Char) :
    primMeta ((ind n ++ "customData = ").toList ++ tail) =
      (dict ((' ' :: tail).length + 1) (' ' :: tail)).bind
        (fun result => some (.customData result.1, result.2)) := by
  simp only [String.toList_append, List.append_assoc]
  rw [primMeta_padding (ind n).toList _ (by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl)]
  simp [primMeta, qual, ident, ws, isWs, isIdStart, isIdCont, lit,
    show "=".length = 1 from rfl]

theorem dict_padding (fuel : Nat) (padding cs : List Char)
    (pad : ∀ c ∈ padding, isWs c = true) :
    dict fuel (padding ++ cs) = dict fuel cs := by
  cases fuel <;> simp [dict, lit_padding "{" padding cs pad]

theorem primMeta_customData_primitives_print (n : Nat) (xs : List (String × DVal))
    (rest : List Char) (valid : ∀ entry ∈ xs, PrimitiveEntryWellFormed entry) :
    primMeta (((PrimMeta.customData xs).print n).toList ++ rest) = some (.customData xs, rest) := by
  have front := primMeta_customData_prefix n ((DVal.printDict n xs).toList ++ rest)
  simp only [String.toList_append, List.append_assoc] at front
  simp only [PrimMeta.print, String.toList_append, List.append_assoc]
  rw [front]
  have count := dictItems_length_ge (n + 1) xs
  have enough : xs.length + 2 ≤ (' ' :: ((DVal.printDict n xs).toList ++ rest)).length + 1 := by
    rw [dict_print_chars]
    simp only [List.length_cons, List.length_append, String.length_toList] at *
    omega
  have strip := dict_padding ((' ' :: ((DVal.printDict n xs).toList ++ rest)).length + 1)
    [' '] ((DVal.printDict n xs).toList ++ rest) (by simp [isWs])
  simp only [List.cons_append, List.nil_append] at strip
  rw [strip, dict_primitives n xs rest _ valid enough]
  rfl
end UsdSpec.Usda
