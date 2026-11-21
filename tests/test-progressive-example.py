#!/usr/bin/env python3
"""Verify cumulative and selective core composition with real external layers."""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import zipfile
app, cat = sys.argv[1:]

def run(*args, good=True):
    result = subprocess.run(args, capture_output=True, text=True)
    assert (result.returncode == 0) == good, result.stdout + result.stderr
    return result.stdout

def body(text, name):
    match = re.search(r'(?:def|over)\s+\w+\s+"'+name+r'"(?:\s*\([^)]*\))?\s*\{([^}]*)', text)
    assert match, name + '\n' + text
    return match[1]

with tempfile.TemporaryDirectory(prefix='lightusd-progressive-') as directory:
    root = pathlib.Path(directory)
    (root/'sub.usda').write_text('#usda 1.0\ndef Scope "Sub" {}\n')
    (root/'asset.usda').write_text('#usda 1.0\ndef Xform "Asset" { float external=4; def Mesh "ExternalMesh" {}; }\n')
    source = root/'root.usda'
    source.write_text('''#usda 1.0
(subLayers=[@sub.usda@])
class Scope "Class" { float inherited=2; }
class Scope "Special" { float specialized=3; }
def Scope "Derived" (prepend inherits=</Class>) {}
def Scope "SpecialDerived" (prepend specializes=</Special>) {}
def Xform "Referenced" (prepend references=@asset.usda@</Asset>) {}
def Xform "Payload" (prepend payload=@asset.usda@</Asset>) {}
def Xform "Variant" (variants={string shape="one"}; prepend variantSets="shape") {
 variantSet "shape" = { "one" { def Mesh "VariantMesh" {}; } }
}
''')
    crate = root/'root.usdc'
    run(cat, '--rewrite-layer', str(source), '-o', str(crate))
    package = root/'root.usdz'
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_STORED) as archive:
        for path in [source, root/'sub.usda', root/'asset.usda']:
            archive.write(path, 'nested/'+path.name)
    for path in [source, crate, package]:
        output = run(app, '--extract-variants', '--relative', str(path))
        report = json.loads(output.split('# authored variants\n', 1)[1].split('\n', 1)[0])
        assert report == [{'prim':'/Variant', 'set':'shape', 'variants':['one']}], report
        final = output.split('# final\n', 1)[1].split('Prim :', 1)[0]
        assert all('# composed '+name in output for name in ['subLayers','inherits','variantSets','references','payload','specializes'])
        assert 'inherited = 2' in body(final, 'Derived'), final
        assert 'specialized = 3' in body(final, 'SpecialDerived'), final
        assert 'external = 4' in body(final, 'Referenced') and 'external = 4' in body(final, 'Payload'), final
        assert '"VariantMesh"' in final and '"Sub"' in final
        assert 'Prim : Referenced/ExternalMesh' in output, output
        assert 'Prim : /' not in output
        flattened = root/'flattened.usda'
        flattened.write_text(final)
        run(cat, str(flattened))
    for selection in ['l', 'i', 'v', 'r', 'p', 's', 'references,payload']:
        output = run(app, '--composition='+selection, str(source))
        final = output.split('# final\n', 1)[1].split('Prim :', 1)[0]
        assert ('inherited = 2' in body(final, 'Derived')) == (selection == 'i')
        assert ('specialized = 3' in body(final, 'SpecialDerived')) == (selection == 's')
        assert ('external = 4' in body(final, 'Referenced')) == (selection in ['r','references,payload'])
        assert ('external = 4' in body(final, 'Payload')) == (selection in ['p','references,payload'])
        assert ('Prim : /Variant/VariantMesh' in output) == (selection == 'v')
        assert ('def Scope "Sub"' in final) == (selection == 'l')
        if selection != 'r' and selection != 'references,payload':
            assert 'prepend references' in final, final
        if selection != 'l': assert 'subLayers' in final, final
        partial = root/'partial.usda'
        partial.write_text(final)
        run(cat, str(partial))
    assert run(app, '--loadOnly', str(source)) == ''
    authored = run(app, '--noflatten', str(source))
    assert 'prepend references' in authored and 'variantSet' in authored
    run(app, good=False)
    for args in [['--composition='], ['--composition=l,'], ['--composition=bad'], ['--unknown']]:
        run(app, *args, str(source), good=False)
    missing = root/'missing.usda'
    missing.write_text('#usda 1.0\ndef Xform "Missing" (references=@absent.usda@) {}\n')
    run(app, str(missing), good=False)
print('PASS: progressive core composition, selective arcs, variants, USDA/USDC/USDZ and CLI errors')
