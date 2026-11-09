#!/usr/bin/env python3
"""Collect every repo file reached by the core build (sources + headers).

Usage: core_deps.py <compile_commands.json> <repo-root> [<out.txt>]

Runs each compile command with -MM (dependency output only, no codegen) and
prints the sorted union of repo-relative paths. Used by the next->core
refactor to separate shared files from legacy-only ones; delete with
scripts/refactor/ when the refactor is done.
"""
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor


def _roots(root):
    # A checkout may be reached through several spellings (symlinks, bind
    # mounts); accept every prefix listed in LIGHTUSD_REFACTOR_ROOTS too.
    extra = [r for r in os.environ.get('LIGHTUSD_REFACTOR_ROOTS', '').split(':') if r]
    return [root] + [os.path.abspath(r) for r in extra]


def _rel(p, roots):
    for r in roots:
        if p.startswith(r + os.sep):
            return os.path.relpath(p, r)
    return None


def deps_for(entry, root):
    args = entry.get('arguments') or shlex.split(entry['command'])
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if a in ('-MD', '-MMD', '-c') or a.startswith('-Werror'):
            continue
        out.append(a)
    out += ['-MM', '-MG', '-w']
    r = subprocess.run(out, cwd=entry['directory'], capture_output=True, text=True)
    if r.returncode != 0:
        return entry['file'], None, r.stderr[-400:]
    text = r.stdout.replace('\\\n', ' ')
    files = set()
    roots = _roots(root)
    for tok in text.split()[1:]:
        p = os.path.normpath(os.path.join(entry['directory'], tok))
        rel = _rel(p, roots) or _rel(os.path.realpath(p), roots)
        if rel:
            files.add(rel)
    return entry['file'], files, None


def main():
    cc, root = sys.argv[1], os.path.realpath(sys.argv[2])
    entries = json.load(open(cc))
    union, failures = set(), []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        for f, files, err in ex.map(lambda e: deps_for(e, root), entries):
            if files is None:
                failures.append((f, err))
            else:
                union |= files
    for f, err in failures:
        print('FAIL', f, err, file=sys.stderr)
    text = '\n'.join(sorted(union)) + '\n'
    if len(sys.argv) > 3:
        open(sys.argv[3], 'w').write(text)
    else:
        sys.stdout.write(text)
    print(f'{len(entries)} TUs, {len(union)} files, {len(failures)} failures', file=sys.stderr)


if __name__ == '__main__':
    main()
