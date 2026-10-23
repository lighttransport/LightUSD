# SPDX-License-Identifier: Apache-2.0
"""MJCF builtin 2D textures generated as PNG (mjCTexture::Builtin2D)."""
import struct
import zlib

import numpy as np
import pytest

import lightusd

XML = b'''<mujoco><asset>
 <texture name="grid" type="2d" builtin="checker" mark="edge" rgb1=".2 .3 .4" rgb2=".1 .2 .3"
          markrgb=".8 .8 .8" width="8" height="6"/>
 <texture name="sky" type="skybox" builtin="gradient" width="16" height="16"/>
 <material name="floor" texture="grid" texrepeat="5 5"/></asset>
<worldbody><geom type="plane" size="1 1 .1" material="floor"/></worldbody></mujoco>'''


def _decode(png):
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    i, idat = 8, b''
    while i < len(png):
        n, = struct.unpack('>I', png[i:i + 4])
        kind, data = png[i + 4:i + 8], png[i + 8:i + 8 + n]
        assert zlib.crc32(kind + data) == struct.unpack('>I', png[i + 8 + n:i + 12 + n])[0]
        if kind == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', data[:10])
            assert (depth, color) == (8, 2)
        elif kind == b'IDAT':
            idat += data
        i += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + 3 * w
    return np.array([list(raw[r * stride + 1:(r + 1) * stride]) for r in range(h)],
                    np.uint8).reshape(h, w, 3)


def _expected():
    c1 = (np.array([.2, .3, .4]) * 255).astype(np.uint8)
    c2 = (np.array([.1, .2, .3]) * 255).astype(np.uint8)
    img = np.empty((6, 8, 3), np.uint8)
    for r in range(6):
        for c in range(8):
            img[r, c] = c1 if (r < 3) == (c < 4) else c2
    img[0, :] = img[-1, :] = img[:, 0] = img[:, -1] = (np.array([.8] * 3) * 255).astype(np.uint8)
    return img


def test_checker_texture_matches_mujoco_layout():
    stage = lightusd.load_bytes(XML, format='mjcf')
    assets = stage.generated_assets()
    assert list(assets) == ['textures/builtin_grid.png']
    assert (_decode(assets['textures/builtin_grid.png']) == _expected()).all()
    shader = next(p for p in stage if p.type_name == 'Shader' and 'inputs:file' in p.attributes)
    assert shader['inputs:file'] == 'textures/builtin_grid.png'
    assert 'type="skybox" is not supported' in stage.warnings()


def test_generated_texture_written_beside_saved_layer(tmp_path):
    stage = lightusd.load_bytes(XML, format='mjcf')
    stage.save(str(tmp_path / 'scene.usda'))
    written = (tmp_path / 'textures' / 'builtin_grid.png').read_bytes()
    assert written == stage.generated_assets()['textures/builtin_grid.png']


def test_legacy_profile_and_bad_sizes_keep_rgba():
    legacy = lightusd._import(data=XML, format='mjcf', profile='legacy')
    assert legacy.generated_assets() == {}
    bad = XML.replace(b'width="8" height="6"', b'width="0" height="6"')
    stage = lightusd.load_bytes(bad, format='mjcf')
    assert stage.generated_assets() == {}
    assert 'width/height must be positive integers' in stage.warnings()
