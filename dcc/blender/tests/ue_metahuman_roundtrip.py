"""Blender 5.2 round-trip for MetaHuman template USD exports from UE."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402


INPUT_DIR = os.environ.get(
    "LIGHTUSD_UE_BLENDER_INPUT", "/tmp/lightusd_ue_blender_roundtrip")
OUTPUT_DIR = os.environ.get(
    "LIGHTUSD_UE_BLENDER_OUTPUT", "/tmp/lightusd_ue_blender_roundtrip/out")


def reset_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        if collection.name != "Collection" and collection.users == 0:
            bpy.data.collections.remove(collection)


def check_scene(name):
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    skinned = [obj for obj in meshes if any(
        modifier.type == "ARMATURE" for modifier in obj.modifiers)]
    if len(armatures) != 1 or not meshes or not skinned:
        raise RuntimeError(
            f"{name}: expected one armature and a skinned mesh, got "
            f"armatures={len(armatures)} meshes={len(meshes)} skinned={len(skinned)}")
    return armatures[0], meshes, skinned


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    lightusd = load_lightusd()
    report = {"templates": {}, "backend": "LightUSD"}
    for name in ("face", "body"):
        source = os.path.join(INPUT_DIR, f"{name}.usda")
        output = os.path.join(OUTPUT_DIR, f"{name}_blender.usda")
        if not os.path.isfile(source):
            raise RuntimeError(f"Missing UE export: {source}")
        reset_scene()
        converter.import_file(source)
        armature, meshes, skinned = check_scene(name)
        source_bones = len(armature.data.bones)
        source_meshes = len(meshes)
        source_skinned_meshes = len(skinned)
        bpy.ops.object.select_all(action="DESELECT")
        armature.select_set(True)
        for mesh in meshes:
            mesh.select_set(True)
        bpy.context.view_layer.objects.active = armature
        converter.export_file(output, selected=True)
        stage = lightusd.load(output, composed=True, load_payloads=True)
        skeletons = list(stage.prims_of_type("Skeleton"))
        skel_roots = list(stage.prims_of_type("SkelRoot"))
        # UE face LOD variants may serialize as Xform prims with Mesh
        # attributes after composition; accept that representation as long as
        # geometry and the skin relationship are present.
        exported_meshes = [prim for prim in stage
                           if prim.type_name in ("Mesh", "Xform")
                           and "skel:skeleton" in prim.relationships
                           and prim.attribute("points").get() is not None]
        if len(skeletons) != 1 or not skel_roots or not exported_meshes:
            raise RuntimeError(
                f"{name}: Blender export lost skeletal USD data: "
                f"skeletons={len(skeletons)} roots={len(skel_roots)} "
                f"skinned_meshes={len(exported_meshes)}")
        stage.close()

        reset_scene()
        converter.import_file(output)
        imported_armature, imported_meshes, imported_skinned = check_scene(name)
        report["templates"][name] = {
            "source": source,
            "output": output,
            "source_armatures": 1,
            "source_meshes": source_meshes,
            "source_bones": source_bones,
            "source_skinned_meshes": source_skinned_meshes,
            "exported_skeletons": len(skeletons),
            "exported_skinned_meshes": len(exported_meshes),
            "imported_bones": len(imported_armature.data.bones),
            "imported_meshes": len(imported_meshes),
            "imported_skinned_meshes": len(imported_skinned),
        }
    with open(os.path.join(OUTPUT_DIR, "report.json"), "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    print("UE MetaHuman Blender roundtrip passed:", OUTPUT_DIR)


main()
