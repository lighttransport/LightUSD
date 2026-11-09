"""Blender 5.2 UsdSkel armature/skinning regression."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter, rigify  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402

INPUT = os.path.join(ROOT, "tests/usda/usdskel-001.usda")
OUT = os.environ.get("LIGHTUSD_BLENDER_USDSKEL_OUT", "/tmp/lightusd_blender_usdskel")


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    converter.import_file(INPUT)
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    assert len(armatures) == 1 and len(armatures[0].data.bones) == 3
    assert len(meshes) == 1
    assert any(mod.type == "ARMATURE" for mod in meshes[0].modifiers)
    assert len(meshes[0].vertex_groups) == 3
    basis = meshes[0].shape_key_add(name="Basis")
    smile = meshes[0].shape_key_add(name="Smile")
    smile.data[0].co.x += 0.1
    for frame, value in ((1, 0.0), (3, 1.0)):
        bpy.context.scene.frame_set(frame)
        smile.value = value
        smile.keyframe_insert("value", frame=frame)
    armature = armatures[0]
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = 3
    armature.animation_data_create()
    armature.animation_data.action = bpy.data.actions.new("ManikinAction")
    pose_bone = armature.pose.bones[0]
    pose_bone.rotation_mode = "QUATERNION"
    for frame, angle in ((1, 0.0), (3, 0.5)):
        bpy.context.scene.frame_set(frame)
        pose_bone.rotation_quaternion = (1.0, 0.0, 0.0, angle)
        pose_bone.keyframe_insert("rotation_quaternion", frame=frame)
    action = armature.animation_data.action
    assert action is not None
    output = os.path.join(OUT, "usdskel_roundtrip.usda")
    converter.export_file(output)
    stage = load_lightusd().load(output)
    skeletons = list(stage.prims_of_type("Skeleton"))
    animations = list(stage.prims_of_type("SkelAnimation"))
    blendshapes = list(stage.prims_of_type("BlendShape"))
    skinned = [prim for prim in stage.prims_of_type("Mesh")
               if "skel:skeleton" in prim.relationships]
    assert len(skeletons) == 1
    assert skeletons[0].get("rigify:retargetProfile") == "LightUSD.UE.Rigify.v1"
    assert rigify.joint_map(["root/pelvis"])["root/pelvis"] == "pelvis"
    assert len(animations) == (1 if action else 0)
    assert len(blendshapes) == 1
    assert animations[0].get("blendShapes") == ("Smile",)
    assert len(skinned) == 1
    assert len(skinned[0].get("primvars:skel:jointIndices")) == 48
    assert skinned[0].attribute("primvars:skel:jointIndices").metadata("elementSize") == 4
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    converter.import_file(output)
    imported_armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    assert len(imported_armatures) == 1
    assert imported_armatures[0].animation_data
    assert imported_armatures[0].animation_data.action
    imported_meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    assert len(imported_meshes) == 1
    assert imported_meshes[0].data.shape_keys
    assert imported_meshes[0].data.shape_keys.animation_data
    with open(os.path.join(OUT, "report.json"), "w", encoding="utf-8") as stream:
        json.dump({"bones": len(imported_armatures[0].data.bones),
                   "vertex_groups": len(imported_armatures[0].children[0].vertex_groups)
                   if imported_armatures[0].children else 0,
                   "joint_indices": len(skinned[0].get("primvars:skel:jointIndices")),
                   "export_element_size": 4,
                   "animations": len(animations),
                   "blendshapes": len(blendshapes)}, stream)
    print("LightUSD Blender UsdSkel regression passed:", OUT)


main()
