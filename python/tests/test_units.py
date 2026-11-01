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
    assert scene['physics:gravityMagnitude'] == pytest.approx(9.81 * 100, rel=1e-5)  # MuJoCo's default gravity
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


def test_time_samples_scale_once_and_keep_declared_types():
    stage = lightusd.loads('''#usda 1.0
( upAxis = "Z" metersPerUnit = 1 )
def Xform "R" {
  def Sphere "s" { double radius.timeSamples = { 0: 1, 10: 2 } }
  def Mesh "m" { half3[] points = [(1, 2, 3)] }
  def PhysicsJoint "j" { float newton:armature = 1 }
}
''')
    report = units.convert_units(stage, meters_per_unit=0.001)
    sphere = stage.prim_at('/R/s')
    assert dict(sphere.attribute('radius').timesamples) == pytest.approx({0: 1000, 10: 2000})
    assert 'double radius =' not in stage.export_usda()  # no default is authored
    mesh = stage.prim_at('/R/m')
    assert mesh.attribute('points').type_name == 'half3[]'
    assert np.asarray(mesh['points']).ravel() == pytest.approx([1000, 2000, 3000], rel=1e-3)
    assert report.skipped == ['/R/j.newton:armature']
    with pytest.raises(ValueError):
        units.convert_units(stage, meters_per_unit=-1)


def test_sdf_break_distance_and_vdb_field_scale():
    stage = lightusd.loads('''#usda 1.0
( metersPerUnit = 1 kilogramsPerUnit = 1 )
def Mesh "M" (prepend apiSchemas = ["NewtonSDFCollisionAPI"]) {
    uniform float newton:sdfNarrowBandInner = -0.1
    uniform float newton:sdfNarrowBandOuter = 0.2
}
def PhysicsDistanceJoint "J" {
    float physics:minDistance = 0.5
    float physics:maxDistance = 2
    float physics:breakForce = 10
    float physics:breakTorque = 3
}
def Volume "V" {
    rel field:sdf = </V/sdf>
    def OpenVDBAsset "sdf" {
        asset filePath = @sdf.vdb@
        double3 xformOp:translate = (1, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
''')
    units.convert_units(stage, meters_per_unit=0.01, kilograms_per_unit=0.001)
    m, j, f = stage.prim_at('/M'), stage.prim_at('/J'), stage.prim_at('/V/sdf')
    assert m['newton:sdfNarrowBandInner'] == pytest.approx(-10)
    assert m['newton:sdfNarrowBandOuter'] == pytest.approx(20)
    assert j['physics:minDistance'] == pytest.approx(50)
    assert j['physics:maxDistance'] == pytest.approx(200)
    assert j['physics:breakForce'] == pytest.approx(10 * 1000 * 100)       # kg m
    assert j['physics:breakTorque'] == pytest.approx(3 * 1000 * 100 ** 2)  # kg m^2
    # The .vdb stays in metres: an innermost scale maps it to centimetres.
    assert list(f['xformOpOrder']) == ['xformOp:translate', 'xformOp:scale:units']
    assert tuple(f['xformOp:scale:units']) == pytest.approx((100, 100, 100))
    assert tuple(f['xformOp:translate']) == pytest.approx((100, 0, 0))
    # Converting back removes it.
    units.convert_units(stage, meters_per_unit=1, kilograms_per_unit=1)
    f = stage.prim_at('/V/sdf')
    assert list(f['xformOpOrder']) == ['xformOp:translate']
    assert 'xformOp:scale:units' not in f.attributes
