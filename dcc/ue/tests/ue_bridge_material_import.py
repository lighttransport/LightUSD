"""Download a material USD through the bridge and import it in UE."""

import json
import os

import unreal

import lightusd_ue


def main():
    url = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
    token = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
    filename = os.environ.get(
        "LIGHTUSD_BRIDGE_INPUT", "D:/work/lightusd/UBTFullTest/BridgeMaterial/input.usda")
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    asset_ids = os.environ.get("LIGHTUSD_ASSET_IDS", os.environ.get("LIGHTUSD_ASSET_ID", ""))
    names = os.environ.get("LIGHTUSD_ASSET_NAMES", os.path.basename(filename))
    downloaded = []
    for asset_id, name in zip(asset_ids.split(";"), names.split(";")):
        target = os.path.join(os.path.dirname(filename), name)
        downloaded.append(lightusd_ue.download_file(url, asset_id, target, token))
    if not downloaded:
        raise RuntimeError("No bridge assets were requested")
    result = lightusd_ue.import_usd(
        filename, backend="native", import_materials=True, import_groom=False,
        import_physics=False, import_skeletal_animation=False,
        package_path="/Game/LightUSD/BridgeMaterial")
    if not result.succeeded:
        raise RuntimeError(result.error or "Bridge material import failed")
    saved_assets = []
    for path in result.created_assets:
        if not path.startswith("/Game/"):
            continue
        if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
            saved_assets.append(path)
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/BridgeMaterial/report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"downloaded": downloaded, "created_assets": result.created_assets,
                   "saved_assets": saved_assets,
                   "warnings": result.warnings}, stream, indent=2)
    unreal.log(f"Bridge material import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
