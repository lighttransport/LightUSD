from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
import json
import os

try:
    import unreal
except ImportError:
    unreal = None


@dataclass
class Result:
    succeeded: bool
    backend: str
    error: str = ""
    warnings: list[str] = field(default_factory=list)
    created_assets: list[str] = field(default_factory=list)
    prim_paths: list[str] = field(default_factory=list)
    root_layer: str = ""
    groom_assets: int = 0
    groom_bindings: int = 0
    groom_caches: int = 0

    @classmethod
    def from_ue(cls, value: Any) -> "Result":
        def get(name, default):
            if hasattr(value, name):
                return getattr(value, name)
            if unreal and hasattr(value, "get_editor_property"):
                try:
                    return value.get_editor_property(name)
                except Exception:
                    pass
            return default
        backend = get("backend_used", get("BackendUsed", "auto"))
        if hasattr(backend, "name"):
            backend = backend.name
        return cls(bool(get("b_succeeded", get("bSucceeded", False))), str(backend),
                   str(get("error", "")), list(get("warnings", [])),
                   list(get("created_assets", [])), list(get("prim_paths", [])),
                   str(get("root_layer", "")),
                   int(get("groom_assets", get("GroomAssets", 0))),
                   int(get("groom_bindings", get("GroomBindings", 0))),
                   int(get("groom_caches", get("GroomCaches", 0))))


def _options(backend: str, **kwargs):
    if backend not in {"auto", "native", "lightusd"}:
        raise ValueError("backend must be auto, native, or lightusd")
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    options = unreal.LightUSDUEOptions()
    enum = {"auto": unreal.LightUSDUEBackend.AUTO,
            "native": unreal.LightUSDUEBackend.NATIVE_UE,
            "lightusd": unreal.LightUSDUEBackend.LIGHT_USD}[backend]
    options.backend = enum
    for key, value in kwargs.items():
        prop = {"import_groom": "b_import_groom",
                "import_physics": "b_import_physics",
                "relink_metahuman": "b_relink_meta_human",
                "max_memory_mb": "max_memory_mb",
                "package_path": "package_path",
                "metahuman_asset_path": "meta_human_asset_path",
                "groom_target_skeletal_mesh_path": "groom_target_skeletal_mesh_path"}.get(key, key)
        if hasattr(options, prop):
            setattr(options, prop, value)
    return options


def _call(method: str, filename: str, backend: str, **kwargs) -> Result:
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    value = getattr(unreal.LightUSDUEBlueprintLibrary, method)(filename, _options(backend, **kwargs))
    return Result.from_ue(value)


def _python_import_usd(filename: str, **kwargs) -> Result:
    """Validate/load USD without the native LightUSDUE module.

    This is the Python-first UE profile: the `lightusd` CPython binding owns
    parsing and composition, while this facade only reports the stage to the
    caller. It intentionally does not claim to create UE assets.
    """
    try:
        import lightusd
        stage = lightusd.load(
            filename,
            composed=bool(kwargs.get("composed", True)),
            load_payloads=bool(kwargs.get("load_payloads", True)),
            max_memory=int(kwargs.get("max_memory_mb", 0)) * 1024 * 1024,
        )
    except Exception as exc:
        return Result(False, "LightUSD-Python", error=str(exc))

    prim_paths: list[str] = []

    def visit(prim):
        prim_paths.append(str(prim.path))
        for child in prim.children:
            visit(child)

    try:
        for root in stage.root_prims:
            visit(root)
        warning_text = str(stage.warnings() or "")
        warnings = [line for line in warning_text.splitlines() if line]
        if kwargs.get("import_groom") or kwargs.get("import_physics"):
            warnings.append(
                "Python-only profile parses USD data but does not create native UE assets")
        return Result(True, "LightUSD-Python", warnings=warnings,
                      prim_paths=prim_paths, root_layer=str(Path(filename).resolve()))
    finally:
        stage.close()


def import_usd(filename: str, *, backend="auto", **kwargs) -> Result:
    python_only = os.environ.get("LIGHTUSD_UE_PYTHON_ONLY", "0") == "1"
    if backend == "lightusd" and (python_only or unreal is None or
                                   not hasattr(unreal, "LightUSDUEBlueprintLibrary")):
        return _python_import_usd(filename, **kwargs)
    return _call("import_usd", filename, backend, **kwargs)


def export_usd(filename: str, *, backend="auto", **kwargs) -> Result:
    if backend in {"auto", "native"}:
        return _native_export(filename, **kwargs)
    return _call("export_usd", filename, backend, **kwargs)


def export_groom(groom_asset, filename: str, **kwargs) -> Result:
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    options = _options("lightusd", **kwargs)
    value = unreal.LightUSDUEBlueprintLibrary.export_groom(
        groom_asset, filename, options)
    return Result.from_ue(value)


def export_groom_cache(groom_cache, groom_asset, filename: str, **kwargs) -> Result:
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    options = _options("lightusd", **kwargs)
    value = unreal.LightUSDUEBlueprintLibrary.export_groom_cache(
        groom_cache, groom_asset, filename, options)
    return Result.from_ue(value)


def _export_asset(asset, filename: str, options: Any, *, label: str) -> Result:
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    if isinstance(asset, str):
        asset = unreal.EditorAssetLibrary.load_asset(asset)
    if not asset:
        return Result(False, "NativeUE", error=f"Missing {label} asset")
    try:
        task = unreal.AssetExportTask()
        task.object = asset
        task.filename = str(Path(filename).resolve())
        task.selected = False
        task.replace_identical = True
        task.prompt = False
        task.automated = True
        task.options = options
        if not unreal.Exporter.run_asset_export_task(task):
            return Result(False, "NativeUE", error=f"UE {label} exporter rejected the task")
        if not Path(task.filename).exists():
            return Result(False, "NativeUE", error=f"UE {label} exporter produced no USD file")
        return Result(True, "NativeUE", root_layer=task.filename,
                      created_assets=[task.filename])
    except Exception as exc:
        return Result(False, "NativeUE", error=f"UE {label} export failed: {exc}")


def export_skeletal_mesh(mesh, filename: str, **kwargs) -> Result:
    """Export a UE SkeletalMesh through UE's native USD exporter."""
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    return _export_asset(mesh, filename, unreal.SkeletalMeshExporterUSDOptions(),
                         label="skeletal mesh")


def export_skeletal_animation(animation, filename: str, **kwargs) -> Result:
    """Export a UE AnimSequence through UE's native USD exporter."""
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    asset = animation
    if isinstance(asset, str):
        asset = unreal.EditorAssetLibrary.load_asset(asset)
    result = _export_asset(asset, filename, unreal.AnimSequenceExporterUSDOptions(),
                           label="skeletal animation")
    if result.succeeded:
        _append_morph_curves_to_usda(asset, result.root_layer)
    return result


def _animation_float_curves(animation):
    if not animation or not unreal:
        return {}
    try:
        curves_result = unreal.LightUSDUEBlueprintLibrary.get_skeletal_animation_curves(animation)
        curves = curves_result[-1] if isinstance(curves_result, tuple) else curves_result
        if curves:
            return {
                str(curve.curve_name): [
                    (float(time), float(value))
                    for time, value in zip(curve.times, curve.values)]
                for curve in curves
            }
    except Exception:
        pass
    if not hasattr(unreal, "AnimationBlueprintLibrary"):
        return {}
    library = unreal.AnimationBlueprintLibrary
    curve_type = getattr(getattr(unreal, "RawCurveTrackTypes", None), "RCT_FLOAT", None)
    if curve_type is None:
        return {}
    try:
        names_result = library.get_animation_curve_names(animation, curve_type)
        names = names_result[-1] if isinstance(names_result, tuple) else names_result
        curves = {}
        skeleton = animation.get_skeleton() if hasattr(animation, "get_skeleton") else None
        for name in names or []:
            if skeleton and hasattr(library, "get_curve_meta_data_morph_target"):
                if not library.get_curve_meta_data_morph_target(skeleton, name):
                    continue
            keys_result = library.get_float_keys(animation, name)
            if not isinstance(keys_result, tuple) or len(keys_result) < 2:
                continue
            times, values = keys_result[-2], keys_result[-1]
            if times and values:
                curves[str(name)] = [(float(t), float(v)) for t, v in zip(times, values)]
        return curves
    except Exception:
        return {}


def _append_morph_curves_to_usda(animation, filename: str):
    """Fill the UE exporter gap for morph curves using standard UsdSkel fields."""
    curves = _animation_float_curves(animation)
    if not curves or not filename or not filename.lower().endswith(".usda"):
        return
    path = Path(filename)
    if not path.exists():
        return
    import bisect
    times = sorted({time for keys in curves.values() for time, _ in keys})
    lines = ["    uniform token[] blendShapes = [" + ", ".join(
        json.dumps(name) for name in curves) + "]",
             "    float[] blendShapeWeights.timeSamples = {"]
    for time in times:
        weights = []
        for keys in curves.values():
            key_times = [item[0] for item in keys]
            index = bisect.bisect_left(key_times, time)
            if index < len(keys) and key_times[index] == time:
                value = keys[index][1]
            elif index <= 0:
                value = keys[0][1]
            elif index >= len(keys):
                value = keys[-1][1]
            else:
                t0, v0 = keys[index - 1]
                t1, v1 = keys[index]
                alpha = (time - t0) / (t1 - t0) if t1 != t0 else 0.0
                value = v0 + (v1 - v0) * alpha
            weights.append(f"{value:.9g}")
        lines.append(f"        {time:g}: [" + ", ".join(weights) + "],")
    lines.append("    }")
    text = path.read_text(encoding="utf-8")
    if "blendShapeWeights" in text:
        return
    close = text.rfind("\n}")
    if close < 0:
        return
    path.write_text(text[:close] + "\n" + "\n".join(lines) + text[close:], encoding="utf-8")


def create_skeletal_animation(name: str, skeleton, tracks: dict[str, dict], *,
                              package_path="/Game/LightUSD/Animations",
                              frame_rate=30, num_frames=None,
                              preview_mesh=None) -> Result:
    """Author a persistent UE ``AnimSequence`` from USD/Blender-style tracks.

    ``tracks`` maps a UE bone name to ``translations``, ``rotations`` and
    ``scales`` arrays.  The arrays use UE Python ``Vector``/``Quat`` values;
    callers may also pass numeric tuples, which are converted here.  This is
    the explicit fallback for USD layers whose native importer only exposes a
    transient ``SkelAnimation`` evaluation.
    """
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")

    def vec(value, default):
        if isinstance(value, (tuple, list)):
            return unreal.Vector(float(value[0]), float(value[1]), float(value[2]))
        return value if value is not None else default

    def quat(value):
        if isinstance(value, (tuple, list)):
            return unreal.Quat(float(value[0]), float(value[1]),
                               float(value[2]), float(value[3]))
        return value

    try:
        skeleton_obj = skeleton
        if isinstance(skeleton_obj, str):
            skeleton_obj = unreal.EditorAssetLibrary.load_asset(skeleton_obj)
        if not skeleton_obj:
            return Result(False, "NativeUE", error="Missing target skeleton asset")
        mesh_obj = preview_mesh
        if isinstance(mesh_obj, str):
            mesh_obj = unreal.EditorAssetLibrary.load_asset(mesh_obj)

        lengths = [len(data.get("translations", [])) for data in tracks.values()]
        lengths += [len(data.get("rotations", [])) for data in tracks.values()]
        lengths += [len(data.get("scales", [])) for data in tracks.values()]
        frame_count = int(num_frames or max(lengths or [1]))
        identity_t = unreal.Vector(0.0, 0.0, 0.0)
        identity_r = unreal.Quat(0.0, 0.0, 0.0, 1.0)
        identity_s = unreal.Vector(1.0, 1.0, 1.0)
        ue_tracks = []
        for bone_name, data in tracks.items():
            track = unreal.LightUSDUEBoneTrack()
            track.bone_name = str(bone_name)
            translations = [vec(v, identity_t) for v in data.get("translations", [])]
            rotations = [quat(v) for v in data.get("rotations", [])]
            scales = [vec(v, identity_s) for v in data.get("scales", [])]
            count = max(len(translations), len(rotations), len(scales), frame_count)
            translations = translations or [identity_t] * count
            rotations = rotations or [identity_r] * count
            scales = scales or [identity_s] * count
            track.translations = (translations + [translations[-1]] * count)[:count]
            track.rotations = (rotations + [rotations[-1]] * count)[:count]
            track.scales = (scales + [scales[-1]] * count)[:count]
            ue_tracks.append(track)
        value = unreal.LightUSDUEBlueprintLibrary.create_skeletal_animation(
            name, skeleton_obj, ue_tracks, package_path, int(frame_rate),
            frame_count, mesh_obj)
        return Result.from_ue(value)
    except Exception as exc:
        return Result(False, "NativeUE", error=f"UE AnimSequence authoring failed: {exc}")


def import_skeletal_animation(filename: str, name: str, skeleton, *,
                              package_path="/Game/LightUSD/Animations",
                              preview_mesh=None) -> Result:
    """Convert a real USD SkelAnimation layer into a persistent UE asset."""
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    skeleton_obj = skeleton
    if isinstance(skeleton_obj, str):
        skeleton_obj = unreal.EditorAssetLibrary.load_asset(skeleton_obj)
    mesh_obj = preview_mesh
    if isinstance(mesh_obj, str):
        mesh_obj = unreal.EditorAssetLibrary.load_asset(mesh_obj)
    if not skeleton_obj:
        return Result(False, "LightUSD", error="Missing target skeleton asset")
    value = unreal.LightUSDUEBlueprintLibrary.import_skeletal_animation(
        filename, name, skeleton_obj, package_path, mesh_obj)
    return Result.from_ue(value)


def add_skeletal_animation_curves(animation, curves: dict[str, dict]) -> Result:
    """Add facial/morph curves to a saved UE AnimSequence."""
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    if isinstance(animation, str):
        animation = unreal.EditorAssetLibrary.load_asset(animation)
    if not animation:
        return Result(False, "NativeUE", error="Missing target AnimSequence asset")
    ue_curves = []
    for name, data in curves.items():
        curve = unreal.LightUSDUEFloatCurve()
        curve.curve_name = str(name)
        curve.times = [float(value) for value in data.get("times", [])]
        curve.values = [float(value) for value in data.get("values", [])]
        ue_curves.append(curve)
    return Result.from_ue(
        unreal.LightUSDUEBlueprintLibrary.add_skeletal_animation_curves(
            animation, ue_curves))


def compose_skeletal_animation(mesh_usd: str, animation_usd: str, filename: str,
                               *, skeleton_path="/TutorialTPP/Skel",
                               animation_path="/Tutorial_Walk_Fwd") -> str:
    """Compose a UE skeleton layer with a standalone SkelAnimation layer.

    UE's USD importer creates persistent AnimSequence assets only when the
    animation resolves through a Skeleton query. UE's AnimSequence exporter
    intentionally writes a standalone animation layer, so this small wrapper
    adds the standard Skeleton ``skel:animationSource`` relationship.
    """
    mesh_path = Path(mesh_usd).resolve().as_posix()
    anim_path = Path(animation_usd).resolve().as_posix()
    skeleton_parent, skeleton_name = skeleton_path.rsplit("/", 1)
    root_name = skeleton_parent.strip("/").split("/", 1)[0]
    text = "#usda 1.0\n(\n"
    text += f"    subLayers = [@{mesh_path}@, @{anim_path}@]\n)\n\n"
    text += f"over \"{root_name}\" {{\n"
    if skeleton_parent.strip("/").count("/") == 1:
        text += f"    over \"{skeleton_name}\" {{\n"
    else:
        text += f"    over \"{skeleton_name}\" {{\n"
    text += f"        rel skel:animationSource = <{animation_path}>\n"
    text += "    }\n}\n"
    Path(filename).write_text(text, encoding="utf-8")
    return str(Path(filename).resolve())


def validate_usd(filename: str, *, backend="lightusd", **kwargs) -> Result:
    if backend == "lightusd" and os.environ.get("LIGHTUSD_UE_PYTHON_ONLY", "0") == "1":
        return _python_import_usd(filename, **kwargs)
    return _call("validate_usd", filename, backend, **kwargs)


def export_material(material, filename: str, **kwargs) -> Result:
    from .material import export_material as _export_material
    return _export_material(material, filename, **kwargs)


def import_material(filename: str, package_path: str, **kwargs) -> Result:
    from .material import import_material as _import_material
    return _import_material(filename, package_path, **kwargs)


def capabilities() -> dict[str, Any]:
    if unreal is None:
        return {"unreal": False, "backends": ["auto", "native", "lightusd"]}
    return {"unreal": True,
            "native_usd": unreal.LightUSDUEBlueprintLibrary.is_native_backend_available(),
            "backends": ["auto", "native", "lightusd"],
            "materialx": True, "physics": True, "usdskel": True, "groom": True}


def _native_export(filename: str, **kwargs) -> Result:
    """Export the current editor world through UE's USD exporter.

    UE's level exporter is intentionally driven through its public Python
    exporter task. This keeps exporter option wiring compatible across UE
    5.6-5.8 and lets Epic's native MaterialX/UsdSkel paths do the work.
    """
    if unreal is None:
        raise RuntimeError("lightusd_ue must run inside Unreal Editor")
    try:
        from usd_unreal import level_exporter

        try:
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        except AttributeError:
            world = unreal.EditorLevelLibrary.get_editor_world()
        if not world:
            return Result(False, "NativeUE", error="No editor world is available")

        options = unreal.LevelExporterUSDOptions()
        inner = options.get_editor_property("inner")
        if "selection_only" in kwargs:
            # UE 5.8 exposes this reflected property as `selection_only`;
            # older UE USD exporter bindings used the C++-style b_ prefix.
            property_name = "selection_only"
            try:
                inner.set_editor_property(property_name, bool(kwargs["selection_only"]))
            except Exception:
                inner.set_editor_property("b_selection_only", bool(kwargs["selection_only"]))
        if "root_prim_name" in kwargs:
            inner.set_editor_property("root_prim_name", str(kwargs["root_prim_name"]))
        options.set_editor_property("inner", inner)
        if "start_time_code" in kwargs:
            options.set_editor_property("start_time_code", float(kwargs["start_time_code"]))
        if "end_time_code" in kwargs:
            options.set_editor_property("end_time_code", float(kwargs["end_time_code"]))

        task = unreal.AssetExportTask()
        task.object = world
        task.filename = str(Path(filename).resolve())
        task.selected = False
        task.replace_identical = True
        task.prompt = False
        task.automated = True
        task.options = options
        if not unreal.Exporter.run_asset_export_task(task):
            return Result(False, "NativeUE", error="UE USD exporter rejected the export task")
        if not Path(task.filename).exists():
            return Result(False, "NativeUE", error="UE USD exporter produced no root layer")
        return Result(True, "NativeUE", root_layer=task.filename,
                      created_assets=[task.filename])
    except Exception as exc:
        return Result(False, "NativeUE", error=f"Native UE USD export failed: {exc}")
