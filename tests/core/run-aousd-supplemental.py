#!/usr/bin/env python3
"""Run lightusd-next against an external AOUSD Core supplemental release.

The corpus is intentionally not vendored. Point --suite-root (or
AOUSD_CORE_SUPPLEMENTAL_ROOT) at releases/1.0.1 in the official
aousd/core-spec-supplemental-public checkout, or a standalone release.
"""

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys


def run(command):
    return subprocess.run(command, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True)


# Per-prim keys of the corpus pcp.json that the structural compare covers.
# Legacy v1 stacks are objects keyed by layer, collapsing repeated sites.
# Lossless v2 stacks are strongest-first arrays of layer/path pairs; their
# comparison preserves order, multiplicity and every authored site.
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
    """Compare ordered v1 objects or lossless v2 site arrays.

    Return the first difference, including multiplicity, or None.
    """
    if isinstance(expected, list) or isinstance(actual, list):
        if not isinstance(expected, list) or not isinstance(actual, list):
            return 'stack schema differs (ordered sites versus collapsed layer object)'
        for i, (left, right) in enumerate(zip(expected, actual)):
            if left != right:
                return f'site {i}: expected {left!r} got {right!r}'
        if len(expected) != len(actual):
            return f'site count expected {len(expected)} got {len(actual)}'
        return None
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



def verify_invalid_text_case(case_dir, expected, classification, next_usdcat,
                             oracle_python=None, oracle_version="26.11", oracle_env=None):
    """Verify a hash-pinned negative root and all available binary dependencies.

    Classifications are not composition exemptions: changed bytes, accepted text,
    a different failure, or an unreadable binary dependency fails the gate.
    """
    errors = []
    text = case_dir / classification['text']
    if not text.is_file() or hashlib.sha256(text.read_bytes()).hexdigest() != classification['sha256']:
        return ['invalid-text fixture hash differs; independent reclassification required']
    if pathlib.Path(classification['text']) != pathlib.Path('usda') / expected['Entry']:
        errors.append('entry path differs from the reviewed text root')
    binary_root = case_dir / expected['Entry']
    if binary_root.is_file() != classification['binaryRootPresent']:
        errors.append('binary root presence differs from the reviewed release')
    for flags in (['-l'], ['--dump-pcp-json', '-', '--pcp-json-version', '2']):
        result = subprocess.run([next_usdcat, *flags, str(text)], capture_output=True, text=True)
        if result.returncode == 0 or classification['nextDiagnostic'] not in result.stderr:
            errors.append('negative text did not produce the required rejection: ' + ' '.join(flags))
    oracle_script = str(pathlib.Path(__file__).with_name('dump-pcp-oracle.py'))
    if oracle_python:
        result = subprocess.run([oracle_python, oracle_script, '--expected-version',
            oracle_version, str(text)], capture_output=True, text=True, env=oracle_env)
        try:
            report = json.loads(result.stdout)
            if (result.returncode != 3 or report.get('status') != 'parse-error' or
                    classification['stockDiagnostic'] not in report.get('error', '')):
                errors.append('stock oracle did not confirm the classified text rejection')
        except ValueError:
            errors.append('stock rejection oracle failed: ' + result.stderr[-800:])
    binaries = classification['binaryDependencies']
    available = {path.name for path in case_dir.glob('*.usd') if path != binary_root}
    if available != set(binaries):
        errors.append('available original binary dependencies differ from the reviewed release')
    for name, digest in binaries.items():
        path = case_dir / name
        if (not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest or
                path.read_bytes()[:8] != b'PXR-USDC'):
            errors.append('original binary dependency missing or changed: ' + name)
            continue
        result = run([next_usdcat, '-l', str(path)])
        if result.returncode:
            errors.append('original binary dependency failed to load: ' + name + ': ' + result.stderr[-800:])
        if oracle_python:
            result = subprocess.run([oracle_python, oracle_script, '--expected-version',
                oracle_version, str(path)], capture_output=True, text=True, env=oracle_env)
            if result.returncode:
                errors.append('stock binary dependency check failed: ' + name + ': ' + result.stderr[-800:])
    return errors


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
    parser.add_argument("--pcp-oracle-python",
                        help="also compare lossless v2 JSON against stock OpenUSD")
    parser.add_argument("--pcp-oracle-pythonpath",
                        help="isolated site-packages directory for the stock oracle interpreter")
    parser.add_argument("--pcp-oracle-version", default="26.11",
                        help="required usd-core wheel version (default: 26.11)")
    parser.add_argument("--max-pcp-oracle-diff", type=int, default=0,
                        help="lossless oracle difference ceiling; default zero")
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
    if args.max_pcp_oracle_diff < 0:
        parser.error('--max-pcp-oracle-diff must be nonnegative')
    if args.pcp_oracle_pythonpath and not args.pcp_oracle_python:
        parser.error('--pcp-oracle-pythonpath requires --pcp-oracle-python')
    if args.pcp_oracle_python and 'composition' not in categories:
        parser.error('--pcp-oracle-python requires the composition category')
    invalid_manifest = json.loads(pathlib.Path(__file__).with_name(
        'aousd-invalid-text.json').read_text(encoding='utf-8'))
    invalid_cases = invalid_manifest['cases']
    invalid_rejections = 0
    oracle_results = {}
    oracle_compared = 0
    oracle_env = os.environ.copy()
    if args.pcp_oracle_pythonpath:
        oracle_env['PYTHONPATH'] = str(pathlib.Path(args.pcp_oracle_pythonpath).resolve())
        oracle_env.pop('LD_LIBRARY_PATH', None)
        oracle_env.pop('PXR_PLUGINPATH_NAME', None)
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
        if len(cases) != invalid_manifest['compositionCases']:
            parser.error('composition case count differs from the pinned release; no cases may be silently omitted')
        if not set(invalid_cases).issubset({case.parent.name for case in cases}):
            parser.error('reviewed invalid-text cases are missing from the supplemental suite')
        if args.case:
            selected = set(args.case)
            cases = [case for case in cases if case.parent.name in selected]
        if args.case and set(args.case) != {case.parent.name for case in cases}:
            parser.error('one or more --case names do not exist in the supplemental suite')
        for expected_path in cases:
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
            case_dir = expected_path.parent
            classification = invalid_cases.get(case_dir.name)
            if classification:
                errors = verify_invalid_text_case(case_dir, expected, classification,
                    args.next_usdcat, args.pcp_oracle_python, args.pcp_oracle_version, oracle_env)
                if errors:
                    failures.append(('invalid_text', case_dir, '\n'.join(errors)))
                else:
                    invalid_rejections += 1
                continue
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
            if args.pcp_oracle_python:
                oracle_compared += 1
                oracle_command = [args.pcp_oracle_python,
                    str(pathlib.Path(__file__).with_name('dump-pcp-oracle.py')),
                    '--expected-version', args.pcp_oracle_version,
                    '--variant-fallback', 'standin=render,proxy', str(entry)]
                oracle = subprocess.run(oracle_command, capture_output=True,
                                        text=True, env=oracle_env)
                lossless_command = [args.next_usdcat, '--instance-mode', 'native',
                    '--variant-fallback', 'standin=render,proxy',
                    '--dump-pcp-json', '-', '--pcp-json-version', '2', str(entry)]
                lossless = subprocess.run(lossless_command, capture_output=True, text=True)
                if oracle.returncode or lossless.returncode:
                    oracle_results[case_dir.name] = [('<root>', '<oracle>',
                        f'oracle exit {oracle.returncode}, next exit {lossless.returncode}: ' +
                        (oracle.stderr or oracle.stdout or lossless.stderr)[-800:])]
                else:
                    try:
                        truth = json.loads(oracle.stdout)
                        actual_v2 = json.loads(lossless.stdout)
                        if truth.get('schemaVersion') != 2 or actual_v2.get('schemaVersion') != 2:
                            raise ValueError('lossless comparison requires schemaVersion 2')
                        differences = diff_pcp(truth, actual_v2, strict_order=True)
                        if differences:
                            oracle_results[case_dir.name] = differences
                    except (ValueError, TypeError) as error:
                        oracle_results[case_dir.name] = [('<root>', '<oracle>', str(error))]
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

    if invalid_rejections:
        print(f"AOUSD classified invalid text: {invalid_rejections} verified rejection(s); "
              "all reviewed original binary dependencies loaded")
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
    if args.pcp_oracle_python:
        print(f'AOUSD lossless PCP oracle: {oracle_compared} compared, '
              f'{oracle_compared - len(oracle_results)} matching, '
              f'{len(oracle_results)} differing (strict order, no allowlist)')
        for name, differences in sorted(oracle_results.items()):
            print(f'  ORACLE-DIFF: {name}: {len(differences)} difference(s)')
            for path, key, message in differences[:max(args.pcp_diff_lines, 0)]:
                print(f'    {path} [{key}]: {message}')
        if len(oracle_results) > args.max_pcp_oracle_diff:
            print('AOUSD lossless PCP oracle: difference ceiling exceeded', file=sys.stderr)
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
