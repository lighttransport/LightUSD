"""Upload UE-generated USD files through the LightUSD asset bridge."""

import json
import os

import unreal

from lightusd_ue import upload_file


def main():
    url = os.environ["LIGHTUSD_ASSET_BRIDGE_URL"]
    token = os.environ.get("LIGHTUSD_ASSET_BRIDGE_TOKEN", "")
    filenames = [item for item in os.environ["LIGHTUSD_BRIDGE_FILES"].split(";")
                 if item]
    uploads = [upload_file(url, filename, token) for filename in filenames]
    report_file = os.environ.get(
        "LIGHTUSD_BRIDGE_REPORT", "D:/work/lightusd/UBTFullTest/bridge_uploads.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump({"uploads": uploads}, stream, indent=2)
    unreal.log(f"Uploaded {len(uploads)} USD files through LightUSD bridge")
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


try:
    main()
except Exception as exc:
    unreal.log_error(str(exc))
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
