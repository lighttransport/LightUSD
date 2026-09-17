import json
import os
import re
from pathlib import Path

import bpy
from mathutils import Matrix

from . import materialx, physics, usdskel
from .preferences import get_preferences, load_lightusd


def _attr(prim, name):
    value = prim.get(name)
    return value.tolist() if hasattr(value, "tolist") else value


def _matrix(value):
    return Matrix(value) if value else Matrix.Identity(4)


def _new_collection(name):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _curve_attribute(data, name):
    """Return a Blender curve attribute as plain Python values."""
    if not hasattr(data, "attributes"):
        return None
    attr = data.attributes.get(name)
    if not attr:
        return None
    values = []
    for item in attr.data:
        if hasattr(item, "vector"):
            values.append(tuple(item.vector))
        elif hasattr(item, "color"):
            values.append(tuple(item.color))
        elif hasattr(item, "value"):
            values.append(item.value)
    return values


def _set_curve_attribute(data, name, data_type, domain, values):
    if not values:
        return
    attr = data.attributes.get(name) or data.attributes.new(
        name=name, type=data_type, domain=domain)
    for item, value in zip(attr.data, values):
        if hasattr(item, "vector"):
            item.vector = value
        elif hasattr(item, "color"):
            item.color = (*value[:3], value[3] if len(value) > 3 else 1.0)
        elif hasattr(item, "value"):
            item.value = value


def _import_blendshapes(stage, objects):
    """Restore USD BlendShape targets as Blender shape keys."""
    for mesh_prim in stage.prims_of_type("Mesh"):
        obj = objects.get(mesh_prim.path)
        if not obj or obj.type != "MESH" or not _attr(mesh_prim, "skel:blendShapes"):
            continue
        names = _attr(mesh_prim, "skel:blendShapes") or []
        relation = mesh_prim.relationship("skel:blendShapeTargets") \
            if "skel:blendShapeTargets" in mesh_prim.relationships else None
        targets = relation.targets if relation else []
        if not names or len(names) != len(targets):
            continue
        obj.shape_key_add(name="Basis")
        for name, target_path in zip(names, targets):
            target = stage.prim_at(target_path)
            key = obj.shape_key_add(name=str(name))
            offsets = _attr(target, "offsets") or []
            point_indices = _attr(target, "pointIndices") or list(range(len(offsets)))
            for point_index, offset in zip(point_indices, offsets):
                if int(point_index) < len(key.data):
                    base = obj.data.vertices[int(point_index)].co
                    key.data[int(point_index)].co = tuple(base[i] + offset[i] for i in range(3))
        obj["lightusd_usd_blendshapes"] = json.dumps([str(name) for name in names])


def _import_blendshape_animation(stage, objects):
    for anim in stage.prims_of_type("SkelAnimation"):
        names = _attr(anim, "blendShapes") or []
        if not names:
            continue
        weights = anim.attribute("blendShapeWeights")
        samples = list(weights.timesamples) if weights else []
        if not samples:
            value = weights.get() if weights else None
            samples = [(stage.start_time, value)] if value is not None else []
        for obj in objects.values():
            if not obj or obj.type != "MESH" or not obj.data.shape_keys:
                continue
            keys = {key.name: key for key in obj.data.shape_keys.key_blocks}
            if not all(str(name) in keys for name in names):
                continue
            for time, values in samples:
                values = values.tolist() if hasattr(values, "tolist") else values
                for name, value in zip(names, values):
                    keys[str(name)].value = float(value)
                    keys[str(name)].keyframe_insert("value", frame=float(time))
            obj["lightusd_usd_blendshape_animation"] = anim.path
            break


def _curve_points_and_counts(obj):
    curves = obj.data
    points = []
    counts = []
    radii = []
    if hasattr(curves, "curves"):
        source_curves = curves.curves
        for curve in source_curves:
            counts.append(curve.points_length)
            for point in curve.points:
                points.append(tuple(point.position))
                # Blender's newly-created Hair Curves default to zero radius;
                # USD/UE groom widths must remain non-zero for HairStrands.
                radii.append(float(point.radius) if point.radius > 0.0 else 0.01)
    else:
        # Blender's legacy Curve datablock uses splines and ``co`` points.
        for spline in curves.splines:
            counts.append(len(spline.points))
            for point in spline.points:
                points.append(tuple(point.co[:3]))
                radius = getattr(point, "radius", 0.0)
                radii.append(float(radius) if radius > 0.0 else 0.01)
    return points, counts, radii


def _basis_curves_from_object(stage, obj, path):
    points, counts, radii = _curve_points_and_counts(obj)
    prim = stage.define_prim(path, "BasisCurves")
    prim.set("type", "linear", type="token")
    prim.set("wrap", "nonperiodic", type="token")
    prim.set("points", points, type="point3f[]")
    prim.set("curveVertexCounts", counts, type="int[]")
    if radii:
        prim.set("widths", [2.0 * radius for radius in radii], type="float[]")

    group_ids = _curve_attribute(obj.data, "groom_group_id")
    if group_ids:
        prim.set("primvars:groom_group_id", group_ids, type="int[]", uniform=True)
        prim.attribute("primvars:groom_group_id").set_metadata("interpolation", "uniform")
    guides = _curve_attribute(obj.data, "groom_guide")
    if guides:
        prim.set("primvars:groom_guide", guides, type="int[]", uniform=True)
        prim.attribute("primvars:groom_guide").set_metadata("interpolation", "uniform")
    strand_ids = _curve_attribute(obj.data, "groom_id")
    if strand_ids:
        prim.set("primvars:groom_id", strand_ids, type="int[]", uniform=True)
        prim.attribute("primvars:groom_id").set_metadata("interpolation", "uniform")
    root_uv = _curve_attribute(obj.data, "groom_root_uv")
    if root_uv:
        # The Python binding accepts the base float2 POD type; retain the
        # semantic in the primvar name rather than relying on role aliases.
        prim.set("primvars:groom_root_uv", root_uv, type="float2[]", uniform=True)
        prim.attribute("primvars:groom_root_uv").set_metadata("interpolation", "uniform")
    colors = _curve_attribute(obj.data, "groom_color")
    if colors:
        prim.set("primvars:groom_color", [tuple(c[:3]) for c in colors], type="color3f[]")
        prim.attribute("primvars:groom_color").set_metadata("interpolation", "vertex")
    roughness = _curve_attribute(obj.data, "groom_roughness")
    if roughness:
        prim.set("primvars:groom_roughness", roughness, type="float[]")
        prim.attribute("primvars:groom_roughness").set_metadata("interpolation", "vertex")
    prim.set("xformOp:transform", tuple(v for row in obj.matrix_world for v in row),
             type="matrix4d")
    prim.set("xformOpOrder", ["xformOp:transform"], type="token[]")
    scene = bpy.context.scene
    if (obj.animation_data or getattr(obj.data, "animation_data", None)) \
            and scene.frame_end > scene.frame_start:
        original_frame = scene.frame_current
        depsgraph = bpy.context.evaluated_depsgraph_get()
        for frame in range(scene.frame_start, scene.frame_end + 1):
            scene.frame_set(frame)
            evaluated = obj.evaluated_get(depsgraph)
            sample_points, sample_counts, sample_radii = _curve_points_and_counts(evaluated)
            if sample_counts != counts:
                raise ValueError(f"Animated groom topology changed at frame {frame}")
            prim.set("points", sample_points, type="point3f[]", time=float(frame))
            prim.set("widths", [2.0 * radius for radius in sample_radii],
                     type="float[]", time=float(frame))
        scene.frame_set(original_frame)
    obj["lightusd_usd_path"] = path
    return path


def _nurbs_curves_from_object(stage, obj, path):
    """Export Blender NURBS splines using the USD NurbsCurves schema."""
    splines = [s for s in obj.data.splines if s.type == "NURBS"]
    if not splines:
        return _basis_curves_from_object(stage, obj, path)
    points = []
    counts = []
    orders = []
    knots = []
    ranges = []
    weights = []
    knot_offset = 0
    for spline in splines:
        count = len(spline.points)
        order = max(2, min(int(spline.order_u), count))
        counts.append(count)
        orders.append(order)
        points.extend(tuple(point.co[:3]) for point in spline.points)
        weights.extend(float(point.co[3]) for point in spline.points)
        # Clamped uniform knot vector, matching Blender's endpoint mode.
        knot_count = count + order
        local_knots = [float(index) for index in range(knot_count)]
        knots.extend(local_knots)
        ranges.extend((local_knots[order - 1], local_knots[count]))
        knot_offset += knot_count
    prim = stage.define_prim(path, "NurbsCurves")
    prim.set("points", points, type="point3f[]")
    prim.set("curveVertexCounts", counts, type="int[]")
    prim.set("order", orders, type="int[]")
    prim.set("knots", knots, type="double[]")
    prim.set("ranges", ranges, type="double2[]")
    prim.set("pointWeights", weights, type="double[]")
    prim.set("widths", [0.02] * len(points), type="float[]")
    prim.set("xformOp:transform", tuple(v for row in obj.matrix_world for v in row),
             type="matrix4d")
    prim.set("xformOpOrder", ["xformOp:transform"], type="token[]")
    obj["lightusd_usd_path"] = path
    return path


def import_file(filepath):
    lightusd = load_lightusd()
    pref = get_preferences()
    limit = pref.max_memory_mb * 1024 * 1024
    stage = lightusd.load(filepath, composed=True, load_payloads=True, max_memory=limit)
    collection = _new_collection(Path(filepath).stem)
    materials = {
        prim.path: materialx.import_material(stage, prim, prim.name)
        for prim in stage.prims_of_type("Material")
    }
    objects = {}
    skeletons = usdskel.import_stage_skeletons(stage, collection)
    for skeleton_path, (armature, _) in skeletons.items():
        objects[skeleton_path] = armature
    usdskel.import_stage_animations(stage, skeletons)
    for prim in stage:
        if prim.type_name in ("Material", "Shader", "Scope", "PhysicsScene",
                              "SkelRoot", "Skeleton", "SkelAnimation"):
            continue
        parent = objects.get(prim.parent.path if prim.parent else "")
        obj = None
        if prim.type_name == "Mesh":
            points = _attr(prim, "points") or []
            counts = _attr(prim, "faceVertexCounts") or []
            indices = _attr(prim, "faceVertexIndices") or []
            faces, offset = [], 0
            for count in counts:
                faces.append(indices[offset:offset + count])
                offset += count
            mesh = bpy.data.meshes.new(prim.name)
            mesh.from_pydata(points, [], faces)
            obj = bpy.data.objects.new(prim.name, mesh)
            collection.objects.link(obj)
            skeleton = usdskel._skeleton_for_mesh(stage, prim)
            if skeleton is not None and skeleton.path in skeletons:
                armature, bone_names = skeletons[skeleton.path]
                obj["lightusd_usd_skeleton"] = skeleton.path
                usdskel.import_skinning(prim, obj, armature, bone_names, skeleton)
            if prim.get("groom_card"):
                obj["lightusd_groom_card"] = True
                obj["lightusd_groom_card_id"] = prim.get("groom_card_id", 0)
        elif prim.type_name in ("BasisCurves", "NurbsCurves"):
            points = _attr(prim, "points") or []
            counts = _attr(prim, "curveVertexCounts") or []
            if prim.type_name == "NurbsCurves":
                curves = bpy.data.curves.new(prim.name, "CURVE")
                curves.dimensions = "3D"
                orders = _attr(prim, "order") or []
                offset = 0
                for index, count in enumerate(counts):
                    spline = curves.splines.new("NURBS")
                    spline.points.add(max(0, int(count) - 1))
                    spline.order_u = max(2, min(int(orders[index]) if index < len(orders) else 3, int(count)))
                    for point_index, point in enumerate(points[offset:offset + int(count)]):
                        spline.points[point_index].co = (*point, 1.0)
                    spline.use_endpoint_u = True
                    offset += int(count)
                obj = bpy.data.objects.new(prim.name, curves)
                collection.objects.link(obj)
            elif prim.attribute("points").has_timesamples:
                # Blender's Hair Curves geometry attributes are not reliably
                # keyframeable across 5.x. Use legacy Curve splines for
                # animated USD curves so point positions and widths survive
                # import -> export as ordinary Blender animation.
                curves = bpy.data.curves.new(prim.name, "CURVE")
                curves.dimensions = "3D"
                for count in counts:
                    spline = curves.splines.new("POLY")
                    spline.points.add(max(0, int(count) - 1))
                point_attr = prim.attribute("points")
                width_attr = prim.attribute("widths")
                for time, sample in point_attr.timesamples:
                    sample_points = sample.tolist()
                    sample_widths = width_attr.get(time).tolist() if width_attr else []
                    offset = 0
                    for spline, count in zip(curves.splines, counts):
                        for point_index in range(int(count)):
                            point = sample_points[offset + point_index]
                            spline.points[point_index].co = (*point, 1.0)
                            spline.points[point_index].keyframe_insert("co", frame=time)
                            width_index = offset + point_index
                            if width_index < len(sample_widths):
                                spline.points[point_index].radius = float(sample_widths[width_index]) * 0.5
                                spline.points[point_index].keyframe_insert("radius", frame=time)
                        offset += int(count)
                obj = bpy.data.objects.new(prim.name, curves)
                collection.objects.link(obj)
            elif hasattr(bpy.data, "hair_curves"):
                curves = bpy.data.hair_curves.new(prim.name)
                curves.add_curves(counts)
                curves.attributes["position"].data.foreach_set("vector", [v for p in points for v in p])
                widths = _attr(prim, "widths") or []
                for index, curve in enumerate(curves.curves):
                    start = sum(counts[:index])
                    for point_index, point in enumerate(curve.points):
                        width_index = start + point_index
                        if width_index < len(widths):
                            point.radius = float(widths[width_index]) * 0.5
                for attr_name, data_type, domain, usd_name in (
                    ("groom_group_id", "INT", "CURVE", "primvars:groom_group_id"),
                    ("groom_guide", "INT", "CURVE", "primvars:groom_guide"),
                    ("groom_id", "INT", "CURVE", "primvars:groom_id"),
                    ("groom_root_uv", "FLOAT2", "CURVE", "primvars:groom_root_uv"),
                ):
                    values = _attr(prim, usd_name)
                    if values:
                        _set_curve_attribute(curves, attr_name, data_type, domain, values)
                for attr_name, data_type, _, usd_name in (
                    ("groom_color", "FLOAT_COLOR", "POINT", "primvars:groom_color"),
                    ("groom_roughness", "FLOAT", "POINT", "primvars:groom_roughness"),
                ):
                    values = _attr(prim, usd_name)
                    if values:
                        _set_curve_attribute(curves, attr_name, data_type, "POINT", values)
                obj = bpy.data.objects.new(prim.name, curves)
                collection.objects.link(obj)
        elif prim.type_name == "Camera":
            data = bpy.data.cameras.new(prim.name)
            obj = bpy.data.objects.new(prim.name, data)
            collection.objects.link(obj)
        elif prim.type_name.endswith("Light"):
            data = bpy.data.lights.new(prim.name, "POINT")
            obj = bpy.data.objects.new(prim.name, data)
            collection.objects.link(obj)
        else:
            obj = bpy.data.objects.new(prim.name, None)
            collection.objects.link(obj)
        if obj:
            obj.matrix_world = _matrix(prim.world_transform())
            obj["lightusd_usd_path"] = prim.path
            if "material:binding" in prim.relationships and hasattr(obj.data, "materials"):
                targets = prim.relationship("material:binding").targets
                if targets and targets[0] in materials:
                    obj.data.materials.append(materials[targets[0]])
            objects[prim.path] = obj
            if parent and obj.parent is None:
                obj.parent = parent
    _import_blendshapes(stage, objects)
    _import_blendshape_animation(stage, objects)
    physics.import_physics(stage, objects)
    if stage.start_time != stage.end_time:
        bpy.context.scene.frame_start = int(stage.start_time)
        bpy.context.scene.frame_end = int(stage.end_time)
    return stage


def _export_object(stage, obj, parent_path, armature_exports=None):
    name = re.sub(r"[^A-Za-z0-9_]", "_", obj.name)
    if not name or name[0].isdigit():
        name = "Object_" + name
    path = parent_path + "/" + name
    if obj.type == "ARMATURE":
        exported = usdskel.export_armature(stage, obj, path)
        if armature_exports is not None:
            armature_exports[obj.name] = exported
        return path
    if obj.type == "CURVES" and hasattr(obj.data, "curves"):
        return _basis_curves_from_object(stage, obj, path)
    if obj.type == "CURVE":
        return _nurbs_curves_from_object(stage, obj, path)
    prim = stage.define_prim(path, "Mesh" if obj.type == "MESH" else "Xform")
    prim.set("xformOp:transform", tuple(v for row in obj.matrix_world for v in row), type="matrix4d")
    prim.set("xformOpOrder", ["xformOp:transform"], type="token[]")
    obj["lightusd_usd_path"] = path
    if obj.type == "MESH":
        mesh = obj.data
        prim.set("points", [tuple(v.co) for v in mesh.vertices], type="point3f[]")
        prim.set("faceVertexCounts", [len(p.vertices) for p in mesh.polygons], type="int[]")
        prim.set("faceVertexIndices", [i for p in mesh.polygons for i in p.vertices], type="int[]")
        shape_keys = obj.data.shape_keys.key_blocks if obj.data.shape_keys else []
        if len(shape_keys) > 1:
            names = []
            targets = []
            basis = shape_keys[0].data
            for key in shape_keys[1:]:
                target_path = path + "/" + re.sub(r"[^A-Za-z0-9_]", "_", key.name)
                target = stage.define_prim(target_path, "BlendShape")
                point_indices = []
                offsets = []
                for index, (base, value) in enumerate(zip(basis, key.data)):
                    offset = value.co - base.co
                    if offset.length > 1e-8:
                        point_indices.append(index)
                        offsets.append(tuple(offset))
                target.set("pointIndices", point_indices, type="int[]", uniform=True)
                target.set("offsets", offsets, type="vector3f[]", uniform=True)
                names.append(key.name)
                targets.append(target_path)
            if names:
                schemas = list(prim.metadata("apiSchemas") or ())
                if "SkelBindingAPI" not in schemas:
                    schemas.append("SkelBindingAPI")
                prim.set_metadata("apiSchemas", schemas)
                prim.set("skel:blendShapes", names, type="token[]", uniform=True)
                prim.add_relationship("skel:blendShapeTargets", targets)
                prim.set("skel:blendShapeSource", "BLENDER_SHAPE_KEYS", type="token", custom=True)
        if obj.get("lightusd_groom_card", obj.get("groom_card", False)):
            prim.set("groom_card", True, type="bool")
            prim.set("groom_card_id", int(obj.get("lightusd_groom_card_id", obj.get("groom_card_id", 0))),
                     type="int", uniform=True)
        if obj.material_slots and obj.material_slots[0].material:
            material_path = "/Materials/" + obj.material_slots[0].material.name.replace(" ", "_")
            materialx.export_material(stage, obj.material_slots[0].material, material_path)
            prim.add_relationship("material:binding", [material_path])
        armature = usdskel.find_armature_modifier(obj)
        if armature and armature.name in (armature_exports or {}):
            _, skeleton_path, joints = armature_exports[armature.name]
            usdskel.export_skinning(stage, obj, prim, armature, skeleton_path, joints)
    return path


def export_file(filepath, selected=False):
    lightusd = load_lightusd()
    stage = lightusd.Stage.create()
    stage.up_axis = "Z"
    stage.meters_per_unit = 1.0
    stage.start_time = bpy.context.scene.frame_start
    stage.end_time = bpy.context.scene.frame_end
    stage.frames_per_second = bpy.context.scene.render.fps
    world = stage.define_prim("/World", "Xform")
    objects = [o for o in bpy.context.scene.objects if not selected or o.select_get()]
    armature_exports = {}
    for obj in objects:
        if obj.type == "ARMATURE":
            armature_path = _export_object(stage, obj, "/World", armature_exports)
            exported = armature_exports.get(obj.name)
            if exported and obj.animation_data and obj.animation_data.action:
                _, skeleton_path, joints = exported
                source_skeleton = obj.get("lightusd_usd_skeleton", skeleton_path)
                skinned_meshes = [candidate for candidate in objects
                                  if candidate.type == "MESH"
                                  and (usdskel.find_armature_modifier(candidate) == obj
                                       or candidate.get("lightusd_usd_skeleton") == skeleton_path)]
                skinned_meshes.extend(candidate for candidate in objects
                                      if candidate.type == "MESH"
                                      and candidate not in skinned_meshes
                                      and candidate.get("lightusd_usd_skeleton") == source_skeleton)
                usdskel.export_animation(stage, obj, armature_path + "/Animation",
                                         joints, skeleton_path, skinned_meshes)
    for obj in objects:
        if obj.type != "ARMATURE":
            parent_path = "/World"
            armature = usdskel.find_armature_modifier(obj)
            if armature and armature.name in armature_exports:
                parent_path = armature_exports[armature.name][0]
            else:
                source_skeleton = obj.get("lightusd_usd_skeleton")
                if source_skeleton:
                    matching = next((value for arm_name, value in armature_exports.items()
                                     if bpy.data.objects.get(arm_name).get("lightusd_usd_skeleton")
                                     == source_skeleton), None)
                    if matching:
                        parent_path = matching[0]
            _export_object(stage, obj, parent_path, armature_exports)
    physics.export_physics(stage, objects)
    ext = os.path.splitext(filepath)[1].lower()
    if ext == ".usdz":
        stage.save_usdz(filepath)
    else:
        stage.save(filepath, format="usda" if ext == ".usda" else "usdc")
    return stage
