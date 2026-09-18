"""Validate a skeletal USD exported back from UE through Blender MCP."""

import json
import os
import sys

import bpy

ROOT = os.environ.get(
    "LIGHTUSD_REPO_ROOT",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402


INPUT = os.environ.get(
    "LIGHTUSD_BLENDER_UE_INPUT", "/tmp/lightusd_blender_ue/ue_roundtrip.usda")


def main():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    converter.import_file(INPUT)
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    skinned = [obj for obj in meshes if any(
        modifier.type == "ARMATURE" for modifier in obj.modifiers)]
    if len(armatures) != 1 or not skinned:
        raise RuntimeError(
            f"Expected one armature and a skinned mesh, got "
            f"armatures={len(armatures)} meshes={len(meshes)} skinned={len(skinned)}")
    return {"armatures": len(armatures), "meshes": len(meshes),
            "skinned_meshes": len(skinned),
            "bones": len(armatures[0].data.bones)}


result = main()
print("LIGHTUSD_BLENDER_UE_REPORT=" + json.dumps(result, sort_keys=True))
