# SPDX-License-Identifier: Apache-2.0
"""Conversion fidelity for the physics parameters the consumer contracts depend on.

The bounded-dynamics acceptance matrix has three failing contracts — Newton
Featherstone's passive spring, MuJoCo's default soft-contact penetration and
Newton XPBD contact settling. All three fail on the *source MJCF* path as well as
the converted USD path, so they are consumer-solver behaviours, not conversion
losses. These tests lock the half that LightUSD actually controls: the exact
spring and contact parameters must survive ``import_mjcf(profile="newton")`` and
a USDC round-trip unchanged.

Values mirror ``tools/view_control/jobs/physics-dynamics.json`` (slide spring
stiffness 8, springref 0) and the frictionless-contact case (zero gap).
"""
import pytest

import lightusd
from lightusd import physics

SPRING = '''<mujoco><worldbody>
 <body name="body" pos="0 0 0">
  <joint name="joint" type="slide" axis="1 0 0" stiffness="8" springref="0.25" damping="0.5"/>
  <inertial mass="2" diaginertia=".02 .02 .02"/>
  <geom name="shape" type="sphere" size=".1"/>
 </body>
</worldbody></mujoco>'''

CONTACT = '''<mujoco><worldbody>
 <geom name="floor" type="plane" size="5 5 .1"/>
 <body name="body" pos="0 0 2">
  <freejoint name="root"/><inertial mass="2" diaginertia=".008 .008 .008"/>
  <geom name="shape" type="sphere" size=".1" mass="2"%s/>
 </body>
</worldbody></mujoco>'''


def _reload(stage):
    return lightusd.load_bytes(stage.export_usdc())


def test_passive_spring_parameters_survive_roundtrip():
    stage = lightusd.load_bytes(SPRING.encode(), format='mjcf')
    (joint,) = [j for j in physics.inspect(stage).joints
                if j.prim_path.endswith('/joint')]
    assert joint.type_name == 'PhysicsPrismaticJoint'

    reloaded = _reload(stage)
    prim = reloaded.prim_at('/World/Joints/joint')
    # The newton profile carries the MJCF spring as Mjc/Newton attributes; the
    # consumer reads these instead of a UsdPhysics drive.
    assert prim['mjc:stiffness'] == pytest.approx(8.0)
    assert prim['mjc:springref'] == pytest.approx(0.25)
    assert prim['newton:damping'] == pytest.approx(0.5)
    reloaded.close()
    stage.close()


def test_default_contact_gap_is_explicit_zero():
    # MJCF's default gap is zero. It must be authored explicitly: a consumer
    # that leaves the attribute unset falls back to its own default (Newton's
    # MJCF importer uses 0.1), which is exactly the discrepancy the dynamics
    # matrix reports for its MJCF control.
    stage = lightusd.load_bytes((CONTACT % '').encode(), format='mjcf')
    reloaded = _reload(stage)
    for path in ('/World/Links/body/shape', '/World/Links/__world_geoms/floor'):
        prim = reloaded.prim_at(path)
        assert prim['newton:contactGap'] == pytest.approx(0.0)
        assert prim['newton:contactMargin'] == pytest.approx(0.0)
    reloaded.close()
    stage.close()


def test_explicit_solref_and_zero_gap_survive_roundtrip():
    stage = lightusd.load_bytes(
        (CONTACT % ' gap="0" solref="0.005 1"').encode(), format='mjcf')
    reloaded = _reload(stage)
    prim = reloaded.prim_at('/World/Links/body/shape')
    assert tuple(prim['mjc:solref']) == pytest.approx((0.005, 1.0))
    assert prim['newton:contactGap'] == pytest.approx(0.0)
    reloaded.close()
    stage.close()
