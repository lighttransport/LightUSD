"""UE mannequin/MetaHuman to Rigify interchange metadata.

UsdSkel remains the interchange contract and the UE skeleton remains
authoritative.  This module only detects the optional addon and records the
source-armature metadata needed for a user-driven Rigify retarget.  It does
not silently change bone names or generate a different canonical skeleton.
"""

from __future__ import annotations

import json


# The names on the left are the stable UE joint leaf names.  Rigify names on
# the right are the deform bones used by the generated metarig.  Keeping this
# table in the USD bridge makes a retarget deterministic without requiring the
# optional Epic ue2rigify addon at export time.
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


def _leaf(joint):
    return str(joint).rsplit("/", 1)[-1]


def joint_map(joints, template="ue_mannequin"):
    """Return a deterministic USD-joint -> Rigify-deform-bone map."""
    result = {}
    for joint in joints:
        joint = str(joint)
        target = UE_TO_RIGIFY.get(_leaf(joint))
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
        "rotation_mode": "quaternion",
        "translation_units": "centimeters",
    }

def available():
    """Return whether Epic's separately-installed ue2rigify addon is loaded."""
    try:
        import ue2rigify  # noqa: F401
    except ImportError:
        return False
    return True


def metadata(armature):
    """Return stable LightUSD/Rigify metadata for a Blender armature."""
    joints = json.loads(armature.get("lightusd_usd_joints", "[]"))
    template = armature.get("lightusd_rigify_template", "ue_mannequin")
    return {
        "available": available(),
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
    armature["lightusd_rigify_external"] = "ue2rigify"
    joints = json.loads(armature.get("lightusd_usd_joints", "[]"))
    armature["lightusd_rigify_joint_map"] = json.dumps(
        joint_map(joints, template), sort_keys=True)
    armature["lightusd_rigify_retarget"] = json.dumps(
        retarget_metadata(joints, template), sort_keys=True)
    return metadata(armature)
