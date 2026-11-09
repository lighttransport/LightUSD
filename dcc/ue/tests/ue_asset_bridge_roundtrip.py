"""UE-side asset bridge round-trip for a Blender-authored skeletal USD."""

import json
import os

import unreal

import lightusd_ue


BRIDGE_URL = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
BRIDGE_TOKEN = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
ASSET_ID = os.environ["LIGHTUSD_ASSET_ID"]
ASSET_BUNDLE_ID = os.environ.get("LIGHTUSD_ASSET_BUNDLE_ID", "")
INPUT = os.environ.get("LIGHTUSD_BRIDGE_INPUT", "D:/work/lightusd/bridge/skinned.usda")
OUTPUT = os.environ.get("LIGHTUSD_BRIDGE_OUTPUT", "D:/work/lightusd/bridge/ue_roundtrip.usda")


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def main():
    input_file = INPUT
    os.makedirs(os.path.dirname(INPUT), exist_ok=True)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    if ASSET_BUNDLE_ID:
        bundle_root = os.path.join(os.path.dirname(INPUT), "bundle")
        downloaded = lightusd_ue.download_bundle(
            BRIDGE_URL, ASSET_BUNDLE_ID, bundle_root, BRIDGE_TOKEN)
        candidates = []
        for root, _, files in os.walk(bundle_root):
            candidates.extend(os.path.join(root, item) for item in files
                              if item.lower().endswith((".usd", ".usda", ".usdc")))
        if not candidates:
            fail("Downloaded bridge bundle contains no USD layer")
        input_file = sorted(candidates)[0]
    else:
        downloaded = lightusd_ue.download_file(
            BRIDGE_URL, ASSET_ID, input_file, BRIDGE_TOKEN)
    imported = lightusd_ue.import_usd(
        input_file, backend="native", package_path="/Game/LightUSD/AssetBridge",
        import_physics=True, import_skeletal_animation=False, import_groom=False)
    if not imported.succeeded:
        fail(f"UE bridge import failed: {imported.error}")
    mesh_path = next((path for path in imported.created_assets
                      if "/SK_" in path and ".SK_" in path), "")
    if not mesh_path:
        fail(f"UE bridge import created no skeletal mesh: {imported.created_assets}")
    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    exported = lightusd_ue.export_skeletal_mesh(mesh, OUTPUT)
    if not exported.succeeded:
        fail(f"UE bridge export failed: {exported.error}")
    validation = lightusd_ue.validate_usd(OUTPUT, backend="lightusd")
    if not validation.succeeded:
        fail(f"UE bridge LightUSD validation failed: {validation.error}")
    uploaded = lightusd_ue.upload_file(BRIDGE_URL, OUTPUT, BRIDGE_TOKEN)
    report = {
        "input_asset": downloaded,
        "import_backend": imported.backend,
        "imported_assets": imported.created_assets,
        "mesh_path": mesh_path,
        "output_asset": uploaded,
        "validation_backend": validation.backend,
        "validation_prim_count": len(validation.prim_paths),
    }
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/bridge/report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"UE asset bridge roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
