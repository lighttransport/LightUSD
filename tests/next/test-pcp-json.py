#!/usr/bin/env python3
"""Exercise lossless PCP JSON and the ordered comparison through the real CLI."""
import argparse
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('supplemental', HERE / 'run-aousd-supplemental.py')
supplemental = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supplemental)
ARGS = None


class ComparisonTests(unittest.TestCase):
    def test_duplicate_layers_and_order_are_observable(self):
        entries = [{'layer': 'root.usda', 'path': '/P'},
                   {'layer': 'root.usda', 'path': '/Class'}]
        self.assertIsNone(supplemental.diff_stack(entries, entries))
        self.assertIsNotNone(supplemental.diff_stack(entries, entries[::-1]))
        self.assertIsNotNone(supplemental.diff_stack(entries, entries[-1:]))
        self.assertIsNotNone(supplemental.diff_stack(entries, {'root.usda': '/Class'}))
        self.assertIsNone(supplemental.diff_stack({'a': '/P', 'b': '/Q'},
                                                 {'a': '/P', 'b': '/Q'}))
        self.assertIsNotNone(supplemental.diff_stack({'a': '/P', 'b': '/Q'},
                                                    {'b': '/Q', 'a': '/P'}))

    def test_changed_negative_fixture_requires_reclassification(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-negative-fixture-') as directory:
            base = Path(directory)
            root = base / 'root.usda'
            root.write_text('#usda 1.0\ndef "P" {}\n')
            classification = {'text': root.name,
                'sha256': hashlib.sha256(root.read_bytes()).hexdigest()}
            root.write_text(root.read_text() + 'def "Q" {}\n')
            errors = supplemental.verify_invalid_text_case(base,
                {'Entry': root.name}, classification, '/missing/executable')
            self.assertEqual(len(errors), 1)
            self.assertIn('reclassification required', errors[0])

    def test_missing_corpus_cases_fail_instead_of_reducing_coverage(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-incomplete-corpus-') as directory:
            base = Path(directory)
            (base / 'LICENSE').write_text('fixture')
            case = base / 'composition' / 'tests' / 'assets' / 'OnlyCase'
            case.mkdir(parents=True)
            (case / 'pcp.json').write_text('{"Entry":"root.usd","Composing":{}}')
            result = subprocess.run([sys.executable, str(HERE / 'run-aousd-supplemental.py'),
                '--suite-root', str(base), '--next-usdcat', ARGS.lusdcat,
                '--category', 'composition'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('case count differs from the pinned release', result.stderr)

    def test_property_name_order_is_strict_when_requested(self):
        left = {'Composing': {'/P': {'Property names': ['a', 'r']}}}
        right = {'Composing': {'/P': {'Property names': ['r', 'a']}}}
        self.assertEqual(supplemental.diff_pcp(left, right, True)[0][1], 'Property names')


class CliTests(unittest.TestCase):
    def assert_stock_matches(self, root, actual):
        if not ARGS.oracle_python:
            return
        env = os.environ.copy()
        if ARGS.oracle_pythonpath:
            env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
            env.pop('LD_LIBRARY_PATH', None)
            env.pop('PXR_PLUGINPATH_NAME', None)
        result = subprocess.run([ARGS.oracle_python,
            str(HERE / 'dump-pcp-oracle.py'), str(root)],
            capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        truth = json.loads(result.stdout)
        self.assertEqual(supplemental.diff_pcp(truth, actual, True), [])
        return truth

    def test_versions_preserve_repeated_prim_and_property_sites(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-json-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('''#usda 1.0
class "Class" {
  int x = 1
}
def "P" (inherits = </Class>) {
  int x = 2
}
''')
            def run(*flags):
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                                         *flags, str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(result.stdout)
            legacy = run()
            self.assertEqual(legacy, run('--pcp-json-version', '1'))
            self.assertNotIn('schemaVersion', legacy)
            self.assertEqual(legacy['Composing']['/P']['Prim Stack'], {'root.usda': '/Class'})
            lossless = run('--pcp-json-version', '2')
            self.assertEqual(lossless['schemaVersion'], 2)
            self.assertEqual(lossless['Composing']['/P']['Prim Stack'],
                             [{'layer': 'root.usda', 'path': '/P'},
                              {'layer': 'root.usda', 'path': '/Class'}])
            self.assertEqual(lossless['Composing']['/P']['Property stacks']['/P.x'],
                             [{'layer': 'root.usda', 'path': '/P.x'},
                              {'layer': 'root.usda', 'path': '/Class.x'}])
            for flags in (['--pcp-json-version'], ['--pcp-json-version', '3'],
                          ['--pcp-json-version', '2', str(root)]):
                invalid = subprocess.run([ARGS.lusdcat, *flags], capture_output=True, text=True)
                self.assertEqual(invalid.returncode, 2)
                self.assertIn('--pcp-json-version', invalid.stderr)
            if ARGS.oracle_python:
                env = os.environ.copy()
                if ARGS.oracle_pythonpath:
                    env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
                    env.pop('LD_LIBRARY_PATH', None)
                    env.pop('PXR_PLUGINPATH_NAME', None)
                command = [ARGS.oracle_python, str(HERE / 'dump-pcp-oracle.py'), str(root)]
                oracle = subprocess.run(command, capture_output=True, text=True, env=env)
                self.assertEqual(oracle.returncode, 0, oracle.stderr)
                expected = json.loads(oracle.stdout)
                self.assertEqual(expected['oracle']['version'], '26.11')
                self.assertEqual(supplemental.diff_pcp(expected, lossless, True), [])
                wrong_version = subprocess.run(command + ['--expected-version', '0.0'],
                                               capture_output=True, text=True, env=env)
                self.assertEqual(wrong_version.returncode, 2)
                root.write_text('#usda 1.0\ndef "P" {\n')
                malformed = subprocess.run(command, capture_output=True, text=True, env=env)
                self.assertEqual(malformed.returncode, 3)
                self.assertEqual(json.loads(malformed.stdout)['status'], 'parse-error')

    def test_variant_sites_follow_each_composed_host_including_empty_options(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-variants-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('''#usda 1.0
def "Model" (prepend variantSets = "look"; variants = {string look = "a"}) {
  variantSet "look" = {
    "a" {
      int x = 1
      def "ChildA" {}
    }
    "b" {
      int x = 2
      def "ChildB" {}
    }
    "empty" {}
  }
}
def "P" (references = </Model>; variants = {string look = "a"}) {}
def "Q" (references = </Model>; variants = {string look = "b"}) {}
def "R" (references = </Model>; variants = {string look = "empty"}) {}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = json.loads(result.stdout)
            for host, choice in [('P', 'a'), ('Q', 'b'), ('R', 'empty')]:
                sites = actual['Composing']['/' + host]['Prim Stack']
                self.assertEqual(sites, [
                    {'layer': 'root.usda', 'path': '/' + host},
                    {'layer': 'root.usda', 'path': '/Model'},
                    {'layer': 'root.usda', 'path': '/Model{look=' + choice + '}'}])
            if ARGS.oracle_python:
                env = os.environ.copy()
                if ARGS.oracle_pythonpath:
                    env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
                    env.pop('LD_LIBRARY_PATH', None)
                    env.pop('PXR_PLUGINPATH_NAME', None)
                result = subprocess.run([ARGS.oracle_python,
                    str(HERE / 'dump-pcp-oracle.py'), str(root)],
                    capture_output=True, text=True, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(supplemental.diff_pcp(json.loads(result.stdout), actual, True), [])


    def test_distinct_reference_offsets_retain_repeated_sites(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-offsets-') as directory:
            root = Path(directory) / 'root.usda'
            (root.parent / 'asset.usda').write_text(
                '#usda 1.0\ndef "A" {\n int x = 1\n}\n')
            root.write_text('#usda 1.0\ndef "P" (references = [\n'
                ' @asset.usda@</A> (offset = 0),\n'
                ' @asset.usda@</A> (offset = 1)\n]) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            actual = document['Composing']['/P']
            self.assertEqual(actual['Prim Stack'], [
                {'layer': 'root.usda', 'path': '/P'},
                {'layer': 'asset.usda', 'path': '/A'},
                {'layer': 'asset.usda', 'path': '/A'}])
            self.assertEqual(actual['Property stacks']['/P.x'], [
                {'layer': 'asset.usda', 'path': '/A.x'},
                {'layer': 'asset.usda', 'path': '/A.x'}])


    def test_instance_descendant_queries_ignore_local_overs(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-instance-') as directory:
            root = Path(directory) / 'root.usda'
            (root.parent / 'asset.usda').write_text(
                '#usda 1.0\ndef "A" {\n def "Child" {\n int x = 1\n }\n}\n')
            root.write_text('#usda 1.0\ndef "P" (references = @asset.usda@</A>; '
                'instanceable = true) {\n over "Child" {\n int x = 2\n }\n}\n'
                'def "Q" (references = @asset.usda@</A>; instanceable = false) '
                '{\n over "Child" {\n int x = 3\n }\n}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            actual = document['Composing']
            self.assertEqual(actual['/P/Child']['Prim Stack'], [
                {'layer': 'asset.usda', 'path': '/A/Child'}])
            self.assertEqual(actual['/P/Child']['Property stacks']['/P/Child.x'], [
                {'layer': 'asset.usda', 'path': '/A/Child.x'}])
            self.assertEqual(actual['/Q/Child']['Prim Stack'], [
                {'layer': 'root.usda', 'path': '/Q/Child'},
                {'layer': 'asset.usda', 'path': '/A/Child'}])


    def test_property_stacks_filter_kind_but_retain_value_type_conflicts(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-property-kind-') as directory:
            root = Path(directory) / 'root.usda'
            (root.parent / 'asset.usda').write_text(
                '#usda 1.0\ndef "A" {\n double x = 1\n string y = "weak"\n rel z\n}\n')
            root.write_text('#usda 1.0\ndef "P" (references = @asset.usda@</A>) '
                '{\n rel x\n double y = 3\n double z = 2\n}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            stacks = document['Composing']['/P']['Property stacks']
            for name in ('x', 'z'):
                self.assertEqual(stacks['/P.' + name], [
                    {'layer': 'root.usda', 'path': '/P.' + name}])
            self.assertEqual(stacks['/P.y'], [
                {'layer': 'root.usda', 'path': '/P.y'},
                {'layer': 'asset.usda', 'path': '/A.y'}])


    def test_external_layers_with_equal_basenames_keep_distinct_identifiers(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-layer-ids-') as directory:
            base = Path(directory)
            for folder in ('scene', 'a', 'b'):
                (base / folder).mkdir()
            for folder in ('a', 'b'):
                (base / folder / 'asset.usda').write_text(
                    '#usda 1.0\ndef "A" {\n int x = 1\n}\n')
            root = base / 'scene' / 'root.usda'
            root.write_text('#usda 1.0\ndef "P" (references = [\n'
                ' @../a/asset.usda@</A>, @../b/asset.usda@</A>\n]) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            self.assertEqual(document['Composing']['/P']['Prim Stack'], [
                {'layer': 'root.usda', 'path': '/P'},
                {'layer': '../a/asset.usda', 'path': '/A'},
                {'layer': '../b/asset.usda', 'path': '/A'}])


    def test_variant_path_text_rejections_match_stock(self):
        fields = [field + ' = </Target{look=a}Child>'
                  for field in ('references', 'payload', 'inherits', 'specializes')]
        edits = ['relocates = {</P{look=a}Child>: </P/Moved>}',
                 'relocates = {</P/Child>: </P{look=a}Moved>}']
        sources = ['#usda 1.0\ndef "P" (' + field + ') {}\n' for field in fields + edits]
        sources += ['#usda 1.0\n(' + edit + ')\ndef "P" {}\n' for edit in edits]
        with tempfile.TemporaryDirectory(prefix='lightusd-invalid-variant-paths-') as directory:
            root = Path(directory) / 'root.usda'
            for source in sources:
                with self.subTest(source=source):
                    root.write_text(source)
                    result = subprocess.run([ARGS.lusdcat, '-l', str(root)],
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('cannot contain variant selections', result.stderr)
                    if ARGS.oracle_python:
                        env = os.environ.copy()
                        if ARGS.oracle_pythonpath:
                            env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
                            env.pop('LD_LIBRARY_PATH', None)
                            env.pop('PXR_PLUGINPATH_NAME', None)
                        result = subprocess.run([ARGS.oracle_python,
                            str(HERE / 'dump-pcp-oracle.py'), str(root)],
                            capture_output=True, text=True, env=env)
                        self.assertEqual(result.returncode, 3, result.stderr)
                        self.assertEqual(json.loads(result.stdout)['status'], 'parse-error')


    def test_child_order_restatement_preserves_unordered_prefix(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-child-order-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('#usda 1.0\nreorder rootPrims = ["b", "a"]\ndef "P" {\n'
                ' reorder nameChildren = ["b", "a"]\n'
                ' def "x" {}\n def "a" {}\n def "b" {}\n}\n'
                'def "x" {}\ndef "a" {}\ndef "b" {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            self.assertEqual(document['Composing']['/P']['Child names'], ['x', 'b', 'a'])
            roots = [path for path in document['Composing'] if path.count('/') == 1]
            self.assertEqual(roots, ['/P', '/x', '/b', '/a'])
            if truth is not None:
                stock_roots = [path for path in truth['Composing'] if path.count('/') == 1]
                self.assertEqual(roots, stock_roots)


    def test_each_spec_orders_the_accumulated_raw_child_names(self):
        for bulk in (0, 40):
            with self.subTest(bulk=bulk), tempfile.TemporaryDirectory(
                    prefix='lightusd-composed-child-order-') as directory:
                base = Path(directory)
                (base / 'asset.usd').write_text('#usda 1.0\ndef "A" {\n def "b" {}\n' +
                    ''.join(' def "n' + str(i) + '" {}\n' for i in range(bulk)) + '}\n')
                (base / 'middle.usd').write_text('#usda 1.0\n'
                    'def "P" (references = @asset.usd@</A>) {\n'
                    ' reorder nameChildren = ["b", "a"]\n'
                    ' def "a" {}\n def "u" {}\n def "b" {}\n def "v" {}\n}\n')
                root = base / 'root.usd'
                root.write_text('#usda 1.0\n(subLayers = [@middle.usd@])\n'
                    'over "P" {\n def "extra" {}\n def "n1" {}\n}\n')
                for binary in (False, True):
                    if binary:
                        crate_dir = base / 'crates'
                        crate_dir.mkdir()
                        for name in ('asset.usd', 'middle.usd', 'root.usd'):
                            rewrite = subprocess.run([ARGS.lusdcat, '--rewrite-layer',
                                '--output-format', 'usdc', '-o', str(crate_dir / name),
                                str(base / name)], capture_output=True, text=True)
                            self.assertEqual(rewrite.returncode, 0, rewrite.stderr)
                        root = crate_dir / 'root.usd'
                    result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                        '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    document = json.loads(result.stdout)
                    self.assert_stock_matches(root, document)
                    expected = ['b'] + ['n' + str(i) for i in range(bulk)] + ['a', 'u', 'v', 'extra']
                    if not bulk:
                        expected.append('n1')
                    self.assertEqual(document['Composing']['/P']['Child names'], expected)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lusdcat', required=True)
    parser.add_argument('--oracle-python')
    parser.add_argument('--oracle-pythonpath')
    ARGS = parser.parse_args()
    ARGS.lusdcat = str(Path(ARGS.lusdcat).resolve())
    unittest.main(argv=[__file__])
