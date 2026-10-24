# SPDX-License-Identifier: Apache-2.0
"""UsdVol authoring and inspection (OpenVDB through tinyvdb).

- :func:`inspect_vdb`: list the grids of a ``.vdb`` (name, type, class,
  transform, bounds) without decoding voxels.
- :func:`create_volume_from_vdb`: author a ``Volume`` with one
  ``OpenVDBAsset`` field per grid, following the UsdVol schema
  (``field:<name>`` relationships with one target, ``fieldName`` /
  ``fieldIndex`` for grids sharing a name, ``fieldDataType`` from the grid
  type, ``fieldClass``, and ``extent``).
- :func:`compute_extent`: the local extent of a Volume from its fields'
  grid bounds.
- :func:`field_paths`, :func:`set_field`, :func:`block_field`: the
  ``UsdVolVolume`` field-relationship API.

Command line::

    python -m lightusd.volume inspect smoke.vdb
    python -m lightusd.volume create out.usda smoke.vdb [--prim /Smoke] [--grid density]

Voxel data for rendering comes from :meth:`lightusd.RenderScene.volumes` /
``volume_dense``.
"""
from __future__ import annotations

import json
import os
import re
from typing import Any, Optional, Sequence, Union

from . import _core

__all__ = ["inspect_vdb", "create_volume_from_vdb", "compute_extent", "field_paths",
           "set_field", "block_field"]

FIELD_PREFIX = "field:"


def _bytes(vdb: Union[str, os.PathLike, bytes, bytearray, memoryview]) -> bytes:
    if isinstance(vdb, (bytes, bytearray, memoryview)):
        return bytes(vdb)
    with open(os.fspath(vdb), "rb") as f:
        return f.read()


def inspect_vdb(vdb: Union[str, os.PathLike, bytes]) -> list[dict[str, Any]]:
    """Describe each grid of a ``.vdb`` (a path or the file bytes).

    Each dict has ``name``, ``index`` (file position), ``nameIndex`` (the
    UsdVol ``fieldIndex`` among grids of that name), ``valueType``,
    ``fieldDataType``, ``gridClass``, ``components``, ``savedAsHalf``,
    ``pointGrid``, ``activeVoxels``, ``indexToWorld`` (4x4 rows, USD
    row-vector) and, when the file records them, ``indexMin``/``indexMax``
    (inclusive) and ``worldMin``/``worldMax``.
    """
    info = json.loads(_core._vdb_inspect(_bytes(vdb)))
    grids = info["grids"]
    for g in grids:
        m = g.get("indexToWorld")
        if m:
            g["indexToWorld"] = [m[0:4], m[4:8], m[8:12], m[12:16]]
    return grids


def field_paths(volume: "_core.Prim") -> dict[str, str]:
    """``{binding: field prim path}`` for each ``field:*`` relationship with
    exactly one target (UsdVolVolume::GetFieldPaths); blocked and
    multi-target relationships are skipped."""
    out = {}
    for name in volume.relationships:
        if not name.startswith(FIELD_PREFIX):
            continue
        targets = volume.relationship(name).targets
        if len(targets) == 1:
            out[name[len(FIELD_PREFIX):]] = targets[0]
    return out


def set_field(volume: "_core.Prim", binding: str, field_path: str) -> None:
    """Point ``field:<binding>`` at one field prim (CreateFieldRelationship)."""
    name = FIELD_PREFIX + binding
    if name in volume.relationships:
        volume.relationship(name).set_targets([field_path])
    else:
        volume.add_relationship(name, [field_path])


def block_field(volume: "_core.Prim", binding: str) -> None:
    """Author ``field:<binding>`` with no targets: the Volume has no such
    field, even when a weaker layer binds one (BlockFieldRelationship)."""
    name = FIELD_PREFIX + binding
    if name in volume.relationships:
        volume.relationship(name).set_targets([])
    else:
        volume.add_relationship(name, [])


def _identifier(name: str) -> str:
    ident = re.sub(r"[^A-Za-z0-9_]", "_", name) or "field"
    return "_" + ident if ident[0].isdigit() else ident


def _union(boxes: Sequence[tuple[Sequence[float], Sequence[float]]]):
    if not boxes:
        return None
    lo = [min(b[0][a] for b in boxes) for a in range(3)]
    hi = [max(b[1][a] for b in boxes) for a in range(3)]
    return lo, hi


def create_volume_from_vdb(stage: "_core.Stage", path: str, vdb_path: str, *,
                           grids: Optional[Sequence[str]] = None,
                           asset_path: Optional[str] = None,
                           data: Optional[bytes] = None) -> "_core.Prim":
    """Define a ``Volume`` at ``path`` with an ``OpenVDBAsset`` child per grid
    of ``vdb_path`` (or only the ``grids`` named), bound as
    ``field:<grid name>``. ``asset_path`` is what ``filePath`` authors
    (default: ``vdb_path``); ``data`` supplies the file bytes when
    ``vdb_path`` is not readable from here. Point grids are skipped. The
    Volume ``extent`` is the union of the grids' world boxes."""
    infos = inspect_vdb(data if data is not None else vdb_path)
    wanted = set(grids) if grids is not None else None
    volume = stage.define_prim(path, "Volume")
    file_path = asset_path if asset_path is not None else os.fspath(vdb_path)
    used: set[str] = set()
    boxes = []
    for g in infos:
        if g["pointGrid"] or (wanted is not None and g["name"] not in wanted):
            continue
        binding = g["name"] if g["nameIndex"] == 0 else "%s_%d" % (g["name"], g["nameIndex"])
        child = _identifier(binding)
        while child in used:
            child += "_"
        used.add(child)
        field = stage.define_prim(path.rstrip("/") + "/" + child, "OpenVDBAsset")
        field.set("filePath", file_path, type="asset")
        field.set("fieldName", g["name"], type="token")
        if g["nameIndex"]:
            field.set("fieldIndex", int(g["nameIndex"]), type="int")
        if g["fieldDataType"]:
            field.set("fieldDataType", g["fieldDataType"], type="token")
        if g["gridClass"] in ("levelSet", "fogVolume", "staggered"):
            field.set("fieldClass", g["gridClass"], type="token")
        if g["components"] == 3:
            field.set("vectorDataRoleHint", "Vector", type="token")
        set_field(volume, binding, field.path)
        if "worldMin" in g:
            boxes.append((g["worldMin"], g["worldMax"]))
    box = _union(boxes)
    if box:
        volume.set("extent", [tuple(box[0]), tuple(box[1])], type="float3[]")
    return volume


def compute_extent(stage: "_core.Stage", volume_path: str, *,
                   base_dir: Optional[str] = None):
    """``(min, max)`` of a Volume's fields' grid boxes in the Volume's space
    (each field prim's transform relative to the Volume is applied when it is
    a translate/scale; general field xforms use the 8-corner box), or None
    when no field has bounds. Relative ``filePath`` values resolve against
    ``base_dir`` (default: the current directory)."""
    volume = stage.prim_at(volume_path)
    vol_inv = _inverse(_rows(volume.world_transform()))
    boxes = []
    for _, fpath in field_paths(volume).items():
        field = stage.get_prim_at(fpath)
        if field is None:
            continue
        fp = field.get("filePath")
        if fp is None:
            continue
        fp = str(getattr(fp, "path", fp))
        if not os.path.isabs(fp) and base_dir:
            fp = os.path.join(base_dir, fp)
        name = field.get("fieldName")
        name = str(name) if name is not None else ""
        index = field.get("fieldIndex")
        index = int(index) if index is not None else 0
        try:
            infos = inspect_vdb(fp)
        except (OSError, _core.UsdError):
            continue
        match = [g for g in infos if not name or g["name"] == name]
        if index >= len(match) or "worldMin" not in match[index]:
            continue
        g = match[index]
        # world_min/max are in the grid's (field prim) space; carry them to
        # the Volume's space through field-to-world * inverse(volume-to-world).
        m = _mul(_rows(field.world_transform()), vol_inv)
        boxes.append(_transform_box(g["worldMin"], g["worldMax"], m))
    return _union(boxes)


def _rows(m) -> list[list[float]]:
    return [[float(m[r][c]) for c in range(4)] for r in range(4)]


def _mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def _inverse(m):
    # Gauss-Jordan; transforms here are affine and invertible.
    n = 4
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        if pv == 0:
            raise ValueError("singular transform")
        a[c] = [x / pv for x in a[c]]
        for r in range(n):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[n:] for row in a]


def _transform_box(lo, hi, m):
    pts = []
    for c in range(8):
        q = (hi[0] if c & 1 else lo[0], hi[1] if c & 2 else lo[1], hi[2] if c & 4 else lo[2])
        pts.append([q[0] * m[0][a] + q[1] * m[1][a] + q[2] * m[2][a] + m[3][a] for a in range(3)])
    return ([min(p[a] for p in pts) for a in range(3)], [max(p[a] for p in pts) for a in range(3)])


def _main(argv: Optional[Sequence[str]] = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(prog="python -m lightusd.volume", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_inspect = sub.add_parser("inspect", help="list the grids of a .vdb")
    p_inspect.add_argument("vdb")
    p_create = sub.add_parser("create", help="author a Volume for a .vdb")
    p_create.add_argument("output", help=".usda/.usdc/.usd")
    p_create.add_argument("vdb")
    p_create.add_argument("--prim", default="/Volume")
    p_create.add_argument("--grid", action="append", help="only this grid (repeatable)")
    p_create.add_argument("--asset-path", help="filePath to author (default: relative to output)")
    args = ap.parse_args(argv)
    if args.cmd == "inspect":
        print(json.dumps(inspect_vdb(args.vdb), indent=2))
        return 0
    stage = _core.Stage.create()
    asset = args.asset_path
    if asset is None:
        asset = os.path.relpath(os.path.abspath(args.vdb), os.path.dirname(os.path.abspath(args.output)))
        if not asset.startswith("."):
            asset = "./" + asset
    create_volume_from_vdb(stage, args.prim, args.vdb, grids=args.grid, asset_path=asset)
    root = args.prim.strip("/").split("/")[0]
    if root:
        stage.set_default_prim(root)
    stage.save(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
