# SPDX-License-Identifier: Apache-2.0
"""Write a converted stage as an atomic component asset.

``save_component(stage, directory)`` splits a stage into the layer layout the
urdf-usd-converter and mujoco-usd-converter produce::

    <directory>/<name>.usda            interface: default prim /<name>,
                                        kind=component, assetInfo, payload
    <directory>/Payload/Contents.usda  sublayers Physics, Materials, Geometry
    <directory>/Payload/Geometry.usda  prims, transforms, geometry, visuals
    <directory>/Payload/Materials.usda Material networks + material:binding
    <directory>/Payload/Physics.usda   physics schemas/attributes (overs),
                                        joints, tendons, actuators, scene...
    <directory>/Payload/GeometryLibrary.usda  one prim per distinct mesh,
                                        referenced by the Geometry layer
    <directory>/Payload/Textures/      copied texture files

The stage's root prim (``/World`` for native imports) becomes ``/<name>``;
relationship targets and connections are rewritten with it. Opening the
interface file composes back to the original prims and values.
"""
from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from typing import Optional

__all__ = ["ComponentLayers", "save_component"]

_PHYSICS_PREFIXES = ("physics:", "newton:", "mjc:", "urdf:", "physx", "drive:", "limit:", "state:")
_PHYSICS_SCHEMA_PREFIXES = ("Physics", "Newton", "Mjc", "Physx")
_PHYSICS_SCOPES = ("Joints", "Tendons", "Equalities", "Actuators", "MjcActuators", "Sensors",
                   "Keyframes", "PhysicsScene", "PhysicsMaterials", "ContactPairs", "Deformables")
_ATTRIBUTE_METADATA = ("interpolation", "elementSize")
# Mesh attributes describing the shape itself; identical sets share one
# GeometryLibrary prim, while transforms, colors and bindings stay per use.
_MESH_INTRINSIC = frozenset((
    "points", "faceVertexIndices", "faceVertexCounts", "normals", "extent",
    "primvars:st", "primvars:st:indices", "primvars:normals", "primvars:normals:indices",
    "holeIndices", "cornerIndices", "cornerSharpnesses", "creaseIndices", "creaseLengths",
    "creaseSharpnesses", "subdivisionScheme", "orientation", "interpolateBoundary",
    "faceVaryingLinearInterpolation", "triangleSubdivisionRule"))


@dataclass(frozen=True)
class ComponentLayers:
    """Paths written by :func:`save_component`."""

    interface: str
    contents: str
    geometry: str
    materials: str
    physics: str
    geometry_library: Optional[str] = None


def _category(rel_path: str, root: str) -> Optional[str]:
    """Layer owning a whole prim (by top-level scope), or None if split."""
    parts = rel_path[len(root):].strip("/").split("/")
    head = parts[0] if parts and parts[0] else ""
    if head == "Materials":
        return "materials"
    if head in _PHYSICS_SCOPES:
        return "physics"
    return None


def _property_layer(name: str) -> str:
    if name == "material:binding" or name.startswith("material:binding:") and name != "material:binding:physics":
        return "materials"
    if name == "material:binding:physics" or name.startswith(_PHYSICS_PREFIXES):
        return "physics"
    return "geometry"


def _schema_layer(schema: str) -> str:
    if schema == "MaterialBindingAPI":
        return "materials"
    return "physics" if schema.startswith(_PHYSICS_SCHEMA_PREFIXES) else "geometry"


def _intrinsic_value(attribute):
    """Comparable value of a mesh-intrinsic attribute (None to keep it local)."""
    if attribute.is_connection or attribute.has_timesamples:
        return None
    try:
        value = attribute.get()
    except KeyError:
        return None
    if value is None:
        return None
    try:
        import numpy as np
        array = np.asarray(value)
        if array.dtype != object:
            value = (array.dtype.str, array.shape, array.tobytes())
    except ImportError:
        pass
    if not isinstance(value, tuple):
        value = repr(value)
    meta = tuple(repr(attribute.metadata(key)) for key in _ATTRIBUTE_METADATA)
    return (attribute.type_name, attribute.is_uniform, value, meta)


def save_component(stage, directory, *, name: Optional[str] = None,
                   format: str = "usda", textures: str = "copy",
                   asset_root=None, geometry_library: bool = True) -> ComponentLayers:
    """Write ``stage`` under ``directory`` as an atomic component.

    ``textures="copy"`` copies every asset-valued file that exists (absolute,
    or relative to ``asset_root``) into ``Payload/Textures/`` and rewrites the
    path to ``./Textures/<file>``; ``"keep"`` leaves asset paths unchanged.
    Paths that cannot be resolved are kept as authored.

    With ``geometry_library`` (the default) each distinct mesh shape is
    written once to ``Payload/GeometryLibrary`` and referenced from the
    Geometry layer, so meshes shared by visuals, colliders or repeated links
    are stored once.
    """
    import lightusd

    if textures not in ("copy", "keep"):
        raise ValueError("textures must be 'copy' or 'keep'")
    directory = os.fspath(directory)
    texture_dir = os.path.join(directory, "Payload", "Textures")
    copied_assets = {}
    generated = stage.generated_assets()

    def relocate(path):
        if not isinstance(path, str) or not path:
            return path
        if path in generated:
            # Importer-generated (MJCF builtin texture): no source file exists.
            os.makedirs(texture_dir, exist_ok=True)
            out_name = os.path.basename(path)
            with open(os.path.join(texture_dir, out_name), "wb") as f:
                f.write(generated[path])
            return "./Textures/" + out_name
        if textures != "copy":
            return path
        source = path if os.path.isabs(path) or asset_root is None else \
            os.path.join(os.fspath(asset_root), path)
        if not os.path.isfile(source):
            return path
        source = os.path.realpath(source)
        if source not in copied_assets:
            base, ext = os.path.splitext(os.path.basename(source))
            used = set(copied_assets.values())
            name_out, k = base + ext, 1
            while name_out in used:
                name_out, k = f"{base}_{k}{ext}", k + 1
            os.makedirs(texture_dir, exist_ok=True)
            shutil.copyfile(source, os.path.join(texture_dir, name_out))
            copied_assets[source] = name_out
        return "./Textures/" + copied_assets[source]
    root_prim = stage.default_prim or (stage.root_prims[0] if stage.root_prims else None)
    if root_prim is None:
        raise ValueError("stage has no root prim")
    old_root = root_prim.path
    name = name or (stage.get_metadata("defaultPrim") and root_prim.name) or "Asset"
    if name == "World":
        name = "Asset"
    new_root = "/" + name

    def remap(path: str) -> str:
        if path == old_root or path.startswith(old_root + "/") or path.startswith(old_root + "."):
            return new_root + path[len(old_root):]
        return path

    layers = {key: lightusd.Stage.create() for key in ("geometry", "materials", "physics")}
    for layer in layers.values():
        layer.up_axis = stage.up_axis
        layer.meters_per_unit = stage.meters_per_unit
        kpu = stage.get_metadata("kilogramsPerUnit")
        if kpu is not None:
            layer.set_metadata("kilogramsPerUnit", float(kpu))
        layer.set_default_prim(name)

    def target_prim(layer_key, prim, defining):
        layer = layers[layer_key]
        path = remap(prim.path)
        existing = layer.get_prim_at(path)
        if existing is not None:
            return existing
        if defining:
            return layer.define_prim(path, prim.type_name or None)
        return layer.override_prim(path)

    # Mesh path -> library prim path, and the attributes the library holds.
    library_of = {}
    library_attrs = {}
    library = None
    if geometry_library:
        library = lightusd.Stage.create()
        library.up_axis = stage.up_axis
        library.meters_per_unit = stage.meters_per_unit
        library.define_prim("/Geometry", "Scope")
        library.set_default_prim("Geometry")
        shapes, used = {}, set()
        for prim in stage:
            if prim.type_name != "Mesh" or not prim.path.startswith(old_root + "/") or \
                    _category(prim.path, old_root) is not None:
                continue
            values = {}
            for prop in prim.attributes:
                if prop in _MESH_INTRINSIC:
                    value = _intrinsic_value(prim.attribute(prop))
                    if value is not None:
                        values[prop] = value
            if "points" not in values:
                continue
            key = tuple(sorted(values.items()))
            if key not in shapes:
                base = prim.name
                lib_name, k = base, 1
                while lib_name in used:
                    lib_name, k = f"{base}_{k}", k + 1
                used.add(lib_name)
                target = library.define_prim(f"/Geometry/{lib_name}", "Mesh")
                for prop in values:
                    attribute = prim.attribute(prop)
                    target.set(prop, attribute.get(), type=attribute.type_name,
                               uniform=attribute.is_uniform)
                    fresh = library.prim_at(f"/Geometry/{lib_name}").attribute(prop)
                    for meta_key in _ATTRIBUTE_METADATA:
                        meta = attribute.metadata(meta_key)
                        if meta is not None:
                            fresh.set_metadata(meta_key, meta)
                shapes[key] = f"/Geometry/{lib_name}"
            library_of[prim.path] = shapes[key]
            library_attrs[prim.path] = set(values)

    for prim in stage:
        if not prim.path.startswith(old_root):
            continue
        owner = _category(prim.path, old_root)
        home = owner or "geometry"
        # The prim's definition (type, metadata) lives in its home layer.
        defined = target_prim(home, prim, True)
        if prim.path in library_of:
            defined.add_reference(f"./GeometryLibrary.{format}", library_of[prim.path])
            defined = layers[home].prim_at(remap(prim.path))
        for key in ("kind", "displayName", "instanceable", "active"):
            value = prim.metadata(key)
            if value is not None:
                defined.set_metadata(key, value)
        schemas = list(prim.metadata("apiSchemas") or ())
        by_layer = {}
        for schema in schemas:
            by_layer.setdefault(owner or _schema_layer(schema), []).append(schema)
        for layer_key, items in by_layer.items():
            target_prim(layer_key, prim, layer_key == home).set_metadata("prepend apiSchemas",
                                                                         tuple(items))
        shared = library_attrs.get(prim.path, ())
        for prop in prim.attributes:
            if prop in shared:
                continue  # authored on the referenced GeometryLibrary prim
            attribute = prim.attribute(prop)
            layer_key = owner or _property_layer(prop)
            target = target_prim(layer_key, prim, layer_key == home)
            try:
                value = attribute.get()
            except KeyError:  # declared without a default value
                value = None
            if attribute.type_name == "asset" and value is not None:
                value = relocate(value)
            elif attribute.type_name == "asset[]" and value is not None:
                value = [relocate(v) for v in value]
            if value is not None:
                target.set(prop, value, type=attribute.type_name, uniform=attribute.is_uniform)
            elif not attribute.has_timesamples:
                target.declare(prop, attribute.type_name, uniform=attribute.is_uniform)
            if attribute.is_connection:
                fresh = layers[layer_key].prim_at(remap(prim.path))  # handle after authoring
                for connection in attribute.connections:
                    fresh.attribute(prop).connect(remap(connection))
            if attribute.has_timesamples:
                for time, sample in attribute.timesamples:
                    target.set(prop, sample, type=attribute.type_name, time=time)
            fresh = layers[layer_key].prim_at(remap(prim.path))
            if prop in fresh.attributes:
                copied = fresh.attribute(prop)
                for key in _ATTRIBUTE_METADATA:
                    meta = attribute.metadata(key)
                    if meta is not None:
                        copied.set_metadata(key, meta)
        for rel in prim.relationships:
            layer_key = owner or _property_layer(rel)
            target = target_prim(layer_key, prim, layer_key == home)
            target.add_relationship(rel, [remap(t) for t in prim.relationship(rel).targets])

    os.makedirs(os.path.join(directory, "Payload"), exist_ok=True)
    library_path = None
    if library is not None and library_of:
        library_path = os.path.join(directory, "Payload", f"GeometryLibrary.{format}")
        library.save(library_path)
    paths = {}
    for key, title in (("geometry", "Geometry"), ("materials", "Materials"), ("physics", "Physics")):
        paths[key] = os.path.join(directory, "Payload", f"{title}.{format}")
        layers[key].save(paths[key])

    contents = lightusd.Stage.create()
    for title in ("Physics", "Materials", "Geometry"):
        contents.add_sublayer(f"./{title}.{format}")
    contents.up_axis = stage.up_axis
    contents.meters_per_unit = stage.meters_per_unit
    contents.set_default_prim(name)
    contents.define_prim(new_root, root_prim.type_name or "Xform")
    contents_path = os.path.join(directory, "Payload", f"Contents.{format}")
    contents.save(contents_path)

    # assetInfo is dictionary metadata, which the authoring API cannot set;
    # the interface layer is small enough to author as text.
    kpu = stage.get_metadata("kilogramsPerUnit")
    header = [f'    defaultPrim = "{name}"', f"    metersPerUnit = {_num(stage.meters_per_unit)}",
              f'    upAxis = "{stage.up_axis}"']
    if kpu is not None:
        header.append(f"    kilogramsPerUnit = {_num(float(kpu))}")
    text = "\n".join([
        "#usda 1.0", "(", *header, ")", "",
        f'def {root_prim.type_name or "Xform"} "{name}" (',
        f'    assetInfo = {{\n        string name = "{name}"\n    }}',
        '    kind = "component"',
        f"    payload = @./Payload/Contents.{format}@",
        ")", "{", "}", ""])
    interface_path = os.path.join(directory, f"{name}.{format}")
    if format == "usda":
        with open(interface_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        lightusd.loads(text).save(interface_path)
    return ComponentLayers(interface=interface_path, contents=contents_path,
                           geometry=paths["geometry"], materials=paths["materials"],
                           physics=paths["physics"], geometry_library=library_path)



def _num(value) -> str:
    text = repr(float(value))
    return text[:-2] if text.endswith(".0") else text
