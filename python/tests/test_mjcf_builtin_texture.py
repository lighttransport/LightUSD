# SPDX-License-Identifier: Apache-2.0
"""MJCF builtin textures generated as PNG (mjCTexture::Builtin2D / BuiltinCube)."""
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
    assert sorted(assets) == ['textures/builtin_grid.png', 'textures/builtin_sky.png']
    assert (_decode(assets['textures/builtin_grid.png']) == _expected()).all()
    shader = next(p for p in stage if p.type_name == 'Shader' and 'inputs:file' in p.attributes)
    assert shader['inputs:file'] == 'textures/builtin_grid.png'
    assert 'builtin texture' not in stage.warnings()


def test_gradient_skybox_becomes_latlong_dome_light():
    stage = lightusd.load_bytes(XML, format='mjcf')
    dome = stage.prim_at('/World/Lights/sky')
    assert dome.type_name == 'DomeLight'
    assert dome['inputs:texture:file'] == 'textures/builtin_sky.png'
    assert dome['inputs:texture:format'] == 'latlong'
    # The dome's +Y pole is rotated onto the Z-up stage's +Z.
    pole = np.array([0, 1, 0, 0]) @ np.asarray(dome.local_transform())
    assert pole[:3] == pytest.approx([0, 0, 1], abs=1e-12)
    img = _decode(stage.generated_assets()['textures/builtin_sky.png'])
    assert img.shape == (32, 64, 3)  # 4w x 2w for width=16
    # MuJoCo's default gradient: rgb1 (.8 -> 204) at the zenith, rgb2 (.5 ->
    # 127) at the nadir; rows sample half a pixel off the poles.
    assert np.abs(img[0].astype(int) - 204).max() <= 3
    assert np.abs(img[-1].astype(int) - 127).max() <= 3
    means = img.reshape(img.shape[0], -1).mean(axis=1)
    assert (np.diff(means) <= 1e-9).all()  # darkens monotonically towards the nadir


def test_only_the_first_skybox_is_converted_and_cube_materials_warn():
    xml = XML.replace(b'<material name="floor"', b'<texture name="sky2" type="skybox" builtin="flat" width="4"/>'
                      b'<texture name="box" builtin="checker" width="4"/><material name="boxmat" texture="box"/>'
                      b'<material name="floor"')
    stage = lightusd.load_bytes(xml, format='mjcf')
    assert [p.name for p in stage if p.type_name == 'DomeLight'] == ['sky']
    assert 'textures/builtin_sky2.png' not in stage.generated_assets()
    assert 'builtin texture `box` is not generated (type="cube" is not supported)' in stage.warnings()
    assert lightusd._import(data=XML, format='mjcf', profile='legacy').get_prim_at('/World/Lights/sky') is None
    filed = lightusd.load_bytes(XML.replace(b'builtin="gradient"', b'file="sky.png"'), format='mjcf')
    assert 'skybox texture `sky` is not converted' in filed.warnings()


def test_skybox_packaged_into_usdz(tmp_path):
    import zipfile
    stage = lightusd.load_bytes(XML, format='mjcf')
    stage.save(str(tmp_path / 'scene.usdz'))
    names = zipfile.ZipFile(tmp_path / 'scene.usdz').namelist()
    assert any(n.endswith('builtin_sky.png') for n in names)


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
    assert list(stage.generated_assets()) == ['textures/builtin_sky.png']
    assert 'width/height must be positive integers' in stage.warnings()
