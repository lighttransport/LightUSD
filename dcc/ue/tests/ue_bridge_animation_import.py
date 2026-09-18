"""Download a composed skeletal USD through the bridge and import it in UE."""

import json
import os

import unreal

import lightusd_ue


def main():
    url = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
    token = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
    asset_id = os.environ["LIGHTUSD_ASSET_ID"]
    filename = os.environ.get(
        "LIGHTUSD_BRIDGE_INPUT", "D:/work/lightusd/UBTFullTest/BridgeAnimation/input.usda")
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    downloaded = lightusd_ue.download_file(url, asset_id, filename, token)
    result = lightusd_ue.import_usd(
        filename, backend="native", import_materials=True, import_physics=True,
        import_groom=False, import_skeletal_animation=True,
        package_path="/Game/LightUSD/BridgeAnimation")
    if not result.succeeded:
        raise RuntimeError(result.error or "Bridge animation import failed")
    skeleton = next((path for path in result.created_assets
                     if "/SKEL_" in path and ".SKEL_" in path), "")
    explicit_animation = None
    if skeleton:
        explicit_animation = lightusd_ue.import_skeletal_animation(
            filename, "AN_BridgeAnimation", skeleton,
            package_path="/Game/LightUSD/BridgeAnimation/Animation")
        if not explicit_animation.succeeded:
            raise RuntimeError(explicit_animation.error or
                               "Explicit bridge animation import failed")
    saved_assets = []
    for path in result.created_assets:
        if path.startswith("/Game/") and unreal.EditorAssetLibrary.save_asset(
                path, only_if_is_dirty=False):
            saved_assets.append(path)
    if explicit_animation:
        for path in explicit_animation.created_assets:
            if path.startswith("/Game/") and unreal.EditorAssetLibrary.save_asset(
                    path, only_if_is_dirty=False):
                saved_assets.append(path)
    animation_export = None
    animation_validation = None
    if explicit_animation and explicit_animation.created_assets:
        animation_asset = unreal.EditorAssetLibrary.load_asset(
            explicit_animation.created_assets[0])
        if not animation_asset:
            raise RuntimeError("Saved bridge AnimSequence could not be reloaded")
        export_file = os.environ.get(
            "LIGHTUSD_BRIDGE_ANIMATION_EXPORT",
            "D:/work/lightusd/UBTFullTest/BridgeAnimation/animation_export.usda")
        animation_export = lightusd_ue.export_skeletal_animation(
            animation_asset, export_file)
        if not animation_export.succeeded:
            raise RuntimeError(animation_export.error or
                               "Bridge AnimSequence export failed")
        animation_validation = lightusd_ue.validate_usd(export_file)
        if not animation_validation.succeeded:
            raise RuntimeError(animation_validation.error or
                               "Bridge animation export validation failed")
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/BridgeAnimation/report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"downloaded": downloaded, "created_assets": result.created_assets,
                   "explicit_animation_assets": (
                       explicit_animation.created_assets if explicit_animation else []),
                   "animation_export": (animation_export.created_assets
                                        if animation_export else []),
                   "animation_validation_prim_count": (
                       len(animation_validation.prim_paths)
                       if animation_validation else 0),
                   "saved_assets": saved_assets, "warnings": result.warnings},
                  stream, indent=2)
    unreal.log(f"Bridge animation import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
