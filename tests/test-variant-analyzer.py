#!/usr/bin/env python3
"""Core authored variant analysis: nested variants, cap, JSON and Crate parity."""
import json
import pathlib
import subprocess
import sys
import tempfile

app, cat = map(str, sys.argv[1:3])
def run(*args, ok=True):
    result = subprocess.run(args, text=True, capture_output=True)
    assert (result.returncode == 0) == ok, result.stderr
    return result.stdout
with tempfile.TemporaryDirectory() as directory:
    source = pathlib.Path(directory) / 'quoted"file.usda'
    source.write_text('''#usda 1.0
 def Xform "World" {
  variantSet "outer" = {
   "a" {
    variantSet "inner" = {
     "x" { variantSet "deep" = { "last" {} } }
    }
   }
   "b" {}
  }
 }
''')
    report = json.loads(run(app, '--json', str(source)))
    assert report['file'] == str(source)
    assert report['summary'] == dict(prims_with_variants=1, total_variant_sets=1, total_variant_options=2), report
    assert report['statistics']['max_nesting_depth'] == 3, report
    crate = pathlib.Path(directory) / 'scene.usdc'
    run(cat, '--rewrite-layer', str(source), '-o', str(crate))
    other = json.loads(run(app, '--json', str(crate)))
    assert other['summary'] == report['summary'], other
    assert other['statistics'] == report['statistics'], other
    source.write_text('#usda 1.0\ndef Scope "Empty" {}\n')
    report = json.loads(run(app, '--json', str(source)))
    assert report['summary']['total_variant_sets'] == 0
    source.write_text('#usda 1.0\n' + '\n'.join(
        'def Xform "P%d" {\n variantSet "shape" = {\n "a" {}\n "b" {}\n }\n }' % i for i in range(40)))
    report = json.loads(run(app, '--json', str(source)))
    assert report['statistics']['total_combinations'] is None, report
    run(app, '--bogus', str(source), ok=False)
    run(app, str(source), str(source), ok=False)
print('PASS: core variant analyzer nesting, capped combinations, JSON and Crate parity')
