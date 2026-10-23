# SPDX-License-Identifier: Apache-2.0
"""Physics-aware unit conversion (lightusd.units.convert_units)."""
import numpy as np
import pytest

import lightusd
from lightusd import units


def _robot():
    xml = b'''<mujoco><worldbody>
      <body name="a" pos="0 0 1"><freejoint/><inertial pos=".1 0 0" mass="2" diaginertia="1 2 3"/>
        <geom type="box" size=".1 .2 .3"/>
        <body name="b" pos=".5 0 0"><joint name="s" type="slide" axis="1 0 0" range="-.2 .3" damping="4" armature=".5"/>
          <inertial pos="0 0 0" mass="1" diaginertia="1 1 1"/><geom type="sphere" size=".05"/></body>
        <body name="c" pos="0 .5 0"><joint name="h" axis="0 0 1" range="-30 30" damping="2" armature=".1"/>
          <inertial pos="0 0 0" mass="1" diaginertia="1 1 1"/><geom type="sphere" size=".05"/></body>
      </body></worldbody></mujoco>'''
    return lightusd.load_bytes(xml, format='mjcf')


def test_meters_to_centimeters_and_grams():
    stage = _robot()
    a, s, h = stage.prim_at('/World/Links/a'), stage.prim_at('/World/Joints/s'), stage.prim_at('/World/Joints/h')
    before = {k: v for k, v in [('mass', a['physics:mass']), ('inertia', tuple(a['physics:diagonalInertia'])),
                                ('com', tuple(a['physics:centerOfMass'])), ('upper', s['physics:upperLimit']),
                                ('hupper', h['physics:upperLimit']), ('sarm', s['newton:armature']),
                                ('harm', h['newton:armature']), ('sdamp', s['newton:damping']),
                                ('pos', np.asarray(a.world_transform())[3, :3].copy())]}
    report = units.convert_units(stage, meters_per_unit=0.01, kilograms_per_unit=0.001)
    assert report.length_scale == pytest.approx(100) and report.mass_scale == pytest.approx(1000)
    assert stage.meters_per_unit == pytest.approx(0.01)
    assert stage.get_metadata('kilogramsPerUnit') == pytest.approx(0.001)
    assert a['physics:mass'] == pytest.approx(before['mass'] * 1000)
    assert tuple(a['physics:diagonalInertia']) == pytest.approx(tuple(v * 1000 * 100 ** 2 for v in before['inertia']))
    assert tuple(a['physics:centerOfMass']) == pytest.approx(tuple(v * 100 for v in before['com']))
    assert s['physics:upperLimit'] == pytest.approx(before['upper'] * 100)       # linear
    assert h['physics:upperLimit'] == pytest.approx(before['hupper'])           # degrees
    assert s['newton:armature'] == pytest.approx(before['sarm'] * 1000)         # kg
    assert h['newton:armature'] == pytest.approx(before['harm'] * 1000 * 1e4)   # kg m^2
    assert s['newton:damping'] == pytest.approx(before['sdamp'] * 1000)         # N s/m -> kg/s
    assert np.asarray(a.world_transform())[3, :3] == pytest.approx(before['pos'] * 100)
    box = next(p for p in stage if p.type_name == 'Cube')
    # size scales; the transform's scale part does not (half extents x 100).
    half = box['size'] / 2 * np.abs(np.asarray(box.local_transform())[:3, :3]).sum(axis=0)
    assert half == pytest.approx([10, 20, 30])
    scene = stage.prim_at('/World/PhysicsScene')
    assert scene['physics:gravityMagnitude'] == pytest.approx(9.80665 * 100, rel=1e-5)
    assert not report.skipped


def test_up_axis_rotates_roots_and_gravity():
    stage = lightusd.loads('''#usda 1.0
( upAxis = "Y" metersPerUnit = 1 )
def PhysicsScene "Scene" { vector3f physics:gravityDirection = (0, -1, 0) }
def Xform "Robot" { double3 xformOp:translate = (0, 1, 0)  uniform token[] xformOpOrder = ["xformOp:translate"] }
''')
    report = units.convert_units(stage, up_axis='Z')
    assert report.rotated_roots == 2 and stage.up_axis == 'Z'
    assert tuple(stage.prim_at('/Scene')['physics:gravityDirection']) == pytest.approx((0, 0, -1))
    robot = np.asarray(stage.prim_at('/Robot').world_transform())
    assert robot[3, :3] == pytest.approx([0, 0, 1], abs=1e-9)


def test_up_axis_round_trip_removes_the_rotation():
    stage = lightusd.loads('''#usda 1.0
( upAxis = "Z" metersPerUnit = 1 )
def Xform "Robot" { double3 xformOp:translate = (1, 2, 3)  uniform token[] xformOpOrder = ["xformOp:translate"] }
''')
    units.convert_units(stage, up_axis='Y')
    assert list(stage.prim_at('/Robot')['xformOpOrder'])[0] == 'xformOp:rotateX:upAxis'
    units.convert_units(stage, up_axis='Z')
    robot = stage.prim_at('/Robot')
    assert list(robot['xformOpOrder']) == ['xformOp:translate']
    assert 'xformOp:rotateX:upAxis' not in robot.attributes
