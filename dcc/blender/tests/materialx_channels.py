"""Blender MaterialX/OpenPBR channel and unsupported-node regression."""

import json
import os
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, ROOT)
from dcc.blender import converter  # noqa: E402
from dcc.blender.preferences import load_lightusd  # noqa: E402


def main():
    output = os.environ.get("LIGHTUSD_BLENDER_MTLX_OUT",
                            "/tmp/lightusd_blender_materialx_channels.usda")
    material = bpy.data.materials.new("MaterialXChannels")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    principled = next(node for node in nodes
                      if node.bl_idname == "ShaderNodeBsdfPrincipled")
    output_node = next(node for node in nodes
                       if node.bl_idname == "ShaderNodeOutputMaterial")
    for name, value in (("Metallic", 0.35), ("Roughness", 0.42),
                        ("IOR", 1.45), ("Coat Weight", 0.2),
                        ("Emission Strength", 3.0)):
        socket = principled.inputs.get(name)
        if socket:
            socket.default_value = value
    emission = principled.inputs.get("Emission Color") or principled.inputs.get("Emission")
    if emission:
        emission.default_value = (0.1, 0.2, 0.8, 1.0)
    unsupported = nodes.new("ShaderNodeMixRGB")
    unsupported.name = "IntentionalUnsupportedMix"
    if principled.inputs.get("Base Color"):
        links.new(unsupported.outputs[0], principled.inputs["Base Color"])
    material.node_tree.nodes.active = output_node

    bpy.ops.mesh.primitive_cube_add()
    mesh_object = bpy.context.object
    mesh_object.data.materials.append(material)
    converter.export_file(output)
    stage = load_lightusd().load(output)
    shader = next(shader for shader in stage.prims_of_type("Shader")
                  if "MaterialXChannels" in shader.path)
    diagnostics = json.loads(shader.get("userProperties:lightusd:unsupportedNodes"))
    assert any(item["type"] == "ShaderNodeMixRGB" for item in diagnostics)
    assert shader.get("inputs:base_metalness") is not None
    assert shader.get("inputs:specular_ior") is not None
    assert shader.get("inputs:emission_strength") is not None
    print("LightUSD Blender MaterialX channel regression passed", output)


main()
