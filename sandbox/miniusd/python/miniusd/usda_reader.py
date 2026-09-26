"""USDA (text) reader: tokenizer + recursive-descent parser -> Layer."""

import re

from .model import (FIELD_TYPES, AttributeSpec, Layer, RelationshipSpec, make_absolute)
from .values import (BLOCK, AssetPath, ListOp, Payload, Reference, UnregisteredValue, coerce,
                     split_type, wrap_typed)

_TOKEN_RE = re.compile(r"""
 (?P<ws>[ \t\r\n]+|\#[^\n]*|//[^\n]*|/\*.*?\*/)
|(?P<str>\"\"\"(?:[^\\]|\\.)*?\"\"\"|'''(?:[^\\]|\\.)*?'''|"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')
|(?P<asset>@@@(?:\\@@@|.)*?@@@|@[^@\n]*@)
|(?P<path><[^<>\n]*>)
|(?P<num>(?:[-+]?(?:inf|nan)|-?nan)(?![\w:])|[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?))
|(?P<id>[A-Za-z_][\w:]*(?:\.[A-Za-z_][\w:]*)*)
|(?P<p>[()\[\]{}=,;:&])
""", re.S | re.X)

_LISTOP_WORDS = {"prepend": "prepended", "append": "appended", "delete": "deleted",
                 "add": "added", "reorder": "ordered"}
_ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0",
        "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


class ParseError(ValueError):
    pass


def tokenize(text):
    toks = []
    pos, n = 0, len(text)
    match = _TOKEN_RE.match
    while pos < n:
        m = match(text, pos)
        if m is None:
            line = text.count("\n", 0, pos) + 1
            raise ParseError("line %d: unexpected character %r" % (line, text[pos]))
        kind = m.lastgroup
        if kind != "ws":
            toks.append((kind, m.group(kind), pos))
        pos = m.end()
    return toks


def _unquote(s):
    q = 3 if s[:3] in ('"""', "'''") else 1
    body = s[q:-q]
    if "\\" not in body:
        return body
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            e = body[i + 1]
            if e == "x" and i + 3 < len(body):
                out.append(chr(int(body[i + 2:i + 4], 16)))
                i += 4
                continue
            out.append(_ESC.get(e, e))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _num(s):
    if s.lstrip("+-").isdigit():
        v = int(s)
        return -0.0 if v == 0 and s.startswith("-") else v
    return float(s)


class _Parser:
    def __init__(self, text):
        self.text = text
        self.toks = tokenize(text)
        self.i = 0
        self.warnings = []

    # -- token helpers -------------------------------------------------------
    def error(self, msg):
        pos = self.toks[self.i][2] if self.i < len(self.toks) else len(self.text)
        line = self.text.count("\n", 0, pos) + 1
        raise ParseError("line %d: %s" % (line, msg))

    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else ("eof", "", -1)

    def next(self):
        if self.i >= len(self.toks):
            self.error("unexpected end of file")
        t = self.toks[self.i]
        self.i += 1
        return t

    def accept(self, val):
        t = self.peek()
        if t[1] == val and t[0] in ("p", "id"):
            self.i += 1
            return True
        return False

    def expect(self, val):
        if not self.accept(val):
            self.error("expected %r, got %r" % (val, self.peek()[1]))

    def ident(self):
        t = self.next()
        if t[0] != "id":
            self.i -= 1
            self.error("expected identifier, got %r" % t[1])
        return t[1]

    def string(self):
        t = self.next()
        if t[0] != "str":
            self.i -= 1
            self.error("expected string, got %r" % t[1])
        return _unquote(t[1])

    def skip_seps(self):
        while self.peek()[1] in (";", ",") and self.peek()[0] == "p":
            self.i += 1

    # -- generic values ------------------------------------------------------
    def value(self):
        kind, s, _ = self.next()
        if kind == "num":
            return _num(s)
        if kind == "str":
            return _unquote(s)
        if kind == "asset":
            return AssetPath(s[3:-3].replace("\\@@@", "@@@") if s.startswith("@@@") else s[1:-1])
        if kind == "path":
            return s[1:-1]
        if kind == "id":
            if s == "None":
                return BLOCK
            if s in ("true", "false"):
                return s == "true"
            return s
        if s == "(":
            return tuple(self._seq(")"))
        if s == "[":
            return self._seq("]")
        if s == "{":
            self.i -= 1
            return self.dictionary()
        self.i -= 1
        self.error("unexpected %r" % s)

    def _seq(self, close):
        out = []
        while True:
            if self.accept(close):
                return out
            out.append(self.value())
            if not self.accept(","):
                self.expect(close)
                return out

    def typed_value(self, type_name):
        if self.peek()[1] == "None" and self.peek()[0] == "id":
            self.i += 1
            return BLOCK
        if type_name == "dictionary":
            return self.dictionary()
        v = self.value()
        try:
            return coerce(type_name, v)
        except (TypeError, ValueError) as e:
            self.error("bad %s value: %s" % (type_name, e))

    def dictionary(self):
        self.expect("{")
        d = {}
        while not self.accept("}"):
            self.skip_seps()
            if self.accept("}"):
                break
            t = self.ident()
            if self.accept("["):
                self.expect("]")
                t += "[]"
            k = self.next()
            key = _unquote(k[1]) if k[0] == "str" else k[1]
            self.expect("=")
            v = self.typed_value(t)
            d[key] = wrap_typed(t, v)
            self.skip_seps()
        return d

    def targets(self, base):
        """<path> | [<a>, <b>] | None -> list of absolute path strings."""
        v = self.value()
        if v is BLOCK:
            return []
        if isinstance(v, str):
            v = [v]
        return [make_absolute(base, p) for p in v]

    def ref_item(self, cls):
        asset, prim = "", ""
        t = self.peek()
        if t[0] == "asset":
            asset = self.value()
        if self.peek()[0] == "path":
            prim = self.value()
        if not asset and not prim:
            self.error("expected reference")
        item = cls(asset, prim)
        if self.accept("("):
            while not self.accept(")"):
                self.skip_seps()
                if self.accept(")"):
                    break
                k = self.ident()
                self.expect("=")
                if k == "offset":
                    item.offset = float(self.value())
                elif k == "scale":
                    item.scale = float(self.value())
                elif k == "customData" and cls is Reference:
                    item.custom_data = self.dictionary()
                else:
                    self.value()
                self.skip_seps()
        return item

    def ref_list(self, cls):
        if self.accept("None"):
            return []
        if self.accept("["):
            out = []
            while not self.accept("]"):
                out.append(self.ref_item(cls))
                self.skip_seps()
            return out
        return [self.ref_item(cls)]

    # -- metadata ------------------------------------------------------------
    def metadata(self, meta, ctx, owner_path="/"):
        """Parse '( ... )' contents (the '(' is already consumed)."""
        while True:
            self.skip_seps()
            if self.accept(")"):
                return
            t = self.peek()
            if t[0] == "str":
                meta["comment"] = self.string()
                continue
            op = None
            if t[0] == "id" and t[1] in _LISTOP_WORDS and self.peek(1)[0] == "id":
                op = _LISTOP_WORDS[self.next()[1]]
            key = self.ident()
            self.expect("=")
            self.meta_value(meta, key, op, ctx, owner_path)

    def meta_value(self, meta, key, op, ctx, owner_path):
        if key == "doc":
            key = "documentation"
        elif key == "variants":
            key = "variantSelection"
        elif key == "variantSets":
            key = "variantSetNames"
        ftype = FIELD_TYPES.get(key)
        if ctx == "layer" and key == "subLayers":
            items = self.ref_list(Payload)
            meta["subLayers"] = [str(it.asset_path) for it in items]
            if any(it.offset != 0.0 or it.scale != 1.0 for it in items):
                meta["subLayerOffsets"] = [(it.offset, it.scale) for it in items]
            return
        if ftype in ("tokenlistop", "stringlistop", "pathlistop", "referencelistop",
                     "payloadlistop"):
            if ftype == "referencelistop":
                items = self.ref_list(Reference)
            elif ftype == "payloadlistop":
                items = self.ref_list(Payload)
            elif ftype == "pathlistop":
                items = self.targets(owner_path)
            else:
                v = self.value()
                items = [] if v is BLOCK else ([v] if isinstance(v, str) else [str(x) for x in v])
            lo = meta.get(key)
            if not isinstance(lo, ListOp):
                lo = meta[key] = ListOp()
            setattr(lo, op or "explicit", items)
            return
        if ftype == "variantmap":
            d = {}
            self.expect("{")
            while not self.accept("}"):
                self.skip_seps()
                if self.accept("}"):
                    break
                self.ident()  # 'string'
                k = self.next()
                name = _unquote(k[1]) if k[0] == "str" else k[1]
                self.expect("=")
                d[name] = self.string()
                self.skip_seps()
            meta[key] = d
            return
        if ftype == "permission":
            meta[key] = self.ident()
            return
        if op is not None and ftype is None:
            self.warnings.append("list op on unknown metadata %r ignored" % key)
            self.value()
            return
        if ftype is not None:
            meta[key] = self.typed_value(ftype)
        elif self.peek()[1] == "{":
            meta[key] = self.dictionary()
        else:
            start = self.peek()[2]
            self.value()
            last = self.toks[self.i - 1]
            meta[key] = UnregisteredValue(self.text[start:last[2] + len(last[1])])

    # -- layer / prims ---------------------------------------------------------
    def layer(self):
        layer = Layer()
        if self.accept("("):
            self.metadata(layer.metadata, "layer")
        while self.peek()[0] != "eof":
            self.skip_seps()
            if self.peek()[0] == "eof":
                break
            t = self.peek()
            if t[1] in ("def", "over", "class"):
                self.prim(layer.root)
            elif t[1] == "reorder" and self.peek(1)[1] == "rootPrims":
                self.next(), self.next()
                self.expect("=")
                layer.root.metadata["primOrder"] = [str(x) for x in self.value()]
            else:
                self.error("expected prim, got %r" % t[1])
        layer.warnings = self.warnings
        return layer

    def prim(self, parent):
        spec = self.ident()
        type_name = ""
        if self.peek()[0] == "id":
            type_name = self.ident()
        name = self.string()
        prim = parent.define(name, type_name, spec)
        prim.type_name, prim.specifier = type_name, spec
        if self.accept("("):
            self.metadata(prim.metadata, "prim", prim.path)
        self.expect("{")
        self.prim_body(prim)
        return prim

    def prim_body(self, prim):
        while True:
            self.skip_seps()
            if self.accept("}"):
                return
            t = self.peek()
            if t[0] != "id":
                self.error("unexpected %r in prim body" % t[1])
            w = t[1]
            if w in ("def", "over", "class"):
                self.prim(prim)
            elif w == "variantSet":
                self.variant_set(prim)
            elif w == "reorder" and self.peek(1)[1] in ("nameChildren", "properties"):
                self.next()
                what = self.next()[1]
                self.expect("=")
                key = "primOrder" if what == "nameChildren" else "propertyOrder"
                prim.metadata[key] = [str(x) for x in self.value()]
            else:
                self.prop(prim)

    def variant_set(self, prim):
        self.next()
        set_name = self.string()
        self.expect("=")
        self.expect("{")
        while True:
            self.skip_seps()
            if self.accept("}"):
                return
            vname = self.string()
            v = prim.variant_sets.setdefault(set_name, {}).get(vname)
            if v is None:
                from .model import PrimSpec
                v = PrimSpec(vname, "", None)
                v.parent, v.variant_of = prim, (set_name, vname)
                prim.variant_sets[set_name][vname] = v
            if self.accept("("):
                self.metadata(v.metadata, "prim", v.path)
            self.expect("{")
            self.prim_body(v)

    def prop(self, prim):
        op = None
        if self.peek()[1] in _LISTOP_WORDS:
            op = _LISTOP_WORDS[self.next()[1]]
        custom = self.accept("custom")
        uniform = varying = False
        if self.peek()[1] in ("uniform", "varying", "config"):
            w = self.next()[1]
            uniform, varying = w == "uniform", w == "varying"
        if self.accept("rel"):
            name = self.ident()
            rel = prim.properties.get(name)
            if rel is None or rel.is_attribute:
                rel = prim.add_property(RelationshipSpec(name))
            rel.custom = rel.custom or custom
            rel.varying = rel.varying or varying
            if self.accept("="):
                if rel.targets is None:
                    rel.targets = ListOp()
                setattr(rel.targets, op or "explicit", self.targets(prim.path))
            if self.accept("("):
                self.metadata(rel.metadata, "prop", prim.path)
            return
        type_name = self.ident()
        if self.accept("["):
            self.expect("]")
            type_name += "[]"
        full = self.ident()
        name, suffix = full, None
        for sfx in (".timeSamples", ".connect", ".spline", ".default"):
            if full.endswith(sfx):
                name, suffix = full[:-len(sfx)], sfx[1:]
                break
        attr = prim.properties.get(name)
        if attr is None or not attr.is_attribute:
            attr = prim.add_property(AttributeSpec(name, type_name))
        attr.type_name = type_name
        attr.custom = attr.custom or custom
        attr.uniform = attr.uniform or uniform
        if split_type(type_name)[0] is None and type_name not in ("dictionary", "opaque"):
            self.warnings.append("unsupported attribute type %r (%s)" % (type_name, name))
        if suffix == "connect":
            self.expect("=")
            if attr.connections is None:
                attr.connections = ListOp()
            setattr(attr.connections, op or "explicit", self.targets(prim.path))
        elif suffix == "timeSamples":
            self.expect("=")
            attr.time_samples = self.time_samples(type_name)
        elif suffix == "spline":
            self.warnings.append("spline on %s ignored" % name)
            self.expect("=")
            self._skip_block()
        elif self.accept("="):
            attr.default = self.typed_value(type_name)
        if self.accept("("):
            self.metadata(attr.metadata, "prop", prim.path)

    def time_samples(self, type_name):
        self.expect("{")
        ts = {}
        while True:
            self.skip_seps()
            if self.accept("}"):
                return ts
            t = self.next()
            if t[0] != "num":
                self.i -= 1
                self.error("expected time")
            self.expect(":")
            ts[float(t[1])] = self.typed_value(type_name)
            if not self.accept(","):
                self.skip_seps()

    def _skip_block(self):
        self.expect("{")
        depth = 1
        while depth:
            s = self.next()[1]
            depth += {"{": 1, "}": -1}.get(s, 0)


def parse_value_text(text):
    """Parse a single USDA value (e.g. '(1, 2)', '"a"', '[1, 2]') into python."""
    return _Parser(text).value()


def read_usda(text):
    if isinstance(text, (bytes, bytearray)):
        text = text.decode("utf-8")
    if text.startswith("\ufeff"):
        text = text[1:]
    if not text.startswith("#usda"):
        raise ParseError("not a USDA file (missing '#usda' header)")
    return _Parser(text).layer()
