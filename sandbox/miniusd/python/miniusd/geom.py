"""Convenience builders for common UsdGeom / UsdShade / UsdLux prims.

Aimed at scripts and LLM/coding agents that construct 3D scenes:

    from miniusd import Stage, geom
    stage = Stage(up_axis="Y")
    world = geom.add_xform(stage, "/World")
    mesh = geom.add_mesh(stage, "/World/Tri", points=[(0,0,0), (1,0,0), (0,1,0)],
                         face_vertex_counts=[3], face_vertex_indices=[0, 1, 2])
    mat = geom.add_preview_material(stage, "/World/Looks/Red", diffuse_color=(1, 0, 0))
    geom.bind_material(mesh, mat)
    stage.save("tri.usdz")
"""

import math

from .values import BLOCK, AssetPath, _is_np, flatten, infer_type

# Well-known schema attributes: name -> (type, uniform)
ATTR_TYPES = {
    "points": ("point3f[]", False), "normals": ("normal3f[]", False),
    "velocities": ("vector3f[]", False), "accelerations": ("vector3f[]", False),
    "faceVertexCounts": ("int[]", False), "faceVertexIndices": ("int[]", False),
    "holeIndices": ("int[]", False), "cornerIndices": ("int[]", False),
    "cornerSharpnesses": ("float[]", False), "creaseIndices": ("int[]", False),
    "creaseLengths": ("int[]", False), "creaseSharpnesses": ("float[]", False),
    "extent": ("float3[]", False), "widths": ("float[]", False),
    "curveVertexCounts": ("int[]", False), "radius": ("double", False),
    "size": ("double", False), "height": ("double", False), "axis": ("token", True),
    "visibility": ("token", False), "purpose": ("token", True),
    "orientation": ("token", True), "doubleSided": ("bool", True),
    "subdivisionScheme": ("token", True), "interpolateBoundary": ("token", False),
    "faceVaryingLinearInterpolation": ("token", False), "triangleSubdivisionRule": ("token", False),
    "xformOpOrder": ("token[]", True),
    "xformOp:translate": ("double3", False), "xformOp:rotateXYZ": ("float3", False),
    "xformOp:rotateX": ("float", False), "xformOp:rotateY": ("float", False),
    "xformOp:rotateZ": ("float", False), "xformOp:scale": ("float3", False),
    "xformOp:orient": ("quatf", False), "xformOp:transform": ("matrix4d", False),
    "primvars:displayColor": ("color3f[]", False), "primvars:displayOpacity": ("float[]", False),
    "primvars:st": ("texCoord2f[]", False), "primvars:normals": ("normal3f[]", False),
    "focalLength": ("float", False), "horizontalAperture": ("float", False),
    "verticalAperture": ("float", False), "clippingRange": ("float2", False),
    "focusDistance": ("float", False), "fStop": ("float", False), "projection": ("token", False),
    "inputs:intensity": ("float", False), "inputs:exposure": ("float", False),
    "inputs:color": ("color3f", False), "inputs:angle": ("float", False),
    "inputs:radius": ("float", False), "inputs:width": ("float", False),
    "inputs:height": ("float", False), "inputs:texture:file": ("asset", False),
    "info:id": ("token", True), "outputs:surface": ("token", False),
    "outputs:displacement": ("token", False), "outputs:volume": ("token", False),
}


def guess_attr_type(name, value):
    """Return (type_name, uniform) for an attribute, from ATTR_TYPES or the value."""
    if name in ATTR_TYPES:
        return ATTR_TYPES[name]
    if name.startswith("xformOp:"):
        base = name.split(":")[1]
        for k, v in ATTR_TYPES.items():
            if k.startswith("xformOp:" + base):
                return v
    t = infer_type(value)
    # prefer float precision for geometric values (USD convention)
    t = t.replace("double", "float") if t.startswith("double") and not name.startswith("xformOp") else t
    return t, False


def _flat3(points):
    f = flatten(points)
    return [tuple(f[i:i + 3]) for i in range(0, len(f), 3)]


def compute_extent(points):
    """[(minx, miny, minz), (maxx, maxy, maxz)] of a point list/array."""
    if _is_np(points):
        p = points.reshape(-1, 3)
        return [tuple(float(x) for x in p.min(axis=0)), tuple(float(x) for x in p.max(axis=0))]
    pts = _flat3(points)
    if not pts:
        return [(0.0, 0.0, 0.0), (0.0, 0.0, 0.0)]
    return [tuple(min(p[i] for p in pts) for i in range(3)),
            tuple(max(p[i] for p in pts) for i in range(3))]


def _prim(parent, path, type_name):
    """Define a prim at an absolute path of a Stage, or a relative path under a prim."""
    if hasattr(parent, "root"):
        return parent.define(path, type_name)
    parts = [x for x in str(path).split("/") if x]
    for part in parts[:-1]:
        parent = parent.children.get(part) or parent.define(part)
    return parent.define(parts[-1], type_name)


# -- transforms ---------------------------------------------------------------

def set_transform(prim, translate=None, rotate=None, scale=None, matrix=None, orient=None):
    """Author xformOps: translate (double3), rotateXYZ in degrees (float3),
    orient quaternion (w, x, y, z) (quatf), scale (float3) or a full matrix4d
    (row-major, translation in the last row, as USD)."""
    order = []
    if matrix is not None:
        prim.create_attribute("xformOp:transform", "matrix4d", matrix)
        order.append("xformOp:transform")
    else:
        if translate is not None:
            prim.create_attribute("xformOp:translate", "double3", translate)
            order.append("xformOp:translate")
        if orient is not None:
            prim.create_attribute("xformOp:orient", "quatf", orient)
            order.append("xformOp:orient")
        elif rotate is not None:
            prim.create_attribute("xformOp:rotateXYZ", "float3", rotate)
            order.append("xformOp:rotateXYZ")
        if scale is not None:
            if isinstance(scale, (int, float)):
                scale = (scale, scale, scale)
            prim.create_attribute("xformOp:scale", "float3", scale)
            order.append("xformOp:scale")
    prim.create_attribute("xformOpOrder", "token[]", order, uniform=True)
    return prim


def add_xform(stage, path, translate=None, rotate=None, scale=None, matrix=None, orient=None):
    prim = _prim(stage, path, "Xform")
    if any(v is not None for v in (translate, rotate, scale, matrix, orient)):
        set_transform(prim, translate, rotate, scale, matrix, orient)
    return prim


def add_scope(stage, path):
    return _prim(stage, path, "Scope")


# -- geometry -----------------------------------------------------------------

def add_mesh(stage, path, points, face_vertex_counts, face_vertex_indices, normals=None,
             uvs=None, uv_indices=None, display_color=None, subdivision_scheme="none",
             normals_interpolation="vertex", uv_interpolation=None, double_sided=None):
    """Polygon mesh. uvs are per-vertex unless uv_indices or uv_interpolation
    ('faceVarying') say otherwise; display_color is one color or per-vertex."""
    m = _prim(stage, path, "Mesh")
    m.create_attribute("points", "point3f[]", points)
    m.create_attribute("faceVertexCounts", "int[]", face_vertex_counts)
    m.create_attribute("faceVertexIndices", "int[]", face_vertex_indices)
    m.create_attribute("extent", "float3[]", compute_extent(points))
    if normals is not None:
        m.create_attribute("normals", "normal3f[]", normals, interpolation=normals_interpolation)
    if uvs is not None:
        interp = uv_interpolation or ("faceVarying" if uv_indices is not None else "vertex")
        m.create_attribute("primvars:st", "texCoord2f[]", uvs, interpolation=interp)
        if uv_indices is not None:
            m.create_attribute("primvars:st:indices", "int[]", uv_indices)
    if display_color is not None:
        set_display_color(m, display_color)
    m.create_attribute("subdivisionScheme", "token", subdivision_scheme, uniform=True)
    if double_sided is not None:
        m.create_attribute("doubleSided", "bool", double_sided, uniform=True)
    return m


def set_display_color(prim, color):
    flat = flatten(color)
    n = len(flat) // 3
    prim.create_attribute("primvars:displayColor", "color3f[]", flat,
                          interpolation="constant" if n == 1 else "vertex")
    return prim


def add_cube(stage, path, size=2.0, display_color=None):
    c = _prim(stage, path, "Cube")
    c.create_attribute("size", "double", size)
    h = size / 2.0
    c.create_attribute("extent", "float3[]", [(-h, -h, -h), (h, h, h)])
    if display_color is not None:
        set_display_color(c, display_color)
    return c


def add_sphere(stage, path, radius=1.0, display_color=None):
    s = _prim(stage, path, "Sphere")
    s.create_attribute("radius", "double", radius)
    r = float(radius)
    s.create_attribute("extent", "float3[]", [(-r, -r, -r), (r, r, r)])
    if display_color is not None:
        set_display_color(s, display_color)
    return s


def add_cylinder(stage, path, radius=1.0, height=2.0, axis="Z", display_color=None):
    c = _prim(stage, path, "Cylinder")
    c.create_attribute("radius", "double", radius)
    c.create_attribute("height", "double", height)
    c.create_attribute("axis", "token", axis, uniform=True)
    r, h = float(radius), float(height) / 2.0
    lo, hi = [-r, -r, -r], [r, r, r]
    i = "XYZ".index(axis)
    lo[i], hi[i] = -h, h
    c.create_attribute("extent", "float3[]", [tuple(lo), tuple(hi)])
    if display_color is not None:
        set_display_color(c, display_color)
    return c


def uv_sphere_mesh(radius=1.0, segments=32, rings=16):
    """Generate (points, face_vertex_counts, face_vertex_indices, normals) of a UV sphere."""
    pts, nrm = [], []
    for r in range(rings + 1):
        th = math.pi * r / rings
        for s in range(segments):
            ph = 2 * math.pi * s / segments
            n = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            nrm.append(n)
            pts.append(tuple(radius * c for c in n))
    counts, idx = [], []
    for r in range(rings):
        for s in range(segments):
            a = r * segments + s
            b = r * segments + (s + 1) % segments
            idx += [a, b, b + segments, a + segments]
            counts.append(4)
    return pts, counts, idx, nrm


# -- materials ----------------------------------------------------------------

def add_preview_material(stage, path, diffuse_color=(0.18, 0.18, 0.18), roughness=0.5,
                         metallic=0.0, opacity=1.0, emissive_color=None, diffuse_texture=None,
                         normal_texture=None, roughness_texture=None, uv_primvar="st"):
    """Material with a UsdPreviewSurface shader. Texture args are asset paths
    (relative to the layer; put the files next to it or in the USDZ)."""
    mat = _prim(stage, path, "Material")
    sh = mat.define("PreviewSurface", "Shader")
    sh.create_attribute("info:id", "token", "UsdPreviewSurface", uniform=True)
    sh.create_attribute("inputs:roughness", "float", roughness)
    sh.create_attribute("inputs:metallic", "float", metallic)
    sh.create_attribute("inputs:opacity", "float", opacity)
    if emissive_color is not None:
        sh.create_attribute("inputs:emissiveColor", "color3f", emissive_color)
    out = sh.create_attribute("outputs:surface", "token")
    mat.create_attribute("outputs:surface", "token").connect(out.path)

    reader = None
    if any(t is not None for t in (diffuse_texture, normal_texture, roughness_texture)):
        reader = mat.define("PrimvarReader_" + uv_primvar, "Shader")
        reader.create_attribute("info:id", "token", "UsdPrimvarReader_float2", uniform=True)
        reader.create_attribute("inputs:varname", "token", uv_primvar)
        reader.create_attribute("outputs:result", "float2")

    def tex(name, file, out_name, out_type, color_space=None, **extra):
        t = mat.define(name, "Shader")
        t.create_attribute("info:id", "token", "UsdUVTexture", uniform=True)
        t.create_attribute("inputs:file", "asset", AssetPath(file))
        t.create_attribute("inputs:st", "float2").connect(reader.path + ".outputs:result")
        if color_space:
            t.create_attribute("inputs:sourceColorSpace", "token", color_space)
        for k, (tn, v) in extra.items():
            t.create_attribute("inputs:" + k, tn, v)
        return t.create_attribute("outputs:" + out_name, out_type)

    if diffuse_texture is not None:
        o = tex("DiffuseTexture", diffuse_texture, "rgb", "float3", "sRGB")
        sh.create_attribute("inputs:diffuseColor", "color3f").connect(o.path)
    else:
        sh.create_attribute("inputs:diffuseColor", "color3f", diffuse_color)
    if normal_texture is not None:
        o = tex("NormalTexture", normal_texture, "rgb", "float3", "raw",
                scale=("float4", (2, 2, 2, 1)), bias=("float4", (-1, -1, -1, 0)))
        sh.create_attribute("inputs:normal", "normal3f").connect(o.path)
    if roughness_texture is not None:
        o = tex("RoughnessTexture", roughness_texture, "r", "float", "raw")
        sh.properties.pop("inputs:roughness")
        sh.create_attribute("inputs:roughness", "float").connect(o.path)
    return mat


def bind_material(prim, material):
    """Bind a Material (prim or path) with MaterialBindingAPI."""
    prim.apply_api("MaterialBindingAPI")
    prim.create_relationship("material:binding", [getattr(material, "path", material)])
    return prim


# -- camera / lights -----------------------------------------------------------

def add_camera(stage, path, focal_length=50.0, horizontal_aperture=36.0, vertical_aperture=24.0,
               clipping_range=(0.1, 10000.0), translate=None, rotate=None):
    c = _prim(stage, path, "Camera")
    c.create_attribute("focalLength", "float", focal_length)
    c.create_attribute("horizontalAperture", "float", horizontal_aperture)
    c.create_attribute("verticalAperture", "float", vertical_aperture)
    c.create_attribute("clippingRange", "float2", clipping_range)
    if translate is not None or rotate is not None:
        set_transform(c, translate=translate, rotate=rotate)
    return c


def add_distant_light(stage, path, intensity=3.0, color=(1.0, 1.0, 1.0), angle=0.53, rotate=None):
    lt = _prim(stage, path, "DistantLight")
    lt.create_attribute("inputs:intensity", "float", intensity)
    lt.create_attribute("inputs:color", "color3f", color)
    lt.create_attribute("inputs:angle", "float", angle)
    if rotate is not None:
        set_transform(lt, rotate=rotate)
    return lt


def add_dome_light(stage, path, intensity=1.0, texture=None, color=(1.0, 1.0, 1.0)):
    lt = _prim(stage, path, "DomeLight")
    lt.create_attribute("inputs:intensity", "float", intensity)
    lt.create_attribute("inputs:color", "color3f", color)
    if texture is not None:
        lt.create_attribute("inputs:texture:file", "asset", AssetPath(texture))
    return lt


def add_sphere_light(stage, path, intensity=50.0, radius=0.5, color=(1.0, 1.0, 1.0), translate=None):
    lt = _prim(stage, path, "SphereLight")
    lt.create_attribute("inputs:intensity", "float", intensity)
    lt.create_attribute("inputs:radius", "float", radius)
    lt.create_attribute("inputs:color", "color3f", color)
    if translate is not None:
        set_transform(lt, translate=translate)
    return lt


__all__ = [n for n in dir() if n.startswith(("add_", "set_", "bind_", "compute_", "uv_", "guess_"))] \
    + ["ATTR_TYPES", "BLOCK"]
