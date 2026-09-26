"""USDC (crate) writer: Layer -> bytes (crate version 0.8.0)."""

import struct

from . import crate_format as C
from . import lz4
from .intcodec import compress_ints
from .model import FIELD_TYPES, split_path
from .values import (BLOCK, NP_DTYPE, ListOp, TypedValue, UnregisteredValue, flatten,
                     infer_type, np, split_type)

_Q = struct.Struct("<Q")
_I = struct.Struct("<I")

# model metadata key -> crate field name (when they differ)
CRATE_FIELD_NAMES = {"inherits": "inheritPaths"}
# metadata fields with non-array crate container types
_VECTOR_FIELDS = {"primOrder": C.TOKEN_VECTOR, "propertyOrder": C.TOKEN_VECTOR,
                  "subLayers": C.STRING_VECTOR}
_LISTOP_TYPES = {"tokenlistop": (C.TOKEN_LISTOP, "token"),
                 "stringlistop": (C.STRING_LISTOP, "string"),
                 "pathlistop": (C.PATH_LISTOP, "path"),
                 "referencelistop": (C.REFERENCE_LISTOP, "reference"),
                 "payloadlistop": (C.PAYLOAD_LISTOP, "payload")}


class CrateWriter:
    def __init__(self):
        self.out = bytearray(C.BOOTSTRAP_SIZE)
        self.tokens, self.tok_idx = [], {}
        self.strings, self.str_idx = [], {}
        self.paths, self.path_idx, self.path_children = [], {}, {}
        self.path_elem = []
        self.fields, self.field_idx = [], {}
        self.fieldsets, self.fs_idx = [], {}
        self.specs = []
        self.cache = {}
        self.warnings = []
        self.token("")
        self.path("/")

    # -- tables -----------------------------------------------------------------
    def token(self, s):
        s = str(s)
        i = self.tok_idx.get(s)
        if i is None:
            i = self.tok_idx[s] = len(self.tokens)
            self.tokens.append(s)
        return i

    def string(self, s):
        s = str(s)
        i = self.str_idx.get(s)
        if i is None:
            i = self.str_idx[s] = len(self.strings)
            self.strings.append(self.token(s))
        return i

    def path(self, p):
        p = str(p)
        i = self.path_idx.get(p)
        if i is not None:
            return i
        if p == "":
            # the empty path gets an index but no node in the path tree (as pxr)
            i = self.path_idx[p] = len(self.paths)
            self.paths.append(p)
            self.path_elem.append(0)
            return i
        parent, elem, is_prop = split_path(p)
        if parent is not None:
            self.path(parent)
        i = self.path_idx[p] = len(self.paths)
        self.paths.append(p)
        self.path_children[p] = []
        if parent is None:
            self.path_elem.append(0)
        else:
            t = self.token(elem)
            self.path_elem.append(-t if is_prop else t)
            self.path_children[parent].append(p)
        return i

    def add_spec(self, path, spec_type, fields):
        fis = []
        for name, rep in fields:
            key = (self.token(name), rep)
            fi = self.field_idx.get(key)
            if fi is None:
                fi = self.field_idx[key] = len(self.fields)
                self.fields.append(key)
            fis.append(fi)
        key = tuple(fis)
        fsi = self.fs_idx.get(key)
        if fsi is None:
            fsi = self.fs_idx[key] = len(self.fieldsets)
            self.fieldsets.extend(fis)
            self.fieldsets.append(0xFFFFFFFF)
        self.specs.append((self.path(path), fsi, spec_type))

    # -- values -----------------------------------------------------------------
    def _data(self, type_id, data, array=False):
        key = (type_id, array, data)
        off = self.cache.get(key)
        if off is None:
            off = self.cache[key] = len(self.out)
            self.out += data
        return C.make_rep(type_id, off, array=array)

    def pack(self, type_name, value):
        if isinstance(value, TypedValue):
            value = value.value
        if value is BLOCK:
            return C.make_rep(C.VALUE_BLOCK, 0, inlined=True)
        if type_name == "dictionary":
            return self.pack_dict(value)
        vt, is_array = split_type(type_name)
        if vt is None:
            raise TypeError("unsupported value type %r" % type_name)
        if is_array:
            return self.pack_array(vt, value)
        return self.pack_scalar(vt, value)

    def pack_scalar(self, vt, v):
        k, f = vt.kind, vt.fmt
        if k == "string":
            return C.make_rep(vt.crate, self.string(v), inlined=True)
        if k in ("token", "asset"):
            return C.make_rep(vt.crate, self.token(v), inlined=True)
        if vt.n == 1:
            if f == "?":
                return C.make_rep(vt.crate, 1 if v else 0, inlined=True)
            if f in "BiIef":
                raw = struct.pack("<" + f, int(v) if f in "BiI" else float(v))
                return C.make_rep(vt.crate, int.from_bytes(raw, "little"), inlined=True)
            return self._data(vt.crate, struct.pack("<" + f, int(v) if f in "qQ" else float(v)))
        flat = flatten(v)
        if len(flat) != vt.n:
            raise ValueError("%s expects %d components, got %d" % (vt.name, vt.n, len(flat)))
        if k == "quat":
            flat = flat[1:] + flat[:1]
        if vt.n == 2 and f == "e":  # GfVec2h fits in the payload: always inlined
            raw = struct.pack("<2e", *flat)
            return C.make_rep(vt.crate, int.from_bytes(raw, "little"), inlined=True)
        return self._data(vt.crate, struct.pack("<%d%s" % (vt.n, f), *flat))

    def pack_array(self, vt, value):
        k = vt.kind
        if k in ("string", "token", "asset"):
            fn = self.token if k == "token" else self.string  # asset[] -> string idx
            idx = [fn(s) for s in value]
            if not idx:
                return C.make_rep(vt.crate, 0, array=True)
            return self._data(vt.crate, _Q.pack(len(idx)) + struct.pack("<%dI" % len(idx), *idx), True)
        f = vt.fmt
        if np is not None:
            a = np.asarray(value, dtype=NP_DTYPE[f]).reshape(-1)
            if a.size % vt.n:
                raise ValueError("%s[]: %d values is not a multiple of %d" % (vt.name, a.size, vt.n))
            if k == "quat":
                a = a.reshape(-1, 4)[:, [1, 2, 3, 0]].reshape(-1)
            n = a.size // vt.n
            data = np.ascontiguousarray(a).tobytes()
        else:
            flat = flatten(value)
            if len(flat) % vt.n:
                raise ValueError("%s[]: %d values is not a multiple of %d" % (vt.name, len(flat), vt.n))
            if k == "quat":
                flat = [x for i in range(0, len(flat), 4) for x in flat[i + 1:i + 4] + flat[i:i + 1]]
            if f in "BiIqQ":
                flat = [int(x) for x in flat]
            n = len(flat) // vt.n
            data = struct.pack("<%d%s" % (len(flat), f), *flat)
        if n == 0:
            return C.make_rep(vt.crate, 0, array=True)
        return self._data(vt.crate, _Q.pack(n) + data, True)

    def pack_any(self, v):
        return self.pack(infer_type(v), v)

    def _write_dict(self, d):
        out = self.out
        out += _Q.pack(len(d))
        for k, v in d.items():
            out += _I.pack(self.string(k))
            rec = len(out)
            out += bytes(8)
            rep = self.pack_any(v)
            out[rec:rec + 8] = struct.pack("<q", len(out) - rec)
            out += _Q.pack(rep)

    def pack_dict(self, d):
        start = len(self.out)
        self._write_dict(d or {})
        return C.make_rep(C.DICTIONARY, start)

    def pack_time_samples(self, type_name, ts):
        out = self.out
        p = len(out)
        out += bytes(8)
        times = [float(t) for t in ts]
        times_rep = C.make_rep(C.DOUBLE_VECTOR, len(out))
        out += _Q.pack(len(times)) + struct.pack("<%dd" % len(times), *times)
        out[p:p + 8] = struct.pack("<q", len(out) - p)
        out += _Q.pack(times_rep)
        q = len(out)
        out += bytes(8)
        reps = [self.pack(type_name, v) for v in ts.values()]
        out[q:q + 8] = struct.pack("<q", len(out) - q)
        out += _Q.pack(len(reps)) + struct.pack("<%dQ" % len(reps), *reps)
        return C.make_rep(C.TIME_SAMPLES, p)

    def pack_listop(self, type_id, kind, lo):
        if isinstance(lo, (list, tuple)):
            lo = ListOp(explicit=lo)
        out = self.out
        start = len(out)
        header = C.LO_IS_EXPLICIT if lo.explicit is not None else 0
        for field, bit in C.LO_RUNS:
            if getattr(lo, field):
                header |= bit
        out.append(header)
        for field, bit in C.LO_RUNS:
            items = getattr(lo, field)
            if not items:
                continue
            out += _Q.pack(len(items))
            for it in items:
                if kind == "token":
                    out += _I.pack(self.token(it))
                elif kind == "string":
                    out += _I.pack(self.string(it))
                elif kind == "path":
                    out += _I.pack(self.path(it))
                else:
                    out += struct.pack("<II", self.string(it.asset_path),
                                       self.path(it.prim_path))
                    out += struct.pack("<dd", it.offset, it.scale)
                    if kind == "reference":
                        self._write_dict(it.custom_data or {})
        return C.make_rep(type_id, start)

    def pack_vector(self, type_id, items):
        if type_id == C.TOKEN_VECTOR:
            idx = [self.token(s) for s in items]
        elif type_id == C.STRING_VECTOR:
            idx = [self.string(s) for s in items]
        else:
            idx = [self.path(s) for s in items]
        return self._data(type_id, _Q.pack(len(idx)) + struct.pack("<%dI" % len(idx), *idx))

    def pack_meta(self, key, v):
        if isinstance(v, UnregisteredValue):
            # [i64 relative offset = 8][nested String ValueRep of the source text]
            p = len(self.out)
            self.out += struct.pack("<q", 8)
            self.out += _Q.pack(C.make_rep(C.STRING, self.string(v.text), inlined=True))
            return C.make_rep(C.UNREGISTERED_VALUE, p)
        ftype = FIELD_TYPES.get(key)
        if key in _VECTOR_FIELDS:
            return self.pack_vector(_VECTOR_FIELDS[key], v)
        if ftype in _LISTOP_TYPES:
            tid, kind = _LISTOP_TYPES[ftype]
            return self.pack_listop(tid, kind, v)
        if ftype == "variantmap":
            start = len(self.out)
            self.out += _Q.pack(len(v))
            for k, s in v.items():
                self.out += struct.pack("<II", self.string(k), self.string(s))
            return C.make_rep(C.VARIANT_SELECTION_MAP, start)
        if ftype == "layeroffsets":
            flat = [float(x) for o in v for x in o]
            return self._data(C.LAYER_OFFSET_VECTOR, _Q.pack(len(v)) + struct.pack("<%dd" % len(flat), *flat))
        if ftype == "permission":
            return C.make_rep(C.PERMISSION, C.PERMISSIONS.index(v), inlined=True)
        if ftype is not None:
            return self.pack(ftype, v)
        return self.pack_any(v)

    def _meta_fields(self, meta):
        if "subLayers" in meta and not meta.get("subLayerOffsets"):
            meta = dict(meta)
            meta["subLayerOffsets"] = [(0.0, 1.0)] * len(meta["subLayers"])
        fields = []
        for key, v in meta.items():
            if v is None:
                continue
            fields.append((CRATE_FIELD_NAMES.get(key, key), self.pack_meta(key, v)))
        return fields

    # -- specs --------------------------------------------------------------------
    def write_layer(self, layer):
        fields = self._meta_fields(layer.metadata)
        root_meta = {k: v for k, v in layer.root.metadata.items() if k == "primOrder"}
        fields += self._meta_fields(root_meta)
        if layer.root.children:
            fields.append(("primChildren", self.pack_vector(C.TOKEN_VECTOR, list(layer.root.children))))
        self.add_spec("/", C.SPEC_PSEUDOROOT, fields)
        for c in layer.root.children.values():
            self.write_prim(c, "/" + c.name)

    def write_prim(self, prim, path, is_variant=False):
        fields = []
        if not is_variant:
            fields.append(("specifier", C.make_rep(C.SPECIFIER, C.SPECIFIERS.index(prim.specifier or "def"),
                                                   inlined=True)))
        if prim.type_name:
            fields.append(("typeName", C.make_rep(C.TOKEN, self.token(prim.type_name), inlined=True)))
        fields += self._meta_fields(prim.metadata)
        if prim.properties:
            fields.append(("properties", self.pack_vector(C.TOKEN_VECTOR, list(prim.properties))))
        if prim.children:
            fields.append(("primChildren", self.pack_vector(C.TOKEN_VECTOR, list(prim.children))))
        if prim.variant_sets:
            fields.append(("variantSetChildren", self.pack_vector(C.TOKEN_VECTOR, list(prim.variant_sets))))
        self.add_spec(path, C.SPEC_VARIANT if is_variant else C.SPEC_PRIM, fields)
        for p in prim.properties.values():
            ppath = path + "." + p.name
            if p.is_attribute:
                self.write_attribute(p, ppath)
            else:
                self.write_relationship(p, ppath)
        for c in prim.children.values():
            self.write_prim(c, path + c.name if path.endswith("}") else path + "/" + c.name)
        for set_name, variants in prim.variant_sets.items():
            self.add_spec(path + "{%s=}" % set_name, C.SPEC_VARIANT_SET,
                          [("variantChildren", self.pack_vector(C.TOKEN_VECTOR, list(variants)))])
            for vname, v in variants.items():
                self.write_prim(v, path + "{%s=%s}" % (set_name, vname), is_variant=True)

    def write_attribute(self, a, path):
        fields = []
        if a.custom:
            fields.append(("custom", C.make_rep(C.BOOL, 1, inlined=True)))
        fields.append(("typeName", C.make_rep(C.TOKEN, self.token(a.type_name), inlined=True)))
        if a.uniform:
            fields.append(("variability", C.make_rep(C.VARIABILITY, 1, inlined=True)))
        supported = split_type(a.type_name)[0] is not None or a.type_name == "dictionary"
        if not supported and (a.default is not None or a.time_samples is not None):
            self.warnings.append("%s: values of type %r are not supported; skipped" % (path, a.type_name))
        elif a.default is not None:
            fields.append(("default", self.pack(a.type_name, a.default)))
        if supported and a.time_samples is not None:
            fields.append(("timeSamples", self.pack_time_samples(a.type_name, a.time_samples)))
        if a.connections is not None:
            fields.append(("connectionPaths", self.pack_listop(C.PATH_LISTOP, "path", a.connections)))
        fields += self._meta_fields(a.metadata)
        self.add_spec(path, C.SPEC_ATTRIBUTE, fields)

    def write_relationship(self, r, path):
        fields = []
        if r.custom:
            fields.append(("custom", C.make_rep(C.BOOL, 1, inlined=True)))
        fields.append(("variability", C.make_rep(C.VARIABILITY, 0 if r.varying else 1, inlined=True)))
        if r.targets is not None:
            fields.append(("targetPaths", self.pack_listop(C.PATH_LISTOP, "path", r.targets)))
        fields += self._meta_fields(r.metadata)
        self.add_spec(path, C.SPEC_RELATIONSHIP, fields)

    # -- sections -------------------------------------------------------------------
    def finish(self):
        out = self.out
        sections = []

        def begin(name):
            sections.append([name, len(out), 0])

        def end():
            sections[-1][2] = len(out) - sections[-1][1]

        # PATHS/SPECS may intern more tokens, so encode them before TOKENS.
        paths_data = self._paths_section()
        begin("TOKENS")
        raw = b"".join(t.encode("utf-8") + b"\0" for t in self.tokens)
        blob = lz4.compress(raw)
        out += struct.pack("<QQQ", len(self.tokens), len(raw), len(blob)) + blob
        end()
        begin("STRINGS")
        out += _Q.pack(len(self.strings)) + struct.pack("<%dI" % len(self.strings), *self.strings)
        end()
        begin("FIELDS")
        out += _Q.pack(len(self.fields))
        out += compress_ints([t for t, _ in self.fields])
        reps = lz4.compress(struct.pack("<%dQ" % len(self.fields), *[r for _, r in self.fields]))
        out += _Q.pack(len(reps)) + reps
        end()
        begin("FIELDSETS")
        out += _Q.pack(len(self.fieldsets)) + compress_ints(self.fieldsets)
        end()
        begin("PATHS")
        out += paths_data
        end()
        begin("SPECS")
        out += _Q.pack(len(self.specs))
        for col in range(3):
            out += compress_ints([s[col] for s in self.specs])
        end()
        toc = len(out)
        out += _Q.pack(len(sections))
        for name, start, size in sections:
            out += name.encode().ljust(16, b"\0") + struct.pack("<qq", start, size)
        out[0:8] = C.MAGIC
        out[8:16] = bytes(C.WRITE_VERSION) + bytes(5)
        out[16:24] = struct.pack("<q", toc)
        return bytes(out)

    def _paths_section(self):
        """Compressed path tree: pre-order nodes with (path index, element token,
        jump). jump: >0 child next + sibling at +jump, 0 sibling next only,
        -1 child next only, -2 leaf."""
        order, elems, pos = [], [], {}
        stack = ["/"]
        while stack:
            p = stack.pop()
            pos[p] = len(order)
            pi = self.path_idx[p]
            order.append(pi)
            elems.append(self.path_elem[pi])
            stack.extend(reversed(self.path_children[p]))
        jumps = [-1 if self.path_children[p] else -2 for p in pos]
        for p in pos:
            kids = self.path_children[p]
            for a, b in zip(kids, kids[1:]):
                ia = pos[a]
                jumps[ia] = pos[b] - ia if jumps[ia] == -1 else 0
        n = len(order)
        return (_Q.pack(len(self.paths)) + _Q.pack(n) + compress_ints(order)
                + compress_ints([e & 0xFFFFFFFF for e in elems])
                + compress_ints([j & 0xFFFFFFFF for j in jumps]))


def write_usdc(layer):
    w = CrateWriter()
    w.write_layer(layer)
    return w.finish()
