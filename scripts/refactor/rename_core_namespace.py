#!/usr/bin/env python3
"""Drop `next` from the core C++ namespaces.

Usage: rename_core_namespace.py [files...]   (default: all C/C++ sources, not src/external)

  lightusd::tydra::next -> lightusd::tydra
  lightusd::next        -> lightusd
  tydra::next           -> tydra          (bare, e.g. namespace aliases)
  next::X               -> ::lightusd::X  (bare qualifier; fully qualified so it
                                           also resolves from lightusd::legacy)
  namespace next { ... }  -> body kept, wrapper removed (brace-aware)

Part of the next->core refactor; delete with scripts/refactor/.
"""
import re
import subprocess
import sys

sys.path.insert(0, 'scripts/refactor')
from wrap_legacy_namespace import find_close, code_positions  # noqa: E402

EXTS = ('.cc', '.c', '.cpp', '.cxx', '.hh', '.h', '.hpp', '.inc', '.inl', '.mm', '.m')
SUBS = [
    (re.compile(r'\blightusd::tydra::next\b'), 'lightusd::tydra'),
    (re.compile(r'\blightusd::next\b'), 'lightusd'),
    (re.compile(r'(?<![\w:])tydra::next\b'), 'tydra'),
    (re.compile(r'(?<![\w:.>])next::(?=[A-Za-z_])'), '::lightusd::'),
]
OPEN = re.compile(r'namespace\s+next\s*\{')
CLOSE_COMMENT = re.compile(r'[ \t]*//[ \t]*namespace next\b[^\n]*')


def drop_next_blocks(s):
    count = 0
    while True:
        real = set(code_positions(s))
        m = next((m for m in OPEN.finditer(s) if m.start() in real), None)
        if m is None:
            return s, count
        brace = m.end() - 1
        close = find_close(s, brace)
        # Remove the closing brace (and a trailing `// namespace next` comment).
        tail = s[close + 1:]
        cm = CLOSE_COMMENT.match(tail)
        after = tail[cm.end():] if cm else tail
        if after.startswith('\n') and s[:close].rstrip(' \t').endswith('\n'):
            after = after[1:]
            head = s[:close].rstrip(' \t')
        else:
            head = s[:close]
        s = head + after
        # Remove the opener (and the rest of its line if now empty).
        line_end = s.find('\n', m.end())
        rest = s[m.end():line_end if line_end >= 0 else len(s)]
        if rest.strip() == '' and s[:m.start()].rstrip(' \t').endswith('\n'):
            start = len(s[:m.start()].rstrip(' \t'))
            s = s[:start] + s[(line_end + 1) if line_end >= 0 else len(s):]
        else:
            s = s[:m.start()] + s[m.end():]
        count += 1


def main():
    files = sys.argv[1:] or [f for f in subprocess.run(['git', 'ls-files'], capture_output=True,
                                                        text=True, check=True).stdout.split()
                             if f.endswith(EXTS) and not f.startswith('src/external/')
                             and not f.startswith('scripts/refactor/')]
    nfiles = nsubs = nblocks = 0
    for f in files:
        try:
            s = open(f, encoding='utf-8', errors='surrogateescape').read()
        except OSError:
            continue
        t = s
        for pat, rep in SUBS:
            t, k = pat.subn(rep, t)
            nsubs += k
        try:
            t, k = drop_next_blocks(t)
        except ValueError as e:
            print(f'SKIP blocks {f}: {e}', file=sys.stderr)
            k = 0
        nblocks += k
        if t != s:
            nfiles += 1
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(t)
    print(f'{nfiles} files: {nsubs} qualified names, {nblocks} `namespace next` blocks removed')


if __name__ == '__main__':
    main()
