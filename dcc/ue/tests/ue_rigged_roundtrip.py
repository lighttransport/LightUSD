"""Headless UE skeletal USD export/import smoke test.

Run with UnrealEditor-Cmd and the LightUSDUE plugin enabled. The engine's
TutorialTPP asset is used when no MetaHuman asset is available locally.
"""

import json
import os
import sys

import unreal

import lightusd_ue


OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_rigged_roundtrip")
SKELETAL_MESH = "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP"
ANIMATION = "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd"


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    mesh = unreal.load_asset(SKELETAL_MESH)
    if not mesh:
        fail(f"Missing test skeletal mesh: {SKELETAL_MESH}")
    animation = unreal.load_asset(ANIMATION)

    mesh_file = os.path.join(OUT_DIR, "TutorialTPP.usda")
    exported_mesh = lightusd_ue.export_skeletal_mesh(mesh, mesh_file)
    if not exported_mesh.succeeded:
        fail(f"UE skeletal mesh export failed: {exported_mesh.error}")

    animation_file = None
    composed_animation_file = None
    animation_validation = None
    if animation:
        animation_file = os.path.join(OUT_DIR, "Tutorial_Walk_Fwd.usda")
        exported_animation = lightusd_ue.export_skeletal_animation(animation, animation_file)
        if not exported_animation.succeeded:
            fail(f"UE skeletal animation export failed: {exported_animation.error}")
        animation_validation = lightusd_ue.validate_usd(animation_file)
        if not animation_validation.succeeded:
            fail(f"LightUSD animation validation failed: {animation_validation.error}")
        composed_animation_file = os.path.join(OUT_DIR, "TutorialTPP_Walk_Composed.usda")
        lightusd_ue.compose_skeletal_animation(
            mesh_file, animation_file, composed_animation_file)

    validation = lightusd_ue.validate_usd(mesh_file)
    if not validation.succeeded:
        fail(f"LightUSD validation failed: {validation.error}")

    imported = lightusd_ue.import_usd(mesh_file, backend="native",
                                      import_groom=False,
                                      import_physics=True,
                                      import_skeletal_animation=True,
                                      package_path="/Game/LightUSD/RiggedRoundtrip")
    if not imported.succeeded:
        fail(f"Native UE roundtrip import failed: {imported.error}")

    # The native USD importer may evaluate a SkelAnimation without creating a
    # persistent UAnimSequence. Exercise the public UE animation data API as a
    # deterministic authoring fallback, using the imported skeleton asset.
    authored_animation = None
    imported_usd_animation = None
    imported_usd_animation_reloaded = None
    imported_usd_animation_export = None
    imported_usd_animation_validation = None
    imported_usd_playback = False
    authored_animation_export = None
    authored_animation_validation = None
    authored_animation_reloaded = None
    skeleton_asset = next((path for path in imported.created_assets
                           if "/SKEL_" in path and ".SKEL_" in path), None)
    skeletal_mesh_asset = next((path for path in imported.created_assets
                                if "/SK_" in path and ".SK_" in path), None)
    if skeleton_asset:
        if animation_file:
            imported_usd_animation = lightusd_ue.import_skeletal_animation(
                animation_file, "AN_TutorialTPP_USDImported",
                skeleton_asset,
                package_path="/Game/LightUSD/RiggedRoundtrip/ImportedAnimation",
                preview_mesh=skeletal_mesh_asset)
            if not imported_usd_animation.succeeded:
                fail("USD SkelAnimation to UE AnimSequence failed: "
                     f"{imported_usd_animation.error}")
            imported_usd_animation_reloaded = unreal.load_asset(
                imported_usd_animation.created_assets[0])
            if not imported_usd_animation_reloaded:
                fail("UE could not reload the imported USD AnimSequence")
            imported_file = os.path.join(OUT_DIR, "TutorialTPP_USDImported.usda")
            imported_usd_animation_export = lightusd_ue.export_skeletal_animation(
                imported_usd_animation_reloaded, imported_file)
            if not imported_usd_animation_export.succeeded:
                fail("Reloaded USD-imported AnimSequence export failed: "
                     f"{imported_usd_animation_export.error}")
            imported_usd_animation_validation = lightusd_ue.validate_usd(imported_file)
            if not imported_usd_animation_validation.succeeded:
                fail("Reloaded USD-imported animation validation failed: "
                     f"{imported_usd_animation_validation.error}")
            try:
                actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
                actor = actor_subsystem.spawn_actor_from_class(
                    unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
                component = actor.skeletal_mesh_component
                mesh_for_playback = unreal.load_asset(skeletal_mesh_asset)
                if hasattr(component, "set_skeletal_mesh_asset"):
                    component.set_skeletal_mesh_asset(mesh_for_playback)
                else:
                    component.set_skinned_asset_and_update(mesh_for_playback)
                if hasattr(component, "override_animation_data"):
                    component.override_animation_data(
                        imported_usd_animation_reloaded, False, True, 0.0, 1.0)
                else:
                    component.set_animation(imported_usd_animation_reloaded)
                    component.play_animation(imported_usd_animation_reloaded, False)
                imported_usd_playback = (
                    bool(component.animation_data and component.animation_data.anim_to_play))
                actor_subsystem.destroy_actor(actor)
            except Exception as exc:
                fail(f"UE could not assign imported animation for playback: {exc}")
        authored_animation = lightusd_ue.create_skeletal_animation(
            "AN_TutorialTPP_USDGenerated", skeleton_asset,
            {"Root": {"translations": [(0, 0, 0), (0, 0, 2)],
                       "rotations": [(0, 0, 0, 1), (0, 0, 0, 1)],
                       "scales": [(1, 1, 1), (1, 1, 1)]}},
            package_path="/Game/LightUSD/RiggedRoundtrip/Generated",
            frame_rate=30, num_frames=2, preview_mesh=skeletal_mesh_asset)
        if not authored_animation.succeeded:
            fail(f"UE persistent AnimSequence authoring failed: {authored_animation.error}")
        if authored_animation.succeeded:
            authored_animation_reloaded = unreal.load_asset(authored_animation.created_assets[0])
            if not authored_animation_reloaded:
                fail("UE could not reload the saved AnimSequence asset")
            authored_file = os.path.join(OUT_DIR, "TutorialTPP_USDGenerated.usda")
            authored_animation_export = lightusd_ue.export_skeletal_animation(
                authored_animation_reloaded, authored_file)
            if not authored_animation_export.succeeded:
                fail(f"UE authored animation export failed: {authored_animation_export.error}")
            authored_animation_validation = lightusd_ue.validate_usd(authored_file)
            if not authored_animation_validation.succeeded:
                fail(f"LightUSD authored animation validation failed: "
                     f"{authored_animation_validation.error}")

    animation_import = None
    if animation_file:
        # Keep this as a separate import: UE's USD importer expects the mesh
        # import to establish the skeleton before it consumes a SkelAnimation
        # layer exported from an AnimSequence.
        animation_import = lightusd_ue.import_usd(
            composed_animation_file, backend="native", import_physics=False,
            import_skeletal_animation=True,
            package_path="/Game/LightUSD/RiggedRoundtrip/Animations")
        if not animation_import.succeeded:
            fail(f"Native UE animation roundtrip import failed: {animation_import.error}")

    animation_registry_assets = []
    imported_animation_components = []
    if animation_file:
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        for asset_data in registry.get_assets_by_path("/Game/LightUSD", True):
            if "AnimSequence" not in str(asset_data.asset_class_path):
                continue
            package_name = str(asset_data.package_name)
            animation_registry_assets.append(package_name)
        try:
            actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
            for actor in actor_subsystem.get_all_level_actors():
                for component in actor.get_components_by_class(unreal.SkeletalMeshComponent):
                    imported_animation_components.append({
                        "actor": actor.get_path_name(),
                        "component": component.get_path_name(),
                        "skeletal_mesh": str(component.skeletal_mesh.get_path_name())
                        if component.skeletal_mesh else "",
                        "animation_mode": str(component.animation_mode),
                        "animation_asset": (
                            component.animation_data.anim_to_play.get_path_name()
                            if component.animation_data and component.animation_data.anim_to_play
                            else ""),
                    })
        except Exception as exc:
            imported_animation_components.append({"inspection_error": str(exc)})

    report = {
        "source_asset": SKELETAL_MESH,
        "metahuman_asset_found": False,
        "surrogate": "UE TutorialTPP skeletal mesh",
        "mesh_usda": mesh_file,
        "animation_usda": animation_file,
        "composed_animation_usda": composed_animation_file,
        "validation_prim_count": len(validation.prim_paths),
        "usdskel_prims": [path for path in validation.prim_paths
                          if any(token in path for token in ("Skel", "Skeleton", "Anim"))],
        "animation_validation_prim_count": (
            len(animation_validation.prim_paths) if animation_validation else 0),
        # The animation exporter emits a dedicated layer whose root name is
        # exporter-defined, so retain every validated prim path here rather
        # than relying on a path-name convention.
        "animation_usdskel_prims": (
            animation_validation.prim_paths if animation_validation else []),
        "native_import_assets": imported.created_assets,
        "native_import_warnings": imported.warnings,
        "native_animation_import_succeeded": (
            animation_import.succeeded if animation_import else False),
        "native_animation_import_assets": (
            animation_import.created_assets if animation_import else []),
        "native_animation_import_error": (
            animation_import.error if animation_import else ""),
        "native_animation_import_warnings": (
            animation_import.warnings if animation_import else []),
        "authored_animation_succeeded": bool(
            authored_animation and authored_animation.succeeded),
        "imported_usd_animation_succeeded": bool(
            imported_usd_animation and imported_usd_animation.succeeded),
        "imported_usd_animation_assets": (
            imported_usd_animation.created_assets if imported_usd_animation else []),
        "imported_usd_animation_error": (
            imported_usd_animation.error if imported_usd_animation else ""),
        "imported_usd_animation_reloaded": bool(imported_usd_animation_reloaded),
        "imported_usd_animation_export_succeeded": bool(
            imported_usd_animation_export and imported_usd_animation_export.succeeded),
        "imported_usd_animation_validation_prim_count": (
            len(imported_usd_animation_validation.prim_paths)
            if imported_usd_animation_validation else 0),
        "imported_usd_animation_playback_assigned": imported_usd_playback,
        "authored_animation_assets": (
            authored_animation.created_assets if authored_animation else []),
        "authored_animation_reloaded": bool(authored_animation_reloaded),
        "authored_animation_error": (
            authored_animation.error if authored_animation else ""),
        "authored_animation_export_succeeded": bool(
            authored_animation_export and authored_animation_export.succeeded),
        "authored_animation_export_error": (
            authored_animation_export.error if authored_animation_export else ""),
        "authored_animation_usda": (
            authored_animation_export.root_layer if authored_animation_export else ""),
        "authored_animation_validation_prim_count": (
            len(authored_animation_validation.prim_paths)
            if authored_animation_validation else 0),
        "animation_registry_assets": animation_registry_assets,
        "imported_animation_components": imported_animation_components,
    }
    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log(f"LightUSDUE rigged roundtrip passed: {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
