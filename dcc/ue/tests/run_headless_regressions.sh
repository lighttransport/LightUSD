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

echo "LightUSD UE headless regression suite passed: ${OUT_ROOT}"
