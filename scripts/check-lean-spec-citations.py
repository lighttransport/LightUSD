#!/usr/bin/env python3
"""Mechanical citation audit for the Lean SPEC-ISSUE registry.

For every registry row: all `L<n>` / `L<n>-<m>` references must lie inside the
vendored Core Spec, and every quoted fragment (>= 12 characters) in the claim
column must occur verbatim (after whitespace/quote/markup normalization) inside
the union of the cited line ranges, widened by SLACK lines. For every
`CLAIM: "..."` quote in verif/lean/UsdSpec/** followed by `(L<n>[-<m>])` the
same containment check runs. This checks that citations point at the quoted
text; it does not judge whether the claim built on the text is correct.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / 'aousd/core/1.1.0/core_spec.md'
ISSUES = ROOT / 'verif/lean/ISSUES.md'
SOURCES = ROOT / 'verif/lean/UsdSpec'
SLACK = 6
REF = re.compile(r'L(\d+)(?:-(\d+))?')
QUOTE = re.compile(r'[“"]([^”"]{12,}?)[”"]')


def norm(text):
    text = text.replace('“', '"').replace('”', '"').replace('’', "'").replace('‘', "'")
    text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text)  # markdown links
    text = re.sub(r'[`*_\\|]', ' ', text)
    text = re.sub(r'\s+', ' ', text)
    return text.strip().lower()


def ranges(cell, limit):
    out = []
    for m in REF.finditer(cell):
        lo = int(m.group(1))
        hi = int(m.group(2) or lo)
        out.append((lo, hi))
    return out


def window(lines, spans):
    chunks = []
    for lo, hi in spans:
        chunks.append(' '.join(lines[max(lo - 1 - SLACK, 0):min(hi + SLACK, len(lines))]))
    return norm(' '.join(chunks))


def fragments(quote):
    return [f for f in re.split(r'…|\.\.\.', quote) if len(f.strip()) >= 8]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--max-unverified', type=int, default=None,
                    help='fail when more quote fragments than this are not found')
    args = ap.parse_args()
    lines = SPEC.read_text().split('\n')
    problems, rows, quotes, unverified = [], 0, 0, []
    for raw in ISSUES.read_text().split('\n'):
        if not raw.startswith('| LEAN-'):
            continue
        cells = [c.strip() for c in raw.split('|')]
        ident, spec_cell, claim = cells[1], cells[3], cells[4]
        rows += 1
        spans = ranges(spec_cell, len(lines))
        if not spans:
            problems.append(f'{ident}: no line citation')
            continue
        for lo, hi in spans:
            if lo < 1 or hi > len(lines) or hi < lo:
                problems.append(f'{ident}: citation L{lo}-{hi} outside spec ({len(lines)} lines)')
        hay = window(lines, spans + ranges(claim, len(lines)))
        for q in QUOTE.findall(claim):
            for frag in fragments(q):
                quotes += 1
                if norm(frag) not in hay:
                    unverified.append(f'{ident}: quote not within cited lines: {frag.strip()[:70]!r}')
    claim_re = re.compile(r'CLAIM:\s*[“"]([^”"]+)[”"]\s*\((L\d+(?:-\d+)?)\)')
    for path in sorted(SOURCES.rglob('*.lean')):
        text = re.sub(r'\s*\n\s*', ' ', path.read_text())
        for q, ref in claim_re.findall(text):
            spans = ranges(ref, len(lines))
            hay = window(lines, spans)
            for frag in fragments(q):
                quotes += 1
                if norm(frag) not in hay:
                    unverified.append(f'{path.relative_to(ROOT)}: CLAIM not within {ref}: {frag.strip()[:70]!r}')
    print(f'spec citations: {rows} registry rows, {quotes} quote fragments checked, '
          f'{len(problems)} bad citations, {len(unverified)} unverified fragments')
    for line in problems + unverified:
        print('  ' + line)
    limit = args.max_unverified
    if problems or (limit is not None and len(unverified) > limit):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
