#!/usr/bin/env python3
"""Verify optional core renderer scripting controls a resident scene and renders."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    scene, script, output = root / 'scene.usda', root / 'script.js', root / 'frame.png'
    scene.write_text('#usda 1.0\ndef Sphere "Ball" {}\n')
    script.write_text('''
if (lusdrender.stats().triangles <= 0) throw Error("Missing core geometry");
if (!(lusdrender.bounds().radius > 0)) throw Error("Missing bounds");
lusdrender.autoframe();
lusdrender.setResolution(16, 16);
const frame = lusdrender.render(%s);
if (frame.width !== 16 || frame.height !== 16) throw Error("Ignored resolution");
''' % json.dumps(str(output)))
    result = subprocess.run([sys.argv[1], '-js', str(script), str(scene)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    data = output.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', data[16:24]) == (16, 16)
    script.write_text('throw Error("expected renderer script failure");')
    result = subprocess.run([sys.argv[1], '-js', str(script), str(scene)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode != 0 and 'expected renderer script failure' in result.stderr, result
print('PASS: core renderer scripting stats, bounds, rendering, resolution and errors')
