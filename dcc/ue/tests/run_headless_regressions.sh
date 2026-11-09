#!/usr/bin/env bash
set -euo pipefail

ENGINE_EDITOR="${ENGINE_EDITOR:?set ENGINE_EDITOR to UnrealEditor-Cmd}"
PROJECT="${PROJECT:?set PROJECT to a .uproject}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_ROOT="${LIGHTUSD_UE_TEST_OUT:-/tmp/lightusd_ue_regressions}"
RUN_ID="${LIGHTUSD_UE_TEST_RUN_ID:-$(date +%s)}"
mkdir -p "${OUT_ROOT}"

run_script() {
  local script="$1"
  local name="$2"
  local out="${OUT_ROOT}/${name}"
  mkdir -p "${out}"
  LIGHTUSD_UE_TEST_OUT="${out}" \
  LIGHTUSD_UE_TEST_PACKAGE="/Game/LightUSD/Automated_${RUN_ID}_${name}" \
  LIGHTUSD_GROOM_PACKAGE="/Game/LightUSD/Grooms_${RUN_ID}_${name}" \
  "${ENGINE_EDITOR}" "${PROJECT}" \
    -ExecutePythonScript="${SCRIPT_DIR}/${script}" \
    -unattended -nullrhi -NoSplash -stdout -FullStdOutLogOutput -NoSound -NoUBA \
    >"${out}/editor.log" 2>&1
  test -s "${out}/report.json"
}

run_script ue_physics_roundtrip.py physics
run_script ue_material_graph_roundtrip.py material_graph
run_script ue_groom_roundtrip.py groom
run_script ue_rigged_roundtrip.py rigged
run_script ue_usdskel_fixture.py usdskel_fixture

run_groom_fixture() {
  local fixture="$1"
  local name="$2"
  local out="${OUT_ROOT}/${name}"
  mkdir -p "${out}"
  LIGHTUSD_UE_TEST_OUT="${out}" \
  LIGHTUSD_UE_TEST_PACKAGE="/Game/LightUSD/Automated_${RUN_ID}_${name}" \
  LIGHTUSD_GROOM_PACKAGE="/Game/LightUSD/Grooms_${RUN_ID}_${name}" \
  LIGHTUSD_GROOM_USD="${fixture}" \
  "${ENGINE_EDITOR}" "${PROJECT}" \
    -ExecutePythonScript="${SCRIPT_DIR}/ue_groom_roundtrip.py" \
    -unattended -nullrhi -NoSplash -stdout -FullStdOutLogOutput -NoSound -NoUBA \
    >"${out}/editor.log" 2>&1
  test -s "${out}/report.json"
}

run_groom_fixture "${ROOT_DIR}/tests/usda/blender-animated-groom.usda" groom_animated
run_groom_fixture "${ROOT_DIR}/tests/usda/blender-nurbs-groom.usda" groom_nurbs
run_groom_fixture "${ROOT_DIR}/tests/usda/blender-guide-groom.usda" groom_guides
run_script ue_groom_cards_roundtrip.py groom_cards

if [[ "${LIGHTUSD_RUN_METAHUMAN:-0}" == "1" ]]; then
  run_script ue_metahuman_template_roundtrip.py metahuman_template
  run_script ue_metahuman_material_roundtrip.py metahuman_material_udim
  run_script ue_metahuman_template_scene_roundtrip.py metahuman_scene
fi

if [[ -n "${LIGHTUSD_ASSET_ID:-}" && -n "${LIGHTUSD_ASSET_BRIDGE_URL:-}" ]]; then
  run_script ue_asset_bridge_roundtrip.py asset_bridge
fi

echo "LightUSD UE headless regression suite passed: ${OUT_ROOT}"
