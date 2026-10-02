#!/usr/bin/env python3
"""Compare exact reference error identifiers, sites, severities, and process status.

No wildcard rule-family matching. Additional LightUSD findings are allowed;
reference crashes, missing inputs, unparsed diagnostics and empty corpora fail.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from checker_parity_fixtures import generate

ROOT = Path(__file__).resolve().parents[1]
FINDING = re.compile(r'^(Error|Warning|Warn):\s*\(([^)]+:[^).]+\.[^)]+)\)\s*(.*)$', re.M)
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def run(command, timeout):
    p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if p.returncode < 0: raise RuntimeError(f'process crashed: {command}: {p.returncode}')
    return p, ANSI.sub('', p.stdout + '\n' + p.stderr)


def prim_path(path):
    return path.split('.', 1)[0]


def matches_site(message, issue, scope):
    if scope == 'layer': return True
    sites = re.findall(r'<(/[^>\s]*)>', message)
    sites += re.findall(r'(?:prim path:|[Pp]rim:|prim|path|jointPrim)\s+[\'\"]?(/[^\s,;\'\"<>]+)', message)
    sites += re.findall(r'(?:for|at) (/[^\s,;]+)', message)
    sites += re.findall(r'\((/[^)\s]+)\)', message)
    if not sites: raise RuntimeError('cannot extract reference site: ' + message)
    actual = prim_path(issue['location'])
    for site in sites:
        expected = prim_path(site.rstrip('.'))
        if actual == expected: return True
        if scope == 'subtree' and actual.startswith(expected + '/'): return True
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--usdchecker', default=os.environ.get('USDCHECKER_PATH'))
    p.add_argument('--lusdchecker', default=os.environ.get('LUSDCHECKER_PATH', str(ROOT/'build_ninja/lusdchecker')))
    p.add_argument('--fixtures', action='append', type=Path)
    p.add_argument('--require-reference', action='store_true', default=os.environ.get('REQUIRE_USDCHECKER') == '1')
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--report', type=Path)
    p.add_argument('--generated-only', action='store_true')
    a = p.parse_args()
    if not a.usdchecker:
        candidates = [ROOT/'ref/dist/bin/usdchecker', ROOT.parents[1]/'OpenUSD/dist/bin/usdchecker']
        a.usdchecker = next((str(c) for c in candidates if c.is_file()), shutil.which('usdchecker'))
    if not a.usdchecker or not Path(a.usdchecker).is_file():
        if a.require_reference: p.error('reference usdchecker is required')
        print('SKIP: reference usdchecker unavailable'); return 0
    manifest = json.loads((ROOT/'tools/lusdchecker/reference-errors.json').read_text())
    mapping = manifest['errors']
    reference, inventory_text = run([a.usdchecker, '--dumpRules'], a.timeout)
    if reference.returncode: raise RuntimeError('reference rule inventory failed: ' + inventory_text)
    inventory = set(re.findall(r'^\[([^\]]+)\]:', inventory_text, re.M))
    mapped_validators = {key.rsplit('.', 1)[0] for key in mapping}
    unknown = inventory - mapped_validators
    if unknown: raise RuntimeError('unmapped reference validators: ' + ', '.join(sorted(unknown)))
    directories = a.fixtures or [Path(os.environ.get('FIXTURE_DIR', ROOT/'tests/usda')), ROOT/'tools/lusdchecker/testdata', ROOT/'tests/feat/lusdchecker']
    if a.generated_only: directories = []
    fixtures = sorted({f.resolve() for d in directories for f in (d.rglob('*') if d.is_dir() else [d])
                       if f.is_file() and f.suffix.lower() in ('.usd', '.usda', '.usdc', '.usdz')})
    if not fixtures and not a.generated_only: raise RuntimeError('empty fixture corpus')
    generated = tempfile.TemporaryDirectory(prefix='checker-parity-')
    fixtures += generate(Path(generated.name))
    if not fixtures: raise RuntimeError('empty fixture corpus')
    failures, observed, checked = [], set(), 0
    for fixture in fixtures:
        try:
            ref, text = run([a.usdchecker, '--skipVariants', str(fixture)], a.timeout)
            out, _ = run([a.lusdchecker, '--json', '--usdchecker-compat', '--composed', '--skip-variants', str(fixture)], a.timeout)
            report = json.loads(out.stdout)
            if out.returncode not in (0, 1, 2): raise RuntimeError('unexpected checker status')
            findings = FINDING.findall(text)
            if not findings and ref.returncode:
                if 'Failed to open stage' in text or 'Failed to open layer' in text:
                    # Both tools must reject the input. A reader may defer a
                    # malformed authored opinion to semantic validation.
                    if out.returncode not in (1, 2) or not any(i['severity'] == 'error' for i in report['issues']):
                        failures.append([str(fixture), 'reference parse failure was not rejected', out.returncode])
                    checked += 1; continue
                raise RuntimeError('reference failed without recognizable diagnostics: ' + text[-1000:])
            if not findings and 'Success!' not in text:
                raise RuntimeError('reference produced no validation result: ' + text[-1000:])
            if out.returncode == 2:
                failures.append([str(fixture), 'checker could not validate reference-readable input', report.get('issues')])
                checked += 1; continue
            for severity, error, message in findings:
                observed.add(error)
                if error not in mapping:
                    failures.append([str(fixture), 'unmapped error', error]); continue
                row = mapping[error]
                candidates = [i for i in report['issues'] if i['ruleId'] in row['rules']]
                expected_severity = 'error' if severity == 'Error' else 'warning'
                if not any(i['severity'] == expected_severity and matches_site(message, i, row['site']) for i in candidates):
                    failures.append([str(fixture), 'missing matching finding/site/severity', error, message])
            if fixture.parent == Path(generated.name) and fixture.stem in ('clean', 'physics-valid'):
                if ref.returncode != 0 or out.returncode != 0:
                    failures.append([str(fixture), 'positive reference fixture failed', ref.returncode, out.returncode])
            if ref.returncode == 1 and out.returncode != 1:
                failures.append([str(fixture), 'reference failure did not fail checker', out.returncode])
            checked += 1
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append([str(fixture), 'harness failure', str(error)])
    summary = {'referenceCommit': manifest['referenceCommit'], 'fixtures': checked,
               'registeredValidators': sorted(inventory), 'observedErrors': sorted(observed),
               'unexercisedErrors': sorted(set(mapping)-observed), 'failures': failures}
    if a.report: a.report.write_text(json.dumps(summary, indent=2)+'\n')
    print(f'Checker parity: {checked} fixtures, {len(inventory)} validators, {len(observed)} observed error IDs, {len(failures)} failures')
    for failure in failures: print(json.dumps(failure))
    return int(bool(failures))


if __name__ == '__main__': sys.exit(main())
