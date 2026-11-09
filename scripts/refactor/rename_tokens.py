#!/usr/bin/env python3
"""Rename `next`-named build/preprocessor identifiers (token map).

Usage: rename_tokens.py [--check|--apply]

--check reports, for every token to be renamed, whether its new name already
occurs in the repository (a collision). --apply rewrites code, CMake, scripts
and CI (docs, vendored src/external and scripts/refactor are skipped).
Part of the next->core refactor; delete with scripts/refactor/.
"""
import re
import subprocess
import sys

# Explicit renames (applied first, longest first).
EXPLICIT = {
    # CMake options whose plain names were already taken / need new meaning
    'LIGHTUSD_NEXT_BUILD_TOOLS': 'LIGHTUSD_BUILD_LUSDCAT',
    'LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE': 'LIGHTUSD_LEGACY_USE_CORE_PCP',
    'TYDRA_NEXT_USE_PCH': 'LIGHTUSD_TYDRA_USE_PCH',
    'TYDRA_NEXT_BUILD_TESTS': 'LIGHTUSD_TYDRA_BUILD_TESTS',
    'TYDRA_NEXT_SOURCES': 'TYDRA_SOURCES',
    'TYDRA_NEXT_HEADERS': 'TYDRA_HEADERS',
    # preprocessor
    'LIGHTUSD_WASM_WITH_NEXT': 'LIGHTUSD_WASM_WITH_CORE',
    'LIGHTUSD_URDF_TO_USD_NEXT': 'LIGHTUSD_URDF_TO_USD_CORE',
    'PERFRC_ENABLE_NEXT': 'PERFRC_ENABLE_CORE',
    'PERFRC_HAS_NEXT': 'PERFRC_HAS_CORE',
    'NEXT_PCP_READ_LOCK': 'LIGHTUSD_PCP_READ_LOCK',
    'NEXT_PCP_WRITE_LOCK': 'LIGHTUSD_PCP_WRITE_LOCK',
    'NEXT_PCP_LOCK': 'LIGHTUSD_PCP_LOCK',
    # CMake-internal variables
    'NEXT_SOURCES': 'CORE_SOURCES',
    'NEXT_FUZZ_LIB_FLAGS': 'CORE_FUZZ_LIB_FLAGS',
    'NEXT_FUZZ_BIN_FLAGS': 'CORE_FUZZ_BIN_FLAGS',
    'NEXT_TEST_TARGET': 'CORE_TEST_TARGET',
    'NEXT_CORPUS_ASSETS_DIR': 'CORE_CORPUS_ASSETS_DIR',
    'NEXT_PXR_USDCAT': 'CORE_PXR_USDCAT',
    'NEXT_PXR_FLATTEN_ARGS': 'CORE_PXR_FLATTEN_ARGS',
    'NEXT_TSAN_FLAGS': 'CORE_TSAN_FLAGS',
    'NEXT_SETARCH': 'CORE_SETARCH',
    'NEXT_KNOWN_REJECTS': 'CORE_KNOWN_REJECTS',
    # plain names already used by legacy: keep a CORE_ infix
    'LIGHTUSD_NEXT_ALLOC_JEMALLOC': 'LIGHTUSD_CORE_ALLOC_JEMALLOC',
    'LIGHTUSD_NEXT_ALLOC_MIMALLOC': 'LIGHTUSD_CORE_ALLOC_MIMALLOC',
    'LIGHTUSD_NEXT_ALLOC_TCMALLOC': 'LIGHTUSD_CORE_ALLOC_TCMALLOC',
    'LIGHTUSD_NEXT_EMCC_LINK_FLAGS': 'LIGHTUSD_CORE_EMCC_LINK_FLAGS',
    'LIGHTUSD_NEXT_EMCC_LINK_OPT_FLAGS': 'LIGHTUSD_CORE_EMCC_LINK_OPT_FLAGS',
    'LIGHTUSD_NEXT_NODE_EXECUTABLE': 'LIGHTUSD_CORE_NODE_EXECUTABLE',
    'LIGHTUSD_NEXT_SOURCES': 'LIGHTUSD_CORE_SOURCES',
    'LIGHTUSD_NEXT_WASM_WITH_EXR': 'LIGHTUSD_CORE_WASM_WITH_EXR',
    'LIGHTUSD_NEXT_WITH_TCMALLOC': 'LIGHTUSD_CORE_WITH_TCMALLOC',
}
# Generic: LIGHTUSD_NEXT_<X> -> LIGHTUSD_<X> (the names that collided are
# handled above or in the hand-edited root CMakeLists core block).
GENERIC = re.compile(r'\bLIGHTUSD_NEXT_([A-Z0-9_]+)\b')
# Tokens that are deprecated aliases kept verbatim on purpose.
KEEP = {'LIGHTUSD_WASM_NEXT_ONLY'}
SKIP_PREFIX = ('doc/', 'src/external/', 'scripts/refactor/', 'examples/common/')
EXTS = ('.cc', '.c', '.cpp', '.cxx', '.hh', '.h', '.hpp', '.inc', '.inl', '.mm', '.m',
        '.txt', '.cmake', '.in', '.sh', '.py', '.yml', '.yaml', '.mjs', '.js', '.toml', '.bat', '.ps1')


def files():
    out = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if f.endswith(EXTS) and not f.startswith(SKIP_PREFIX)
            and not f.endswith('.md')]


def new_name(tok):
    if tok in KEEP:
        return None
    if tok in EXPLICIT:
        return EXPLICIT[tok]
    m = GENERIC.fullmatch(tok)
    return f'LIGHTUSD_{m.group(1)}' if m else None


def main():
    fs = files()
    corpus = {f: open(f, encoding='utf-8', errors='surrogateescape').read() for f in fs}
    tok_re = re.compile(r'\b(?:' + '|'.join(sorted(map(re.escape, EXPLICIT), key=len, reverse=True))
                        + r'|LIGHTUSD_NEXT_[A-Z0-9_]+)\b')
    toks = set()
    for t in corpus.values():
        toks |= set(tok_re.findall(t))
    if '--check' in sys.argv:
        everything = '\n'.join(corpus.values())
        for tok in sorted(toks):
            nn = new_name(tok)
            if nn and re.search(r'\b' + re.escape(nn) + r'\b', everything):
                print(f'COLLISION {tok} -> {nn}')
        print(f'{len(toks)} tokens to rename')
        return
    n = 0
    for f, t in corpus.items():
        s = tok_re.sub(lambda m: new_name(m.group(0)) or m.group(0), t)
        if s != t:
            n += 1
            open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    print(f'{len(toks)} tokens renamed in {n} files')


if __name__ == '__main__':
    main()
