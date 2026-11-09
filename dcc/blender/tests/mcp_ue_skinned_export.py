"""Create a small Blender UsdSkel asset for the UE MCP bridge test."""

import json
import os
import sys

import bpy

ROOT = os.environ.get(
    "LIGHTUSD_REPO_ROOT",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402


OUTPUT = os.environ.get(
    "LIGHTUSD_BLENDER_UE_OUTPUT", "/tmp/lightusd_blender_ue/skinned.usda")


def main():
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    mesh_data = bpy.data.meshes.new("MCP_UE_Mesh")
    mesh_data.from_pydata(
        [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0),
         (-1, -1, 2), (1, -1, 2), (1, 1, 2), (-1, 1, 2)],
        [], [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1),
             (1, 5, 6, 2), (2, 6, 7, 3), (4, 0, 3, 7)])
    mesh_obj = bpy.data.objects.new("MCP_UE_SkinnedMesh", mesh_data)
    bpy.context.collection.objects.link(mesh_obj)

    arm_data = bpy.data.armatures.new("MCP_UE_Skeleton")
    armature = bpy.data.objects.new("MCP_UE_Skeleton", arm_data)
    bpy.context.collection.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    armature.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm_data.edit_bones.new("root")
    root.head = (0, 0, 0)
    root.tail = (0, 0, 1)
    tip = arm_data.edit_bones.new("root/tip")
    tip.head = (0, 0, 1)
    tip.tail = (0, 0, 2)
    tip.parent = root
    bpy.ops.object.mode_set(mode="OBJECT")

    modifier = mesh_obj.modifiers.new("MCP_UE_Armature", "ARMATURE")
    modifier.object = armature
    root_group = mesh_obj.vertex_groups.new(name="root")
    root_group.add([0, 1, 2, 3], 1.0, "REPLACE")
    tip_group = mesh_obj.vertex_groups.new(name="root/tip")
    tip_group.add([4, 5, 6, 7], 1.0, "REPLACE")

    converter.export_file(OUTPUT)
    stage = load_lightusd().load(OUTPUT)
    skeletons = list(stage.prims_of_type("Skeleton"))
    skinned = [prim for prim in stage.prims_of_type("Mesh")
               if "skel:skeleton" in prim.relationships]
    stage.close()
    if len(skeletons) != 1 or len(skinned) != 1:
        raise RuntimeError(
            f"Expected one skeleton and one skinned mesh, got "
            f"{len(skeletons)} and {len(skinned)}")
    return {"output": OUTPUT, "skeletons": 1, "skinned_meshes": 1}


result = main()
