# SPDX-License-Identifier: Apache-2.0
"""Newton-profile details checked against Newton's USD importer contract."""
import math

import pytest

import lightusd


def _mjcf(body, extra=''):
    return f'<mujoco>{extra}<worldbody>{body}</worldbody></mujoco>'.encode()


def test_urdf_planar_free_axes_are_unlimited_dofs():
    urdf = b'''<robot name="r"><link name="base"/><link name="s">
      <inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <joint name="p" type="planar"><parent link="base"/><child link="s"/><axis xyz="0 0 1"/></joint></robot>'''
    plane = lightusd.load_bytes(urdf, format='urdf').prim_at('/World/Joints/p')
    apis = set(plane.metadata('apiSchemas'))
    for dof in ('transY', 'transZ', 'rotX'):
        assert f'PhysicsLimitAPI:{dof}' in apis
        assert plane[f'limit:{dof}:physics:low'] == -math.inf
        assert plane[f'limit:{dof}:physics:high'] == math.inf


def test_urdf_effort_becomes_joint_force_range_and_no_zero_gap():
    urdf = b'''<robot name="r"><link name="a"><collision><geometry><box size="1 1 1"/></geometry></collision></link>
      <link name="b"><inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <joint name="j" type="revolute"><parent link="a"/><child link="b"/><axis xyz="0 0 1"/>
      <limit lower="-1" upper="1" effort="12" velocity="2"/></joint></robot>'''
    stage = lightusd.load_bytes(urdf, format='urdf')
    j = stage.prim_at('/World/Joints/j')
    assert j['mjc:actuatorfrclimited'] == 'true'
    assert (j['mjc:actuatorfrcrange:min'], j['mjc:actuatorfrcrange:max']) == (-12, 12)
    collider = next(p for p in stage if p.type_name == 'Cube')
    assert 'newton:contactGap' not in collider.attributes
    assert 'newton:contactMargin' not in collider.attributes


def test_scene_flags_gravity_and_filterparent():
    xml = _mjcf('<body name="a"><joint name="h" axis="0 0 1"/><geom size=".1"/></body>',
                '<option><flag gravity="disable" filterparent="disable" spring="disable"/></option>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    scene = stage.prim_at('/World/PhysicsScene')
    assert scene['newton:gravityEnabled'] is False
    assert scene['mjc:flag:spring'] is False
    assert stage.prim_at('/World/Joints/h')['physics:collisionEnabled'] is True


def test_ref_is_not_mirrored_into_joint_state():
    xml = _mjcf('<body name="a"><joint name="h" axis="0 0 1" ref="30"/><geom size=".1"/></body>')
    j = lightusd.load_bytes(xml, format='mjcf').prim_at('/World/Joints/h')
    assert 'state:angular:physics:position' not in j.attributes
    assert j['mjc:ref'] == pytest.approx(30)


def test_weld_relpose_anchors_body2_frame():
    xml = _mjcf('<body name="a" pos="0 0 1"><freejoint/><geom size=".1"/></body>'
                '<body name="b" pos="3 0 1"><freejoint/><geom size=".1"/></body>',
                ).replace(b'</mujoco>', b'<equality><weld body1="a" body2="b" anchor=".1 .2 .3" '
                          b'relpose="1 0 0 1 0 0 0"/></equality></mujoco>')
    weld = next(p for p in lightusd.load_bytes(xml, format='mjcf')
                if p.type_name == 'PhysicsFixedJoint')
    assert tuple(weld['physics:localPos1']) == pytest.approx((.1, .2, .3))
    assert tuple(weld['physics:localPos0']) == pytest.approx((1.1, .2, .3))


def test_stack_with_ball_keeps_chain_in_newton_profile():
    xml = _mjcf('<body name="a"><joint name="j0" type="ball"/><joint name="j1" type="slide"/>'
                '<geom size=".1"/></body>')
    joints = [p for p in lightusd.load_bytes(xml, format='mjcf') if p.path.startswith('/World/Joints/')]
    bodies = {p.relationship('physics:body1').targets for p in joints}
    assert len(joints) == 2 and len(bodies) == 2  # one intermediate link
    stacked = lightusd._import(data=xml, format='mjcf', profile='openusd')
    joints = [p for p in stacked if p.path.startswith('/World/Joints/')]
    assert len({p.relationship('physics:body1').targets for p in joints}) == 1


def test_unresolved_actuator_target_is_skipped():
    xml = _mjcf('<body name="a"><joint name="h"/><geom size=".1"/></body>',
                ).replace(b'</mujoco>', b'<tendon><spatial name="t"><site site="nope"/><site site="x"/>'
                          b'</spatial></tendon><actuator><motor name="m" tendon="t"/></actuator></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    assert not [p for p in stage if p.type_name == 'MjcActuator']
    assert 'Skipping actuator `m`' in stage.warnings()


def test_exclude_with_world_filters_world_colliders():
    xml = _mjcf('<geom name="floor" type="plane" size="1 1 .1"/>'
                '<body name="a" pos="0 0 1"><freejoint/><geom size=".1"/></body>'
                ).replace(b'</mujoco>', b'<contact><exclude body1="world" body2="a"/></contact></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    a = stage.prim_at('/World/Links/a')
    targets = a.relationship('physics:filteredPairs').targets
    assert targets and all(stage.prim_at(t).type_name == 'Plane' for t in targets)
