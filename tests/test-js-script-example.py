#!/usr/bin/env python3
"""Core metadata scripts retain Unicode, sublayer offsets and error behavior."""
import json
import pathlib
import subprocess
import sys
import tempfile
app = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    scene = root / 'scene.usda'
    scene.write_text('''#usda 1.0
(
 upAxis="Z"
 defaultPrim="World"
 metersPerUnit=2
 endTimeCode=48
 comment="Unicode café 日本語"
 subLayers=[@child.usda@ (offset=3; scale=2)]
)
def Xform "World" {}
''')
    script = root / 'query.js'
    script.write_text('console.log(JSON.stringify(getLayerMetas()));')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    data = json.loads(result.stdout)
    assert data['upAxis'] == 'Z' and data['defaultPrim'] == 'World', data
    assert data['comment'] == 'Unicode café 日本語', data
    assert data['metersPerUnit'] == 2 and data['endTimeCode'] == 48, data
    assert data['primChildren'] == ['World'], data
    assert data['subLayers'] == [dict(assetPath='child.usda', layerOffset=dict(offset=3, scale=2))], data
    script.write_text('throw new Error("expected script failure");')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode != 0 and 'expected script failure' in result.stderr, result
    script.write_text('Promise.reject(new Error("unhandled"));')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode != 0 and 'Promise rejection' in result.stderr, result
    script.write_text('Promise.reject(new Error("handled")).catch(()=>{});')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    script.write_text('while(true) {}')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode != 0 and 'interrupted' in result.stderr, result
    script.write_text('let f=()=>Promise.resolve().then(f); f();')
    result = subprocess.run([app, str(scene), str(script)], text=True, capture_output=True, timeout=15)
    assert result.returncode != 0, result
print('PASS: core JavaScript metadata, Unicode, offsets, errors and execution deadlines')
