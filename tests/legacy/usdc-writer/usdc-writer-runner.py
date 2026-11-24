#!/usr/bin/env python3
"""
USDC writer roundtrip test runner.

Pipeline per file:
  1. lusdcat input.usda -o temp.usdc    (LightUSD writes USDC)
  2. lusdcat temp.usdc  -o temp_rt.usda (LightUSD reads USDC back as USDA)
  3. lusddiff input.usda temp_rt.usda   (Layer-level diff on two USDA files)

lusddiff exit codes: 0 = identical, 1 = differences, 2 = error.

Usage:
  python usdc-writer-runner.py \
    --lusdcat ./build/lusdcat \
    --lusddiff ./build/lusddiff \
    --basedir tests/usda

  python usdc-writer-runner.py \
    --lusdcat ./build/lusdcat \
    --lusddiff ./build/lusddiff \
    --basedir tests/usda \
    --verbose --keep-tmp
"""

import argparse
import glob
import os
import subprocess
import sys
import tempfile


# Files known to diverge in Layer-level diff for reasons unrelated to the USDC
# writer (e.g. composition semantics, unsupported features).  Keep this list as
# short as possible and document the reason for each entry.
KNOWN_SKIP = set([
    # Add basenames here if needed, e.g.:
    # "some-known-issue.usda",
])


def run_tests(lusdcat: str, lusddiff: str, basedir: str,
              verbose: bool = False, keep_tmp: bool = False) -> int:
    if not os.path.isfile(lusdcat):
        print(f"Error: lusdcat not found: {lusdcat}", file=sys.stderr)
        return 1
    if not os.path.isfile(lusddiff):
        print(f"Error: lusddiff not found: {lusddiff}", file=sys.stderr)
        return 1
    if not os.path.isdir(basedir):
        print(f"Error: directory not found: {basedir}", file=sys.stderr)
        return 1

    usda_files = sorted(glob.glob(os.path.join(basedir, "*.usda")))
    if not usda_files:
        print(f"No .usda files found in {basedir}", file=sys.stderr)
        return 1

    total = len(usda_files)
    passed = 0
    failed = 0
    skipped = 0
    write_errors = 0
    failures = []

    tmpdir = tempfile.mkdtemp(prefix="usdc_writer_test_")

    print(f"USDC writer roundtrip: {total} files from {basedir}")
    print(f"  lusdcat:  {lusdcat}")
    print(f"  lusddiff: {lusddiff}")
    print(f"  tmpdir:   {tmpdir}")
    print()

    for filepath in usda_files:
        basename = os.path.basename(filepath)

        if basename in KNOWN_SKIP:
            skipped += 1
            if verbose:
                print(f"  SKIP: {basename}")
            continue

        stem = basename.replace(".usda", "")
        usdc_path = os.path.join(tmpdir, stem + ".usdc")
        usda_rt_path = os.path.join(tmpdir, stem + "_rt.usda")

        # Step 1: Write USDC
        wr = subprocess.run([lusdcat, filepath, "-o", usdc_path],
                            capture_output=True, text=True, timeout=60)
        if wr.returncode != 0:
            write_errors += 1
            failed += 1
            err_line = (wr.stderr.strip().split("\n")[0] if wr.stderr.strip() else "unknown error")
            failures.append((basename, f"write USDC failed: {err_line}"))
            if verbose:
                print(f"  FAIL (write USDC): {basename}")
                print(f"        {err_line}")
            continue

        # Step 2: Read USDC back and write USDA
        rd = subprocess.run([lusdcat, usdc_path, "-o", usda_rt_path],
                            capture_output=True, text=True, timeout=60)
        if rd.returncode != 0:
            write_errors += 1
            failed += 1
            err_line = (rd.stderr.strip().split("\n")[0] if rd.stderr.strip() else "unknown error")
            failures.append((basename, f"read-back failed: {err_line}"))
            if verbose:
                print(f"  FAIL (read-back): {basename}")
                print(f"        {err_line}")
            continue

        # Check for VALUE_PPRINT placeholder in roundtripped USDA.
        # Force UTF-8: the default text mode on Windows is cp1252, which
        # blows up on non-ASCII bytes that legitimately appear in USDA
        # output (token vectors, asset paths, etc.). errors="replace"
        # avoids hard failure on corrupted output — we only care about
        # finding the literal "VALUE_PPRINT" substring.
        try:
            with open(usda_rt_path, encoding="utf-8", errors="replace") as f:
                rt_content = f.read()
            if "VALUE_PPRINT" in rt_content:
                failed += 1
                vp_lines = [l.strip() for l in rt_content.split("\n")
                            if "VALUE_PPRINT" in l]
                failures.append((basename, f"VALUE_PPRINT bug: {vp_lines[0][:120]}"))
                if verbose:
                    print(f"  FAIL (VALUE_PPRINT): {basename}")
                    for vl in vp_lines[:3]:
                        print(f"        {vl[:120]}")
                continue
        except OSError:
            pass

        # Step 3: Diff original USDA vs roundtripped USDA
        dr = subprocess.run([lusddiff, filepath, usda_rt_path],
                            capture_output=True, text=True, timeout=60)

        if dr.returncode == 0:
            passed += 1
            if verbose:
                print(f"  PASS: {basename}")
            if not keep_tmp:
                for p in (usdc_path, usda_rt_path):
                    try:
                        os.unlink(p)
                    except OSError:
                        pass
        elif dr.returncode == 1:
            failed += 1
            diff_summary = dr.stdout.strip()[:200] if dr.stdout.strip() else "differences found"
            failures.append((basename, diff_summary))
            if verbose:
                print(f"  FAIL (diff): {basename}")
                if dr.stdout.strip():
                    for line in dr.stdout.strip().split("\n")[:5]:
                        print(f"        {line}")
        else:
            failed += 1
            err_line = (dr.stderr.strip().split("\n")[0] if dr.stderr.strip() else "diff error")
            failures.append((basename, f"diff error: {err_line}"))
            if verbose:
                print(f"  FAIL (diff error): {basename}")
                print(f"        {err_line}")

    # Summary
    print()
    print(f"Results: {passed}/{total} passed, {failed} failed, {skipped} skipped")

    if failures:
        print(f"\nFailed files ({len(failures)}):")
        for basename, reason in failures:
            print(f"  - {basename}")
            if reason:
                first_line = reason.split("\n")[0][:120]
                print(f"    {first_line}")

    if not keep_tmp:
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass  # non-empty if failures left temp files

    return 0 if failed == 0 else 1


def main():
    parser = argparse.ArgumentParser(
        description="USDC writer roundtrip test: lusdcat writes USDC, lusddiff compares")
    parser.add_argument("--lusdcat", required=True,
                        help="Path to lusdcat executable")
    parser.add_argument("--lusddiff", required=True,
                        help="Path to lusddiff executable")
    parser.add_argument("--basedir", required=True,
                        help="Directory containing .usda files")
    parser.add_argument("--verbose", action="store_true",
                        help="Verbose output")
    parser.add_argument("--keep-tmp", action="store_true",
                        help="Keep temporary USDC files")
    parser.add_argument("--report-only", action="store_true",
                        help="Report results but always exit 0 (informational mode)")
    args = parser.parse_args()
    rc = run_tests(args.lusdcat, args.lusddiff, args.basedir,
                   args.verbose, args.keep_tmp)
    return 0 if args.report_only else rc


if __name__ == "__main__":
    sys.exit(main())
