"""Verify LightUSD groom-card Mesh prims use UE's native geometry handoff."""

import json
import os

import unreal
import lightusd_ue


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
USD_FILE = os.environ.get(
    "LIGHTUSD_GROOM_CARDS_USD", os.path.join(ROOT, "tests/usda/blender-groom-cards.usda"))
OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_groom_cards")
PACKAGE = os.environ.get("LIGHTUSD_UE_TEST_PACKAGE", "/Game/LightUSD/GroomCards")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    result = lightusd_ue.import_usd(
        USD_FILE, backend="lightusd", package_path=PACKAGE,
        import_groom=True, import_physics=False)
    if not result.succeeded:
        raise RuntimeError(result.error)
    static_meshes = []
    for path in result.created_assets:
        if not path.startswith("/"):
            continue
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset and asset.get_class().get_name() == "StaticMesh":
            static_meshes.append(path)
    if not static_meshes:
        raise RuntimeError(f"Native groom-card handoff created no StaticMesh: {result.created_assets}")
    if not any("native USD geometry handoff" in warning for warning in result.warnings):
        raise RuntimeError(f"Groom-card handoff was not reported: {result.warnings}")
    report = {"usd_file": USD_FILE, "static_meshes": static_meshes,
              "created_assets": result.created_assets, "warnings": result.warnings}
    with open(os.path.join(OUT_DIR, "report.json"), "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log("LightUSD groom card regression passed")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
