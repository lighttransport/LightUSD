#!/usr/bin/env python3
"""Compiler-driven requalification of legacy names to `lightusd::legacy::`.

Usage: requalify_legacy.py <build-log> [--apply]

Parses GCC/Clang "X is not a member of lightusd[::N]" / "no member named X in
namespace lightusd[::N]" errors from a legacy build log, adds each (scope, name)
to scripts/refactor/legacy-symbols.txt, then rewrites `lightusd::[N::]X` to
`lightusd::legacy::[N::]X` in legacy sources and legacy consumers. Repeat
build -> requalify until the legacy build is clean. Part of the next->core
refactor; delete with scripts/refactor/.
"""
import os
import re
import subprocess
import sys

SYMS = 'scripts/refactor/legacy-symbols.txt'
ERR = [
    re.compile(r"[‘']([A-Za-z_]\w*)[’'] is not a member of [‘']lightusd((?:::\w+)*)[’']"),
    re.compile(r"no member named '([A-Za-z_]\w*)' in namespace 'lightusd((?:::\w+)*)'"),
    re.compile(r"[‘']lightusd((?:::\w+)*)::([A-Za-z_]\w*)[’'] has not been declared"),
    re.compile(r"[‘']([A-Za-z_]\w*)[’'] in namespace [‘']lightusd((?:::\w+)*)[’'] does not name a (?:template )?type"),
]
# Files whose `lightusd::` references are rewritten: legacy itself plus every
# consumer that compiles against legacy headers.
CONSUMER_ROOTS = ('src/legacy/', 'tests/test-main.cc', 'tests/usda-roundtrip.cc', 'tests/usdc-roundtrip.cc', 'tests/unit/', 'tests/feat/', 'tests/fuzzer/', 'tests/mcp/',
                  'tests/pprint/', 'tests/volumetric/', 'tests/decompress-int/', 'examples/',
                  'tools/', 'web/binding.cc', 'web/binding-combined-api.', 'web/binding-feature',
                  'benchmark/', 'sandbox/')
EXTS = ('.cc', '.c', '.cpp', '.cxx', '.hh', '.h', '.hpp', '.inc', '.inl', '.mm')
# Core-native consumers under those roots: never rewritten.
CORE_CONSUMERS = ('tools/lusdchecker/', 'tools/lusdpreview/', 'examples/next_workflows/',
                  'examples/lmetalview/', 'examples/ld3d12view/', 'examples/urdf-to-usd/main.cc',
                  'tests/feat/render-convert/', 'examples/c_api_example/next-',
                  'tests/volumetric/')
# Names that belong to the core and must never be treated as legacy.
CORE_NAMES = {'next',
              # shared (src/, outside src/legacy) namespaces/types legacy does not own
              'minijson', 'io', 'image', 'imageio', 'imageproc', 'logging', 'ptx', 'safe',
              'security_policy', 'udim', 'usdVol', 'api', 'color', 'mcp_schema', 'Image'}


def load_syms():
    syms = set()  # filtered against CORE_NAMES below
    if os.path.exists(SYMS):
        for line in open(SYMS):
            line = line.strip()
            if line and not line.startswith('#') and line.split('::')[0] not in CORE_NAMES:
                syms.add(line)
    return syms


def parse_log(path):
    found = set()
    text = open(path, errors='ignore').read()
    for pat in ERR:
        for m in pat.finditer(text):
            if pat is ERR[2]:
                scope, name = m.group(1), m.group(2)
            else:
                name, scope = m.group(1), m.group(2)
            if scope.startswith('::legacy'):
                continue
            sym = (scope.lstrip(':') + '::' + name).lstrip(':')
            if sym.split('::')[0] not in CORE_NAMES:
                found.add(sym)
    return found


def consumer_files():
    out = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True).stdout.split()
    files = []
    for f in out:
        if not f.endswith(EXTS) or not f.startswith(CONSUMER_ROOTS):
            continue
        # Core-native code under these roots must keep core names.
        if ('/next' in f or '-next.' in f or f.startswith(CORE_CONSUMERS)
                or f.startswith(('src/legacy/legacy-namespace.hh',))):
            continue
        files.append(f)
    return files


def rewrite(syms, apply):
    # e.g. "Stage" or "tydra::RenderScene"; longest first.
    keys = sorted(syms, key=len, reverse=True)
    pat = re.compile(r'(?<![\w:])(::)?lightusd::(' + '|'.join(re.escape(k) for k in keys) + r')(?![\w])')
    total = 0
    for f in consumer_files():
        try:
            s = open(f, encoding='utf-8', errors='surrogateescape').read()
        except OSError:
            continue
        t, n = pat.subn(lambda m: f'{m.group(1) or ""}lightusd::legacy::{m.group(2)}', s)
        if n:
            total += n
            if apply:
                open(f, 'w', encoding='utf-8', errors='surrogateescape').write(t)
    return total


def main():
    log = sys.argv[1]
    apply = '--apply' in sys.argv
    syms = load_syms()
    new = parse_log(log) - syms
    syms |= new
    if apply:
        with open(SYMS, 'w') as fh:
            fh.write('# Legacy names requalified to lightusd::legacy:: (scope::name).\n')
            fh.write('\n'.join(sorted(syms)) + '\n')
    n = rewrite(syms, apply) if syms else 0
    print(f'{len(new)} new symbols ({len(syms)} total); {n} references rewritten')
    for s in sorted(new)[:40]:
        print('  +', s)


if __name__ == '__main__' and '--ambiguous' not in sys.argv:
    main()


def fix_ambiguous(log, apply):
    """`reference to 'N' is ambiguous` (N a bridged namespace) in consumer files:
    with both `using namespace lightusd;` and `using namespace lightusd::legacy;`
    a bare `N::` is ambiguous. `lightusd::legacy::N` also sees the shared
    `lightusd::N` (bridge), so qualify every bare `N::` in that file."""
    amb = re.compile(r"^(\S+?):\d+:\d+: error: reference to [‘']"
                     r"(tydra|value|mcp|mtlx|tsd)[’'] is ambiguous", re.M)
    root = os.path.realpath('.')
    alt = [r for r in os.environ.get('LIGHTUSD_REFACTOR_ROOTS', '').split(':') if r]
    hits = {}
    for m in amb.finditer(open(log, errors='ignore').read()):
        p = os.path.realpath(m.group(1))
        for r in [root] + alt:
            if m.group(1).startswith(r + '/'):
                p = os.path.join(root, m.group(1)[len(r) + 1:])
        rel = os.path.relpath(p, root)
        if rel.startswith('src/legacy/') or rel not in set(consumer_files()):
            continue
        hits.setdefault(rel, set()).add(m.group(2))
    total = 0
    for f, names in hits.items():
        s = open(f, encoding='utf-8', errors='surrogateescape').read()
        for n in names:
            s, k = re.subn(r'(?<![\w:])' + n + r'::', f'lightusd::legacy::{n}::', s)
            total += k
        if apply:
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    return total


if __name__ == '__main__' and len(sys.argv) > 2 and sys.argv[2] == '--ambiguous':
    print(fix_ambiguous(sys.argv[1], True), 'ambiguous namespace references qualified')
