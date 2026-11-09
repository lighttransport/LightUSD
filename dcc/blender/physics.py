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
                # Headless/background imports do not always provide the
                # active UI context expected by the rigid-body operator.
                # Supply an explicit object override so physics round trips
                # work through Blender MCP and command-line Blender too.
                try:
                    with bpy.context.temp_override(
                            object=obj, active_object=obj,
                            selected_objects=[obj], selected_editable_objects=[obj]):
                        bpy.ops.rigidbody.object_add()
                except RuntimeError:
                    # Blender's rigid-body operator is unavailable in some
                    # background contexts. The USD schema/attributes below
                    # remain authoritative and are sufficient for a lossless
                    # LightUSD round trip.
                    pass
                finally:
                    obj.select_set(False)
            if obj.rigid_body is not None:
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
        raw = obj.get("lightusd:physics")
        if not obj.rigid_body and not raw:
            continue
        path = obj.get("lightusd_usd_path", "/World/" + obj.name)
        prim = stage.get_prim_at(path, None) or stage.define_prim(path, "Xform")
        if obj.rigid_body:
            prim.set_metadata("apiSchemas", ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsCollisionAPI"])
            prim.set("physics:rigidBodyEnabled", True, type="bool")
            prim.set("physics:kinematic", bool(obj.rigid_body.kinematic), type="bool")
            prim.set("physics:mass", float(obj.rigid_body.mass), type="float")
        if raw:
            try:
                stored = json.loads(raw)
                schemas = stored.get("schemas", [])
                if schemas:
                    prim.set_metadata("apiSchemas", schemas)
                for key, value in stored.get("attributes", {}).items():
                    if value is not None:
                        prim.set(key, value, custom=True)
            except (TypeError, ValueError, json.JSONDecodeError):
                pass
