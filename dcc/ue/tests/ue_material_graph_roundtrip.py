"""Headless LightUSD material graph round-trip regression test.

This is deliberately independent of MetaHuman assets. It exercises the
MaterialX-first graph, the UE preservation graph, canonical output reconnection,
and Texture2D property restoration through the LightUSDUE Python facade.
"""

import json
import os

import unreal

import lightusd_ue


OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_material_graph")
PACKAGE = os.environ.get(
    "LIGHTUSD_UE_TEST_PACKAGE", "/Game/LightUSD/AutomatedMaterialGraph")


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def make_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(
        "M_GraphRoundtrip", PACKAGE, unreal.Material, unreal.MaterialFactoryNew())
    if not material:
        fail("Unable to create material test asset")

    color_a = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -700, 0)
    color_a.set_editor_property("constant", unreal.LinearColor(0.8, 0.1, 0.05, 1.0))
    color_b = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -700, 220)
    color_b.set_editor_property("constant", unreal.LinearColor(0.05, 0.2, 0.9, 1.0))
    alpha = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant, -700, 440)
    alpha.set_editor_property("r", 0.35)
    mix = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -350, 0)
    unreal.MaterialEditingLibrary.connect_material_expressions(color_a, "", mix, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(color_b, "", mix, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(alpha, "", mix, "Alpha")
    unreal.MaterialEditingLibrary.connect_material_property(
        mix, "", unreal.MaterialProperty.MP_BASE_COLOR)

    texture = unreal.load_object(
        None, "/Engine/EngineResources/DefaultTexture.DefaultTexture")
    sample = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -400, 520)
    sample.set_editor_property("texture", texture)
    unreal.MaterialEditingLibrary.connect_material_property(
        sample, "RGB", unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    material = make_material()
    usd_file = os.path.join(OUT_DIR, "material_graph.usda")
    exported = lightusd_ue.export_material(material, usd_file)
    if not exported.succeeded:
        fail(f"Material export failed: {exported.error}")

    text = open(usd_file, encoding="utf-8").read()
    for marker in ("MaterialXGraph", "ND_mix_color3", "MaterialUEConfigAPI"):
        if marker not in text:
            fail(f"Expected material graph marker is missing: {marker}")

    validation = lightusd_ue.validate_usd(usd_file)
    if not validation.succeeded:
        fail(f"Material graph validation failed: {validation.error}")

    imported = lightusd_ue.import_material(usd_file, PACKAGE + "Imported")
    if not imported.succeeded:
        fail(f"Material import failed: {imported.error}")
    imported_material = unreal.EditorAssetLibrary.load_asset(
        PACKAGE + "Imported/M_GraphRoundtrip")
    if not imported_material:
        fail("Imported material asset was not created")

    expressions = unreal.MaterialEditingLibrary.get_material_expressions(imported_material)
    if len(expressions) != 5:
        fail(f"Expected five reconstructed expressions, got {len(expressions)}")
    base_color = unreal.MaterialEditingLibrary.get_material_property_input_node(
        imported_material, unreal.MaterialProperty.MP_BASE_COLOR)
    if not base_color or "LinearInterpolate" not in base_color.get_class().get_name():
        fail("Base Color output was not restored to LinearInterpolate")
    texture_nodes = [e for e in expressions
                     if "TextureSample" in e.get_class().get_name()]
    if not texture_nodes or not texture_nodes[0].get_editor_property("texture"):
        fail("Texture2D property was not restored")

    report = {
        "source_material": material.get_path_name(),
        "imported_material": imported_material.get_path_name(),
        "expression_count": len(expressions),
        "base_color_node": base_color.get_path_name(),
        "lightusd_prim_count": len(validation.prim_paths),
        "export_file": usd_file,
    }
    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"LightUSD material graph roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
