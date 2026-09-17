"""MaterialX-compatible graph conversion using the LightUSD authoring API."""

import json
import os
import re


def _socket(node, names):
    for name in names:
        socket = node.inputs.get(name)
        if socket:
            return socket
    return None


def _value(socket):
    value = socket.default_value
    if hasattr(value, "__iter__") and not isinstance(value, str):
        value = tuple(value)
        return value[:3] if len(value) == 4 else value
    return value


def _set_input(prim, name, socket, type_name=None):
    if socket and not socket.is_linked:
        prim.set("inputs:" + name, _value(socket), type=type_name)


def export_material(stage, material, material_path, preserve=True):
    mprim = stage.define_prim(material_path, "Material")
    shader_path = material_path + "/OpenPBR"
    shader = stage.define_prim(shader_path, "Shader")
    shader.set("info:id", "ND_open_pbr_surface_surfaceshader", type="token")
    nodes = material.node_tree.nodes if material and material.use_nodes else ()
    principled = next((n for n in nodes if n.bl_idname == "ShaderNodeBsdfPrincipled"), None)
    aliases = {
        "base_color": (["Base Color"], "color3f"),
        "base_metalness": (["Metallic"], "float"),
        "specular_roughness": (["Roughness"], "float"),
        "specular_ior": (["IOR"], "float"),
        "coat_weight": (["Coat Weight"], "float"),
        "coat_roughness": (["Coat Roughness"], "float"),
        "transmission_weight": (["Transmission Weight", "Transmission"], "float"),
        "opacity": (["Alpha"], "float"),
    }
    if principled:
        for target, (names, type_name) in aliases.items():
            _set_input(shader, target, _socket(principled, names), type_name)
        linked_textures = {
            "base_color": ("BaseColorTexture", ["Base Color"], "color3f", "color3f"),
            "base_metalness": ("MetallicTexture", ["Metallic"], "float", "float"),
            "specular_roughness": ("RoughnessTexture", ["Roughness"], "float", "float"),
            "opacity": ("OpacityTexture", ["Alpha"], "float", "float"),
        }
        for target, (node_name, socket_names, input_type, output_type) in linked_textures.items():
            linked_socket = _socket(principled, socket_names)
            source = linked_socket.links[0].from_node if linked_socket and linked_socket.is_linked else None
            if source and source.bl_idname == "ShaderNodeTexImage" and source.image:
                texture_path = shader_path + "/" + node_name
                texture = stage.define_prim(texture_path, "Shader")
                texture.set("info:id", "ND_image_" + output_type, type="token")
                filepath = source.image.filepath_raw or source.image.filepath
                texture.set("inputs:file", filepath, type="asset")
                texture.set("outputs:out", "out", type="token")
                shader.set("inputs:" + target,
                           (0.0, 0.0, 0.0) if input_type == "color3f" else 0.0,
                           type=input_type)
                shader.attribute("inputs:" + target).connect(texture_path + ".outputs:out")
    if preserve and material:
        graph = [{"name": n.name, "type": n.bl_idname} for n in nodes]
        shader.set("userProperties:lightusd:originalNodeGraph", json.dumps(graph), type="string", custom=True)
    mprim.set("outputs:surface", "surface", type="token")
    mprim.attribute("outputs:surface").connect(shader_path + ".outputs:out")
    shader.set("outputs:out", "out", type="token")
    return material_path


def import_material(stage, prim, name):
    import bpy
    shader = next((child for child in prim.children if child.type_name == "Shader"), prim)
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.name = "OpenPBR (LightUSD)"
    for source, targets in {
        "base_color": ["Base Color"], "base_metalness": ["Metallic"],
        "specular_roughness": ["Roughness"], "specular_ior": ["IOR"],
        "opacity": ["Alpha"],
    }.items():
        value = shader.get("inputs:" + source)
        socket = _socket(bsdf, targets)
        if value is not None and socket:
            try:
                socket.default_value = value
            except (TypeError, ValueError):
                pass
    texture_specs = {
        "base_color": ("BaseColorTexture", "Base Color"),
        "base_metalness": ("MetallicTexture", "Metallic"),
        "specular_roughness": ("RoughnessTexture", "Roughness"),
        "opacity": ("OpacityTexture", "Alpha"),
    }
    serialized = stage.export_usda()
    for source, (node_name, socket_name) in texture_specs.items():
        texture_shader = next((child for child in shader.children
                               if child.type_name == "Shader" and
                               child.path.endswith("/" + node_name)), None)
        texture_path = texture_shader.get("inputs:file") if texture_shader else None
        if not texture_path:
            match = re.search(r'def Shader "' + re.escape(node_name) +
                              r'".*?asset inputs:file = @([^@]+)@', serialized, re.DOTALL)
            texture_path = match.group(1) if match else None
        if not texture_path:
            continue
        image = None
        load_path = str(texture_path)
        if "<UDIM>" in load_path and not os.path.exists(load_path):
            load_path = load_path.replace("<UDIM>", "1001")
        if os.path.exists(load_path):
            try:
                image = bpy.data.images.load(load_path, check_existing=True)
            except RuntimeError:
                image = None
        if image is None:
            image = bpy.data.images.new(name + "_Texture", 1, 1)
        image.filepath = str(texture_path)
        if "<UDIM>" in str(texture_path):
            image.source = "TILED"
        image_node = tree.nodes.new("ShaderNodeTexImage")
        image_node.name = node_name + " (LightUSD)"
        image_node.image = image
        output_name = "Color" if socket_name == "Base Color" else "Color"
        tree.links.new(image_node.outputs.get(output_name), bsdf.inputs[socket_name])
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(bsdf.outputs.get("BSDF") or bsdf.outputs[0], output.inputs["Surface"])
    return material
