import UsdSpec.Usda.RecursiveMetadataProofs
/-!
# General nested prim and body recovery for the operational USDA subset

Prim.WellFormed retains lexical conditions for optional types, metadata and
attributes recursively, with arbitrary quoted names. No nesting, sibling-count,
numeric-magnitude or string-length bound is imposed. prim_recovery uses direct
simultaneous induction on actual prim/primItems fuel; printed length dominates
the conservative recursive cost. Top-level prim-list and Layer assembly remain
separate obligations. These are subset inverses, not full-PEG or composition
validity claims. Executable parser/printer definitions are unchanged.
-/
namespace UsdSpec.Usda
mutual
 def Prim.parserCost : Prim → Nat
   | .mk _ _ _ _ items => Prim.itemsCost items + 1
 def Prim.itemsCost : List (Sum (String × Sum Num (List (Num × Num))) Prim) → Nat
   | [] => 1
   | .inl _ :: rest => Prim.itemsCost rest + 1
   | .inr child :: rest => Prim.parserCost child + Prim.itemsCost rest + 1
end
mutual
 def Prim.WellFormed : Prim → Prop
   | .mk _ ty _ metadata items =>
       (∀ t ∈ ty, IdentLexeme t) ∧ (∀ m ∈ metadata, m.WellFormed) ∧ Prim.itemsWellFormed items
 def Prim.itemsWellFormed : List (Sum (String × Sum Num (List (Num × Num))) Prim) → Prop
   | [] => True
   | .inl a :: rest => AttrWellFormed a ∧ Prim.itemsWellFormed rest
   | .inr child :: rest => child.WellFormed ∧ Prim.itemsWellFormed rest
end

theorem peek_q (name : String) (rest : List Char) :
    peek ((q name).toList ++ rest) = some '"' := by
  simp only [peek, q, quoteString, String.toList_ofList, List.cons_append,
    ws_cons _ _ (show isWs '"' = false from rfl), List.head?_cons]

theorem peek_ident_not_quote (ty : String) (rest : List Char) (valid : IdentLexeme ty) :
    peek (ty.toList ++ rest) ≠ some '"' := by
  rcases valid with ⟨c, cs, spelling, start, cont⟩
  have first := ident_first_not_ws c start
  have no_quote : c ≠ '"' := by intro h; subst c; simp [isIdStart] at start
  simpa only [peek, spelling, List.cons_append, ws_cons _ _ first, List.head?_cons,
    ne_eq, Option.some.injEq] using no_quote

theorem peek_space (cs : List Char) : peek (' ' :: cs) = peek cs := by rfl

theorem str_space (cs : List Char) : str (' ' :: cs) = str cs := by rfl

theorem type_name_print {α : Type} (ty : Option String) (name : String) (rest : List Char)
    (next : Option String → String → List Char → Option α)
    (valid : ∀ t ∈ ty, IdentLexeme t) :
    let cs := ' ' :: (((ty.map (· ++ " ")).getD "" ++ q name).toList ++ rest)
    ((if peek cs == some '"' then some (none, cs) else do
        let (t, r) ← ident cs; some (some t, r)).bind
      (fun tr => (str tr.2).bind (fun nr => next tr.1 nr.1 nr.2))) = next ty name rest := by
  cases ty with
  | none =>
    dsimp only
    simp only [Option.map, Option.getD, String.empty_append]
    have peekv : peek (' ' :: ((q name).toList ++ rest)) = some '"' := by
      rw [peek_space, peek_q]
    have string : str (' ' :: ((q name).toList ++ rest)) = some (name, rest) :=
      (str_space _).trans (str_print name rest)
    simp only [peekv, BEq.rfl, ↓reduceIte]
    simp only [Option.bind, string]
  | some ty =>
    have ty_valid := valid ty (by simp)
    dsimp only
    simp only [Option.map, Option.getD]
    simp only [String.toList_append, show " ".toList = [' '] from rfl,
      List.append_assoc, List.cons_append, List.nil_append]
    have peekv : peek (' ' :: (ty.toList ++ ' ' :: ((q name).toList ++ rest))) ≠ some '"' := by
      simpa only [peek_space] using peek_ident_not_quote ty (' ' :: ((q name).toList ++ rest)) ty_valid
    have identifier : ident (' ' :: (ty.toList ++ ' ' :: ((q name).toList ++ rest))) =
        some (ty, ' ' :: ((q name).toList ++ rest)) := by
      simpa only [ident_space] using ident_print ty ' ' ((q name).toList ++ rest) ty_valid (by decide)
    have string : str (' ' :: ((q name).toList ++ rest)) = some (name, rest) :=
      (str_space _).trans (str_print name rest)
    simp only [beq_iff_eq, peekv, ↓reduceIte]
    rw [identifier]
    simp only [Bind.bind, Option.bind, string]

def metadataText (n : Nat) (mds : List PrimMeta) : String :=
  if mds.isEmpty then "" else
    " (\n" ++ String.join (mds.map (fun m => m.print (n + 1) ++ "\n")) ++ ind n ++ ")"

theorem metadata_body_print {α : Type} (n : Nat) (mds : List PrimMeta) (body : List Char)
    (next : List PrimMeta → List Char → Option α) (valid : ∀ m ∈ mds, m.WellFormed) :
    let cs := (metadataText n mds ++ "\n" ++ ind n ++ "{\n").toList ++ body
    ((if peek cs == some '(' then do
        let ((), r) ← lit "(" cs; primMetas (r.length + 1) [] r
      else some ([], cs)).bind (fun mr => (lit "{" mr.2).bind (fun br => next mr.1 br.2))) =
      next mds ('\n' :: body) := by
  cases mds with
  | nil =>
    dsimp only
    simp [metadataText, String.toList_append, List.append_assoc, peek, lit,
      ws_newline, ws_ind, ws_cons _ _ (show isWs '{' = false from rfl),
      show "\n".toList = ['\n'] from rfl, show "{\n".toList = ['{', '\n'] from rfl,
      show "{".length = 1 from rfl, Option.bind]
  | cons m mds =>
    dsimp only
    let tail := ("\n" ++ ind n ++ "{\n").toList ++ body
    let contents := '\n' ::
      (String.join ((m :: mds).map (fun m => m.print (n + 1) ++ "\n")) ++ ind n ++ ")").toList ++ tail
    have parsed_metadata := primMetas_print_text n (m :: mds) tail valid
    have meta' : primMetas (contents.length + 1) [] contents = some (m :: mds, tail) := parsed_metadata
    have frame : (metadataText n (m :: mds) ++ "\n" ++ ind n ++ "{\n").toList ++ body =
        ' ' :: '(' :: contents := by
      simp [metadataText, contents, tail, String.toList_append, List.append_assoc]
    rw [frame]
    have open_peek : peek (' ' :: '(' :: contents) = some '(' := rfl
    have open_lit : lit "(" (' ' :: '(' :: contents) = some ((), contents) := rfl
    simp only [open_peek, BEq.rfl, ↓reduceIte, open_lit, Bind.bind, Option.bind]
    rw [meta']
    have brace : lit "{" tail = some ((), '\n' :: body) := by
      simp [tail, String.toList_append, lit, ws_newline, ws_ind,
        ws_cons _ _ (show isWs '{' = false from rfl),
        show "\n".toList = ['\n'] from rfl, show "{\n".toList = ['{', '\n'] from rfl,
        show "{".length = 1 from rfl]
    simp only [brace]

theorem specifier_padding (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    specifier (padding ++ cs) = specifier cs := by
  unfold specifier
  rw [ident_padding padding cs pad]

theorem prim_print_prefix (n fuel : Nat) (sp : Specifier) (ty : Option String) (name : String)
    (mds : List PrimMeta) (items : List (Sum (String × Sum Num (List (Num × Num))) Prim))
    (rest : List Char) (type_valid : ∀ t ∈ ty, IdentLexeme t)
    (meta_valid : ∀ m ∈ mds, m.WellFormed) :
    prim (fuel + 1) ((Prim.print n (.mk sp ty name mds items)).toList ++ rest) =
      (primItems fuel [] ('\n' :: ((Prim.printItems (n + 1) items).toList ++
          (ind n).toList ++ '}' :: '\n' :: rest))).bind
        (fun ir => some (.mk sp ty name mds ir.1, ir.2)) := by
  let body := (Prim.printItems (n + 1) items).toList ++ (ind n).toList ++ '}' :: '\n' :: rest
  let after_name := (metadataText n mds ++ "\n" ++ ind n ++ "{\n").toList ++ body
  let after_spec := ' ' :: (((ty.map (· ++ " ")).getD "" ++ q name).toList ++ after_name)
  have frame : (Prim.print n (.mk sp ty name mds items)).toList ++ rest =
      (ind n).toList ++ (sp.print.toList ++ ' ' ::
        (((ty.map (· ++ " ")).getD "" ++ q name).toList ++ after_name)) := by
    simp [Prim.print, metadataText, after_name, body, String.toList_append, List.append_assoc]
  have indent : ∀ c ∈ (ind n).toList, isWs c = true := by
    intro c hc
    simp only [ind, String.toList_ofList, List.mem_replicate] at hc
    rcases hc with ⟨_, rfl⟩
    rfl
  have spec : specifier ((ind n).toList ++ (sp.print.toList ++ ' ' ::
      (((ty.map (· ++ " ")).getD "" ++ q name).toList ++ after_name))) =
        some (sp, after_spec) :=
    (specifier_padding _ _ indent).trans (specifier_print_general sp _)
  rw [frame, prim]
  dsimp only
  rw [spec]
  simp only [Bind.bind, Option.bind_some]
  let next := fun ty name cs =>
    ((if peek cs == some '(' then do
      let ((), r) ← lit "(" cs; primMetas (r.length + 1) [] r
    else some ([], cs)).bind fun mr =>
      (lit "{" mr.2).bind fun br => (primItems fuel [] br.2).bind fun ir =>
        some (Prim.mk sp ty name mr.1 ir.1, ir.2))
  have type_name := type_name_print ty name after_name next type_valid
  change ((if peek after_spec == some '"' then some (none, after_spec) else do
      let (t, r) ← ident after_spec; some (some t, r)).bind
      (fun tr => (str tr.2).bind (fun nr => next tr.1 nr.1 nr.2))) = _
  rw [type_name]
  have metadata_body := metadata_body_print n mds body
    (fun metadata cs => (primItems fuel [] cs).bind
      (fun ir => some (Prim.mk sp ty name metadata ir.1, ir.2))) meta_valid
  exact metadata_body

theorem prim_padding (fuel : Nat) (padding cs : List Char) (pad : ∀ c ∈ padding, isWs c = true) :
    prim fuel (padding ++ cs) = prim fuel cs := by
  cases fuel <;> simp [prim, specifier_padding padding cs pad]

theorem prim_print_start (p : Prim) (n : Nat) (rest : List Char) :
    peek ((p.print n).toList ++ rest) ≠ some '}' ∧
      ∃ word tail, ident ((p.print n).toList ++ rest) = some (word, tail) ∧ word ≠ "double" := by
  cases p with
  | mk sp ty name mds items =>
    have indent : ∀ c ∈ (ind n).toList, isWs c = true := by
      intro c hc
      simp only [ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    simp only [Prim.print, String.toList_append, List.append_assoc]
    rw [peek_padding (ind n).toList _ indent, ident_padding (ind n).toList _ indent]
    cases sp <;> simp [Specifier.print, peek, ident, ws, isWs, isIdStart, isIdCont]

theorem primItems_printPrim (n fuel : Nat) (p : Prim)
    (acc : List (Sum (String × Sum Num (List (Num × Num))) Prim)) (padding rest : List Char)
    (pad : ∀ c ∈ padding, isWs c = true) :
    primItems (fuel + 1) acc (padding ++ (p.print n).toList ++ rest) =
      (prim fuel ((p.print n).toList ++ rest)).bind
        (fun pr => primItems fuel (Sum.inr pr.1 :: acc) pr.2) := by
  rcases prim_print_start p n rest with ⟨not_close, word, tail, identifier, not_double⟩
  simp only [List.append_assoc]
  rw [primItems]
  dsimp only
  rw [peek_padding padding _ pad]
  simp only [beq_iff_eq, not_close, ↓reduceIte]
  rw [ident_padding padding _ pad, identifier]
  split
  · rename_i h
    exact False.elim (not_double (congrArg Prod.fst (Option.some.inj h)))
  · rw [prim_padding fuel padding _ pad]
    rfl

theorem Prim.parserCost_pos (p : Prim) : 1 ≤ p.parserCost := by
  cases p; simp [Prim.parserCost]

theorem Prim.itemsCost_pos (xs : List (Sum (String × Sum Num (List (Num × Num))) Prim)) :
    1 ≤ Prim.itemsCost xs := by
  cases xs with
  | nil => simp [Prim.itemsCost]
  | cons x xs => cases x <;> simp [Prim.itemsCost]

theorem prim_recovery (fuel : Nat) :
    (∀ (p : Prim) (n : Nat) (rest : List Char), p.WellFormed → p.parserCost ≤ fuel →
      prim fuel ((p.print n).toList ++ rest) = some (p, '\n' :: rest)) ∧
    (∀ (xs : List (Sum (String × Sum Num (List (Num × Num))) Prim)) (n : Nat)
      (acc : List (Sum (String × Sum Num (List (Num × Num))) Prim))
      (padding closing rest : List Char),
      (∀ c ∈ padding, isWs c = true) → (∀ c ∈ closing, isWs c = true) →
      Prim.itemsWellFormed xs → Prim.itemsCost xs ≤ fuel →
      primItems fuel acc (padding ++ (Prim.printItems n xs).toList ++ closing ++ '}' :: rest) =
        some (acc.reverse ++ xs, rest)) := by
  induction fuel with
  | zero =>
    constructor
    · intro p n rest valid enough
      have := p.parserCost_pos
      omega
    · intro xs n acc padding closing rest pad close valid enough
      have := Prim.itemsCost_pos xs
      omega
  | succ fuel ih =>
    constructor
    · intro p n rest valid enough
      cases p with
      | mk sp ty name mds items =>
        simp only [Prim.WellFormed] at valid
        have body_enough : Prim.itemsCost items ≤ fuel := by
          simp only [Prim.parserCost] at enough
          omega
        rw [prim_print_prefix n fuel sp ty name mds items rest valid.1 valid.2.1]
        have indent : ∀ c ∈ (ind n).toList, isWs c = true := by
          intro c hc
          simp only [ind, String.toList_ofList, List.mem_replicate] at hc
          rcases hc with ⟨_, rfl⟩
          rfl
        have body := ih.2 items (n + 1) [] ['\n'] (ind n).toList ('\n' :: rest)
          (by simp [isWs]) indent valid.2.2 body_enough
        simp only [List.cons_append, List.nil_append, List.append_assoc,
          List.reverse_nil] at body
        simp only [List.append_assoc]
        rw [body]
        rfl
    · intro xs n acc padding closing rest pad close valid enough
      cases xs with
      | nil =>
        have strip : ws (padding ++ (closing ++ '}' :: rest)) = '}' :: rest := by
          unfold ws
          rw [dropWhile_prefix isWs padding _ pad, dropWhile_prefix isWs closing _ close]
          rfl
        simp [Prim.printItems, primItems, peek, lit, strip, List.append_assoc,
          show "}".length = 1 from rfl]
      | cons item xs =>
        cases item with
        | inl a =>
          simp only [Prim.itemsWellFormed] at valid
          have tail_enough : Prim.itemsCost xs ≤ fuel := by simp only [Prim.itemsCost] at enough; omega
          simp only [Prim.printItems, String.toList_append, List.append_assoc]
          have step := primItems_printAttr n fuel a acc
            ((Prim.printItems n xs).toList ++ (closing ++ '}' :: rest)) padding valid.1 pad
          simp only [List.append_assoc] at step
          rw [step]
          have tail := ih.2 xs n (Sum.inl a :: acc) ['\n'] closing rest
            (by simp [isWs]) close valid.2 tail_enough
          simpa [List.reverse_cons, List.append_assoc] using tail
        | inr p =>
          simp only [Prim.itemsWellFormed] at valid
          have child_enough : p.parserCost ≤ fuel := by simp only [Prim.itemsCost] at enough; omega
          have tail_enough : Prim.itemsCost xs ≤ fuel := by simp only [Prim.itemsCost] at enough; omega
          simp only [Prim.printItems, String.toList_append, List.append_assoc]
          have step := primItems_printPrim n fuel p acc padding
            ((Prim.printItems n xs).toList ++ (closing ++ '}' :: rest)) pad
          simp only [List.append_assoc] at step
          rw [step, ih.1 p n _ valid.1 child_enough]
          have tail := ih.2 xs n (Sum.inr p :: acc) ['\n'] closing rest
            (by simp [isWs]) close valid.2 tail_enough
          simpa [Option.bind, List.reverse_cons, List.append_assoc] using tail

theorem printAttr_length_pos (n : Nat) (a : String × Sum Num (List (Num × Num))) :
    1 ≤ (printAttr n a).length := by
  rcases a with ⟨name, value⟩
  cases value <;> simp only [printAttr, String.length_append,
    show "\n".length = 1 from rfl, show "}\n".length = 2 from rfl] <;> omega

mutual
theorem Prim.print_cost_bound (p : Prim) (n : Nat) : p.parserCost + 1 ≤ (p.print n).length := by
  cases p with
  | mk sp ty name mds items =>
    have child := Prim.items_print_cost_bound items (n + 1)
    simp only [Prim.parserCost, Prim.print, String.length_append,
      show "\n".length = 1 from rfl, show "{\n".length = 2 from rfl,
      show "}\n".length = 2 from rfl]
    omega
termination_by sizeOf p

theorem Prim.items_print_cost_bound
    (xs : List (Sum (String × Sum Num (List (Num × Num))) Prim)) (n : Nat) :
    Prim.itemsCost xs ≤ (Prim.printItems n xs).length + 1 := by
  cases xs with
  | nil => simp [Prim.itemsCost, Prim.printItems]
  | cons item xs =>
    have tail := Prim.items_print_cost_bound xs n
    cases item with
    | inl a =>
      have head := printAttr_length_pos n a
      simp only [Prim.itemsCost, Prim.printItems, String.length_append]
      omega
    | inr p =>
      have head := Prim.print_cost_bound p n
      simp only [Prim.itemsCost, Prim.printItems, String.length_append]
      omega
termination_by sizeOf xs
end

theorem prim_print (p : Prim) (n : Nat) (rest : List Char) (valid : p.WellFormed) :
    prim (((p.print n).toList ++ rest).length + 1) ((p.print n).toList ++ rest) =
      some (p, '\n' :: rest) := by
  apply (prim_recovery _).1 p n rest valid
  have bound := p.print_cost_bound n
  simp only [List.length_append, String.length_toList]
  omega
end UsdSpec.Usda
