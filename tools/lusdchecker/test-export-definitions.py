#!/usr/bin/env python3
"""Definition conversion is deterministic and never executes shader sources."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
script = ROOT / 'scripts/export-checker-shaders.py'
with tempfile.TemporaryDirectory(prefix='shader-definitions-') as directory:
    root = Path(directory)
    usd = root / 'shaderDefs.usda'
    usd.write_text('''#usda 1.0
# fake declaration: def Shader "Ignored" {}
def Shader "Vendor" {
 uniform token info:id = "VendorShader"
 float inputs:gain = 1
 color3f outputs:color
}
''')
    mtlx = root / 'defs.mtlx'
    mtlx.write_text('''<materialx version="1.39">
<nodedef name="ND_base"><input name="gain" type="float"/><output name="out" type="color3"/></nodedef>
<nodedef name="ND_derived" inherit="ND_base"><input name="offset" type="vector3"/></nodedef>
</materialx>''')
    output = root / 'definitions.json'
    command = [sys.executable, str(script), str(usd), str(mtlx), '--output', str(output)]
    subprocess.run(command, check=True, capture_output=True)
    data = json.loads(output.read_text())
    nodes = {n['identifier']: n for n in data['shaders']}
    assert set(nodes) == {'VendorShader', 'ND_base', 'ND_derived'}
    assert nodes['VendorShader']['inputs'] == {'gain': 'float'}
    assert nodes['ND_derived']['inputs'] == {'gain': 'float', 'offset': 'float3'}
    assert nodes['ND_derived']['outputs'] == {'out': 'color3f'}
    original = output.read_bytes()
    subprocess.run(command, check=True, capture_output=True)
    assert output.read_bytes() == original
    for xml in ('<!DOCTYPE materialx><materialx/>',
                '<materialx><nodedef name="A" inherit="A"/></materialx>',
                '<materialx><nodedef name="A"><input name="x" type="vendorType"/></nodedef></materialx>'):
        mtlx.write_text(xml)
        p = subprocess.run(command, capture_output=True)
        assert p.returncode != 0 and output.read_bytes() == original
print('definition exporter contracts passed')
