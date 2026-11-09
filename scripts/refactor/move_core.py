#!/usr/bin/env python3
"""Move the core (src/next, src/tydra/next) up into src/ and src/tydra/.

Usage: move_core.py [--dry-run]

1. `git mv` src/next/** -> src/**, src/tydra/next/** -> src/tydra/**.
2. Rewrite quoted #includes (whole repo) that resolve to a moved file:
   same directory -> bare name; else src-relative ("layer/prim-spec.hh") unless
   that path also exists under src/legacy/ or would be shadowed by the
   includer's own directory, in which case a path relative to the includer.
3. Rewrite path tokens in the moved CMake files relative to their new
   directory, and `src/next` -> `src`, `src/tydra/next` -> `src/tydra` in other
   build files and scripts.
Part of the next->core refactor; delete with scripts/refactor/.
"""
import os
import re
import subprocess
import sys

SRC_EXT = ('.cc', '.c', '.cpp', '.cxx', '.hh', '.h', '.hpp', '.inc', '.inl', '.mm', '.m')
INC = re.compile(r'^(\s*#\s*(?:include|import)\s*")([^"]+)(")', re.M)
# Include roots tried after the includer's directory (approximates the union of
# core and consumer -I flags; order matters only for ambiguous names).
OLD_ROOTS = ['src/next', 'src', 'src/tydra/next', 'src/c-api', 'src/tydra', 'web']


def git_files():
    return subprocess.run(['git', 'ls-files'], capture_output=True, text=True,
                          check=True).stdout.split()


def new_path(p):
    if p.startswith('src/tydra/next/'):
        return 'src/tydra/' + p[len('src/tydra/next/'):]
    if p == 'src/tydra/next':
        return 'src/tydra'
    if p.startswith('src/next/'):
        return 'src/' + p[len('src/next/'):]
    if p == 'src/next':
        return 'src'
    return p


def resolve(includer, inc, exists):
    cand = os.path.normpath(os.path.join(os.path.dirname(includer), inc))
    if cand in exists:
        return cand
    for r in OLD_ROOTS:
        cand = os.path.normpath(os.path.join(r, inc))
        if cand in exists:
            return cand
    return None


def plan_includes(files, moved_old):
    exists_old = set(files)
    after = {new_path(f) for f in files}
    plan = {}  # old file -> list of (match span text, new include)
    for f in files:
        if not f.endswith(SRC_EXT) or f.startswith('src/external/'):
            continue
        try:
            text = open(f, encoding='utf-8', errors='surrogateescape').read()
        except OSError:
            continue
        edits = []
        for m in INC.finditer(text):
            inc = m.group(2)
            target = resolve(f, inc, exists_old)
            if target is None or (target not in moved_old and f not in moved_old):
                continue
            new_f, new_t = new_path(f), new_path(target)
            ndir = os.path.dirname(new_f)
            if os.path.dirname(new_t) == ndir:
                cand = os.path.basename(new_t)
            else:
                cand = None
                if new_t.startswith('src/'):
                    srel = new_t[len('src/'):]
                    shadow = os.path.normpath(os.path.join(ndir, srel))
                    if ('src/legacy/' + srel) not in after and shadow not in after:
                        cand = srel
                if cand is None:
                    cand = os.path.relpath(new_t, ndir)
            if cand != inc:
                edits.append((m.start(2), m.end(2), cand))
        if edits:
            plan[f] = edits
    return plan


CMAKE_TOKEN = re.compile(r'(\$\{CMAKE_CURRENT_(?:SOURCE|LIST)_DIR\})((?:/[^\s")>;]*)?)'
                         r'|(?<![\w${}/.-])(\.\./[^\s")>;]*)')


def rewrite_moved_cmake(old, new, text):
    od, nd = os.path.dirname(old), os.path.dirname(new)

    def repl(m):
        if m.group(1):
            rel = m.group(2)[1:] if m.group(2) else ''
            prefix = m.group(1)
        else:
            rel, prefix = m.group(3), None
        abs_old = os.path.normpath(os.path.join(od, rel)) if rel else od
        abs_new = new_path(abs_old)
        r = os.path.relpath(abs_new, nd)
        trail = '/' if (m.group(2) or m.group(3) or '').endswith('/') else ''
        if prefix:
            out = prefix if r == '.' else f'{prefix}/{r}'
            return out + trail if r != '.' or trail else out
        return r + trail  # bare relative token
    return CMAKE_TOKEN.sub(repl, text)


GENERIC = [(re.compile(r'src/tydra/next(?![\w-])'), 'src/tydra'),
           (re.compile(r'src/next(?![\w-])'), 'src')]
BUILD_LIKE = re.compile(r'(^|/)(CMakeLists\.txt|meson\.build|xmake\.lua|MANIFEST\.in|setup\.py|'
                        r'pyproject\.toml)$|\.(cmake|sh|py|yml|yaml|mjs|in|toml|bat|ps1)$')


def main():
    dry = '--dry-run' in sys.argv
    files = git_files()
    moved_old = {f for f in files if f.startswith(('src/next/', 'src/tydra/next/'))}
    plan = plan_includes(files, moved_old)
    nedits = sum(len(v) for v in plan.values())
    print(f'{len(moved_old)} files to move; {nedits} includes in {len(plan)} files to rewrite')
    if dry:
        for f, ed in list(plan.items())[:8]:
            print(' ', f, [e[2] for e in ed][:4])
        return

    # Apply include edits on the old paths, then move.
    for f, edits in plan.items():
        text = open(f, encoding='utf-8', errors='surrogateescape').read()
        for s, e, cand in sorted(edits, reverse=True):
            text = text[:s] + cand + text[e:]
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(text)

    cmake_moved = {f: new_path(f) for f in moved_old
                   if f.endswith(('CMakeLists.txt', '.cmake'))}
    for f, nf in cmake_moved.items():
        text = open(f).read()
        open(f, 'w').write(rewrite_moved_cmake(f, nf, text))

    for f in sorted(moved_old):
        nf = new_path(f)
        os.makedirs(os.path.dirname(nf), exist_ok=True)
        subprocess.run(['git', 'mv', f, nf], check=True)

    # Generic path rewrites in other build files / scripts.
    n = 0
    for f in git_files():
        if f in cmake_moved.values() or f.startswith(('src/legacy/', 'scripts/refactor/', 'doc/')):
            continue
        if not BUILD_LIKE.search(f):
            continue
        try:
            text = open(f, encoding='utf-8', errors='surrogateescape').read()
        except (OSError, IsADirectoryError):
            continue
        t = text
        for pat, rep in GENERIC:
            t = pat.sub(rep, t)
        if t != text:
            n += 1
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(t)
    print(f'moved; {n} build files/scripts path-rewritten')


if __name__ == '__main__':
    main()
