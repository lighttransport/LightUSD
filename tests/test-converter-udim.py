#!/usr/bin/env python3
"""Native UDIM output, pixels, layer edits and failure controls (stdlib only)."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

converter, cat = sys.argv[1:3]
with_exr = "--with-exr" in sys.argv[3:]
def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
def png(color, width=2, height=2):
    rows = (b'\0' + bytes(color) * width) * height
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
def colors(data):
    assert data.startswith(b'\x89PNG\r\n\x1a\n')
    pos, encoded = 8, b''
    while pos < len(data):
        size = struct.unpack_from('>I', data, pos)[0]
        kind, payload = data[pos+4:pos+8], data[pos+8:pos+8+size]
        if kind == b'IHDR':
            width, height, depth, kind_id, _, _, interlace = struct.unpack('>IIBBBBB', payload)
            assert depth in (8, 16) and kind_id in (2, 6) and interlace == 0
            channels = 4 if kind_id == 6 else 3
            pixel_bytes = channels * (depth // 8)
        if kind == b'IDAT': encoded += payload
        pos += size + 12
    rows = zlib.decompress(encoded)
    stride, previous, found = width * pixel_bytes, bytearray(width * pixel_bytes), set()
    for y in range(height):
        mode = rows[y * (stride + 1)]
        row = bytearray(rows[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = row[i-pixel_bytes] if i >= pixel_bytes else 0
            up = previous[i]
            upper_left = previous[i-pixel_bytes] if i >= pixel_bytes else 0
            p = left + up - upper_left
            distances = [abs(p-left), abs(p-up), abs(p-upper_left)]
            predictor = (left, up, upper_left)[distances.index(min(distances))]
            row[i] = (row[i] + (0, left, up, (left+up)//2, predictor)[mode]) & 255
        for i in range(0, stride, pixel_bytes):
            found.add(tuple(row[i:i+3]) if depth == 8 else struct.unpack_from('>HHH', row, i))
        previous = row
    return found

def run(*args, success=True):
    result = subprocess.run(args, capture_output=True, text=True)
    if success:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0, result.stdout + result.stderr
    return result

scene = '''#usda 1.0
def Xform "World" {
 def Mesh "Mesh" {
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  uniform token subdivisionScheme = "none"
  texCoord2f[] primvars:st = [(0.1,0.1), (0.8,0.1), (0.1,0.8)] (
   interpolation = "faceVarying"
  )
  rel material:binding = </World/Mat>
 }
 def Material "Mat" {
  token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
   token outputs:surface
  }
  def Shader "Texture" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @textures/tile.<UDIM>.png@
   float2 inputs:st.connect = </World/Mat/Reader.outputs:result>
   float3 outputs:rgb
  }
  def Shader "Reader" {
   uniform token info:id = "UsdPrimvarReader_float2"
   token inputs:varname = "st"
   float2 outputs:result
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='lightusd-udim-') as directory:
    directory = pathlib.Path(directory)
    (directory / 'textures').mkdir()
    expected = {(255,0,0), (0,255,0), (0,0,255)}
    for tile, color in zip((1001,1002,1101), sorted(expected)):
        (directory / f'textures/tile.{tile}.png').write_bytes(png((*color, 255)))
    source = directory / 'source.usda'
    source.write_text(scene)
    for mode in ('grid', 'dense'):
        for root in ('usda', 'usdc'):
            output = directory / f'{mode}-{root}.usdz'
            run(converter, str(source), str(output), '--bake-udim', mode, '--rootLayerFormat', root)
            with zipfile.ZipFile(output) as package:
                atlases = [name for name in package.namelist() if name.endswith('.png')]
                assert len(atlases) == 1, package.namelist()
                assert expected <= colors(package.read(atlases[0]))
            text_file = directory / 'rewritten.usda'
            run(cat, '--rewrite-layer', str(output), '-o', str(text_file))
            text = text_file.read_text()
            assert '<UDIM>' not in text and atlases[0] in text, text
            assert ('UsdTransform2d' in text) if mode == 'grid' else ('_udim' in text)
        for format in ('usda', 'usdc'):
            flat = directory / 'flat' / f'{mode}.{format}'
            run(converter, str(source), str(flat), '--outputFormat', format, '--bake-udim', mode)
            inspection = flat if format == 'usda' else flat.with_name(f'{mode}-inspection.usda')
            if format == 'usdc': run(cat, '--rewrite-layer', str(flat), '-o', str(inspection))
            asset = re.search(r'asset inputs:file = @([^@]+)@', inspection.read_text()).group(1)
            assert expected <= colors((flat.parent / asset).read_bytes())
    # A pre-existing unrelated file at the generated name must survive.
    first_asset = re.search(r'asset inputs:file = @([^@]+)@',
        (directory / 'flat/grid.usda').read_text()).group(1)
    occupied = directory / 'flat' / first_asset
    occupied.write_bytes(b'preserve this file')
    run(converter, str(source), str(directory / 'flat/grid.usda'),
        '--outputFormat', 'usda', '--bake-udim', 'grid')
    assert occupied.read_bytes() == b'preserve this file'
    new_asset = re.search(r'asset inputs:file = @([^@]+)@',
        (directory / 'flat/grid.usda').read_text()).group(1)
    assert first_asset != new_asset
    assert expected <= colors((directory / 'flat' / new_asset).read_bytes())

    # Composition must anchor the pattern to the layer that authored it.
    look = directory / 'look'
    (look / 'local').mkdir(parents=True)
    for tile, color in zip((1001,1002,1101), sorted(expected)):
        (look / f'local/tile.{tile}.png').write_bytes(png((*color, 255)))
    dependency = look / 'dependency.usda'
    dependency.write_text(scene.replace('@textures/', '@local/'))
    referencing = directory / 'reference.usda'
    referencing.write_text('#usda 1.0\ndef Xform "Referenced" (references = @look/dependency.usda@</World>) {}\n')
    run(converter, str(referencing), str(directory / 'reference.usdz'), '--bake-udim', 'grid')

    # All variant options must be baked in a non-flattened root layer.
    inner = scene[scene.index('{\n')+2:scene.rfind('}')]
    variant = directory / 'variant.usda'
    variant.write_text('#usda 1.0\ndef Xform "World" (prepend variantSets = "look"\n'
        'variants = { string look = "a" }) {\nvariantSet "look" = {\n'
        '"a" {\n' + inner + '}\n"b" {\n' + inner + '}\n}\n}\n')
    for mode in ('grid', 'dense'):
        output = directory / f'variant-{mode}.usdz'
        run(converter, str(variant), str(output), '--bake-udim', mode, '-noFlatten', '--rootLayerFormat', 'usda')
        with zipfile.ZipFile(output) as package:
            root = next(name for name in package.namelist() if name.endswith('.usda'))
            text = package.read(root).decode()
            assert 'variantSet "look"' in text and '<UDIM>' not in text, text
            assert len([name for name in package.namelist() if name.endswith('.png')]) == 1

    # Stored USDZ input has authoring-layer-relative package entry anchors.
    archived = directory / 'input.usdz'
    with zipfile.ZipFile(archived, 'w', compression=zipfile.ZIP_STORED) as package:
        entries = [('nested/root.usda', scene.encode())]
        entries += [(f'nested/textures/tile.{tile}.png', png((*color,255)))
                    for tile,color in zip((1001,1002,1101), sorted(expected))]
        for name, data in entries:
            entry = zipfile.ZipInfo(name)
            padding = (-(package.fp.tell() + 30 + len(name.encode()) + 4)) % 64
            entry.extra = struct.pack('<HH', 0x1986, padding) + bytes(padding)
            package.writestr(entry, data)
    run(converter, str(archived), str(directory / 'from-package.usdz'), '--bake-udim', 'grid')
    with zipfile.ZipFile(directory / 'from-package.usdz') as package:
        atlas = next(name for name in package.namelist() if name.endswith('.png'))
        assert expected <= colors(package.read(atlas))

    (directory / 'textures/big.1001.png').write_bytes(png((255,0,0,255), 128, 128))
    oversized = directory / 'oversized.usda'
    oversized.write_text(scene.replace('textures/tile.', 'textures/big.'))
    over_budget = directory / 'over-budget.usdz'
    result = run(converter, str(oversized), str(over_budget), '--bake-udim', 'grid',
        '--udim-memory-budget', '64KB', success=False)
    assert 'memory' in result.stderr.lower() and not over_budget.exists(), result.stderr

    # Time-sampled file patterns share a union layout, even with different
    # tile identities and resolutions in each frame.
    (directory / 'textures/frame.1003.png').write_bytes(png((255,255,0,255), 4, 4))
    animated = directory / 'animated.usda'
    animated.write_text(scene.replace('asset inputs:file = @textures/tile.<UDIM>.png@',
        'asset inputs:file.timeSamples = {0: @textures/tile.<UDIM>.png@, 1: @textures/frame.<UDIM>.png@}'))
    for mode in ('grid', 'dense'):
        output = directory / f'animated-{mode}.usdz'
        run(converter, str(animated), str(output), '--bake-udim', mode, '--rootLayerFormat', 'usda', '-noFlatten')
        with zipfile.ZipFile(output) as package:
            atlases = [package.read(name) for name in package.namelist() if name.endswith('.png')]
            assert len(atlases) == 2
            assert len({struct.unpack_from('>II', data, 16) for data in atlases}) == 1
            assert any((255,255,0) in colors(data) for data in atlases)
            assert any(expected <= colors(data) for data in atlases)

    # Preserve 16-bit PNG channels rather than silently reducing to eight bits.
    precision_colors = {(0x1234,0x5678,0x9abc), (0xfedc,0x3210,0x7654)}
    pixel16 = b''.join(struct.pack('>HHHH', *color, 65535) for color in sorted(precision_colors))
    png16 = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',2,1,16,6,0,0,0)) + chunk(b'IDAT',zlib.compress(b'\0'+pixel16)) + chunk(b'IEND',b'')
    (directory / 'textures/precision.1001.png').write_bytes(png16)
    precision = directory / 'precision.usda'
    precision.write_text(scene.replace('textures/tile.', 'textures/precision.').replace(
        'uniform token info:id = "UsdUVTexture"',
        'uniform token info:id = "UsdUVTexture"\n token inputs:sourceColorSpace = "raw"'))
    run(converter, str(precision), str(directory / 'precision.usdz'), '--bake-udim', 'grid')
    with zipfile.ZipFile(directory / 'precision.usdz') as package:
        atlas = next(name for name in package.namelist() if name.endswith('.png'))
        data = package.read(atlas)
        assert data[24] == 16 and precision_colors <= colors(data)

    if with_exr:
        hdr = b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n' + bytes([128,0,0,129,0,128,0,130])
        (directory / 'textures/hdr.1001.hdr').write_bytes(hdr)
        floating = directory / 'hdr.usda'
        floating.write_text(scene.replace('textures/tile.<UDIM>.png', 'textures/hdr.<UDIM>.hdr'))
        run(converter, str(floating), str(directory / 'hdr.usdz'), '--bake-udim', 'grid')
        with zipfile.ZipFile(directory / 'hdr.usdz') as package:
            atlas = next(name for name in package.namelist() if name.endswith('.exr'))
            data = package.read(atlas)
            assert struct.unpack_from('<I',data)[0] == 20000630
        (directory / 'textures/exr.1001.exr').write_bytes(data)
        floating.write_text(scene.replace('textures/tile.<UDIM>.png', 'textures/exr.<UDIM>.exr'))
        run(converter, str(floating), str(directory / 'exr.usdz'), '--bake-udim', 'grid')
        with zipfile.ZipFile(directory / 'exr.usdz') as package:
            atlas = next(name for name in package.namelist() if name.endswith('.exr'))
            assert package.read(atlas) == data, 'EXR atlas values changed on decode/re-encode'
        rejected = directory / 'lossy-float.usdz'
        result = run(converter, str(floating), str(rejected), '--bake-udim', 'grid', '-textureFormat', 'png', success=False)
        assert 'precision' in result.stderr or 'floating' in result.stderr, result.stderr
        assert not rejected.exists()

    failed = directory / 'failed.usdz'
    result = run(converter, str(source), str(failed), '--bake-udim', 'grid', '--udim-max-tiles', '2', success=False)
    assert 'tile count' in result.stderr and not failed.exists(), result.stderr
    crossing = directory / 'crossing.usda'
    crossing.write_text(scene.replace('(0.8,0.1)', '(1.1,0.1)'))
    run(converter, str(crossing), str(failed), '--bake-udim', 'dense', success=False)
    assert not failed.exists()
    run(converter, str(crossing), str(failed), '--bake-udim', 'dense', '--udim-cross-tile', 'split')
    run(cat, str(failed))
print('PASS: native grid/dense UDIM pixels, edits, output formats and failure controls')
