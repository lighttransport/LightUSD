"""Download a groom USD through the asset bridge and import it in UE."""

import json
import os

import unreal

import lightusd_ue


def main():
    url = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
    token = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
    asset_id = os.environ["LIGHTUSD_ASSET_ID"]
    filename = os.environ.get(
        "LIGHTUSD_BRIDGE_INPUT", "D:/work/lightusd/UBTFullTest/BridgeGroom/input.usda")
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    downloaded = lightusd_ue.download_file(url, asset_id, filename, token)
    result = lightusd_ue.import_usd(
        filename, backend="lightusd", import_groom=True, import_physics=False,
        package_path="/Game/LightUSD/BridgeGroom",
        groom_target_skeletal_mesh_path="/MetaHumanCharacter/Face/SKM_Face")
    if not result.succeeded:
        raise RuntimeError(result.error or "Bridge groom import failed")
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/BridgeGroom/report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"downloaded": downloaded, "created_assets": result.created_assets,
                   "warnings": result.warnings}, stream, indent=2)
    unreal.log(f"Bridge groom import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
