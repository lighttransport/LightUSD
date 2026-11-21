#!/usr/bin/env python3
"""Variant parity covers authored/unselected data as well as composed results."""
import pathlib
import subprocess
import sys
import tempfile

diff, cat, root = sys.argv[1:]
root = pathlib.Path(root)
def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f'{args}\n{result.stdout}\n{result.stderr}')
    return result.stdout

for ascii_file in sorted((root / 'tests/usda').glob('variantSet-*.usda')):
    crate_file = root / 'tests/usdc' / (ascii_file.stem + '.usdc')
    if crate_file.exists():
        if ascii_file.stem == 'variantSet-prop-001':
            # The checked-in Crate genuinely lacks the USDA selection (also
            # confirmed by OpenUSD usdcat). Preserve that real differential.
            result = subprocess.run([diff, str(ascii_file), str(crate_file)], capture_output=True, text=True)
            assert result.returncode == 1 and 'variants' in result.stdout
            continue
        run(diff, str(ascii_file), str(crate_file))
        run(diff, '--flatten', str(ascii_file), str(crate_file))

source = '''#usda 1.0
def Scope "P" (
 prepend variantSets = "look"
 variants = { string look = "red" }
) {
 variantSet "look" = {
  "red" (active = true hidden = false kind = "component"
         customPipelineTag = "hero"
         variants = { string detail = "high" }) {
   float gain = 1 (doc = "retained gain metadata")
   float animated.timeSamples = { 0: 1, 1: 2 }
   rel material:binding = </RedMaterial>
   def Scope "Child" { int x = 1; }
   variantSet "detail" = {
    "high" { int quality = 2 }
    "low" { int quality = 1 }
   }
  }
  "blue" { def Scope "Child" { int x = 2; } }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='lightusd-variants-') as directory:
    directory = pathlib.Path(directory)
    ascii_file = directory / 'source.usda'
    crate_file = directory / 'source.usdc'
    rewritten = directory / 'rewritten.usda'
    ascii_file.write_text(source)
    run(cat, '--rewrite-layer', str(ascii_file), '-o', str(crate_file))
    run(diff, str(ascii_file), str(crate_file))
    run(diff, '--flatten', str(ascii_file), str(crate_file))
    run(cat, '--rewrite-layer', str(crate_file), '-o', str(rewritten))
    run(diff, str(ascii_file), str(rewritten))
    # Both layers contain exactly the same prim paths. A body-value edit in
    # the unselected option must nevertheless produce a differential.
    changed = directory / 'changed.usda'
    changed.write_text(source.replace('int x = 2', 'int x = 3'))
    result = subprocess.run([diff, str(ascii_file), str(changed)], capture_output=True, text=True)
    assert result.returncode == 1, result.stdout + result.stderr
    assert 'content' in result.stdout, result.stdout
