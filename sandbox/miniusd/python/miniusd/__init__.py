"""Mini USD: pure-python USDA / USDC / USDZ reader & writer (experimental).

    import miniusd
    stage = miniusd.Stage(up_axis="Y", meters_per_unit=1.0)
    box = stage.define("/World/Box", "Cube")
    box.set("size", 2.0)
    stage.save("box.usdz")

    stage = miniusd.open("scene.usdc")
    for prim in stage.traverse():
        print(prim.path, prim.type_name)

Layer-level only: composition arcs (references, payloads, variants, ...) are
read and written but not composed.
"""

import builtins
import os

from .model import AttributeSpec, Layer, PrimSpec, RelationshipSpec, specs_equal
from .values import (BLOCK, AssetPath, ListOp, Payload, Reference, Token, TypedValue, UnregisteredValue,
                     infer_type)

from . import geom  # noqa: E402  (convenience builders)

Stage = Layer

__version__ = "0.1.0"
__all__ = ["Stage", "Layer", "PrimSpec", "AttributeSpec", "RelationshipSpec", "ListOp",
           "Reference", "Payload", "AssetPath", "Token", "TypedValue", "UnregisteredValue", "BLOCK", "open",
           "loads", "detect_format", "specs_equal", "infer_type", "geom"]


def detect_format(data):
    head = bytes(data[:8])
    if head.startswith(b"PXR-USDC"):
        return "usdc"
    if head.startswith(b"PK"):
        return "usdz"
    if head.lstrip(b"\xef\xbb\xbf").startswith(b"#usda"):
        return "usda"
    raise ValueError("unrecognized USD data")


def loads(data, fmt=None):
    """Parse USD data (bytes, or a USDA str) into a Layer."""
    if isinstance(data, str):
        from .usda_reader import read_usda
        return read_usda(data)
    fmt = fmt or detect_format(data)
    if fmt == "usda":
        from .usda_reader import read_usda
        return read_usda(bytes(data).decode("utf-8"))
    if fmt == "usdc":
        from .usdc_reader import read_usdc
        return read_usdc(data)
    if fmt == "usdz":
        from .usdz import read_usdz
        _, root, assets = read_usdz(bytes(data))
        layer = loads(root)
        layer.assets = assets
        return layer
    raise ValueError("unknown format %r" % fmt)


def open(path):  # noqa: A001
    """Open a .usda / .usdc / .usd / .usdz file (format detected from content)."""
    with builtins.open(os.fspath(path), "rb") as f:
        return loads(f.read())
