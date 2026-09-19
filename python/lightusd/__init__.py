# SPDX-License-Identifier: Apache-2.0
"""LightUSD — a lightweight USD (Universal Scene Description) library.

Built on the lightusd "next" core: load USDA/USDC/USDZ (composed or
layer-level), inspect and author stages, and extract render-ready geometry
via the ``lightusd.tydra`` submodule.

Quick start::

    import lightusd

    stage = lightusd.load("scene.usdz")
    for prim in stage:
        print(prim.path, prim.type_name)

    mesh = stage.prim_at("/World/Mesh")
    points = mesh["points"]           # zero-copy Array
    import numpy as np
    np_points = np.asarray(points)    # no copy
"""

from __future__ import annotations

import os as _os
import struct as _struct
from typing import Any, Optional, Sequence, Tuple, Union

from . import _core
from ._core import (  # noqa: F401  (re-exports)
    Array,
    Attribute,
    Prim,
    Relationship,
    Stage,
    StaleHandleError,
    TimeSamples,
    UsdError,
    UsdIoError,
    UsdParseError,
    ValueBlock,
    VariantSet,
    VariantSets,
    flatten_file,
    type_from_name,
    type_name,
)

try:
    from ._version import __version__  # type: ignore[import-not-found]
except ImportError:  # pragma: no cover - sdist/dev tree without scm stamp
    __version__ = "0.0.0+unknown"

__all__ = [
    "Array",
    "Attribute",
    "Prim",
    "Relationship",
    "Stage",
    "StaleHandleError",
    "TimeSamples",
    "UsdError",
    "UsdIoError",
    "UsdParseError",
    "ValueBlock",
    "VariantSet",
    "VariantSets",
    "load",
    "loads",
    "load_bytes",
    "flatten_file",
    "is_usd",
    "asarray",
    "__version__",
]

_PathLike = Union[str, "_os.PathLike[str]"]


# ============================================================
# Loading
# ============================================================

def load(
    path: _PathLike,
    *,
    format: Optional[str] = None,
    composed: bool = True,
    variants: Optional[dict] = None,
    load_payloads: bool = True,
    max_memory: int = 0,
) -> Stage:
    """Load a USD file (USDA / USDC / USDZ, auto-detected).

    With ``composed=True`` (default) composition arcs — sublayers,
    references, payloads, inherits, specializes and variants — are resolved.
    ``variants`` maps variant-set names to selections, overriding authored
    selections. A positive ``max_memory`` caps input, asset, and aggregate
    resident estimates in bytes. Zero keeps the library's finite safe defaults.
    """
    return _core.load(
        _os.fspath(path),
        format=format,
        composed=composed,
        variants=variants,
        load_payloads=load_payloads,
        max_memory=max_memory,
    )


def loads(text: str) -> Stage:
    """Parse a USDA document from a string."""
    return _core.loads(text)


def load_bytes(
    data: Union[bytes, bytearray, memoryview],
    *,
    format: Optional[str] = None,
    max_memory: int = 0,
) -> Stage:
    """Load USD from an in-memory buffer (format sniffed from content)."""
    return _core.load_bytes(data, format=format, max_memory=max_memory)


def is_usd(path: _PathLike) -> bool:
    """True when the file loads as USD (cheap header/parse check)."""
    try:
        stage = _core.load(_os.fspath(path))
    except Exception:
        return False
    stage.close()
    return True


def asarray(array: Array):
    """Wrap a lightusd.Array in a numpy.ndarray without copying.

    Equivalent to ``numpy.asarray(array)``; requires numpy.
    """
    import numpy as np

    return np.asarray(array)


# ============================================================
# Value normalization (authoring ingestion)
#
# All authoring APIs (Prim.set / Attribute.set / Stage metadata) route
# Python values through this function; the C layer receives one
# pre-normalized (type, is_array, bytes, count) package per call.
# ============================================================

_STR_TYPES = {"string": "string", "token": "token", "asset": "asset_path",
              "asset_path": "asset_path"}

# type name -> (components, struct format char for authoring)
_POD_INFO = {
    "bool": (1, "B"),
    "int": (1, "i"),
    "uint": (1, "I"),
    "int64": (1, "q"),
    "uint64": (1, "Q"),
    "half": (1, "e"),
    "float": (1, "f"),
    "double": (1, "d"),
    "timecode": (1, "d"),
    "int2": (2, "i"), "int3": (3, "i"), "int4": (4, "i"),
    "uint2": (2, "I"), "uint3": (3, "I"), "uint4": (4, "I"),
    "half2": (2, "e"), "half3": (3, "e"), "half4": (4, "e"),
    "float2": (2, "f"), "float3": (3, "f"), "float4": (4, "f"),
    "double2": (2, "d"), "double3": (3, "d"), "double4": (4, "d"),
    "quath": (4, "e"), "quatf": (4, "f"), "quatd": (4, "d"),
    "point3h": (3, "e"), "point3f": (3, "f"), "point3d": (3, "d"),
    "vector3h": (3, "e"), "vector3f": (3, "f"), "vector3d": (3, "d"),
    "normal3h": (3, "e"), "normal3f": (3, "f"), "normal3d": (3, "d"),
    "color3h": (3, "e"), "color3f": (3, "f"), "color3d": (3, "d"),
    "color4h": (4, "e"), "color4f": (4, "f"), "color4d": (4, "d"),
    "texcoord2h": (2, "e"), "texcoord2f": (2, "f"), "texcoord2d": (2, "d"),
    "texcoord3h": (3, "e"), "texcoord3f": (3, "f"), "texcoord3d": (3, "d"),
    "matrix2f": (4, "f"), "matrix2d": (4, "d"),
    "matrix3f": (9, "f"), "matrix3d": (9, "d"),
    "matrix4f": (16, "f"), "matrix4d": (16, "d"),
}

# buffer format char -> default USD scalar type
_FMT_TO_TYPE = {
    "?": "bool", "b": "int", "B": "uint",
    "h": "int", "H": "uint",
    "i": "int", "I": "uint", "l": "int", "L": "uint",
    "q": "int64", "Q": "uint64",
    "e": "half", "f": "float", "d": "double",
}

# default vector types by (component count, base scalar)
_VEC_TYPE = {
    (2, "float"): "float2", (3, "float"): "float3", (4, "float"): "float4",
    (2, "double"): "double2", (3, "double"): "double3",
    (4, "double"): "double4",
    (2, "half"): "half2", (3, "half"): "half3", (4, "half"): "half4",
    (2, "int"): "int2", (3, "int"): "int3", (4, "int"): "int4",
    (2, "uint"): "uint2", (3, "uint"): "uint3", (4, "uint"): "uint4",
    (16, "float"): "matrix4f", (16, "double"): "matrix4d",
    (9, "float"): "matrix3f", (9, "double"): "matrix3d",
}


def _parse_type_hint(hint):
    """'point3f[]' -> ('point3f', True); 'float' -> ('float', False)."""
    if hint is None:
        return None, None
    hint = hint.strip()
    if hint.endswith("[]"):
        return hint[:-2], True
    return hint, False


def _pack_numbers(flat, fmt):
    if fmt == "B":  # bools
        return bytes(1 if x else 0 for x in flat)
    return _struct.pack("<%d%s" % (len(flat), fmt), *flat)


def _half_array_note(base: str, fmt: str, is_array: bool) -> str:
    # Half-element ARRAYS are stored as float32 by the core; scalars keep
    # 16-bit packing.
    if fmt == "e" and is_array:
        return "f"
    return fmt


def _normalize_value(value: Any, hint: Optional[str]):
    """Normalize a Python value for the C authoring layer.

    Returns one of:
      ("pod",    type_id, is_array, data_bytes, count)
      ("str",    type_id, s)
      ("tokens", type_id, tuple_of_str)
    """
    base, hint_is_array = _parse_type_hint(hint)

    # ---- explicit value block ----
    if value is ValueBlock:
        raise TypeError("use Attribute.block() to author a value block")

    # ---- string family ----
    if isinstance(value, str):
        tname = _STR_TYPES.get(base or "token", base or "token")
        tid = type_from_name("asset" if tname == "asset_path" else tname)
        if tid == 0:
            tid = type_from_name("token")
        return ("str", tid, value)

    # ---- scalars ----
    if isinstance(value, bool):
        return ("pod", type_from_name(base or "bool"), 0,
                b"\x01" if value else b"\x00", 1)
    if isinstance(value, int) and not isinstance(value, bool):
        tname = base or "int"
        comps, fmt = _POD_INFO[tname]
        return ("pod", type_from_name(tname), 0,
                _pack_numbers([value] * comps, fmt), 1)
    if isinstance(value, float):
        tname = base or "double"
        comps, fmt = _POD_INFO[tname]
        if comps != 1:
            raise TypeError(f"scalar float given for vector type {tname!r}")
        return ("pod", type_from_name(tname), 0, _pack_numbers([value], fmt),
                1)

    # ---- buffer protocol (numpy arrays, array.array, memoryview, bytes) ----
    mv = None
    if not isinstance(value, (list, tuple)):
        try:
            mv = memoryview(value)
        except TypeError:
            mv = None
    if mv is not None:
        return _normalize_buffer(mv, base, hint_is_array)

    # ---- lists / tuples ----
    if isinstance(value, (list, tuple)):
        return _normalize_sequence(value, base, hint_is_array)

    raise TypeError(f"cannot author value of type {type(value).__name__}")


def _normalize_buffer(mv: memoryview, base, hint_is_array):
    if mv.format not in _FMT_TO_TYPE and mv.format != "B":
        # Recast unknown formats through numpy if available.
        raise TypeError(f"unsupported buffer format {mv.format!r}")
    if not mv.c_contiguous:
        mv = memoryview(bytes(mv))  # force a contiguous copy

    shape = mv.shape if mv.ndim else (1,)
    scalar = _FMT_TO_TYPE.get(mv.format, "uint")

    if base is None:
        # Infer the USD type from dtype + innermost dimension.
        if mv.ndim == 2 and (shape[1], scalar) in _VEC_TYPE:
            base = _VEC_TYPE[(shape[1], scalar)]
        else:
            base = scalar
    comps, fmt = _POD_INFO[base]
    fmt = _half_array_note(base, fmt, True)

    total = 1
    for dim in shape:
        total *= dim
    if comps == 0 or total % comps != 0:
        raise ValueError(
            f"buffer with {total} scalars does not fit type {base!r} "
            f"({comps} components/element)")
    count = total // comps

    # Convert dtype if it does not match the target storage.
    src_fmt = mv.format
    want_fmt = fmt
    if src_fmt != want_fmt and not (src_fmt == "B" and want_fmt == "B"):
        try:
            import numpy as np

            np_map = {"B": np.uint8, "i": np.int32, "I": np.uint32,
                      "q": np.int64, "Q": np.uint64, "e": np.float16,
                      "f": np.float32, "d": np.float64}
            arr = np.asarray(mv).astype(np_map[want_fmt], copy=False)
            data = arr.tobytes()
        except ImportError:
            flat = [x for x in memoryview(mv).cast(
                "B").cast(src_fmt)]  # 1-D scalars
            data = _pack_numbers(flat, want_fmt)
    else:
        data = mv.tobytes()

    is_array = 1 if (hint_is_array is not False and
                     (hint_is_array or count > 1 or mv.ndim > 1)) else 0
    tid = type_from_name(
        "asset" if base == "asset_path" else base)
    if tid == 0:
        raise ValueError(f"unknown USD type {base!r}")
    return ("pod", tid, is_array, data, count)


def _normalize_sequence(seq, base, hint_is_array):
    if len(seq) == 0:
        if base is None:
            raise TypeError("cannot author an empty sequence without a type")
        comps, fmt = _POD_INFO[base]
        fmt = _half_array_note(base, fmt, True)
        return ("pod", type_from_name(base), 1, b"", 0)

    # sequence of strings -> token/string array
    if all(isinstance(x, str) for x in seq):
        tname = base or "token"
        tid = type_from_name("asset" if tname == "asset_path" else tname)
        if tid == 0:
            tid = type_from_name("token")
        return ("tokens", tid, tuple(seq))

    # sequence of sequences -> array of vectors
    if all(isinstance(x, (list, tuple)) for x in seq):
        comps_seen = len(seq[0])
        flat = []
        for row in seq:
            if len(row) != comps_seen:
                raise ValueError("ragged nested sequence")
            flat.extend(float(v) for v in row)
        if base is None:
            base = _VEC_TYPE.get((comps_seen, "float"), None)
            if base is None:
                raise TypeError(
                    f"cannot infer USD type for {comps_seen}-component rows")
        comps, fmt = _POD_INFO[base]
        fmt = _half_array_note(base, fmt, True)
        if comps != comps_seen:
            raise ValueError(
                f"rows have {comps_seen} components but type {base!r} "
                f"expects {comps}")
        return ("pod", type_from_name(base), 1, _pack_numbers(flat, fmt),
                len(seq))

    # flat numeric sequence
    if all(isinstance(x, (int, float)) and not isinstance(x, bool)
           for x in seq):
        all_int = all(isinstance(x, int) for x in seq)
        if base is None:
            # A bare 2/3/4-tuple is a vector SCALAR unless the hint forces
            # an array.
            n = len(seq)
            if not hint_is_array and (n, "double" if not all_int else "int") \
                    in _VEC_TYPE and n in (2, 3, 4):
                base = _VEC_TYPE[(n, "double" if not all_int else "int")]
                comps, fmt = _POD_INFO[base]
                return ("pod", type_from_name(base), 0,
                        _pack_numbers(list(seq), fmt), 1)
            base = "int" if all_int else "double"
        comps, fmt = _POD_INFO[base]
        fmt = _half_array_note(base, fmt, comps == 1 or True)
        flat = list(seq)
        if comps > 1:
            if len(flat) % comps != 0:
                raise ValueError(
                    f"sequence of {len(flat)} scalars does not fit "
                    f"{base!r} ({comps} components)")
            count = len(flat) // comps
            is_array = 1 if (hint_is_array or count > 1) else 0
            if not all_int:
                flat = [float(v) for v in flat]
            return ("pod", type_from_name(base), is_array,
                    _pack_numbers(flat, fmt), count)
        is_array = 1  # a flat list of scalars is an array
        if not all_int:
            flat = [float(v) for v in flat]
        return ("pod", type_from_name(base), is_array,
                _pack_numbers(flat, fmt), len(flat))

    # sequence of bools
    if all(isinstance(x, bool) for x in seq):
        return ("pod", type_from_name(base or "bool"), 1,
                bytes(1 if x else 0 for x in seq), len(seq))

    raise TypeError("cannot author heterogeneous sequence")


_core._set_normalizer(_normalize_value)
