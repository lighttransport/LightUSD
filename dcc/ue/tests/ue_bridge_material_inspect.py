"""Inspect texture references on a UE material imported from bridged USD."""

import json
import os

import unreal


def main():
    material_path = os.environ["LIGHTUSD_MATERIAL_INSTANCE"]
    material = unreal.EditorAssetLibrary.load_asset(material_path)
    if not material:
        raise RuntimeError(f"Missing material instance: {material_path}")
    values = []
    try:
        for value in material.get_editor_property("texture_parameter_values"):
            texture = value.get_editor_property("parameter_value")
            values.append({
                "name": str(value.get_editor_property("parameter_info").get_editor_property("name")),
                "texture": texture.get_path_name() if texture else "",
            })
    except Exception as exc:
        values.append({"inspection_error": str(exc)})
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/BridgeMaterial/inspect.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"material": material_path, "texture_parameters": values}, stream, indent=2)
    unreal.log(f"Bridge material inspection wrote {report_file}")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
