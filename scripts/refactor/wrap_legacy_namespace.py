#!/usr/bin/env python3
"""Wrap legacy `namespace lightusd` bodies in `inline namespace legacy`.

Usage: wrap_legacy_namespace.py [--inline] [files...]
       (default: all C/C++ sources under src/legacy, plain namespace)

For every top-level `namespace lightusd {` (and C++17 `namespace
lightusd::x::y {`) the body is wrapped:

    namespace lightusd { inline namespace legacy {  ...  }  // namespace legacy
    }

`inline` keeps every existing `lightusd::X` user compiling while core takes
over the plain `lightusd::` names; --plain later turns it into a regular
namespace. Braces are matched with a small lexer that skips comments, string,
raw-string and character literals. Part of the next->core refactor.
"""
import re
import subprocess
import sys

OPEN_RE = re.compile(r'namespace\s+lightusd((?:\s*::\s*\w+)*)\s*\{')


def lex_skip(s, i):
    """If s[i] starts a comment or literal, return the index after it, else None."""
    c = s[i]
    if s.startswith('//', i):
        j = s.find('\n', i)
        return len(s) if j < 0 else j
    if s.startswith('/*', i):
        j = s.find('*/', i + 2)
        return len(s) if j < 0 else j + 2
    m = re.match(r'(?:u8|u|U|L)?R"([^(\s]{0,16})\(', s[i:i + 24])
    if m and (i == 0 or not (s[i - 1].isalnum() or s[i - 1] == '_')):
        end = ')' + m.group(1) + '"'
        j = s.find(end, i + m.end())
        return len(s) if j < 0 else j + len(end)
    if c in '"\'':
        # A quote right after a digit with a digit following is a C++14 digit
        # separator (1'000), not a char literal.
        if c == "'" and i > 0 and s[i - 1].isalnum() and i + 1 < len(s) and s[i + 1].isalnum():
            return i + 1
        j = i + 1
        while j < len(s) and s[j] != c:
            if s[j] == '\\':
                j += 1
            elif s[j] == '\n':
                break
            j += 1
        return j + 1
    return None


def find_close(s, open_idx):
    """Index of the '}' matching the '{' at open_idx."""
    depth = 0
    i = open_idx
    while i < len(s):
        k = lex_skip(s, i)
        if k is not None:
            i = k
            continue
        if s[i] == '{':
            depth += 1
        elif s[i] == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError('unbalanced braces')


def code_positions(s):
    """Yield indices that are real code (outside comments/literals)."""
    i = 0
    while i < len(s):
        k = lex_skip(s, i)
        if k is not None:
            i = k
            continue
        yield i
        i += 1


def transform(s, inline):
    kw = 'inline namespace legacy' if inline else 'namespace legacy'
    if 'namespace legacy' in s:
        return s, 0
    code = set()
    for m in OPEN_RE.finditer(s):
        code.add(m.start())
    real = {i for i in code_positions(s)} & code
    out, last, count = [], 0, 0
    pos = 0
    matches = [m for m in OPEN_RE.finditer(s) if m.start() in real]
    # Only top-level occurrences: skip matches nested inside an earlier one.
    top, end_of_prev = [], -1
    for m in matches:
        if m.start() > end_of_prev:
            brace = m.end() - 1
            close = find_close(s, brace)
            top.append((m, brace, close))
            end_of_prev = close
    for m, brace, close in top:
        inner = [p.strip() for p in m.group(1).split('::') if p.strip()]
        opening = 'namespace lightusd { ' + kw + ' {'
        closing = '}  // namespace legacy\n'
        for name in inner:
            opening += ' namespace ' + name + ' {'
            closing = '}  // namespace ' + name + '\n' + closing
        out.append(s[last:m.start()])
        out.append(opening)
        out.append(s[brace + 1:close])
        out.append(closing)
        out.append('}')
        last = close + 1
        count += 1
    out.append(s[last:])
    return ''.join(out), count


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    inline = '--inline' in sys.argv
    files = args or [f for f in subprocess.run(['git', 'ls-files', 'src/legacy'], capture_output=True,
                                               text=True, check=True).stdout.split()
                     if f.endswith(('.cc', '.c', '.cpp', '.hh', '.h', '.hpp', '.inc', '.inl'))]
    total = changed = 0
    for f in files:
        s = open(f, encoding='utf-8', errors='surrogateescape').read()
        try:
            t, n = transform(s, inline)
        except ValueError as e:
            print(f'SKIP {f}: {e}', file=sys.stderr)
            continue
        if n:
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(t)
            total += n
            changed += 1
    print(f'{total} namespace blocks wrapped in {changed} files')


if __name__ == '__main__':
    main()
