# SPDX-License-Identifier: Apache-2.0
"""MJCF visual fidelity: native primitive visuals, colors, cameras, lights."""
import math

import numpy as np
import pytest

import lightusd

SCENE = b'''<mujoco><statistic extent="2"/><visual><map znear=".05" zfar="20"/></visual>
<asset><texture name="t" type="2d" file="tex.png"/><texture name="c" type="2d" builtin="checker" width="8" height="8"/>
  <material name="m" texture="t" texrepeat="3 2"/></asset>
<worldbody><light name="l" pos="0 0 3" dir="0 0 -1" intensity="5" exponent="10" cutoff="30"/>
  <camera name="cam" pos="0 -2 1"/>
  <body name="b" pos="0 0 1"><freejoint/>
    <geom name="cap" type="capsule" size=".05 .2" rgba="1 0 0 .5" contype="0" conaffinity="0"/>
    <geom name="egg" type="ellipsoid" size=".1 .2 .3" material="m" contype="0" conaffinity="0"/>
    <geom name="hit" type="box" size=".1 .1 .1"/></body></worldbody></mujoco>'''


def _stage(profile='newton'):
    return lightusd._import(data=SCENE, format='mjcf', profile=profile)


def _find(stage, name):
    return next(p for p in stage if p.name == name)


def test_primitive_visuals_are_native_gprims_with_color():
    stage = _stage()
    cap = _find(stage, 'cap')
    assert cap.type_name == 'Capsule'
    assert 'PhysicsCollisionAPI' not in (cap.metadata('apiSchemas') or ())
    assert list(cap['primvars:displayColor'][0]) == pytest.approx([1, 0, 0])
    assert list(cap['primvars:displayOpacity']) == pytest.approx([.5])
    assert cap['radius'] == pytest.approx(.05) and cap['height'] == pytest.approx(.4)
    # No UsdGeom ellipsoid: the radii are baked into a mesh (a scaled Sphere
    # is read as a unit sphere by Newton).
    egg = _find(stage, 'egg')
    assert egg.type_name == 'Mesh'
    pts = np.asarray(egg['points']).reshape(-1, 3)
    assert np.abs(pts).max(axis=0) == pytest.approx([.1, .2, .3], rel=1e-6)
    assert egg.relationship('material:binding').targets == ('/World/Materials/m',)
    assert 'Ellipsoid collider' not in stage.warnings()  # visual ellipsoids are exact


def test_legacy_capsule_mesh_has_caps():
    cap = _find(_stage('legacy'), 'cap')
    assert cap.type_name == 'Mesh'
    z = np.asarray(cap['points']).reshape(-1, 3)[:, 2]
    # Hemispherical caps reach half_length + radius beyond the body origin.
    assert z.max() == pytest.approx(.25, abs=1e-6) and z.min() == pytest.approx(-.25, abs=1e-6)


def test_camera_light_and_texture_transform():
    stage = _stage()
    cam = _find(stage, 'cam')
    fovy = 2 * math.degrees(math.atan(cam['verticalAperture'] / (2 * cam['focalLength'])))
    assert fovy == pytest.approx(45, abs=1e-3)
    assert tuple(cam['clippingRange']) == pytest.approx((.1, 40))
    light = _find(stage, 'l')
    assert light['inputs:intensity'] == pytest.approx(5)
    assert light['inputs:shaping:focus'] == pytest.approx(10)
    transform = stage.prim_at('/World/Materials/m/stTransform')
    assert tuple(transform['inputs:scale']) == pytest.approx((3, 2))
    assert 'builtin texture `c`' in stage.warnings()
