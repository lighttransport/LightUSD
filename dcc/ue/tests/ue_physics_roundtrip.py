"""Headless UE/LightUSD physics schema preservation regression."""

import json
import os

import unreal

import lightusd_ue


ROOT = os.environ.get(
    "LIGHTUSD_REPO_ROOT",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
INPUT = os.environ.get(
    "LIGHTUSD_PHYSICS_USD",
    os.path.join(ROOT, "tests/usda/ue-physics-roundtrip.usda"))
OUT_DIR = os.environ.get("LIGHTUSD_UE_TEST_OUT", "/tmp/lightusd_ue_physics")


def fail(message):
    unreal.log_error(message)
    raise RuntimeError(message)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    result = lightusd_ue.import_usd(
        INPUT, backend="lightusd", import_geometry=True,
        import_materials=False, import_groom=False, import_physics=True)
    if not result.succeeded:
        fail("Physics fixture import failed: " + result.error)
    required = ("/PhysicsScene", "/RigidBody", "/Collider",
                "/PhysicsMaterial", "/FixedJoint")
    missing = [path for path in required if path not in result.prim_paths]
    if missing:
        fail("Physics fixture lost prims: " + ", ".join(missing))
    if not any("Physics API schemas" in warning for warning in result.warnings):
        fail("Physics preservation diagnostic was not emitted")
    report = {
        "input": INPUT,
        "prim_paths": result.prim_paths,
        "warnings": result.warnings,
        "created_assets": result.created_assets,
    }
    report_file = os.path.join(OUT_DIR, "report.json")
    with open(report_file, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log("LightUSD physics schema roundtrip passed: " + report_file)
    unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, False)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        unreal.log_error(str(exc))
        unreal.SystemLibrary.quit_game(None, None, unreal.QuitPreference.QUIT, True)
