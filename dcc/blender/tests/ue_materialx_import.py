"""Import a UE-authored layered MaterialX graph and retain its topology."""

import json
import os
import sys
import xml.etree.ElementTree as ET

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402


source = os.environ.get("LIGHTUSD_UE_MATERIAL_USD")
assert source and os.path.isfile(source), "LIGHTUSD_UE_MATERIAL_USD is required"
converter.import_file(source)
material = bpy.data.materials.get("M_GraphRoundtrip")
assert material is not None
graph = json.loads(material["lightusd_materialx_graph"])
ids = {node["attributes"].get("info:id") for node in graph}
assert "ND_open_pbr_surface_surfaceshader" in ids
assert "ND_mix_surfaceshader" in ids
assert "ND_ue_MakeFloat3_MakeFloat3" in ids
mix = next(node for node in graph
           if node["attributes"].get("info:id") == "ND_mix_surfaceshader")
assert set(mix["connections"]) >= {"inputs:bg", "inputs:fg", "inputs:mix"}
library = os.path.splitext(source)[0] + ".functions.mtlx"
document = ET.parse(library).getroot()
assert document.find("./nodedef[@name='ND_ue_MakeFloat3_MakeFloat3']") is not None
assert document.find("./nodegraph/combine3") is not None
print("UE layered MaterialX Blender import passed", source)
