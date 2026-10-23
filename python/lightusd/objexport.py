# SPDX-License-Identifier: Apache-2.0
"""Export USD meshes to Wavefront OBJ/MTL (with optional ``vw`` skin weights).

``export_obj(stage, "out.obj")`` writes every Mesh (or the given prims) into
one OBJ with an MTL next to it. Points are written in world space by default
(``world_space=False`` keeps each mesh's local coordinates); normals follow the
inverse-transpose. Normals and UVs honor USD interpolation and ``:indices``
(vertex, faceVarying, uniform, constant). Polygons are kept unless
``triangulate`` is set (fan triangulation). Material bindings, including
``materialBind`` GeomSubsets, become ``usemtl`` groups; UsdPreviewSurface
inputs become MTL ``Kd``/``d``/``Ke``/``Ni``/``Pr``/``Pm`` and a diffuse
texture becomes ``map_Kd``. ``skin_weights`` writes LightUSD's zero-based
``vw <vertex> <joint> <weight> ...`` rows from ``primvars:skel:*``.
"""
from __future__ import annotations

import math
import os
from dataclasses import dataclass
from typing import Iterable, Optional, Sequence

__all__ = ["ObjExport", "export_obj"]


@dataclass(frozen=True)
class ObjExport:
    """Counts written by :func:`export_obj`."""

    meshes: int
    vertices: int
    faces: int
    materials: int
    skinned_vertices: int


def _num(value: float) -> str:
    # Nine significant digits round-trip the float32 values USD stores.
    text = "%.9g" % value
    return "0" if text == "-0" else text


def _rows(value, width):
    flat = [float(v) for v in (value if value is not None else ())]
    if flat and isinstance(flat[0], (list, tuple)):
        flat = [float(x) for row in flat for x in row]
    return [tuple(flat[i:i + width]) for i in range(0, len(flat) - width + 1, width)]


def _flat_values(value):
    try:
        import numpy as np
        return np.asarray(value, dtype=float).ravel().tolist() if value is not None else []
    except (ImportError, TypeError, ValueError):
        pass
    out = []
    for item in value if value is not None else ():
        if isinstance(item, (list, tuple)):
            out.extend(float(x) for x in item)
        else:
            try:
                out.extend(float(x) for x in item)
            except TypeError:
                out.append(float(item))
    return out


def _matrix(prim):
    m = prim.world_transform()
    return [[float(m[r][c]) for c in range(4)] for r in range(4)]


def _apply_point(m, p):
    return tuple(p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + m[3][c] for c in range(3))


def _normal_matrix(m):
    # Inverse-transpose of the upper 3x3 for row vectors: n' = n * inv(A)^T.
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-300:
        return a
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] -
             a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det
            for j in range(3)] for i in range(3)]
    return [[inv[c][r] for c in range(3)] for r in range(3)]


def _apply_normal(nm, n):
    v = tuple(n[0] * nm[0][c] + n[1] * nm[1][c] + n[2] * nm[2][c] for c in range(3))
    length = math.sqrt(sum(x * x for x in v)) or 1.0
    return tuple(x / length for x in v)


def _primvar(prim, names, width, corners, counts, points):
    """Per-corner values for the first present primvar, honoring indices."""
    for name in names:
        if name not in prim.attributes:
            continue
        values = _rows(_flat_values(prim[name]), width)
        attribute = prim.attribute(name)
        interpolation = attribute.interpolation or ("vertex" if name == "normals" else "constant")
        if f"{name}:indices" in prim.attributes:
            values = [values[int(i)] for i in prim[f"{name}:indices"]]
        if interpolation == "faceVarying" and len(values) == len(corners):
            return values
        if interpolation in ("vertex", "varying") and len(values) == points:
            return [values[v] for v in corners]
        if interpolation == "uniform" and len(values) == len(counts):
            return [values[f] for f, n in enumerate(counts) for _ in range(n)]
        if interpolation == "constant" and values:
            return [values[0]] * len(corners)
    return None


def _material_of(stage, prim):
    if "material:binding" in prim.relationships:
        targets = prim.relationship("material:binding").targets
        if targets:
            return targets[0]
    return None


def _shader_inputs(stage, material_path):
    """UsdPreviewSurface inputs and a diffuse texture path for a Material."""
    material = stage.prim_at(material_path)
    shader = None
    for child in material.children:
        if child.type_name == "Shader" and child.get("info:id") == "UsdPreviewSurface":
            shader = child
            break
    if shader is None:
        return {}, None
    inputs = {name[len("inputs:"):]: shader.get(name) for name in shader.attributes
              if name.startswith("inputs:") and not shader.attribute(name).is_connection}
    texture = None
    diffuse = shader.attribute("inputs:diffuseColor")
    if diffuse is not None and diffuse.is_connection and diffuse.connections:
        source = stage.get_prim_at(diffuse.connections[0].split(".")[0])
        if source is not None and "inputs:file" in source.attributes:
            texture = str(source.get("inputs:file"))
    return inputs, texture


def export_obj(stage, path, *, prims: Optional[Iterable] = None, world_space: bool = True,
               triangulate: bool = False, materials: bool = True,
               skin_weights: bool = True) -> ObjExport:
    """Write ``stage`` meshes to ``path`` (and ``path`` with ``.mtl``)."""
    path = os.fspath(path)
    meshes = [p for p in (prims if prims is not None else stage) if p.type_name == "Mesh"]
    mtl_path = os.path.splitext(path)[0] + ".mtl"
    out = ["# exported from LightUSD"]
    if materials:
        out.append("mtllib " + os.path.basename(mtl_path))
    used_materials = {}
    vertex_base = uv_base = normal_base = 0
    total_faces = skinned = 0

    def material_name(material_path):
        if material_path not in used_materials:
            base = material_path.rstrip("/").rsplit("/", 1)[-1] or "material"
            name, k = base, 1
            while name in used_materials.values():
                name, k = f"{base}_{k}", k + 1
            used_materials[material_path] = name
        return used_materials[material_path]

    for mesh in meshes:
        points = _rows(_flat_values(mesh.get("points")), 3)
        counts = [int(c) for c in mesh.get("faceVertexCounts") or ()]
        corners = [int(i) for i in mesh.get("faceVertexIndices") or ()]
        if not points or sum(counts) != len(corners):
            continue
        m = _matrix(mesh) if world_space else None
        nm = _normal_matrix(m) if m else None
        out.append("o " + mesh.name)
        try:
            import numpy as np
            world = np.asarray(points, dtype=float)
            if m:
                world = world @ np.asarray(m)[:3, :3] + np.asarray(m)[3, :3]
            out.extend("v %s %s %s" % (_num(x), _num(y), _num(z)) for x, y, z in world.tolist())
        except ImportError:
            for p in points:
                q = _apply_point(m, p) if m else p
                out.append("v " + " ".join(_num(x) for x in q))
        if skin_weights and "primvars:skel:jointIndices" in mesh.attributes:
            ids = [int(i) for i in mesh["primvars:skel:jointIndices"]]
            weights = [float(w) for w in mesh["primvars:skel:jointWeights"]]
            size = int(mesh.attribute("primvars:skel:jointIndices").metadata("elementSize") or 1)
            if len(ids) == len(weights) == size * len(points):
                for v in range(len(points)):
                    row = " ".join(f"{ids[v * size + k]} {_num(weights[v * size + k])}" for k in range(size))
                    out.append(f"vw {vertex_base + v} {row}")
                skinned += len(points)
        uvs = _primvar(mesh, ("primvars:st", "primvars:st0", "primvars:UVMap"), 2, corners, counts, len(points))
        normals = _primvar(mesh, ("primvars:normals", "normals"), 3, corners, counts, len(points))
        uv_ids, normal_ids = [], []
        for values, ids, kind in ((uvs, uv_ids, "vt"), (normals, normal_ids, "vn")):
            if values is None:
                continue
            # Deduplicate exact source values; transform only unique ones.
            table = {}
            for value in values:
                index = table.get(value)
                if index is None:
                    index = table[value] = len(table)
                    shown = _apply_normal(nm, value) if kind == "vn" and nm else value
                    out.append(kind + " " + " ".join(_num(x) for x in shown))
                ids.append(index)
        # Face materials: GeomSubsets (materialBind family) over the mesh binding.
        face_material = [_material_of(stage, mesh)] * len(counts)
        for child in mesh.children:
            if child.type_name != "GeomSubset" or child.get("familyName") not in (None, "materialBind"):
                continue
            bound = _material_of(stage, child)
            if bound:
                for f in child.get("indices") or ():
                    if 0 <= int(f) < len(counts):
                        face_material[int(f)] = bound
        current = object()
        offset = 0
        for f, n in enumerate(counts):
            if materials and face_material[f] != current:
                current = face_material[f]
                if current:
                    out.append("usemtl " + material_name(current))
            corner_ids = list(range(offset, offset + n))
            polys = [corner_ids] if not triangulate or n == 3 else \
                [[corner_ids[0], corner_ids[k], corner_ids[k + 1]] for k in range(1, n - 1)]
            for poly in polys:
                refs = []
                for c in poly:
                    ref = str(vertex_base + corners[c] + 1)
                    if uv_ids or normal_ids:
                        ref += "/" + (str(uv_base + uv_ids[c] + 1) if uv_ids else "")
                    if normal_ids:
                        ref += "/" + str(normal_base + normal_ids[c] + 1)
                    refs.append(ref)
                out.append("f " + " ".join(refs))
                total_faces += 1
            offset += n
        vertex_base += len(points)
        uv_base += len(set(uv_ids)) if uv_ids else 0
        normal_base += len(set(normal_ids)) if normal_ids else 0

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    if materials:
        lines = ["# exported from LightUSD"]
        for material_path, name in used_materials.items():
            inputs, texture = _shader_inputs(stage, material_path)
            lines.append("newmtl " + name)
            diffuse = inputs.get("diffuseColor")
            if diffuse is not None:
                lines.append("Kd " + " ".join(_num(x) for x in diffuse))
            for key, tag in (("opacity", "d"), ("ior", "Ni"), ("roughness", "Pr"), ("metallic", "Pm")):
                if inputs.get(key) is not None:
                    lines.append(f"{tag} {_num(inputs[key])}")
            if inputs.get("emissiveColor") is not None:
                lines.append("Ke " + " ".join(_num(x) for x in inputs["emissiveColor"]))
            if texture:
                lines.append("map_Kd " + texture)
        with open(mtl_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    return ObjExport(meshes=len(meshes), vertices=vertex_base, faces=total_faces,
                     materials=len(used_materials), skinned_vertices=skinned)
