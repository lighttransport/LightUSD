#!/usr/bin/env python3
"""Exercise format-specific core reader examples and their composition controls."""
import pathlib
import re
import subprocess
import sys
import tempfile
app, cat, kind = sys.argv[1:]

def run(*args, good=True):
    result = subprocess.run(args, capture_output=True, text=True)
    assert (result.returncode == 0) == good, result.stderr + result.stdout
    return result.stdout

def body(text, name):
    match = re.search(r'(?:def|over)\s+\w+\s+"'+name+r'"(?:\s*\([^)]*\))?\s*\{([^}]*)', text)
    assert match, name + '\n' + text
    return match[1]

with tempfile.TemporaryDirectory(prefix='lightusd-parser-example-') as directory:
    root = pathlib.Path(directory)
    (root/'sub.usda').write_text('#usda 1.0\ndef Scope "Sub" {}\n')
    (root/'asset.usda').write_text('#usda 1.0\ndef Xform "Asset" { float external=4; }\n')
    source = root/'source.usda'
    source.write_text('''#usda 1.0
(subLayers=[@sub.usda@])
class Scope "Class" { float inherited=2; }
class Scope "Special" { float specialized=3; }
def Scope "Derived" (prepend inherits=</Class>) {}
def Scope "SpecialDerived" (prepend specializes=</Special>) {}
def Xform "Referenced" (prepend references=@asset.usda@</Asset>) {}
def Xform "Payload" (prepend payload=@asset.usda@</Asset>) {}
def Xform "Variant" (variants={string shape="one"}; prepend variantSets="shape") {
 variantSet "shape" = { "one" { float variantValue=5; } }
}
''')
    binary = root/'source.usdc'
    run(cat, '--rewrite-layer', str(source), '-o', str(binary))
    input_path = binary if kind == 'usdc' else source
    authored = run(app, str(input_path))
    assert authored.startswith('#usda 1.0') and 'prepend references' in authored
    for selection in [None, 'l', 'i', 'v', 'r', 'p', 's', 'references,payload']:
        args = ['--flatten'] + ([] if selection is None else ['--composition='+selection])
        result = run(app, *args, str(input_path))
        assert ('inherited' in body(result, 'Derived')) == (selection in [None,'i'])
        assert ('specialized' in body(result, 'SpecialDerived')) == (selection in [None,'s'])
        assert ('external' in body(result, 'Referenced')) == (selection in [None,'r','references,payload'])
        assert ('external' in body(result, 'Payload')) == (selection in [None,'p','references,payload'])
        assert ('variantSet "shape"' not in result) == (selection in [None,'v'])
        assert ('def Scope "Sub"' in result) == (selection in [None,'l'])
        if selection not in [None,'r','references,payload']: assert 'prepend references' in result
        output = root/'output.usda'
        output.write_text(result)
        run(cat, str(output))
    run(app, str(source if kind == 'usdc' else binary), good=False)
    run(app, str(root/'absent'), good=False)
    run(app, good=False)
    for options in [['--unknown'], ['--composition=r'], ['--flatten','--composition='],
                    ['--flatten','--composition=l,'], ['--flatten','--composition=oops']]:
        run(app, *options, str(input_path), good=False)
    assert 'Usage:' in run(app, '--help')
    missing = root/'missing.usda'
    missing.write_text('#usda 1.0\ndef Scope "S" (references=@absent.usda@) {}\n')
    if kind == 'usdc':
        converted = root/'missing.usdc'
        run(cat, '--rewrite-layer', str(missing), '-o', str(converted))
        missing = converted
    run(app, '--flatten', str(missing), good=False)
    assert 'references' in run(app, '--flatten','--composition=l',str(missing))
    malformed = root/('malformed.'+kind)
    malformed.write_bytes(b'PXR-USDC' if kind == 'usdc' else b'#usda 1.0\ndef Xform "Broken" {')
    run(app, str(malformed), good=False)
print('PASS: '+kind+' core parser, selective composition, reload, format checks and CLI errors')
