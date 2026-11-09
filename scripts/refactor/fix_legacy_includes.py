#!/usr/bin/env python3
"""Fix quoted includes in src/legacy/ that relied on their old directory.

After the move, a legacy file such as src/legacy/tydra/render-data.hh that did
`#include "shape-to-mesh.hh"` (a shared header that stayed in src/tydra/) no
longer finds it next to itself. For each quoted include that does not resolve
relative to the file's new directory but did resolve relative to its old one
(src/<dir>), rewrite it to the src-root-relative path ("tydra/shape-to-mesh.hh"),
which the build finds through -I src. Part of the next->core refactor.
"""
import os
import re
import subprocess

INC = re.compile(r'^(\s*#\s*include\s*")([^"]+)(")', re.M)


def main():
    files = [f for f in subprocess.run(['git', 'ls-files', 'src/legacy'], capture_output=True,
                                       text=True, check=True).stdout.split()
             if f.endswith(('.cc', '.c', '.cpp', '.hh', '.h', '.hpp', '.inc', '.inl'))]
    changed = 0
    for f in files:
        new_dir = os.path.dirname(f)
        old_dir = 'src' + new_dir[len('src/legacy'):]
        text = open(f, encoding='utf-8', errors='surrogateescape').read()

        def repl(m):
            nonlocal changed
            inc = m.group(2)
            if os.path.exists(os.path.normpath(os.path.join(new_dir, inc))):
                return m.group(0)
            old_target = os.path.normpath(os.path.join(old_dir, inc))
            if not os.path.exists(old_target) or not old_target.startswith('src/'):
                return m.group(0)
            rel = os.path.relpath(old_target, 'src')
            # Must not be shadowed by a legacy file of the same src-relative path.
            if os.path.exists(os.path.join('src/legacy', rel)):
                return m.group(0)
            if rel == inc:
                return m.group(0)
            changed += 1
            return f'{m.group(1)}{rel}{m.group(3)}'

        new_text = INC.sub(repl, text)
        if new_text != text:
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(new_text)
    print(f'{changed} includes rewritten')


if __name__ == '__main__':
    main()
