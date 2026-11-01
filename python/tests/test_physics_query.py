# SPDX-License-Identifier: Apache-2.0
import gc
import json
import math
from pathlib import Path

import numpy as np
import pytest
import lightusd
from lightusd import physics

SOURCE = '''#usda 1.0
def Xform "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsCollisionAPI"]) {
    float physics:mass = 7
    float physics:mass.timeSamples = { 0: 2, 2: 4 }
    uniform int mjc:group = 3
    double[] mjc:qpos = [1, 2, 3]
    token[] newton:labels = ["a", "b"]
    string newton:future = "preserved"
    float physics:friction = None
    rel physics:filteredPairs = [</Other>]
}
def Xform "Other" {}
def PhysicsCollisionGroup "Filter" {
    rel physics:filteredGroups = [</Other>]
}
def MjcKeyframe "State" {
    double[] mjc:qvel = [4, 5]
}
'''


def test_native_typed_snapshot_and_group_overlap():
    stage = lightusd.loads(SOURCE)
    data = physics.inspect(stage, time=1)
    body = data.rigid_bodies[0]
    assert data.colliders[0] is body
    assert body.attributes["physics:mass"] == 3
    assert body.attributes["mjc:qpos"] == (1, 2, 3)
    assert body.attributes["newton:labels"] == ("a", "b")
    assert body.attributes["newton:future"] == "preserved"
    assert body.property_types["mjc:qpos"] == "double[]"
    assert body.property_types["physics:mass"] == "float"
    assert "mjc:group" in body.uniform_properties
    assert "physics:friction" in body.blocked_properties
    assert body.attributes["physics:friction"] is lightusd.ValueBlock
    assert body.relationships["physics:filteredPairs"] == ("/Other",)
    assert data.collision_groups[0].prim_path == "/Filter"
    assert data.keyframes[0].attributes["mjc:qvel"] == (4, 5)
    assert physics.inspect(stage, time=None).rigid_bodies[0].attributes["physics:mass"] == 7
    with pytest.raises(TypeError):
        body.attributes["physics:mass"] = 6
    with pytest.raises(TypeError):
        body.property_types["mjc:qpos"] = "float[]"


def test_owned_array_views_survive_edits_and_close():
    stage = lightusd.loads(SOURCE)
    snapshot = physics.inspect(stage, array_views=True)
    array = snapshot.rigid_bodies[0].attributes["mjc:qpos"]
    assert isinstance(array, lightusd.Array)
    view = np.asarray(array)
    assert view.dtype == np.float64 and not view.flags.writeable
    stage.prim_at("/Body").set("mjc:qpos", [9, 8], type="double[]")
    stage.close()
    del stage, snapshot, array
    gc.collect()
    np.testing.assert_array_equal(view, [1, 2, 3])


def test_large_snapshot_budget_and_record_limit():
    stage = lightusd.loads('#usda 1.0\ndef MjcKeyframe "State" {}')
    stage.prim_at("/State").set("mjc:qpos", np.arange(200000, dtype=np.float64), type="double[]")
    with pytest.raises(lightusd.UsdError, match="budget"):
        physics.inspect(stage, array_views=True, max_memory=1024**2)
    data = physics.inspect(stage, array_views=True, max_memory=4 * 1024**2)
    assert len(data.keyframes[0].attributes["mjc:qpos"]) == 200000
    with pytest.raises(lightusd.UsdError, match="record limit"):
        physics.inspect(lightusd.loads(SOURCE), max_records=1)


def test_snapshot_empty_declarations_and_dictionary():
    stage = lightusd.loads('''#usda 1.0
def MjcKeyframe "State" {
    double mjc:unset
    dictionary newton:custom = { string name = "value" int[] data = [1, 2] }
}
''')
    data = physics.inspect(stage)
    assert data.keyframes[0].attributes["mjc:unset"] is None
    assert data.keyframes[0].attributes["newton:custom"]["name"] == "value"
    assert data.keyframes[0].attributes["newton:custom"]["data"] == (1, 2)


@pytest.mark.parametrize("options", [dict(time=math.nan), dict(time=math.inf),
    dict(max_memory=-1), dict(max_records=0)])
def test_invalid_snapshot_options(options):
    with pytest.raises((ValueError, OverflowError)):
        physics.inspect(lightusd.loads(SOURCE), **options)


def test_snapshot_rejects_closed_or_nonstage():
    stage = lightusd.loads(SOURCE)
    stage.close()
    with pytest.raises(lightusd.UsdError, match="closed"):
        physics.inspect(stage)
    with pytest.raises(TypeError, match="Stage"):
        physics.inspect(object())


def test_generated_standard_schema_descriptors_and_variability():
    definitions = physics.schema_definitions()
    drive = definitions["PhysicsDriveAPI"]
    assert drive["kind"] == "multipleApply" and drive["property_namespace"] == "drive"
    assert drive["properties"]["physics:type"]["allowed_tokens"] == ("force", "acceleration")
    assert definitions["PhysicsRigidBodyAPI"]["properties"]["physics:startsAsleep"]["variability"] == "uniform"
    with pytest.raises(TypeError):
        drive["properties"]["physics:type"]["type"] = "string"
    stage = lightusd.loads('#usda 1.0\ndef Xform "Body" {}')
    physics.apply_schema(stage.prim_at("/Body"), "PhysicsRigidBodyAPI", attributes={"physics:startsAsleep": True})
    assert stage.prim_at("/Body").attribute("physics:startsAsleep").is_uniform
    joint = physics.define_prim(stage, "/Joint", "PhysicsRevoluteJoint", attributes={"physics:axis": "Z", "physics:breakForce": 20})
    physics.apply_schema(joint, "PhysicsDriveAPI:angular", attributes={
        "drive:angular:physics:type": "force", "drive:angular:physics:stiffness": 3})
    assert joint.attribute("drive:angular:physics:type").is_uniform
    assert not joint.attribute("drive:angular:physics:stiffness").is_uniform
    assert joint.attribute("drive:angular:physics:stiffness").type_name == "float"
    physics.apply_schema(joint, "PhysicsLimitAPI:rotX", attributes={"limit:rotX:physics:low": -3})
    assert joint["limit:rotX:physics:low"] == -3
    record = physics.inspect(stage).joints[0]
    assert record.attributes["drive:angular:physics:stiffness"] == 3
    assert record.attributes["limit:rotX:physics:low"] == -3


def test_schema_descriptor_provenance_matches_generated_manifests():
    root = Path(__file__).resolve().parents[2]
    engines = json.loads((root / "doc/generated/engine-physics-schemas.json").read_text())["engines"]
    openusd = json.loads((root / "doc/generated/openusd-schema-26.08.json").read_text())["openusd"]
    definitions = physics.schema_definitions()
    for name, project, expected in (
        ("PhysicsRigidBodyAPI", "openusd", openusd),
        ("MjcActuator", "mujoco", engines["mujoco"]),
        ("NewtonActuator", "newton-usd-schemas", engines["newton-usd-schemas"]),
    ):
        provenance = definitions[name]["provenance"]
        assert provenance == {"project": project, **expected}
        with pytest.raises(TypeError):
            provenance["version"] = "changed"
    for name, definition in definitions.items():
        if name.startswith("Mjc"):
            project, expected = "mujoco", engines["mujoco"]
        elif name.startswith("Newton"):
            project, expected = "newton-usd-schemas", engines["newton-usd-schemas"]
        else:
            project, expected = "openusd", openusd
        assert definition["provenance"] == {"project": project, **expected}


@pytest.mark.parametrize("schema", ["PhysicsDriveAPI", "PhysicsLimitAPI:", "PhysicsMassAPI:invalid"])
def test_schema_instance_rules(schema):
    with pytest.raises(ValueError, match="instance"):
        physics.apply_schema(lightusd.loads('#usda 1.0\ndef Xform "Body" {}').prim_at("/Body"), schema)
