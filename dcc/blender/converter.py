import os
import re
from pathlib import Path

import bpy
from mathutils import Matrix

from . import materialx, physics
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


def import_file(filepath):
    lightusd = load_lightusd()
    pref = get_preferences()
    limit = pref.max_memory_mb * 1024 * 1024
    stage = lightusd.load(filepath, composed=True, load_payloads=True, max_memory=limit)
    collection = _new_collection(Path(filepath).stem)
    objects = {}
    for prim in stage:
        if prim.type_name in ("Material", "Shader", "Scope", "PhysicsScene"):
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
        elif prim.type_name == "BasisCurves":
            points = _attr(prim, "points") or []
            counts = _attr(prim, "curveVertexCounts") or []
            if hasattr(bpy.data, "hair_curves"):
                curves = bpy.data.hair_curves.new(prim.name)
                curves.add_curves(counts)
                curves.attributes["position"].data.foreach_set("vector", [v for p in points for v in p])
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
            objects[prim.path] = obj
            if parent and obj.parent is None:
                obj.parent = parent
    for prim in stage.prims_of_type("Material"):
        materialx.import_material(stage, prim, prim.name)
    physics.import_physics(stage, objects)
    if stage.start_time != stage.end_time:
        bpy.context.scene.frame_start = int(stage.start_time)
        bpy.context.scene.frame_end = int(stage.end_time)
    return stage


def _export_object(stage, obj, parent_path):
    name = re.sub(r"[^A-Za-z0-9_]", "_", obj.name)
    if not name or name[0].isdigit():
        name = "Object_" + name
    path = parent_path + "/" + name
    prim = stage.define_prim(path, "Mesh" if obj.type == "MESH" else "Xform")
    prim.set("xformOp:transform", tuple(v for row in obj.matrix_world for v in row), type="matrix4d")
    prim.set("xformOpOrder", ["xformOp:transform"], type="token[]")
    obj["lightusd_usd_path"] = path
    if obj.type == "MESH":
        mesh = obj.data
        prim.set("points", [tuple(v.co) for v in mesh.vertices], type="point3f[]")
        prim.set("faceVertexCounts", [len(p.vertices) for p in mesh.polygons], type="int[]")
        prim.set("faceVertexIndices", [i for p in mesh.polygons for i in p.vertices], type="int[]")
        if obj.material_slots and obj.material_slots[0].material:
            material_path = "/Materials/" + obj.material_slots[0].material.name.replace(" ", "_")
            materialx.export_material(stage, obj.material_slots[0].material, material_path)
            prim.add_relationship("material:binding", [material_path])
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
    for obj in objects:
        _export_object(stage, obj, "/World")
    physics.export_physics(stage, objects)
    ext = os.path.splitext(filepath)[1].lower()
    if ext == ".usdz":
        stage.save_usdz(filepath)
    else:
        stage.save(filepath, format="usda" if ext == ".usda" else "usdc")
    return stage
