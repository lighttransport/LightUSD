"""Blender-side LightUSD curve regression; run with Blender's Python."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402

OUT = os.environ.get("LIGHTUSD_BLENDER_TEST_OUT", "/tmp/lightusd_blender_regressions")
NURBS = os.path.join(ROOT, "tests/usda/blender-nurbs-groom.usda")
ANIMATED = os.path.join(ROOT, "tests/usda/blender-animated-groom.usda")


def reset():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def main():
    os.makedirs(OUT, exist_ok=True)
    lightusd = load_lightusd()
    reset()
    converter.import_file(NURBS)
    curves = [obj for obj in bpy.context.scene.objects if obj.type == "CURVE"]
    assert len(curves) == 1 and curves[0].data.splines[0].type == "NURBS"
    nurbs_out = os.path.join(OUT, "nurbs.usda")
    converter.export_file(nurbs_out)
    nurbs = list(lightusd.load(nurbs_out).prims_of_type("NurbsCurves"))
    assert len(nurbs) == 1 and list(nurbs[0].get("curveVertexCounts")) == [4]

    reset()
    converter.import_file(ANIMATED)
    animated_out = os.path.join(OUT, "animated.usda")
    converter.export_file(animated_out)
    text = open(animated_out, encoding="utf-8").read()
    assert "points.timeSamples" in text and "widths.timeSamples" in text
    report = {"nurbs_counts": list(nurbs[0].get("curveVertexCounts")),
              "animated_time_samples": True}
    with open(os.path.join(OUT, "report.json"), "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    print("LightUSD Blender curve regression passed:", OUT)


main()
