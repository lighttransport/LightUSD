# SPDX-License-Identifier: Apache-2.0
"""Signed distance fields and volumetric meshes (tinyvdb).

- :func:`mesh_to_level_set`: narrow-band level set (OpenVDB bytes) of a
  triangle or polygon mesh; signs from generalized winding numbers, so open
  and inside-out meshes work.
- :class:`LevelSet`: sample, mesh (sparse marching cubes) or tetrahedralize
  (isosurface stuffing) a level set; read/write ``.vdb``.
- :func:`add_mesh_sdf` / :func:`make_volume_deformable`: author the results on
  a stage, as the MJCF importer does (a guide-purpose ``Volume`` with an
  ``OpenVDBAsset`` field next to the mesh and a ``lightusd:sdf`` relationship;
  a ``TetMesh`` with ``PhysicsVolumeDeformableSimAPI``).

Every computation takes ``device``: ``"cpu"`` (default), ``"cuda"``, ``"hip"``
or ``"auto"`` (CUDA, then HIP, then the CPU). CUDA/HIP need a build with
``-DLIGHTUSD_TINYVDB_GPU=ON`` (``LIGHTUSD_CMAKE_ARGS`` for pip builds) and a
device; an explicit one that is unavailable raises. Results match the CPU's.
:func:`device_available` reports what a device name maps to.
"""
from __future__ import annotations

import json
import os
import struct
from array import array
from typing import Optional, Sequence

from . import _core

__all__ = ["LevelSet", "TetMesh", "mesh_to_level_set", "read_level_set",
           "add_mesh_sdf", "make_volume_deformable", "device_available"]


def device_available(device: str = "auto") -> Optional[str]:
    """The device `device` runs on here ("cpu", "cuda" or "hip"), or None
    when it cannot run (an unavailable CUDA/HIP device, or a build without
    GPU support)."""
    try:
        return _core._volumetric_device(str(device))
    except (_core.UsdError, ValueError):
        return None


def _flat(values, typecode, width):
    """Contiguous native buffer of `typecode` from an Array, ndarray or nested
    sequence (`width` components per element)."""
    try:
        view = memoryview(values)
        if view.format == typecode and view.c_contiguous:
            return view.cast("B")
    except TypeError:
        pass
    try:
        import numpy as np
        dtype = {"f": np.float32, "d": np.float64, "i": np.int32}[typecode]
        return memoryview(np.ascontiguousarray(np.asarray(values, dtype=dtype).reshape(-1))).cast("B")
    except ImportError:
        out = array(typecode)
        for v in values:
            if isinstance(v, (int, float)):
                out.append(v)
            else:
                out.extend(v)
        if width and len(out) % width:
            raise ValueError("array length is not a multiple of %d" % width)
        return memoryview(out).cast("B")


def _unpack(data: bytes, typecode: str, width: int):
    """Native bytes as an (n, width) ndarray when numpy is available,
    otherwise a flat array.array."""
    try:
        import numpy as np
        dtype = {"f": np.float32, "i": np.int32}[typecode]
        out = np.frombuffer(data, dtype=dtype)
        return out.reshape(-1, width) if width > 1 else out
    except ImportError:
        out = array(typecode)
        out.frombytes(data)
        return out


class TetMesh:
    """Isosurface-stuffing result: points (n, 3), tets (m, 4) with positive
    orientation and outward boundary triangles (k, 3)."""

    def __init__(self, points, tets, surface):
        self.points, self.tets, self.surface = points, tets, surface


class LevelSet:
    """A float level set (negative inside) held as OpenVDB bytes."""

    def __init__(self, vdb: bytes):
        self.vdb = bytes(vdb)
        self.voxel_size, self.background, self.active_voxels = _core._level_set_info(self.vdb)

    def save(self, path) -> None:
        with open(os.fspath(path), "wb") as f:
            f.write(self.vdb)

    def sample(self, points):
        """Trilinear world-space values at (n, 3) points."""
        raw = _core._level_set_sample(self.vdb, _flat(points, "d", 3))
        return _unpack(raw, "f", 1)

    def to_mesh(self, max_cells: int = 0):
        """Zero isosurface as (points (n, 3), triangle indices (m, 3))."""
        pts, idx = _core._level_set_to_mesh(self.vdb, int(max_cells))
        return _unpack(pts, "f", 3), _unpack(idx, "i", 3)

    def to_tet_mesh(self, spacing: Optional[float] = None, *, max_tets: int = 0,
                    threads: int = 0, device: str = "cpu") -> TetMesh:
        """Tetrahedra of the interior; `spacing` defaults to 4 voxels."""
        spacing = float(spacing) if spacing else 4.0 * self.voxel_size
        settings = json.dumps({"spacing": spacing, "max_tets": int(max_tets), "threads": int(threads),
                               "device": str(device)})
        pts, tets, surf = _core._level_set_to_tet_mesh(self.vdb, settings)
        return TetMesh(_unpack(pts, "f", 3), _unpack(tets, "i", 4), _unpack(surf, "i", 3))


def read_level_set(source) -> LevelSet:
    """A level set from a ``.vdb`` path or its bytes."""
    if isinstance(source, (bytes, bytearray, memoryview)):
        return LevelSet(bytes(source))
    with open(os.fspath(source), "rb") as f:
        return LevelSet(f.read())


def _extent(points) -> float:
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    raw = _flat(points, "f", 3).cast("f")
    for i in range(0, len(raw) - 2, 3):
        for a in range(3):
            v = raw[i + a]
            if v < lo[a]: lo[a] = v
            if v > hi[a]: hi[a] = v
    return max(hi[a] - lo[a] for a in range(3)) if len(raw) >= 3 else 0.0


def mesh_to_level_set(points, indices, counts=None, *, voxel_size: Optional[float] = None,
                      resolution: int = 64, band: float = 3.0, exact_winding: bool = False,
                      threads: int = 0, max_voxels: int = 0, name: str = "sdf",
                      device: str = "cpu") -> LevelSet:
    """Narrow-band level set of a mesh. `voxel_size` defaults to the largest
    extent / `resolution`; the band is `band` voxels each side."""
    if voxel_size is None:
        extent = _extent(points)
        if not extent > 0:
            raise ValueError("mesh has no extent")
        voxel_size = extent / int(resolution)
    settings = json.dumps({"voxel_size": float(voxel_size), "exterior_width": band * voxel_size,
                           "interior_width": band * voxel_size, "exact_winding": bool(exact_winding),
                           "threads": int(threads), "max_voxels": int(max_voxels), "name": name,
                           "device": str(device)})
    vdb = _core._mesh_to_level_set(_flat(points, "f", 3), _flat(indices, "i", 0),
                                   None if counts is None else _flat(counts, "i", 0), settings)
    return LevelSet(vdb)


def _mesh_arrays(prim):
    counts = prim["faceVertexCounts"] if "faceVertexCounts" in prim.attributes else None
    return prim["points"], prim["faceVertexIndices"], counts


def _copy_transform(src, dst) -> None:
    names = [n for n in src.attributes if n.startswith("xformOp:") or n == "xformOpOrder"]
    for n in names:
        attr = src.attribute(n)
        dst.set(n, src[n], type=attr.type_name, uniform=attr.is_uniform)


def _sibling(stage, path: str, suffix: str) -> str:
    base = path + suffix
    candidate, n = base, 2
    while True:
        try:
            stage.prim_at(candidate)
        except KeyError:
            return candidate
        candidate, n = f"{base}_{n}", n + 1


def add_mesh_sdf(stage, mesh_path: str, *, resolution: int = 64, band: float = 3.0,
                 asset: Optional[str] = None, threads: int = 0, newton: bool = True,
                 device: str = "cpu"):
    """Compute the level set of a Mesh prim (in its own frame) and author it:
    a guide-purpose ``Volume`` sibling with the mesh's transform and an
    ``OpenVDBAsset`` field reading `asset` (default ``sdf/<name>.vdb``, a
    generated asset written on save), ``rel lightusd:sdf`` on the mesh and,
    with `newton`, ``NewtonSDFCollisionAPI`` parameters. Returns the Volume."""
    mesh = stage.prim_at(mesh_path)
    if mesh.type_name != "Mesh":
        raise ValueError(f"{mesh_path} is not a Mesh")
    points, indices, counts = _mesh_arrays(mesh)
    level_set = mesh_to_level_set(points, indices, counts, resolution=resolution, band=band,
                                  threads=threads, device=device)
    asset = asset or f"sdf/{mesh.name}.vdb"
    stage.add_generated_asset(asset, level_set.vdb)
    vpath = _sibling(stage, mesh_path, "_sdf")
    volume = stage.define_prim(vpath, "Volume")
    _copy_transform(mesh, volume)
    volume.set("purpose", "guide", type="token", uniform=True)
    field = stage.define_prim(vpath + "/sdf", "OpenVDBAsset")
    field.set("filePath", asset, type="asset")
    field.set("fieldName", "sdf", type="token")
    field.set("fieldClass", "levelSet", type="token")
    field.set("fieldDataType", "float", type="token")
    volume.add_relationship("field:sdf", [vpath + "/sdf"])
    mesh.add_relationship("lightusd:sdf", [vpath])
    if newton:
        schemas = list(mesh.metadata("apiSchemas") or ())
        if "NewtonSDFCollisionAPI" not in schemas:
            schemas = [s for s in schemas if s != "NewtonMeshCollisionAPI"] + ["NewtonSDFCollisionAPI"]
            mesh.set_metadata("apiSchemas", schemas)
        mesh.set("newton:sdfMaxResolution", (int(resolution) + 7) // 8 * 8, type="int", uniform=True)
        width = band * level_set.voxel_size
        mesh.set("newton:sdfNarrowBandInner", -width, type="float", uniform=True)
        mesh.set("newton:sdfNarrowBandOuter", width, type="float", uniform=True)
    return volume


def make_volume_deformable(stage, source_path: str, path: Optional[str] = None, *,
                           spacing: Optional[float] = None, resolution: int = 64,
                           mass: Optional[float] = None, density: float = 1000.0,
                           youngs_modulus: float = 1.0e5, poissons_ratio: float = 0.3,
                           threads: int = 0, max_tets: int = 0, device: str = "cpu"):
    """Tetrahedralize a Mesh prim (through its level set) or a Volume whose
    ``field:*`` OpenVDBAsset is a level set, and author a ``TetMesh`` with
    ``PhysicsVolumeDeformableSimAPI``, per-point masses (`mass`, or `density`
    times the tet volume, shared equally) and a bound
    ``PhysicsVolumeDeformableMaterialAPI`` material. `spacing` defaults to
    four voxels. The TetMesh takes the source's transform. Returns it."""
    src = stage.prim_at(source_path)
    if src.type_name == "Mesh":
        points, indices, counts = _mesh_arrays(src)
        level_set = mesh_to_level_set(points, indices, counts, resolution=resolution,
                                      threads=threads, device=device)
    elif src.type_name == "Volume":
        level_set = None
        generated = stage.generated_assets()
        for rel in src.relationships:
            if not rel.startswith("field:"):
                continue
            for target in src.relationship(rel).targets:
                field = stage.prim_at(target)
                if field.type_name != "OpenVDBAsset" or "filePath" not in field.attributes:
                    continue
                asset = str(field["filePath"])
                level_set = (LevelSet(generated[asset]) if asset in generated
                             else read_level_set(asset))
                break
            if level_set is not None:
                break
        if level_set is None:
            raise ValueError(f"{source_path} has no OpenVDBAsset level-set field")
    else:
        raise ValueError(f"{source_path} must be a Mesh or a Volume")
    tm = level_set.to_tet_mesh(spacing, max_tets=max_tets, threads=threads, device=device)
    path = path or _sibling(stage, source_path, "_tets")
    prim = stage.define_prim(path, "TetMesh")
    _copy_transform(src, prim)
    prim.set("points", tm.points, type="point3f[]")
    prim.set("tetVertexIndices", tm.tets, type="int4[]")
    prim.set("surfaceFaceVertexIndices", tm.surface, type="int3[]")
    prim.set_metadata("apiSchemas", ["PhysicsVolumeDeformableSimAPI", "MaterialBindingAPI",
                                     "PhysicsCollisionAPI"])
    prim.set("physics:collisionEnabled", True, type="bool")
    npts = len(tm.points)
    if mass is None:
        mass = density * _tet_volume(tm)
    prim.set("physics:masses", [float(mass) / max(npts, 1)] * npts, type="float[]")
    prim.set("physics:masses:elementType", "point", type="token", uniform=True)
    material = stage.define_prim(path + "_material", "Material")
    material.set_metadata("apiSchemas", ["PhysicsMaterialAPI", "PhysicsVolumeDeformableMaterialAPI"])
    material.set("physics:youngsModulus", float(youngs_modulus), type="float")
    material.set("physics:poissonsRatio", float(poissons_ratio), type="float")
    prim.add_relationship("material:binding:physics", [path + "_material"])
    return prim


def _tet_volume(tm: TetMesh) -> float:
    total = 0.0
    pts = tm.points
    for t in tm.tets:
        a, b, c, d = (pts[int(i)] for i in t)
        u = [b[k] - a[k] for k in range(3)]
        v = [c[k] - a[k] for k in range(3)]
        w = [d[k] - a[k] for k in range(3)]
        total += (u[0] * (v[1] * w[2] - v[2] * w[1]) - u[1] * (v[0] * w[2] - v[2] * w[0]) +
                  u[2] * (v[0] * w[1] - v[1] * w[0])) / 6.0
    return total


def main(argv=None) -> int:
    """``python -m lightusd.volumetric IN -o OUT --sdf PRIM --deformable PRIM``:
    add level-set sidecars and volume deformables to an existing USD file."""
    import argparse
    from . import load
    ap = argparse.ArgumentParser(prog="python -m lightusd.volumetric", description=main.__doc__)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--sdf", action="append", default=[], metavar="MESH",
                    help="Mesh prim to give a level-set sidecar (repeatable)")
    ap.add_argument("--deformable", action="append", default=[], metavar="PRIM",
                    help="Mesh or level-set Volume prim to tetrahedralize (repeatable)")
    ap.add_argument("--resolution", type=int, default=64)
    ap.add_argument("--band", type=float, default=3.0)
    ap.add_argument("--spacing", type=float, default=None)
    ap.add_argument("--density", type=float, default=1000.0)
    ap.add_argument("--youngs-modulus", type=float, default=1.0e5)
    ap.add_argument("--poissons-ratio", type=float, default=0.3)
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--device", default="cpu", choices=("cpu", "cuda", "hip", "auto"))
    args = ap.parse_args(argv)
    stage = load(args.input)
    for path in args.sdf:
        volume = add_mesh_sdf(stage, path, resolution=args.resolution, band=args.band,
                              threads=args.threads, device=args.device)
        print(f"{path}: level set {volume.path}")
    for path in args.deformable:
        prim = make_volume_deformable(stage, path, spacing=args.spacing, resolution=args.resolution,
                                      density=args.density, youngs_modulus=args.youngs_modulus,
                                      poissons_ratio=args.poissons_ratio, threads=args.threads,
                                      device=args.device)
        print(f"{path}: {len(prim['tetVertexIndices'])} tets at {prim.path}")
    stage.save(args.output)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
