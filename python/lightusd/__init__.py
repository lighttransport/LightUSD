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
from typing import Any, Optional, Sequence, Tuple, Union

from . import _core
from ._core import (  # noqa: F401  (re-exports)
    Array,
    AssetResolver,
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
    "AssetResolver",
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
    "import_obj",
    "import_urdf",
    "import_mjcf",
    "physics",
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
    resolver: Optional[AssetResolver] = None,
    up_axis: str = "Z",
    asset_dirs: Sequence[_PathLike] = (),
    packages: Optional[dict[str, _PathLike]] = None,
    allow_missing: bool = False,
    tessellate_collision_shapes: bool = False,
    max_input: Optional[int] = None,
    max_depth: Optional[int] = None,
    unsupported_policy: str = "warn",
) -> Stage:
    """Load USD or import OBJ, URDF or MuJoCo MJCF into a USD stage.

    With ``composed=True`` (default) composition arcs — sublayers,
    references, payloads, inherits, specializes and variants — are resolved.
    ``variants`` maps variant-set names to selections, overriding authored
    selections. A positive ``max_memory`` caps input, asset, and aggregate
    resident estimates in bytes. Zero keeps the library's finite safe defaults.
    ``resolver`` supplies asset resolution for composition arcs; its active
    context is snapshotted for composition workers.

    Native imports resolve assets relative to the source file, with optional
    ``asset_dirs`` and URDF ``packages`` roots. ``allow_missing`` converts
    missing assets into stage warnings. OBJ preserves source coordinates;
    URDF and MJCF use SI units and the requested ``up_axis``.
    For native imports, ``max_input`` independently caps each source/dependency
    in bytes (zero uses 512 MiB); omission preserves the ``max_memory`` input
    cap. ``max_depth`` bounds XML/include nesting (default 64).
    ``unsupported_policy`` selects ``warn``, ``error`` or non-executable source
    ``preserve`` for known unsupported OBJ/MJCF features; see doc/usdobj.md.
    """
    path = _os.fspath(path)
    extension = _os.path.splitext(path)[1].lower()
    if format in ("obj", "urdf", "mjcf") or (format in (None, "auto") and extension in (".obj", ".urdf", ".xml", ".mjcf")):
        if variants:
            raise ValueError("variant selections apply to USD input only")
        return _import(path=path, format=format or "auto", max_memory=max_memory,
                       resolver=resolver, up_axis=up_axis, asset_dirs=asset_dirs,
                       packages=packages, allow_missing=allow_missing,
                       tessellate_collision_shapes=tessellate_collision_shapes,
                       max_input=max_input, max_depth=max_depth,
                       unsupported_policy=unsupported_policy)
    if max_input is not None or max_depth is not None or unsupported_policy != "warn":
        raise ValueError("resource/import policies require obj, urdf or mjcf input")
    return _core.load(
        _os.fspath(path),
        format=format,
        composed=composed,
        variants=variants,
        load_payloads=load_payloads,
        max_memory=max_memory,
        resolver=resolver,
    )


def loads(text: str) -> Stage:
    """Parse a USDA document from a string."""
    return _core.loads(text)


def load_bytes(
    data: Union[bytes, bytearray, memoryview],
    *,
    format: Optional[str] = None,
    max_memory: int = 0,
    source: _PathLike = "",
    resolver: Optional[AssetResolver] = None,
    **import_options,
) -> Stage:
    """Load USD bytes, or import explicit ``obj``, ``urdf`` or ``mjcf`` bytes.

    ``source`` anchors relative import assets; ``resolver`` can supply them
    without filesystem access. Other import options match :func:`load`.
    """
    if format in ("obj", "urdf", "mjcf"):
        return _import(data=data, source=_os.fspath(source), format=format,
                       max_memory=max_memory, resolver=resolver, **import_options)
    if import_options or source or resolver is not None:
        raise ValueError("source, resolver and import options require obj, urdf or mjcf format")
    return _core.load_bytes(data, format=format, max_memory=max_memory)


def is_usd(path: _PathLike) -> bool:
    """True when the file loads as USD (cheap header/parse check)."""
    try:
        stage = _core.load(_os.fspath(path))
    except Exception:
        return False
    stage.close()
    return True


def _import(*, path=None, data=None, source="", format="auto", max_memory=0,
            resolver=None, up_axis="Z", asset_dirs=(), packages=None,
            allow_missing=False, tessellate_collision_shapes=False,
            max_input=None, max_depth=None, unsupported_policy="warn",
            profile="newton", obj_topology="expanded", layout="flat"):
    import json
    if max_memory < 0:
        raise ValueError("max_memory must be non-negative")
    if max_input is not None and max_input < 0:
        raise ValueError("max_input must be non-negative")
    if max_depth is not None and not 1 <= max_depth <= 1024:
        raise ValueError("max_depth must be in [1, 1024]")
    if unsupported_policy not in ("warn", "error", "preserve"):
        raise ValueError("unsupported_policy must be warn, error or preserve")
    if up_axis not in ("Y", "Z"):
        raise ValueError("up_axis must be Y or Z")
    if profile not in ("newton", "openusd", "legacy"):
        raise ValueError("profile must be newton, openusd or legacy")
    if obj_topology not in ("expanded", "indexed"):
        raise ValueError("obj_topology must be expanded or indexed")
    if layout not in ("flat", "nested"):
        raise ValueError("layout must be flat or nested")
    settings = json.dumps({
        "up_axis": up_axis, "profile": profile, "asset_dirs": [_os.fspath(p) for p in asset_dirs],
        "packages": {k: _os.fspath(v) for k, v in (packages or {}).items()},
        "allow_missing": allow_missing,
        "tessellate_collision_shapes": tessellate_collision_shapes,
        "unsupported_policy": unsupported_policy, "obj_topology": obj_topology, "layout": layout,
    })
    return _core._import_scene(path=path, data=data, source=source, format=format,
                               settings=settings, max_memory=max_memory, resolver=resolver,
                               max_input=max_input, max_depth=max_depth)


def import_obj(path: _PathLike, **options) -> Stage:
    """Import a Wavefront OBJ and its MTL materials as a USD stage."""
    return _import(path=_os.fspath(path), format="obj", **options)


def import_urdf(path: _PathLike, **options) -> Stage:
    """Import URDF links, geometry and physics using the native frontend."""
    return _import(path=_os.fspath(path), format="urdf", **options)


def import_mjcf(path: _PathLike, **options) -> Stage:
    """Import MuJoCo MJCF as USD Physics, mjc and Newton schema data."""
    return _import(path=_os.fspath(path), format="mjcf", **options)


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

def _pod_info(name):
    return _core._type_descriptor(type_from_name(name))


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


def _normalize_value(value: Any, hint: Optional[str]):
    """Infer type and shape; the C11 layer converts numeric components."""
    base, force_array = _parse_type_hint(hint)
    base = "asset" if base == "asset_path" else base
    if value is ValueBlock:
        raise TypeError("use Attribute.block() to author a value block")
    if isinstance(value, str):
        base = base or "token"
        if base not in ("string", "token", "asset", "pathExpression"):
            raise TypeError(f"string given for numeric type {base!r}")
        return ("str", type_from_name(base), value)
    if isinstance(value, (bool, int, float)):
        base = base or ("bool" if isinstance(value, bool) else
                        "int" if isinstance(value, int) else "double")
        components, _ = _pod_info(base)
        if components != 1 and isinstance(value, float):
            raise TypeError(f"scalar float given for vector type {base!r}")
        return ("pod", type_from_name(base), int(bool(force_array)),
                (value,) * components, 1)
    if not isinstance(value, (list, tuple)):
        try:
            view = memoryview(value)
        except TypeError:
            raise TypeError(f"cannot author value of type {type(value).__name__}") from None
        fmt = view.format.lstrip("@=<>!")
        if fmt not in _FMT_TO_TYPE:
            raise TypeError(f"unsupported buffer format {view.format!r}")
        scalar = _FMT_TO_TYPE[fmt]
        if fmt in ("l", "L") and view.itemsize == 8:
            scalar = "int64" if fmt == "l" else "uint64"
        if base is None:
            base = _VEC_TYPE.get((view.shape[-1], scalar), scalar) if view.ndim == 2 else scalar
        components, _ = _pod_info(base)
        total = view.nbytes // view.itemsize
        if total % components:
            raise ValueError(f"buffer does not fit type {base!r}")
        count = total // components
        array = force_array or count != 1 or (force_array is None and view.ndim > 1)
        return ("pod", type_from_name(base), int(bool(array)), view, count)
    if not value:
        if base is None:
            raise TypeError("cannot author an empty sequence without a type")
        if base in ("string", "token", "asset", "pathExpression"):
            return ("tokens", type_from_name(base), ())
        _pod_info(base)
        return ("pod", type_from_name(base), 1, (), 0)
    if all(isinstance(x, str) for x in value):
        base = base or "token"
        if base not in ("string", "token", "asset", "pathExpression"):
            raise TypeError(f"string sequence given for numeric type {base!r}")
        return ("tokens", type_from_name(base), tuple(value))
    nested = all(isinstance(row, (list, tuple)) for row in value)
    if nested:
        columns = len(value[0])
        if not columns or any(len(row) != columns for row in value):
            raise ValueError("ragged nested sequence")
        flat = tuple(x for row in value for x in row)
        if base is None:
            base = _VEC_TYPE.get((columns, "float"))
            if base is None:
                raise TypeError(f"cannot infer USD type for {columns}-component rows")
        array = True
    else:
        flat = tuple(value)
        if not all(isinstance(x, (int, float)) for x in flat):
            raise TypeError("cannot author heterogeneous sequence")
        if base is None:
            scalar = "bool" if all(isinstance(x, bool) for x in flat) else (
                "int" if all(isinstance(x, int) for x in flat) else "double")
            base = None if force_array or scalar == "bool" else _VEC_TYPE.get((len(flat), scalar))
            base = base or scalar
        components, _ = _pod_info(base)
        array = bool(force_array) or components == 1 or len(flat) != components
    components, _ = _pod_info(base)
    if len(flat) % components:
        raise ValueError(f"sequence does not fit type {base!r}")
    return ("pod", type_from_name(base), int(array), flat, len(flat) // components)


_core._set_normalizer(_normalize_value)

# Imported after value normalization is registered.
from . import physics  # noqa: E402,F401
