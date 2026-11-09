"""LightUSD UsdSkel <-> Blender armature/skinned-mesh bridge.

The UE skeleton joint paths remain authoritative. Blender bone names are
display names only; every imported/exported bone carries its USD joint path in
``lightusd_usd_joint`` so Rigify retargeting can operate without renaming the
source skeleton.
"""

from __future__ import annotations

import json
import re

import bpy
from mathutils import Matrix, Vector

from . import rigify


def _value(prim, name, default=None):
    value = prim.get(name)
    if value is None:
        return default
    return value.tolist() if hasattr(value, "tolist") else value


def _matrix(value):
    if value is None:
        return Matrix.Identity(4)
    rows = value.tolist() if hasattr(value, "tolist") else value
    if rows and not hasattr(rows[0], "__iter__"):
        rows = [rows[index:index + 4] for index in range(0, 16, 4)]
    return Matrix(tuple(tuple(float(v) for v in row) for row in rows))


def _metadata(attr, name, default=None):
    if not attr:
        return default
    try:
        value = attr.metadata(name)
        return value.tolist() if hasattr(value, "tolist") else value
    except (AttributeError, TypeError):
        return default


def _safe_name(value, used):
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", str(value).split("/")[-1]) or "Bone"
    base = name
    suffix = 1
    while name in used:
        suffix += 1
        name = f"{base}.{suffix:03d}"
    used.add(name)
    return name


def _joint_parent(joint):
    return str(joint).rsplit("/", 1)[0] if "/" in str(joint) else None


def _skeleton_for_mesh(stage, mesh):
    relation = mesh.relationship("skel:skeleton") if "skel:skeleton" in mesh.relationships else None
    if relation and relation.targets:
        try:
            return stage.prim_at(relation.targets[0])
        except (AttributeError, IndexError):
            pass
    candidates = list(stage.prims_of_type("Skeleton"))
    return candidates[0] if len(candidates) == 1 else None


def import_skeleton(stage, prim, collection):
    """Create one Blender armature from a USD Skeleton prim."""
    joints = [str(v) for v in (_value(prim, "joints", []) or [])]
    if not joints:
        return None, {}
    rest = _value(prim, "restTransforms", []) or []
    arm_data = bpy.data.armatures.new(prim.name)
    arm_obj = bpy.data.objects.new(prim.name, arm_data)
    collection.objects.link(arm_obj)
    arm_obj["lightusd_usd_path"] = prim.path
    arm_obj["lightusd_usd_skeleton"] = prim.path
    arm_obj["lightusd_usd_joints"] = json.dumps(joints)
    arm_obj["lightusd_rigify_source"] = True
    arm_obj["lightusd_rigify_template"] = str(
        prim.get("rigify:template") or "ue_mannequin")
    arm_obj["lightusd_source_asset"] = str(
        prim.get("unreal:sourceAsset") or "")
    rigify.mark_source_armature(
        arm_obj, arm_obj["lightusd_rigify_template"])
    used = set()
    bone_names = {}
    world_matrices = {}

    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = {}
    for index, joint in enumerate(joints):
        local = _matrix(rest[index]) if index < len(rest) else Matrix.Identity(4)
        parent_joint = _joint_parent(joint)
        parent_world = world_matrices.get(parent_joint, Matrix.Identity(4))
        world = parent_world @ local
        world_matrices[joint] = world
        name = _safe_name(joint, used)
        bone_names[joint] = name
        bone = edit_bones[joint] = arm_data.edit_bones.new(name)
        head = world.translation
        direction = world.to_3x3() @ Vector((0.0, 0.1, 0.0))
        bone.head = head
        bone.tail = head + (direction if direction.length > 1e-5 else Vector((0, 0.1, 0)))
        bone["lightusd_usd_joint"] = joint
        bone["lightusd_joint_index"] = index
        if parent_joint in edit_bones:
            bone.parent = edit_bones[parent_joint]
    bpy.ops.object.mode_set(mode="OBJECT")
    arm_obj.select_set(False)
    arm_obj.matrix_world = _matrix(prim.world_transform())
    return arm_obj, bone_names


def _joint_names_for_mesh(mesh, skeleton):
    local = _value(mesh, "skel:joints", None)
    return [str(v) for v in (local or _value(skeleton, "joints", []) or [])]


def import_skinning(mesh_prim, mesh_obj, skeleton_obj, bone_names, skeleton):
    """Attach a Blender mesh to an imported armature and restore weights."""
    indices = _value(mesh_prim, "primvars:skel:jointIndices", []) or []
    weights = _value(mesh_prim, "primvars:skel:jointWeights", []) or []
    if not indices or not weights or not skeleton_obj:
        return False
    index_attr = mesh_prim.attribute("primvars:skel:jointIndices")
    weight_attr = mesh_prim.attribute("primvars:skel:jointWeights")
    element_size = int(_metadata(index_attr, "elementSize", 1) or 1)
    weight_size = int(_metadata(weight_attr, "elementSize", element_size) or element_size)
    element_size = min(element_size, weight_size)
    joint_names = _joint_names_for_mesh(mesh_prim, skeleton)
    for local_index, joint in enumerate(joint_names):
        bone_name = bone_names.get(joint)
        if bone_name:
            mesh_obj.vertex_groups.new(name=bone_name)
    vertex_count = len(mesh_obj.data.vertices)
    for vertex_index in range(vertex_count):
        begin = vertex_index * element_size
        for offset in range(element_size):
            index_offset = begin + offset
            if index_offset >= len(indices) or index_offset >= len(weights):
                continue
            joint_index = int(indices[index_offset])
            if joint_index < 0 or joint_index >= len(joint_names):
                continue
            bone_name = bone_names.get(joint_names[joint_index])
            if not bone_name:
                continue
            weight = float(weights[index_offset])
            if weight > 0.0:
                mesh_obj.vertex_groups[bone_name].add([vertex_index], weight, "REPLACE")
    modifier = mesh_obj.modifiers.new("LightUSD Armature", "ARMATURE")
    modifier.object = skeleton_obj
    mesh_obj.parent = skeleton_obj
    mesh_obj["lightusd_usd_skeleton"] = skeleton.path
    mesh_obj["lightusd_usd_joint_order"] = json.dumps(joint_names)
    return True


def import_stage_skeletons(stage, collection):
    skeletons = {}
    for prim in stage.prims_of_type("Skeleton"):
        arm_obj, bone_names = import_skeleton(stage, prim, collection)
        if arm_obj:
            skeletons[prim.path] = (arm_obj, bone_names)
    return skeletons


def _matrix_value(matrix):
    return tuple(float(v) for row in matrix for v in row)


def _local_bone_matrix(bone):
    if bone.parent:
        return bone.parent.matrix_local.inverted() @ bone.matrix_local
    return bone.matrix_local.copy()


def _bone_joint_path(bone, parent_path=None):
    stored = bone.get("lightusd_usd_joint")
    if stored:
        return str(stored)
    name = bone.name
    return (parent_path + "/" + name) if parent_path else name


def _samples(attribute, fallback_time=1.0):
    if not attribute:
        return []
    samples = list(attribute.timesamples)
    if samples:
        return [(float(time), value.tolist() if hasattr(value, "tolist") else value)
                for time, value in samples]
    value = attribute.get()
    if value is None:
        return []
    return [(float(fallback_time), value.tolist() if hasattr(value, "tolist") else value)]


def _quat_from_usd(value):
    return (float(value[1]), float(value[2]), float(value[3]), float(value[0]))


def _usd_quat(value):
    return (float(value.w), float(value.x), float(value.y), float(value.z))


def import_animation(prim, arm_obj, bone_names, stage_start=1.0):
    """Create a Blender Action from a USD SkelAnimation prim."""
    joints = [str(v) for v in (_value(prim, "joints", []) or [])]
    if not joints:
        joints = json.loads(arm_obj.get("lightusd_usd_joints", "[]"))
    translations = dict(_samples(prim.attribute("translations"), stage_start))
    rotations = dict(_samples(prim.attribute("rotations"), stage_start))
    scales = dict(_samples(prim.attribute("scales"), stage_start))
    times = sorted(set(translations) | set(rotations) | set(scales))
    if not times:
        return None
    action = bpy.data.actions.new(prim.name)
    arm_obj.animation_data_create()
    arm_obj.animation_data.action = action
    for joint_index, joint in enumerate(joints):
        bone_name = bone_names.get(joint)
        pose_bone = arm_obj.pose.bones.get(bone_name) if bone_name else None
        if not pose_bone:
            continue
        pose_bone.rotation_mode = "QUATERNION"
        for time in times:
            if time in translations and joint_index < len(translations[time]):
                pose_bone.location = tuple(translations[time][joint_index])
                pose_bone.keyframe_insert("location", frame=time, group=bone_name)
            if time in rotations and joint_index < len(rotations[time]):
                pose_bone.rotation_quaternion = _quat_from_usd(rotations[time][joint_index])
                pose_bone.keyframe_insert("rotation_quaternion", frame=time, group=bone_name)
            if time in scales and joint_index < len(scales[time]):
                pose_bone.scale = tuple(scales[time][joint_index])
                pose_bone.keyframe_insert("scale", frame=time, group=bone_name)
    arm_obj["lightusd_usd_animation"] = prim.path
    return action


def import_stage_animations(stage, skeletons):
    animation_skeletons = {}
    for skeleton_prim in stage.prims_of_type("Skeleton"):
        relation = (skeleton_prim.relationship("skel:animationSource")
                    if "skel:animationSource" in skeleton_prim.relationships else None)
        if relation and relation.targets:
            animation_skeletons[relation.targets[0]] = skeleton_prim.path
    imported = {}
    for prim in stage.prims_of_type("SkelAnimation"):
        target = None
        relation = prim.relationship("skel:skeleton") if "skel:skeleton" in prim.relationships else None
        if relation and relation.targets:
            target = skeletons.get(relation.targets[0])
        if target is None and prim.path in animation_skeletons:
            target = skeletons.get(animation_skeletons[prim.path])
        if target is None and len(skeletons) == 1:
            target = next(iter(skeletons.values()))
        if target is None:
            continue
        armature, bone_names = target
        action = import_animation(prim, armature, bone_names, stage.start_time)
        if action:
            imported[prim.path] = action
    return imported


def export_animation(stage, arm_obj, path, joints, skeleton_path, blendshape_meshes=None):
    """Author a SkelAnimation from the active Blender armature Action."""
    action = arm_obj.animation_data.action if arm_obj.animation_data else None
    if not action:
        return None
    animation = stage.define_prim(path, "SkelAnimation")
    animation.set("joints", joints, type="token[]", uniform=True)
    animation.add_relationship("skel:skeleton", [skeleton_path])
    stage.prim_at(skeleton_path).add_relationship("skel:animationSource", [path])
    blend_mesh = next((obj for obj in (blendshape_meshes or [])
                       if obj.data.shape_keys and len(obj.data.shape_keys.key_blocks) > 1), None)
    blend_keys = list(blend_mesh.data.shape_keys.key_blocks[1:]) if blend_mesh else []
    if blend_keys:
        animation.set("blendShapes", [key.name for key in blend_keys], type="token[]", uniform=True)
    scene = bpy.context.scene
    original_frame = scene.frame_current
    start = int(scene.frame_start)
    end = int(scene.frame_end)
    for frame in range(start, end + 1):
        scene.frame_set(frame)
        translations = []
        rotations = []
        scales = []
        for joint in joints:
            bone = next((b for b in arm_obj.data.bones if _bone_joint_path(b) == joint), None)
            pose_bone = arm_obj.pose.bones.get(bone.name) if bone else None
            basis = pose_bone.matrix_basis.copy() if pose_bone else Matrix.Identity(4)
            translation, rotation, scale = basis.decompose()
            translations.append(tuple(translation))
            rotations.append(_usd_quat(rotation))
            scales.append(tuple(scale))
        blend_weights = [float(key.value) for key in blend_keys]
        if frame == start:
            animation.set("translations", translations, type="float3[]")
            animation.set("rotations", rotations, type="quatf[]")
            animation.set("scales", scales, type="half3[]")
            if blend_keys:
                animation.set("blendShapeWeights", blend_weights, type="float[]")
        else:
            animation.set("translations", translations, type="float3[]", time=float(frame))
            animation.set("rotations", rotations, type="quatf[]", time=float(frame))
            animation.set("scales", scales, type="half3[]", time=float(frame))
            if blend_keys:
                animation.set("blendShapeWeights", blend_weights, type="float[]", time=float(frame))
    scene.frame_set(original_frame)
    arm_obj["lightusd_usd_animation"] = path
    return path


def export_armature(stage, arm_obj, path):
    """Author a SkelRoot/Skeleton pair from a Blender armature."""
    root = stage.define_prim(path, "SkelRoot")
    root.set_metadata("apiSchemas", ["SkelBindingAPI"])
    root.set("unreal:sourceAsset", arm_obj.get("lightusd_source_asset", arm_obj.name), type="string", custom=True)
    root.set("rigify:sourceArmature", arm_obj.name, type="string", custom=True)
    root.set("rigify:template", arm_obj.get("lightusd_rigify_template", "ue_mannequin"),
             type="token", custom=True)
    skeleton_path = path + "/Skeleton"
    skeleton = stage.define_prim(skeleton_path, "Skeleton")
    joints = []
    rest = []
    bind = []
    for bone in arm_obj.data.bones:
        joint = _bone_joint_path(bone)
        joints.append(joint)
        rest_matrix = _local_bone_matrix(bone)
        rest.append(_matrix_value(rest_matrix))
        bind.append(_matrix_value((arm_obj.matrix_world @ bone.matrix_local).inverted()))
    skeleton.set("joints", joints, type="token[]", uniform=True)
    skeleton.set("restTransforms", rest, type="matrix4d[]", uniform=True)
    skeleton.set("bindTransforms", bind, type="matrix4d[]", uniform=True)
    skeleton.set("unreal:skeletonAuthority", "UE", type="token", custom=True)
    skeleton.set("rigify:template", arm_obj.get("lightusd_rigify_template", "ue_mannequin"),
                 type="token", custom=True)
    retarget = rigify.retarget_metadata(
        joints, arm_obj.get("lightusd_rigify_template", "ue_mannequin"))
    root.set("rigify:jointMap", json.dumps(retarget["joint_map"], sort_keys=True),
             type="string", custom=True)
    root.set("rigify:retargetProfile", retarget["profile"],
             type="token", custom=True)
    skeleton.set("rigify:jointMap", json.dumps(retarget["joint_map"], sort_keys=True),
                 type="string", custom=True)
    skeleton.set("rigify:retargetProfile", retarget["profile"],
                 type="token", custom=True)
    root.add_relationship("skel:skeleton", [skeleton_path])
    arm_obj["lightusd_usd_path"] = path
    return path, skeleton_path, joints


def export_skinning(stage, mesh_obj, mesh_prim, arm_obj, skeleton_path, joints):
    if not arm_obj:
        return
    mesh_prim.add_relationship("skel:skeleton", [skeleton_path])
    schemas = list(mesh_prim.metadata("apiSchemas") or ())
    if "SkelBindingAPI" not in schemas:
        schemas.append("SkelBindingAPI")
    mesh_prim.set_metadata("apiSchemas", schemas)
    mesh_prim.set("skel:joints", joints, type="token[]", uniform=True)
    index_by_name = {name: index for index, name in enumerate(joints)}
    indices = []
    weights = []
    for vertex in mesh_obj.data.vertices:
        influences = []
        for group in vertex.groups:
            if group.group >= len(mesh_obj.vertex_groups):
                continue
            bone_name = mesh_obj.vertex_groups[group.group].name
            bone = arm_obj.data.bones.get(bone_name)
            joint = bone.get("lightusd_usd_joint") if bone else bone_name
            if joint in index_by_name and group.weight > 0.0:
                influences.append((index_by_name[joint], float(group.weight)))
        influences.sort(key=lambda pair: pair[1], reverse=True)
        influences = influences[:4]
        total = sum(weight for _, weight in influences) or 1.0
        influences += [(0, 0.0)] * (4 - len(influences))
        indices.extend(index for index, _ in influences)
        weights.extend(weight / total for _, weight in influences)
    mesh_prim.set("primvars:skel:jointIndices", indices, type="int[]")
    mesh_prim.attribute("primvars:skel:jointIndices").set_metadata("elementSize", 4)
    mesh_prim.set("primvars:skel:jointWeights", weights, type="float[]")
    mesh_prim.attribute("primvars:skel:jointWeights").set_metadata("elementSize", 4)
    mesh_prim.attribute("primvars:skel:jointIndices").set_metadata("interpolation", "vertex")
    mesh_prim.attribute("primvars:skel:jointWeights").set_metadata("interpolation", "vertex")


def find_armature_modifier(obj):
    for modifier in obj.modifiers:
        if modifier.type == "ARMATURE" and modifier.object:
            return modifier.object
    return None
