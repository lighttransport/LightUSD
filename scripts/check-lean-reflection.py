#!/usr/bin/env python3
"""Verify the validator's Lean reflection table against verif/lean.

The validator rule metadata (src/next/validation/validation-context.cc,
``kLeanReflectionTable`` between the ``BEGIN/END LEAN_REFLECTION_TABLE``
markers) names, per rule id, the fully-qualified Lean 4 declaration that
formalizes the rule. This script parses that C++ table directly (no build
needed) and checks that every declaration exists in verif/lean/UsdSpec/**.

Resolution is tolerant on purpose: a name ``UsdSpec.A.B.decl`` is accepted when
some ``.lean`` file whose module path is a prefix of ``UsdSpec.A.B`` (or whose
filename matches ``A`` / ``B``) declares ``decl`` with ``def``, ``theorem``,
``lemma``, ``structure``, ``inductive``, ``abbrev``, ``class`` or ``instance``,
with a namespace chain (``namespace X`` ... ``end X``) that yields the last two
name components. ``structure`` fields are not declarations.

Optionally ``--lusdchecker PATH`` runs ``PATH --dump-rules`` and verifies that
its ``Lean:`` lines agree with the table (same rule -> same declaration).

Exit 1 listing every missing declaration (or dump-rules mismatch).
"""
import argparse
import os
import re
import subprocess
import sys

TABLE_RE = re.compile(r'\{"([^"]+)",\s*"([^"]+)"\}')
DECL_RE = re.compile(
    r'^\s*(?:@\[[^\]]*\]\s*)?(?:private\s+|protected\s+|noncomputable\s+|partial\s+|'
    r'unsafe\s+|nonrec\s+)*(def|theorem|lemma|structure|inductive|abbrev|class|'
    r'instance|opaque|axiom)\s+([A-Za-z_][\w.!?\']*)')
NS_RE = re.compile(r'^\s*namespace\s+([\w.]+)')
END_RE = re.compile(r'^\s*end(?:\s+([\w.]+))?\s*$')
SECTION_RE = re.compile(r'^\s*section(?:\s+([\w.]+))?\s*$')


def parse_table(path):
    text = open(path, encoding='utf-8').read()
    begin = text.find('BEGIN LEAN_REFLECTION_TABLE')
    end = text.find('END LEAN_REFLECTION_TABLE')
    if begin < 0 or end < 0 or end < begin:
        sys.exit(f'{path}: LEAN_REFLECTION_TABLE markers not found')
    rows = TABLE_RE.findall(text[begin:end])
    if not rows:
        sys.exit(f'{path}: LEAN_REFLECTION_TABLE is empty')
    return rows


def lean_declarations(lean_root):
    """Map fully-qualified declaration name -> file (every namespace prefix)."""
    decls = {}
    for dirpath, _dirs, files in os.walk(lean_root):
        for name in files:
            if not name.endswith('.lean'):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, os.path.dirname(lean_root))
            module = rel[:-len('.lean')].replace(os.sep, '.')
            ns_stack = []  # (kind, name) with kind in {'namespace', 'section'}
            for line in open(path, encoding='utf-8'):
                m = NS_RE.match(line)
                if m:
                    ns_stack.append(('namespace', m.group(1)))
                    continue
                m = SECTION_RE.match(line)
                if m:
                    ns_stack.append(('section', m.group(1)))
                    continue
                m = END_RE.match(line)
                if m:
                    if ns_stack:
                        ns_stack.pop()
                    continue
                m = DECL_RE.match(line)
                if not m:
                    continue
                prefix = '.'.join(n for k, n in ns_stack if k == 'namespace' and n)
                decl = m.group(2)
                full = f'{prefix}.{decl}' if prefix else decl
                decls.setdefault(full, path)
                # Module-qualified alias (`UsdSpec.ListOp.Legacy.applyLegacy`
                # where the file is UsdSpec/ListOp/Legacy.lean with
                # `namespace UsdSpec.ListOp`).
                decls.setdefault(f'{module}.{decl}', path)
    return decls


def resolve(ref, decls):
    if ref in decls:
        return decls[ref]
    parts = ref.split('.')
    if len(parts) >= 2:
        tail = '.'.join(parts[-2:])
        for full, path in decls.items():
            if full == tail or full.endswith('.' + tail):
                return path
    return None


def check_dump_rules(binary, rows):
    try:
        out = subprocess.run([binary, '--dump-rules'], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        return [f'{binary} --dump-rules failed: {e}']
    dumped = {}
    rule = None
    for line in out.splitlines():
        m = re.match(r'^\[[^:\]]+:([^\]]+)\]:$', line)
        if m:
            rule = m.group(1)
        elif line.startswith('\tLean: ') and rule:
            dumped[rule] = line[len('\tLean: '):]
    problems = []
    table = dict(rows)
    for rule, ref in table.items():
        if dumped.get(rule) != ref:
            problems.append(f'--dump-rules: {rule} reports {dumped.get(rule)!r}, '
                            f'table says {ref!r}')
    for rule, ref in dumped.items():
        if rule not in table:
            problems.append(f'--dump-rules: {rule} has Lean {ref!r} not in table')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--repo', default=os.path.join(os.path.dirname(__file__), '..'),
                    help='repository root (default: script dir/..)')
    ap.add_argument('--lusdchecker', default=None,
                    help='optional lusdchecker binary; its --dump-rules Lean lines '
                         'must agree with the table (skipped when absent)')
    ap.add_argument('--check', action='store_true',
                    help='exit 1 on any missing declaration (default behavior; '
                         'kept for symmetry with the other --check scripts)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    table = os.path.join(repo, 'src', 'next', 'validation', 'validation-context.cc')
    lean_root = os.path.join(repo, 'verif', 'lean', 'UsdSpec')
    rows = parse_table(table)
    decls = lean_declarations(lean_root)

    missing = []
    for rule, ref in rows:
        path = resolve(ref, decls)
        if path is None:
            missing.append((rule, ref))
        elif args.verbose:
            print(f'ok: {rule} -> {ref} ({os.path.relpath(path, repo)})')
    problems = []
    if args.lusdchecker and os.path.exists(args.lusdchecker):
        problems = check_dump_rules(args.lusdchecker, rows)
    elif args.lusdchecker and args.verbose:
        print(f'note: {args.lusdchecker} not found; --dump-rules cross-check skipped')

    for rule, ref in missing:
        print(f'MISSING: {rule} -> {ref} (no such declaration under verif/lean/UsdSpec)')
    for p in problems:
        print(p)
    print(f'{len(rows) - len(missing)}/{len(rows)} Lean references resolved')
    return 1 if (missing or problems) else 0


if __name__ == '__main__':
    sys.exit(main())
