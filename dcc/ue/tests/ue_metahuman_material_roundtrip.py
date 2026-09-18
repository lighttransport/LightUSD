"""Headless MetaHuman body mesh + material/texture/UDIM USD roundtrip."""

import json
import os
import struct
import zlib

import unreal

import lightusd_ue


OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_metahuman_materials")
BODY_TEMPLATE = "/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body"
PACKAGE = "/Game/LightUSD/MetaHumanMaterials"


def write_png(path, rgb):
    """Write a dependency-free 8x8 RGB PNG for the UDIM smoke test."""
    width = height = 8
    row = bytes(rgb) * width
    raw = b"".join(b"\x00" + row for _ in range(height))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    data = (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as stream:
        stream.write(data)


def generate_udim_tiles():
    source_dir = os.environ.get("LIGHTUSD_UE_UDIM_SOURCE_DIR", OUT_DIR)
    os.makedirs(source_dir, exist_ok=True)
    write_png(os.path.join(source_dir, "lightusd_skin.1001.png"), (220, 48, 48))
    write_png(os.path.join(source_dir, "lightusd_skin.1002.png"), (48, 96, 220))


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def import_udim_tiles():
    source_dir = os.environ.get("LIGHTUSD_UE_UDIM_SOURCE_DIR", OUT_DIR)
    tasks = []
    for tile in ("1001", "1002"):
        task = unreal.AssetImportTask()
        task.filename = os.path.join(source_dir, f"lightusd_skin.{tile}.png")
        task.destination_path = f"{PACKAGE}/Textures"
        # UE's image importer strips the dotted UDIM suffix when deriving an
        # asset name. Set an explicit per-tile destination name so 1001 and
        # 1002 cannot collide in the Content Browser.
        task.destination_name = f"T_lightusd_skin_{tile}"
        task.automated = True
        task.replace_existing = True
        task.save = True
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    assets = []
    for task in tasks:
        assets.extend(task.imported_object_paths)
    if not assets:
        fail("UE imported no UDIM texture assets")
    if len(set(assets)) != 2:
        fail(f"UDIM tiles collapsed to duplicate UE assets: {assets}")
    return assets


def create_material(texture_path):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material_path = f"{PACKAGE}/M_LightUSD_UDIM.M_LightUSD_UDIM"
    material = unreal.EditorAssetLibrary.load_asset(material_path)
    if not material:
        material = tools.create_asset(
            asset_name="M_LightUSD_UDIM",
            package_path=PACKAGE,
            asset_class=unreal.Material,
            factory=unreal.new_object(type=unreal.MaterialFactoryNew),
        )
    texture = unreal.EditorAssetLibrary.load_asset(texture_path)
    if not material or not texture:
        fail(f"Unable to create material or load texture: {texture_path}")
    expression = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0)
    expression.set_editor_property("parameter_name", "SkinUDIM")
    expression.set_editor_property("texture", texture)
    unreal.MaterialEditingLibrary.connect_material_property(
        expression, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


def export_asset(asset, filename, options):
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = filename
    task.selected = False
    task.replace_identical = True
    task.prompt = False
    task.automated = True
    task.options = options
    if not unreal.Exporter.run_asset_export_task(task):
        fail(f"UE exporter rejected {asset.get_path_name()}")
    if not os.path.isfile(filename):
        fail(f"UE exporter produced no file: {filename}")


def write_udim_material(filename):
    content = """#usda 1.0

def Material "UDIMMaterial" {
    token outputs:surface.connect = </UDIMMaterial/PreviewSurface.outputs:surface>
    def Shader "PreviewSurface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </UDIMMaterial/UDIMTexture.outputs:rgb>
        float inputs:roughness = 0.5
        token outputs:surface
    }
    def Shader "UDIMTexture" {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @lightusd_skin.<UDIM>.png@
        token inputs:st = "st"
        float outputs:rgb
    }
}
"""
    with open(filename, "w", encoding="utf-8") as stream:
        stream.write(content)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    body = unreal.EditorAssetLibrary.load_asset(BODY_TEMPLATE)
    if not body:
        fail(f"Missing MetaHuman body template: {BODY_TEMPLATE}")

    generate_udim_tiles()
    imported_texture_paths = import_udim_tiles()
    material = create_material(imported_texture_paths[0])

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    # Keep the engine template mesh immutable in unattended mode.
    # Export the skinned mesh and generated material as separate USD assets.
    mesh = body
    texture_dir = os.path.join(OUT_DIR, "Textures")
    os.makedirs(texture_dir, exist_ok=True)

    mesh_options = unreal.SkeletalMeshExporterUSDOptions()
    mesh_options.mesh_asset_options.bake_materials = False
    mesh_options.mesh_asset_options.material_baking_options.textures_dir = (
        unreal.DirectoryPath(texture_dir))
    mesh_file = os.path.join(OUT_DIR, "body_udim.usda")
    export_asset(mesh, mesh_file, mesh_options)


    validation = lightusd_ue.validate_usd(mesh_file)
    if not validation.succeeded:
        fail(f"LightUSD validation failed: {validation.error}")
    imported = lightusd_ue.import_usd(
        mesh_file, backend="native", import_physics=True,
        import_skeletal_animation=False, import_groom=False)
    if not imported.succeeded:
        fail(f"Native UE material/skinned import failed: {imported.error}")

    udim_file = os.path.join(OUT_DIR, "material_udim.usda")
    write_udim_material(udim_file)
    udim_validation = lightusd_ue.validate_usd(udim_file)
    if not udim_validation.succeeded:
        fail(f"LightUSD UDIM material validation failed: {udim_validation.error}")
    udim_imported = lightusd_ue.import_usd(
        udim_file, backend="native", import_physics=False,
        import_skeletal_animation=False, import_groom=False)
    if not udim_imported.succeeded:
        fail(f"Native UE UDIM material import failed: {udim_imported.error}")


    exported_files = []
    for root, _, files in os.walk(OUT_DIR):
        exported_files.extend(os.path.relpath(os.path.join(root, f), OUT_DIR)
                              for f in files)
    report = {
        "eos_used": False,
        "source_mesh": BODY_TEMPLATE,
        "skinned_mesh_asset": mesh.get_path_name(),
        "material_asset": material.get_path_name(),
        "imported_udim_assets": imported_texture_paths,
        "exported_files": sorted(exported_files),
        "udim_token_present": any("<UDIM>" in open(os.path.join(OUT_DIR, f),
                                                  encoding="utf-8", errors="ignore").read()
                                   for f in exported_files if f.endswith((".usd", ".usda"))),
        "lightusd_prim_count": len(validation.prim_paths),
        "udim_material_prim_count": len(udim_validation.prim_paths),
        "native_import_assets": imported.created_assets,
        "udim_native_import_assets": udim_imported.created_assets,
        "native_import_warnings": imported.warnings,
    }
    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"MetaHuman material roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
