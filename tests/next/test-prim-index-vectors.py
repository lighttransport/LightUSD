#!/usr/bin/env python3
"""Replay Lean-computed layered arc edits through production PCP and stock."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
VECTORS = HERE / 'generated/lean/prim-index-vectors.json'
EXPECTED_CASES = 256
MECHANISMS = HERE / 'generated/lean/prim-index-mechanism-vectors.json'
EXPECTED_MECHANISMS = 10

ORACLE = r'''
import importlib.util, json, pathlib, sys
spec = importlib.util.spec_from_file_location('oracle', sys.argv[1])
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)
provenance = oracle.oracle_provenance('26.11')
results = []
for case in json.loads(pathlib.Path(sys.argv[2]).read_text()):
    truth = oracle.dump(case['root'], provenance, {}, case['loadPayloads'])
    assert truth['status'] == 'ok', (case['id'], truth)
    results.append({'parent': truth['Composing'].get(case['query']),
                    'child': truth['Composing'].get(case['query'] + '/Child')})
print(json.dumps(results))
'''


def verify(case, actual, label):
    expected = case['expected']
    if not expected:
        if actual is not None:
            raise AssertionError((label, case['id'], 'unexpected queried prim', case['query']))
        return
    if actual is None:
        raise AssertionError((label, case['id'], 'missing queried prim', case['query']))
    if actual['Prim Stack'] != expected:
        raise AssertionError((label, case['id'], 'Prim Stack', expected, actual['Prim Stack']))
    # The targets author a marker; local P specs do not. Derive property sites
    # from the Lean stack rather than implementing arc list edits in Python.
    properties = [{'layer': entry['layer'], 'path': entry['path'] + '.marker'}
                  for entry in expected if entry['path'] != case['query']]
    got = actual.get('Property stacks', {}).get(case['query'] + '.marker', [])
    if got != properties:
        raise AssertionError((label, case['id'], 'Property Stack', properties, got))
    if ('marker' in actual.get('Property names', [])) != bool(properties):
        raise AssertionError((label, case['id'], 'marker presence', actual))


def mech_check(case, actual, label):
    if not case['expected']:
        if actual is not None:
            raise AssertionError((label, case['id'], 'prim should not exist', actual))
        return
    if actual is None or actual['Prim Stack'] != case['expected']:
        raise AssertionError((label, case['id'], case['expected'],
                              None if actual is None else actual['Prim Stack']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lusdcat', required=True, type=Path)
    parser.add_argument('--vectors', default=VECTORS, type=Path)
    parser.add_argument('--oracle-python')
    parser.add_argument('--oracle-pythonpath')
    args = parser.parse_args()
    cases = json.loads(args.vectors.read_text())['cases']
    if len(cases) != EXPECTED_CASES or len({case['id'] for case in cases}) != EXPECTED_CASES:
        raise AssertionError('prim-index vector coverage changed or contains duplicate IDs')
    tool = str(args.lusdcat.resolve())
    with tempfile.TemporaryDirectory(prefix='lightusd-lean-primindex-') as directory:
        base = Path(directory)
        requests = []
        for case in cases:
            folder = base / case['id']
            folder.mkdir()
            for name in ['root', 'weak', 'asset']:
                (folder / (name + '.usda')).write_text(case[name])
            root = folder / 'root.usda'
            command = [tool, '--dump-pcp-json', '-', '--pcp-json-version', '2']
            if not case['loadPayloads']:
                command.append('--defer-payloads')
            result = subprocess.run(command + [str(root)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError((case['id'], result.stderr))
            actual = json.loads(result.stdout)['Composing']
            verify(case, actual.get(case['query']), 'native')
            child_case = dict(case, query=case['query'] + '/Child', expected=case['expectedChild'])
            verify(child_case, actual.get(child_case['query']), 'native-child')
            requests.append({'id': case['id'], 'root': str(root), 'query': case['query'],
                             'loadPayloads': case['loadPayloads']})
        if args.oracle_python:
            env = os.environ.copy()
            if args.oracle_pythonpath:
                env['PYTHONPATH'] = str(Path(args.oracle_pythonpath).resolve())
                env.pop('LD_LIBRARY_PATH', None)
                env.pop('PXR_PLUGINPATH_NAME', None)
            request_file = base / 'requests.json'
            request_file.write_text(json.dumps(requests))
            result = subprocess.run([args.oracle_python, '-c', ORACLE,
                str(HERE / 'dump-pcp-oracle.py'), str(request_file)],
                capture_output=True, text=True, env=env)
            if result.returncode:
                raise AssertionError(result.stderr)
            truths = json.loads(result.stdout)
            if len(truths) != len(cases):
                raise AssertionError('stock oracle omitted vector cases')
            for case, truth in zip(cases, truths):
                verify(case, truth['parent'], 'stock')
                child_case = dict(case, query=case['query'] + '/Child', expected=case['expectedChild'])
                verify(child_case, truth['child'], 'stock-child')
    mechanisms = json.loads(MECHANISMS.read_text())['cases']
    if len(mechanisms) != EXPECTED_MECHANISMS:
        raise AssertionError('prim-index mechanism vector coverage changed')
    with tempfile.TemporaryDirectory(prefix='lightusd-lean-primindex-mech-') as directory:
        base = Path(directory)
        requests = []
        for case in mechanisms:
            folder = base / case['id']
            folder.mkdir()
            for name, text in case['files'].items():
                (folder / name).write_text(text)
            root = folder / 'root.usda'
            command = [tool, '--dump-pcp-json', '-', '--pcp-json-version', '2']
            if not case['loadPayloads']:
                command.append('--defer-payloads')
            result = subprocess.run(command + [str(root)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError((case['id'], result.stderr))
            actual = json.loads(result.stdout)['Composing'].get(case['query'])
            mech_check(case, actual, 'native')
            requests.append({'id': case['id'], 'root': str(root), 'query': case['query'],
                             'loadPayloads': case['loadPayloads']})
        if args.oracle_python:
            request_file = base / 'requests.json'
            request_file.write_text(json.dumps(requests))
            result = subprocess.run([args.oracle_python, '-c', ORACLE,
                str(HERE / 'dump-pcp-oracle.py'), str(request_file)],
                capture_output=True, text=True, env=env)
            if result.returncode:
                raise AssertionError(result.stderr)
            for case, truth in zip(mechanisms, json.loads(result.stdout)):
                mech_check(case, truth['parent'], 'stock')
    print(f'Lean prim-index mechanisms: {len(mechanisms)} implied-class/relocate/payload/variant-selection cases pass')
    print(f'Lean prim-index: {len(cases)} layered arc-edit/load cases and 512 prim/property queries pass native' +
          (' and pinned stock OpenUSD' if args.oracle_python else ''))


if __name__ == '__main__':
    main()
