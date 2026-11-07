#!/usr/bin/env python3
"""Check the literal/OpenUSD-compatibility classification of the SPEC-ISSUE registry.

verif/lean/data/model-classification.json must classify every registry row.
`dual` rows name a literal declaration and a distinct compatibility
declaration; both must exist in verif/lean/UsdSpec under the stated namespace.
Every `openusd-differs` row must be `dual` or `compat-observation`.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEAN = ROOT / 'verif/lean'
KINDS = {'dual', 'compat-observation', 'spec-defect', 'note'}


def declared(name, texts):
    namespace, _, leaf = name.rpartition('.')
    pat = re.compile(r'^\s*(?:private\s+|protected\s+|noncomputable\s+)*(?:def|theorem|abbrev|structure|inductive)\s+'
                     + re.escape(leaf) + r'(?![\w.\'])', re.M)
    for text in texts:
        ns = re.findall(r'^namespace\s+(\S+)', text, re.M)
        if any(namespace == n or namespace.startswith(n + '.') or n.startswith(namespace) for n in ns) \
                and pat.search(text):
            return True
    return False


def main():
    data = json.loads((LEAN / 'data/model-classification.json').read_text())
    status = {}
    for raw in (LEAN / 'ISSUES.md').read_text().split('\n'):
        if raw.startswith('| LEAN-'):
            cells = [c.strip() for c in raw.split('|')]
            status[cells[1]] = cells[5]
    texts = [p.read_text() for p in (LEAN / 'UsdSpec').rglob('*.lean')]
    problems = []
    for ident in sorted(set(status) - set(data)):
        problems.append(f'{ident}: unclassified')
    for ident in sorted(set(data) - set(status)):
        problems.append(f'{ident}: classified but not in registry')
    for ident, entry in sorted(data.items()):
        kind = entry.get('kind')
        if kind not in KINDS:
            problems.append(f'{ident}: bad kind {kind!r}')
            continue
        if kind == 'dual':
            lit, com = entry.get('literal'), entry.get('compat')
            if not lit or not com or lit == com:
                problems.append(f'{ident}: dual needs distinct literal and compat declarations')
            for label, name in (('literal', lit), ('compat', com)):
                if name and not declared(name, texts):
                    problems.append(f'{ident}: {label} declaration not found: {name}')
        if kind == 'compat-observation':
            if entry.get('implements') not in ('literal', 'compat'):
                problems.append(f'{ident}: compat-observation must say which model Lean implements')
            if not entry.get('decl') or not declared(entry['decl'], texts):
                problems.append(f'{ident}: declaration not found: {entry.get("decl")}')
        if status.get(ident, '').startswith('openusd-differs') and kind not in ('dual', 'compat-observation'):
            problems.append(f'{ident}: openusd-differs row must be dual or compat-observation')
    counts = {k: sum(1 for e in data.values() if e.get('kind') == k) for k in sorted(KINDS)}
    print(f'model split: {len(data)} rows classified {counts}, {len(problems)} problems')
    for line in problems:
        print('  ' + line)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
