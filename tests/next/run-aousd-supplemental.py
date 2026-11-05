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


# Per-prim keys of the corpus pcp.json that the structural compare covers.
# Every stack is a JSON object keyed by layer (strongest first; a layer that
# contributes several sites keeps its first position and last site), so the
# compare is dict-equality plus key order.
PCP_COMPARED_KEYS = ("Prim Stack", "Child names", "Property names",
                     "Property stacks")


def load_pcp_allowlist(path):
    """`<case-name> <reason>` lines; `#` comments and blank lines ignored."""
    allow = {}
    if not path:
        return allow
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        allow[name] = reason.strip()
    return allow


def diff_stack(expected, actual):
    """Ordered-object compare of a prim/property stack. Returns a short
    description of the first difference or None."""
    exp_keys = list(expected)
    act_keys = list(actual)
    if exp_keys != act_keys:
        return f"layers expected {exp_keys} got {act_keys}"
    for key in exp_keys:
        if expected[key] != actual[key]:
            return f"{key}: expected {expected[key]!r} got {actual[key]!r}"
    return None


def diff_pcp(expected, actual, strict_order=False):
    """Structural diff of two pcp.json documents. Returns a list of
    (prim_path, key, message) tuples, strongest-first per prim.

    "Property names" that differ only in order are reported with the key
    "Property names (order)" and, unless strict_order, do not count: next
    keeps relationships in an unordered table, so pxr's authored
    attribute/relationship interleaving is not reproducible. Child-name
    order always counts."""
    diffs = []
    exp_prims = expected.get("Composing", {})
    act_prims = actual.get("Composing", {})
    exp_layers = expected.get("Layer Stack")
    act_layers = actual.get("Layer Stack")
    if exp_layers is not None and exp_layers != act_layers:
        diffs.append(("<root>", "Layer Stack",
                      f"expected {exp_layers} got {act_layers}"))
    for path in exp_prims:
        if path not in act_prims:
            diffs.append((path, "<prim>", "missing from next"))
    for path in act_prims:
        if path not in exp_prims:
            diffs.append((path, "<prim>", "extra in next"))
    for path, exp in exp_prims.items():
        act = act_prims.get(path)
        if act is None:
            continue
        for key in PCP_COMPARED_KEYS:
            e = exp.get(key)
            a = act.get(key)
            if e is None and a is None:
                continue
            # The corpus omits empty lists/objects; next does the same, but
            # tolerate an explicit empty value on either side.
            if not e and not a:
                continue
            if e is None or a is None:
                diffs.append((path, key, f"expected {e!r} got {a!r}"))
            elif key == "Prim Stack":
                msg = diff_stack(e, a)
                if msg:
                    diffs.append((path, key, msg))
            elif key == "Property names" and e != a and sorted(e) == sorted(a):
                if strict_order:
                    diffs.append((path, key, f"expected {e} got {a}"))
                else:
                    diffs.append((path, "Property names (order)",
                                  f"expected {e} got {a}"))
            elif key == "Property stacks":
                # Keyed by property path; the corpus lists them sorted.
                if sorted(e) != sorted(a):
                    diffs.append((path, key,
                                  f"props expected {sorted(e)} got {sorted(a)}"))
                for prop in e:
                    if prop in a:
                        msg = diff_stack(e[prop], a[prop])
                        if msg:
                            diffs.append((path, prop, msg))
            elif e != a:
                diffs.append((path, key, f"expected {e} got {a}"))
    return diffs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite-root",
                        default=os.environ.get("AOUSD_CORE_SUPPLEMENTAL_ROOT"))
    parser.add_argument("--lusdcat", "--next-usdcat", dest="next_usdcat",
                        required=True,
                        help="unified lusdcat CLI (legacy alias: --next-usdcat)")
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
    parser.add_argument("--compare-pcp", action="store_true",
                        help="composition: also run `--dump-pcp-json -` and "
                             "diff prim/property stacks and child/property "
                             "names against each case's pcp.json")
    parser.add_argument("--max-pcp-diff", type=int, default=None,
                        help="ratchet ceiling for differing (non-allowlisted) "
                             "pcp cases; default: report only")
    parser.add_argument("--pcp-allowlist",
                        help="file of `<case> <reason>` lines known to differ")
    parser.add_argument("--pcp-strict-order", action="store_true",
                        help="count property-name order-only differences")
    parser.add_argument("--pcp-diff-lines", type=int, default=4,
                        help="differences to print per case")
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
    pcp_compared = 0
    pcp_results = {}  # case name -> diffs
    pcp_order_only = {}  # case name -> property-name order-only diffs
    pcp_allow = load_pcp_allowlist(args.pcp_allowlist)

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
            if args.compare_pcp:
                compare_command = [args.next_usdcat, "--instance-mode", "native",
                                   "--variant-fallback", "standin=render,proxy",
                                   "--dump-pcp-json", "-", str(entry)]
                dump = subprocess.run(compare_command, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
                name = case_dir.name
                pcp_compared += 1
                if dump.returncode:
                    pcp_results[name] = [("<root>", "<dump>",
                                          "dump failed: " +
                                          (dump.stderr.strip().splitlines() or
                                           ["?"])[-1])]
                    continue
                try:
                    actual = json.loads(dump.stdout)
                except json.JSONDecodeError as exc:
                    pcp_results[name] = [("<root>", "<dump>",
                                          f"invalid JSON: {exc}")]
                    continue
                diffs = diff_pcp(expected, actual, args.pcp_strict_order)
                order_only = [d for d in diffs if d[1].endswith("(order)")]
                if order_only:
                    pcp_order_only[name] = order_only
                diffs = [d for d in diffs if not d[1].endswith("(order)")]
                if diffs:
                    pcp_results[name] = diffs
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

    pcp_blocking = False
    if args.compare_pcp:
        matching = pcp_compared - len(pcp_results)
        unexpected = sorted(n for n in pcp_results if n not in pcp_allow)
        allowed = sorted(n for n in pcp_results if n in pcp_allow)
        xpass = sorted(n for n in pcp_allow if n not in pcp_results and
                       (not args.case or n in set(args.case)))
        print(f"AOUSD pcp compare: {pcp_compared} compared, {matching} "
              f"matching, {len(allowed)} allowlisted, {len(unexpected)} "
              f"unexpected diff(s), {len(pcp_order_only)} with property-name "
              f"order-only differences (not counted)")
        for name in sorted(pcp_order_only):
            if name in pcp_results:
                continue
            diffs = pcp_order_only[name]
            print(f"  order-only: {name}: {len(diffs)} difference(s)")
            for path, key, msg in diffs[:max(args.pcp_diff_lines, 0)]:
                print(f"    {path} [{key}]: {msg}")
        for name in sorted(pcp_results):
            tag = "allowlisted" if name in pcp_allow else "DIFF"
            diffs = pcp_results[name]
            print(f"  {tag}: {name}: {len(diffs)} difference(s)")
            for path, key, msg in diffs[:max(args.pcp_diff_lines, 0)]:
                print(f"    {path} [{key}]: {msg}")
        for name in xpass:
            print(f"  XPASS: {name} matches but is allowlisted "
                  f"({pcp_allow[name]}); prune it")
        if args.max_pcp_diff is not None and len(unexpected) > args.max_pcp_diff:
            print(f"AOUSD pcp compare: {len(unexpected)} unexpected diff(s) "
                  f"exceed ratchet {args.max_pcp_diff}", file=sys.stderr)
            pcp_blocking = True
        if args.max_pcp_diff is not None and xpass:
            print("AOUSD pcp compare: allowlist entries now pass (prune)",
                  file=sys.stderr)
            pcp_blocking = True
    if len(composition_failures) > args.max_composition_fail:
        blocking.extend(composition_failures)
    if failures:
        print(f"AOUSD supplemental: {len(failures)} observed gap(s)",
              file=sys.stderr)
        for category, path, diagnostic in failures:
            last = diagnostic.strip().splitlines()[-1] if diagnostic.strip() else "failed"
            print(f"  {category}: {path}: {last}", file=sys.stderr)
    if blocking or pcp_blocking:
        return 1
    if composition_failures:
        print(f"AOUSD supplemental: PASS at composition ratchet "
              f"{len(composition_failures)}/{args.max_composition_fail}")
    else:
        print("AOUSD supplemental: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
