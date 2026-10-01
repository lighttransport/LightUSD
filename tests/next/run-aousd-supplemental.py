#!/usr/bin/env python3
"""Run lightusd-next against an external AOUSD Core supplemental release.

The corpus is intentionally not vendored. Point --suite-root (or
AOUSD_CORE_SUPPLEMENTAL_ROOT) at releases/1.0.1 in the official
aousd/core-spec-supplemental-public checkout, or a standalone release.
"""

import argparse
import json
import os
import pathlib
import subprocess
import sys


def run(command):
    return subprocess.run(command, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite-root",
                        default=os.environ.get("AOUSD_CORE_SUPPLEMENTAL_ROOT"))
    parser.add_argument("--next-usdcat", required=True)
    parser.add_argument("--aousd-test")
    parser.add_argument("--aousd-value-test",
                        help="test_aousd_value_resolution binary: asserts the "
                             "corpus's expected resolved sample values")
    parser.add_argument("--max-composition-fail", type=int, default=0,
                        help="ratchet ceiling for known composition gaps")
    parser.add_argument("--category", action="append",
                        choices=("data_types", "file_formats", "composition",
                                 "value_resolution"))
    parser.add_argument("--case", action="append",
                        help="composition case directory name to run")
    args = parser.parse_args()
    if not args.suite_root:
        parser.error("--suite-root or AOUSD_CORE_SUPPLEMENTAL_ROOT is required")
    root = pathlib.Path(args.suite_root).resolve()
    # Official releases inherit the checkout's license two levels above;
    # older standalone release archives carry it directly in the suite root.
    licensed = (root / "LICENSE").is_file() or (
        root.parent.name == "releases" and
        (root.parent.parent / "LICENSE").is_file())
    if not licensed or not (root / "composition").is_dir():
        parser.error(f"not an AOUSD Core supplemental checkout: {root}")
    categories = set(args.category or
                     ("data_types", "file_formats", "composition",
                      "value_resolution"))
    failures = []
    counts = {}

    if "data_types" in categories:
        documents = sorted((root / "data_types" / "tests").glob("*.json"))
        case_count = 0
        for document in documents:
            payload = json.loads(document.read_text(encoding="utf-8"))
            case_count += len(payload) if isinstance(payload, (list, dict)) else 1
        if not args.aousd_test:
            failures.append(("data_types", root / "data_types",
                             "--aousd-test is required for the data-type bridge"))
        else:
            result = run((args.aousd_test,))
            if result.returncode:
                failures.append(("data_types", pathlib.Path(args.aousd_test),
                                 result.stderr))
        counts["data_types"] = case_count

    if "file_formats" in categories:
        assets = sorted((root / "file_formats" / "tests" / "assets").rglob("*.usda"))
        assets += sorted((root / "file_formats" / "tests" / "assets").rglob("*.usdc"))
        for asset in assets:
            result = run((args.next_usdcat, "-l", str(asset)))
            if result.returncode:
                failures.append(("file_formats", asset, result.stderr))
        counts["file_formats"] = len(assets)

    if "composition" in categories:
        cases = sorted((root / "composition" / "tests" / "assets").glob("*/pcp.json"))
        if args.case:
            selected = set(args.case)
            cases = [case for case in cases if case.parent.name in selected]
        for expected_path in cases:
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
            case_dir = expected_path.parent
            entry = case_dir / expected["Entry"]
            text_entry = case_dir / "usda" / expected["Entry"]
            if text_entry.is_file():
                entry = text_entry
            # The expected PCP paths include instance-proxy descendants. Keep
            # native instancing for the query; holder/prototype flatten modes
            # intentionally replace duplicate subtrees with references.
            # The corpus expectations (pcp.txt) were generated in a pxr
            # environment with the classic Presto standin->render variant
            # fallback registered (e.g. case1 requires /FergusCloak/rig, which
            # only composes under standin=render). next's default is now
            # fallback-free like stock usdcat, so opt in here.
            command = [args.next_usdcat, "-f", "--instance-mode", "native",
                       "--variant-fallback", "standin=render,proxy"]
            for prim_path in expected.get("Composing", {}):
                command.extend(("--require-prim", prim_path))
            command.append(str(entry))
            result = run(command)
            if result.returncode:
                failures.append(("composition", expected_path.parent,
                                 result.stderr))
        counts["composition"] = len(cases)

    if "value_resolution" in categories:
        # The upstream tests open one entry layer per case; auxiliary clip and
        # manifest fragments are intentionally not standalone documents.
        assets = sorted((root / "value_resolution" / "tests" / "assets").glob(
            "*/entry.usd"))
        for asset in assets:
            result = run((args.next_usdcat, "-f", str(asset)))
            if result.returncode:
                failures.append(("value_resolution", asset, result.stderr))
        counts["value_resolution"] = len(assets)
        # Sampled-value oracle: compare resolved values (bracketing, layer
        # offsets, interpolation, clips) against the corpus expectations.
        if args.aousd_value_test:
            result = run((args.aousd_value_test, str(root)))
            if result.returncode:
                failures.append(("value_resolution",
                                 pathlib.Path(args.aousd_value_test),
                                 result.stderr or "sampled-value oracle failed"))

    for category in sorted(counts):
        print(f"AOUSD supplemental {category}: {counts[category]} cases")
    composition_failures = [f for f in failures if f[0] == "composition"]
    blocking = [f for f in failures if f[0] != "composition"]
    if len(composition_failures) > args.max_composition_fail:
        blocking.extend(composition_failures)
    if failures:
        print(f"AOUSD supplemental: {len(failures)} observed gap(s)",
              file=sys.stderr)
        for category, path, diagnostic in failures:
            last = diagnostic.strip().splitlines()[-1] if diagnostic.strip() else "failed"
            print(f"  {category}: {path}: {last}", file=sys.stderr)
    if blocking:
        return 1
    if composition_failures:
        print(f"AOUSD supplemental: PASS at composition ratchet "
              f"{len(composition_failures)}/{args.max_composition_fail}")
    else:
        print("AOUSD supplemental: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
