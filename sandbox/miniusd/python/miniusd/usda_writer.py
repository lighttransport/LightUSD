"""USDA (text) writer: Layer -> text, formatted close to `usdcat`."""

import math
import struct
from functools import lru_cache

from .model import FIELD_TYPES, LISTOP_KINDS
from .values import BLOCK, ListOp, TypedValue, UnregisteredValue, _is_np, infer_type, split_type

IND = "    "
_F32 = struct.Struct("<f")
_F16 = struct.Struct("<e")


def _norm_exp(s):
    if "e" in s:
        m, e = s.split("e")
        sign = "-" if e.startswith("-") else ""
        e = e.lstrip("+-").lstrip("0") or "0"
        s = m + "e" + sign + e
    return s


def _special(v):
    if v != v:
        return "nan"
    return "inf" if v > 0 else "-inf"


def fmt_double(v):
    v = float(v)
    if math.isinf(v) or v != v:
        return _special(v)
    if v.is_integer() and abs(v) < 1e16:
        return str(int(v)) if v or math.copysign(1, v) > 0 else "-0"
    return _norm_exp(repr(v))


def fmt_float(v):
    """Shortest text that round-trips as float32."""
    v = float(v)
    if v == 0.0:  # before the cache: 0.0 and -0.0 are equal dict keys
        return "-0" if math.copysign(1.0, v) < 0 else "0"
    return _fmt_float(v)


@lru_cache(maxsize=1 << 16)
def _fmt_float(v):
    if math.isinf(v) or v != v:
        return _special(v)
    try:
        f = _F32.unpack(_F32.pack(v))[0]
    except OverflowError:
        return _special(v)
    if f.is_integer() and abs(f) < 1e16:
        return str(int(f)) if f or math.copysign(1, f) > 0 else "-0"
    for p in range(6, 10):
        s = "%.*g" % (p, f)
        if _F32.unpack(_F32.pack(float(s)))[0] == f:
            return _norm_exp(s)
    return _norm_exp(repr(f))


def fmt_half(v):
    v = float(v)
    if math.isinf(v) or v != v:
        return _special(v)
    try:
        f = _F16.unpack(_F16.pack(v))[0]
    except OverflowError:
        return _special(v)
    if f.is_integer():
        return str(int(f)) if f or math.copysign(1, f) > 0 else "-0"
    for p in range(3, 7):
        s = "%.*g" % (p, f)
        if _F16.unpack(_F16.pack(float(s)))[0] == f:
            return _norm_exp(s)
    return _norm_exp(repr(f))


_NUMFMT = {"f": fmt_float, "e": fmt_half, "d": fmt_double}


def quote(s):
    s = str(s)
    if "\n" in s:
        if '"""' not in s and not s.endswith('"') and "\\" not in s:
            return '"""' + s + '"""'
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") \
        .replace("\r", "\\r").replace("\t", "\\t") + '"'


def fmt_asset(s):
    s = str(s)
    return "@@@" + s.replace("@@@", "\\@@@") + "@@@" if "@" in s else "@" + s + "@"


def fmt_path(p):
    return "<" + str(p) + ">"


def _scalar_formatter(vt, attr_bools=True):
    fmt = vt.fmt
    if vt.kind in ("string", "token"):
        return quote
    if vt.kind == "asset":
        return fmt_asset
    if fmt == "?":
        num = (lambda x: "1" if x else "0") if attr_bools else (lambda x: "true" if x else "false")
    elif fmt in _NUMFMT:
        num = _NUMFMT[fmt]
    else:
        num = lambda x: str(int(x))  # noqa: E731
    if vt.n == 1:
        return num
    if vt.kind == "mat":
        return lambda m: "( " + ", ".join("(" + ", ".join(num(x) for x in row) + ")" for row in m) + " )"
    return lambda t: "(" + ", ".join(num(x) for x in t) + ")"


def fmt_value(type_name, value, indent="", attr_bools=True):
    """Format a value of USD type `type_name` as USDA text."""
    if value is BLOCK or value is None:
        return "None"
    if isinstance(value, TypedValue):
        value = value.value
    if type_name == "dictionary":
        return fmt_dict(value, indent)
    vt, is_array = split_type(type_name)
    if vt is None:
        return _fmt_generic(value)
    f = _scalar_formatter(vt, attr_bools)
    if not is_array:
        return f(value)
    if _is_np(value):
        value = value.tolist()
        if vt.n > 1 and vt.kind != "mat":
            value = [tuple(v) for v in value]
        elif vt.kind == "mat":
            value = [tuple(tuple(r) for r in m) for m in value]
    return "[" + ", ".join(f(v) for v in value) + "]"


def _fmt_generic(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return fmt_double(v) if isinstance(v, float) else str(v)
    if isinstance(v, str):
        return quote(v)
    if isinstance(v, (list, tuple)):
        o, c = ("(", ")") if isinstance(v, tuple) else ("[", "]")
        return o + ", ".join(_fmt_generic(x) for x in v) + c
    return quote(str(v))


def _key(k):
    return k if k.replace("_", "a").replace(":", "a").isalnum() and not k[0].isdigit() else quote(k)


def fmt_dict(d, indent=""):
    if not d:
        return "{\n" + indent + "}"
    lines = ["{"]
    for k, v in d.items():
        t = infer_type(v)
        lines.append("%s%s%s %s = %s" % (indent, IND, t, _key(k),
                                         fmt_value(t, v, indent + IND, attr_bools=False)))
    lines.append(indent + "}")
    return "\n".join(lines)


def _fmt_ref(r, is_payload):
    s = ""
    if r.asset_path:
        s += fmt_asset(r.asset_path)
    if r.prim_path:
        s += fmt_path(r.prim_path)
    extra = []
    if r.offset != 0.0:
        extra.append("offset = " + fmt_double(r.offset))
    if r.scale != 1.0:
        extra.append("scale = " + fmt_double(r.scale))
    if not is_payload and r.custom_data:
        extra.append("customData = " + fmt_dict(r.custom_data, IND))
    if extra:
        s += " (" + "; ".join(extra) + ")"
    return s


_OPWORDS = (("explicit", ""), ("deleted", "delete "), ("added", "add "),
            ("prepended", "prepend "), ("appended", "append "), ("ordered", "reorder "))


def _listop_lines(key, lo, item_fmt):
    out = []
    for field, word in _OPWORDS:
        items = getattr(lo, field)
        if items is None:
            continue
        if field == "explicit" and not items:
            out.append("%s%s = None" % (word, key))
        elif len(items) == 1 and key not in ("apiSchemas", "variantSets"):
            out.append("%s%s = %s" % (word, key, item_fmt(items[0])))
        else:
            out.append("%s%s = [%s]" % (word, key, ", ".join(item_fmt(i) for i in items)))
    return out


def _meta_lines(meta, indent, ctx):
    lines = []
    for key, v in meta.items():
        if key in ("primOrder", "propertyOrder", "subLayerOffsets") or v is None:
            continue
        ftype = FIELD_TYPES.get(key)
        if isinstance(v, UnregisteredValue):
            lines.append("%s = %s" % (key, v.text))
        elif key == "comment":
            lines.append(quote(v))
        elif key == "documentation":
            lines.append("doc = " + quote(v))
        elif key == "subLayers":
            offs = meta.get("subLayerOffsets") or []
            items = []
            for i, s in enumerate(v):
                it = fmt_asset(s)
                if i < len(offs) and tuple(offs[i]) != (0.0, 1.0):
                    it += " (offset = %s; scale = %s)" % (fmt_double(offs[i][0]), fmt_double(offs[i][1]))
                items.append(it)
            lines.append("subLayers = [\n%s\n%s]" % (",\n".join(indent + IND + it for it in items), indent))
        elif ftype == "variantmap":
            body = "".join("%s%sstring %s = %s\n" % (indent, IND, _key(k), quote(s)) for k, s in v.items())
            lines.append("variants = {\n%s%s}" % (body, indent))
        elif ftype in LISTOP_KINDS and isinstance(v, ListOp):
            name = "variantSets" if key == "variantSetNames" else key
            if ftype == "referencelistop":
                f = lambda r: _fmt_ref(r, False)  # noqa: E731
            elif ftype == "payloadlistop":
                f = lambda r: _fmt_ref(r, True)  # noqa: E731
            elif ftype == "pathlistop":
                f = fmt_path
            else:
                f = quote
            lines.extend(_listop_lines(name, v, f))
        elif ftype == "permission":
            lines.append("%s = %s" % (key, v))
        else:
            t = ftype or infer_type(v)
            lines.append("%s = %s" % (key, fmt_value(t, v, indent, attr_bools=False)))
    return lines


def _meta_block(meta, indent, ctx):
    lines = _meta_lines(meta, indent + IND, ctx)
    if not lines:
        return ""
    return " (\n" + "".join(indent + IND + ln + "\n" for ln in lines) + indent + ")"


def _write_attr(out, a, ind):
    pre = ("custom " if a.custom else "") + ("uniform " if a.uniform else "")
    decl = "%s%s %s" % (pre, a.type_name, a.name)
    has_ts = a.time_samples is not None
    if a.default is not None or a.metadata or (not has_ts and a.connections is None):
        line = ind + decl
        if a.default is not None:
            line += " = " + fmt_value(a.type_name, a.default, ind)
        out.append(line + _meta_block(a.metadata, ind, "prop"))
    if has_ts:
        out.append("%s%s.timeSamples = {" % (ind, decl))
        for t, v in a.time_samples.items():
            out.append("%s%s%s: %s," % (ind, IND, fmt_double(t), fmt_value(a.type_name, v, ind + IND)))
        out.append(ind + "}")
    if a.connections is not None:
        for ln in _listop_lines(decl + ".connect", a.connections, fmt_path):
            out.append(ind + ln)


def _write_rel(out, r, ind):
    pre = ("custom " if r.custom else "") + ("varying " if r.varying else "")
    decl = "%srel %s" % (pre, r.name)
    lo = r.targets
    only_ops = lo is not None and lo.explicit is None
    if lo is None or lo.explicit is not None or r.metadata or not only_ops:
        line = ind + decl
        if lo is not None and lo.explicit is not None:
            ex = lo.explicit
            line += " = " + ("None" if not ex else fmt_path(ex[0]) if len(ex) == 1
                             else "[" + ", ".join(fmt_path(p) for p in ex) + "]")
        out.append(line + _meta_block(r.metadata, ind, "prop"))
    if lo is not None:
        tmp = ListOp(**{f: getattr(lo, f) for f in ListOp.FIELDS if f != "explicit"})
        for ln in _listop_lines(decl, tmp, fmt_path):
            out.append(ind + ln)


def _write_body(out, prim, ind):
    wrote = False
    order = prim.metadata.get("primOrder")
    if order is not None:
        out.append("%sreorder nameChildren = [%s]" % (ind, ", ".join(quote(x) for x in order)))
        wrote = True
    order = prim.metadata.get("propertyOrder")
    if order is not None:
        out.append("%sreorder properties = [%s]" % (ind, ", ".join(quote(x) for x in order)))
        wrote = True
    for p in prim.properties.values():
        (_write_attr if p.is_attribute else _write_rel)(out, p, ind)
        wrote = True
    for c in prim.children.values():
        if wrote:
            out.append("")
        _write_prim(out, c, ind)
        wrote = True
    for set_name, variants in prim.variant_sets.items():
        if wrote:
            out.append("")
        out.append("%svariantSet %s = {" % (ind, quote(set_name)))
        for vname, v in variants.items():
            out.append("%s%s%s%s {" % (ind, IND, quote(vname), _meta_block(v.metadata, ind + IND, "prim")))
            _write_body(out, v, ind + IND + IND)
            out.append(ind + IND + "}")
            out.append("")
        if out[-1] == "":
            out.pop()
        out.append(ind + "}")
        wrote = True


def _write_prim(out, prim, ind):
    head = ind + (prim.specifier or "def")
    if prim.type_name:
        head += " " + prim.type_name
    head += " " + quote(prim.name)
    out.append(head + _meta_block(prim.metadata, ind, "prim"))
    out.append(ind + "{")
    _write_body(out, prim, ind + IND)
    out.append(ind + "}")


def write_usda(layer):
    out = ["#usda 1.0"]
    lines = _meta_lines(layer.metadata, IND, "layer")
    if lines:
        out.append("(")
        out.extend(IND + ln for ln in lines)
        out.append(")")
    order = layer.root.metadata.get("primOrder")
    if order is not None:
        out.append("")
        out.append("reorder rootPrims = [%s]" % ", ".join(quote(x) for x in order))
    for c in layer.root.children.values():
        out.append("")
        _write_prim(out, c, "")
    out.append("")
    return "\n".join(out)
