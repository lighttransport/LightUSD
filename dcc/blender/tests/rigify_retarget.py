"""Headless regression for the UE -> Rigify action interchange fallback."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import rigify  # noqa: E402


def main():
    arm_data = bpy.data.armatures.new("RigifyTarget")
    armature = bpy.data.objects.new("RigifyTarget", arm_data)
    bpy.context.scene.collection.objects.link(armature)
    joints = ["root", "spine_01", "spine_02"]
    armature["lightusd_usd_joints"] = json.dumps(joints)
    metadata = rigify.mark_source_armature(armature)
    assert metadata["retarget"]["generated_joint_map"]["spine_01"] == "DEF-spine.001"

    source = bpy.data.actions.new("UEWalk")
    curve = rigify.action_fcurves(source, armature, create=True).new(
        'pose.bones["spine_01"].rotation_quaternion', index=0)
    curve.keyframe_points.add(2)
    curve.keyframe_points[0].co = (1.0, 1.0)
    curve.keyframe_points[1].co = (10.0, 0.5)
    curve.update()
    control_curve = rigify.action_fcurves(source, armature).new(
        'pose.bones["face_ctrl"].location', index=0)
    control_curve.keyframe_points.add(1)
    control_curve.keyframe_points[0].co = (1.0, 0.25)
    control_curve.update()
    target = rigify.retarget_action(source, joints, armature)
    assert target.name == "UEWalk_Rigify"
    target_curves = rigify.action_fcurves(target, armature)
    assert len(target_curves) == 2
    assert any('pose.bones["DEF-spine.001"]' in curve.data_path
               for curve in target_curves)
    assert any('pose.bones["face_ctrl"]' in curve.data_path
               for curve in target_curves)
    assert armature.animation_data.action == target
    assert armature["lightusd_rigify_copied_curves"] == 2
    print("LightUSD Rigify retarget regression passed")


main()
