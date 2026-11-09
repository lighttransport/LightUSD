#!/usr/bin/env python3
"""Move legacy-only sources to src/legacy/ and rewrite build-file references.

Usage: move_legacy.py [--dry-run]

Reads scripts/refactor/legacy-manifest.tsv (<old>\t<new>), `git mv`s every
file, then rewrites path tokens that end in a moved `src/<rel>` inside the
tracked build files (CMakeLists.txt, *.cmake, meson.build, xmake.lua,
MANIFEST.in). Part of the next->core refactor; delete with scripts/refactor/.
"""
import os
import re
import subprocess
import sys

MANIFEST = 'scripts/refactor/legacy-manifest.tsv'
BUILD_FILE_RE = re.compile(r'(^|/)(CMakeLists\.txt|meson\.build|xmake\.lua|MANIFEST\.in)$|\.cmake$')


def load_manifest():
    pairs = []
    for line in open(MANIFEST):
        if not line.strip() or line.startswith('#'):
            continue
        old, new = line.rstrip('\n').split('\t')
        pairs.append((old, new))
    return pairs


def main():
    dry = '--dry-run' in sys.argv
    pairs = load_manifest()
    moved = {old[len('src/'):]: new[len('src/'):] for old, new in pairs}

    for old, new in pairs:
        if not os.path.exists(old):
            print('missing', old, file=sys.stderr)
            continue
        if dry:
            continue
        os.makedirs(os.path.dirname(new), exist_ok=True)
        subprocess.run(['git', 'mv', old, new], check=True)

    # Longest first so `src/tydra/foo.cc` wins over a shorter overlapping key.
    keys = sorted(moved, key=len, reverse=True)
    token = re.compile(r'(?<![\w.-])((?:[\w${}().-]*/)*?)src/(' +
                       '|'.join(re.escape(k) for k in keys) + r')(?![\w.-])')
    files = [f for f in subprocess.run(['git', 'ls-files'], capture_output=True,
                                       text=True, check=True).stdout.split()
             if BUILD_FILE_RE.search(f) and not f.startswith('src/legacy/')]
    total = 0
    for f in files:
        try:
            text = open(f).read()
        except (OSError, UnicodeDecodeError):
            continue
        new_text, n = token.subn(lambda m: f'{m.group(1)}src/{moved[m.group(2)]}', text)
        if n:
            total += n
            print(f'{f}: {n}')
            if not dry:
                open(f, 'w').write(new_text)
    print(f'{len(pairs)} files moved, {total} build references rewritten', file=sys.stderr)


if __name__ == '__main__':
    main()
