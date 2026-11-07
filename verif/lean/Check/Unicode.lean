import UsdSpec.Unicode.Normalize

/-! Execute the actual Lean NFC/NFD implementation against the official pinned
NormalizationTest.txt. This is conformance evidence, not a general proof of
idempotence. Hash verification is performed by the Python data gate first. -/

open UsdSpec.Unicode.Norm

private def parseHex (token : String) : Except String UInt32 := do
  if token.isEmpty then throw "empty code point"
  let value ← token.toList.foldlM (fun n c => do
    let digit := c.toNat
    let d ← if 48 ≤ digit && digit ≤ 57 then pure (digit - 48)
      else if 65 ≤ digit && digit ≤ 70 then pure (digit - 55)
      else throw s!"invalid hexadecimal code point {token}"
    pure (n * 16 + d)) 0
  if value > 0x10ffff || (0xd800 ≤ value && value ≤ 0xdfff) then
    throw s!"invalid Unicode scalar {token}"
  return value.toUInt32

private def column (text : String) : Except String (List UInt32) :=
  ((text.trimAscii.toString.splitOn " ").filter (! ·.isEmpty)).mapM parseHex

def main (args : List String) : IO UInt32 := do
  let [filename] := args | throw (IO.userError "usage: usdspec-check-unicode NormalizationTest.txt")
  let text ← IO.FS.readFile filename
  let mut rows := 0
  let mut checks := 0
  let mut failures := 0
  let mut part1 := false
  let mut covered : Std.HashSet UInt32 := {}
  for line in text.splitOn "\n" do
    let content := ((line.splitOn "#").head!).trimAscii.toString
    if content.isEmpty then continue
    if content.startsWith "@" then
      part1 := content.startsWith "@Part1"
      continue
    let parsed := (content.splitOn ";").take 5 |>.mapM column
    let columns ← match parsed with
      | .ok values => pure values
      | .error error => throw (IO.userError s!"row {rows + 1}: {error}")
    let [c1, c2, c3, c4, c5] := columns
      | throw (IO.userError s!"row {rows + 1}: expected five columns")
    if part1 then
      let [cp] := c1 | throw (IO.userError "Part1 requires singleton first columns")
      covered := covered.insert cp
    rows := rows + 1
    for (input, expected) in ([c1, c2, c3, c4, c5].zip [c2, c2, c2, c4, c4]) do
      checks := checks + 1
      if nfcCps input != expected then
        failures := failures + 1
        if failures ≤ 10 then IO.eprintln s!"NFC mismatch at row {rows}: {input}"
    for (input, expected) in ([c1, c2, c3, c4, c5].zip [c3, c3, c3, c5, c5]) do
      checks := checks + 1
      if nfdCps input != expected then
        failures := failures + 1
        if failures ≤ 10 then IO.eprintln s!"NFD mismatch at row {rows}: {input}"
  if rows != 19074 then
    throw (IO.userError s!"Unicode 15.1 corpus row count changed: {rows}, expected 19074")
  -- The official test also requires identity on every scalar omitted from
  -- Part1. Check this complement, including unassigned code points.
  let mut identityScalars := 0
  for value in [0:0x110000] do
    if 0xd800 ≤ value && value ≤ 0xdfff then continue
    let cp := value.toUInt32
    if covered.contains cp then continue
    identityScalars := identityScalars + 1
    checks := checks + 2
    if nfcCps [cp] != [cp] || nfdCps [cp] != [cp] then
      failures := failures + 1
      if failures ≤ 10 then IO.eprintln s!"identity mismatch for scalar {value}"
  -- Bounded exhaustive idempotence sweep (still not a symbolic proof): the
  -- alphabet S holds every composition component, every combining-class>0
  -- scalar and the Hangul jamo/syllable boundary samples. For every pair and
  -- for triples over a sampled mark set, check
  -- nfc(nfc x) = nfc x, nfd(nfc x) = nfd x and nfd(nfd x) = nfd x.
  let mut alphabet : Std.HashSet UInt32 := {}
  for (key, _) in compositions do
    alphabet := (alphabet.insert (key.toNat / 2097152).toUInt32).insert (key.toNat % 2097152).toUInt32
  for (lo, hi, v) in cccRanges do
    if v != 0 then
      for c in [lo.toNat:hi.toNat + 1] do alphabet := alphabet.insert c.toUInt32
  for c in [0x1100, 0x1112, 0x1161, 0x1175, 0x11a8, 0x11c2, 0xac00, 0xac01, 0xd7a3, 0x41, 0x61] do
    alphabet := alphabet.insert c.toUInt32
  let letters := alphabet.toList.toArray
  let marks := (letters.filter fun c => ccc c != 0)
  let sampleMarks := (List.range 40).filterMap fun i => marks[i * (marks.size / 40 + 1)]?
  let mut sweep := 0
  let ok := fun (x : List UInt32) =>
    nfcCps (nfcCps x) == nfcCps x && nfdCps (nfcCps x) == nfdCps x &&
      nfdCps (nfdCps x) == nfdCps x
  for a in letters do
    for b in letters do
      sweep := sweep + 1
      if !ok [a, b] then
        failures := failures + 1
        if failures ≤ 10 then IO.eprintln s!"idempotence sweep failure {a} {b}"
    for m1 in sampleMarks do
      for m2 in sampleMarks do
        sweep := sweep + 1
        if !ok [a, m1, m2] then
          failures := failures + 1
          if failures ≤ 10 then IO.eprintln s!"idempotence sweep failure {a} {m1} {m2}"
  IO.println s!"Idempotence sweep: {letters.size} alphabet scalars, {sweep} strings checked"
  -- Table invariants behind the NFC idempotence reduction
  -- (nfc(nfc x) = nfc x follows from nfd(nfc x) = nfd x): every table
  -- composite decomposes to the full decompositions of its two parts, full
  -- decomposition is stable on the characters it produces, and Hangul
  -- composition inverts Hangul decomposition on every syllable.
  let mut tableChecks := 0
  for (key, composite) in compositions do
    let a := (key.toNat / 2097152).toUInt32
    let b := (key.toNat % 2097152).toUInt32
    tableChecks := tableChecks + 1
    if decompFull 8 composite != decompFull 8 a ++ decompFull 8 b then
      failures := failures + 1
      if failures ≤ 10 then IO.eprintln s!"composite decomposition mismatch {composite}"
  for (k, _, _) in canonDecomp do
    for c in decompFull 8 k do
      tableChecks := tableChecks + 1
      if decompFull 8 c != [c] then
        failures := failures + 1
        if failures ≤ 10 then IO.eprintln s!"decomposition not stable on {c} (from {k})"
  for i in [0:sCount] do
    let c := (sBase + i).toUInt32
    tableChecks := tableChecks + 1
    let back : Option UInt32 := match hangulDecomp c with
      | some [l, v] => hangulCompose l v
      | some [l, v, t] => (hangulCompose l v).bind fun lv => hangulCompose lv t
      | _ => none
    if back != some c then
      failures := failures + 1
      if failures ≤ 10 then IO.eprintln s!"Hangul round trip failed for {c}"
  IO.println s!"Table invariants: {tableChecks} checks"
  IO.println s!"Unicode {unicodeVersion}: {rows} rows, {checks} NFC/NFD checks, {failures} failures"
  IO.println s!"Part1 complement: {identityScalars} Unicode scalars checked"
  return if failures == 0 then 0 else 1
