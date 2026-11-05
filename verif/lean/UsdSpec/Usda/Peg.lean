import UsdSpec.Unicode.XidTable

/-!
# A generic PEG engine (milestone M5)

SPEC: core_spec.md L9583-9587 (USDA grammar conventions: "PEG notation", the
same conventions as the path grammar L3617-3647).

The full USDA grammar is data (`UsdSpec.Usda.Rules`, generated from the spec by
`scripts/extract-usda-peg.py`); this module is the interpreter. Semantics are
the literal PEG ones (Ford 2004):

* `alts` is ordered choice: the first alternative that succeeds is committed
  to; later alternatives are tried only when it *fails* (never after success,
  so there is no backtracking into a successful choice);
* `star`/`plus`/`opt` are greedy and never give characters back;
* `and`/`not` are syntactic predicates that consume nothing;
* an undefined nonterminal fails (`UsdSpec.Usda.Peg.undefinedRefs` decides that
  none remain in the repaired grammar).

One deviation from Ford's formalism, needed for totality: `e*` stops when `e`
succeeds without consuming input (Ford's `e*` with a nullable `e` diverges; no
rule of the USDA grammar has a nullable starred body except through that
pathology, so this changes no verdict).

`eval` is fuel-bounded with *depth* fuel (each nested call costs one unit) and
three outcomes, `ok`/`fail`/`timeout`; `eval_mono` shows a non-`timeout`
verdict is stable under more fuel, so a run that terminates is the PEG verdict.
No packrat memoization: the engine is a direct transcription, so it is
exponential in the worst case, but linear-ish on the USDA rules in practice.

Positions are carried explicitly next to the remaining input (`p`, `rest` with
the invariant `rest = input.drop p`) so the `star` progress test is O(1).

Spec examples that are not in the language (no Lean declaration; recorded as
`Gen.UsdaVec` rows and confirmed with pxr 26.11):
SPEC-ISSUE: LEAN-811 — examples at L4692, L4753, L5288, L5332 (missing `)`), L6545
(curly quotes), L4844 (commas between variants) do not parse.
SPEC-ISSUE: LEAN-812 — L10342-10348 shows `append rel myRel` without assignment, but
`ListOpMetadata` requires `Assignment ListOpValue` (L10388).
-/

namespace UsdSpec.Usda.Peg

/-- A character-class item: `[a-z]`, `[U+0020-U+007E]`, `[XID_Start]`, `[XID_Continue]`. -/
inductive CItem
  | range (lo hi : Nat)
  | xidStart
  | xidContinue
  deriving Repr, DecidableEq

def CItem.test : CItem → Char → Bool
  | .range lo hi, c => lo ≤ c.toNat && c.toNat ≤ hi
  | .xidStart, c => UsdSpec.Unicode.isXidStart c
  | .xidContinue, c => UsdSpec.Unicode.isXidContinue c

/-- PEG expressions. `seqs`/`alts` are n-ary sequence / ordered choice. -/
inductive Expr
  | eps
  | any
  | lit (s : String)
  | cls (items : List CItem)
  | nt (name : String)
  | seqs (xs : List Expr)
  | alts (xs : List Expr)
  | star (e : Expr)
  | plus (e : Expr)
  | opt (e : Expr)
  | and (e : Expr)
  | not (e : Expr)
  deriving Repr

/-- A grammar is an association list; `List.lookup` takes the first binding, so
`repairs ++ g` overrides rules of `g`. -/
abbrev Grammar := List (String × Expr)

/-- Outcome: success at position `p` with remaining input `rest`, failure, or out of fuel. -/
inductive Res
  | ok (p : Nat) (rest : List Char)
  | fail
  | timeout
  deriving Repr, DecidableEq

/-- `l` is a prefix of `s`: the remainder. -/
def stripPrefix : List Char → List Char → Option (List Char)
  | [], s => some s
  | _ :: _, [] => none
  | c :: cs, d :: ds => if c = d then stripPrefix cs ds else none

/-- The interpreter. `G` resolves nonterminals (see `Grammar.find`). -/
def eval (G : String → Option Expr) : Nat → Expr → Nat → List Char → Res
  | 0, _, _, _ => .timeout
  | _ + 1, .eps, p, s => .ok p s
  | _ + 1, .any, p, s => match s with
    | [] => .fail
    | _ :: t => .ok (p + 1) t
  | _ + 1, .lit l, p, s => match stripPrefix l.toList s with
    | some r => .ok (p + l.length) r
    | none => .fail
  | _ + 1, .cls is, p, s => match s with
    | [] => .fail
    | c :: t => if is.any (·.test c) then .ok (p + 1) t else .fail
  | f + 1, .nt n, p, s => match G n with
    | some b => eval G f b p s
    | none => .fail
  | _ + 1, .seqs [], p, s => .ok p s
  | f + 1, .seqs (a :: as), p, s => match eval G f a p s with
    | .ok q r => eval G f (.seqs as) q r
    | x => x
  | _ + 1, .alts [], _, _ => .fail
  | f + 1, .alts (a :: as), p, s => match eval G f a p s with
    | .fail => eval G f (.alts as) p s
    | x => x
  | f + 1, .star a, p, s => match eval G f a p s with
    | .ok q r => if p < q then eval G f (.star a) q r else .ok p s
    | .fail => .ok p s
    | .timeout => .timeout
  | f + 1, .plus a, p, s => eval G f (.seqs [a, .star a]) p s
  | f + 1, .opt a, p, s => match eval G f a p s with
    | .fail => .ok p s
    | x => x
  | f + 1, .and a, p, s => match eval G f a p s with
    | .ok _ _ => .ok p s
    | x => x
  | f + 1, .not a, p, s => match eval G f a p s with
    | .ok _ _ => .fail
    | .fail => .ok p s
    | .timeout => .timeout

def Grammar.find (g : Grammar) (n : String) : Option Expr := g.lookup n

/-- Characters consumed by `rule` on `input`, or `none` (failure or out of fuel). -/
def runWith (G : String → Option Expr) (rule : String) (input : List Char) (fuel : Nat) :
    Option Nat :=
  match eval G fuel (.nt rule) 0 input with
  | .ok q _ => some q
  | _ => none

def run (g : Grammar) (rule : String) (input : List Char) (fuel : Nat) : Option Nat :=
  runWith g.find rule input fuel

/-- Whole-input acceptance. -/
def accepts (g : Grammar) (rule : String) (input : String) (fuel : Nat := 100000) : Bool :=
  run g rule input.toList fuel == some input.length

/-! ## Engine lemmas -/

/-- Determinism: `run` is a function (stated for the record). -/
theorem run_deterministic {g : Grammar} {r : String} {i : List Char} {f n m : Nat}
    (h₁ : run g r i f = some n) (h₂ : run g r i f = some m) : n = m := by
  rw [h₁] at h₂; exact Option.some.inj h₂

theorem stripPrefix_append (l s : List Char) : stripPrefix l (l ++ s) = some s := by
  induction l with
  | nil => rfl
  | cons c cs ih => simp [stripPrefix, ih]

/-- A literal consumes exactly its length. -/
theorem eval_lit (G : String → Option Expr) (f p : Nat) (l : String) (s : List Char) :
    eval G (f + 1) (.lit l) p (l.toList ++ s) = .ok (p + l.length) s := by
  simp [eval, stripPrefix_append]

/-- A rule whose body is a literal: `run` returns its length. -/
theorem run_lit (g : Grammar) (r l : String) (s : List Char) (f : Nat)
    (h : g.find r = some (.lit l)) : run g r (l.toList ++ s) (f + 2) = some l.length := by
  simp [run, runWith, eval, h, stripPrefix_append]

/-- Fuel monotonicity: once the verdict is not `timeout`, more fuel does not change it. -/
theorem eval_mono (G : String → Option Expr) :
    ∀ f e p s, eval G f e p s ≠ .timeout → ∀ k, eval G (f + k) e p s = eval G f e p s := by
  intro f
  induction f with
  | zero => intro e p s h; simp [eval] at h
  | succ f ih =>
    intro e p s h k
    rw [show f + 1 + k = (f + k) + 1 by omega]
    cases e with
    | eps => simp [eval]
    | any => simp [eval]
    | lit l => simp [eval]
    | cls is => simp [eval]
    | nt n =>
      simp only [eval] at h ⊢
      cases hn : G n with
      | none => rfl
      | some b => rw [hn] at h; exact ih b p s h k
    | seqs xs =>
      cases xs with
      | nil => simp [eval]
      | cons a as =>
        simp only [eval] at h ⊢
        cases ha : eval G f a p s with
        | ok q r =>
          rw [ha] at h
          rw [ih a p s (by simp [ha]) k, ha]
          exact ih _ q r h k
        | fail => rw [ih a p s (by simp [ha]) k, ha]
        | timeout => rw [ha] at h; exact absurd rfl h
    | alts xs =>
      cases xs with
      | nil => simp [eval]
      | cons a as =>
        simp only [eval] at h ⊢
        cases ha : eval G f a p s with
        | ok q r => rw [ih a p s (by simp [ha]) k, ha]
        | fail =>
          rw [ha] at h
          rw [ih a p s (by simp [ha]) k, ha]
          exact ih _ p s h k
        | timeout => rw [ha] at h; exact absurd rfl h
    | star a =>
      simp only [eval] at h ⊢
      cases ha : eval G f a p s with
      | ok q r =>
        rw [ha] at h
        rw [ih a p s (by simp [ha]) k, ha]
        by_cases hpq : p < q
        · simp only [hpq, ite_true] at h ⊢; exact ih _ q r h k
        · simp [hpq]
      | fail => rw [ih a p s (by simp [ha]) k, ha]
      | timeout => rw [ha] at h; exact absurd rfl h
    | plus a => simp only [eval] at h ⊢; exact ih _ p s h k
    | opt a =>
      simp only [eval] at h ⊢
      cases ha : eval G f a p s with
      | ok q r => rw [ih a p s (by simp [ha]) k, ha]
      | fail => rw [ih a p s (by simp [ha]) k, ha]
      | timeout => rw [ha] at h; exact absurd rfl h
    | and a =>
      simp only [eval] at h ⊢
      cases ha : eval G f a p s with
      | ok q r => rw [ih a p s (by simp [ha]) k, ha]
      | fail => rw [ih a p s (by simp [ha]) k, ha]
      | timeout => rw [ha] at h; exact absurd rfl h
    | not a =>
      simp only [eval] at h ⊢
      cases ha : eval G f a p s with
      | ok q r => rw [ih a p s (by simp [ha]) k, ha]
      | fail => rw [ih a p s (by simp [ha]) k, ha]
      | timeout => rw [ha] at h; exact absurd rfl h

/-- `run` is monotone in fuel. -/
theorem run_mono {g : Grammar} {r : String} {i : List Char} {f n : Nat}
    (h : run g r i f = some n) (k : Nat) : run g r i (f + k) = some n := by
  unfold run runWith at *
  cases he : eval g.find f (.nt r) 0 i with
  | ok q rest =>
    rw [he] at h
    rw [eval_mono _ f _ 0 i (by simp [he]) k, he]
    exact h
  | fail => rw [he] at h; simp at h
  | timeout => rw [he] at h; simp at h

/-- `e*` never fails (it can only run out of fuel). -/
theorem eval_star_ne_fail (G : String → Option Expr) (a : Expr) :
    ∀ f p s, eval G f (.star a) p s ≠ .fail := by
  intro f
  induction f with
  | zero => intro p s; simp [eval]
  | succ f ih =>
    intro p s
    simp only [eval]
    cases ha : eval G f a p s with
    | ok q r =>
      by_cases hpq : p < q
      · simp only [hpq, ite_true]
        cases f with
        | zero => simp [eval]
        | succ f' => exact ih q r
      · simp [hpq]
    | fail => simp
    | timeout => simp

/-- Maximal munch: when `e*` stops at position `q`, `e` applied at `q` (with the
same fuel the star had left) either fails or succeeds without making progress;
i.e. the result cannot be extended by one more iteration of `e`. -/
theorem eval_star_maximal (G : String → Option Expr) (a : Expr) :
    ∀ f p s q r, eval G (f + 1) (.star a) p s = .ok q r →
      (eval G f a q r = .fail ∨ ∃ q' r', eval G f a q r = .ok q' r' ∧ ¬ q < q') := by
  intro f
  induction f with
  | zero => intro p s q r h; simp [eval] at h
  | succ f ih =>
    intro p s q r h
    simp only [eval] at h
    cases ha : eval G (f + 1) a p s with
    | ok q₁ r₁ =>
      rw [ha] at h
      by_cases hpq : p < q₁
      · simp only [hpq, ite_true] at h
        -- the recursive star call at fuel f+1 = (f)+1
        rcases ih q₁ r₁ q r h with hf | ⟨q', r', he, hn⟩
        · left
          have := eval_mono G f a q r (by simp [hf]) 1
          rw [this, hf]
        · right
          refine ⟨q', r', ?_, hn⟩
          have := eval_mono G f a q r (by simp [he]) 1
          rw [this, he]
      · simp only [hpq, ite_false] at h
        cases h
        exact Or.inr ⟨q₁, r₁, ha, hpq⟩
    | fail => rw [ha] at h; cases h; exact Or.inl ha
    | timeout => rw [ha] at h; cases h

/-! ## Static analysis of a grammar (decidable, used for G1 / G3) -/

mutual
def Expr.refs : Expr → List String
  | .nt n => [n]
  | .seqs xs => refsL xs
  | .alts xs => refsL xs
  | .star e | .plus e | .opt e | .and e | .not e => e.refs
  | _ => []
def refsL : List Expr → List String
  | [] => []
  | e :: es => e.refs ++ refsL es
end

mutual
def Expr.lits : Expr → List String
  | .lit s => [s]
  | .seqs xs => litsL xs
  | .alts xs => litsL xs
  | .star e | .plus e | .opt e | .and e | .not e => e.lits
  | _ => []
def litsL : List Expr → List String
  | [] => []
  | e :: es => e.lits ++ litsL es
end

/-- Nonterminals referenced somewhere in `g` but bound nowhere in `g` (deduplicated, in order). -/
def undefinedRefs (g : Grammar) : List String :=
  let names := g.map Prod.fst
  (g.flatMap (fun r => r.2.refs)).filter (fun n => !names.contains n) |>.eraseDups

/-- Names bound more than once. -/
def duplicateNames (g : Grammar) : List String :=
  let names := g.map Prod.fst
  names.filter (fun n => names.count n > 1) |>.eraseDups

/-- Rules reachable from `start` (worklist with `g.length + 1` rounds of fuel). -/
def reachable (g : Grammar) (start : String) : List String := Id.run do
  let mut seen : List String := []
  let mut work : List String := [start]
  for _ in List.range (g.length * 8 + 8) do
    match work with
    | [] => break
    | n :: rest =>
      work := rest
      if !seen.contains n then
        seen := n :: seen
        match g.find n with
        | some e => work := e.refs ++ work
        | none => pure ()
  return seen

def unreachable (g : Grammar) (start : String) : List String :=
  let r := reachable g start
  (g.map Prod.fst).filter (fun n => !r.contains n)

/-- Identifier-shaped literals of the rules (candidate keywords). -/
def wordLits (g : Grammar) : List String :=
  (g.flatMap (fun r => r.2.lits)).filter
    (fun s => !s.toList.isEmpty && s.toList.all Char.isAlpha) |>.eraseDups

end UsdSpec.Usda.Peg
