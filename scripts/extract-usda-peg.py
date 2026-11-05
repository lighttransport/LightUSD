#!/usr/bin/env python3
"""Extract the normative USDA PEG from aousd/core/1.1.0/core_spec.md.

Reads every `Name <- expr` rule in the USDA text-format chapter (spec
L9569-11375, fenced and 4-space-indented code blocks alike), plus the path
grammar it defers to (L3617-3772, "The productions for Path reference the
**Path** production"), and the optional legacy extension (L11376-11519,
flagged, not part of the normative grammar).

Normalization:
  * `↵` (visual line continuation) and markdown hard-break spaces are dropped.
  * Path-grammar rules live in their own namespace `Path.<Name>` (USDA and the
    path chapter both define `Assignment`, `Identifier`, `Space`, ... with
    different bodies); the USDA reference `Path` resolves to `Path.Path`.
  * Templated rules `List<T>`, `Sequence<T>`, `MetadataBlock<T>` are expanded
    per use into concrete rules named e.g. `List<TupleItem>`.
  * `REPAIRS` (below) are minimal overrides for the grammar defects found by the
    Lean acceptance runs; they are emitted as a separate `repairs` table so the
    literal grammar stays exactly as written.

Outputs: JSON (stdout or --json FILE) and `verif/lean/UsdSpec/Usda/Rules.lean`
(`--lean FILE`, default path; `--check` diffs instead of writing).
`--examples DIR` writes every fenced spec block that starts with `#usda` to
DIR/spec-L<line>.usda.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "aousd", "core", "1.1.0", "core_spec.md")
LEAN_OUT = os.path.join(ROOT, "verif", "lean", "UsdSpec", "Usda", "Rules.lean")

USDA_RANGE = (9569, 11375)
PATH_RANGE = (3617, 3772)
LEGACY_RANGE = (11376, 11519)
KEYWORD_TABLE = (9935, 9993)
ENTRY = "LayerSpec"

RULE_START = re.compile(r"^\s*([A-Za-z][A-Za-z0-9]*(?:<[A-Za-z]+>)?)\s*<-\s*(.*)$")

# Minimal repairs: rule -> (issue id, PEG text). Applied on top of the literal
# grammar to form `usdaRepaired`; each one is the smallest change that lets the
# spec's own examples / pxr output through for the stated reason.
REPAIRS = {
    # LEAN-802: referenced but defined only in the legacy section (L11489).
    "TimeSampleOrDefault": ("LEAN-802",
        "'timeSamples' Assignment TimeSampleMap / 'default' Assignment TypedValue"),
    # LEAN-805: `(Digit)+` is a prefix of the 2nd alternative; PEG commits to it.
    "BasePart": ("LEAN-805",
        "(Digit)+ Dot (Digit)* / (Digit)+ / Dot (Digit)+"),
    # LEAN-806: `!(Keyword)` rejects every identifier that merely starts with a
    # keyword (`defaultPrim`, `customLayerData`, `prefix`); require a word end.
    "KeywordlessIdentifier": ("LEAN-806",
        "!(Keyword !XidContinue) BaseIdentifier"),
    # LEAN-807: alternative 1 succeeds on the declaration prefix, so the
    # `.timeSamples` / `.connect` / `.spline` alternative is unreachable.
    "AttributeSpec": ("LEAN-807",
        "AttributeDeclaration (InlinePadding)* Dot (InlinePadding)* SpecialAttributeType / "
        "AttributeDeclaration (AttributeAssignment)? (InlinePadding)* (AttributeMetadata (InlinePadding)*)? / "
        "ListOpConnect"),
    # LEAN-807 (same shape): RelationshipSpec alternative 1 is a prefix of 3.
    "RelationshipSpec": ("LEAN-807",
        "RelationshipType (InlinePadding)+ NamespacedName (InlinePadding)* Dot (InlinePadding)* TimeSampleOrDefault / "
        "RelationshipType (InlinePadding)+ NamespacedName (RelationshipAssignment)? (InlinePadding)* (RelationshipMetadata)? / "
        "ListOpRelationship"),
    # LEAN-808: `customData` is a keyword, so `customData = {...}` is not a
    # KeyValueMetadata on prims/properties/layers; only ReferenceParameter has it.
    "SharedMetadata": ("LEAN-808",
        "String / 'customData' Assignment DictionaryValue / KeyValueMetadata / DocMetadata"),
    # LEAN-809: Utf8Character starts at U+0020, so multiline strings and
    # /* */ comments cannot contain a line break; and `!(SingleQuote)` forbids
    # a lone quote inside a triple-quoted string.
    "MultilineSingleQuoteContents": ("LEAN-809",
        "Escaped / Crlf / !(ThreeSingleQuotes) Utf8Character"),
    "MultilineDoubleQuoteContents": ("LEAN-809",
        "Escaped / Crlf / !(ThreeDoubleQuotes) Utf8Character"),
    "CppStyleMultilineComment": ("LEAN-809",
        "CppStyleMultilineOpen (!(Asterisk ForwardSlash) (Utf8Character / Crlf))* CppStyleMultilineClose"),
    # LEAN-810: U+0009 (TAB) is not a Utf8Character: no tab in strings/comments.
    "Utf8Character": ("LEAN-810", None),
}


def read_spec():
    with open(SPEC, encoding="utf-8") as f:
        return f.read().split("\n")


def collect_rules(lines, lo, hi):
    """Return [(name, body_text, line)] for rules starting in [lo, hi] (1-based)."""
    rules = []
    i = lo - 1
    while i < hi:
        m = RULE_START.match(lines[i])
        if not m:
            i += 1
            continue
        name, body, start = m.group(1), [m.group(2)], i + 1
        i += 1
        while i < len(lines):
            ln = lines[i]
            if not ln.strip() or ln.lstrip().startswith("```") or RULE_START.match(ln):
                break
            if not ln[:1].isspace():
                break
            body.append(ln)
            i += 1
        text = " ".join(b.replace("↵", " ").strip() for b in body)
        rules.append((name, re.sub(r"\s+", " ", text).strip(), start))
    return rules


# ---------------------------------------------------------------- PEG parser
TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<sq>'[^']*')
  | (?P<dq>"[^"]*")
  | (?P<cls>\[[^\]]*\])
  | (?P<id>[A-Za-z][A-Za-z0-9]*(?:<[A-Za-z]+>)?)
  | (?P<op>[/()*+?&!.])
""", re.X)


def tokenize(s):
    out, pos = [], 0
    while pos < len(s):
        m = TOKEN.match(s, pos)
        if not m:
            raise ValueError(f"cannot tokenize at {s[pos:pos+20]!r} in {s!r}")
        pos = m.end()
        k = m.lastgroup
        if k == "ws":
            continue
        out.append((k, m.group(k)))
    return out


def parse_class(text):
    inner = text[1:-1]
    if inner == "XID_Start":
        return [["xidStart"]]
    if inner == "XID_Continue":
        return [["xidContinue"]]
    items = []
    m = re.fullmatch(r"U\+([0-9A-Fa-f]+)(?:-U\+([0-9A-Fa-f]+))?", inner)
    if m:
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        return [["range", lo, hi]]
    j = 0
    while j < len(inner):
        if j + 2 < len(inner) and inner[j + 1] == "-":
            items.append(["range", ord(inner[j]), ord(inner[j + 2])])
            j += 3
        else:
            items.append(["range", ord(inner[j]), ord(inner[j])])
            j += 1
    return items


class P:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def eat(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def alt(self):
        xs = [self.seq()]
        while self.peek() == ("op", "/"):
            self.eat()
            xs.append(self.seq())
        return xs[0] if len(xs) == 1 else ["alt", xs]

    def seq(self):
        xs = []
        while True:
            k, v = self.peek()
            if k is None or (k == "op" and v in "/)"):
                break
            xs.append(self.prefix())
        if not xs:
            return ["eps"]
        return xs[0] if len(xs) == 1 else ["seq", xs]

    def prefix(self):
        k, v = self.peek()
        if k == "op" and v in "&!":
            self.eat()
            return ["and" if v == "&" else "not", self.prefix()]
        return self.suffix()

    def suffix(self):
        e = self.primary()
        while self.peek()[0] == "op" and self.peek()[1] in "*+?":
            v = self.eat()[1]
            e = [{"*": "star", "+": "plus", "?": "opt"}[v], e]
        return e

    def primary(self):
        k, v = self.eat()
        if k in ("sq", "dq"):
            return ["lit", v[1:-1]]
        if k == "cls":
            return ["cls", parse_class(v)]
        if k == "id":
            return ["nt", v]
        if (k, v) == ("op", "("):
            e = self.alt()
            if self.eat() != ("op", ")"):
                raise ValueError("expected )")
            return e
        if (k, v) == ("op", "."):
            return ["any"]
        raise ValueError(f"unexpected token {k} {v}")


def parse_expr(text):
    p = P(tokenize(text))
    e = p.alt()
    if p.i != len(p.t):
        raise ValueError(f"trailing tokens in {text!r}: {p.t[p.i:]}")
    return e


def map_nts(e, f):
    tag = e[0]
    if tag == "nt":
        return ["nt", f(e[1])]
    if tag in ("alt", "seq"):
        return [tag, [map_nts(x, f) for x in e[1]]]
    if tag in ("star", "plus", "opt", "and", "not"):
        return [tag, map_nts(e[1], f)]
    return e


def refs(e, acc):
    tag = e[0]
    if tag == "nt":
        acc.append(e[1])
    elif tag in ("alt", "seq"):
        for x in e[1]:
            refs(x, acc)
    elif tag in ("star", "plus", "opt", "and", "not"):
        refs(e[1], acc)
    return acc


def lits(e, acc):
    tag = e[0]
    if tag == "lit":
        acc.add(e[1])
    elif tag in ("alt", "seq"):
        for x in e[1]:
            lits(x, acc)
    elif tag in ("star", "plus", "opt", "and", "not"):
        lits(e[1], acc)
    return acc


def expand_templates(rules, templates):
    """rules: name -> expr. templates: Name -> (param, expr). Instantiates uses."""
    work = list(rules)
    while work:
        n = work.pop()
        for r in refs(rules[n], []):
            m = re.fullmatch(r"([A-Za-z]+)<([A-Za-z]+)>", r)
            if m and r not in rules:
                tname, arg = m.groups()
                param, body = templates[tname]
                def sub(x, param=param, arg=arg):
                    if x == param:
                        return arg
                    return re.sub(r"<%s>$" % param, "<%s>" % arg, x)
                rules[r] = map_nts(body, sub)
                work.append(r)


def build(lines):
    usda_raw = collect_rules(lines, *USDA_RANGE)
    path_raw = collect_rules(lines, *PATH_RANGE)
    legacy_raw = collect_rules(lines, *LEGACY_RANGE)

    report = {"duplicates": [], "collisions": []}
    rules, lines_of, templates = {}, {}, {}
    for name, body, ln in usda_raw:
        e = parse_expr(body)
        m = re.fullmatch(r"([A-Za-z]+)<([A-Za-z]+)>", name)
        if m:
            templates[m.group(1)] = (m.group(2), e)
            lines_of[name] = ln
            continue
        if name in rules:
            report["duplicates"].append({"name": name, "lines": [lines_of[name], ln],
                                         "same_body": rules[name] == e})
            continue
        rules[name] = e
        lines_of[name] = ln
    path_names = {n for n, _, _ in path_raw}
    for name, body, ln in path_raw:
        e = map_nts(parse_expr(body), lambda x: "Path." + x if x in path_names else x)
        key = "Path." + name
        rules[key] = e
        lines_of[key] = ln
        if name in rules:
            ue = rules[name]
            pe_plain = parse_expr(body)
            report["collisions"].append({"name": name, "usda_line": lines_of[name],
                                         "path_line": ln, "same_body": ue == pe_plain})
    # Path-grammar refs to names it does not define (none expected) stay bare.
    rules = {k: (map_nts(v, lambda x: "Path.Path" if x == "Path" else x)
                 if not k.startswith("Path.") else v) for k, v in rules.items()}
    expand_templates(rules, templates)

    # Undefined references with the lines of the rules that reference them.
    undefined = {}
    for k, v in rules.items():
        for r in refs(v, []):
            if r not in rules:
                undefined.setdefault(r, set()).add(lines_of.get(k, lines_of.get(k.split("<")[0] + "<T>")))
    # Reachability from the entry rule.
    seen, stack = set(), [ENTRY]
    while stack:
        n = stack.pop()
        if n in seen or n not in rules:
            continue
        seen.add(n)
        stack.extend(refs(rules[n], []))
    unreachable = sorted(k for k in rules if k not in seen)

    # Keyword table vs rule literals.
    kw = rules["Keyword"]
    keywords = {x[1] for x in kw[1]}
    used = set()
    for k, v in rules.items():
        if k != "Keyword" and not k.startswith("Path."):
            lits(v, used)
    wordlike = {u for u in used if re.fullmatch(r"[A-Za-z]+", u)}

    repairs = {}
    for name, (issue, text) in REPAIRS.items():
        if text is None:  # Utf8Character: literal body plus [U+0009]
            e = rules[name]
            repairs[name] = {"issue": issue, "expr": ["alt", [["cls", [["range", 9, 9]]]] + e[1]]}
            continue
        repairs[name] = {"issue": issue, "expr": parse_expr(text)}

    report.update({
        "undefined": {k: sorted(x for x in v if x) for k, v in sorted(undefined.items())},
        "unreachable": unreachable,
        "keywords_not_in_rules": sorted(keywords - wordlike),
        "rule_words_not_keywords": sorted(wordlike - keywords),
        "template_lines": {k: v for k, v in lines_of.items() if "<T>" in k},
    })
    legacy = [{"name": n, "line": ln, "body": b} for n, b, ln in legacy_raw]
    return {
        "entry": ENTRY,
        "counts": {"usda_rules_text": len(usda_raw), "path_rules_text": len(path_raw),
                   "templates": len(templates), "expanded_rules": len(rules),
                   "legacy_rules_text": len(legacy_raw)},
        "rules": [{"name": k, "line": lines_of.get(k), "expr": v} for k, v in rules.items()],
        "repairs": repairs,
        "legacy": legacy,
        "report": report,
    }


# ---------------------------------------------------------------- Lean output
def lean_str(s):
    out = []
    for c in s:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif ord(c) < 0x20 or ord(c) > 0x7e:
            out.append("\\u%04x" % ord(c))
        else:
            out.append(c)
    return '"' + "".join(out) + '"'


def lean_expr(e):
    tag = e[0]
    if tag == "lit":
        return f"(.lit {lean_str(e[1])})"
    if tag == "nt":
        return f"(.nt {lean_str(e[1])})"
    if tag == "any":
        return ".any"
    if tag == "eps":
        return ".eps"
    if tag == "cls":
        items = []
        for it in e[1]:
            if it[0] == "range":
                items.append(f".range 0x{it[1]:X} 0x{it[2]:X}")
            else:
                items.append("." + it[0])
        return "(.cls [" + ", ".join(items) + "])"
    if tag in ("alt", "seq"):
        return f"(.{tag}s [" + ", ".join(lean_expr(x) for x in e[1]) + "])"
    return f"(.{tag} {lean_expr(e[1])})"


def emit_lean(g):
    r = g["report"]
    out = []
    out.append("-- GENERATED by scripts/extract-usda-peg.py from aousd/core/1.1.0/core_spec.md. Do not edit.")
    out.append("-- Regenerate: python3 scripts/extract-usda-peg.py --lean; drift check: --check.")
    out.append("import UsdSpec.Usda.Peg")
    out.append("import UsdSpec.Path.Grammar")
    out.append("")
    out.append("/-!")
    out.append("# The normative USDA PEG as data")
    out.append("")
    out.append(f"SPEC: core_spec.md L{USDA_RANGE[0]}-{USDA_RANGE[1]} (USDA grammar; normative statement L11378) and")
    out.append(f"L{PATH_RANGE[0]}-{PATH_RANGE[1]} (path grammar, namespaced `Path.*`).")
    c = g["counts"]
    out.append("")
    out.append(f"{c['usda_rules_text']} USDA rule texts ({c['templates']} templates) + {c['path_rules_text']} path rule texts;")
    out.append(f"{c['expanded_rules']} rules after template expansion. Entry rule `{g['entry']}` (L11212).")
    out.append("`usda` is the literal text; `repairs` are the minimal overrides behind")
    out.append("SPEC-ISSUE: LEAN-802, LEAN-805..810 (see UsdSpec/Usda/Peg.lean docs and ISSUES.md).")
    out.append("-/")
    out.append("")
    out.append("namespace UsdSpec.Usda.Peg")
    out.append("")
    out.append("/-- The literal USDA + path grammar (template instances expanded). -/")
    out.append("def usda : Grammar := [")
    n = len(g["rules"])
    for i, rule in enumerate(g["rules"]):
        ln = f" -- L{rule['line']}" if rule["line"] else " -- template instance"
        sep = "," if i + 1 < n else ""
        out.append(f"  ({lean_str(rule['name'])}, {lean_expr(rule['expr'])}){sep}{ln}")
    out.append("]")
    out.append("")
    out.append("/-- Minimal repairs, keyed by rule (issue id in the trailing comment). -/")
    out.append("def repairs : Grammar := [")
    reps = list(g["repairs"].items())
    for i, (name, rep) in enumerate(reps):
        sep = "," if i + 1 < len(reps) else ""
        out.append(f"  ({lean_str(name)}, {lean_expr(rep['expr'])}){sep} -- {rep['issue']}")
    out.append("]")
    out.append("")
    out.append("/-- Keyword table L9935-9993 as written. -/")
    kws = [x[1] for x in next(rr for rr in g["rules"] if rr["name"] == "Keyword")["expr"][1]]
    out.append("def keywordTable : List String := [" + ", ".join(lean_str(k) for k in kws) + "]")
    out.append("")
    out.append("")
    out.append("/-- Every fenced spec block that starts with `#usda` (complete layers), keyed `spec-L<line>`")
    out.append("(line of the `#usda` header). Used by the acceptance runs and `Gen.UsdaVec`. -/")
    out.append("def specExamples : List (String × String) := [")
    ex = spec_examples(read_spec())
    for i, (name, text) in enumerate(ex):
        sep = "," if i + 1 < len(ex) else ""
        out.append(f"  ({lean_str(name)}, {lean_str(text)}){sep}")
    out.append("]")
    out.extend(static_checks(g))
    out.append("end UsdSpec.Usda.Peg")
    return "\n".join(out) + "\n"


PATH_SAMPLES = [
    "/", ".", "..", "../..", "../../A/B", "/A", "/A/B", "/A/B.c", "/A.b", "/A.b:c:d",
    "A", "A/B", "A.b", ".b", "../A.b", "/A{v=x}", "/A{v=x}B", "/A{v=x}B/C", "/A/B{v=x}C",
    "/A{v=x}/B", "/A{v=}", "/A{v=x}{w=y}", "/A{v=x.y-z|w}", "/A{ v = x }", "/A.b.c", "a.b.c",
    "/A/", "//A", "/A//B", "/1A", "/_A", "/A_1", "/M\u00fcnchen", "/A-B", "/A.b:", "/A.:b",
    "/A{v=x}.p", "/Root/Child/Grandchild{size=1}/GreatGrandchild",
    "/World/Foo/Baz{VariantSet1=VarFoobar}Foobar.foobarprop1", "", " ", "/A B", "/A..",
    "/A/..", "/A/../B", "../", "/..", "/.", "/A.b[/C]"]


def lean_list(xs):
    return "[" + ", ".join(lean_str(x) for x in xs) + "]"


def static_checks(g):
    r = g["report"]
    undefined = list(r["undefined"].keys())
    o = []
    o.append("")
    o.append("/-! ## G1 well-formedness, G3 keywords, G4 path agreement (all by `decide`) -/")
    o.append("")
    o.append("/-- The full grammar after overriding with `repairs`. -/")
    o.append("def usdaRepaired : Grammar := repairs ++ usda")
    o.append("")
    for name, refs_lines in r["undefined"].items():
        pass
    o.append("/-- G1: every referenced nonterminal is defined, except " + ", ".join(f"`{u}` (referenced at L{','.join(map(str, r['undefined'][u]))})" for u in undefined) + ".")
    o.append("SPEC: core_spec.md L10779-10787 (RelationshipSpec); `TimeSampleOrDefault` is defined only in the")
    o.append("non-normative legacy section (L11489).")
    o.append("SPEC-ISSUE: LEAN-802 — normative `RelationshipSpec` references a rule the normative grammar never defines. -/")
    o.append(f"theorem undefinedRefs_usda : undefinedRefs usda = {lean_list(undefined)} := by decide +kernel")
    o.append("")
    o.append("theorem undefinedRefs_repaired : undefinedRefs usdaRepaired = [] := by decide +kernel")
    o.append("")
    o.append("/-- G1: no rule name is bound twice (the path chapter's colliding names live under `Path.`). -/")
    o.append("theorem duplicateNames_usda : duplicateNames usda = [] := by decide +kernel")
    o.append("")
    o.append(f"/-- G1: rules not reachable from `{ENTRY}` (`StringDictionary` is used only by the legacy")
    o.append("`prefixSubstitutions`/`suffixSubstitutions`, L11494-11498; `ReservedCharacters` is informative). -/")
    o.append(f"theorem unreachable_usda : unreachable usda \"{ENTRY}\" = {lean_list(sorted(r['unreachable'], key=lambda n: [x['name'] for x in g['rules']].index(n)))} := by decide +kernel")
    o.append("")
    o.append("/-- Identifier-shaped literals used by the rules other than `Keyword` itself. -/")
    o.append("def ruleWords : List String := wordLits (usda.filter (fun r => r.1 != \"Keyword\" && !(\"Path.\".toList.isPrefixOf r.1.toList)))")
    o.append("")
    o.append("/-- G3: the keyword table L9935-9993 lists `varying`, which no rule uses (`AttributeVariability`,")
    o.append("L10431, has only `uniform`); and the rules use `autoEase` (L10271), `inf`/`nan` (L9715-9717)")
    o.append("and `usda` (L11204) as literals that are not reserved, so `autoEase` is an `Identifier` too.")
    o.append("SPEC-ISSUE: LEAN-804 — keyword table and rule literals disagree. -/")
    kw_unused = r["keywords_not_in_rules"]
    o.append(f"theorem keywords_unused : keywordTable.filter (fun k => !ruleWords.contains k) = {lean_list(kw_unused)} := by decide +kernel")
    o.append("")
    words = [w for w in r["rule_words_not_keywords"] if len(w) > 1 and w not in ("e", "E")]
    o.append(f"theorem ruleWords_not_reserved : (ruleWords.filter (fun w => w.toList.length > 1 && !keywordTable.contains w)) = {lean_list(['inf', 'nan', 'autoEase', 'usda'])} := by decide +kernel")
    o.append("")
    o.append("/-- Path vector samples (same list as `Gen.PathSchemaVec.pathSamples`). -/")
    o.append(f"def pathSamples : List String := {lean_list(PATH_SAMPLES)}")
    o.append("")
    o.append("/-- G4: the path rules embedded in the USDA grammar (`Path.Path`, L3737) agree with the")
    o.append("hand transcription `UsdSpec.PathGrammar.Peg.accepts` on every path vector sample. -/")
    o.append("theorem path_rules_agree :")
    o.append("    pathSamples.all (fun s => accepts usda \"Path.Path\" s 1000 == UsdSpec.PathGrammar.Peg.accepts s) = true := by")
    o.append("  decide +kernel")
    o.append("")
    o.extend(DEFECT_WITNESSES)
    return o


DEFECT_WITNESSES = r"""/-- LEAN-805 witness: `BasePart` (L9704-9706) tries `(Digit)+` first, which commits on `1`
and leaves `.5`, so no decimal with a fraction is a `Number`.
SPEC-ISSUE: LEAN-805 — `BasePart` alternatives are in prefix order; `1.5` is not a `Number`. -/
theorem number_decimal_rejected :
    run usda "Number" "1.5".toList 100 = some 1 ∧ accepts usda "Number" "1.5" = false
    ∧ accepts usdaRepaired "Number" "1.5" = true ∧ accepts usda "Number" ".5" = true
    ∧ accepts usda "Number" "-1e-7" = true := by decide +kernel

/-- LEAN-806 witness: `!(Keyword)` (L10021) fails on any identifier with a keyword *prefix*.
SPEC-ISSUE: LEAN-806 — `KeywordlessIdentifier` rejects `defaultPrim`, `customLayerData`, `prefix`. -/
theorem keyword_prefix_rejected :
    accepts usda "Identifier" "defaultPrim" = false ∧ accepts usda "Identifier" "customLayerData" = false
    ∧ accepts usda "Identifier" "upAxis" = true
    ∧ accepts usdaRepaired "Identifier" "defaultPrim" = true
    ∧ accepts usdaRepaired "Identifier" "def" = false := by decide +kernel

/-- LEAN-807 witness: `AttributeSpec` alternative 1 (L10466-10468) succeeds on the bare
declaration, so alternative 2 (`.timeSamples`/`.connect`/`.spline`, L10469-10470) is unreachable.
SPEC-ISSUE: LEAN-807 — `float a.timeSamples = {...}` is not a `PropertySpec`. -/
theorem attr_special_unreachable :
    run usda "AttributeSpec" "float a.timeSamples = { }".toList 1000 = some 7
    ∧ accepts usda "AttributeSpec" "float a.timeSamples = { }" = false
    ∧ accepts usdaRepaired "AttributeSpec" "float a.timeSamples = { }" = true := by decide +kernel

/-- LEAN-808 witness: `customData` is a keyword (L9941), so it is no `KeyValueMetadata`
(L10369) and only `ReferenceParameter` (L10839) has a rule for it.
SPEC-ISSUE: LEAN-808 — prim/property/layer `customData = {...}` has no production. -/
theorem customData_metadata_rejected :
    accepts usda "SharedMetadata" "customData = {}" = false
    ∧ accepts usdaRepaired "SharedMetadata" "customData = {}" = true
    ∧ accepts usda "ReferenceParameter" "customData = {}" = true := by decide +kernel

/-- LEAN-809 witness: `Utf8Character` (L9680-9697) starts at U+0020, so a multiline string
(L9750-9776) cannot span lines and cannot hold a lone quote; the same for `/* */` (L9895).
SPEC-ISSUE: LEAN-809 — multiline strings/comments cannot contain line breaks. -/
theorem multiline_newline_rejected :
    accepts usda "String" "\"\"\"a\nb\"\"\"" = false ∧ accepts usdaRepaired "String" "\"\"\"a\nb\"\"\"" = true
    ∧ accepts usda "String" "'''it's'''" = false ∧ accepts usdaRepaired "String" "'''it's'''" = true
    ∧ accepts usda "Comment" "/* a\nb */" = false ∧ accepts usdaRepaired "Comment" "/* a\nb */" = true := by
  decide +kernel

/-- LEAN-810 witness: TAB is not a `Utf8Character`.
SPEC-ISSUE: LEAN-810 — a tab inside a string or comment is not in the language. -/
theorem tab_in_string_rejected :
    accepts usda "String" "\"a\tb\"" = false ∧ accepts usdaRepaired "String" "\"a\tb\"" = true
    ∧ accepts usda "Comment" "# a\tb" = false := by decide +kernel

/-- LEAN-801 (M4) re-checked on the full grammar: `prepend apiSchemas = []` is not
`ListOpMetadata`, `prepend apiSchemas = ["A"]` is; `references = []` is. -/
theorem lean801_full_grammar :
    accepts usda "ListOpMetadata" "prepend apiSchemas = []" = false
    ∧ accepts usda "ListOpMetadata" "prepend apiSchemas = [\"A\"]" = true
    ∧ accepts usda "ReferencesMetadata" "references = []" = true := by decide +kernel
""".split("\n")


def spec_examples(lines):
    res, i = [], 0
    while i < len(lines):
        if lines[i].startswith("```") and i + 1 < len(lines) and lines[i + 1].startswith("#usda"):
            j = i + 1
            body = []
            while j < len(lines) and not lines[j].startswith("```"):
                body.append(lines[j])
                j += 1
            res.append((f"spec-L{i + 2}", "\n".join(body) + "\n"))
            i = j
        i += 1
    return res


def examples(lines, outdir):
    os.makedirs(outdir, exist_ok=True)
    i, n = 0, 0
    found = []
    while i < len(lines):
        if lines[i].startswith("```") and i + 1 < len(lines) and lines[i + 1].startswith("#usda"):
            j = i + 1
            body = []
            while j < len(lines) and not lines[j].startswith("```"):
                body.append(lines[j])
                j += 1
            name = f"spec-L{i + 2}.usda"
            with open(os.path.join(outdir, name), "w", encoding="utf-8") as f:
                f.write("\n".join(body) + "\n")
            found.append(name)
            n += 1
            i = j
        i += 1
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="write JSON here instead of stdout")
    ap.add_argument("--lean", nargs="?", const=LEAN_OUT, help="write Rules.lean")
    ap.add_argument("--check", action="store_true", help="diff Rules.lean, exit 1 on drift")
    ap.add_argument("--examples", help="write spec #usda example blocks into DIR")
    a = ap.parse_args()
    lines = read_spec()
    g = build(lines)
    if a.examples:
        g["examples"] = examples(lines, a.examples)
    text = emit_lean(g)
    if a.check:
        cur = open(LEAN_OUT, encoding="utf-8").read() if os.path.exists(LEAN_OUT) else ""
        if cur != text:
            print(f"DRIFT {LEAN_OUT}", file=sys.stderr)
            return 1
        print(f"ok {LEAN_OUT}")
        return 0
    if a.lean:
        with open(a.lean, "w", encoding="utf-8") as f:
            f.write(text)
    js = json.dumps(g, indent=1)
    if a.json:
        with open(a.json, "w") as f:
            f.write(js)
    elif not a.lean:
        print(js)
    return 0


if __name__ == "__main__":
    sys.exit(main())
