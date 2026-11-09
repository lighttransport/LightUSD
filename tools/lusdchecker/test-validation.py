#!/usr/bin/env python3
"""CLI contract regressions: require exact exit codes and structured findings."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

HEADER = '#usda 1.0\n(\n defaultPrim = "World"\n upAxis = "Y"\n metersPerUnit = 1\n)\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('checker', type=Path)
    parser.add_argument('--custom-checker', type=Path)
    a = parser.parse_args()
    checker = str(a.checker.resolve())
    help_text = subprocess.run([checker, '--help'], capture_output=True, text=True, check=True).stdout
    assert "against AOUSD Core\n1.1.0" in help_text
    count = 0
    with tempfile.TemporaryDirectory(prefix='checker-validation-') as directory:
        root = Path(directory)

        def write(name, text):
            p = root / name
            p.write_text(text)
            return p

        def run(path, *flags, code=0, rule=None):
            nonlocal count
            command = [checker, '--json', *flags, str(path)]
            p = subprocess.run(command, capture_output=True, text=True, timeout=30)
            assert p.returncode == code, (command, p.returncode, p.stdout, p.stderr)
            report = json.loads(p.stdout)
            assert isinstance(report['complete'], bool)
            if rule:
                assert any(i['ruleId'] == rule for i in report['issues']), (rule, report)
            count += 1
            return report

        clean = write('clean.usda', HEADER + 'def Xform "World" {}\n')
        run(clean, '--profile', 'strict')
        repo = Path(__file__).resolve().parents[2]
        run(clean, '--schema-definitions', str(repo/'doc/generated/openusd-schema-26.08.json'),
            '--shader-definitions', str(repo/'tools/lusdchecker/definitions/shaders.json'))
        run(clean, '--profile', 'aousd-core-1.1.0')
        run(clean, '--profile', 'aousd-core-1.0.1')  # compatible alias
        order = write('order.usda', HEADER + '''reorder rootPrims = ["Missing", "bad/name"]
def Xform "World" {
 reorder nameChildren = ["Missing", "ns:Child"]
 reorder properties = ["primvars:color", "bad.name"]
}
''')
        ordered = run(order, '--core-only', rule='core.layer.primOrder')
        for rule in ['core.prim.primOrder', 'core.prim.propertyOrder']:
            assert any(i['ruleId'] == rule for i in ordered['issues']), ordered
        run(order, '--core-only', '--strict', code=1, rule='core.prim.propertyOrder')
        relocates = write('relocates.usda', HEADER.replace(' upAxis = "Y"',
            ' relocates = { </P/A>: </P/A>, </P/B>: </> }\n upAxis = "Y"') +
            'def Xform "World" {}\n')
        relocated = run(relocates, '--core-only', code=1, rule='core.layer.layerRelocates.identity')
        assert any(i['ruleId'] == 'core.layer.layerRelocates.target' for i in relocated['issues']), relocated
        assert all(i['location'] == '<layer>' for i in relocated['issues']
                   if i['ruleId'].startswith('core.layer.layerRelocates.')), relocated
        relocate_rules = {
            'root': '</World>: </Moved>',
            'ancestor': '</World/A/B>: </World/A>',
            'targetUnderSource': '</World/A>: </World/A/B>',
            'duplicateSource': '</World/A>: </World/X>, </World/A>: </World/Y>',
            'duplicateTarget': '</World/A>: </World/X>, </World/B>: </World/X>',
            'nestedSource': '</World/A>: </World/X>, </World/A/B>: </World/Y>',
            'targetIsSource': '</World/A>: </World/B>, </World/B>: </World/C>',
            'targetUnderOtherSource': '</World/A>: </World/X>, </World/B>: </World/A/C>',
        }
        for component, entries in relocate_rules.items():
            source = write('relocate-' + component + '.usda', HEADER.replace(' upAxis = "Y"',
                ' relocates = {' + entries + '}\n upAxis = "Y"') + 'def Xform "World" {}\n')
            rule = 'core.layer.layerRelocates.' + component
            report = run(source, '--core-only', code=1, rule=rule)
            issue = next(i for i in report['issues'] if i['ruleId'] == rule)
            assert issue['location'] == '<layer>' and issue['leanRef'].startswith('UsdSpec.Relocates.'), issue
            if component in ['targetIsSource', 'targetUnderOtherSource']:
                assert issue['category'] == 'compatibility', issue
                run(source, '--profile', 'aousd-core-1.1.0')
            else:
                assert issue['category'] == 'normative', issue
                run(source, '--profile', 'aousd-core-1.1.0', code=1, rule=rule)
        future = write('future.usda', clean.read_text().replace('#usda 1.0', '#usda 1.2'))
        run(future, '--profile', 'aousd-core-1.1.0', code=1, rule='checker.coverage.version')
        run(clean, '--profile', 'strict', '--core-only', code=2, rule='checker.usage')
        run(clean, '--core-only', '--profile', 'strict', code=2, rule='checker.usage')
        run(clean, '--profile', 'strict', '--skip-variants', code=2)
        run(clean, '--profile', 'not-a-profile', code=2)
        run(clean, '--max-samples', '0', code=2)
        run(root/'missing.usda', code=2, rule='checker.io')
        malformed = write('malformed.usda', '#usda 1.0\ndef {')
        run(malformed, code=2, rule='parser.error')
        sarif = subprocess.run([checker, '--sarif', str(malformed)], capture_output=True, text=True)
        assert sarif.returncode == 2
        assert not json.loads(sarif.stdout)['runs'][0]['invocations'][0]['executionSuccessful']
        fragment = write('fragment.usda', '#usda 1.0\nover "World" {}\n')
        run(fragment, '--profile', 'aousd-core-1.1.0')
        run(fragment, '--strict', code=1)
        sphere = write('sphere.usda', HEADER + 'def Xform "World" {\n def Sphere "S" {\n float radius = 1\n }\n}\n')
        run(sphere, '--usdchecker-compat', code=1, rule='core.schema.attributeType')
        run(sphere, '--profile', 'strict', code=1, rule='core.schema.attributeType')
        run(sphere, '--profile', 'aousd-core-1.1.0')
        unknown = write('unknown.usda', HEADER + 'def VendorThing "World" {}\n')
        r = run(unknown, '--profile', 'strict', code=1, rule='checker.coverage.schema')
        assert not r['complete'] and r['conformance'] == 'incomplete'
        baseline = write('baseline.json', json.dumps(r))
        run(unknown, '--profile', 'strict', '--baseline', str(baseline), code=1)
        run(unknown, '--profile', 'aousd-core-1.1.0')
        schemas = write('schemas.json', json.dumps({'formatVersion': 1, 'schemas': [{
            'name': 'VendorThing', 'kind': 'concrete', 'inherits': 'Xform', 'properties': [
                {'name': 'weight', 'type': 'double', 'kind': 'attribute', 'variability': 'varying'}]}]}))
        run(unknown, '--profile', 'strict', '--schema-definitions', str(schemas))
        typed = write('custom.usda', HEADER + 'def VendorThing "World" {\n float weight = 1\n}\n')
        run(typed, '--profile', 'strict', '--schema-definitions', str(schemas), code=1, rule='core.schema.attributeType')
        cyclic = write('cycle.json', json.dumps({'formatVersion': 1, 'schemas': [
            {'name': n, 'kind': 'concrete', 'inherits': parent, 'properties': []}
            for n, parent in [('CycleA', 'CycleB'), ('CycleB', 'CycleA')]]}))
        run(clean, '--schema-definitions', str(cyclic), code=2, rule='checker.definitions')
        invalid = write('invalid.json', '{')
        run(clean, '--shader-definitions', str(invalid), code=2)
        shader = write('shader.usda', HEADER + '''def Xform "World" {
 def Material "M" {
  def Shader "S" {
   uniform token info:id = "ND_test_vendor"
   float inputs:value = 1
  }
 }
}
''')
        run(shader, '--profile', 'strict', code=1, rule='checker.coverage.shader')
        shaders = write('shaders.json', json.dumps({'formatVersion': 1, 'shaders': [{
            'identifier': 'ND_test_vendor', 'sourceType': 'mtlx',
            'inputs': {'value': 'float'}, 'outputs': {'out': 'float'}}]}))
        run(shader, '--profile', 'strict', '--shader-definitions', str(shaders))
        source_shader = write('source.usda', HEADER + '''def Xform "World" {
 def Material "M" {
  def Shader "S" {
   uniform token info:implementationSource = "sourceAsset"
  }
 }
}
''')
        run(source_shader, '--usdchecker-compat', code=1, rule='shade.shader.sourceType')
        geometry = '''def Xform "World" {
 def Mesh "M" {
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  uniform token subsetFamily:regions:familyType = "nonOverlapping"
  def GeomSubset "A" {
   uniform token elementType = "face"
   uniform token familyName = "regions"
   int[] indices = [0]
  }
  def GeomSubset "B" {
   uniform token elementType = "face"
   uniform token familyName = "regions"
   int[] indices = [0]
  }
 }
}
'''
        overlap = write('overlap.usda', HEADER + geometry)
        run(overlap, '--usdchecker-compat', code=1, rule='geom.subset.familyOverlap')
        valid_subset = write('valid-subset.usda', HEADER + geometry.replace('"nonOverlapping"', '"unrestricted"'))
        run(valid_subset, '--profile', 'strict')
        animation = write('animated.usda', HEADER + '''def Xform "World" {
 def Mesh "M" {
  point3f[] points.timeSamples = {0: [(0,0,0),(1,0,0),(0,1,0)], 1: [(0,0,0),(1,0,0),(0,1,0)]}
  int[] faceVertexCounts.timeSamples = {0: [3], 1: [3]}
  int[] faceVertexIndices.timeSamples = {0: [0,1,2], 1: [0,1,99]}
 }
}
''')
        r = run(animation, '--profile', 'strict', code=1, rule='geom.mesh.topology.index')
        assert any(i['ruleId'] == 'geom.mesh.topology.index' and i.get('time') == 1 for i in r['issues'])
        run(animation, '--profile', 'strict', '--max-samples', '1', code=1, rule='checker.coverage.samples')
        run(animation, '--all-time-samples', code=1, rule='geom.mesh.topology.index')
        variant_asset = write('variant-asset.usda', HEADER + '''def Xform "World" (
 prepend variantSets = "shape"
 variants = { string shape = "good" }
) {
 variantSet "shape" = {
  "good" { def Sphere "S" { double radius = 1 } }
  "bad" { def Sphere "S" { float radius = 1 } }
 }
}
''')
        reference = write('reference.usda', HEADER + 'def Xform "World" (\n references = @variant-asset.usda@</World>\n) {}\n')
        r = run(reference, '--composed', '--usdchecker-compat', code=1, rule='core.schema.attributeType')
        assert any('shape}=bad' in i['variants'] for i in r['issues'] if i['ruleId'] == 'core.schema.attributeType')
        # Scope the test to composed geometry, because source declaration audits
        # deliberately inspect all authored variant declarations.
        run(reference, '--composed', '--skip-variants', '--groups', 'geom')
        # Reachable nested selections and duplicate set names stay prim-scoped.
        nested = write('nested.usda', HEADER + '''def Xform "World" (
 prepend variantSets = "outer"
 variants = { string outer = "good" }
) {
 variantSet "outer" = {
  "good" {
   def Xform "Child" {}
  }
  "nested" {
   def Xform "Child" (
    prepend variantSets = "inner"
    variants = { string inner = "good" }
   ) {
    variantSet "inner" = {
     "good" {
      def Sphere "S" {
       double radius = 1
      }
     }
     "bad" {
      def Sphere "S" {
       float radius = 1
      }
     }
    }
   }
  }
 }
}
''')
        r = run(nested, '--profile', 'strict', code=1, rule='core.schema.attributeType')
        assert any('inner}=bad' in i['variants'] for i in r['issues'] if i['ruleId'] == 'core.schema.attributeType')
        edges = write('edges.usda', HEADER + geometry.replace('"face"', '"edge"').replace('[0]','[0,1]'))
        run(edges, '--usdchecker-compat', code=1, rule='geom.subset.familyOverlap')
        valid_edges = write('valid-edges.usda', HEADER + geometry.replace('"face"', '"edge"').replace('[0]','[0,1]').replace('"nonOverlapping"','"unrestricted"'))
        run(valid_edges, '--profile', 'strict')
        good_api = write('applied-api.usda', HEADER + 'def Xform "World" (prepend apiSchemas = ["MaterialBindingAPI"]) {}')
        run(good_api, '--profile', 'strict')
        bad_api = write('typed-as-api.usda', HEADER + 'def Xform "World" (prepend apiSchemas = ["Sphere"]) {}')
        run(bad_api, '--profile', 'strict', code=1, rule='core.apiSchema.kind')
        run(bad_api, '--profile', 'aousd-core-1.1.0')
        wrong_kind = write('wrong-kind.usda', HEADER + 'def Sphere "World" {\n rel radius = </World>\n}\n')
        run(wrong_kind, '--profile', 'strict', code=1, rule='core.schema.propertyKind')
        # A baseline at time 1 does not accept the same error at time 2.
        first = run(animation, '--profile', 'strict', code=1)
        sample_baseline = write('sample-baseline.json', json.dumps(first))
        shifted = write('shifted.usda', animation.read_text().replace('1: [0,1,99]', '2: [0,1,99]'))
        # Keep source identity the same and move the failing sample.
        animation.write_text(shifted.read_text())
        second = run(animation, '--profile', 'strict', '--baseline', str(sample_baseline), code=1)
        assert any(i.get('time') == 2 and i['baselineState'] == 'new' for i in second['issues'])
        uri = write('resolver.usda', HEADER + 'def Xform "World" {\n asset remote = @vendor://asset@\n}\n')
        run(uri, '--profile', 'strict', code=1, rule='checker.coverage.resolver')
        if a.custom_checker:
            old_checker = checker
            checker = str(a.custom_checker.resolve())
            custom = write('callback.usda', HEADER + 'def Sphere "World" {}')
            run(custom, '--includeKeywords', 'ExamplePipeline', code=1, rule='example.pipeline.tag')
            run(custom, '--includeKeywords', 'UsdGeomValidators')
            run(custom, '--includeKeywords', 'NoSuchKeyword', code=2, rule='checker.usage')
            dumped = subprocess.run([checker, '--dumpRules'], capture_output=True, text=True)
            assert dumped.returncode == 0 and 'example.pipeline.tag' in dumped.stdout
            checker = old_checker
        outfile = root/'report.json'
        p = subprocess.run([checker, '--json', '--out', str(outfile), str(malformed)], capture_output=True)
        assert p.returncode == 2 and not p.stdout
        assert not json.loads(outfile.read_text())['executionSuccessful']
    print(f'checker validation contracts: {count} checks passed')


if __name__ == '__main__': main()
