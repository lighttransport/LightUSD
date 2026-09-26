"""UsdMtlx: MaterialX shading networks embedded in USD (MaterialXConfigAPI).

The layout matches what Blender (4.x) exports and OpenUSD's usdMtlx reads:

    def Material "Mat" (prepend apiSchemas = ["MaterialXConfigAPI"])
    {
        string config:mtlx:version = "1.39"
        token outputs:mtlx:surface.connect = </Mat/Surface.outputs:surface>
        token outputs:surface.connect = </Mat/Preview.outputs:surface>   # optional fallback
        def Shader "Surface" { uniform token info:id = "ND_open_pbr_surface_surfaceshader" ... }
        def NodeGraph "NodeGraphs" { def Shader "image" { uniform token info:id = "ND_image_color3" } ... }
    }

Helpers:
  add_openpbr_material / add_standard_surface_material   one-call PBR material (+textures)
  add_mtlx_material, add_node, add_nodegraph_output      generic network authoring
  read_material                                         network -> plain dict
  material_to_mtlx / mtlx_to_material                   standalone .mtlx XML <-> USD
  reference_mtlx_file                                   reference a .mtlx via the usdMtlx file format
"""

import xml.etree.ElementTree as ET

from .values import AssetPath, flatten, split_type

DEFAULT_VERSION = "1.39"

# USD value type <-> MaterialX type
USD_TO_MTLX = {"float": "float", "int": "integer", "bool": "boolean", "string": "string",
               "token": "string", "asset": "filename", "color3f": "color3", "color4f": "color4",
               "float2": "vector2", "float3": "vector3", "float4": "vector4",
               "normal3f": "vector3", "vector3f": "vector3", "point3f": "vector3",
               "texCoord2f": "vector2", "matrix3d": "matrix33", "matrix4d": "matrix44",
               "float[]": "floatarray", "int[]": "integerarray", "color3f[]": "color3array",
               "float2[]": "vector2array", "float3[]": "vector3array"}
MTLX_TO_USD = {"float": "float", "integer": "int", "boolean": "bool", "string": "string",
               "filename": "asset", "color3": "color3f", "color4": "color4f", "vector2": "float2",
               "vector3": "float3", "vector4": "float4", "matrix33": "matrix3d",
               "matrix44": "matrix4d", "surfaceshader": "token", "displacementshader": "token",
               "volumeshader": "token", "material": "token", "BSDF": "token", "EDF": "token",
               "VDF": "token", "floatarray": "float[]", "integerarray": "int[]",
               "color3array": "color3f[]", "vector2array": "float2[]", "vector3array": "float3[]"}
_MTLX_TYPES = set(MTLX_TO_USD) | {"color3FA", "color4FA", "vector2FA", "vector3FA", "vector4FA",
                                  "integer", "float"}

SURFACE_OUTPUTS = {"surfaceshader": "surface", "displacementshader": "displacement",
                   "volumeshader": "volume"}


def parse_nodedef(nodedef):
    """'ND_open_pbr_surface_surfaceshader' -> ('open_pbr_surface', 'surfaceshader')."""
    name = nodedef[3:] if nodedef.startswith("ND_") else nodedef
    parts = name.split("_")
    out_type = None
    while len(parts) > 1 and (parts[-1] in _MTLX_TYPES or parts[-1].rstrip("FAI") in _MTLX_TYPES):
        t = parts.pop()
        if out_type is None:
            out_type = t if t in MTLX_TO_USD else t.rstrip("FAI")
    return "_".join(parts), out_type or "float"


_REDUCE = {"dotproduct", "magnitude", "distance", "extract", "separate2", "separate3", "separate4",
           "crossproduct", "normalize", "transformpoint", "transformvector", "transformnormal"}
_ARITH_FA = {"add", "subtract", "multiply", "divide", "modulo", "power", "min", "max", "clamp",
             "smoothstep", "remap"}


def infer_nodedef(category, out_type, input_types):
    """Best-effort ND_* name for a node without an explicit nodedef."""
    if category == "convert" and input_types.get("in"):
        return "ND_convert_%s_%s" % (input_types["in"], out_type)
    first = next(iter(input_types.values()), None)
    if category in _REDUCE and first and first != out_type:
        return "ND_%s_%s" % (category, first)
    if category in _ARITH_FA and out_type not in ("float", "integer") and \
            any(t == "float" for n, t in input_types.items() if n != "in1" and n != "in"):
        return "ND_%s_%sFA" % (category, out_type)
    if category == "mix" and input_types.get("mix") not in (None, "float") and out_type != "float":
        return "ND_mix_%s_%s" % (out_type, out_type)
    return "ND_%s_%s" % (category, out_type)


def _define(parent, path, type_name):
    if hasattr(parent, "root"):
        return parent.define(path, type_name)
    parts = [x for x in str(path).split("/") if x]
    for part in parts[:-1]:
        parent = parent.children.get(part) or parent.define(part)
    return parent.define(parts[-1], type_name)


# ---------------------------------------------------------------------------
# generic authoring
# ---------------------------------------------------------------------------

def add_mtlx_material(stage, path, version=DEFAULT_VERSION):
    """Material prim with MaterialXConfigAPI (config:mtlx:version)."""
    mat = _define(stage, path, "Material")
    mat.apply_api("MaterialXConfigAPI")
    mat.create_attribute("config:mtlx:version", "string", version)
    return mat


def _input_type(value, type_name):
    if type_name:
        return type_name
    if isinstance(value, AssetPath):
        return "asset"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int"
    if isinstance(value, float):
        return "float"
    if isinstance(value, str):
        return "string"
    if isinstance(value, (tuple, list)):
        return {2: "float2", 3: "color3f", 4: "float4"}.get(len(value), "float3")
    raise TypeError("cannot infer MaterialX input type for %r" % (value,))


def add_node(parent, name, nodedef, inputs=None, output_type=None, output_name="out"):
    """Shader node with info:id = nodedef under a Material or NodeGraph.
    inputs: {name: value | (usd_type, value) | Connection}. Returns the output attribute."""
    node = parent.define(name, "Shader")
    node.create_attribute("info:id", "token", nodedef, uniform=True)
    for k, v in (inputs or {}).items():
        set_input(node, k, v)
    cat, mtype = parse_nodedef(nodedef)
    utype = output_type or MTLX_TO_USD.get(mtype, "float")
    if mtype in SURFACE_OUTPUTS and output_name == "out":
        output_name = SURFACE_OUTPUTS[mtype]
    return node.create_attribute("outputs:" + output_name, utype)


class Connection:
    """Connect an input to an output attribute (or its path): Connection(out_attr, 'color3f')."""

    def __init__(self, source, type_name=None):
        self.source = getattr(source, "path", source)
        self.type_name = type_name or getattr(source, "type_name", None)


def set_input(node, name, value):
    if isinstance(value, Connection):
        return node.create_attribute("inputs:" + name, value.type_name or "float").connect(value.source)
    if hasattr(value, "is_attribute") and value.is_attribute:  # an output attribute
        return node.create_attribute("inputs:" + name, value.type_name).connect(value.path)
    t = None
    if isinstance(value, tuple) and len(value) == 2 and isinstance(value[0], str) \
            and split_type(value[0])[0] is not None:
        t, value = value
    return node.create_attribute("inputs:" + name, _input_type(value, t), value)


def add_nodegraph_output(graph, name, source, type_name=None):
    """Expose a node output on a NodeGraph (outputs:<name>.connect = source)."""
    return graph.create_attribute("outputs:" + name, type_name or source.type_name).connect(source.path)


def connect_surface(material, surface_output, render_context="mtlx"):
    """material.outputs:mtlx:surface -> shader output."""
    return material.create_attribute("outputs:%s:surface" % render_context, "token").connect(
        getattr(surface_output, "path", surface_output))


# ---------------------------------------------------------------------------
# one-call PBR materials
# ---------------------------------------------------------------------------

_OPENPBR = {"base_color": "base_color", "metalness": "base_metalness", "roughness": "specular_roughness",
            "opacity": "geometry_opacity", "emission_color": "emission_color",
            "emission": "emission_luminance", "specular_ior": "specular_ior",
            "coat": "coat_weight", "transmission": "transmission_weight"}
_STDSURF = {"base_color": "base_color", "metalness": "metalness", "roughness": "specular_roughness",
            "opacity": "opacity", "emission_color": "emission_color", "emission": "emission",
            "specular_ior": "specular_IOR", "coat": "coat", "transmission": "transmission"}


def _pbr_material(stage, path, surface, names, base_color, metalness, roughness, opacity,
                  emission_color, emission, base_color_texture, roughness_texture, normal_texture,
                  preview_fallback, version, uv_set, extra_inputs):
    mat = add_mtlx_material(stage, path, version)
    inputs = {names["base_color"]: ("color3f", base_color), names["metalness"]: ("float", metalness),
              names["roughness"]: ("float", roughness)}
    if opacity != 1.0:
        inputs[names["opacity"]] = ("float" if surface == "open_pbr_surface" else "color3f",
                                    opacity if surface == "open_pbr_surface" else (opacity,) * 3)
    if emission_color is not None:
        inputs[names["emission_color"]] = ("color3f", emission_color)
        inputs[names["emission"]] = ("float", emission)
    inputs.update(extra_inputs or {})

    if any(t is not None for t in (base_color_texture, roughness_texture, normal_texture)):
        graph = mat.define("NodeGraphs", "NodeGraph")
        st = add_node(graph, "texcoord", "ND_texcoord_vector2", {"index": ("int", uv_set)})

        def image(node_name, file, itype, cs):
            nd = {"color3f": "ND_image_color3", "float": "ND_image_float", "float3": "ND_image_vector3"}[itype]
            out = add_node(graph, node_name, nd, {"texcoord": st})
            node = graph.children[node_name]
            f = node.create_attribute("inputs:file", "asset", AssetPath(file))
            if cs:
                f.metadata["colorSpace"] = cs
            return out

        if base_color_texture is not None:
            o = image("base_color_image", base_color_texture, "color3f", "srgb_texture")
            inputs[names["base_color"]] = Connection(add_nodegraph_output(graph, "base_color_out", o))
        if roughness_texture is not None:
            o = image("roughness_image", roughness_texture, "float", None)
            inputs[names["roughness"]] = Connection(add_nodegraph_output(graph, "roughness_out", o))
        if normal_texture is not None:
            o = image("normal_image", normal_texture, "float3", None)
            nm = add_node(graph, "normalmap", "ND_normalmap_float", {"in": o}, output_type="float3")
            key = "geometry_normal" if surface == "open_pbr_surface" else "normal"
            inputs[key] = Connection(add_nodegraph_output(graph, "normal_out", nm))

    nodedef = "ND_%s_surfaceshader" % surface
    out = add_node(mat, "Surface", nodedef, inputs)
    connect_surface(mat, out)
    if preview_fallback:
        _add_preview_fallback(mat, base_color, metalness, roughness, opacity, emission_color,
                              base_color_texture)
    return mat


def _add_preview_fallback(mat, base_color, metalness, roughness, opacity, emission_color, texture):
    ps = mat.define("PreviewSurface", "Shader")
    ps.create_attribute("info:id", "token", "UsdPreviewSurface", uniform=True)
    ps.create_attribute("inputs:metallic", "float", metalness)
    ps.create_attribute("inputs:roughness", "float", roughness)
    ps.create_attribute("inputs:opacity", "float", opacity)
    if emission_color is not None:
        ps.create_attribute("inputs:emissiveColor", "color3f", emission_color)
    if texture is not None:
        rd = mat.define("PreviewPrimvarReader", "Shader")
        rd.create_attribute("info:id", "token", "UsdPrimvarReader_float2", uniform=True)
        rd.create_attribute("inputs:varname", "token", "st")
        r_out = rd.create_attribute("outputs:result", "float2")
        tx = mat.define("PreviewTexture", "Shader")
        tx.create_attribute("info:id", "token", "UsdUVTexture", uniform=True)
        tx.create_attribute("inputs:file", "asset", AssetPath(texture))
        tx.create_attribute("inputs:sourceColorSpace", "token", "sRGB")
        tx.create_attribute("inputs:st", "float2").connect(r_out.path)
        t_out = tx.create_attribute("outputs:rgb", "float3")
        ps.create_attribute("inputs:diffuseColor", "color3f").connect(t_out.path)
    else:
        ps.create_attribute("inputs:diffuseColor", "color3f", base_color)
    out = ps.create_attribute("outputs:surface", "token")
    mat.create_attribute("outputs:surface", "token").connect(out.path)


def add_openpbr_material(stage, path, base_color=(0.8, 0.8, 0.8), metalness=0.0, roughness=0.3,
                         opacity=1.0, emission_color=None, emission=1.0, base_color_texture=None,
                         roughness_texture=None, normal_texture=None, preview_fallback=True,
                         version=DEFAULT_VERSION, uv_set=0, extra_inputs=None):
    """OpenPBR Surface material (Blender 4.2+ export style), with an optional
    UsdPreviewSurface fallback for renderers without MaterialX."""
    return _pbr_material(stage, path, "open_pbr_surface", _OPENPBR, base_color, metalness, roughness,
                         opacity, emission_color, emission, base_color_texture, roughness_texture,
                         normal_texture, preview_fallback, version, uv_set, extra_inputs)


def add_standard_surface_material(stage, path, base_color=(0.8, 0.8, 0.8), metalness=0.0,
                                  roughness=0.3, opacity=1.0, emission_color=None, emission=1.0,
                                  base_color_texture=None, roughness_texture=None,
                                  normal_texture=None, preview_fallback=True,
                                  version=DEFAULT_VERSION, uv_set=0, extra_inputs=None):
    """Autodesk Standard Surface material."""
    return _pbr_material(stage, path, "standard_surface", _STDSURF, base_color, metalness, roughness,
                         opacity, emission_color, emission, base_color_texture, roughness_texture,
                         normal_texture, preview_fallback, version, uv_set, extra_inputs)


def reference_mtlx_file(stage, path, mtlx_file, material_name):
    """Material prim referencing a material inside a .mtlx document (OpenUSD's
    usdMtlx file format exposes them under /MaterialX/Materials/<name>)."""
    mat = _define(stage, path, "Material")
    mat.add_reference(mtlx_file, "/MaterialX/Materials/" + material_name)
    return mat


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------

def _resolve(stage, path):
    prim_path, _, prop = path.rpartition(".")
    prim = stage.prim_at(prim_path)
    return prim, prop


def read_node(stage, node):
    """{'id': nodedef, 'inputs': {name: value | {'connect': path, 'node': {...}}}}"""
    info = {"path": node.path, "id": node.get("info:id"), "inputs": {}}
    for name, a in node.properties.items():
        if not name.startswith("inputs:") or not a.is_attribute:
            continue
        key = name[7:]
        if a.connections is not None and a.connections.items():
            src = a.connections.items()[0]
            prim, prop = _resolve(stage, src)
            entry = {"connect": src}
            # follow NodeGraph outputs to the producing node
            seen = 0
            while prim is not None and prim.type_name == "NodeGraph" and seen < 16:
                oa = prim.attribute(prop)
                if oa is None or oa.connections is None or not oa.connections.items():
                    break
                src = oa.connections.items()[0]
                prim, prop = _resolve(stage, src)
                seen += 1
            if prim is not None and prim.type_name == "Shader":
                entry["node"] = read_node(stage, prim)
                entry["output"] = prop
            info["inputs"][key] = entry
        elif a.default is not None:
            info["inputs"][key] = a.default
    return info


def read_material(stage, material):
    """Summary of a Material: {'mtlx_version', 'surface': node info (mtlx context),
    'preview': node info (UsdPreviewSurface), 'textures': [files]}."""
    material = stage.prim_at(material) if isinstance(material, str) else material
    out = {"path": material.path, "mtlx_version": material.get("config:mtlx:version"),
           "surface": None, "preview": None, "textures": []}
    for ctx, key in (("outputs:mtlx:surface", "surface"), ("outputs:surface", "preview")):
        a = material.attribute(ctx)
        if a is not None and a.connections is not None and a.connections.items():
            prim, _ = _resolve(stage, a.connections.items()[0])
            if prim is not None:
                out[key] = read_node(stage, prim)

    def collect(info):
        for v in (info or {}).get("inputs", {}).values():
            if isinstance(v, AssetPath) and str(v) not in out["textures"]:
                out["textures"].append(str(v))
            if isinstance(v, dict):
                collect(v.get("node"))
    collect(out["surface"])
    collect(out["preview"])
    return out


# ---------------------------------------------------------------------------
# MaterialX XML documents
# ---------------------------------------------------------------------------

def _fmt_mtlx(value, usd_type=None):
    if usd_type and (usd_type.startswith(("float", "color", "normal", "vector", "point", "texCoord",
                                          "half"))):
        from .usda_writer import fmt_float
        return ", ".join(fmt_float(v) for v in flatten(value))
    if isinstance(value, bool):
        return "true" if value else "false"
    if hasattr(value, "tolist"):
        value = value.tolist()
    if isinstance(value, (tuple, list)):
        flat = []
        for v in value:
            flat.extend(v if isinstance(v, (tuple, list)) else [v])
        return ", ".join(_fmt_mtlx(v) for v in flat)
    if isinstance(value, float):
        return repr(value).rstrip("0").rstrip(".") if "." in repr(value) and "e" not in repr(value) \
            else repr(value)
    return str(value)


def _parse_mtlx_value(text, usd_type):
    vt, is_array = split_type(usd_type)
    if vt is None:
        return text
    if vt.kind in ("string", "token"):
        return text
    if vt.kind == "asset":
        return AssetPath(text)
    if vt.fmt == "?":
        return text.strip().lower() == "true"
    nums = [float(x) for x in text.replace(",", " ").split()] if text.strip() else []
    if vt.fmt in "iI":
        nums = [int(x) for x in nums]
    if is_array:
        return nums
    return nums[0] if vt.n == 1 else tuple(nums)


def _node_elem(stage, node, parent_el, graph_prim):
    nodedef = node.get("info:id") or ""
    cat, mtype = parse_nodedef(nodedef)
    outs = [a for n, a in node.properties.items() if n.startswith("outputs:") and a.is_attribute]
    if len(outs) == 1 and mtype not in SURFACE_OUTPUTS:
        mtype = USD_TO_MTLX.get(outs[0].type_name, mtype)
    el = ET.SubElement(parent_el, cat, name=node.name, type=mtype)
    if nodedef.startswith("ND_"):
        el.set("nodedef", nodedef)
    for pname, a in node.properties.items():
        if not pname.startswith("inputs:") or not a.is_attribute:
            continue
        connected = a.connections is not None and bool(a.connections.items())
        if not connected and a.default is None:
            continue  # declared but unauthored: the nodedef default applies
        inp = ET.SubElement(el, "input", name=pname[7:], type=USD_TO_MTLX.get(a.type_name, "float"))
        if connected:
            src_prim, prop = _resolve(stage, a.connections.items()[0])
            out_name = prop.split(":", 1)[1] if ":" in prop else prop
            if src_prim is not None and src_prim is graph_prim and prop.startswith("inputs:"):
                inp.set("interfacename", prop[7:])
            elif src_prim is not None and src_prim.type_name == "NodeGraph" and src_prim is not graph_prim:
                inp.set("nodegraph", src_prim.name)
                inp.set("output", out_name)
            elif src_prim is not None:
                inp.set("nodename", src_prim.name)
                if out_name not in ("out", "surface"):
                    inp.set("output", out_name)
        elif a.default is not None:
            inp.set("value", _fmt_mtlx(a.default, a.type_name))
            cs = a.metadata.get("colorSpace")
            if cs:
                inp.set("colorspace", cs)
    return el


def material_to_mtlx(stage, material, colorspace="lin_rec709"):
    """Serialize a USD-embedded MaterialX network to a standalone .mtlx XML string."""
    material = stage.prim_at(material) if isinstance(material, str) else material
    version = material.get("config:mtlx:version") or DEFAULT_VERSION
    root = ET.Element("materialx", version=version, colorspace=colorspace)
    for child in material.children.values():
        if child.type_name == "NodeGraph":
            g = ET.SubElement(root, "nodegraph", name=child.name)
            for pname, a in child.properties.items():
                if pname.startswith("inputs:") and a.is_attribute and a.default is not None:
                    ET.SubElement(g, "input", name=pname[7:], type=USD_TO_MTLX.get(a.type_name, "float"),
                                  value=_fmt_mtlx(a.default, a.type_name))
            for n in child.children.values():
                if n.type_name == "Shader":
                    _node_elem(stage, n, g, child)
            for pname, a in child.properties.items():
                if pname.startswith("outputs:") and a.is_attribute and a.connections is not None:
                    src_prim, prop = _resolve(stage, a.connections.items()[0])
                    o = ET.SubElement(g, "output", name=pname[8:], type=USD_TO_MTLX.get(a.type_name, "float"))
                    if src_prim is not None:
                        o.set("nodename", src_prim.name)
    surf = material.attribute("outputs:mtlx:surface")
    surface_node = None
    if surf is not None and surf.connections is not None and surf.connections.items():
        surface_node, _ = _resolve(stage, surf.connections.items()[0])
    if surface_node is None or not (surface_node.get("info:id") or "").startswith("ND_"):
        raise ValueError("%s has no MaterialX (ND_*) surface shader on outputs:mtlx:surface"
                         % material.path)
    for child in material.children.values():
        if child.type_name == "Shader" and (child.get("info:id") or "").startswith("ND_"):
            _node_elem(stage, child, root, None)
    m = ET.SubElement(root, "surfacematerial", name=material.name, type="material")
    if surface_node is not None:
        ET.SubElement(m, "input", name="surfaceshader", type="surfaceshader", nodename=surface_node.name)
    ET.indent(root, "  ")
    return '<?xml version="1.0"?>\n' + ET.tostring(root, encoding="unicode") + "\n"


def _load_includes(root, base_dir, depth=0):
    """Inline <include filename=".."/> and <xi:include href=".."/> elements
    (resolved against base_dir)."""
    import os
    for inc in [e for e in root if e.tag in ("include", "{http://www.w3.org/2001/XInclude}include")]:
        root.remove(inc)
        fn = inc.get("filename") or inc.get("href")
        if base_dir is None or not fn or depth > 8:
            continue
        path = os.path.join(base_dir, fn)
        if not os.path.exists(path):
            continue
        sub = ET.parse(path).getroot()
        _load_includes(sub, os.path.dirname(path), depth + 1)
        for el in list(sub):
            root.insert(0, el)


def mtlx_file_to_material(stage, parent_path, mtlx_path, preview_fallback=False):
    """mtlx_to_material() for a file on disk (resolves <include>)."""
    import os
    with open(mtlx_path, encoding="utf-8") as f:
        return mtlx_to_material(stage, parent_path, f.read(), preview_fallback,
                                base_dir=os.path.dirname(os.path.abspath(mtlx_path)))


def mtlx_to_material(stage, parent_path, xml_text, preview_fallback=False, base_dir=None):
    """Import the materials of a .mtlx XML document as embedded USD networks
    under `parent_path`. Returns the list of Material prims."""
    root = ET.fromstring(xml_text)
    _load_includes(root, base_dir)
    version = root.get("version", DEFAULT_VERSION)
    scope = stage.prim_at(parent_path) or stage.define(parent_path, "Scope")

    def nodedef_of(el):
        nd = el.get("nodedef")
        if nd:
            return nd
        return infer_nodedef(el.tag, el.get("type", "float"),
                             {i.get("name"): i.get("type") for i in el.findall("input")})

    def build_nodes(container_el, prim, pending):
        for el in container_el:
            if el.tag in ("input", "output", "nodegraph", "surfacematerial", "nodedef", "look",
                          "materialassign", "collection", "typedef", "implementation",
                          "include") or not el.get("name") or not isinstance(el.tag, str):
                continue
            nd = nodedef_of(el)
            _, mtype = parse_nodedef(nd)
            node = prim.define(el.get("name"), "Shader")
            node.create_attribute("info:id", "token", nd, uniform=True)
            out_name = SURFACE_OUTPUTS.get(mtype, "out")
            node.create_attribute("outputs:" + out_name, MTLX_TO_USD.get(el.get("type", "float"), "float"))
            for inp in el.findall("input"):
                utype = MTLX_TO_USD.get(inp.get("type", "float"), "float")
                a = node.create_attribute("inputs:" + inp.get("name"), utype)
                if inp.get("value") is not None:
                    a.set(_parse_mtlx_value(inp.get("value"), utype))
                    if inp.get("colorspace"):
                        a.metadata["colorSpace"] = inp.get("colorspace")
                else:
                    pending.append((a, inp, prim))

    def resolve(pending, graphs, graph_parent):
        for a, inp, container in pending:
            if inp.get("nodegraph"):
                g = graphs.get(inp.get("nodegraph"))
                if g is not None:
                    a.connect(g.path + ".outputs:" + (inp.get("output") or "out"))
            elif inp.get("interfacename"):
                a.connect(container.path + ".inputs:" + inp.get("interfacename"))
            elif inp.get("nodename"):
                src = container.children.get(inp.get("nodename")) or graph_parent.children.get(inp.get("nodename"))
                if src is not None:
                    outs = [n for n in src.properties if n.startswith("outputs:")]
                    oname = "outputs:" + inp.get("output") if inp.get("output") else (outs[0] if outs else "outputs:out")
                    a.connect(src.path + "." + oname)

    materials = []
    for mel in root.findall("surfacematerial"):
        mat = add_mtlx_material(stage, scope.path.rstrip("/") + "/" + mel.get("name"), version)
        pending, graphs = [], {}
        for gel in root.findall("nodegraph"):
            g = mat.define(gel.get("name"), "NodeGraph")
            graphs[gel.get("name")] = g
            for iel in gel.findall("input"):  # graph interface
                utype = MTLX_TO_USD.get(iel.get("type", "float"), "float")
                ia = g.create_attribute("inputs:" + iel.get("name"), utype)
                if iel.get("value") is not None:
                    ia.set(_parse_mtlx_value(iel.get("value"), utype))
            gpend = []
            build_nodes(gel, g, gpend)
            resolve(gpend, graphs, g)
            for oel in gel.findall("output"):
                src = g.children.get(oel.get("nodename"))
                utype = MTLX_TO_USD.get(oel.get("type", "float"), "float")
                o = g.create_attribute("outputs:" + oel.get("name"), utype)
                if src is not None:
                    outs = [n for n in src.properties if n.startswith("outputs:")]
                    o.connect(src.path + "." + (outs[0] if outs else "outputs:out"))
        build_nodes(root, mat, pending)
        resolve(pending, graphs, mat)
        for inp in mel.findall("input"):
            if inp.get("name") == "surfaceshader" and inp.get("nodename") in mat.children:
                connect_surface(mat, mat.children[inp.get("nodename")].attribute("outputs:surface"))
        if preview_fallback:
            _add_preview_fallback(mat, (0.8, 0.8, 0.8), 0.0, 0.5, 1.0, None, None)
        materials.append(mat)
    return materials


__all__ = ["add_mtlx_material", "add_node", "set_input", "Connection", "add_nodegraph_output",
           "connect_surface", "add_openpbr_material", "add_standard_surface_material",
           "reference_mtlx_file", "read_material", "read_node", "material_to_mtlx",
           "mtlx_to_material", "mtlx_file_to_material", "parse_nodedef", "infer_nodedef"]
