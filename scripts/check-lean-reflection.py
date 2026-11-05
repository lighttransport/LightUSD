#!/usr/bin/env python3
"""Verify the validator's Lean reflection table against verif/lean.

The validator rule metadata (src/next/validation/validation-context.cc,
``kLeanReflectionTable`` between the ``BEGIN/END LEAN_REFLECTION_TABLE``
markers) names, per rule id, the fully-qualified Lean 4 declaration that
formalizes the rule. This script parses that C++ table directly (no build
needed) and checks that every declaration exists in verif/lean/UsdSpec/**.

Resolution uses exact namespace-qualified declaration names. File/module names
do not introduce declaration aliases. Structure fields are not declarations.

Optionally ``--lusdchecker PATH`` runs ``PATH --dump-rules`` and verifies that
its ``Lean:`` lines agree with the table (same rule -> same declaration).

Exit 1 listing every missing declaration (or dump-rules mismatch).
"""
import argparse
import os
from pathlib import Path
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
    text = Path(path).read_text(encoding='utf-8')
    begin = text.find('BEGIN LEAN_REFLECTION_TABLE')
    end = text.find('END LEAN_REFLECTION_TABLE')
    if begin < 0 or end < 0 or end < begin:
        sys.exit(f'{path}: LEAN_REFLECTION_TABLE markers not found')
    rows = TABLE_RE.findall(text[begin:end])
    if not rows:
        sys.exit(f'{path}: LEAN_REFLECTION_TABLE is empty')
    return rows


def lean_declarations(lean_root):
    """Map exact namespace-qualified declaration names to source files."""
    decls = {}
    for dirpath, _dirs, files in os.walk(lean_root):
        for name in files:
            if not name.endswith('.lean'):
                continue
            path = os.path.join(dirpath, name)
            ns_stack = []  # (kind, name) with kind in {'namespace', 'section'}
            for line in Path(path).read_text(encoding='utf-8').splitlines():
                m = NS_RE.match(line)
                if m:
                    ns_stack.append(('namespace', m.group(1)))
                    continue
                m = SECTION_RE.match(line)
                if m:
                    ns_stack.append(('section', m.group(1)))
                    continue
                if line.strip() == "mutual":
                    ns_stack.append(("mutual", None))
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
    return decls


def resolve(ref, decls):
    return decls.get(ref)


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
    ap.add_argument('--require-lusdchecker', action='store_true',
                    help='fail if the checker binary is missing')
    ap.add_argument('--lake', nargs='?', const='lake',
                    help='also resolve every name in the compiled Lean environment')
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
    elif args.require_lusdchecker:
        problems.append('required lusdchecker binary is missing: ' + str(args.lusdchecker))
    elif args.lusdchecker and args.verbose:
        print(f'note: {args.lusdchecker} not found; --dump-rules cross-check skipped')

    if args.lake:
        refs = sorted(set(ref for _, ref in rows))
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_'.]*", ref) for ref in refs):
            problems.append('invalid Lean declaration name in reflection table')
        else:
            source = 'import UsdSpec\n' + ''.join('#check ' + ref + '\n' for ref in refs)
            try:
                checked = subprocess.run([args.lake, 'env', 'lean', '--stdin'],
                                         cwd=os.path.join(repo, 'verif', 'lean'),
                                         input=source, capture_output=True, text=True)
                if checked.returncode:
                    problems.append('Lean declaration resolution failed:\n' +
                                    checked.stdout + checked.stderr)
            except OSError as error:
                problems.append('cannot run Lean declaration resolution: ' + str(error))

    for rule, ref in missing:
        print(f'MISSING: {rule} -> {ref} (no such declaration under verif/lean/UsdSpec)')
    for p in problems:
        print(p)
    print(f'{len(rows) - len(missing)}/{len(rows)} Lean references resolved')
    return 1 if (missing or problems) else 0


if __name__ == '__main__':
    sys.exit(main())
