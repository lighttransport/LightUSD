#!/usr/bin/env python3
"""Repair MaterialX files whose ``fileprefix`` duplicates their ``file`` values.

Some third-party MaterialX documents set both

    <materialx fileprefix="./textures/">
      <input name="file" value="./textures/brass_color.jpg"/>

so the resolved path becomes ``./textures/./textures/brass_color.jpg`` and the
reference renderer (OpenUSD/HdSt) cannot find the texture, substituting its
fallback. The candidate renderer resolves it either way, so the comparison is
not like-for-like: the SDK capture is marked INCOMPLETE.

This script does not edit the document (which would change what the candidate
reads). It creates an idempotent relative symlink so the doubled path resolves
to the real directory, e.g.

    textures/textures -> .

Usage:
    python3 tools/view_control/fix_mtlx_fileprefix.py <asset-root> [--check]

Exit codes: 0 = nothing to do / repaired, 1 = --check found a broken prefix.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

_PREFIX = re.compile(r'fileprefix\s*=\s*"([^"]*)"')
_FILE = re.compile(r'<input\s+name="file"[^>]*value="([^"]*)"')


def doubled_prefixes(text: str) -> set[str]:
    """Return fileprefix values that every file input already carries."""
    m = _PREFIX.search(text)
    if not m or not m.group(1):
        return set()
    prefix = m.group(1)
    files = _FILE.findall(text)
    if files and all(f.startswith(prefix) for f in files):
        return {prefix}
    return set()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("assets_root", type=Path)
    ap.add_argument("--check", action="store_true",
                    help="report without creating anything")
    args = ap.parse_args(argv)

    root: Path = args.assets_root.resolve()
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 2

    repaired = 0
    broken = 0
    for mtlx in root.rglob("*.mtlx"):
        try:
            text = mtlx.read_text(errors="replace")
        except OSError:
            continue
        prefixes = doubled_prefixes(text)
        if not prefixes:
            continue
        for prefix in prefixes:
            # The doubled path is prefix + prefix, e.g. textures/textures.
            # A self-referential link inside the real directory makes the
            # inner copy resolve without editing the document.
            inner = mtlx.parent / prefix.strip("/") / prefix.strip("/")
            if inner.exists() or inner.is_symlink():
                continue
            print(f"{mtlx.relative_to(root)}: doubled prefix {prefix!r} -> "
                  f"{inner.relative_to(root)} -> .")
            if args.check:
                broken += 1
                continue
            inner.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(".", inner)
            repaired += 1

    if args.check:
        return 1 if broken else 0
    print(f"repaired {repaired} doubled MaterialX prefix(es)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
