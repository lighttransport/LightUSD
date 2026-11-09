#!/usr/bin/env python3
"""Declaration scan: legacy names still referenced as `lightusd::[N::]X`.

Usage: scan_legacy_symbols.py [--apply]

Complements requalify_legacy.py (which learns names from compiler errors) for
references the compiler reports with other messages. A referenced name X is
treated as legacy when a src/legacy header declares it and no shared/core
header (src/ outside src/legacy) declares it. With --apply the names are added
to scripts/refactor/legacy-symbols.txt and requalify_legacy.rewrite() runs.
"""
import re
import subprocess
import sys

sys.path.insert(0, 'scripts/refactor')
import requalify_legacy as rq  # noqa: E402

HDR = ('.hh', '.h', '.hpp', '.inc')


def decl_names(paths):
    names = set()
    decl = re.compile(
        r'\b(?:class|struct|union|enum(?:\s+class|\s+struct)?|namespace)\s+([A-Za-z_]\w*)'
        r'|\busing\s+([A-Za-z_]\w*)\s*='
        r'|\btypedef\b[^;]*?\b([A-Za-z_]\w*)\s*;'
        r'|^[ \t]*(?:template\s*<[^;{]*>\s*)?(?:(?:static|inline|constexpr|extern|virtual)\s+)*'
        r'[\w:<>,*& \t]+?[\s*&]([A-Za-z_]\w*)\s*\(', re.M)
    for p in paths:
        try:
            text = open(p, errors='ignore').read()
        except OSError:
            continue
        for m in decl.finditer(text):
            n = next(g for g in m.groups() if g)
            names.add(n)
    return names


def main():
    files = subprocess.run(['git', 'ls-files', 'src'], capture_output=True, text=True,
                           check=True).stdout.split()
    legacy = decl_names(f for f in files if f.startswith('src/legacy/') and f.endswith(HDR))
    shared = decl_names(f for f in files if not f.startswith(('src/legacy/', 'src/external/', 'src/next/', 'src/tydra/next/'))
                        and f.endswith(HDR))
    ref = re.compile(r'(?<![\w:])(?:::)?lightusd::((?:[A-Za-z_]\w*::)?)([A-Za-z_]\w*)\b')
    found = set()
    for f in rq.consumer_files():
        try:
            text = open(f, errors='ignore').read()
        except OSError:
            continue
        for m in ref.finditer(text):
            scope, name = m.group(1), m.group(2)
            if scope == 'legacy::' or name == 'legacy' or (scope[:-2] or name) in rq.CORE_NAMES:
                continue
            if name in legacy and name not in shared:
                found.add(scope + name)
            elif scope and scope[:-2] in legacy and scope[:-2] not in shared:
                found.add(scope[:-2])  # whole legacy-only namespace
    syms = rq.load_syms()
    new = found - syms
    print(f'{len(new)} new legacy names from declarations')
    for s in sorted(new)[:80]:
        print('  +', s)
    if '--apply' in sys.argv and new:
        syms |= new
        with open(rq.SYMS, 'w') as fh:
            fh.write('# Legacy names requalified to lightusd::legacy:: (scope::name).\n')
            fh.write('\n'.join(sorted(syms)) + '\n')
        print(rq.rewrite(syms, True), 'references rewritten')


if __name__ == '__main__':
    main()
