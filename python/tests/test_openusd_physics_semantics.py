# SPDX-License-Identifier: Apache-2.0
"""Check physical joint frames through the optional OpenUSD parser.

This verifies parsed rest-frame semantics, not engine dynamics.
"""
import math

import pytest

pytest.importorskip('pxr')
from pxr import Gf, Usd, UsdGeom, UsdPhysics
import lightusd


URDF = '''<robot name="r">
  <link name="base"><inertial><mass value="2"/><inertia ixx="1" ixy="0" ixz="0" iyy="1" iyz="0" izz="1"/></inertial>
    <collision><geometry><box size="1 1 1"/></geometry></collision></link>
  <link name="arm"><inertial><mass value="1"/><inertia ixx=".2" ixy="0" ixz="0" iyy=".2" iyz="0" izz=".2"/></inertial>
    <collision><geometry><box size=".5 .2 .2"/></geometry></collision></link>
  <joint name="hinge" type="revolute"><parent link="base"/><child link="arm"/>
    <origin xyz=".3 -.2 .5" rpy=".3 -.4 .7"/><axis xyz="0 0 1"/>
    <limit lower="-1" upper=".5" effort="10" velocity="2"/></joint>
</robot>'''


def _world_frame(stage, cache, body, local_pos, local_rot, axis):
    prim = stage.GetPrimAtPath(body)
    assert prim
    matrix = cache.GetLocalToWorldTransform(prim)
    position = matrix.Transform(Gf.Vec3d(local_pos))
    direction = Gf.Rotation(Gf.Quatd(local_rot)).TransformDir(Gf.Vec3d(axis))
    direction = matrix.TransformDir(direction).GetNormalized()
    return tuple(position), tuple(direction)


@pytest.mark.parametrize('profile', ['legacy', 'newton', 'openusd'])
@pytest.mark.parametrize('layout', ['flat', 'nested'])
def test_rotated_urdf_joint_semantics(tmp_path, profile, layout):
    source = tmp_path / 'rotated.urdf'
    source.write_text(URDF)
    output = tmp_path / f'{profile}-{layout}.usda'
    lightusd.import_urdf(source, profile=profile, layout=layout).save(str(output))
    stage = Usd.Stage.Open(str(output))
    assert stage
    parsed = UsdPhysics.LoadUsdPhysicsFromRange(stage, ['/'])
    entries = [(paths, descs) for kind, (paths, descs) in parsed.items()
               if str(kind).endswith('RevoluteJoint')]
    assert len(entries) == 1 and len(entries[0][1]) == 1
    joint = entries[0][1][0]
    assert joint.isValid and joint.jointEnabled and not joint.collisionEnabled
    assert str(joint.body0) == '/World/Links/base'
    assert str(joint.body1).endswith('/arm')
    assert joint.limit.enabled
    assert joint.limit.lower == pytest.approx(math.degrees(-1), abs=1e-4)
    assert joint.limit.upper == pytest.approx(math.degrees(0.5), abs=1e-4)
    axis = {'X': (1, 0, 0), 'Y': (0, 1, 0), 'Z': (0, 0, 1)}[str(joint.axis)]
    # URDF fixed-axis RPY: Rz(yaw) Ry(pitch) Rx(roll) applied to its Z axis.
    roll, pitch, yaw = 0.3, -0.4, 0.7
    expected_axis = (math.cos(yaw)*math.sin(pitch)*math.cos(roll) + math.sin(yaw)*math.sin(roll),
                     math.sin(yaw)*math.sin(pitch)*math.cos(roll) - math.cos(yaw)*math.sin(roll),
                     math.cos(pitch)*math.cos(roll))
    cache = UsdGeom.XformCache()
    for body, pos, rot in ((joint.body0, joint.localPose0Position, joint.localPose0Orientation),
                           (joint.body1, joint.localPose1Position, joint.localPose1Orientation)):
        world_pos, world_axis = _world_frame(stage, cache, body, pos, rot, axis)
        assert world_pos == pytest.approx((0.3, -0.2, 0.5), abs=2e-6)
        assert world_axis == pytest.approx(expected_axis, abs=2e-6)


MJCF_EQUALITIES = '''<mujoco><worldbody>
  <site name="anchor" pos=".2 .3 .4" quat=".9238795325 0 0 .3826834324"/>
  <body name="moving" pos="1 2 3"><freejoint/>
    <geom type="sphere" size=".2" mass="1"/>
    <site name="attachment" pos="-.1 .2 .4" quat=".9659258263 .2588190451 0 0"/>
  </body></worldbody><equality>
    <connect name="connect" site1="anchor" site2="attachment"/>
    <weld name="weld" site1="anchor" site2="attachment"/>
  </equality></mujoco>'''


@pytest.mark.parametrize('profile', ['newton', 'openusd'])
@pytest.mark.parametrize('layout', ['flat', 'nested'])
def test_mjcf_site_equality_frames(tmp_path, profile, layout):
    source = tmp_path / 'sites.xml'
    source.write_text(MJCF_EQUALITIES)
    output = tmp_path / f'{profile}-{layout}.usda'
    lightusd.import_mjcf(source, profile=profile, layout=layout).save(str(output))
    stage = Usd.Stage.Open(str(output))
    assert stage
    parsed = UsdPhysics.LoadUsdPhysicsFromRange(stage, ['/'])
    joints = {str(j.primPath).rsplit('/', 1)[-1]: j
              for kind, (_, descs) in parsed.items() if 'Joint' in str(kind) for j in descs}
    assert set(joints) == {'connect', 'weld'}
    cache = UsdGeom.XformCache()
    for name, joint in joints.items():
        assert joint.isValid and joint.jointEnabled and joint.excludeFromArticulation
        assert not joint.body0 and str(joint.body1).endswith('/moving')
        assert tuple(joint.localPose0Position) == pytest.approx((0.2, 0.3, 0.4), abs=1e-6)
        assert tuple(joint.localPose1Position) == pytest.approx((-0.1, 0.2, 0.4), abs=1e-6)
        world_pos, world_x = _world_frame(stage, cache, joint.body1, joint.localPose1Position,
                                          joint.localPose1Orientation, (1, 0, 0))
        assert world_pos == pytest.approx((0.9, 2.2, 3.4), abs=1e-6)
        assert world_x == pytest.approx((1, 0, 0), abs=1e-6)
        q = joint.localPose0Orientation
        anchor_y = Gf.Rotation(Gf.Quatd(q)).TransformDir(Gf.Vec3d(0, 1, 0))
        assert tuple(anchor_y) == pytest.approx((-math.sqrt(0.5), math.sqrt(0.5), 0), abs=1e-6)
        assert str(joint.type).endswith('SphericalJoint' if name == 'connect' else 'FixedJoint')
