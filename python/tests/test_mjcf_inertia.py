# SPDX-License-Identifier: Apache-2.0
"""MuJoCo compiler inertia inference (inertiafromgeom) and mesh-fit geoms."""
import math

import numpy as np
import pytest

import lightusd
from lightusd import physics

# Closed, outward-oriented unit cube (12 triangles) centered at (0.5, 0.5, 0.5).
CUBE_OBJ = b'''v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0 0 1
v 1 0 1
v 1 1 1
v 0 1 1
f 1 3 2
f 1 4 3
f 5 6 7
f 5 7 8
f 1 2 6
f 1 6 5
f 2 3 7
f 2 7 6
f 3 4 8
f 3 8 7
f 4 1 5
f 4 5 8
'''


def _body(stage, name):
    return stage.prim_at(f'/World/Links/{name}')


def _inertia_tensor(prim):
    return np.array(prim['newton:inertia'])


def _mjcf(body, assets=''):
    return (f'<mujoco><asset>{assets}</asset><worldbody>{body}</worldbody></mujoco>').encode()


@pytest.mark.parametrize('geom,mass,diag', [
    ('<geom type="sphere" size=".1"/>', 1000 * 4 / 3 * math.pi * 1e-3,
     [2 / 5 * 1e-2] * 3),
    ('<geom type="box" size=".1 .2 .3" density="500"/>', 500 * 8 * .006,
     [(.04 + .09) / 3, (.01 + .09) / 3, (.01 + .04) / 3]),
    ('<geom type="cylinder" size=".1 .2" mass="2"/>', 2,
     [(3 * .01 + .16) / 12, (3 * .01 + .16) / 12, .01 / 2]),
])
def test_primitive_inertia_from_geoms(geom, mass, diag):
    stage = lightusd.load_bytes(_mjcf(f'<body name="b"><freejoint/>{geom}</body>'), format='mjcf')
    body = _body(stage, 'b')
    assert body['physics:mass'] == pytest.approx(mass, rel=1e-5)
    unit = np.array(diag) * (mass if 'mass' not in geom or 'density' in geom else mass)
    assert sorted(body['physics:diagonalInertia']) == pytest.approx(sorted(unit), rel=1e-4)


def test_parallel_axis_combination_and_visual_geoms_count():
    # Two equal spheres offset +-x: COM at origin, Iyy/Izz gain 2*m*d^2. The
    # second sphere is visual-only (contype=conaffinity=0) but still weighs.
    xml = _mjcf('<body name="b"><freejoint/>'
                '<geom type="sphere" size=".1" pos=".5 0 0"/>'
                '<geom type="sphere" size=".1" pos="-.5 0 0" contype="0" conaffinity="0" group="2"/>'
                '</body>')
    body = _body(lightusd.load_bytes(xml, format='mjcf'), 'b')
    m = 1000 * 4 / 3 * math.pi * 1e-3
    assert body['physics:mass'] == pytest.approx(2 * m, rel=1e-6)
    assert tuple(body['physics:centerOfMass']) == pytest.approx((0, 0, 0), abs=1e-7)
    i = np.sort(np.array(body['physics:diagonalInertia']))
    base = 2 / 5 * m * .01
    assert i == pytest.approx(np.sort([2 * base, 2 * base + 2 * m * .25, 2 * base + 2 * m * .25]), rel=1e-5)


def test_fullinertia_principal_axes_reconstruct_mjcf_tensor():
    full = (5.0, 6.0, 7.0, 0.4, -0.2, 0.3)
    xml = _mjcf('<body name="b"><freejoint/>'
                '<inertial pos="0 0 0" mass="2" '
                'fullinertia="5 6 7 0.4 -0.2 0.3"/>'
                '<geom size=".1"/></body>')
    body = _body(lightusd.load_bytes(xml, format='mjcf'), 'b')
    diagonal = np.asarray(body['physics:diagonalInertia'], dtype=float)
    # LightUSD exposes quatf in scalar-first (w, x, y, z) order.
    w, x, y, z = body['physics:principalAxes']
    rotation = np.array([
        [1 - 2 * (y*y + z*z), 2 * (x*y - w*z), 2 * (x*z + w*y)],
        [2 * (x*y + w*z), 1 - 2 * (x*x + z*z), 2 * (y*z - w*x)],
        [2 * (x*z - w*y), 2 * (y*z + w*x), 1 - 2 * (x*x + y*y)],
    ])
    reconstructed = rotation @ np.diag(diagonal) @ rotation.T
    expected = np.array([[full[0], full[3], full[4]],
                         [full[3], full[1], full[5]],
                         [full[4], full[5], full[2]]])
    np.testing.assert_allclose(reconstructed, expected, rtol=1e-6, atol=1e-6)
    np.testing.assert_allclose(_inertia_tensor(body), full, rtol=0, atol=0)


def test_inertia_group_range_and_explicit_inertial():
    xml = _mjcf('<body name="a"><freejoint/><geom type="sphere" size=".1" group="4"/></body>'
                '<body name="b" pos="2 0 0"><freejoint/><inertial pos="0 0 0" mass="3" diaginertia="1 1 1"/>'
                '<geom type="sphere" size=".1"/></body>')
    xml = xml.replace(b'<mujoco>', b'<mujoco><compiler inertiagrouprange="0 3"/>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    with pytest.raises(KeyError):
        _body(stage, 'a')['physics:mass']
    assert _body(stage, 'b')['physics:mass'] == 3
    report = stage.import_report()
    assert report['counts'].get('inferred_inertia_bodies', 0) == 0


def test_legacy_profile_does_not_infer():
    xml = _mjcf('<body name="b"><freejoint/><geom type="sphere" size=".1"/></body>')
    stage = lightusd._import(data=xml, format='mjcf', profile='legacy')
    with pytest.raises(KeyError):
        _body(stage, 'b')['physics:mass']


def test_mesh_inertia_and_mesh_fit(tmp_path):
    (tmp_path / 'cube.obj').write_bytes(CUBE_OBJ)
    xml = _mjcf('<body name="m"><freejoint/><geom type="mesh" mesh="cube"/></body>'
                '<body name="fit" pos="3 0 0"><freejoint/><geom type="box" mesh="cube"/></body>',
                '<mesh name="cube" file="cube.obj"/>')
    path = tmp_path / 'robot.xml'
    path.write_bytes(xml)
    stage = lightusd.import_mjcf(path)
    mesh = _body(stage, 'm')
    assert mesh['physics:mass'] == pytest.approx(1000, rel=1e-6)
    assert tuple(mesh['physics:centerOfMass']) == pytest.approx((.5, .5, .5), abs=1e-6)
    assert list(mesh['physics:diagonalInertia']) == pytest.approx([1000 / 6] * 3, rel=1e-6)
    # The fitted box is the cube's equivalent inertia box, centered on its COM.
    fit = _body(stage, 'fit')
    assert fit['physics:mass'] == pytest.approx(1000, rel=1e-6)
    collider = [c for c in physics.inspect(stage).colliders if c.prim_path.startswith('/World/Links/fit/')]
    assert len(collider) == 1 and collider[0].type_name == 'Cube'
    xf = np.asarray(stage.prim_at(collider[0].prim_path).local_transform())
    assert xf[3, :3] == pytest.approx([.5, .5, .5], abs=1e-6)
    # Unit Cube scaled to the equivalent inertia box half-sizes (0.5).
    assert np.abs(xf[:3, :3]).sum(axis=0) == pytest.approx([.5, .5, .5], abs=1e-6)


def test_obj_first_object_only(tmp_path):
    two = CUBE_OBJ.replace(b'v 0 0 0', b'o first\nv 0 0 0') + b'o second\nv 5 5 5\nv 6 5 5\nv 5 6 5\nf 9 10 11\n'
    (tmp_path / 'two.obj').write_bytes(two)
    path = tmp_path / 'robot.xml'
    path.write_bytes(_mjcf('<body name="m"><freejoint/><geom type="mesh" mesh="two"/></body>',
                           '<mesh name="two" file="two.obj"/>'))
    stage = lightusd.import_mjcf(path)
    assert _body(stage, 'm')['physics:mass'] == pytest.approx(1000, rel=1e-6)
    assert 'uses only the first' in stage.warnings()


def test_inferred_inertia_matches_mujoco(tmp_path):
    mujoco = pytest.importorskip('mujoco')
    (tmp_path / 'cube.obj').write_bytes(CUBE_OBJ)
    xml = _mjcf('<body name="a" euler="10 20 30"><freejoint/>'
                '<geom type="capsule" fromto="0 0 0 .3 .1 .2" size=".05"/>'
                '<geom type="ellipsoid" size=".1 .2 .05" pos=".1 0 0" euler="0 45 0" density="300"/>'
                '<geom type="mesh" mesh="cube" pos="0 .4 0" mass="2"/>'
                '<geom type="cylinder" mesh="cube" pos="-.5 0 0"/></body>',
                '<mesh name="cube" file="cube.obj" scale=".2 .3 .4"/>')
    path = tmp_path / 'robot.xml'
    path.write_bytes(xml)
    model = mujoco.MjModel.from_xml_path(str(path))
    body = _body(lightusd.import_mjcf(path), 'a')
    assert body['physics:mass'] == pytest.approx(model.body_mass[1], rel=1e-5)
    assert sorted(body['physics:diagonalInertia']) == pytest.approx(sorted(model.body_inertia[1]), rel=1e-4)
    assert tuple(body['physics:centerOfMass']) == pytest.approx(tuple(model.body_ipos[1]), abs=1e-6)
