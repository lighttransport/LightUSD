"""UsdPhysics import/export plus preservation of common simulator extensions."""

import json


def _schemas(prim):
    value = prim.metadata("apiSchemas")
    return tuple(value or ())


def import_physics(stage, objects):
    import bpy
    for prim in stage:
        schemas = _schemas(prim)
        if not schemas and not any(n.startswith(("physics:", "mjc:", "newton:")) for n in prim.attributes):
            continue
        obj = objects.get(prim.path)
        if obj is None:
            continue
        if any("RigidBodyAPI" in s for s in schemas) or prim.get("physics:rigidBodyEnabled"):
            if obj.rigid_body is None:
                bpy.context.view_layer.objects.active = obj
                obj.select_set(True)
                bpy.ops.rigidbody.object_add()
                obj.select_set(False)
            obj.rigid_body.kinematic = bool(prim.get("physics:kinematic", False))
            if prim.get("physics:mass") is not None:
                obj.rigid_body.mass = float(prim.get("physics:mass"))
        obj["lightusd:physics"] = json.dumps({"schemas": schemas, "attributes": {
            name: prim.get(name) for name in prim.attributes
            if name.startswith(("mjc:", "newton:", "physics:"))
        }}, default=str)


def export_physics(stage, objects):
    scene = stage.define_prim("/PhysicsScene", "PhysicsScene")
    scene.set("physics:gravityDirection", (0.0, 0.0, -1.0), type="float3")
    scene.set("physics:gravityMagnitude", 9.81, type="float")
    for obj in objects:
        if not obj.rigid_body:
            continue
        path = obj.get("lightusd_usd_path", "/World/" + obj.name)
        prim = stage.get_prim_at(path, None) or stage.define_prim(path, "Xform")
        prim.set_metadata("apiSchemas", ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsCollisionAPI"])
        prim.set("physics:rigidBodyEnabled", True, type="bool")
        prim.set("physics:kinematic", bool(obj.rigid_body.kinematic), type="bool")
        prim.set("physics:mass", float(obj.rigid_body.mass), type="float")
        raw = obj.get("lightusd:physics")
        if raw:
            try:
                for key, value in json.loads(raw).get("attributes", {}).items():
                    if key.startswith(("mjc:", "newton:")) and value is not None:
                        prim.set(key, value, custom=True)
            except (TypeError, ValueError, json.JSONDecodeError):
                pass
