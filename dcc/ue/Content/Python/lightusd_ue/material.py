"""MaterialX-first Unreal material graph export/import.

The native UE USD exporter intentionally exports a material reference only. This
module provides the editor-side graph bridge used by LightUSDUE. The USD graph
is ordinary UsdShade/MaterialX, with a versioned UE preservation layer on the
Material and expression Shader prims.
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from typing import Any

import unreal

from .api import Result


def _q(value: Any) -> str:
    return json.dumps(str(value), ensure_ascii=False)


def _asset(value: Any) -> str:
    """Serialize a USD asset path while retaining UDIM tokens verbatim."""
    path = _path(value)
    if "@" in path:
        path = path.replace("@", "%40")
    return "@" + path + "@"


def _name(value: Any, fallback: str = "Material") -> str:
    text = str(value)
    text = re.sub(r"[^A-Za-z0-9_]", "_", text)
    if not text:
        return fallback
    if text[0].isdigit():
        text = "_" + text
    return text


def _path(value: Any) -> str:
    if value is None:
        return ""
    for method in ("get_path_name", "get_name"):
        try:
            return str(getattr(value, method)())
        except Exception:
            pass
    return str(value)


def _jsonable(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if hasattr(value, "r") and hasattr(value, "g") and hasattr(value, "b"):
        result = {"r": float(value.r), "g": float(value.g), "b": float(value.b)}
        if hasattr(value, "a"):
            result["a"] = float(value.a)
        return result
    if hasattr(value, "x") and hasattr(value, "y"):
        result = {"x": float(value.x), "y": float(value.y)}
        if hasattr(value, "z"):
            result["z"] = float(value.z)
        if hasattr(value, "w"):
            result["w"] = float(value.w)
        return result
    return _path(value)


def _get(obj: Any, *names: str, default: Any = None) -> Any:
    for name in names:
        try:
            return obj.get_editor_property(name)
        except Exception:
            try:
                return getattr(obj, name)
            except Exception:
                pass
    return default


def _expression_archive(expr: Any) -> str:
    # These cover the common UE material-expression families. The class path,
    # graph links, GUID and layout are stored separately so unknown expression
    # classes still have a deterministic reconstruction envelope.
    names = (
        "r", "g", "b", "a", "constant", "default_value", "default_scalar_value",
        "default_vector_value", "parameter_name", "group", "sort_priority",
        "texture", "texture_object", "sampler_type", "sampler_source", "coordinates",
        "coordinate_index", "desc", "const_a", "const_b", "const_c", "const_value",
        "font", "font_texture", "material_function", "function", "function_output_index",
        "channel_mask", "red", "green", "blue", "alpha", "use_alpha",
        "a_const", "b_const", "default_a", "default_b", "static_switch_parameter",
        "normal_map_texture", "height_map_texture", "virtual_texture", "mip_value_mode",
        "texture_address_mode", "blend_type", "quality", "feature_level",
        "material_expression_editor_x", "material_expression_editor_y",
        "code", "description", "output_name", "input_name",
    )
    props = {}
    for name in names:
        value = _get(expr, name, default=None)
        if value is not None:
            props[name] = _jsonable(value)
    return json.dumps({"format": "LightUSD.MaterialUE.1", "properties": props},
                      sort_keys=True, separators=(",", ":"))


def _material_state(material: Any) -> dict[str, Any]:
    state = {}
    for name in (
        "material_domain", "blend_mode", "two_sided", "opacity_mask_clip_value",
        "decal_response", "translucency_lighting_mode", "translucency_pass",
        "use_material_attributes", "subsurface_profile",
        "shading_model", "shading_models", "d3d11_tessellation_mode",
        "allow_negative_emissive_color", "cast_dynamic_shadow_as_masked",
        "num_customized_u_vs", "is_blendable",
    ):
        value = _get(material, name, default=None)
        if value is not None:
            state[name] = _jsonable(value)
    return state


def _node_input(expr: Any, pin_name: str) -> Any:
    # UE exposes the display labels returned by
    # get_material_expression_input_names(), while get_editor_property()
    # expects the reflected member name. Keep this small normalization layer
    # here so the graph archive remains useful for both old and new UE nodes.
    candidates = [pin_name]
    normalized = re.sub(r"[^A-Za-z0-9]", "", pin_name).lower()
    aliases = {
        "uvs": "coordinates",
        "tex": "texture",
        "applyviewmipbias": "automatic_view_mip_bias",
        "alpha": "alpha",
        "a": "a",
        "b": "b",
    }
    if normalized in aliases:
        candidates.append(aliases[normalized])
    candidates.append(re.sub(r"[^A-Za-z0-9_]", "_", pin_name).lower())
    for candidate in candidates:
        try:
            pin = expr.get_editor_property(candidate)
            if pin is None:
                continue
            source = _get(pin, "expression", default=None)
            return source if source is not None else pin
        except Exception:
            continue
    return None


def _node_output_name(material: Any, source: Any, target: Any) -> str:
    try:
        return str(unreal.MaterialEditingLibrary.get_input_node_output_name_for_material_expression(
            target, source))
    except Exception:
        try:
            names = unreal.MaterialEditingLibrary.get_material_expression_output_names(source)
            return str(names[0]) if names else "out"
        except Exception:
            return "out"


def _material_output_node(material: Any, prop_name: str) -> tuple[Any, str] | tuple[None, None]:
    try:
        prop = getattr(unreal.MaterialProperty, prop_name)
        node = unreal.MaterialEditingLibrary.get_material_property_input_node(material, prop)
        if node is not None:
            return node, unreal.MaterialEditingLibrary.get_material_property_input_node_output_name(
                material, prop)
    except Exception:
        pass
    return None, None


def _materialx_node_id(expr: Any) -> str | None:
    """Return a portable MaterialX node id for common UE expressions."""
    name = expr.get_class().get_name()
    return {
        "MaterialExpressionConstant": "ND_constant_float",
        "MaterialExpressionConstant3Vector": "ND_constant_color3",
        "MaterialExpressionTextureSample": "ND_image_color3",
        "MaterialExpressionTextureSampleParameter2D": "ND_image_color3",
        "MaterialExpressionTextureSampleParameterCube": "ND_image_color3",
        "MaterialExpressionScalarParameter": "ND_constant_float",
        "MaterialExpressionVectorParameter": "ND_constant_color3",
        "MaterialExpressionStaticBoolParameter": "ND_constant_boolean",
        "MaterialExpressionStaticSwitchParameter": "ND_ifgreater",
        "MaterialExpressionTextureCoordinate": "ND_texcoord_vector2",
        "MaterialExpressionLinearInterpolate": "ND_mix_color3",
        "MaterialExpressionMultiply": "ND_multiply_float",
        "MaterialExpressionAdd": "ND_add_float",
        "MaterialExpressionSubtract": "ND_subtract_float",
        "MaterialExpressionDivide": "ND_divide_float",
        "MaterialExpressionComponentMask": "ND_extract",
        "MaterialExpressionClamp": "ND_clamp",
        "MaterialExpressionPower": "ND_power",
        "MaterialExpressionOneMinus": "ND_invert_float",
        "MaterialExpressionAbs": "ND_absval_float",
        "MaterialExpressionSine": "ND_sin_float",
        "MaterialExpressionCosine": "ND_cos_float",
        "MaterialExpressionSquareRoot": "ND_sqrt_float",
        "MaterialExpressionNormalize": "ND_normalize_vector3",
        "MaterialExpressionDotProduct": "ND_dotproduct_vector3",
        "MaterialExpressionCrossProduct": "ND_crossproduct_vector3",
        "MaterialExpressionAppendVector": "ND_combine2_vector2",
        "MaterialExpressionFresnel": "ND_fresnel",
        "MaterialExpressionDesaturation": "ND_luminance_color3",
        "MaterialExpressionNormalFromHeightmap": "ND_normalmap",
        "MaterialExpressionRuntimeVirtualTextureSample": "ND_image_color3",
    }.get(name)


def _materialx_input_name(pin_name: str) -> str:
    return {
        "A": "in1",
        "B": "in2",
        "Alpha": "mix",
        "UVs": "texcoord",
        "Tex": "file",
        "Value": "in",
        "Input": "in",
        "True": "in1",
        "False": "in2",
    }.get(pin_name, re.sub(r"[^A-Za-z0-9_]", "_", pin_name).lower())


def export_material(material: Any, filename: str, *, preserve_ue_config: bool = True,
                    prefer_materialx: bool = True) -> Result:
    if isinstance(material, str):
        material = unreal.EditorAssetLibrary.load_asset(material)
    if not material:
        return Result(False, "LIGHTUSD", error="Material asset is not available")

    try:
        expressions = list(unreal.MaterialEditingLibrary.get_material_expressions(material))
    except Exception as exc:
        return Result(False, "LIGHTUSD", error=f"Unable to enumerate material expressions: {exc}")

    root = _name(material.get_name())
    # Unreal Python may return a fresh wrapper object for the same UObject on
    # each query, so Python id() is not stable. UObject path names are stable
    # for the lifetime of the material and also make useful diagnostics.
    ids = {_path(expr): f"Expr_{index:04d}_{_name(expr.get_name(), 'Node')}"
           for index, expr in enumerate(expressions)}
    mtlx_ids = {}
    if prefer_materialx:
        for expr in expressions:
            if _materialx_node_id(expr):
                mtlx_ids[_path(expr)] = f"Mtlx_{ids[_path(expr)]}"
    lines = ["#usda 1.0", "", f'def Material "{root}" (']
    schemas = []
    if prefer_materialx:
        schemas.append("MaterialXConfigAPI")
    if preserve_ue_config:
        schemas.append("MaterialUEConfigAPI")
    lines.append(f"    prepend apiSchemas = [{', '.join(_q(s) for s in schemas)}]")
    lines.append(") {")
    state = _material_state(material)
    lines.extend([
        '    string config:unreal:version = "1"',
        '    string config:unreal:engineVersion = "5.6+"',
        f"    string config:unreal:sourceAsset = {_q(_path(material))}",
        '    string config:unreal:graphAuthority = "materialx+ue"',
        f"    string config:unreal:propertyArchive = {_q(json.dumps(state, sort_keys=True))}",
    ])
    for key, usd_name in (("material_domain", "materialDomain"),
                          ("blend_mode", "blendMode")):
        if key in state:
            lines.append(f"    string config:unreal:{usd_name} = {_q(state[key])}")
    if "two_sided" in state:
        lines.append(f"    bool config:unreal:twoSided = {'true' if state['two_sided'] else 'false'}")
    if "opacity_mask_clip_value" in state:
        lines.append(f"    float config:unreal:opacityMaskClipValue = {float(state['opacity_mask_clip_value'])}")
    if prefer_materialx:
        lines.append('    string config:mtlx:version = "1.39"')
        lines.append('    string config:mtlx:colorspace = "lin_rec709"')
    lines.append(f'    token outputs:surface.connect = </{root}/OpenPBRSurface.outputs:surface>')
    lines.append('    def Shader "OpenPBRSurface" {')
    lines.append('        uniform token info:id = "OpenPBRSurface"')
    lines.append('        token outputs:surface')
    output_map = {
        "BaseColor": ("MP_BASE_COLOR", "base_color", "color3f"),
        "Metallic": ("MP_METALLIC", "base_metalness", "float"),
        "Roughness": ("MP_ROUGHNESS", "base_roughness", "float"),
        "Specular": ("MP_SPECULAR", "specular_weight", "float"),
        "EmissiveColor": ("MP_EMISSIVE_COLOR", "emission_color", "color3f"),
        "Opacity": ("MP_OPACITY", "geometry_opacity", "float"),
        "Normal": ("MP_NORMAL", "geometry_normal", "normal3f"),
        "AmbientOcclusion": ("MP_AMBIENT_OCCLUSION", "occlusion", "float"),
        "WorldPositionOffset": ("MP_WORLD_POSITION_OFFSET", "displacement", "vector3f"),
        "PixelDepthOffset": ("MP_PIXEL_DEPTH_OFFSET", "depth_offset", "float"),
        "Refraction": ("MP_REFRACTION", "refraction", "float"),
        "SubsurfaceColor": ("MP_SUBSURFACE_COLOR", "subsurface_color", "color3f"),
        "ClearCoat": ("MP_CLEAR_COAT", "coat_weight", "float"),
        "ClearCoatRoughness": ("MP_CLEAR_COAT_ROUGHNESS", "coat_roughness", "float"),
        "ClearCoatNormal": ("MP_CLEAR_COAT_NORMAL", "coat_normal", "normal3f"),
        "Anisotropy": ("MP_ANISOTROPY", "anisotropy", "float"),
        "Tangent": ("MP_TANGENT", "tangent", "vector3f"),
    }
    for prop_name, (ue_prop_name, mtlx_name, usd_type) in output_map.items():
        node, output = _material_output_node(material, ue_prop_name)
        if node is not None and _path(node) in ids:
            graph = "MaterialXGraph" if _path(node) in mtlx_ids else "UEGraph"
            node_name = mtlx_ids.get(_path(node), ids[_path(node)])
            lines.append(f"        {usd_type} inputs:{mtlx_name}.connect = </{root}/{graph}/{node_name}.outputs:{_name(output, 'out')}>")
    lines.append("    }")
    if prefer_materialx:
        lines.append('    def NodeGraph "MaterialXGraph" {')
        for expr in expressions:
            mtlx_id = mtlx_ids.get(_path(expr))
            if not mtlx_id:
                continue
            mtlx_node = _materialx_node_id(expr)
            lines.extend([
                f'        def Shader "{mtlx_id}" {{',
                f'            uniform token info:id = "{mtlx_node}"',
                '            token outputs:out',
            ])
            class_name = expr.get_class().get_name()
            if class_name in ("MaterialExpressionConstant", "MaterialExpressionScalarParameter"):
                value = _get(expr, "r", "default_value", "default_scalar_value", default=0.0)
                value = float(value or 0.0)
                lines.append(f"            float inputs:value = {value}")
            elif class_name in ("MaterialExpressionConstant3Vector", "MaterialExpressionVectorParameter"):
                value = _get(expr, "constant", "default_value", "default_vector_value", default=None)
                if value is not None and all(hasattr(value, c) for c in ("r", "g", "b")):
                    lines.append(f"            color3f inputs:value = ({float(value.r)}, {float(value.g)}, {float(value.b)})")
            elif class_name == "MaterialExpressionStaticBoolParameter":
                value = bool(_get(expr, "default_value", default=False))
                lines.append(f"            boolean inputs:value = {'true' if value else 'false'}")
            elif (class_name.startswith("MaterialExpressionTextureSample") or
                  class_name == "MaterialExpressionRuntimeVirtualTextureSample"):
                texture = _get(expr, "texture", "virtual_texture", default=None)
                if texture:
                    asset_path = _path(texture)
                    # Keep the standard MaterialX/USD representation in
                    # addition to the UE preservation field.  In particular,
                    # a `<UDIM>` token must remain authored as a token rather
                    # than being resolved to tile 1001 during export.
                    lines.append(f'            asset inputs:file = {_asset(asset_path)}')
                    lines.append(f'            string unreal:assetPath = {_q(asset_path)}')
                    lines.append('            token inputs:colorspace = "srgb_texture"')
            try:
                input_names = list(unreal.MaterialEditingLibrary.get_material_expression_input_names(expr))
                connected_inputs = list(
                    unreal.MaterialEditingLibrary.get_inputs_for_material_expression(
                        material, expr))
            except Exception:
                input_names, connected_inputs = [], []
            for index, pin_name in enumerate(input_names):
                source = connected_inputs[index] if index < len(connected_inputs) else None
                if source is None:
                    source = _node_input(expr, str(pin_name))
                if source is None or _path(source) not in mtlx_ids:
                    continue
                input_name = _materialx_input_name(str(pin_name))
                lines.append(
                    f'            token inputs:{input_name}.connect = '
                    f'</{root}/MaterialXGraph/{mtlx_ids[_path(source)]}.outputs:out>')
            lines.append('        }')
        lines.append('    }')
    lines.append('    def NodeGraph "UEGraph" {')
    for expr in expressions:
        node = ids[_path(expr)]
        class_path = _path(expr.get_class())
        guid = _path(_get(expr, "material_expression_guid", default=""))
        x = int(_get(expr, "material_expression_editor_x", default=0) or 0)
        y = int(_get(expr, "material_expression_editor_y", default=0) or 0)
        lines.extend([
            f'        def Shader "{node}" {{',
            f'            uniform token info:id = "UnrealMaterialExpression.{expr.get_class().get_name()}"',
            f'            string unreal:classPath = {_q(class_path)}',
            f'            string unreal:guid = {_q(guid)}',
            f'            int unreal:editorX = {x}',
            f'            int unreal:editorY = {y}',
            f'            string unreal:propertyArchive = {_q(_expression_archive(expr))}',
            '            token outputs:out',
        ])
        try:
            input_names = list(unreal.MaterialEditingLibrary.get_material_expression_input_names(expr))
        except Exception:
            input_names = []
        try:
            connected_inputs = list(
                unreal.MaterialEditingLibrary.get_inputs_for_material_expression(
                    material, expr))
        except Exception:
            connected_inputs = []
        for index, pin_name in enumerate(input_names):
            # get_inputs_for_material_expression() is the authoritative API
            # for protected FExpressionInput members (TextureSample UVs is a
            # notable example). Fall back to reflected properties for nodes
            # where the older UE Python binding does not expose that helper.
            source = (connected_inputs[index]
                      if index < len(connected_inputs) else None)
            if source is None:
                source = _node_input(expr, str(pin_name))
            if source is None or _path(source) not in ids:
                continue
            output = _node_output_name(material, source, expr)
            lines.append(f'            token inputs:{_name(pin_name)}.connect = </{root}/UEGraph/{ids[_path(source)]}.outputs:{_name(output, "out")}>')
        lines.append("        }")
    lines.extend(["    }", "}", ""])
    Path(filename).write_text("\n".join(lines), encoding="utf-8")
    return Result(True, "LIGHTUSD", created_assets=[str(filename)],
                  prim_paths=[f"/{root}", f"/{root}/OpenPBRSurface", f"/{root}/UEGraph"],
                  root_layer=str(filename))


def _block(text: str, kind: str, name: str) -> str:
    match = re.search(rf'def {kind} "{re.escape(name)}"\s*\{{', text)
    if not match:
        return ""
    start = match.end()
    depth = 1
    quoted = False
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
    return ""


def _shader_blocks(text: str) -> list[tuple[str, str]]:
    blocks = []
    for match in re.finditer(r'def Shader "([A-Za-z0-9_]+)"\s*\{', text):
        body = _block(text[match.start():], "Shader", match.group(1))
        if body:
            blocks.append((match.group(1), body))
    return blocks


def _quoted(body: str, key: str) -> str:
    match = re.search(rf'\b{re.escape(key)}\s*=\s*"((?:\\.|[^"\\])*)"', body)
    return json.loads('"' + match.group(1) + '"') if match else ""


def _enum_from_archive(value: Any) -> Any:
    if not isinstance(value, str):
        return value
    match = re.search(r"<([A-Za-z0-9_]+)\.([A-Za-z0-9_]+):", value)
    if not match:
        return value
    enum_type = getattr(unreal, match.group(1), None)
    return getattr(enum_type, match.group(2), value) if enum_type else value


def _restore_material_state(material: Any, text: str) -> None:
    archive = _quoted(text, "string config:unreal:propertyArchive")
    if not archive:
        return
    try:
        state = json.loads(archive)
    except (TypeError, ValueError):
        return
    for name, value in state.items():
        try:
            material.set_editor_property(name, _enum_from_archive(value))
        except Exception:
            # Some UE versions expose material settings as read-only or under
            # a version-specific name. The UE archive remains available for
            # a later version-specific restore pass.
            pass


def import_material(filename: str, package_path: str, *, material_name: str | None = None) -> Result:
    text = Path(filename).read_text(encoding="utf-8")
    material_match = re.search(r'def Material "([A-Za-z_][A-Za-z0-9_]*)"', text)
    if not material_match:
        return Result(False, "LIGHTUSD", error="USD file contains no Material prim")
    source_name = material_match.group(1)
    material_name = material_name or source_name
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    unreal.EditorAssetLibrary.make_directory(package_path)
    material = tools.create_asset(material_name, package_path, unreal.Material,
                                  unreal.MaterialFactoryNew())
    if not material:
        return Result(False, "NATIVE_UE", error="Unable to create UE Material asset")
    _restore_material_state(material, text)

    graph = _block(text, "NodeGraph", "UEGraph")
    nodes = {}
    node_matches = _shader_blocks(graph)
    for match in node_matches:
        name, body = match
        class_path = _quoted(body, "string unreal:classPath")
        x_match = re.search(r"int unreal:editorX\s*=\s*(-?\d+)", body)
        y_match = re.search(r"int unreal:editorY\s*=\s*(-?\d+)", body)
        try:
            cls = unreal.load_class(None, class_path)
            expr = unreal.MaterialEditingLibrary.create_material_expression(
                material, cls, int(x_match.group(1)) if x_match else 0,
                int(y_match.group(1)) if y_match else 0)
            archive = _quoted(body, "string unreal:propertyArchive")
            props = json.loads(archive).get("properties", {}) if archive else {}
            for prop_name, value in props.items():
                if prop_name in ("texture", "texture_object") and isinstance(value, str):
                    value = unreal.load_object(None, value)
                if isinstance(value, (str, int, float, bool)) or (
                        prop_name in ("texture", "texture_object") and value is not None):
                    try:
                        expr.set_editor_property(prop_name, value)
                    except Exception:
                        pass
            nodes[name] = expr
        except Exception as exc:
            unreal.log_warning(f"LightUSD UE material node {name} skipped: {exc}")

    for name, body in node_matches:
        target = nodes.get(name)
        if not target:
            continue
        for input_name, source_name, output_name in re.findall(
                r'token inputs:([A-Za-z0-9_]+)\.connect\s*=\s*</[^>]+/([A-Za-z0-9_]+)\.outputs:([A-Za-z0-9_]+)>', body):
            source = nodes.get(source_name)
            if source:
                # UE represents the primary output with an empty label;
                # LightUSD uses the explicit portable name `out` in USD.
                source_output = "" if output_name == "out" else output_name
                unreal.MaterialEditingLibrary.connect_material_expressions(
                    source, source_output, target, input_name)

    # Reconnect the canonical OpenPBR inputs to the new material as well as
    # the preserved UE graph. MaterialX nodes use a Mtlx_ prefix, but map back
    # to the corresponding UE expression created above.
    output_properties = {
        "base_color": "MP_BASE_COLOR",
        "base_metalness": "MP_METALLIC",
        "base_roughness": "MP_ROUGHNESS",
        "specular_weight": "MP_SPECULAR",
        "emission_color": "MP_EMISSIVE_COLOR",
        "geometry_opacity": "MP_OPACITY",
        "geometry_normal": "MP_NORMAL",
        "occlusion": "MP_AMBIENT_OCCLUSION",
        "displacement": "MP_WORLD_POSITION_OFFSET",
        "depth_offset": "MP_PIXEL_DEPTH_OFFSET",
        "refraction": "MP_REFRACTION",
        "subsurface_color": "MP_SUBSURFACE_COLOR",
        "coat_weight": "MP_CLEAR_COAT",
        "coat_roughness": "MP_CLEAR_COAT_ROUGHNESS",
        "coat_normal": "MP_CLEAR_COAT_NORMAL",
        "anisotropy": "MP_ANISOTROPY",
        "tangent": "MP_TANGENT",
    }
    for input_name, source_name, output_name in re.findall(
            r"(?:color3f|float|normal3f) inputs:([A-Za-z0-9_]+)\.connect\s*=\s*</[^>]+/"
            r"([A-Za-z0-9_]+)\.outputs:([A-Za-z0-9_]*)>", text):
        prop_name = output_properties.get(input_name)
        if not prop_name:
            continue
        if source_name.startswith("Mtlx_"):
            source_name = source_name[len("Mtlx_"):]
        source = nodes.get(source_name)
        if source:
            try:
                unreal.MaterialEditingLibrary.connect_material_property(
                    source, "" if output_name == "out" else output_name,
                    getattr(unreal.MaterialProperty, prop_name))
            except Exception as exc:
                unreal.log_warning(
                    f"LightUSD UE material output {input_name} not connected: {exc}")
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return Result(True, "LIGHTUSD", created_assets=[material.get_path_name()],
                  root_layer=str(filename))
