"""UE native-import regression for the repository UsdSkel/blendshape fixture."""

import json
import os

import unreal

import lightusd_ue


ROOT = os.environ.get(
    "LIGHTUSD_REPO_ROOT",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
FIXTURE = os.environ.get(
    "LIGHTUSD_USDSKEL_FIXTURE",
    os.path.join(ROOT, "tests/usda/ue-skinned-blendshape.usda"))
OUT = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_usdskel_fixture")
PACKAGE = os.environ.get("LIGHTUSD_UE_TEST_PACKAGE", "/Game/LightUSD/UsdSkelFixture")


def main():
    os.makedirs(OUT, exist_ok=True)
    validation = lightusd_ue.validate_usd(FIXTURE)
    if not validation.succeeded:
        raise RuntimeError(f"Fixture LightUSD validation failed: {validation.error}")
    result = lightusd_ue.import_usd(
        FIXTURE, backend="native", import_skeletal_animation=True,
        import_groom=False, import_physics=False, package_path=PACKAGE)
    if not result.succeeded:
        raise RuntimeError(f"UE native UsdSkel fixture import failed: {result.error}")
    skeletal_mesh = next((unreal.load_asset(path) for path in result.created_assets
                          if "/SkeletalMeshes/SK_" in path), None)
    skeleton_path = next((path for path in result.created_assets
                          if "/SkeletalMeshes/SKEL_" in path), None)
    morph_target_count = None
    if skeletal_mesh:
        try:
            morph_target_count = len(skeletal_mesh.get_editor_property("morph_targets"))
        except Exception:
            morph_target_count = -1
    if os.path.basename(FIXTURE) == "ue-skinned-blendshape.usda":
        if not skeletal_mesh:
            raise RuntimeError("UE imported the fixture without a SkeletalMesh asset")
        if morph_target_count != 1:
            raise RuntimeError(
                f"UE imported the fixture with {morph_target_count} morph targets; expected 1")
    facial_animation = None
    facial_animation_export = None
    facial_animation_usda = ""
    facial_curve_names = []
    facial_curve_debug = ""
    if skeleton_path and os.path.basename(FIXTURE) == "ue-skinned-blendshape.usda":
        facial_animation = lightusd_ue.import_skeletal_animation(
            FIXTURE, "AN_Character_Facial", skeleton_path,
            package_path=PACKAGE + "/Animation",
            preview_mesh=skeletal_mesh)
        if not facial_animation.succeeded:
            raise RuntimeError(
                f"Facial SkelAnimation import failed: {facial_animation.error}")
        reloaded = unreal.load_asset(facial_animation.created_assets[0])
        facial_animation_usda = os.path.join(OUT, "AN_Character_Facial.usda")
        facial_animation_export = lightusd_ue.export_skeletal_animation(
            reloaded, facial_animation_usda)
        if not facial_animation_export.succeeded:
            raise RuntimeError(
                f"Facial AnimSequence export failed: {facial_animation_export.error}")
        try:
            curves_result = unreal.LightUSDUEBlueprintLibrary.get_skeletal_animation_curves(
                reloaded)
            curves = curves_result[-1] if isinstance(curves_result, tuple) else curves_result
            facial_curve_names = [str(curve.curve_name) for curve in (curves or [])]
            facial_curve_debug = repr(curves_result)
        except Exception as exc:
            facial_curve_debug = f"curve-introspection-error: {exc}"
        exported_text = open(facial_animation_usda, encoding="utf-8").read()
    report = {
        "fixture": FIXTURE,
        "validation_prim_count": len(validation.prim_paths),
        "created_assets": result.created_assets,
        "warnings": result.warnings,
        "skeletal_mesh_loaded": bool(skeletal_mesh),
        "morph_target_count": morph_target_count,
        "facial_animation_succeeded": bool(facial_animation and facial_animation.succeeded),
        "facial_animation_assets": (
            facial_animation.created_assets if facial_animation else []),
        "facial_animation_export_succeeded": bool(
            facial_animation_export and facial_animation_export.succeeded),
        "facial_animation_usda": facial_animation_usda,
        "facial_curve_names": facial_curve_names,
        "facial_curve_debug": facial_curve_debug,
        "facial_blendshape_weights_exported": (
            "blendShapeWeights" in open(facial_animation_usda, encoding="utf-8").read()
            if facial_animation_usda else False),
    }
    if os.environ.get("LIGHTUSD_INTROSPECT_UE_ANIMATION"):
        report["anim_sequence_members"] = [
            name for name in dir(unreal.AnimSequence)
            if any(token in name.lower() for token in ("raw", "track", "curve", "skeleton"))]
        report["asset_tools_members"] = [
            name for name in dir(unreal.AssetToolsHelpers.get_asset_tools())
            if "asset" in name.lower() or "factory" in name.lower()]
        report["animation_api_types"] = [
            name for name in dir(unreal)
            if "animation" in name.lower() or "animsequence" in name.lower()]
        report["animation_controller_members"] = dir(unreal.AnimationDataController)
        report["raw_track_members"] = dir(unreal.RawAnimSequenceTrackExtensions)
        report["anim_factory_members"] = dir(unreal.AnimSequenceFactory)
    report_file = os.path.join(OUT, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"LightUSDUE UsdSkel fixture import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
