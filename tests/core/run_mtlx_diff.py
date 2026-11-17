#!/usr/bin/env python3
"""Differential check of the C11 MaterialX core against the C++ oracle.

  run_mtlx_diff.py --work DIR --oracle EXE [--candidate EXE] [--conformance DIR]

Builds a corpus in DIR/corpus:
  - every tracked *.mtlx file of the repository (copied with its directory, so
    relative XIncludes keep working);
  - every *.mtlx under --conformance (default
    /mnt/nvme02/work/materialx-conformance, skipped when absent);
  - every XML document embedded in the Lean vector tables
    (tests/core/generated/lean-mtlx/*.inc).
Runs the oracle on each file (DIR/oracle/<id>.txt) and, with --candidate, the
candidate (DIR/candidate/<id>.txt), then reports byte differences. Exit 1 when
any output differs.
"""
import argparse
import ast
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def lean_documents():
    """XML string arguments of the generated vector macros."""
    out = []
    lit = re.compile(r'"(?:[^"\\]|\\.)*"')
    d = os.path.join(HERE, "generated", "lean-mtlx")
    for name in sorted(os.listdir(d)):
        if not name.endswith(".inc"):
            continue
        for line in open(os.path.join(d, name), encoding="utf-8"):
            for m in lit.finditer(line):
                s = ast.literal_eval(m.group(0))
                if s.lstrip().startswith("<") and "materialx" in s:
                    out.append(s)
    return list(dict.fromkeys(out))


def build_corpus(work, conformance):
    corpus = os.path.join(work, "corpus")
    shutil.rmtree(corpus, ignore_errors=True)
    os.makedirs(corpus)
    files = []
    tracked = subprocess.run(["git", "-C", ROOT, "ls-files", "*.mtlx"], capture_output=True,
                             text=True, check=True).stdout.split()
    for rel in tracked:
        dst = os.path.join(corpus, "repo", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), dst)
        files.append(dst)
    if conformance and os.path.isdir(conformance):
        for dirpath, _, names in os.walk(conformance):
            if "/build" in dirpath or "/.git" in dirpath:
                continue
            for n in sorted(names):
                if n.endswith(".mtlx"):
                    files.append(os.path.join(dirpath, n))
    for i, xml in enumerate(lean_documents()):
        p = os.path.join(corpus, "lean", f"doc{i:04d}.mtlx")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(xml)
        files.append(p)
    return files


def run(exe, files, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    ids = []
    for i, path in enumerate(files):
        r = subprocess.run([exe, path], capture_output=True, timeout=120)
        ident = f"{i:05d}"
        with open(os.path.join(out_dir, ident + ".txt"), "wb") as f:
            f.write(b"# " + path.encode() + b"\n" + r.stdout + b"# exit " +
                    str(r.returncode).encode() + b"\n")
        ids.append(ident)
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--candidate")
    ap.add_argument("--conformance", default="/mnt/nvme02/work/materialx-conformance")
    ap.add_argument("--max-report", type=int, default=10)
    a = ap.parse_args()
    files = build_corpus(a.work, a.conformance)
    print(f"corpus: {len(files)} documents")
    ids = run(a.oracle, files, os.path.join(a.work, "oracle"))
    if not a.candidate:
        return 0
    run(a.candidate, files, os.path.join(a.work, "candidate"))
    bad = []
    for ident, path in zip(ids, files):
        o = open(os.path.join(a.work, "oracle", ident + ".txt"), "rb").read()
        c = open(os.path.join(a.work, "candidate", ident + ".txt"), "rb").read()
        if o != c:
            bad.append((ident, path))
    for ident, path in bad[:a.max_report]:
        print(f"DIFF {ident} {path}")
    print(f"{len(files) - len(bad)}/{len(files)} identical")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
