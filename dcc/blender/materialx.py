"""MaterialX-compatible graph conversion using the LightUSD authoring API."""

import json


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
    if preserve and material:
        graph = [{"name": n.name, "type": n.bl_idname} for n in nodes]
        shader.set("userProperties:lightusd:originalNodeGraph", json.dumps(graph), type="string", custom=True)
    mprim.set("outputs:surface", "surface", type="token")
    mprim.attribute("outputs:surface").connect(shader_path + ".outputs:out")
    shader.set("outputs:out", "out", type="token")
    return material_path


def import_material(stage, prim, name):
    import bpy
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.name = "OpenPBR (LightUSD)"
    shader = next((child for child in prim.children if child.type_name == "Shader"), prim)
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
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(bsdf.outputs.get("BSDF") or bsdf.outputs[0], output.inputs["Surface"])
    return material
