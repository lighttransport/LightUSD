"""Create a fresh MetaHuman template scene and round-trip its USD meshes.

This test intentionally uses the local MetaHuman identity templates instead of
EOS, MetaHuman Creator, or auto-rigging.  It is suitable for a newly created
UE project with the MetaHumanCharacter, USDImporter, and LightUSDUE plugins
enabled.
"""

import json
import os

import unreal

import lightusd_ue


OUT_DIR = os.environ.get(
    "LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_metahuman_template_scene")
MAP_PATH = os.environ.get(
    "LIGHTUSD_UE_TEST_MAP", "/Game/LightUSD/MetaHumanTemplateScene")
PACKAGE_PATH = os.environ.get(
    "LIGHTUSD_UE_TEST_PACKAGE", "/Game/LightUSD/MetaHumanTemplateScene")
TEMPLATES = {
    "face": "/MetaHumanCharacter/Face/SKM_Face",
    "body": "/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body",
}


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def save_scene():
    """Save the current editor world, using the UE 5.8-compatible API."""
    world = unreal.EditorLevelLibrary.get_editor_world()
    if not world:
        fail("No editor world is available for the MetaHuman template scene")
    save_utils = getattr(unreal, "EditorLoadingAndSavingUtils", None)
    if save_utils and hasattr(save_utils, "save_map"):
        if not save_utils.save_map(world, MAP_PATH):
            fail(f"Unable to save MetaHuman template scene: {MAP_PATH}")
        return
    if not unreal.EditorLevelLibrary.save_current_level_as(MAP_PATH):
        fail(f"Unable to save MetaHuman template scene: {MAP_PATH}")


def create_scene(assets):
    """Spawn one skeletal actor per template and persist the level."""
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = []
    locations = {"face": unreal.Vector(-60, 0, 0),
                 "body": unreal.Vector(60, 0, 0)}
    for name, mesh in assets.items():
        actor = actor_subsystem.spawn_actor_from_class(
            unreal.SkeletalMeshActor, locations[name], unreal.Rotator(0, 0, 0))
        if not actor:
            fail(f"Unable to spawn {name} MetaHuman template actor")
        actor.set_actor_label(f"LightUSD_MetaHumanTemplate_{name}")
        component = actor.skeletal_mesh_component
        if hasattr(component, "set_skeletal_mesh_asset"):
            component.set_skeletal_mesh_asset(mesh)
        else:
            component.set_skinned_asset_and_update(mesh)
        actors.append(actor.get_path_name())
    save_scene()
    return actors


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
        fail(f"UE rejected skeletal mesh export: {asset.get_path_name()}")
    if not os.path.isfile(filename):
        fail(f"UE produced no USD file: {filename}")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    assets = {}
    for name, path in TEMPLATES.items():
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if not asset:
            fail(f"Missing MetaHuman identity template: {path}")
        assets[name] = asset

    actor_paths = create_scene(assets)
    report = {
        "eos_used": False,
        "auto_rigging_used": False,
        "scene": MAP_PATH,
        "actors": actor_paths,
        "templates": {},
    }

    for name, asset in assets.items():
        usd_file = os.path.join(OUT_DIR, f"{name}.usda")
        export_asset(asset, usd_file)
        validation = lightusd_ue.validate_usd(usd_file, backend="lightusd")
        if not validation.succeeded:
            fail(f"LightUSD validation failed for {name}: {validation.error}")
        imported = lightusd_ue.import_usd(
            usd_file,
            backend="lightusd",
            package_path=f"{PACKAGE_PATH}/Imported/{name}",
            import_physics=True,
            import_skeletal_animation=False,
            import_groom=False,
        )
        if not imported.succeeded:
            fail(f"LightUSD import failed for {name}: {imported.error}")
        native_imported = lightusd_ue.import_usd(
            usd_file,
            backend="native",
            package_path=f"{PACKAGE_PATH}/NativeImported/{name}",
            import_physics=True,
            import_skeletal_animation=False,
            import_groom=False,
        )
        if not native_imported.succeeded:
            fail(f"Native UE import failed for {name}: {native_imported.error}")
        report["templates"][name] = {
            "source_asset": TEMPLATES[name],
            "usd_file": usd_file,
            "validation_backend": validation.backend,
            "validation_prim_count": len(validation.prim_paths),
            "import_backend": imported.backend,
            "imported_assets": imported.created_assets,
            "native_imported_assets": native_imported.created_assets,
            "warnings": imported.warnings,
        }

    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"MetaHuman template scene USD roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
