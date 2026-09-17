"""Headless LightUSD BasisCurves -> UE HairStrands regression."""

import json
import os

import unreal

import lightusd_ue


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
USD_FILE = os.environ.get(
    "LIGHTUSD_GROOM_USD", os.path.join(ROOT, "tests/usda/ue-groom-roundtrip.usda"))
OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_groom")
TARGET = os.environ.get(
    "LIGHTUSD_GROOM_TARGET", "/MetaHumanCharacter/Face/SKM_Face")
PACKAGE = os.environ.get("LIGHTUSD_GROOM_PACKAGE", "/Game/LightUSD/Grooms")


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def main():
    if not os.path.isfile(USD_FILE):
        fail(f"Missing groom fixture: {USD_FILE}")
    os.makedirs(OUT_DIR, exist_ok=True)
    result = lightusd_ue.import_usd(
        USD_FILE,
        backend="lightusd",
        import_groom=True,
        import_physics=False,
        package_path=PACKAGE,
        groom_target_skeletal_mesh_path=TARGET,
    )
    if not result.succeeded:
        fail(result.error or "LightUSD groom import failed")
    groom_assets = []
    binding_assets = []
    groom_caches = []
    hair_materials = []
    for path in result.created_assets:
        # The LightUSD adapter also records a human-readable render summary;
        # only package paths can be passed to EditorAssetLibrary.
        if not path.startswith("/"):
            continue
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if not asset:
            continue
        class_name = asset.get_class().get_name()
        if class_name == "GroomAsset":
            groom_assets.append(path)
        elif class_name == "GroomBindingAsset":
            binding_assets.append(path)
        elif class_name == "GroomCache":
            groom_caches.append(path)
        elif class_name == "Material":
            hair_materials.append(path)
    if not groom_assets:
        fail(f"No UGroomAsset was created: {result.created_assets}")
    if TARGET and not binding_assets:
        fail(f"No UGroomBindingAsset was created for {TARGET}: {result.created_assets}")
    if not hair_materials:
        fail(f"No default Hair material was created: {result.created_assets}")
    with open(USD_FILE, "r", encoding="utf-8") as stream:
        source_text = stream.read()
    is_nurbs = "NurbsCurves" in source_text
    if is_nurbs and not any("NurbsCurves were tessellated" in warning
                            for warning in result.warnings):
        fail(f"NurbsCurves input was not tessellated: {result.warnings}")
    if ".timeSamples" in source_text and not groom_caches:
        fail(f"Animated groom did not create a UGroomCache: {result.created_assets}")
    exported_usd = os.path.join(OUT_DIR, "ue_groom_export.usda")
    exported = lightusd_ue.export_groom(
        unreal.EditorAssetLibrary.load_asset(groom_assets[0]),
        exported_usd,
    )
    if not exported.succeeded:
        fail(f"UE groom export failed: {exported.error}")
    exported_validation = lightusd_ue.validate_usd(exported_usd)
    if not exported_validation.succeeded:
        fail(f"Exported groom validation failed: {exported_validation.error}")
    with open(exported_usd, "r", encoding="utf-8") as stream:
        exported_text = stream.read()
    for primvar in ("primvars:groom_color", "primvars:groom_roughness"):
        if primvar not in exported_text:
            fail(f"Exported groom is missing {primvar}")
    if "BasisCurves" not in exported_text:
        fail("Exported groom did not use BasisCurves")
    if not is_nurbs and "1, 0, 2" not in exported_text:
        fail("Exported groom lost non-zero source point coordinates")
    exported_cache_usd = None
    cache_reimport_assets = []
    if groom_caches:
        exported_cache_usd = os.path.join(OUT_DIR, "ue_groom_cache_export.usda")
        exported_cache = lightusd_ue.export_groom_cache(
            unreal.EditorAssetLibrary.load_asset(groom_caches[0]),
            unreal.EditorAssetLibrary.load_asset(groom_assets[0]),
            exported_cache_usd,
        )
        if not exported_cache.succeeded:
            fail(f"UE groom cache export failed: {exported_cache.error}")
        with open(exported_cache_usd, "r", encoding="utf-8") as stream:
            exported_cache_text = stream.read()
        for animated_attr in ("points.timeSamples", "widths.timeSamples"):
            if animated_attr not in exported_cache_text:
                fail(f"Exported groom cache is missing {animated_attr}")
        if "0.2" not in exported_cache_text:
            fail("Exported groom cache lost non-zero animated point coordinates")
        cache_reimport = lightusd_ue.import_usd(
            exported_cache_usd, backend="lightusd", import_groom=True,
            import_physics=False, package_path=PACKAGE + "/CacheImported",
            groom_target_skeletal_mesh_path=TARGET,
        )
        if not cache_reimport.succeeded:
            fail(f"UE groom cache reimport failed: {cache_reimport.error}")
        cache_reimport_assets = cache_reimport.created_assets
    imported_again = lightusd_ue.import_usd(
        exported_usd, backend="lightusd", import_groom=True,
        import_physics=False, package_path=PACKAGE + "/Imported",
        groom_target_skeletal_mesh_path=TARGET,
    )
    if not imported_again.succeeded:
        fail(f"UE groom reimport failed: {imported_again.error}")
    report = {
        "usd_file": USD_FILE,
        "target_skeletal_mesh": TARGET,
        "created_assets": result.created_assets,
        "warnings": result.warnings,
        "groom_assets": groom_assets,
        "binding_assets": binding_assets,
        "groom_caches": groom_caches,
        "hair_materials": hair_materials,
        "exported_usd": exported_usd,
        "exported_cache_usd": exported_cache_usd,
        "exported_prim_paths": exported_validation.prim_paths,
        "reimport_created_assets": imported_again.created_assets,
        "cache_reimport_created_assets": cache_reimport_assets,
    }
    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"LightUSD groom import passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
