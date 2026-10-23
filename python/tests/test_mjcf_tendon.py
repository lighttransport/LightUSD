# SPDX-License-Identifier: Apache-2.0
"""MJCF tendons as MjcTendon prims (MuJoCo USD decoder / Newton contract)."""
import pytest

import lightusd

SPATIAL = b'''<mujoco><worldbody>
  <body name="a" pos="0 0 1"><freejoint/><geom type="sphere" size=".1"/>
    <site name="s0" pos="0 0 .1"/>
    <geom name="wrap" type="cylinder" size=".05 .02" pos=".2 0 0" contype="0" conaffinity="0"/>
    <site name="side" pos=".2 .1 0"/></body>
  <body name="b" pos=".5 0 1"><joint name="hb" axis="0 1 0"/><geom type="sphere" size=".1"/>
    <site name="s1" pos="0 0 .1"/><site name="s2" pos="0 .1 .1"/></body>
</worldbody>
<tendon>
  <spatial name="t" width=".01" springlength=".3" group="2" armature=".1" actuatorfrcrange="-5 5">
    <site site="s0"/><geom geom="wrap" sidesite="side"/><site site="s1"/>
    <pulley divisor="2"/><site site="s1"/><site site="s2"/>
  </spatial>
  <spatial name="lead"><pulley divisor="3"/><site site="s0"/><site site="s1"/></spatial>
  <fixed name="f"><joint joint="hb" coef="2"/></fixed>
</tendon>
<actuator><motor tendon="t"/></actuator></mujoco>'''


def test_spatial_tendon_path_encoding():
    stage = lightusd.load_bytes(SPATIAL, format='mjcf')
    t = stage.prim_at('/World/Tendons/t')
    assert t.type_name == 'MjcTendon'
    assert t['mjc:type'] == 'spatial'
    path = t.relationship('mjc:path').targets
    # The wrap geom stays a native, non-colliding cylinder under its body.
    wrap = stage.prim_at(path[1])
    assert path[1].startswith('/World/Links/a/') and wrap.type_name == 'Cylinder'
    assert 'PhysicsCollisionAPI' not in (wrap.metadata('apiSchemas') or ())
    assert [p.rsplit('/', 1)[1] for p in path] == ['s0', 'wrap', 's1', 's2']
    assert list(t['mjc:path:indices']) == [0, 1, 2, 2, 3]
    assert list(t['mjc:path:segments']) == [0, 0, 0, 1, 1]
    assert list(t['mjc:path:divisors']) == [1, 2]
    assert t.relationship('mjc:sideSites').targets[0].endswith('/side')
    assert list(t['mjc:sideSites:indices']) == [-1, 0, -1, -1, -1]
    assert 'mjc:path:coef' not in t.attributes
    assert list(t['mjc:springlength']) == [.3, .3]
    assert t['mjc:group'] == 2 and t['mjc:armature'] == .1
    assert t['mjc:actuatorfrclimited'] == 'true'
    assert (t['mjc:actuatorfrcrange:min'], t['mjc:actuatorfrcrange:max']) == (-5, 5)

    lead = stage.prim_at('/World/Tendons/lead')
    assert list(lead['mjc:path:segments']) == [1, 1]
    assert list(lead['mjc:path:divisors']) == [1, 3]

    fixed = stage.prim_at('/World/Tendons/f')
    assert fixed['mjc:type'] == 'fixed' and list(fixed['mjc:path:coef']) == [2]
    act = next(p for p in stage if p.type_name == 'MjcActuator')
    assert act.relationship('mjc:target').targets == ('/World/Tendons/t',)


def test_unresolved_wrap_geom_skips_tendon():
    xml = SPATIAL.replace(b'type="cylinder"', b'type="box"')
    stage = lightusd.load_bytes(xml, format='mjcf')
    with pytest.raises(KeyError):
        stage.prim_at('/World/Tendons/t')
    assert 'must be a sphere or cylinder' in stage.warnings()


REPLICATE = b'''<mujoco><worldbody>
  <replicate count="3" offset=".5 0 0" euler="0 0 90" sep="-">
    <site name="anchor" pos="0 0 1"/>
    <body name="ball" pos="0 0 .5"><freejoint/><geom size=".1"/><site name="tip"/></body>
  </replicate>
</worldbody>
<tendon><spatial name="rope"><site site="anchor"/><site site="tip"/></spatial></tendon></mujoco>'''


def test_replicate_names_frames_and_referencing_tendons():
    stage = lightusd.load_bytes(REPLICATE, format='mjcf')
    balls = [stage.prim_at(f'/World/Links/ball_{i}') for i in range(3)]
    pos = [tuple(round(v, 6) for v in b.world_transform()[3][:3]) for b in balls]
    # Offsets accumulate in the rotated frame of the previous copy.
    assert pos == [(0, 0, .5), (.5, 0, .5), (.5, .5, .5)]
    ropes = sorted(p.name for p in stage if p.type_name == 'MjcTendon')
    assert ropes == ['rope_0', 'rope_1', 'rope_2']
    targets = stage.prim_at('/World/Tendons/rope_2').relationship('mjc:path').targets
    assert targets[1] == '/World/Links/ball_2/tip_2'
    mujoco = pytest.importorskip('mujoco')
    model = mujoco.MjModel.from_xml_string(REPLICATE.decode())
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    assert [tuple(round(v, 6) for v in data.xpos[model.body(f'ball-{i}').id]) for i in range(3)] == pos
