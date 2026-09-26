"""USDC (crate) reader -> Layer."""

import struct

from . import crate_format as C
from . import lz4
from .intcodec import read_compressed_ints
from .model import AttributeSpec, Layer, PrimSpec, RelationshipSpec, child_path, join_element
from .values import (BLOCK, CRATE_TO_TYPE, NP_DTYPE, TYPES, AssetPath, ListOp, Payload,
                     Reference, UnregisteredValue, array_from_flat, np, wrap_typed)

_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
_I64 = struct.Struct("<q")
_F32 = struct.Struct("<f")
_F16 = struct.Struct("<e")


class CrateError(ValueError):
    pass


class CrateReader:
    def __init__(self, data):
        self.buf = bytes(data)
        self.warnings = []
        b = self.buf
        if len(b) < C.BOOTSTRAP_SIZE or b[:8] != C.MAGIC:
            raise CrateError("not a USDC file")
        self.version = tuple(b[8:11])
        if self.version < (0, 4, 0) or self.version[:2] > (0, 12):
            raise CrateError("unsupported crate version %d.%d.%d" % self.version)
        (toc,) = _I64.unpack_from(b, 16)
        (n,) = _U64.unpack_from(b, toc)
        self.sections = {}
        for i in range(n):
            o = toc + 8 + i * 32
            name = b[o:o + 16].split(b"\0", 1)[0].decode()
            start, size = struct.unpack_from("<qq", b, o + 16)
            self.sections.setdefault(name, (start, size))
        self._read_tokens()
        self._read_strings()
        self._read_fields()
        self._read_fieldsets()
        self._read_paths()
        self._read_specs()

    # -- structural sections ---------------------------------------------------
    def _sec(self, name):
        if name not in self.sections:
            raise CrateError("missing %s section" % name)
        return self.sections[name][0]

    def _read_tokens(self):
        pos = self._sec("TOKENS")
        n, usize, csize = struct.unpack_from("<QQQ", self.buf, pos)
        raw = lz4.decompress(self.buf[pos + 24:pos + 24 + csize], usize)
        toks = raw.split(b"\0")
        self.tokens = [t.decode("utf-8") for t in toks[:n]]

    def _read_strings(self):
        pos = self._sec("STRINGS")
        (n,) = _U64.unpack_from(self.buf, pos)
        idx = struct.unpack_from("<%dI" % n, self.buf, pos + 8)
        self.strings = [self.tokens[i] for i in idx]

    def _read_fields(self):
        pos = self._sec("FIELDS")
        (n,) = _U64.unpack_from(self.buf, pos)
        tok_idx, pos = read_compressed_ints(self.buf, pos + 8, n)
        (csize,) = _U64.unpack_from(self.buf, pos)
        reps = lz4.decompress(self.buf[pos + 8:pos + 8 + csize], n * 8) if csize else b""
        self.fields = [(self.tokens[t], r) for t, r in
                       zip(tok_idx, struct.unpack_from("<%dQ" % n, reps))]

    def _read_fieldsets(self):
        pos = self._sec("FIELDSETS")
        (n,) = _U64.unpack_from(self.buf, pos)
        self.fieldsets, _ = read_compressed_ints(self.buf, pos + 8, n)

    def _read_paths(self):
        pos = self._sec("PATHS")
        (n,) = _U64.unpack_from(self.buf, pos)
        self.paths = [""] * n
        if n == 0:
            self.paths = ["/"]
            return
        (ne,) = _U64.unpack_from(self.buf, pos + 8)
        pos += 16
        pidx, pos = read_compressed_ints(self.buf, pos, ne)
        etok, pos = read_compressed_ints(self.buf, pos, ne, signed=True)
        jumps, pos = read_compressed_ints(self.buf, pos, ne, signed=True)
        # iterative pre-order decode: stack of (node index, parent path)
        stack = [(0, None)]
        seen = set()
        while stack:
            i, parent = stack.pop()
            while i < ne and i not in seen:
                seen.add(i)
                if parent is None:
                    path = "/"
                else:
                    t = etok[i]
                    path = join_element(parent, self.tokens[-t if t < 0 else t], t < 0)
                self.paths[pidx[i]] = path
                j = jumps[i]
                has_child = j > 0 or j == -1
                has_sib = j >= 0
                if has_sib:
                    sib = i + (j if j > 0 else 1)
                    if has_child:
                        stack.append((sib, parent))
                    else:
                        i = sib
                        continue
                if has_child:
                    parent = path
                    i += 1
                    continue
                break

    def _read_specs(self):
        pos = self._sec("SPECS")
        (n,) = _U64.unpack_from(self.buf, pos)
        pidx, pos = read_compressed_ints(self.buf, pos + 8, n)
        fsidx, pos = read_compressed_ints(self.buf, pos, n)
        stype, pos = read_compressed_ints(self.buf, pos, n)
        self.specs = {}
        fs = self.fieldsets
        for p, f, t in zip(pidx, fsidx, stype):
            fields = []
            while f < len(fs) and fs[f] != 0xFFFFFFFF:
                fields.append(self.fields[fs[f]])
                f += 1
            self.specs[self.paths[p]] = (t, fields)

    # -- values ---------------------------------------------------------------
    def type_name_of(self, rep):
        t = C.rep_type(rep)
        if t in CRATE_TO_TYPE:
            return CRATE_TO_TYPE[t] + ("[]" if rep & C.REP_ARRAY else "")
        return {C.DICTIONARY: "dictionary", C.TOKEN_VECTOR: "token[]",
                C.STRING_VECTOR: "string[]", C.DOUBLE_VECTOR: "double[]"}.get(t, "")

    def value(self, rep):
        t = C.rep_type(rep)
        payload = rep & C.PAYLOAD_MASK
        b = self.buf
        if rep & C.REP_ARRAY_EDIT:
            self.warnings.append("array-edit values are not supported")
            return None
        if t == C.VALUE_BLOCK:
            return BLOCK
        if t in CRATE_TO_TYPE:
            vt = TYPES[CRATE_TO_TYPE[t]]
            if rep & C.REP_ARRAY:
                return self._array(vt, payload, bool(rep & C.REP_COMPRESSED))
            return self._scalar(vt, payload, bool(rep & C.REP_INLINED))
        if t == C.SPECIFIER:
            return C.SPECIFIERS[payload] if payload < 3 else "def"
        if t == C.PERMISSION:
            return C.PERMISSIONS[payload & 1]
        if t == C.VARIABILITY:
            return payload == 1
        if t == C.DICTIONARY:
            return self._dict_at(payload)[0] if payload else {}
        if t in (C.TOKEN_VECTOR, C.STRING_VECTOR, C.PATH_VECTOR):
            if not payload:
                return []
            (n,) = _U64.unpack_from(b, payload)
            tbl = {C.TOKEN_VECTOR: self.tokens, C.STRING_VECTOR: self.strings,
                   C.PATH_VECTOR: self.paths}[t]
            return [tbl[i] for i in struct.unpack_from("<%dI" % n, b, payload + 8)]
        if t == C.DOUBLE_VECTOR:
            (n,) = _U64.unpack_from(b, payload)
            return list(struct.unpack_from("<%dd" % n, b, payload + 8))
        if t == C.LAYER_OFFSET_VECTOR:
            (n,) = _U64.unpack_from(b, payload)
            v = struct.unpack_from("<%dd" % (2 * n), b, payload + 8)
            return [(v[i], v[i + 1]) for i in range(0, 2 * n, 2)]
        if t == C.VARIANT_SELECTION_MAP:
            if not payload:
                return {}
            (n,) = _U64.unpack_from(b, payload)
            v = struct.unpack_from("<%dI" % (2 * n), b, payload + 8)
            return {self.strings[v[i]]: self.strings[v[i + 1]] for i in range(0, 2 * n, 2)}
        if t == C.TIME_SAMPLES:
            return self._time_samples(payload)
        if t in _LISTOP_ITEMS:
            return self._listop(payload, _LISTOP_ITEMS[t])
        if t == C.UNREGISTERED_VALUE:
            (off,) = _I64.unpack_from(b, payload)
            (nested,) = _U64.unpack_from(b, payload + off)
            if C.rep_type(nested) == C.STRING:
                return UnregisteredValue(self.value(nested))
            return self.value(nested)
        if t == C.PAYLOAD:
            return self._ref_item(payload, Payload)[0]
        self.warnings.append("unsupported crate value type %d" % t)
        return None

    def _scalar(self, vt, payload, inlined):
        b = self.buf
        if vt.kind in ("string", "token", "asset"):
            idx = payload if inlined else _U32.unpack_from(b, payload)[0]
            if vt.kind == "string":
                return self.strings[idx]
            s = self.tokens[idx]
            return AssetPath(s) if vt.kind == "asset" else s
        if inlined:
            f = vt.fmt
            if vt.n == 2 and f == "e":  # GfVec2h: raw 4 bytes in the payload
                return struct.unpack("<2e", (payload & 0xFFFFFFFF).to_bytes(4, "little"))
            if vt.n > 1:
                raw = payload.to_bytes(8, "little")
                comps = struct.unpack("<8b", raw)
                conv = int if f == "i" else float
                if vt.kind == "mat":
                    d = vt.dim
                    return tuple(tuple(conv(comps[r]) if r == c else conv(0) for c in range(d))
                                 for r in range(d))
                return tuple(conv(x) for x in comps[:vt.n])
            if f == "?":
                return bool(payload)
            if f in "BI":
                return payload & 0xFFFFFFFF
            if f == "i":
                v = payload & 0xFFFFFFFF
                return v - (1 << 32) if v >= (1 << 31) else v
            if f == "q":
                if payload >> 32 == 0:
                    v = payload
                    return v - (1 << 32) if v >= (1 << 31) else v
                return payload - (1 << 48) if payload >= (1 << 47) else payload
            if f == "Q":
                return payload
            if f == "e":
                return _F16.unpack(struct.pack("<H", payload & 0xFFFF))[0]
            return _F32.unpack(struct.pack("<I", payload & 0xFFFFFFFF))[0]  # float / double
        vals = struct.unpack_from("<%d%s" % (vt.n, vt.fmt), b, payload)
        if vt.n == 1:
            return vals[0]
        if vt.kind == "quat":
            return (vals[3], vals[0], vals[1], vals[2])
        if vt.kind == "mat":
            d = vt.dim
            return tuple(tuple(vals[r * d:r * d + d]) for r in range(d))
        return tuple(vals)

    def _array(self, vt, payload, compressed):
        b = self.buf
        if payload == 0:
            n, pos = 0, 0
        elif self.version < (0, 7, 0):
            (n,) = _U32.unpack_from(b, payload)
            pos = payload + 4
        else:
            (n,) = _U64.unpack_from(b, payload)
            pos = payload + 8
        if vt.kind in ("string", "token", "asset"):
            idx = struct.unpack_from("<%dI" % n, b, pos) if n else ()
            if vt.kind == "string":
                return [self.strings[i] for i in idx]
            if vt.kind == "asset":  # asset[] elements are string indices
                return [AssetPath(self.strings[i]) for i in idx]
            return [self.tokens[i] for i in idx]
        f = vt.fmt
        if compressed and n and vt.n == 1:
            if f in "iIqQ":
                flat, _ = read_compressed_ints(b, pos, n, f in "qQ", f in "iq")
            elif f in "efd":
                code = chr(b[pos])
                pos += 1
                if code == "i":
                    flat, _ = read_compressed_ints(b, pos, n, False, True)
                    flat = [float(x) for x in flat]
                elif code == "t":
                    (lsz,) = _U32.unpack_from(b, pos)
                    lut = struct.unpack_from("<%d%s" % (lsz, f), b, pos + 4)
                    pos += 4 + lsz * struct.calcsize(f)
                    idx, _ = read_compressed_ints(b, pos, n)
                    flat = [lut[i] for i in idx]
                else:
                    raise CrateError("unknown float array compression code %r" % code)
            else:
                raise CrateError("unexpected compressed %s array" % vt.name)
            return array_from_flat(flat, vt)
        count = n * vt.n
        if np is not None:
            flat = np.frombuffer(b, dtype=NP_DTYPE[f], count=count, offset=pos).copy() if count else []
        else:
            flat = struct.unpack_from("<%d%s" % (count, f), b, pos)
        arr = array_from_flat(flat, vt)
        if vt.kind == "quat" and n:
            if np is not None:
                arr = arr[:, [3, 0, 1, 2]]
            else:
                arr = [(q[3], q[0], q[1], q[2]) for q in arr]
        return arr

    def _dict_at(self, pos):
        """Read a VtDictionary at pos. Returns (dict, end position)."""
        b = self.buf
        (n,) = _U64.unpack_from(b, pos)
        pos += 8
        d = {}
        for _ in range(n):
            (k,) = _U32.unpack_from(b, pos)
            pos += 4
            (off,) = _I64.unpack_from(b, pos)
            rep_pos = pos + off
            (rep,) = _U64.unpack_from(b, rep_pos)
            pos = rep_pos + 8
            v = self.value(rep)
            if v is not None:
                d[self.strings[k]] = wrap_typed(self.type_name_of(rep), v)
        return d, pos

    def _time_samples(self, p):
        b = self.buf
        (off,) = _I64.unpack_from(b, p)
        tpos = p + off
        (trep,) = _U64.unpack_from(b, tpos)
        times = self.value(trep)
        times = [float(t) for t in (times.tolist() if hasattr(times, "tolist") else times)]
        q = tpos + 8
        (off,) = _I64.unpack_from(b, q)
        vpos = q + off
        (n,) = _U64.unpack_from(b, vpos)
        reps = struct.unpack_from("<%dQ" % n, b, vpos + 8)
        return {t: self.value(r) for t, r in zip(times, reps)}

    def _ref_item(self, pos, cls):
        b = self.buf
        a, p = struct.unpack_from("<II", b, pos)
        pos += 8
        item = cls(self.strings[a], self.paths[p] if self.paths[p] != "/" else "")
        if cls is Reference or self.version >= (0, 8, 0):
            item.offset, item.scale = struct.unpack_from("<dd", b, pos)
            pos += 16
        if cls is Reference:
            item.custom_data, pos = self._dict_at(pos)
        return item, pos

    def _listop(self, pos, kind):
        lo = ListOp()
        if not pos:
            return lo
        b = self.buf
        header = b[pos]
        pos += 1
        for field, bit in C.LO_RUNS:
            if not header & bit:
                continue
            (n,) = _U64.unpack_from(b, pos)
            pos += 8
            items = []
            if kind in ("token", "string", "path"):
                idx = struct.unpack_from("<%dI" % n, b, pos)
                pos += 4 * n
                tbl = {"token": self.tokens, "string": self.strings, "path": self.paths}[kind]
                items = [tbl[i] for i in idx]
            elif kind in ("reference", "payload"):
                cls = Reference if kind == "reference" else Payload
                for _ in range(n):
                    it, pos = self._ref_item(pos, cls)
                    items.append(it)
            else:
                items = list(struct.unpack_from("<%d%s" % (n, kind), b, pos))
                pos += n * struct.calcsize(kind)
            setattr(lo, field, items)
        if header & C.LO_IS_EXPLICIT and lo.explicit is None:
            lo.explicit = []
        return lo

    # -- layer ------------------------------------------------------------------
    def fields_of(self, path):
        s = self.specs.get(path)
        return (s[0], s[1]) if s else (None, [])

    def build_layer(self):
        layer = Layer()
        _, fields = self.fields_of("/")
        for name, rep in fields:
            if name == "primChildren":
                continue
            v = self.value(rep)
            if v is None:
                continue
            if name == "subLayerOffsets" and all(tuple(o) == (0.0, 1.0) for o in v):
                continue
            if name == "primOrder":
                layer.root.metadata[name] = v
            else:
                layer.metadata[name] = v
        self._build_children(layer.root, "/", fields)
        layer.warnings = self.warnings
        return layer

    def _build_children(self, prim, path, fields):
        fd = dict(fields)
        for name in self._names(fd, "properties"):
            ppath = path + "." + name
            st, pf = self.fields_of(ppath)
            if st == C.SPEC_ATTRIBUTE:
                prim.add_property(self._attribute(name, pf))
            elif st == C.SPEC_RELATIONSHIP:
                prim.add_property(self._relationship(name, pf))
        for name in self._names(fd, "primChildren"):
            cpath = child_path(path, name)
            st, cf = self.fields_of(cpath)
            if st != C.SPEC_PRIM:
                continue
            child = PrimSpec(name, "", None)
            prim.add_child(child)
            self._prim(child, cpath, cf)
        for set_name in self._names(fd, "variantSetChildren"):
            _, sf = self.fields_of(path + "{%s=}" % set_name)
            for vname in self._names(dict(sf), "variantChildren"):
                vpath = path + "{%s=%s}" % (set_name, vname)
                _, vf = self.fields_of(vpath)
                v = PrimSpec(vname, "", None)
                v.parent, v.variant_of = prim, (set_name, vname)
                prim.variant_sets.setdefault(set_name, {})[vname] = v
                self._prim(v, vpath, vf)

    def _names(self, fd, key):
        rep = fd.get(key)
        return self.value(rep) if rep is not None else []

    _PRIM_STRUCT = {"primChildren", "properties", "variantSetChildren", "variantChildren"}

    def _prim(self, prim, path, fields):
        for name, rep in fields:
            if name in self._PRIM_STRUCT:
                continue
            v = self.value(rep)
            if v is None:
                continue
            name = _FIELD_ALIASES.get(name, name)
            if isinstance(v, Payload) and name == "payload":  # pre-0.8 single SdfPayload
                v = ListOp(explicit=[v] if (v.asset_path or v.prim_path) else [])
            if name == "specifier":
                prim.specifier = v
            elif name == "typeName":
                prim.type_name = v
            else:
                prim.metadata[name] = v
        if prim.variant_of is None and prim.specifier is None:
            prim.specifier = "over"
        self._build_children(prim, path, fields)

    def _attribute(self, name, fields):
        a = AttributeSpec(name, "")
        for fname, rep in fields:
            if fname in ("connectionChildren",):
                continue
            v = self.value(rep)
            if v is None:
                continue
            if fname == "typeName":
                a.type_name = v
            elif fname == "default":
                a.default = v
            elif fname == "timeSamples":
                a.time_samples = v
            elif fname == "connectionPaths":
                a.connections = v
            elif fname == "variability":
                a.uniform = v
            elif fname == "custom":
                a.custom = v
            else:
                a.metadata[fname] = v
        if not a.type_name:
            fd = dict(fields)
            if "default" in fd:
                a.type_name = self.type_name_of(fd["default"]) or "token"
        return a

    def _relationship(self, name, fields):
        r = RelationshipSpec(name)
        for fname, rep in fields:
            if fname == "targetChildren":
                continue
            v = self.value(rep)
            if v is None:
                continue
            if fname == "targetPaths":
                r.targets = v
            elif fname == "custom":
                r.custom = v
            elif fname == "variability":
                r.varying = not v
            else:
                r.metadata[fname] = v
        return r


_FIELD_ALIASES = {"inheritPaths": "inherits"}

_LISTOP_ITEMS = {C.TOKEN_LISTOP: "token", C.STRING_LISTOP: "string", C.PATH_LISTOP: "path",
                 C.REFERENCE_LISTOP: "reference", C.PAYLOAD_LISTOP: "payload",
                 C.INT_LISTOP: "i", C.INT64_LISTOP: "q", C.UINT_LISTOP: "I", C.UINT64_LISTOP: "Q"}


def read_usdc(data):
    return CrateReader(data).build_layer()
