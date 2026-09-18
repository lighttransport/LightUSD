"""Validate UE joint-map targets against a generated Rigify deform rig."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import rigify  # noqa: E402
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402


def main():
    bpy.ops.preferences.addon_enable(module="rigify")
    bpy.ops.object.armature_human_metarig_add()
    metarig = bpy.context.object
    bpy.context.view_layer.objects.active = metarig
    metarig.select_set(True)
    bpy.ops.pose.rigify_generate()
    armature = bpy.context.object
    joints = list(rigify.UE_TO_RIGIFY)
    mapped = rigify.joint_map(joints, generated=True)
    missing = sorted(set(mapped.values()) - {bone.name for bone in armature.data.bones})
    assert not missing, "Rigify metarig is missing mapped targets: " + ", ".join(missing)
    inverse = {target: source for source, target in mapped.items()}
    for bone in armature.data.bones:
        source_joint = inverse.get(bone.name)
        if source_joint:
            bone["lightusd_usd_joint"] = source_joint
    metadata = rigify.mark_source_armature(armature)
    assert metadata["retarget"]["profile"] == "LightUSD.UE.Rigify.v1"
    source_action = bpy.data.actions.new("UEGeneratedRigAction")
    source_curves = rigify.action_fcurves(source_action, armature, create=True)
    curve = source_curves.new('pose.bones["spine_01"].rotation_quaternion', index=0)
    curve.keyframe_points.add(2)
    curve.keyframe_points[0].co = (1.0, 1.0)
    curve.keyframe_points[1].co = (10.0, 0.6)
    curve.update()
    rigify.retarget_action(source_action, joints, armature)
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = 10
    output = "/tmp/lightusd_rigify_generated.usda"
    bpy.ops.object.select_all(action="DESELECT")
    armature.select_set(True)
    bpy.context.view_layer.objects.active = armature
    converter.export_file(output, selected=True)
    stage = load_lightusd().load(output)
    skeletons = stage.prims_of_type("Skeleton")
    animations = stage.prims_of_type("SkelAnimation")
    assert len(skeletons) == 1 and len(animations) == 1
    assert skeletons[0].get("rigify:retargetProfile") == "LightUSD.UE.Rigify.v1"
    report = {"metarig_bones": len(armature.data.bones),
              "mapped_joints": len(mapped), "missing_targets": missing,
              "implementation": "blender_rigify_5.2+",
              "skeletons": len(skeletons),
              "animations": len(animations), "output": output}
    print("LightUSD Rigify metarig regression passed", json.dumps(report, sort_keys=True))


main()
