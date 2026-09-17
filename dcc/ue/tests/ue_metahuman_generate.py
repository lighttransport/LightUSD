"""Create and assemble a MetaHuman Character asset from UE Python.

This is intended for UnrealEditor-Cmd. It uses the public MetaHuman Character
editor subsystem shipped in UE 5.8, so no UI interaction is required.
"""

import json
import os

import unreal


PACKAGE_PATH = os.environ.get("LIGHTUSD_UE_METAHUMAN_PACKAGE", "/Game/LightUSD/MetaHumanCLI")
ASSET_NAME = os.environ.get("LIGHTUSD_UE_METAHUMAN_NAME", "MH_LightUSD_CLI")


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def main():
    asset_path = f"{PACKAGE_PATH}/{ASSET_NAME}.{ASSET_NAME}"
    character = unreal.load_asset(asset_path)
    if character is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        character = tools.create_asset(
            asset_name=ASSET_NAME,
            package_path=PACKAGE_PATH,
            asset_class=unreal.MetaHumanCharacter,
            factory=unreal.new_object(type=unreal.MetaHumanCharacterFactoryNew),
        )
    if character is None:
        fail(f"Unable to create MetaHuman Character asset: {asset_path}")

    subsystem = unreal.get_editor_subsystem(unreal.MetaHumanCharacterEditorSubsystem)
    if not subsystem.try_add_object_to_edit(character=character):
        fail(f"Unable to open MetaHuman Character for editing: {asset_path}")

    built = False
    error = ""
    can_build = False
    generated_assets = []
    try:
        # Seed the blank character with the plugin's built-in identity
        # templates. This is the supported offline path for producing a
        # riggable MetaHuman without a downloaded character archive.
        face_mesh = unreal.EditorAssetLibrary.load_asset(
            "/MetaHumanCharacter/Face/SKM_Face")
        body_mesh = unreal.EditorAssetLibrary.load_asset(
            "/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body")
        if not face_mesh or not body_mesh:
            fail("MetaHuman identity template assets are unavailable")

        import_params = unreal.ImportFromTemplateParams()
        import_params.use_eye_meshes = True
        import_params.use_teeth_mesh = True
        import_params.alignment_options = (
            unreal.MetaHumanAlignmentOptions.SCALING_ROTATION_TRANSLATION)
        result = subsystem.import_from_template(
            character, face_mesh, None, None, None, import_params)
        if result != unreal.ImportErrorCode.SUCCESS:
            fail(f"MetaHuman face template import failed: {result}")

        result, vertices = subsystem.get_mesh_for_body_conforming_from_template(
            character, body_mesh, face_mesh, match_vertices_by_u_vs=False)
        if result != unreal.ImportErrorCode.SUCCESS:
            fail(f"MetaHuman body template extraction failed: {result}")
        result, _, joint_rotations = subsystem.get_joints_for_body_conforming_from_template(
            body_mesh)
        if result != unreal.ImportErrorCode.SUCCESS:
            fail(f"MetaHuman body joint extraction failed: {result}")
        if not subsystem.conform_body_to_target(
                character, vertices, joint_rotations,
                target_is_in_a_pose=True, estimate_joints_from_mesh=False):
            fail("MetaHuman body conform failed")

        rig_request = unreal.MetaHumanCharacterAutoRiggingRequestParams()
        rig_request.blocking = True
        rig_request.report_progress = False
        rig_request.rig_type = unreal.MetaHumanRigType.JOINTS_ONLY
        subsystem.request_auto_rigging(character, rig_request)

        can_build = subsystem.can_build_meta_human(character, True)
        if not can_build:
            fail("MetaHuman is still not ready for assembly after template conform and auto-rig")

        params = unreal.MetaHumanCharacterEditorBuildParameters()
        params.pipeline_type = unreal.MetaHumanDefaultPipelineType.OPTIMIZED
        params.pipeline_quality = unreal.MetaHumanQualityLevel.MEDIUM
        params.absolute_build_path = f"{PACKAGE_PATH}/Generated"
        params.common_folder_path = f"{PACKAGE_PATH}/Common"
        params.enable_wardrobe_item_validation = False
        subsystem.build_meta_human(character=character, params=params)
        generated_assets = unreal.EditorAssetLibrary.list_assets(
            f"{PACKAGE_PATH}/Generated", recursive=True, include_folder=False)
        built = bool(generated_assets)
    except Exception as exc:
        error = str(exc)
    finally:
        if subsystem.is_object_added_for_editing(character):
            subsystem.remove_object_to_edit(character=character)

    unreal.EditorAssetLibrary.save_loaded_asset(character)
    report = {
        "asset": asset_path,
        "created_or_reused": True,
        "build_requested": True,
        "can_build_before_assembly": can_build,
        "build_succeeded": built,
        "generated_assets": generated_assets,
        "build_error": error,
    }
    report_path = os.environ.get(
        "LIGHTUSD_UE_METAHUMAN_REPORT", "/tmp/lightusd_ue_metahuman_generate.json")
    with open(report_path, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    if not built:
        fail(f"MetaHuman build failed; see {report_path}: {error}")
    unreal.log(f"MetaHuman CLI generation passed: {report_path}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
