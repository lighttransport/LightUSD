"""UE mannequin/MetaHuman to Rigify interchange metadata.

UsdSkel remains the interchange contract and the UE skeleton remains
authoritative.  This module only detects the optional addon and records the
source-armature metadata needed for a user-driven Rigify retarget.  It does
not silently change bone names or generate a different canonical skeleton.
"""

from __future__ import annotations

import json
import re


# The names on the left are the stable UE joint leaf names.  Rigify names on
# the right are the deform bones used by the Rigify metarig template. Keeping this
# table in the USD bridge makes a retarget deterministic using Blender's native
# Rigify add-on; no Epic-specific bridge add-on is required.
UE_TO_RIGIFY = {
    "root": "root",
    "pelvis": "pelvis",
    "spine_01": "spine",
    "spine_02": "spine.001",
    "spine_03": "spine.002",
    "neck_01": "spine.006",
    "head": "spine.007",
    "clavicle_l": "shoulder.L",
    "upperarm_l": "upper_arm.L",
    "lowerarm_l": "forearm.L",
    "hand_l": "hand.L",
    "clavicle_r": "shoulder.R",
    "upperarm_r": "upper_arm.R",
    "lowerarm_r": "forearm.R",
    "hand_r": "hand.R",
    "thigh_l": "thigh.L",
    "calf_l": "shin.L",
    "foot_l": "foot.L",
    "ball_l": "toe.L",
    "thigh_r": "thigh.R",
    "calf_r": "shin.R",
    "foot_r": "foot.R",
    "ball_r": "toe.R",
}

# Names on a generated Rigify rig.  The metarig and generated deform armature
# intentionally have different namespaces; keep both explicit so metadata
# inspection can use the metarig while animation retargeting uses DEF bones.
UE_TO_RIGIFY_GENERATED = {
    "root": "root",
    "pelvis": "DEF-spine",
    "spine_01": "DEF-spine.001",
    "spine_02": "DEF-spine.002",
    "spine_03": "DEF-spine.003",
    "neck_01": "DEF-spine.006",
    "head": "head",
    "clavicle_l": "DEF-shoulder.L",
    "upperarm_l": "DEF-upper_arm.L",
    "lowerarm_l": "DEF-forearm.L",
    "hand_l": "DEF-hand.L",
    "clavicle_r": "DEF-shoulder.R",
    "upperarm_r": "DEF-upper_arm.R",
    "lowerarm_r": "DEF-forearm.R",
    "hand_r": "DEF-hand.R",
    "thigh_l": "DEF-thigh.L",
    "calf_l": "DEF-shin.L",
    "foot_l": "DEF-foot.L",
    "ball_l": "DEF-toe.L",
    "thigh_r": "DEF-thigh.R",
    "calf_r": "DEF-shin.R",
    "foot_r": "DEF-foot.R",
    "ball_r": "DEF-toe.R",
}


def _leaf(joint):
    return str(joint).rsplit("/", 1)[-1]


def joint_map(joints, template="ue_mannequin", generated=False):
    """Return a deterministic USD-joint -> Rigify-deform-bone map."""
    table = UE_TO_RIGIFY_GENERATED if generated else UE_TO_RIGIFY
    result = {}
    for joint in joints:
        joint = str(joint)
        target = table.get(_leaf(joint))
        if target:
            result[joint] = target
    return result


def retarget_metadata(joints, template="ue_mannequin"):
    """Return serializable metadata consumed by UE/Rigify retarget tools."""
    return {
        "profile": "LightUSD.UE.Rigify.v1",
        "template": template,
        "authority": "UE",
        "source_joint_paths": [str(joint) for joint in joints],
        "joint_map": joint_map(joints, template),
        "generated_joint_map": joint_map(joints, template, generated=True),
        "rotation_mode": "quaternion",
        "translation_units": "centimeters",
    }

def metadata(armature):
    """Return stable LightUSD/Rigify metadata for a Blender armature."""
    joints = json.loads(armature.get("lightusd_usd_joints", "[]"))
    template = armature.get("lightusd_rigify_template", "ue_mannequin")
    return {
        "source_armature": armature.name,
        "template": template,
        "usd_skeleton": armature.get("lightusd_usd_skeleton", ""),
        "authority": "UE",
        "joint_map": joint_map(joints, template),
        "retarget": retarget_metadata(joints, template),
    }


def mark_source_armature(armature, template="ue_mannequin"):
    """Mark an armature as a UE source for an optional Rigify retarget."""
    armature["lightusd_rigify_source"] = True
    armature["lightusd_rigify_template"] = template
    armature["lightusd_rigify_implementation"] = "blender_rigify_5.2+"
    joints = json.loads(armature.get("lightusd_usd_joints", "[]"))
    armature["lightusd_rigify_joint_map"] = json.dumps(
        joint_map(joints, template), sort_keys=True)
    armature["lightusd_rigify_retarget"] = json.dumps(
        retarget_metadata(joints, template), sort_keys=True)
    return metadata(armature)


def action_fcurves(action, armature=None, create=False):
    """Return Blender 5.2+ action F-curves, including layered actions."""
    if hasattr(action, "fcurves"):
        return action.fcurves
    if not create and not action.layers:
        return ()
    layer = action.layers[0] if action.layers else action.layers.new("LightUSD")
    strip = layer.strips[0] if layer.strips else layer.strips.new(type="KEYFRAME")
    if strip.channelbags:
        return strip.channelbags[0].fcurves
    if armature is None:
        raise ValueError("armature is required to create a layered action")
    slot = action.slots.new("OBJECT", armature.name)
    return strip.channelbags.new(slot).fcurves


def retarget_action(action, source_joints, target_armature,
                    template="ue_mannequin", name=None):
    """Copy a UE-source action onto Blender 5.2+ Rigify deform-bone names.

    This data-API retarget is deliberately limited to pose-bone paths. It
    makes the USD bridge deterministic in headless Blender and preserves all
    unrecognized/object curves without guessing at Rigify controls.
    """
    if action is None:
        raise ValueError("source action is required")
    import bpy

    mapping = {_leaf(source): target for source, target in
               joint_map(source_joints, template, generated=True).items()}
    target_action = bpy.data.actions.new(name or action.name + "_Rigify")
    bone_path = re.compile(r'pose\.bones\["([^"]+)"\]')
    copied = 0
    source_curves = action_fcurves(action, target_armature)
    target_curves = action_fcurves(target_action, target_armature, create=True)
    for source_curve in source_curves:
        match = bone_path.search(source_curve.data_path)
        if match:
            target_name = mapping.get(match.group(1))
            data_path = (source_curve.data_path.replace(
                'pose.bones["' + match.group(1) + '"]',
                'pose.bones["' + target_name + '"]', 1)
                         if target_name else source_curve.data_path)
        else:
            data_path = source_curve.data_path
        group_name = source_curve.group.name if source_curve.group else None
        try:
            target_curve = target_curves.new(
                data_path, index=source_curve.array_index,
                action_group=group_name)
        except TypeError:
            target_curve = target_curves.new(data_path, index=source_curve.array_index)
        target_curve.keyframe_points.add(len(source_curve.keyframe_points))
        for target_point, source_point in zip(
                target_curve.keyframe_points, source_curve.keyframe_points):
            target_point.co = source_point.co
            target_point.interpolation = source_point.interpolation
        target_curve.update()
        copied += 1
    target_armature.animation_data_create()
    target_armature.animation_data.action = target_action
    target_armature["lightusd_rigify_retarget_profile"] = \
        retarget_metadata(source_joints, template)["profile"]
    target_armature["lightusd_rigify_source_action"] = action.name
    target_armature["lightusd_rigify_copied_curves"] = copied
    return target_action
