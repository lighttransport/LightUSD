"""Layer-level data model (Sdf-like, no composition)."""

from .values import (BLOCK, AssetPath, ListOp, Payload, Reference, coerce, split_type,
                    values_equal)

# Metadata field name -> value kind. Plain USD type names are coerced with
# types.coerce(); the special kinds are list ops and maps.
FIELD_TYPES = {
    # strings
    "comment": "string", "documentation": "string", "displayName": "string",
    "displayGroup": "string",
    # tokens
    "kind": "token", "interpolation": "token", "colorSpace": "token", "defaultPrim": "token",
    "upAxis": "token", "connectability": "token", "renderType": "token",
    "symmetryFunction": "token", "permission": "permission", "allowedTokens": "token[]",
    "primOrder": "token[]", "propertyOrder": "token[]",
    # bools / numbers
    "active": "bool", "hidden": "bool", "instanceable": "bool", "elementSize": "int",
    "metersPerUnit": "double", "kilogramsPerUnit": "double", "timeCodesPerSecond": "double",
    "framesPerSecond": "double", "startTimeCode": "double", "endTimeCode": "double",
    # dictionaries
    "customData": "dictionary", "assetInfo": "dictionary", "customLayerData": "dictionary",
    "clips": "dictionary", "sdrMetadata": "dictionary", "prefixSubstitutions": "dictionary",
    "suffixSubstitutions": "dictionary",
    # list ops / maps
    "apiSchemas": "tokenlistop", "variantSetNames": "stringlistop",
    "references": "referencelistop", "payload": "payloadlistop",
    "inherits": "pathlistop", "specializes": "pathlistop",
    "variantSelection": "variantmap", "subLayers": "string[]",
    "subLayerOffsets": "layeroffsets",
    # other registered Sdf / Usd fields
    "owner": "string", "sessionOwner": "string", "hasOwnedSubLayers": "bool",
    "colorConfiguration": "asset", "colorManagementSystem": "token",
    "expressionVariables": "dictionary", "renderSettingsPrimPath": "string",
    "displayUnit": "token", "unauthoredValuesIndex": "int", "clipSets": "stringlistop",
    "symmetryArguments": "dictionary", "symmetricPeer": "string", "limits": "dictionary",
    "payloadAssetDependencies": "asset[]", "arraySizeConstraint": "int64",
    "outputName": "token",
}

LISTOP_KINDS = ("tokenlistop", "stringlistop", "referencelistop", "payloadlistop", "pathlistop")


# ---------------------------------------------------------------------------
# Path helpers (paths are plain strings)
# ---------------------------------------------------------------------------

def child_path(parent, name):
    if parent == "/":
        return "/" + name
    if parent.endswith("}"):
        return parent + name
    return parent + "/" + name


def split_path(path):
    """Split a path into (parent, element, is_property). Root returns (None, '', False).

    element is a prim name, a property name, '{set=sel}' for variant paths or
    '[/target]' for target paths.
    """
    if path == "/" or path == "":
        return None, "", False
    if path.endswith("]"):
        depth, i = 0, len(path) - 1
        while i >= 0:
            c = path[i]
            if c == "]":
                depth += 1
            elif c == "[":
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        return path[:i], path[i:], False
    if path.endswith("}"):
        i = path.rfind("{")
        return path[:i], path[i:], False
    i = max(path.rfind("/"), path.rfind("."), path.rfind("}"), path.rfind("]"))
    c = path[i]
    if c == ".":
        return path[:i], path[i + 1:], True
    if c == "/":
        return (path[:i] or "/"), path[i + 1:], False
    return path[:i + 1], path[i + 1:], False


def join_element(parent, elem, is_prop):
    if is_prop:
        return parent + "." + elem
    if elem.startswith("{") or elem.startswith("["):
        return parent + elem
    return child_path(parent, elem)


def make_absolute(base, p):
    """Resolve relative path `p` against prim path `base`."""
    p = str(p)
    if not p or p.startswith("/"):
        return p
    while p == ".." or p.startswith("../"):
        base = split_path(base)[0] or "/"
        p = p[3:]
    if not p:
        return base
    if p.startswith("."):
        return base + p
    return child_path(base, p)


# ---------------------------------------------------------------------------
# Specs
# ---------------------------------------------------------------------------

class AttributeSpec:
    is_attribute = True

    def __init__(self, name, type_name, default=None, uniform=False, custom=False, metadata=None):
        self.name = name
        self.type_name = type_name
        self.default = coerce(type_name, default)
        self.time_samples = None      # dict {time: value} or None
        self.connections = None       # ListOp of path strings or None
        self.uniform = uniform
        self.custom = custom
        self.metadata = dict(metadata or {})
        self.owner = None

    @property
    def path(self):
        return (self.owner.path if self.owner else "") + "." + self.name

    def set(self, value, time=None):
        """Set the default value, or a time sample when `time` is given."""
        if time is None:
            self.default = coerce(self.type_name, value)
        else:
            if self.time_samples is None:
                self.time_samples = {}
            self.time_samples[float(time)] = coerce(self.type_name, value)
        return self

    def get(self, time=None):
        """Default value, or the held (step-interpolated) sample at `time`."""
        if time is None or not self.time_samples:
            return None if self.default is BLOCK else self.default
        times = sorted(self.time_samples)
        t = times[0]
        for tt in times:
            if tt <= time:
                t = tt
        v = self.time_samples[t]
        return None if v is BLOCK else v

    def connect(self, *targets, op="explicit"):
        """Connect this attribute (e.g. a shader input) to attribute path(s)."""
        if self.connections is None:
            self.connections = ListOp()
        setattr(self.connections, op, [str(getattr(t, "path", t)) for t in targets])
        return self

    def __repr__(self):
        return "<Attribute %s %s>" % (self.type_name, self.name)


class RelationshipSpec:
    is_attribute = False

    def __init__(self, name, targets=None, custom=False, varying=False, metadata=None):
        self.name = name
        self.targets = targets        # ListOp of path strings or None
        self.custom = custom
        self.varying = varying        # relationships are uniform unless 'varying'
        self.uniform = False
        self.metadata = dict(metadata or {})
        self.owner = None

    @property
    def path(self):
        return (self.owner.path if self.owner else "") + "." + self.name

    def set_targets(self, *targets, op="explicit"):
        if self.targets is None:
            self.targets = ListOp()
        setattr(self.targets, op, [str(getattr(t, "path", t)) for t in targets])
        return self

    def get_targets(self):
        return self.targets.items() if self.targets else []

    def __repr__(self):
        return "<Relationship %s>" % self.name


class PrimSpec:
    def __init__(self, name, type_name="", specifier="def", metadata=None):
        self.name = name
        self.type_name = type_name
        self.specifier = specifier    # 'def' | 'over' | 'class' (None for pseudo-root/variant)
        self.metadata = dict(metadata or {})
        self.properties = {}
        self.children = {}
        self.variant_sets = {}        # {set name: {variant name: PrimSpec}}
        self.parent = None
        self.variant_of = None           # (set, variant) when this spec is a variant

    # -- paths ---------------------------------------------------------------
    @property
    def path(self):
        if self.parent is None:
            return "/"
        if self.variant_of:
            return self.parent.path + "{%s=%s}" % self.variant_of
        return child_path(self.parent.path, self.name)

    # -- children ------------------------------------------------------------
    def define(self, name, type_name="", specifier="def"):
        """Get or create a child prim (updating its type/specifier)."""
        c = self.children.get(name)
        if c is None:
            c = PrimSpec(name, type_name, specifier)
            c.parent = self
            self.children[name] = c
        else:
            if type_name:
                c.type_name = type_name
            if specifier:
                c.specifier = specifier
        return c

    def add_child(self, prim):
        prim.parent = self
        self.children[prim.name] = prim
        return prim

    def child(self, name):
        return self.children.get(name)

    def remove_child(self, name):
        return self.children.pop(name, None)

    def prim_at(self, relpath):
        p = self
        for part in relpath.strip("/").split("/"):
            if part:
                p = p.children.get(part)
                if p is None:
                    return None
        return p

    def traverse(self):
        """Depth-first iteration over descendant prims (not into variants)."""
        for c in self.children.values():
            yield c
            yield from c.traverse()

    # -- properties ----------------------------------------------------------
    def create_attribute(self, name, type_name, value=None, uniform=False, custom=False, **metadata):
        a = self.properties.get(name)
        if a is None or not a.is_attribute or a.type_name != type_name:
            a = AttributeSpec(name, type_name, uniform=uniform, custom=custom)
            a.owner = self
            self.properties[name] = a
        a.uniform = a.uniform or uniform
        if value is not None:
            a.set(value)
        a.metadata.update(metadata)
        return a

    def set(self, name, value, type_name=None, uniform=None, **metadata):
        """Set an attribute value. type_name defaults to the existing attribute's
        type, then to well-known schema attribute types (see geom.ATTR_TYPES)."""
        a = self.properties.get(name)
        if type_name is None:
            if a is not None and a.is_attribute:
                type_name = a.type_name
            else:
                from .geom import guess_attr_type
                type_name, u = guess_attr_type(name, value)
                if uniform is None:
                    uniform = u
        return self.create_attribute(name, type_name, value, uniform=bool(uniform), **metadata)

    def get(self, name, time=None, default=None):
        a = self.properties.get(name)
        if a is None or not a.is_attribute:
            return default
        v = a.get(time)
        return default if v is None else v

    def attribute(self, name):
        a = self.properties.get(name)
        return a if a is not None and a.is_attribute else None

    def relationship(self, name):
        r = self.properties.get(name)
        return r if r is not None and not r.is_attribute else None

    def create_relationship(self, name, targets=None, custom=False):
        r = self.properties.get(name)
        if r is None or r.is_attribute:
            r = RelationshipSpec(name, custom=custom)
            r.owner = self
            self.properties[name] = r
        if targets is not None:
            if isinstance(targets, (str, PrimSpec, AttributeSpec)):
                targets = [targets]
            r.set_targets(*targets)
        return r

    def add_property(self, prop):
        prop.owner = self
        self.properties[prop.name] = prop
        return prop

    # -- composition arcs / metadata helpers --------------------------------
    def _listop(self, field, item, op):
        lo = self.metadata.get(field)
        if lo is None:
            lo = self.metadata[field] = ListOp()
        lst = getattr(lo, op)
        if lst is None:
            lst = []
            setattr(lo, op, lst)
        if item not in lst:
            lst.append(item)
        return lo

    def add_reference(self, asset_path="", prim_path="", op="prepended", offset=0.0, scale=1.0):
        return self._listop("references", Reference(asset_path, prim_path, offset, scale), op)

    def add_payload(self, asset_path="", prim_path="", op="prepended"):
        return self._listop("payload", Payload(asset_path, prim_path), op)

    def add_inherit(self, path, op="prepended"):
        return self._listop("inherits", str(path), op)

    def apply_api(self, schema, op="prepended"):
        return self._listop("apiSchemas", schema, op)

    def variant(self, set_name, variant_name):
        """Get or create the PrimSpec holding the contents of a variant."""
        vs = self.variant_sets.setdefault(set_name, {})
        v = vs.get(variant_name)
        if v is None:
            v = PrimSpec(variant_name, "", None)
            v.parent = self
            v.variant_of = (set_name, variant_name)
            vs[variant_name] = v
            names = self.metadata.get("variantSetNames")
            if names is None or set_name not in names.items():
                self._listop("variantSetNames", set_name, "prepended")
        return v

    def set_variant_selection(self, set_name, variant_name):
        self.metadata.setdefault("variantSelection", {})[set_name] = variant_name

    @property
    def kind(self):
        return self.metadata.get("kind")

    @kind.setter
    def kind(self, v):
        self.metadata["kind"] = v

    # pxr-flavored aliases
    GetPath = lambda self: self.path  # noqa: E731
    GetName = lambda self: self.name  # noqa: E731
    GetTypeName = lambda self: self.type_name  # noqa: E731
    GetChildren = lambda self: list(self.children.values())  # noqa: E731
    GetAttribute = attribute
    CreateAttribute = create_attribute
    GetRelationship = relationship
    CreateRelationship = create_relationship

    def __repr__(self):
        return "<Prim %s %s>" % (self.type_name or "-", self.path)


class Layer:
    """A single USD layer. Also serves as the (non-composing) Stage."""

    def __init__(self, up_axis=None, meters_per_unit=None, default_prim=None, metadata=None):
        self.metadata = dict(metadata or {})
        self.root = PrimSpec("", "", None)
        self.assets = {}   # extra files for USDZ packaging: {name: bytes}
        self.warnings = []
        if up_axis:
            self.metadata["upAxis"] = up_axis
        if meters_per_unit is not None:
            self.metadata["metersPerUnit"] = float(meters_per_unit)
        if default_prim:
            self.metadata["defaultPrim"] = default_prim

    # -- prim access ---------------------------------------------------------
    def define(self, path, type_name="", specifier="def"):
        """Define a prim at an absolute path, creating typeless ancestors."""
        parts = [p for p in str(path).split("/") if p]
        if not parts:
            raise ValueError("invalid prim path %r" % path)
        p = self.root
        for part in parts[:-1]:
            p = p.children.get(part) or p.define(part)
        prim = p.define(parts[-1], type_name, specifier)
        if "defaultPrim" not in self.metadata and len(parts) == 1 and specifier == "def":
            self.metadata["defaultPrim"] = parts[0]
        return prim

    def override(self, path):
        prim = self.prim_at(path)
        return prim if prim is not None else self.define(path, "", "over")

    def prim_at(self, path):
        path = str(path)
        if path == "/":
            return self.root
        if "{" in path:
            return None
        return self.root.prim_at(path)

    def remove_prim(self, path):
        parent, name, _ = split_path(str(path))
        p = self.prim_at(parent) if parent else None
        return p.remove_child(name) if p else None

    def property_at(self, path):
        prim_path, name, is_prop = split_path(str(path))
        prim = self.prim_at(prim_path) if is_prop else None
        return prim.properties.get(name) if prim else None

    def traverse(self):
        return self.root.traverse()

    @property
    def root_prims(self):
        return list(self.root.children.values())

    # -- layer metadata ------------------------------------------------------
    def _meta_prop(key, conv=lambda v: v):  # noqa: N805
        def g(self):
            return self.metadata.get(key)

        def s(self, v):
            if v is None:
                self.metadata.pop(key, None)
            else:
                self.metadata[key] = conv(v)
        return property(g, s)

    default_prim = _meta_prop("defaultPrim", str)
    up_axis = _meta_prop("upAxis", str)
    meters_per_unit = _meta_prop("metersPerUnit", float)
    start_time_code = _meta_prop("startTimeCode", float)
    end_time_code = _meta_prop("endTimeCode", float)
    time_codes_per_second = _meta_prop("timeCodesPerSecond", float)
    frames_per_second = _meta_prop("framesPerSecond", float)
    del _meta_prop

    # -- I/O -----------------------------------------------------------------
    def to_usda(self):
        from .usda_writer import write_usda
        return write_usda(self)

    def to_usdc(self):
        from .usdc_writer import write_usdc
        return write_usdc(self)

    def to_usdz(self, assets=None, root_format="usdc"):
        from .usdz import write_usdz
        return write_usdz(self, assets, root_format)

    def save(self, path, fmt=None, assets=None):
        """Save by extension: .usda (text), .usdc / .usd (crate), .usdz (package)."""
        path = str(path)
        ext = (fmt or path.rsplit(".", 1)[-1]).lower()
        if ext == "usda":
            data = self.to_usda().encode("utf-8")
        elif ext in ("usdc", "usd"):
            data = self.to_usdc()
        elif ext == "usdz":
            data = self.to_usdz(assets)
        else:
            raise ValueError("unknown USD format %r" % ext)
        with open(path, "wb") as f:
            f.write(data)
        return path

    # pxr-flavored aliases
    DefinePrim = define
    OverridePrim = override
    GetPrimAtPath = prim_at
    Traverse = traverse

    def Export(self, path):  # noqa: N802
        return self.save(path)

    def __repr__(self):
        return "<Layer %d root prims>" % len(self.root.children)


def specs_equal(a, b):
    """Structural equality of two layers or prims (for tests)."""
    if isinstance(a, Layer):
        return _meta_eq(a.metadata, b.metadata) and specs_equal(a.root, b.root)
    if (a.name, a.type_name, a.specifier, a.variant_of) != (b.name, b.type_name, b.specifier, b.variant_of):
        return False
    if not _meta_eq(a.metadata, b.metadata):
        return False
    if list(a.properties) != list(b.properties) or list(a.children) != list(b.children):
        return False
    for k, pa in a.properties.items():
        pb = b.properties[k]
        if pa.is_attribute != pb.is_attribute or not _meta_eq(pa.metadata, pb.metadata):
            return False
        if pa.custom != pb.custom or pa.uniform != pb.uniform \
                or getattr(pa, "varying", False) != getattr(pb, "varying", False):
            return False
        if pa.is_attribute:
            if pa.type_name != pb.type_name or not values_equal(pa.default, pb.default):
                return False
            if (pa.time_samples is None) != (pb.time_samples is None):
                return False
            if pa.time_samples is not None and (
                    list(pa.time_samples) != list(pb.time_samples)
                    or not all(values_equal(pa.time_samples[t], pb.time_samples[t])
                               for t in pa.time_samples)):
                return False
            if pa.connections != pb.connections:
                return False
        elif pa.targets != pb.targets:
            return False
    if not all(specs_equal(a.children[k], b.children[k]) for k in a.children):
        return False
    if list(a.variant_sets) != list(b.variant_sets):
        return False
    for s, vs in a.variant_sets.items():
        if list(vs) != list(b.variant_sets[s]):
            return False
        if not all(specs_equal(vs[k], b.variant_sets[s][k]) for k in vs):
            return False
    return True


def _meta_eq(a, b):
    return a.keys() == b.keys() and all(values_equal(a[k], b[k]) for k in a)


__all__ = ["Layer", "PrimSpec", "AttributeSpec", "RelationshipSpec", "FIELD_TYPES",
           "split_path", "child_path", "make_absolute", "specs_equal", "AssetPath",
           "split_type"]
