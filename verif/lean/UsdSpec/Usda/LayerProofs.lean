import UsdSpec.Usda.PrimProofs
/-!
# Operational Layer parser/printer recovery under explicit lexical conditions

Layer.WellFormed composes the existing sublayer, metadata, optional type and
attribute lexical conditions. Arbitrary prim nesting, list counts and quoted
names/values are supported, with actual input-length fuel and EOF checks.
Dictionary keys are arbitrary Strings: the printer quotes them, and
EntriesWellFormed_all proves their admissibility at every depth. The remaining
conditions describe the supported scanners (identifiers/numeric lexemes) and
asset/path delimiters; this is not a claim about the full PEG or semantic
composition validity.
-/
namespace UsdSpec.Usda

def primsBody (xs : List Prim) : List Char := xs.flatMap (fun p => '\n' :: (p.print 0).toList)

theorem prim_print_not_empty (p : Prim) (n : Nat) (rest : List Char) :
    ws ((p.print n).toList ++ rest) ≠ [] := by
  rcases (prim_print_start p n rest).2 with ⟨word, tail, header, _⟩
  intro empty
  unfold ident at header
  rw [empty] at header
  cases header

theorem prims_print (xs acc : List Prim) (padding closing : List Char) (fuel : Nat)
    (pad : ∀ c ∈ padding, isWs c = true) (close : ∀ c ∈ closing, isWs c = true)
    (valid : ∀ p ∈ xs, p.WellFormed) (enough : xs.length + 1 ≤ fuel) :
    prims fuel acc (padding ++ primsBody xs ++ closing) = some (acc.reverse ++ xs, []) := by
  induction xs generalizing acc padding fuel with
  | nil =>
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      have strip : ws (padding ++ closing) = [] := by
        unfold ws
        rw [dropWhile_prefix isWs padding closing pad]
        simpa only [List.append_nil, List.dropWhile_nil] using dropWhile_prefix isWs closing [] close
      simp [primsBody, prims, strip]
  | cons p xs ih =>
    have head_valid := valid p (by simp)
    have tail_valid : ∀ x ∈ xs, x.WellFormed := fun x hx => valid x (by simp [hx])
    cases fuel with
    | zero => simp at enough
    | succ fuel =>
      let tail := primsBody xs ++ closing
      let input := padding ++ '\n' :: ((p.print 0).toList ++ tail)
      have strip : ws input = ws ((p.print 0).toList ++ tail) := by
        unfold input ws
        rw [dropWhile_prefix isWs padding _ pad]
        rfl
      have nonempty : ws input ≠ [] := strip ▸ prim_print_not_empty p 0 tail
      have bound := p.print_cost_bound 0
      have parse_bound : p.parserCost ≤ input.length + 1 := by
        simp only [input, List.length_append, List.length_cons, String.length_toList]
        omega
      have child : prim (input.length + 1) input = some (p, '\n' :: tail) := by
        unfold input
        rw [prim_padding _ padding _ pad]
        have newline := prim_padding (input.length + 1) ['\n'] ((p.print 0).toList ++ tail)
          (by simp [isWs])
        simp only [List.cons_append, List.nil_append] at newline
        rw [newline]
        exact (prim_recovery _).1 p 0 tail head_valid parse_bound
      simp only [primsBody, List.flatMap_cons, List.append_assoc, List.cons_append]
      change prims (fuel + 1) acc input = _
      rw [prims]
      simp only [nonempty, ↓reduceIte, child]
      have next := ih (p :: acc) ['\n'] fuel (by simp [isWs]) tail_valid (by simp at enough; omega)
      simpa [tail, Option.bind, List.reverse_cons, List.append_assoc] using next

theorem primsBody_text (xs : List Prim) :
    (String.join (xs.map (fun p => "\n" ++ p.print 0))).toList = primsBody xs := by
  simp [String.toList_join, List.flatMap_map, String.toList_append, primsBody]

theorem primsBody_length_ge (xs : List Prim) : xs.length ≤ (primsBody xs).length := by
  induction xs with
  | nil => simp [primsBody]
  | cons p xs ih =>
    simp only [primsBody, List.flatMap_cons, List.length_append, List.length_cons, List.length_cons] at *
    omega

theorem subLayer_bracket_body (n : Nat) (xs : List SubLayer) (rest : List Char)
    (valid : ∀ s ∈ xs, s.WellFormed) :
    (do let ((), r) ← lit "[" ((bracket n (xs.map SubLayer.print)).toList ++ rest)
        listUntil subLayer (r.length + 1) [] r) = some (xs, rest) := by
  by_cases empty : xs = []
  · subst xs
    simp [bracket, lit, ws, isWs, listUntil, peek, show "[".length = 1 from rfl,
      show "]".length = 1 from rfl]
  · rw [bracket_chars SubLayer.print n xs rest empty]
    let emit := fun s : SubLayer => (ind (n + 1) ++ s.print).toList
    let closing := (ind n).toList
    let tail := closing ++ ']' :: rest
    change listUntil subLayer (('\n' :: commaBody emit xs tail).length + 1) []
      ('\n' :: commaBody emit xs tail) = some (xs, rest)
    have pad : ∀ c ∈ closing, isWs c = true := by
      intro c hc
      simp only [closing, ind, String.toList_ofList, List.mem_replicate] at hc
      rcases hc with ⟨_, rfl⟩
      rfl
    have count := commaBody_length_ge emit xs tail
    have result := listUntil_print subLayer emit SubLayer.WellFormed
      (fun s valid rest => by
        have strip := subLayer_padding ('\n' :: (ind (n + 1)).toList)
          (s.print.toList ++ ',' :: rest) (newline_ind_ws (n + 1))
        simpa only [emit, String.toList_append, List.cons_append, List.append_assoc] using
          strip.trans (subLayer_print s rest valid))
      (fun s valid => by
        simpa only [emit, String.toList_append, ws_ind] using subLayer_print_start s)
      xs [] closing rest (('\n' :: commaBody emit xs tail).length + 1) pad
      (by simp only [List.length_cons]; omega) valid
    simpa only [List.reverse_nil, List.nil_append] using result


theorem subLayer_bracket_next {α : Type} (n : Nat) (xs : List SubLayer) (rest : List Char)
    (next : List SubLayer → List Char → Option α) (valid : ∀ s ∈ xs, s.WellFormed) :
    (do let ((), r) ← lit "[" ((bracket n (xs.map SubLayer.print)).toList ++ rest)
        let (ss, r) ← listUntil subLayer (r.length + 1) [] r
        next ss r) = next xs rest := by
  have result := congrArg (fun parsed => parsed.bind (fun sr => next sr.1 sr.2))
    (subLayer_bracket_body n xs rest valid)
  simpa only [Bind.bind, Option.bind_assoc, Option.bind_some] using result

def subLayersText (xs : List SubLayer) : String :=
  "(\n" ++ ind 1 ++ "subLayers = " ++ bracket 1 (xs.map SubLayer.print) ++ "\n)\n"

theorem layerSubs_some (xs : List SubLayer) (tail : List Char)
    (valid : ∀ s ∈ xs, s.WellFormed) :
    let cs := '\n' :: ((subLayersText xs).toList ++ tail)
    (do let ((), r) ← lit "(" cs
        match ident r with
        | some ("subLayers", r) => do
          let ((), r) ← lit "=" r
          let ((), r) ← lit "[" r
          let (ss, r) ← listUntil subLayer (r.length + 1) [] r
          let ((), r) ← lit ")" r
          some (some ss, r)
        | _ => do let ((), r) ← lit ")" r; some (some [], r)) =
      some (some xs, '\n' :: tail) := by
  dsimp only
  simp only [subLayersText, String.toList_append, List.append_assoc]
  let contents := (bracket 1 (xs.map SubLayer.print)).toList ++ '\n' :: ')' :: '\n' :: tail
  have token : ident ('\n' :: ((ind 1).toList ++ "subLayers = ".toList ++ contents)) =
      some ("subLayers", ' ' :: '=' :: ' ' :: contents) := rfl
  have open_lit : lit "(" ('\n' :: ("(\n".toList ++ (ind 1).toList ++ "subLayers = ".toList ++ contents)) =
      some ((), '\n' :: ((ind 1).toList ++ "subLayers = ".toList ++ contents)) := rfl
  have frame : '\n' :: (("(\n").toList ++ ((ind 1).toList ++
      ("subLayers = ".toList ++ ((bracket 1 (xs.map SubLayer.print)).toList ++ ("\n)\n".toList ++ tail))))) =
      '\n' :: ("(\n".toList ++ (ind 1).toList ++ "subLayers = ".toList ++ contents) := by
    simp [contents, List.append_assoc]
  rw [frame, open_lit]
  simp only [Bind.bind, Option.bind_some, token]
  have eq_lit : lit "=" (' ' :: '=' :: ' ' :: contents) = some ((), ' ' :: contents) := rfl
  rw [eq_lit]
  simp only [Option.bind_some]
  have bracket_lit : lit "[" (' ' :: contents) = lit "[" contents := by rfl
  rw [bracket_lit]
  have parsed := subLayer_bracket_next 1 xs ('\n' :: ')' :: '\n' :: tail)
    (fun ss r => do let ((), r) ← lit ")" r; some (some ss, r)) valid
  exact parsed

theorem prim_print_not_open (p : Prim) (rest : List Char) :
    peek ((p.print 0).toList ++ rest) ≠ some '(' := by
  cases p with
  | mk sp ty name mds items =>
    simp only [Prim.print, String.toList_append, List.append_assoc, peek, ws_ind]
    cases sp <;> simp [Specifier.print, ws, isWs]

theorem primsBody_not_open (xs : List Prim) : peek ('\n' :: primsBody xs) ≠ some '(' := by
  cases xs with
  | nil => simp [primsBody, peek, ws, isWs]
  | cons p xs =>
    simpa only [primsBody, List.flatMap_cons, List.cons_append, peek, ws_newline] using
      prim_print_not_open p (primsBody xs)

theorem prims_print_text (xs : List Prim) (valid : ∀ p ∈ xs, p.WellFormed) :
    prims (('\n' :: primsBody xs).length + 1) [] ('\n' :: primsBody xs) = some (xs, []) := by
  have count := primsBody_length_ge xs
  have result := prims_print xs [] ['\n'] [] (('\n' :: primsBody xs).length + 1)
    (by simp [isWs]) (by simp) valid (by simp only [List.length_cons]; omega)
  simpa only [List.cons_append, List.nil_append, List.append_nil, List.reverse_nil] using result

theorem parseHeader_print (tail : List Char) :
    parseHeader ("#usda 1.0\n".toList ++ tail) = some ((), '\n' :: tail) := by rfl

def Layer.WellFormed (l : Layer) : Prop :=
  (∀ xs ∈ l.subLayers, ∀ s ∈ xs, s.WellFormed) ∧ (∀ p ∈ l.prims, p.WellFormed)

theorem parseLayer_print (l : Layer) (valid : l.WellFormed) : parseLayer l.print = some l := by
  rcases l with ⟨subs, ps⟩
  rcases valid with ⟨sub_valid, prim_valid⟩
  have forest := prims_print_text ps prim_valid
  cases subs with
  | none =>
    have no_open := primsBody_not_open ps
    simp only [Layer.print, parseLayer, String.toList_append, primsBody_text,
      show "".toList = [] from rfl, List.append_nil, parseHeader_print, Bind.bind, Option.bind_some,
      beq_iff_eq, no_open, ↓reduceIte, forest]
    rfl
  | some xs =>
    have sub_wf : ∀ s ∈ xs, s.WellFormed := sub_valid xs (by simp)
    have open_peek : peek ('\n' :: ((subLayersText xs).toList ++ primsBody ps)) = some '(' := by
      simp [subLayersText, String.toList_append, peek, ws, isWs]
    have subs_parse := layerSubs_some xs (primsBody ps) sub_wf
    have frame : (Layer.print ⟨some xs, ps⟩).toList = "#usda 1.0\n".toList ++
        ((subLayersText xs).toList ++ primsBody ps) := by
      simp [Layer.print, subLayersText, String.toList_append, List.append_assoc,
        primsBody, List.flatMap_map]
    unfold parseLayer
    rw [frame]
    dsimp only
    rw [parseHeader_print]
    simp only [Bind.bind, Option.bind_some, open_peek, BEq.rfl, ↓reduceIte]
    simp only [Bind.bind] at subs_parse
    let finish := fun (sr : Option (List SubLayer) × List Char) =>
      (prims (sr.2.length + 1) [] sr.2).bind (fun pr =>
        if ws pr.2 = [] then some (Layer.mk sr.1 pr.1) else none)
    have result := congrArg (fun parsed => parsed.bind finish) subs_parse
    have finished : (some (some xs, '\n' :: primsBody ps)).bind finish =
        some (Layer.mk (some xs) ps) := by
      simp only [Option.bind_some, finish]
      rw [forest]
      rfl
    exact result.trans finished

/-- Key admissibility is universal, including arbitrary nested values. This
corollary prevents the Layer inverse from relying on identifier-only keys. -/
theorem parseLayer_print_dictionary (key : String) (value : DVal) :
    let layer : Layer := ⟨none, [.mk .def_ none "P" [.customData [(key, value)]] []]⟩
    parseLayer layer.print = some layer := by
  apply parseLayer_print
  simp [Layer.WellFormed, Prim.WellFormed, Prim.itemsWellFormed,
    PrimMeta.WellFormed, EntriesWellFormed_all]

end UsdSpec.Usda
