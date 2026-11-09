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

    def assert_stock_rejects(self, root):
        if not ARGS.oracle_python:
            return
        env = os.environ.copy()
        if ARGS.oracle_pythonpath:
            env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
            env.pop('LD_LIBRARY_PATH', None)
            env.pop('PXR_PLUGINPATH_NAME', None)
        result = subprocess.run([ARGS.oracle_python, str(HERE / 'dump-pcp-oracle.py'), str(root)],
                                capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(json.loads(result.stdout)['status'], 'parse-error')

    def test_class_diamond_coalesces_the_repeated_class_source(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-class-diamond-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('''#usda 1.0
class "D" {
  int d = 1
}
class "B" (specializes = </D>) {}
class "C" (specializes = </D>) {}
def "A" (specializes = [</B>, </C>]) {}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            stack = document['Composing']['/A']['Prim Stack']
            self.assertEqual([item['path'] for item in stack], ['/A', '/B', '/D', '/C'])
            self.assertEqual(document['Composing']['/A']['Property stacks']['/A.d'],
                             [{'layer': 'root.usda', 'path': '/D.d'}])

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

    def test_nested_variant_sites_match_exact_option_identity(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-nested-sites-') as directory:
            root = Path(directory) / 'root.usda'
            for outer_inline in ['', 'int outerValue = 1\n']:
                for inner_inline in ['', 'int innerValue = 2\n']:
                    root.write_text('''#usda 1.0
def "P" (prepend variantSets = "outer"; variants = {string outer = "a"
string inner = "b"}) {
  variantSet "outer" = {
    "a" (prepend variantSets = "inner") {
      ''' + outer_inline + '''
      variantSet "inner" = {
        "b" (prepend variantSets = "outer") {
          ''' + inner_inline + '''
          def "InnerChild" {}
          variantSet "outer" = {
            "a" { def "DeepChild" {}
            }
          }
        }
      }
    }
  }
}
''')
                    result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                        '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    actual = json.loads(result.stdout)
                    self.assertEqual(actual['Composing']['/P']['Prim Stack'], [
                        {'layer': 'root.usda', 'path': path} for path in
                        ['/P', '/P{outer=a}', '/P{outer=a}{inner=b}',
                         '/P{outer=a}{inner=b}{outer=a}']])
                    self.assert_stock_matches(root, actual)

    def test_empty_inherited_variant_precedes_dependent_root_variant(self):
        for content, payload in [('', False), ('int marker = 11', False), ('', True)]:
            with self.subTest(content=content, payload=payload), tempfile.TemporaryDirectory(
                    prefix='lightusd-deferred-class-variant-') as directory:
                (Path(directory) / 'asset.usda').write_text(
                    '#usda 1.0\ndef "Asset" { int marker = 33\n}\n')
                root = Path(directory) / 'root.usda'
                root.write_text('''#usda 1.0
class "Class" (prepend variantSets = ["selector", "geometry"]) {
  variantSet "selector" = {
    "choose" (variants = {string geometry = "cube"}) {
      CONTENT
    }
  }
}
def "P" (inherits = </Class>;
         prepend variantSets = "geometry";
         variants = {string selector = "choose"}) {
  variantSet "geometry" = {
    "cube" { int marker = 22
      def "Child" {}
    }
  }
}
'''.replace('CONTENT', content).replace('"cube" {',
                    '"cube" (payload = @asset.usda@</Asset>) {' if payload else '"cube" {'))
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                actual = json.loads(result.stdout)
                self.assertEqual(actual['Composing']['/P']['Prim Stack'], [
                    {'layer': 'root.usda', 'path': path} for path in
                    ['/P', '/Class', '/Class{selector=choose}', '/P{geometry=cube}']] +
                    ([{'layer': 'asset.usda', 'path': '/Asset'}] if payload else []))
                self.assertEqual(actual['Composing']['/P']['Child names'], ['Child'])
                self.assert_stock_matches(root, actual)

    def test_subroot_reference_preserves_implied_class_strength(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-subroot-class-anchor-') as directory:
            base = Path(directory)
            (base / 'set.usda').write_text('''#usda 1.0
class "Inherit" {
  over "Model" { int marker = 33
  }
}
class "Specializes" {
  over "Model" { int marker = 44
  }
}
def "Set" (inherits = </Inherit>; specializes = </Specializes>) {
  def "Model" { int marker = 22
  }
}
''')
            root = base / 'root.usda'
            root.write_text('''#usda 1.0
over "Inherit" {
  over "Model" { int marker = 11
  }
}
over "Specializes" {
  over "Model" { int marker = 55
  }
}
def "Ref" (references = @set.usda@</Set/Model>) {}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = json.loads(result.stdout)
            self.assertEqual(actual['Composing']['/Ref']['Property stacks']['/Ref.marker'], [
                {'layer': layer, 'path': path + '.marker'} for layer, path in [
                    ('root.usda', '/Inherit/Model'), ('set.usda', '/Set/Model'),
                    ('set.usda', '/Inherit/Model'), ('root.usda', '/Specializes/Model'),
                    ('set.usda', '/Specializes/Model')]])
            self.assert_stock_matches(root, actual)

    def test_implied_subroot_inherit_uses_expressing_ancestor_variant(self):
        for selection in ['z', 'x', '', None]:
            with self.subTest(selection=selection), tempfile.TemporaryDirectory(
                    prefix='lightusd-implied-ancestor-variant-') as directory:
                base = Path(directory)
                (base / 'group.usda').write_text('''#usda 1.0
def "Group" (prepend variantSets = "v"; variants = {string v = "y"}) {
  variantSet "v" = {
    "y" { over "Model" { string a = "weak_y"
    }
    }
  }
}
def "Instance" (inherits = </Group/Model>) {}
''')
                root = base / 'root.usda'
                authored = ('variants = {string v = "' + selection + '"};'
                            if selection is not None else '')
                root.write_text('''#usda 1.0
over "Group" (SELECTION prepend variantSets = "v") {
  variantSet "v" = {
    "x" { over "Model" { string a = "strong_x"
    }
    }
    "z" { over "Model" { string a = "strong_z"
    }
    }
  }
}
def "Ref" (references = @group.usda@</Instance>) {}
'''.replace('SELECTION', authored))
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                actual = json.loads(result.stdout)
                expected = ([{'layer': 'root.usda',
                              'path': '/Group{v=' + selection + '}Model.a'}]
                            if selection in ['z', 'x'] else [])
                expected.append({'layer': 'group.usda', 'path': '/Group{v=y}Model.a'})
                self.assertEqual(actual['Composing']['/Ref']['Property stacks']['/Ref.a'],
                                 expected)
                self.assert_stock_matches(root, actual)

    def test_pending_variant_siblings_keep_inherit_order(self):
        for nested in [False, True]:
            with self.subTest(nested=nested), tempfile.TemporaryDirectory(
                    prefix='lightusd-empty-class-variants-') as directory:
                root = Path(directory) / 'root.usda'
                root.write_text('''#usda 1.0
class "A" (prepend variantSets = "a"; variants = {string a = "on"} INHERIT) {
  variantSet "a" = { "on" {} }
}
class "B" (prepend variantSets = "b"; variants = {string b = "on"}) {
  variantSet "b" = { "on" {} }
}
def "P" (inherits = INHERITS) {}
'''.replace('INHERITS', '</A>' if nested else '[</A>, </B>]')
                    .replace('INHERIT', '; inherits = </B>' if nested else ''))
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_stock_matches(root, json.loads(result.stdout))

    def test_implied_specializes_selections_remain_weak_across_references(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-pcp-specializes-') as directory:
            root = Path(directory) / 'root.usda'
            looks = root.with_name('looks.usda')
            for selected in ['brass', 'base', '', None]:
                selection = ('variants = {string v = "' + selected + '"}'
                             if selected is not None else '')
                looks.write_text('''#usda 1.0
def "Looks" {
  class "Metal" (prepend variantSets = "v"; variants = {string v = "base"}) {
    variantSet "v" = {
      "metal" { int marker = 1
      }
      "brass" { int marker = 2
      }
      "base" { int marker = 3
      }
    }
  }
  def "Brass" (specializes = </Looks/Metal>; ''' + selection + ''') {}
}
''')
                root.write_text('''#usda 1.0
def "Model" {
  def "Looks" (references = @looks.usda@</Looks>) {
    over "Metal" (variants = {string v = "metal"}) {}
    over "Brass" {}
  }
}
''')
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                actual = json.loads(result.stdout)
                chosen = selected if selected is not None else 'metal'
                sites = actual['Composing']['/Model/Looks/Brass']['Prim Stack']
                if chosen:
                    self.assertIn({'layer': 'looks.usda',
                                   'path': '/Looks/Metal{v=' + chosen + '}'}, sites)
                else:
                    self.assertFalse(any('{v=' in site['path'] for site in sites))
                self.assert_stock_matches(root, actual)

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
                    self.assertEqual(document['Composing']['/P'].get('Child names', []), expected)


    def test_relocated_child_extensions_follow_stock_path_order(self):
        # The spec asks for path-element sorting of relocated arrivals, while
        # stock 26.11 orders these extension names by their authored token
        # spelling (Item10 before Item2). Keep this compatibility case separate
        # from ordinary per-spec child ordering.
        with tempfile.TemporaryDirectory(prefix='lightusd-relocated-child-order-') as directory:
            base = Path(directory)
            (base / 'asset.usda').write_text(
                '#usda 1.0\ndef Xform "A" {\n'
                '    def Xform "Source" {\n'
                '        def "Item2" {}\n'
                '        def "Item10" {}\n'
                '    }\n}\n')
            root = base / 'root.usda'
            root.write_text(
                '#usda 1.0\n(\n'
                '    relocates = {\n'
                '        </P/Source/Item2>: </P/Item2>,\n'
                '        </P/Source/Item10>: </P/Item10>\n'
                '    }\n)\n'
                'def Xform "P" (references = @asset.usda@</A>) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            expected = ['Source', 'Item10', 'Item2']
            self.assertEqual(document['Composing']['/P']['Child names'], expected)
            if truth is not None:
                self.assertEqual(truth['Composing']['/P']['Child names'], expected)


    def test_relocate_destination_elides_same_stack_inherited_squatter(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-relocate-squatter-') as directory:
            base = Path(directory)
            (base / 'rig.usda').write_text('''#usda 1.0
def Scope "Y" {
    def Scope "A" {}
    def Scope "A2" {}
}
''')
            root = base / 'root.usda'
            root.write_text('''#usda 1.0
(
    relocates = {
        </X/A>: </X/A2>,
        </X/B>: </X/B2>
    }
)
def "X" (
    add references = @rig.usda@</Y>
    add inherits = </_Class_X>
) {
    over "A2" {}
    over "B2" {}
}
class "_Class_X" {
    def Scope "B" {}
    def Scope "B2" {}
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            stack = document['Composing']['/X/B2']['Prim Stack']
            self.assertEqual([site['path'] for site in stack], ['/X/B2', '/_Class_X/B'])
            if truth is not None:
                self.assertEqual(stack, truth['Composing']['/X/B2']['Prim Stack'])


    def test_each_layer_orders_the_accumulated_raw_root_names(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-composed-root-order-') as directory:
            base = Path(directory)
            (base / 'weak.usd').write_text('#usda 1.0\ndef "b" {}\n')
            (base / 'middle.usd').write_text('#usda 1.0\n'
                'reorder rootPrims = ["b", "a"]\n'
                'def "a" {}\ndef "u" {}\ndef "b" {}\ndef "v" {}\n')
            root = base / 'root.usd'
            root.write_text('#usda 1.0\n(subLayers = [@middle.usd@, @weak.usd@])\n'
                'def "extra" {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            self.assertEqual(list(document['Composing']), ['/b', '/a', '/u', '/v', '/extra'])
            if truth is not None:
                self.assertEqual(list(document['Composing']), list(truth['Composing']))


    def test_relocation_names_are_ordered_at_their_contributing_node(self):
        cases = [
            ('rename', ['keep', 'x', 'old', 'y'],
             '</Model/old>: </Model/new>', ['keep', 'x', 'new', 'y', 'Local']),
            ('collision', ['keep', 'new', 'old', 'x'],
             '</Model/old>: </Model/new>', ['keep', 'new', 'x', 'Local']),
            ('missing', ['keep', 'x'],
             '</Model/absent>: </Model/new>', ['keep', 'x', 'Local']),
            ('arrival', ['keep', 'x'],
             '</Model/Sub/In>: </Model/Arrived>', ['keep', 'x', 'Sub', 'Arrived', 'Local']),
        ]
        for name, children, relocate, expected in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory(
                    prefix='lightusd-relocate-order-') as directory:
                base = Path(directory)
                nested = ' def "Sub" {\n def "In" {}\n }\n' if name == 'arrival' else ''
                (base / 'asset.usd').write_text('#usda 1.0\ndef "A" {\n' +
                    ''.join(' def "' + child + '" {}\n' for child in children) + nested + '}\n')
                root = base / 'root.usd'
                root.write_text('#usda 1.0\n(relocates = {' + relocate + '})\n'
                    'def "Model" (references = @asset.usd@</A>) {\n def "Local" {}\n}\n')
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(result.stdout)
                self.assert_stock_matches(root, document)
                self.assertEqual(document['Composing']['/Model']['Child names'], expected)


    def test_relocate_source_strength_and_invalid_entry_filtering(self):
        weak = '</P/A>: </P/Weak>'
        cases = [
            ('strongest', '</P/A>: </P/Strong>', weak, ['Strong']),
            ('empty-stronger', '', weak, ['Weak']),
            ('strong-delete', '</P/A>: <>', weak, []),
            ('invalid-identity', '</P/A>: </P/A>', weak, ['Weak']),
            ('relative-root', '<P/A>: <P/Relative>', weak, ['Relative']),
        ]
        for name, edits, weaker, expected in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory(
                    prefix='lightusd-relocate-strength-') as directory:
                base = Path(directory)
                (base / 'asset.usda').write_text('#usda 1.0\ndef "Model" {\n'
                    ' def "A" {\n int value = 7\n }\n}\n')
                if weaker is not None:
                    (base / 'weak.usda').write_text('#usda 1.0\n(relocates = {' + weaker + '})\n')
                root = base / 'root.usda'
                root.write_text('#usda 1.0\n(\n' +
                    (' subLayers = [@weak.usda@]\n' if weaker is not None else '') +
                    ' relocates = {' + edits + '}\n)\n'
                    'def "P" (references = @asset.usda@</Model>) {\n}\n')
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(result.stdout)
                self.assert_stock_matches(root, document)
                self.assertEqual(document['Composing']['/P'].get('Child names', []), expected)

    def test_ancestral_relocate_arrival_supersedes_elided_source(self):
        with tempfile.TemporaryDirectory(
                prefix='lightusd-ancestral-relocate-arrival-') as directory:
            base = Path(directory)
            (base / 'base.usd').write_text('''#usda 1.0
def Scope "Rigs" {
    def Scope "CharRig" (
        prepend variantSets = ["rigVariant"]
        variants = { string rigVariant = "default" }
    ) {
        variantSet "rigVariant" = {
            "default" {
                def "CollisionRig" {
                    def "Collision" {}
                }
            }
        }
    }
    def Scope "CharBaseRig" (
        prepend variantSets = ["rigVariant"]
        variants = { string rigVariant = "default" }
    ) {
        variantSet "rigVariant" = {
            "default" {
                def "CollisionRig" {
                    def "Collision" {}
                }
            }
        }
    }
}
''')
            root = base / 'root.usd'
            root.write_text('''#usda 1.0
(
    relocates = {
        </CharBase/CharBaseRig/CollisionRig/Collision>: </CharBase/Anim/Collision>,
        </Char/CharRig/CollisionRig/Collision>: </Char/Anim/Collision>
    }
)
def Scope "CharBase" (references = @base.usd@</Rigs>) {
    over "Anim" {
        over "Collision" {}
    }
}
def Scope "Char" (references = </CharBase>) {
    over "Anim" {
        over "Collision" {}
    }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            stack = document['Composing']['/Char/Anim/Collision']['Prim Stack']
            self.assertEqual(stack, [
                {'layer': 'root.usd', 'path': '/Char/Anim/Collision'},
                {'layer': 'base.usd',
                 'path': '/Rigs/CharRig{rigVariant=default}CollisionRig/Collision'},
            ])
            if truth is not None:
                self.assertEqual(stack,
                    truth['Composing']['/Char/Anim/Collision']['Prim Stack'])

    def test_chained_relocates_expand_each_reference_once(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-chained-relocate-') as directory:
            base = Path(directory)
            (base / 'subrig.usd').write_text('''#usda 1.0
def "SubRig" {
    def "Rig" {
        def "Leaf" {
            int value = 7
        }
    }
}
''')
            (base / 'rig.usd').write_text('''#usda 1.0
def "RigAsset" {
    def "Rig" {
        def "SubRig" (references = @subrig.usd@</SubRig>) {}
    }
}
''')
            root = base / 'root.usd'
            root.write_text('''#usda 1.0
(
    relocates = {
        </Char/Rig>: </Char/Rig2>,
        </Char/Rig2/SubRig>: </Char/Rig2/SubRig2>,
        </Char/Rig2/SubRig2/Rig>: </Char/Rig2/SubRig2/Rig2>
    }
)
def "Char" (references = @rig.usd@</RigAsset>) {
    over "Rig2" {
        over "SubRig2" {}
    }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            leaf = document['Composing']['/Char/Rig2/SubRig2/Rig2/Leaf']
            self.assertEqual(leaf['Prim Stack'],
                [{'layer': 'subrig.usd', 'path': '/SubRig/Rig/Leaf'}])
            self.assertEqual(leaf['Property stacks'][
                '/Char/Rig2/SubRig2/Rig2/Leaf.value'],
                [{'layer': 'subrig.usd', 'path': '/SubRig/Rig/Leaf.value'}])

    def test_selected_variant_can_override_relocate_destination(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-variant-arrival-') as directory:
            base = Path(directory)
            (base / 'model.usd').write_text('''#usda 1.0
def "Model" {
    def "Source" {
        def "Geom" {
            string value = "reference"
        }
    }
}
''')
            root = base / 'root.usd'
            root.write_text('''#usda 1.0
(relocates = { </Model/Source>: </Model/Destination> })
def "Model" (
    references = @model.usd@</Model>
    prepend variantSets = "look"
    variants = { string look = "selected" }
) {
    variantSet "look" = {
        "selected" {
            over "Destination" {
                over "Geom" {
                    string value = "variant"
                    rel target = </Model>
                }
            }
        }
    }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            geom = document['Composing']['/Model/Destination/Geom']
            self.assertEqual([s['layer'] for s in geom['Prim Stack']],
                             ['root.usd', 'model.usd'])
            self.assertIn('target', geom['Property names'])

    def test_referenced_relocate_suppresses_root_source_opinions(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-root-source-opinion-') as directory:
            base = Path(directory)
            (base / 'content.usd').write_text('''#usda 1.0
def "Asset" {
    def "Source" {
        int value = 7
    }
}
''')
            (base / 'rig.usd').write_text('''#usda 1.0
(relocates = { </Rig/Source>: </Rig/Destination> })
def "Rig" (references = @content.usd@</Asset>) {}
''')
            root = base / 'root.usd'
            root.write_text('''#usda 1.0
def "Model" (references = @rig.usd@</Rig>) {
    over "Source" {
        int prohibited = 99
    }
    over "Destination" {
        int legal = 1
    }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            destination = document['Composing']['/Model/Destination']
            self.assertNotIn('prohibited', destination['Property names'])
            self.assertIn('legal', destination['Property names'])
            self.assertIn('value', destination['Property names'])
            self.assertNotIn('/Model/Source', document['Composing'])

    def test_above_root_relocate_target_is_not_native_deletion(self):
        # Malformed-input classification, deliberately not a valid parity case.
        # Core 1.1.0 L3370-3375 forbids anchoring above the absolute root.
        with tempfile.TemporaryDirectory(prefix='lightusd-relocate-underflow-') as directory:
            base = Path(directory)
            (base / 'asset.usda').write_text('#usda 1.0\ndef "Model" {\n def "A" {}\n}\n')
            root = base / 'root.usda'
            root.write_text('#usda 1.0\n(relocates = {<P/A>: <../Gone>})\n'
                            'def "P" (references = @asset.usda@</Model>) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['Composing']['/P']['Child names'], ['A'])
            if ARGS.oracle_python:
                env = os.environ.copy()
                if ARGS.oracle_pythonpath:
                    env['PYTHONPATH'] = str(Path(ARGS.oracle_pythonpath).resolve())
                    env.pop('LD_LIBRARY_PATH', None)
                    env.pop('PXR_PLUGINPATH_NAME', None)
                probe = subprocess.run([ARGS.oracle_python, '-c',
                    'import importlib.metadata, sys; '
                    'assert importlib.metadata.version("usd-core") == "26.11"; '
                    'from pxr import Sdf; '
                    'layer = Sdf.Layer.FindOrOpen(sys.argv[1]); '
                    'assert layer is not None; '
                    'assert [(str(a), str(b)) for a,b in layer.relocates] == [("/P/A", "")]',
                    str(root)], capture_output=True, text=True, env=env)
                self.assertEqual(probe.returncode, 0, probe.stderr)

    def test_legacy_prim_relocate_extension_and_layer_priority(self):
        for layer_edits, expected in [(None, ['Legacy']), ('', ['A']),
                                       ('</P/A>: </P/Strong>', ['Strong'])]:
            with self.subTest(edits=layer_edits), tempfile.TemporaryDirectory(
                    prefix='lightusd-relocate-layer-priority-') as directory:
                base = Path(directory)
                (base / 'asset.usda').write_text('#usda 1.0\ndef "Model" {\n def "A" {}\n}\n')
                root = base / 'root.usda'
                root.write_text('#usda 1.0\n' +
                    ('(relocates = {' + layer_edits + '})\n' if layer_edits is not None else '') +
                    'def "P" (\n references = @asset.usda@</Model>\n'
                    ' relocates = { <A>: <Legacy> }\n) {}\n')
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(result.stdout)
                # Stock 26.11 no longer accepts this legacy USDA field.
                # Preserve the native extension and test its layer priority.
                self.assert_stock_rejects(root)
                self.assertEqual(document['Composing']['/P']['Child names'], expected)

    def test_relocate_rejected_path_controls(self):
        cases = [
            ('variant-target', '</P/A>: </P/Bad{v=x}>', None, False),
            ('variant-source', '</P/Missing{v=x}>: </P/New>, </P/A>: </P/New>', None, False),
            ('pseudo-source', '</>: </P/New>, </P/A>: </P/New>', ['New'], False),
            ('property-source', '</P/Missing.attr>: </P/New>, </P/A>: </P/New>', ['New'], False),
            ('property-target', '</P/A>: </P/Bad.attr>', ['Weak'], True),
        ]
        for name, edits, expected, weaker in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory(
                    prefix='lightusd-relocate-reject-') as directory:
                base = Path(directory)
                (base / 'asset.usda').write_text('#usda 1.0\ndef "Model" {\n def "A" {}\n}\n')
                if weaker:
                    (base / 'weak.usda').write_text('#usda 1.0\n(relocates = {</P/A>: </P/Weak>})\n')
                root = base / 'root.usda'
                root.write_text('#usda 1.0\n(\n' +
                    (' subLayers = [@weak.usda@]\n' if weaker else '') +
                    ' relocates = {' + edits + '}\n)\n'
                    'def "P" (references = @asset.usda@</Model>) {}\n')
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
                if expected is not None:
                    # Stock rejects these paths at layer read. The permissive
                    # native reader retains them; composition must ignore them.
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout)['Composing']['/P']['Child names'], expected)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('variant selections', result.stderr)
                self.assert_stock_rejects(root)

    def test_relocation_to_root_precedes_local_root_names(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-root-relocate-order-') as directory:
            base = Path(directory)
            (base / 'asset.usd').write_text('#usda 1.0\ndef "A" {\n def "Child" {}\n}\n')
            root = base / 'root.usd'
            root.write_text('#usda 1.0\n(relocates = {</P/Child>: </Moved>})\n'
                'def "P" (references = @asset.usd@</A>) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            self.assertEqual(list(document['Composing']), ['/Moved', '/P'])
            if truth is not None:
                self.assertEqual(list(document['Composing']), list(truth['Composing']))

            # Source order differs from target order: target names sort first.
            (base / 'asset.usd').write_text('#usda 1.0\ndef "A" {\n'
                ' def "C" {}\n def "D" {}\n}\n')
            root.write_text('#usda 1.0\n(relocates = {</P/C>: </Z>, </P/D>: </A>})\n'
                'def "P" (references = @asset.usd@</A>) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            truth = self.assert_stock_matches(root, document)
            self.assertEqual(list(document['Composing']), ['/A', '/Z', '/P'])
            if truth is not None:
                self.assertEqual(list(document['Composing']), list(truth['Composing']))


    def test_separate_reference_branches_can_rename_the_same_child(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-branch-relocate-order-') as directory:
            base = Path(directory)
            (base / 'base.usd').write_text('#usda 1.0\ndef "Base" {\n def "Child" {}\n}\n')
            for branch in (1, 2):
                suffix = str(branch)
                (base / ('ref' + suffix + '.usd')).write_text('#usda 1.0\n'
                    '(relocates = {</R/Child>: </R/Child_' + suffix + '>})\n'
                    'def "R" (references = @base.usd@</Base>) {\n'
                    ' def "Local_' + suffix + '" {}\n}\n')
            root = base / 'root.usd'
            root.write_text('#usda 1.0\ndef "P" (references = [\n'
                ' @ref1.usd@</R>, @ref2.usd@</R>\n]) {}\n')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            self.assertEqual(document['Composing']['/P']['Child names'],
                             ['Child_2', 'Local_2', 'Child_1', 'Local_1'])


    def test_nested_variant_arc_origins_survive_descendant_expansion(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-variant-origin-') as directory:
            base = Path(directory)
            (base / 'ref.usda').write_text('''#usda 1.0
def "Outer" {
 def "B" {
  def "C" {}
 }
}
def "DirectB" {
 def "C" {}
}
def "VariantB" {
 def "C" {}
}
def "DirectC" {}
def "OuterC" {}
def "VariantC" {}
''')
            root = base / 'root.usda'
            root.write_text('''#usda 1.0
def "A" (variantSets = "outer"
 variants = {string outer = "x"}) {
  variantSet "outer" = {
    "x" (references = @ref.usda@</Outer>) {
      over "B" (variantSets = "middle"
 variants = {string middle = "y"}) {
        variantSet "middle" = {
          "y" (references = @ref.usda@</VariantB>) {
            over "C" (variantSets = "inner"
 variants = {string inner = "z"}) {
              variantSet "inner" = {
                "z" (references = @ref.usda@</VariantC>) {}
              }
            }
          }
        }
        def "C" (references = @ref.usda@</OuterC>) {}
      }
    }
  }
  def "B" (references = @ref.usda@</DirectB>) {
    def "C" (references = @ref.usda@</DirectC>) {}
  }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            for path, expected in [('/A/B', ['/VariantB', '/Outer/B', '/DirectB']),
                    ('/A/B/C', ['/VariantC', '/VariantB/C', '/OuterC',
                               '/Outer/B/C', '/DirectC', '/DirectB/C'])]:
                self.assertEqual([site['path'] for site in
                    document['Composing'][path]['Prim Stack']
                    if site['layer'] == 'ref.usda'], expected)


    def test_raw_property_order_interleaves_relationships_and_attributes(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-property-order-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('''#usda 1.0
def "P" {
 int z = 1
 rel r = </Target>
 int a = 2
 rel s
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            self.assertEqual(document['Composing']['/P']['Property names'], ['z', 'r', 'a', 's'])

    def test_variant_property_order_interleaves_all_storage_forms(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-variant-property-order-') as directory:
            root = Path(directory) / 'root.usda'
            root.write_text('''#usda 1.0
def "P" (variantSets = "v"
 variants = {string v = "x"}) {
 variantSet "v" = {
  "x" {
   int z = 1
   custom rel r = </Target>
   int a = 2 (doc = "metadata routes this into content")
   rel s
   int t.timeSamples = {0: 3, 1: 5}
  }
 }
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            self.assertEqual(document['Composing']['/P']['Property names'],
                             ['z', 'r', 'a', 's', 't'])
            binary = root.with_suffix('.usdc')
            written = subprocess.run([ARGS.lusdcat, '--rewrite-layer', '-o',
                str(binary), str(root)], capture_output=True, text=True)
            self.assertEqual(written.returncode, 0, written.stderr)
            reread = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(binary)], capture_output=True, text=True)
            self.assertEqual(reread.returncode, 0, reread.stderr)
            binary_document = json.loads(reread.stdout)
            self.assert_stock_matches(binary, binary_document)
            self.assertEqual(binary_document['Composing']['/P']['Property names'],
                             ['z', 'r', 'a', 's', 't'])

    def test_arcs_cannot_reach_deleted_children_of_relocated_prims(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-relocated-deletion-') as directory:
            base = Path(directory)
            (base / 'base.usda').write_text('''#usda 1.0
def "R" {
 def "Before" {
  def "Gone" {
   int poison = 1
   def "Kid" {}
  }
  def "Alive" {
   int good = 2
  }
 }
}
''')
            root = base / 'root.usda'
            root.write_text('''#usda 1.0
(relocates = {</P/Before>: </P/After>, </P/After/Gone>: <>})
def "P" (references = @base.usda@</R>) {
 over "After" {
  over "Gone" {}
 }
}
def "Bad" (references = </P/After/Gone>) {}
def "BadChild" (references = </P/After/Gone/Kid>) {}
def "BadInherit" (inherits = </P/After/Gone>) {}
def "BadSpecialize" (specializes = </P/After/Gone>) {}
def "Good" (references = </P/After/Alive>) {}
''')
            external = base / 'external.usda'
            external.write_text('''#usda 1.0
def "Bad" (references = @root.usda@</P/After/Gone>) {}
def "BadChild" (references = @root.usda@</P/After/Gone/Kid>) {}
def "BadPayload" (payload = @root.usda@</P/After/Gone>) {}
def "Good" (references = @root.usda@</P/After/Alive>) {}
''')
            for entry in (root, external):
                result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                    '--pcp-json-version', '2', str(entry)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(result.stdout)
                self.assert_stock_matches(entry, document)
                rejected = ['/Bad', '/BadChild']
                rejected += ['/BadInherit', '/BadSpecialize'] if entry == root else ['/BadPayload']
                for path in rejected:
                    self.assertEqual(document['Composing'][path]['Prim Stack'],
                                     [{'layer': entry.name, 'path': path}])
                    self.assertNotIn('Property names', document['Composing'][path])
                self.assertEqual(document['Composing']['/Good']['Property names'], ['good'])

    def test_cross_file_arc_cycle_returning_to_an_ancestor(self):
        with tempfile.TemporaryDirectory(prefix='lightusd-ancestor-cycle-') as directory:
            base = Path(directory)
            root = base / 'root.usda'
            root.write_text('''#usda 1.0
def "Parent" {
 int outer = 1
 over "Child" (references = @model.usda@</Model>) {}
}
def "Safe" (references = @model.usda@</Other>) {}
def "Direct" {
 over "Child" (references = </Direct>) {}
}
''')
            (base / 'model.usda').write_text('''#usda 1.0
def "Model" (references = @root.usda@</Parent>) {
 int inner = 2
}
def "Other" {
 def "Kid" {}
}
''')
            result = subprocess.run([ARGS.lusdcat, '--dump-pcp-json', '-',
                '--pcp-json-version', '2', str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(result.stdout)
            self.assert_stock_matches(root, document)
            child = document['Composing']['/Parent/Child']
            self.assertEqual(child['Prim Stack'], [
                {'layer': 'root.usda', 'path': '/Parent/Child'},
                {'layer': 'model.usda', 'path': '/Model'}])
            self.assertEqual(child['Property names'], ['inner'])
            self.assertNotIn('Child names', child)
            self.assertNotIn('/Parent/Child/Child', document['Composing'])
            self.assertNotIn('/Direct/Child/Child', document['Composing'])
            self.assertIn('/Safe/Kid', document['Composing'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lusdcat', required=True)
    parser.add_argument('--oracle-python')
    parser.add_argument('--oracle-pythonpath')
    ARGS = parser.parse_args()
    ARGS.lusdcat = str(Path(ARGS.lusdcat).resolve())
    unittest.main(argv=[__file__])
