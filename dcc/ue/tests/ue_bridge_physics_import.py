"""Download and validate a physics USD through the asset bridge in UE."""

import json
import os

import unreal

import lightusd_ue


def main():
    url = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
    token = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
    asset_id = os.environ["LIGHTUSD_ASSET_ID"]
    filename = os.environ.get(
        "LIGHTUSD_BRIDGE_INPUT", "D:/work/lightusd/UBTFullTest/BridgePhysics/input.usda")
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    downloaded = lightusd_ue.download_file(url, asset_id, filename, token)
    result = lightusd_ue.import_usd(
        filename, backend="lightusd", import_geometry=True,
        import_materials=False, import_groom=False, import_physics=True)
    if not result.succeeded:
        raise RuntimeError(result.error or "Bridge physics import failed")
    required = ("/PhysicsScene", "/RigidBody", "/Collider",
                "/PhysicsMaterial", "/FixedJoint")
    missing = [path for path in required if path not in result.prim_paths]
    if missing:
        raise RuntimeError("Missing physics prims: " + ", ".join(missing))
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/BridgePhysics/report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"downloaded": downloaded, "prim_paths": result.prim_paths,
                   "created_assets": result.created_assets,
                   "warnings": result.warnings}, stream, indent=2)
    unreal.log(f"Bridge physics import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
