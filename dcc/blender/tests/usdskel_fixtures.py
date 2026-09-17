"""Blender regression for repository UsdSkel animation fixtures."""

import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402


def main():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    converter.import_file(os.path.join(ROOT, "tests/usda/skelanimation-full-001.usda"))
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    assert len(armatures) == 3
    assert sum(bool(obj.animation_data and obj.animation_data.action) for obj in armatures) == 3
    face_meshes = [obj for obj in meshes if obj.data.shape_keys]
    assert len(face_meshes) == 1
    assert len(face_meshes[0].data.shape_keys.key_blocks) == 4
    assert face_meshes[0].data.shape_keys.animation_data
    output = os.path.join(os.environ.get("LIGHTUSD_BLENDER_USDSKEL_FIXTURE_OUT", "/tmp"),
                          "usdskel_fixture_roundtrip.usda")
    os.makedirs(os.path.dirname(output), exist_ok=True)
    converter.export_file(output)
    stage = load_lightusd().load(output)
    assert len(list(stage.prims_of_type("Skeleton"))) == 3
    assert len(list(stage.prims_of_type("SkelAnimation"))) == 3
    assert len(list(stage.prims_of_type("BlendShape"))) == 3
    animated = [prim for prim in stage.prims_of_type("SkelAnimation")
                if prim.get("blendShapes")]
    assert len(animated) == 1
    assert len(animated[0].attribute("blendShapeWeights").timesamples) == 47
    print("LightUSD Blender UsdSkel fixture regression passed")


main()
