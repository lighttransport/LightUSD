"""Value types for Mini USD.

Python representation of USD values:

  bool/int/uint/int64/uint64/uchar -> int (bool -> bool)
  half/float/double/timecode       -> float
  string/token                     -> str   (Token subclass marks a token where
                                             the type is not otherwise known)
  asset                            -> AssetPath (str subclass)
  vecN / quat                      -> tuple of N numbers (quat is (w, x, y, z))
  matrixN                          -> tuple of N row tuples
  arrays                           -> numpy.ndarray when numpy is available
                                      ((N,), (N,k) or (N,k,k)), else a list of
                                      scalars / tuples. string/token/asset
                                      arrays are always a list of str.

`None` means "no value authored"; the `BLOCK` sentinel is a ValueBlock
(`= None` in USDA).
"""

import math
import os
import struct

if os.environ.get("MINIUSD_NO_NUMPY"):
    np = None
else:
    try:
        import numpy as np
    except ImportError:  # pragma: no cover
        np = None


class _Block:
    _inst = None

    def __new__(cls):
        if cls._inst is None:
            cls._inst = super().__new__(cls)
        return cls._inst

    def __repr__(self):
        return "BLOCK"

    def __bool__(self):
        return False


BLOCK = _Block()


class Token(str):
    """A string that should be stored as a token (dictionary values etc.)."""


class AssetPath(str):
    """An asset path (`@path@`)."""

    def __repr__(self):
        return "AssetPath(%s)" % str.__repr__(self)


class TypedValue:
    """A value with an explicit USD type name, e.g. TypedValue('float3', (1, 2, 3)).

    Used for dictionary entries and unregistered metadata whose type cannot be
    inferred from the Python value alone.
    """

    __slots__ = ("type_name", "value")

    def __init__(self, type_name, value):
        self.type_name = type_name
        self.value = coerce(type_name, value)

    def __eq__(self, other):
        return (isinstance(other, TypedValue) and other.type_name == self.type_name
                and values_equal(self.value, other.value))

    def __repr__(self):
        return "TypedValue(%r, %r)" % (self.type_name, self.value)


class UnregisteredValue:
    """Metadata not known to the schema registry, kept as its USDA source text
    (as OpenUSD does). `.value` parses the text into a python value."""

    __slots__ = ("text",)

    def __init__(self, text):
        self.text = str(text)

    @property
    def value(self):
        from .usda_reader import parse_value_text
        return parse_value_text(self.text)

    def __eq__(self, other):
        return isinstance(other, UnregisteredValue) and other.text == self.text

    def __repr__(self):
        return "UnregisteredValue(%r)" % self.text


class ListOp:
    """SdfListOp. Each list is None when absent. `explicit=[]` is an explicit clear."""

    FIELDS = ("explicit", "added", "prepended", "appended", "deleted", "ordered")
    __slots__ = FIELDS

    def __init__(self, explicit=None, prepended=None, appended=None, deleted=None,
                 added=None, ordered=None):
        self.explicit = None if explicit is None else list(explicit)
        self.prepended = None if prepended is None else list(prepended)
        self.appended = None if appended is None else list(appended)
        self.deleted = None if deleted is None else list(deleted)
        self.added = None if added is None else list(added)
        self.ordered = None if ordered is None else list(ordered)

    @property
    def is_explicit(self):
        return self.explicit is not None

    def items(self):
        """The resulting item list when applied to an empty list."""
        if self.explicit is not None:
            return list(self.explicit)
        out = []
        for lst in (self.prepended, self.added, self.appended):
            for it in lst or ():
                if it not in out:
                    out.append(it)
        for it in self.deleted or ():
            if it in out:
                out.remove(it)
        return out

    def __eq__(self, other):
        return isinstance(other, ListOp) and all(
            getattr(self, f) == getattr(other, f) for f in self.FIELDS)

    def __repr__(self):
        parts = ["%s=%r" % (f, getattr(self, f)) for f in self.FIELDS
                 if getattr(self, f) is not None]
        return "ListOp(%s)" % ", ".join(parts)


class Reference:
    __slots__ = ("asset_path", "prim_path", "offset", "scale", "custom_data")

    def __init__(self, asset_path="", prim_path="", offset=0.0, scale=1.0, custom_data=None):
        self.asset_path = str(asset_path)
        self.prim_path = str(prim_path)
        self.offset = float(offset)
        self.scale = float(scale)
        self.custom_data = dict(custom_data) if custom_data else {}

    def _key(self):
        return (self.asset_path, self.prim_path, self.offset, self.scale)

    def __eq__(self, other):
        return (type(other) is type(self) and self._key() == other._key()
                and getattr(self, "custom_data", {}) == getattr(other, "custom_data", {}))

    def __hash__(self):
        return hash(self._key())

    def __repr__(self):
        return "%s(%r, %r)" % (type(self).__name__, self.asset_path, self.prim_path)


class Payload(Reference):
    __slots__ = ()

    def __init__(self, asset_path="", prim_path="", offset=0.0, scale=1.0):
        super().__init__(asset_path, prim_path, offset, scale)


# ---------------------------------------------------------------------------
# Value type table
# ---------------------------------------------------------------------------

class VT:
    """Scalar value type info."""

    __slots__ = ("name", "crate", "fmt", "n", "kind", "dim")

    def __init__(self, name, crate, fmt, n, kind, dim=0):
        self.name, self.crate, self.fmt, self.n, self.kind, self.dim = name, crate, fmt, n, kind, dim

    def __repr__(self):
        return "VT(%s)" % self.name


TYPES = {}
CRATE_TO_TYPE = {}


def _reg(vt):
    TYPES[vt.name] = vt
    CRATE_TO_TYPE.setdefault(vt.crate, vt.name)


for _n, _id, _f in (("bool", 1, "?"), ("uchar", 2, "B"), ("int", 3, "i"), ("uint", 4, "I"),
                    ("int64", 5, "q"), ("uint64", 6, "Q"), ("half", 7, "e"), ("float", 8, "f"),
                    ("double", 9, "d"), ("timecode", 56, "d")):
    _reg(VT(_n, _id, _f, 1, "num"))
_reg(VT("string", 10, None, 1, "string"))
_reg(VT("token", 11, None, 1, "token"))
_reg(VT("asset", 12, None, 1, "asset"))
for _d, _id in ((2, 13), (3, 14), (4, 15)):
    _reg(VT("matrix%dd" % _d, _id, "d", _d * _d, "mat", _d))
_reg(VT("frame4d", 15, "d", 16, "mat", 4))
for _s, _id in (("d", 16), ("f", 17), ("h", 18)):
    _reg(VT("quat" + _s, _id, {"d": "d", "f": "f", "h": "e"}[_s], 4, "quat"))
_VEC_BASE = {"d": "double", "f": "float", "h": "half", "i": "int"}
_VEC_FMT = {"d": "d", "f": "f", "h": "e", "i": "i"}
_VEC_ID = {}
for _i, _n in enumerate((2, 3, 4)):
    for _j, _s in enumerate("dfhi"):
        _VEC_ID[(_n, _s)] = 19 + _i * 4 + _j
        _reg(VT("%s%d" % (_VEC_BASE[_s], _n), 19 + _i * 4 + _j, _VEC_FMT[_s], _n, "vec"))
for _role, _dims in (("point", (3,)), ("normal", (3,)), ("vector", (3,)), ("color", (3, 4)),
                     ("texCoord", (2, 3))):
    for _n in _dims:
        for _s in "dfh":
            _reg(VT("%s%d%s" % (_role, _n, _s), _VEC_ID[(_n, _s)], _VEC_FMT[_s], _n, "vec"))

NP_DTYPE = {"?": "bool", "B": "u1", "i": "<i4", "I": "<u4", "q": "<i8", "Q": "<u8",
            "e": "<f2", "f": "<f4", "d": "<f8"}


def split_type(type_name):
    """'float3[]' -> (VT, True). Returns (None, is_array) for unknown types."""
    is_array = type_name.endswith("[]")
    base = type_name[:-2] if is_array else type_name
    return TYPES.get(base), is_array


# ---------------------------------------------------------------------------
# Coercion / inference
# ---------------------------------------------------------------------------

def _is_np(v):
    return np is not None and isinstance(v, np.ndarray)


def flatten(value):
    """Flatten nested sequences / arrays into a flat list of python numbers."""
    if _is_np(value):
        return value.reshape(-1).tolist()
    out = []

    def rec(v):
        if isinstance(v, (list, tuple)) or _is_np(v):
            for e in v:
                rec(e)
        else:
            out.append(v)
    rec(value)
    return out


def _conv_fn(fmt):
    if fmt == "?":
        return bool
    if fmt in "BiIqQ":
        return int
    if fmt == "f":
        return to_f32
    if fmt == "e":
        return to_f16
    return float


def _group(flat, vt):
    """Group a flat numeric list into scalar python values of vt."""
    conv = _conv_fn(vt.fmt)
    if vt.n == 1:
        return [conv(x) for x in flat]
    if len(flat) % vt.n:
        raise ValueError("%s: element count %d not a multiple of %d" % (vt.name, len(flat), vt.n))
    if vt.kind == "mat":
        d = vt.dim
        return [tuple(tuple(conv(x) for x in flat[i + r * d:i + r * d + d]) for r in range(d))
                for i in range(0, len(flat), vt.n)]
    return [tuple(conv(x) for x in flat[i:i + vt.n]) for i in range(0, len(flat), vt.n)]


def array_from_flat(flat, vt):
    """Build the canonical array value from a flat list (or 1-D ndarray) of numbers."""
    if np is not None:
        a = np.asarray(flat, dtype=NP_DTYPE[vt.fmt])
        if vt.kind == "mat":
            return a.reshape(-1, vt.dim, vt.dim)
        return a.reshape(-1, vt.n) if vt.n > 1 else a.reshape(-1)
    if _is_np(flat):  # pragma: no cover
        flat = flat.tolist()
    return _group(list(flat), vt)


def coerce(type_name, value):
    """Normalize `value` to the canonical python representation of `type_name`."""
    if value is None or value is BLOCK:
        return value
    vt, is_array = split_type(type_name)
    if vt is None:
        if type_name in ("dictionary",) and isinstance(value, dict):
            return value
        return value
    if is_array:
        if vt.kind in ("string", "token"):
            return [str(v) for v in value]
        if vt.kind == "asset":
            return [AssetPath(v) for v in value]
        if np is not None:
            a = np.asarray(value, dtype=NP_DTYPE[vt.fmt])
            if vt.kind == "mat":
                return a.reshape(-1, vt.dim, vt.dim)
            return a.reshape(-1, vt.n) if vt.n > 1 else a.reshape(-1)
        return _group(flatten(value), vt)
    if vt.kind in ("string", "token"):
        return str(value)
    if vt.kind == "asset":
        return AssetPath(value)
    if vt.n == 1:
        if _is_np(value) or isinstance(value, (list, tuple)):
            value = flatten(value)[0]
        return _conv_fn(vt.fmt)(value)
    return _group(flatten(value), vt)[0]


def infer_type(value):
    """Infer a USD type name for a python value (used for dictionaries/metadata)."""
    if isinstance(value, TypedValue):
        return value.type_name
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int" if -2**31 <= value < 2**31 else "int64"
    if isinstance(value, float):
        return "double"
    if isinstance(value, AssetPath):
        return "asset"
    if isinstance(value, Token):
        return "token"
    if isinstance(value, str):
        return "string"
    if isinstance(value, dict):
        return "dictionary"
    if _is_np(value):
        k = value.dtype.kind
        base = {"b": "bool", "i": "int", "u": "uint", "f": "double"}.get(k, "double")
        if k == "i" and value.dtype.itemsize == 8:
            base = "int64"
        if k == "f":
            base = {2: "half", 4: "float"}.get(value.dtype.itemsize, "double")
        if value.ndim == 1:
            return base + "[]"
        if value.ndim == 2 and 2 <= value.shape[1] <= 4 and base in ("int", "half", "float", "double"):
            return "%s%d[]" % (base, value.shape[1])
        if value.ndim == 3 and value.shape[1] == value.shape[2] and base == "double":
            return "matrix%dd[]" % value.shape[1]
        return base + "[]"
    if isinstance(value, tuple) and value:
        if all(isinstance(e, tuple) for e in value) and len(value) in (2, 3, 4) \
                and all(len(e) == len(value) for e in value):
            return "matrix%dd" % len(value)
        if 2 <= len(value) <= 4 and all(isinstance(e, (int, float)) and not isinstance(e, bool)
                                        for e in value):
            if all(isinstance(e, int) for e in value):
                return "int%d" % len(value)
            return "double%d" % len(value)
    if isinstance(value, (list, tuple)):
        if not value:
            return "token[]"
        et = infer_type(value[0])
        if et in ("dictionary",) or et.endswith("[]"):
            raise TypeError("cannot infer USD type for nested list")
        if et == "int":
            if any(isinstance(e, float) for e in value):
                et = "double"
        return et + "[]"
    raise TypeError("cannot infer USD type for %r" % (value,))


def values_equal(a, b):
    if _is_np(a) or _is_np(b):
        if np is None:  # pragma: no cover
            return False
        a2, b2 = np.asarray(a), np.asarray(b)
        if a2.dtype.kind in "fc" or b2.dtype.kind in "fc":
            return a2.shape == b2.shape and bool(np.all((a2 == b2) | (np.isnan(a2) & np.isnan(b2))))
        return a2.shape == b2.shape and bool(np.all(a2 == b2))
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(values_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(values_equal(a[k], b[k]) for k in a)
    return a == b


# float32 / half helpers
_F32 = struct.Struct("<f")
_F16 = struct.Struct("<e")


def to_f32(v):
    try:
        return _F32.unpack(_F32.pack(v))[0]
    except OverflowError:
        return math.copysign(math.inf, v)


def to_f16(v):
    try:
        return _F16.unpack(_F16.pack(v))[0]
    except OverflowError:
        return math.copysign(math.inf, v)


def wrap_typed(type_name, v):
    """Wrap a dictionary value in TypedValue when infer_type() would not
    reproduce `type_name` (so the type survives a round trip)."""
    if v is BLOCK or type_name == "dictionary":
        return v
    try:
        if infer_type(v) == type_name:
            return v
    except TypeError:
        pass
    return TypedValue(type_name, v)
