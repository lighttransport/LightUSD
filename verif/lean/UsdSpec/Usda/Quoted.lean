import Mathlib.Tactic
/-!
# Quoted strings emitted by the supported USDA AST printer

SPEC: core_spec.md L9725-9746, L9785-9809. The canonical spelling escapes double
quotes and emits controls as three-digit octal sequences. Backslash uses \134,
which uses the listed EscapeOct production rather than relying on an omitted
backslash entry in EscapeSingleCharacter. The decoder additionally accepts the
named escapes and OpenUSD's backslash escape. Hexadecimal and one/two-digit
octal source forms are not modeled here; this is not a complete USDA lexer.

The inverse theorem covers arbitrary character lists, including embedded NUL,
Unicode, and arbitrary trailing input, with an explicit sufficient fuel bound.
The scanner is wider than the PEG `Utf8Character` (U+007F-U+009F, FDD0-FDEF and
FFFE/FFFF pass through raw), so printed text is not claimed PEG-valid. The
inverse is against this module's own `scanQuoted`, independent of `parseLayer_print`.
-/
namespace UsdSpec.Usda

def octalDigit (n : Nat) : Char := Char.ofNat (48 + n)
def octalValue (c : Char) : Option Nat :=
  if 48 ≤ c.toNat ∧ c.toNat < 56 then some (c.toNat - 48) else none

def escapeChar (c : Char) : List Char :=
  if c == '"' then ['\\', '"']
  else if c == '\\' then ['\\', '1', '3', '4']
  else if c.toNat < 32 then ['\\', '0', octalDigit (c.toNat / 8), octalDigit (c.toNat % 8)]
  else [c]

def escapeChars (cs : List Char) : List Char := cs.flatMap escapeChar

def readEscape : List Char → Option (Char × List Char)
  | '"' :: rest => some ('"', rest)
  | '\'' :: rest => some ('\'', rest)
  | '\\' :: rest => some ('\\', rest)
  | 'a' :: rest => some (Char.ofNat 7, rest)
  | 'b' :: rest => some (Char.ofNat 8, rest)
  | 'f' :: rest => some (Char.ofNat 12, rest)
  | 'n' :: rest => some ('\n', rest)
  | 'r' :: rest => some ('\r', rest)
  | 't' :: rest => some ('\t', rest)
  | 'v' :: rest => some (Char.ofNat 11, rest)
  | a :: b :: c :: rest => do
    let x ← octalValue a
    let y ← octalValue b
    let z ← octalValue c
    some (Char.ofNat (64 * x + 8 * y + z), rest)
  | _ => none

def scanQuoted : Nat → List Char → Option (List Char × List Char)
  | 0, _ => none
  | _ + 1, [] => none
  | fuel + 1, c :: rest =>
    if c == '"' then some ([], rest)
    else if c == '\\' then do
      let (c, rest) ← readEscape rest
      let (cs, rest) ← scanQuoted fuel rest
      some (c :: cs, rest)
    else if c.toNat < 32 then none
    else do
      let (cs, rest) ← scanQuoted fuel rest
      some (c :: cs, rest)

theorem octalValue_digit (n : Nat) (hn : n < 8) : octalValue (octalDigit n) = some n := by
  interval_cases n <;> decide

theorem scanQuoted_step (c : Char) (tail : List Char) (fuel : Nat) :
    scanQuoted (fuel + 1) (escapeChar c ++ tail) = (do
      let (cs, rest) ← scanQuoted fuel tail
      some (c :: cs, rest)) := by
  by_cases hq : c = '"'
  · subst c; simp [escapeChar, scanQuoted, readEscape]
  by_cases hb : c = '\\'
  · subst c; simp [escapeChar, scanQuoted, readEscape, octalValue]
  by_cases hc : c.toNat < 32
  · have div_bound : c.toNat / 8 < 8 := by omega
    have mod_bound : c.toNat % 8 < 8 := Nat.mod_lt _ (by decide)
    have recover : Char.ofNat (8 * (c.toNat / 8) + c.toNat % 8) = c := by
      rw [show 8 * (c.toNat / 8) + c.toNat % 8 = c.toNat by omega, Char.ofNat_toNat]
    have zero : octalValue '0' = some 0 := by decide
    simp [escapeChar, hq, hb, hc, scanQuoted, readEscape, octalValue_digit _ div_bound,
      octalValue_digit _ mod_bound, zero, recover]
  · simp [escapeChar, hq, hb, hc, scanQuoted]

theorem scanQuoted_escape (cs rest : List Char) (fuel : Nat) (hf : cs.length + 1 ≤ fuel) :
    scanQuoted fuel (escapeChars cs ++ '"' :: rest) = some (cs, rest) := by
  induction cs generalizing fuel with
  | nil => cases fuel <;> simp_all [escapeChars, scanQuoted]
  | cons c cs ih =>
    cases fuel with
    | zero => simp at hf
    | succ fuel =>
      simp only [escapeChars, List.flatMap_cons, List.append_assoc]
      rw [scanQuoted_step]
      have next := ih fuel (by simpa using hf)
      simpa [escapeChars] using congrArg
        (fun result => result.bind (fun (cs, rest) => some (c :: cs, rest))) next

theorem escapeChar_length_pos (c : Char) : 1 ≤ (escapeChar c).length := by
  unfold escapeChar
  split <;> (try split) <;> (try split) <;> simp

theorem escapeChars_length_ge (cs : List Char) : cs.length ≤ (escapeChars cs).length := by
  induction cs with
  | nil => simp [escapeChars]
  | cons c cs ih =>
    have positive := escapeChar_length_pos c
    simp only [escapeChars, List.flatMap_cons, List.length_append, List.length_cons] at *
    omega

def quoteString (s : String) : String :=
  String.ofList ('"' :: escapeChars s.toList ++ ['"'])

def parseQuotedChars : List Char → Option (String × List Char)
  | '"' :: rest => do
    let (cs, rest) ← scanQuoted (rest.length + 1) rest
    some (String.ofList cs, rest)
  | _ => none

theorem parseQuotedChars_quoteString (s : String) (rest : List Char) :
    parseQuotedChars ((quoteString s).toList ++ rest) = some (s, rest) := by
  have bound : s.toList.length + 1 ≤ (escapeChars s.toList ++ '"' :: rest).length + 1 := by
    have := escapeChars_length_ge s.toList
    simp only [List.length_append, List.length_cons] at *
    omega
  simp only [quoteString, String.toList_ofList, List.cons_append,
    List.nil_append, List.append_assoc, parseQuotedChars]
  rw [scanQuoted_escape _ _ _ bound]
  simp
end UsdSpec.Usda
