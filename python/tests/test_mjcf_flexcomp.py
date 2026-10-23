# SPDX-License-Identifier: Apache-2.0
"""MJCF flexcomp soft bodies as UsdPhysics deformables (newton profile)."""
import numpy as np
import pytest

import lightusd

FLEX = '''<mujoco><worldbody>
  <body name="holder" pos="0 0 1">
    <flexcomp name="%s" type="grid" dim="%d" count="%s" spacing=".1 .1 .1" mass="2" radius=".01"
              pos=".1 0 0" euler="0 0 90">
      <elasticity young="1e4" poisson=".3" thickness=".002" elastic2d="both"/>
      <edge stiffness="50" damping=".5"/>
      %s
    </flexcomp>
  </body></worldbody></mujoco>'''


def _stage(name, dim, count, extra='', profile='newton'):
    return lightusd._import(data=(FLEX % (name, dim, count, extra)).encode(), format='mjcf',
                            profile=profile)


def _points(prim):
    return np.asarray(prim['points'], dtype=float).reshape(-1, 3)


def _material(stage, prim):
    return stage.prim_at(prim.relationship('material:binding:physics').targets[0])


def test_cloth_grid_matches_mujoco_layout_and_pins():
    stage = _stage('cloth', 2, '3 4 1', '<pin id="0 11"/>')
    cloth = stage.prim_at('/World/Deformables/cloth')
    assert cloth.type_name == 'Mesh'
    assert 'PhysicsSurfaceDeformableSimAPI' in cloth.metadata('apiSchemas')
    assert len(cloth['faceVertexCounts']) == 2 * 2 * 3
    pts = _points(cloth)
    # Grid point (ix, iy) = spacing * (i - (count - 1) / 2), rotated 90 deg
    # about Z and placed at the flexcomp pose inside the body.
    assert pts[0] == pytest.approx([.1 + .15, -.1, 1])
    assert list(cloth['physics:masses']) == pytest.approx([2 / 12] * 12)
    mat = _material(stage, cloth)
    assert mat['physics:youngsModulus'] == pytest.approx(1e4)
    assert mat['physics:surfaceStretchStiffness'] == pytest.approx(1e4 * .002 / (1 - .09))
    pins = stage.prim_at('/World/Deformables/cloth_pins')
    assert pins.type_name == 'PhysicsAttachment'
    assert list(pins['physics:indices0']) == [0, 11]
    assert pins.relationship('physics:src1').targets == ('/World/Links/holder',)
    # Pin coordinates are in the body frame.
    assert tuple(pins['physics:coords1'][0]) == pytest.approx((.25, -.1, 0))


def test_cable_and_volume():
    stage = _stage('rope', 1, '5 1 1')
    rope = stage.prim_at('/World/Deformables/rope')
    assert rope.type_name == 'BasisCurves' and list(rope['curveVertexCounts']) == [5]
    mat = _material(stage, rope)
    assert mat['physics:curvesStretchStiffness'] == pytest.approx(50 * .1)
    assert mat['newton:curvesStretchDamping'] == pytest.approx(.5 * .1)
    assert list(rope['physics:thicknesses']) == pytest.approx([.02])

    stage = _stage('jelly', 3, '3 3 3')
    jelly = stage.prim_at('/World/Deformables/jelly')
    tets = np.asarray(jelly['tetVertexIndices']).reshape(-1, 4)
    assert len(tets) == 8 * 6
    pts = _points(jelly)
    vol = [np.linalg.det(np.stack([pts[t[1]] - pts[t[0]], pts[t[2]] - pts[t[0]],
                                   pts[t[3]] - pts[t[0]]])) for t in tets]
    assert min(vol) > 0  # Newton keeps only right-handed tets
    assert len(jelly['surfaceFaceVertexIndices']) == 6 * 2 * 2 * 2


def test_flexcomp_outside_newton_profile_is_unsupported():
    stage = _stage('cloth', 2, '3 3 1', profile='openusd')
    with pytest.raises(KeyError):
        stage.prim_at('/World/Deformables')
    assert 'Unsupported MJCF element: flexcomp' in stage.warnings()


def test_flexcomp_matches_mujoco_vertices():
    mujoco = pytest.importorskip('mujoco')
    # MuJoCo accepts edge stiffness only for 1D flexes.
    xml = (FLEX % ('cloth', 2, '4 3 1', '<pin id="2"/>')).replace(
        '<edge stiffness="50" damping=".5"/>', '')
    model = mujoco.MjModel.from_xml_string(xml)
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    mujoco.mj_flex(model, data)
    ours = _points(lightusd.load_bytes(xml.encode(), format='mjcf').prim_at('/World/Deformables/cloth'))
    np.testing.assert_allclose(ours, data.flexvert_xpos, atol=1e-6)


CABLE = '''<mujoco><worldbody><body name="hook" pos="0 0 1">
  <composite type="cable" prefix="c" curve="s cos(s) 0" count="9 1 1" size="1 .2 1" offset=".1 0 0">
    <joint kind="main" damping=".02" stiffness=".1"/>
    <geom type="capsule" size=".01" density="500"/>
  </composite></body></worldbody></mujoco>'''


def test_cable_composite_expands_like_mujoco():
    stage = lightusd.load_bytes(CABLE.encode(), format='mjcf')
    names = ['cB_first'] + [f'cB_{i}' for i in range(1, 7)] + ['cB_last']
    joints = [p for p in stage if p.type_name == 'PhysicsSphericalJoint']
    assert len(joints) == 8  # initial="ball": the first segment hangs on a ball too
    mujoco = pytest.importorskip('mujoco')
    model = mujoco.MjModel.from_xml_string(CABLE)
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    for name in names:
        prim = stage.prim_at(f'/World/Links/{name}')
        body = model.body(name)
        m = np.asarray(prim.world_transform())
        np.testing.assert_allclose(m[3, :3], data.xpos[body.id], atol=1e-6)
        # USD row-vector rotation is MuJoCo's xmat transposed.
        np.testing.assert_allclose(m[:3, :3], data.xmat[body.id].reshape(3, 3).T, atol=1e-6)
        assert prim['physics:mass'] == pytest.approx(model.body_mass[body.id], rel=1e-5)
    assert sum(1 for p in stage if p.path.startswith('/World/Links/cB_') and 'Site' not in p.type_name
               and p.name.startswith('cS_')) == 2
