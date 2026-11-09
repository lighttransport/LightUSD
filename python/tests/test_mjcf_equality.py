# SPDX-License-Identifier: Apache-2.0
"""MJCF equality constraints as UsdPhysics loop-closing joints (non-legacy)."""
import numpy as np
import pytest

import lightusd
from lightusd import physics


def _world_point(stage, joint, side):
    body = joint.relationships.get(f'physics:body{side}')
    local = np.asarray(joint.attributes[f'physics:localPos{side}'], dtype=float)
    if not body:
        return local
    m = np.asarray(stage.prim_at(body[0]).world_transform())
    return local @ m[:3, :3] + m[3, :3]


BASE = '''<mujoco><worldbody>
  <body name="a" pos="0 0 1" euler="0 0 30"><joint name="ja" axis="0 1 0"/><geom type="sphere" size=".1"/>
    <site name="sa" pos=".2 0 0"/></body>
  <body name="b" pos=".5 0 1" euler="10 0 0"><joint name="jb" axis="0 1 0"/><geom type="sphere" size=".1"/>
    <site name="sb" pos="-.3 .1 0"/></body>
</worldbody><equality>%s</equality></mujoco>'''


def _joints(stage, type_name):
    return [j for j in physics.inspect(stage).joints if j.type_name == type_name
            and j.prim_path.startswith('/World/Equalities/')]


def test_connect_is_spherical_loop_joint():
    stage = lightusd.load_bytes((BASE % '<connect name="c" body1="a" body2="b" anchor=".2 .1 0" solref=".01 1"/>').encode(), format='mjcf')
    (joint,) = _joints(stage, 'PhysicsSphericalJoint')
    assert joint.attributes['physics:excludeFromArticulation'] is True
    assert 'MjcEqualityConnectAPI' in joint.api_schemas
    assert joint.attributes['mjc:solref'] == pytest.approx((.01, 1))
    # Both joint frames sit at the anchor (given in body1's frame).
    assert _world_point(stage, joint, 0) == pytest.approx(_world_point(stage, joint, 1), abs=1e-6)
    m = np.asarray(stage.prim_at('/World/Links/a').world_transform())
    assert _world_point(stage, joint, 0) == pytest.approx(np.array([.2, .1, 0]) @ m[:3, :3] + m[3, :3], abs=1e-6)


def test_site_connect_and_world_weld():
    xml = BASE % ('<connect name="s" site1="sa" site2="sb" active="false"/>'
                  '<weld name="w" body1="b"/>')
    stage = lightusd.load_bytes(xml.encode(), format='mjcf')
    (connect,) = _joints(stage, 'PhysicsSphericalJoint')
    assert connect.attributes['physics:jointEnabled'] is False
    assert connect.attributes['physics:localPos0'] == pytest.approx((.2, 0, 0), abs=1e-6)
    assert connect.attributes['physics:localPos1'] == pytest.approx((-.3, .1, 0), abs=1e-6)
    (weld,) = _joints(stage, 'PhysicsFixedJoint')
    # Welded to the world: body0 empty, body1 the dynamic body, frames coincide.
    assert 'physics:body0' not in weld.relationships
    assert weld.relationships['physics:body1'] == ('/World/Links/b',)
    assert _world_point(stage, weld, 0) == pytest.approx(_world_point(stage, weld, 1), abs=1e-6)


def test_joint_equality_is_newton_mimic_in_degrees():
    xml = BASE % '<joint name="m" joint1="jb" joint2="ja" polycoef=".1 2 0 0 0"/>'
    stage = lightusd.load_bytes(xml.encode(), format='mjcf')
    follower = stage.prim_at('/World/Joints/jb')
    assert {'NewtonMimicAPI', 'MjcEqualityJointAPI'} <= set(follower.metadata('apiSchemas'))
    assert follower['newton:mimicCoef0'] == pytest.approx(np.degrees(.1), rel=1e-6)
    assert follower['newton:mimicCoef1'] == 2
    assert follower.relationship('newton:mimicJoint').targets == ('/World/Joints/ja',)


def test_legacy_keeps_equality_xforms():
    xml = BASE % '<connect name="c" body1="a" body2="b" anchor="0 0 0"/>'
    stage = lightusd._import(data=xml.encode(), format='mjcf', profile='legacy')
    assert stage.prim_at('/World/Equalities/c').type_name == 'Xform'
