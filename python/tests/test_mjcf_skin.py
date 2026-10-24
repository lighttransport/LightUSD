# SPDX-License-Identifier: Apache-2.0
"""MJCF <skin> as UsdSkel (newton/openusd profiles) plus LightUSD extension data."""
import struct

import numpy as np
import pytest

import lightusd

SKIN = '''<mujoco>
  <worldbody>
    <body name="a" pos="0.1 0.2 0.3" euler="20 30 40"><freejoint/><geom size=".05"/>
      <body name="b" pos="0.3 0 0" euler="0 45 10"><joint axis="0 0 1" ref="15"/><geom size=".05"/></body>
    </body>
  </worldbody>
  <deformable>
    <skin name="s" rgba=".2 .6 .9 1" inflate="%s"
          vertex="0 0 0  .3 0 0  .3 .1 0  0 .1 0  .15 .05 .1"
          face="0 1 2  0 2 3  0 1 4  1 2 4  2 3 4  3 0 4">
      <bone body="a" bindpos="0.02 0.01 0" bindquat="0.9 0.1 0.2 0.3" vertid="0 1 3 4" vertweight="1 0.5 2 0.3"/>
      <bone body="b" bindpos="0.3 0 0" bindquat="1 0 0.4 0" vertid="1 2 4" vertweight="0.5 1 0.7"/>
    </skin>
  </deformable>
</mujoco>'''


def _matrices(value):
    return [np.array(m, dtype=float).reshape(4, 4) for m in value]


def _skinned(stage, root='/World/Skins/s'):
    """UsdSkel linear blend skinning of the skin mesh at its rest pose."""
    skel, mesh = stage.prim_at(root + '/Skeleton'), stage.prim_at(root + '/Mesh')
    bind = _matrices(skel['bindTransforms'])
    rest = _matrices(skel['restTransforms'])
    xf = [np.linalg.inv(b) @ r for b, r in zip(bind, rest)]  # row-vector convention
    pts = np.array(mesh['points'], dtype=float)
    k = mesh.attribute('primvars:skel:jointIndices').metadata('elementSize') or 1
    idx = np.array(mesh['primvars:skel:jointIndices']).reshape(-1, k)
    w = np.array(mesh['primvars:skel:jointWeights']).reshape(-1, k)
    out = np.zeros_like(pts)
    for v, p in enumerate(pts):
        for j, wj in zip(idx[v], w[v]):
            out[v] += wj * (np.append(p, 1) @ xf[j])[:3]
    return out


def test_skin_is_usdskel_with_lightusd_extension():
    stage = lightusd.load_bytes((SKIN % '0').encode(), format='mjcf')
    assert stage.prim_at('/World/Skins/s').type_name == 'SkelRoot'
    skel, mesh = stage.prim_at('/World/Skins/s/Skeleton'), stage.prim_at('/World/Skins/s/Mesh')
    assert list(skel['joints']) == ['a', 'b']
    assert skel.relationship('lightusd:mjcSkin:bodies').targets == ('/World/Links/a', '/World/Links/b')
    assert mesh.relationship('skel:skeleton').targets == ('/World/Skins/s/Skeleton',)
    assert 'SkelBindingAPI' in (mesh.metadata('apiSchemas') or ())
    w = np.array(mesh['primvars:skel:jointWeights']).reshape(5, -1)
    np.testing.assert_allclose(w.sum(axis=1), 1, rtol=1e-6)  # MuJoCo normalizes per vertex
    openusd = lightusd._import(data=(SKIN % '0').encode(), format='mjcf', profile='openusd')
    assert not [a for a in openusd.prim_at('/World/Skins/s/Skeleton').attributes if a.startswith('lightusd:')]
    legacy = lightusd._import(data=(SKIN % '0').encode(), format='mjcf', profile='legacy')
    with pytest.raises(KeyError):
        legacy.prim_at('/World/Skins')


@pytest.mark.parametrize('inflate', ['0', '0.01'])
def test_skin_rest_pose_matches_mujoco(tmp_path, inflate):
    mujoco = pytest.importorskip('mujoco')
    path = tmp_path / 'skin.xml'
    path.write_text(SKIN % inflate)
    model = mujoco.MjModel.from_xml_path(str(path))
    data = mujoco.MjData(model)
    mujoco.mj_forward(model, data)
    scene = mujoco.MjvScene(model, maxgeom=100)
    mujoco.mjv_updateScene(model, data, mujoco.MjvOption(), None, mujoco.MjvCamera(),
                           mujoco.mjtCatBit.mjCAT_ALL, scene)
    expected = np.array(scene.skinvert).reshape(-1, 3)
    np.testing.assert_allclose(_skinned(lightusd.import_mjcf(path)), expected, atol=1e-6)


def test_skn_file_and_world_bone(tmp_path):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    faces = [(0, 2, 1), (0, 1, 3), (0, 3, 2), (1, 2, 3)]
    data = struct.pack('4i', 4, 0, 4, 2) + b''.join(struct.pack('3f', *v) for v in verts)
    data += b''.join(struct.pack('3i', *f) for f in faces)
    for body, pos, ids in (('a', (0, 0, 1), (0, 1)), ('world', (0, 0, 0), (2, 3))):
        data += body.encode().ljust(40, b'\0') + struct.pack('3f4f', *pos, 1, 0, 0, 0)
        data += struct.pack('i', len(ids)) + struct.pack(f'{len(ids)}i', *ids)
        data += struct.pack(f'{len(ids)}f', *([1.0] * len(ids)))
    (tmp_path / 'tet.skn').write_bytes(data)
    path = tmp_path / 'skn.xml'
    path.write_text('<mujoco><asset><skin file="tet.skn"/></asset><worldbody>'
                    '<body name="a" pos="0 0 2"><freejoint/><geom size=".1"/></body></worldbody></mujoco>')
    stage = lightusd.import_mjcf(path)
    skel = stage.prim_at('/World/Skins/tet/Skeleton')  # named after the file, as MuJoCo
    assert skel.relationship('lightusd:mjcSkin:bodies').targets == ('/World/Links/a', '/World')
    out = _skinned(stage, '/World/Skins/tet')
    np.testing.assert_allclose(out, [(0, 0, 1), (1, 0, 1), (0, 1, 0), (0, 0, 1)], atol=1e-6)


def test_invalid_skin_is_rejected():
    xml = (SKIN % '0').replace('vertid="0 1 3 4"', 'vertid="0 1 3 9"')
    with pytest.raises(Exception, match='out of range'):
        lightusd.load_bytes(xml.encode(), format='mjcf')
