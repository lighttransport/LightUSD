# SPDX-License-Identifier: Apache-2.0
from dataclasses import FrozenInstanceError

import pytest
import lightusd
from lightusd import physics

VALID = '''#usda 1.0
( defaultPrim = "Body" upAxis = "Z" metersPerUnit = 1 )
def Xform "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]) {
    float physics:mass = 1
    float3 physics:diagonalInertia = (1, 2, 3)
    quatf physics:principalAxes = (1, 0, 0, 0)
    uniform bool physics:startsAsleep = false
}
'''


def test_valid_report_and_checked_groups():
    report = physics.validate(lightusd.loads(VALID))
    assert report.valid and bool(report) and report.complete
    assert report.error_count == report.omitted_issues == 0
    assert report.checked_groups == ("core", "physics")
    physics_only = physics.validate(lightusd.loads(VALID), include_core=False)
    assert physics_only.checked_groups == ("physics",)
    assert physics_only.valid
    with pytest.raises(FrozenInstanceError):
        report.valid = False


def test_invalid_physics_report_is_owned_and_has_native_rules():
    stage = lightusd.loads(VALID.replace('physics:mass = 1', 'physics:mass = -1') + '''
def PhysicsRevoluteJoint "Joint" { rel physics:body1 = </Missing> }
''')
    before = stage.export_usda()
    report = physics.validate(stage)
    assert stage.export_usda() == before
    stage.close()
    assert not report.valid and not bool(report) and report.complete
    assert report.error_count > 0
    assert any(i.rule_id == "physics.mass" and i.location == "/Body" for i in report.issues)
    assert any(i.rule_id == "physics.joint.bodyTarget" and "Missing" in i.message for i in report.issues)
    assert all(i.time is None for i in report.issues)
    assert report.error_count == sum(i.severity == "error" for i in report.issues)
    with pytest.raises(FrozenInstanceError):
        report.issues[0].message = "changed"


def test_sampled_physics_uses_coherent_values_and_times():
    text = VALID.replace('float physics:mass = 1', '''float physics:mass = 1
    float physics:mass.timeSamples = { 0: 2, 2: -3 }''')
    stage = lightusd.loads(text)
    assert physics.validate(stage).valid
    report = physics.validate(stage, samples=True)
    assert not report.valid
    assert any(i.rule_id == "physics.mass" and i.time == 2 for i in report.issues)
    assert not any(i.rule_id == "physics.mass" and i.time == 0 for i in report.issues)
    assert stage.prim_at("/Body").attribute("physics:startsAsleep").is_uniform
    assert not any("variability" in i.rule_id for i in report.issues)


def test_animated_mass_without_default_is_checked():
    stage = lightusd.loads(VALID.replace('float physics:mass = 1',
        'float physics:mass.timeSamples = { 0: 2, 2: -3 }'))
    report = physics.validate(stage, samples=True)
    assert not report.valid
    assert any(i.rule_id == "physics.mass" and i.time == 2 for i in report.issues)


def test_validation_sample_limit_reports_incomplete():
    stage = lightusd.loads(VALID.replace('float physics:mass = 1', '''float physics:mass = 1
    float physics:mass.timeSamples = { 0: 1, 1: 2, 2: 3 }'''))
    report = physics.validate(stage, samples=True, max_samples=2)
    assert not report.complete and not report.valid
    assert any(i.rule_id == "checker.coverage.samples" for i in report.issues)


def test_report_limits_cannot_turn_failure_into_success():
    source = '#usda 1.0\n' + ''.join(
        f'def Xform "B{i}" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]) {{ float physics:mass = -1 }}\n'
        for i in range(2000))
    stage = lightusd.loads(source)
    report = physics.validate(stage, include_core=False, max_issues=3)
    assert len(report.issues) == 3 and report.omitted_issues > 0
    assert not report.complete and not report.valid
    tiny = physics.validate(stage, max_report_bytes=1)
    assert not tiny.issues and tiny.omitted_issues > 0
    assert tiny.error_count == 0 and not tiny.valid and not tiny.complete


def test_missing_schema_coverage_can_be_required():
    stage = lightusd.loads('#usda 1.0\ndef Scope "P" (prepend apiSchemas = ["MissingPhysicsDefinitionAPI"]) {}')
    report = physics.validate(stage, require_complete=True)
    assert not report.complete and not report.valid
    assert any(i.rule_id == "checker.coverage.schema" for i in report.issues)


@pytest.mark.parametrize("limits", [dict(max_issues=0), dict(max_report_bytes=0), dict(max_samples=-1)])
def test_invalid_limits(limits):
    with pytest.raises(ValueError, match="positive"):
        physics.validate(lightusd.loads(VALID), **limits)


def test_invalid_stage():
    with pytest.raises(TypeError, match="Stage"):
        physics.validate(object())
    stage = lightusd.loads(VALID)
    stage.close()
    with pytest.raises(lightusd.UsdError, match="closed"):
        physics.validate(stage)


@pytest.mark.parametrize("attributes", [
    'quatf physics:principalAxes = (1, 0, 0, 0)',
    'float3 physics:diagonalInertia = (1, 2, 3)',
    'float3 physics:diagonalInertia = (0, 0, 0)\nquatf physics:principalAxes = (1, 0, 0, 0)',
    'float3 physics:diagonalInertia = (1, 2, 3)\nquatf physics:principalAxes = (0, 0, 0, 0)',
])
def test_independent_inertia_fallbacks_are_valid(attributes):
    stage = lightusd.loads('#usda 1.0\ndef Xform "Body" (prepend apiSchemas = ["PhysicsMassAPI", "PhysicsRigidBodyAPI"]) {\n' + attributes + '\n}')
    report = physics.validate(stage, include_core=False)
    assert report.valid, report.issues


@pytest.mark.parametrize("attribute", [
    'float3 physics:diagonalInertia = (-1, 2, 3)',
    'quatf physics:principalAxes = (2, 0, 0, 0)',
])
def test_invalid_inertia_still_fails(attribute):
    stage = lightusd.loads('#usda 1.0\ndef Xform "Body" (prepend apiSchemas = ["PhysicsMassAPI", "PhysicsRigidBodyAPI"]) {\n' + attribute + '\n}')
    assert not physics.validate(stage, include_core=False).valid


def test_joint_quaternion_does_not_use_mass_fallback():
    stage = lightusd.loads('#usda 1.0\ndef PhysicsRevoluteJoint "Joint" { quatf physics:localRot0 = (0, 0, 0, 0) }')
    assert not physics.validate(stage, include_core=False).valid


@pytest.mark.parametrize("integrator", ["Euler", "RK4"])
def test_mjcf_solver_defaults_tokens_and_zero_tolerance(integrator):
    source = b'''<mujoco><option solver="Newton" integrator="Euler" noslip_tolerance="0"/>
    <default><geom solimp=".8 .9 .02 .4 3" solref=".03 .8"/>
      <default class="child"><geom solimp=".7 .85 .01" solref=".05"/></default></default>
    <worldbody><body name="body"><joint name="j"/><geom name="shape" class="child" type="sphere" size=".2" solimp=".6 .8"/></body></worldbody>
    <sensor><jointactuatorfrc name="force" joint="j"/></sensor></mujoco>'''
    source = source.replace(b'Euler', integrator.encode())
    stage = lightusd.load_bytes(source, format="mjcf")
    data = physics.inspect(stage)
    collider = data.colliders[0]
    assert collider.attributes["mjc:solimp"] == pytest.approx((.6, .8, .01, .4, 3))
    assert collider.attributes["mjc:solref"] == pytest.approx((.05, .8))
    assert data.scenes[0].attributes["mjc:option:solver"] == "newton"
    assert data.scenes[0].attributes["mjc:option:integrator"] == integrator.lower()
    assert stage.import_report()["counts"]["completed_solver_arrays"] > 0
    assert any(d["code"] == "mjcf.token_canonicalized" for d in stage.import_report()["diagnostics"])
    assert physics.validate(stage).valid
    mujoco = pytest.importorskip("mujoco")
    model = mujoco.MjModel.from_xml_string(source.decode())
    assert collider.attributes["mjc:solimp"] == pytest.approx(model.geom_solimp[0])
    assert collider.attributes["mjc:solref"] == pytest.approx(model.geom_solref[0])


def test_ellipsoid_collision_policy_and_mesh_profile():
    source = b'<mujoco><worldbody><body name="b"><geom type="ellipsoid" size=".1 .2 .3"/></body></worldbody></mujoco>'
    stage = lightusd.load_bytes(source, format="mjcf")
    assert any(d["code"] == "mjcf.ellipsoid_collision_approximated" for d in stage.import_report()["diagnostics"])
    assert not physics.validate(stage).valid
    mesh = lightusd.load_bytes(source, format="mjcf", tessellate_collision_shapes=True)
    assert physics.validate(mesh).valid
    assert physics.inspect(mesh).colliders[0].type_name == "Mesh"
    with pytest.raises(lightusd.UsdError, match="Ellipsoid collider"):
        lightusd.load_bytes(source, format="mjcf", unsupported_policy="error")


def test_oversized_solver_array_is_rejected():
    with pytest.raises(lightusd.UsdError, match="Too many MJCF solver values"):
        lightusd.load_bytes(b'<mujoco><worldbody><body><geom type="sphere" size=".1" solref="1 2 3"/></body></worldbody></mujoco>', format="mjcf")


JOINT_LIMITS = VALID + '''
def Xform "Base" {}
def PhysicsJoint "D6" (prepend apiSchemas = ["PhysicsLimitAPI:rotX", "PhysicsLimitAPI:transY"]) {
    rel physics:body0 = </Base>
    rel physics:body1 = </Body>
    float limit:rotX:physics:low = %s
    float limit:rotX:physics:high = 30
    float limit:transY:physics:low = 1
    float limit:transY:physics:high = -1
}
def PhysicsScene "Scene" { vector3f physics:gravityDirection = (0, 0, 0) }
'''


def test_zero_gravity_direction_means_negative_up_axis():
    report = physics.validate(lightusd.loads(JOINT_LIMITS % "-30"))
    assert not any(i.rule_id == "physics.scene.gravity" for i in report.issues)


def test_standard_limit_names_are_validated():
    report = physics.validate(lightusd.loads(JOINT_LIMITS % "-inf"))
    # -inf is the unlimited fallback; low > high is a locked transY axis.
    assert report.valid, [(i.rule_id, i.message) for i in report.issues if i.severity == "error"]
    assert not [i for i in report.issues if i.rule_id == "physics.limit.range"]
    nan = physics.validate(lightusd.loads(JOINT_LIMITS % "nan"))
    assert any(i.rule_id == "physics.limit.range" and i.severity == "error" for i in nan.issues)


def test_historical_limit_names_are_flagged():
    text = VALID + '''
def PhysicsRevoluteJoint "Hinge" (prepend apiSchemas = ["PhysicsLimitAPI:rotX"]) {
    rel physics:body1 = </Body>
    float physics:limit:rotX:low = -1
    float physics:limit:rotX:high = 1
}
'''
    report = physics.validate(lightusd.loads(text))
    flagged = [i for i in report.issues if i.rule_id == "physics.property.nonStandardName"]
    assert len(flagged) == 2 and all(i.severity == "warning" for i in flagged)


@pytest.mark.parametrize("profile", ["newton", "openusd"])
def test_profile_outputs_validate_without_schema_mismatches(profile):
    xml = b'''<mujoco><worldbody><body name="a" pos="0 0 1"><freejoint/>
      <geom type="box" size=".1 .1 .1" density="300"/>
      <body name="b" pos="0 0 -.3"><joint name="j" type="hinge" axis="0 1 0" range="-30 45"
        damping="2" armature=".1" frictionloss=".3" ref="5" springref="10" stiffness="4"/>
      <joint name="k" type="slide" axis="1 0 0" range="-.1 .1"/>
      <geom type="capsule" size=".05 .2" friction=".7 .01 .001"/><site name="s" pos="0 0 .1"/>
      </body></body></worldbody>
      <tendon><fixed name="t" limited="false" range="0 1"><joint joint="j" coef="1"/></fixed></tendon>
      <actuator><motor name="m" joint="j" gear="2"/></actuator></mujoco>'''
    stage = lightusd._import(data=xml, format="mjcf", profile=profile)
    report = physics.validate(stage)
    bad = [(i.rule_id, i.location, i.message) for i in report.issues
           if i.severity == "error" or i.rule_id.startswith("core.schema")
           or i.rule_id == "physics.property.nonStandardName"]
    assert not bad, bad


def test_schema_variability_mismatch_is_reported():
    # Historical output authored joint frames/limits as uniform; the generated
    # UsdPhysics descriptors declare them varying.
    xml = b'''<mujoco><worldbody><body name="a"><freejoint/><geom type="sphere" size=".1"/>
      <body name="b"><joint name="j" range="-30 45"/><geom type="sphere" size=".1"/></body>
      </body></worldbody></mujoco>'''
    legacy = physics.validate(lightusd._import(data=xml, format="mjcf", profile="legacy"))
    mismatched = {i.location.rsplit(".", 1)[-1] for i in legacy.issues
                  if i.rule_id == "core.schema.variability"}
    assert {"physics:localPos0", "physics:localRot1", "physics:lowerLimit"} <= mismatched
    current = physics.validate(lightusd._import(data=xml, format="mjcf"))
    assert not any(i.rule_id == "core.schema.variability" for i in current.issues)


NEWTON_BODY = '''#usda 1.0
( defaultPrim = "Body" upAxis = "Z" metersPerUnit = 1 )
def Xform "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "NewtonMassAPI"]) {
    float physics:mass = 1
    float3 physics:diagonalInertia = (%s)
    double[] newton:inertia = [%s]
}
def PhysicsRevoluteJoint "J" (prepend apiSchemas = ["NewtonJointAPI"]) {
    float newton:limitStiffness = %s
    float newton:armature = %s
}
'''


@pytest.mark.parametrize('diag,tensor,stiffness,armature,rule,severity', [
    ('1, 2, 3', '1, 2, 3, 0, 0, 0', '-inf', '0', None, None),
    ('1, 2, 3', '1, 2, 3, 0, 0', '1', '0', 'physics.extension.newton.inertia', 'error'),
    ('1, 1, 1', '1, 1, 1, 2, 0, 0', '1', '0', 'physics.extension.newton.inertia', 'error'),
    ('1, 1, 3', '1, 1, 3, 0, 0, 0', '1', '0', 'physics.extension.newton.inertia', 'warning'),
    ('1, 2, 2', '1, 2, 3, 0, 0, 0', '1', '0', 'physics.extension.newton.inertia', 'warning'),
    ('1, 2, 3', '1, 2, 3, 0, 0, 0', '-1', '0', 'physics.extension.newton.range', 'error'),
    ('1, 2, 3', '1, 2, 3, 0, 0, 0', '1', '-1', 'physics.extension.newton.range', 'error'),
])
def test_newton_mass_and_joint_rules(diag, tensor, stiffness, armature, rule, severity):
    report = physics.validate(lightusd.loads(NEWTON_BODY % (diag, tensor, stiffness, armature)))
    newton = [i for i in report.issues if i.rule_id.startswith('physics.extension.newton')]
    if rule is None:
        assert not newton, newton
    else:
        assert any(i.rule_id == rule and i.severity == severity for i in newton), report.issues
