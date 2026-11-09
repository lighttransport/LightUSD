"""Headless MetaHuman template skeletal-mesh USD roundtrip.

This deliberately does not invoke MetaHuman auto-rigging or EOS. It verifies
the local UE template meshes and their skeletal USD export/import path.
"""

import json
import os

import unreal

import lightusd_ue


OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_metahuman_templates")
TEMPLATES = {
    "face": "/MetaHumanCharacter/Face/SKM_Face",
    "body": "/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body",
}


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def export_asset(asset, filename):
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = filename
    task.selected = False
    task.replace_identical = True
    task.prompt = False
    task.automated = True
    task.options = unreal.SkeletalMeshExporterUSDOptions()
    if not unreal.Exporter.run_asset_export_task(task):
        fail(f"UE exporter rejected {asset.get_path_name()}")
    if not os.path.isfile(filename):
        fail(f"UE exporter produced no file: {filename}")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    report = {"eos_used": False, "templates": {}}

    for name, asset_path in TEMPLATES.items():
        asset = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not asset:
            fail(f"Missing MetaHuman template mesh: {asset_path}")

        usd_file = os.path.join(OUT_DIR, f"{name}.usda")
        export_asset(asset, usd_file)
        validation = lightusd_ue.validate_usd(usd_file)
        if not validation.succeeded:
            fail(f"LightUSD validation failed for {name}: {validation.error}")

        imported = lightusd_ue.import_usd(
            usd_file, backend="native", import_physics=True,
            import_skeletal_animation=False, import_groom=False)
        if not imported.succeeded:
            fail(f"Native UE import failed for {name}: {imported.error}")

        report["templates"][name] = {
            "source_asset": asset_path,
            "usd_file": usd_file,
            "validation_prim_count": len(validation.prim_paths),
            "native_import_assets": imported.created_assets,
            "native_import_warnings": imported.warnings,
        }

    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"MetaHuman template roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
