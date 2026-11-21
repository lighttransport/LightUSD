#!/usr/bin/env python3
"""Exercise the core texture companion authoring example without image dependencies."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

app, cat, zstd_enabled = sys.argv[1:]
def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + result.stdout
    return result.stdout

def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))

with tempfile.TemporaryDirectory(prefix='lightusd-texcomp-') as directory:
    root = pathlib.Path(directory)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>2I5B', 4, 4, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress((b'\0' + bytes([100, 150, 200, 255]) * 4) * 4))
    png += chunk(b'IEND', b'')
    (root / 'color.png').write_bytes(png)
    source = root / 'input.usda'
    source.write_text('''#usda 1.0
 def Scope "World" {
  def Shader "Texture" {
   uniform token info:id="UsdUVTexture"
   asset inputs:file=@color.png@ (customData={ string keep="retained"; asset ktx2=@old.ktx2@; })
   float inputs:scale=2
  }
  def Shader "Other" { asset inputs:file=@color.png@; }
 }
''')
    binary = root / 'input.usdc'
    run(cat, '--rewrite-layer', str(source), '-o', str(binary))
    for index, input_path in enumerate([source, binary]):
        output = root / ('output' + str(index) + '.usda')
        run(app, str(input_path), '-o', str(output), '--mips', 'on', '--zstd', 'off')
        text = output.read_text()
        assert text.count('@color.png@') == 2, text
        assert text.count('@color.ktx2@') == 2, text
        assert 'retained' in text and 'old.ktx2' not in text, text
        run(cat, str(output))
        encoded = (root / 'color.ktx2').read_bytes()
        assert encoded[:12] == b'\xabKTX 20\xbb\r\n\x1a\n'
        assert struct.unpack_from('<II', encoded, 20) == (4, 4)
        assert struct.unpack_from('<II', encoded, 40) == (3, 0)
        run(app, str(input_path), '-o', str(output), '--mips', 'off', '--zstd', 'on')
        encoded = (root / 'color.ktx2').read_bytes()
        levels, scheme = struct.unpack_from('<II', encoded, 40)
        assert levels == 1 and scheme == (2 if zstd_enabled == "1" else 0), (levels, scheme)
    (root / 'color.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n' + bytes([64, 128, 192, 130]) * 16)
    hdr = root / 'hdr.usda'
    hdr.write_text(source.read_text().replace('color.png', 'color.hdr'))
    hdr_output = root / 'hdr-output.usda'
    run(app, str(hdr), '-o', str(hdr_output), '--mips', 'off')
    encoded = (root / 'color.ktx2').read_bytes()
    assert struct.unpack_from('<I', encoded, 12)[0] in (143, 144), 'HDR must retain BC6H format'
    run(cat, str(hdr_output))
    missing = root / 'missing.usda'
    missing.write_text('#usda 1.0\ndef Shader "S" { asset inputs:file=@missing.png@; }\n')
    result = subprocess.run([app, str(missing), '-o', str(root/'missing-output.usda')], capture_output=True)
    assert result.returncode != 0
print('PASS: core texture companions, metadata preservation, USDA/USDC and mip/compression options')
