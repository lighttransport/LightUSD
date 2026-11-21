#!/usr/bin/env bash
set -euo pipefail

# Run the same cold-load measurement over representative Caldera districts.
# The dataset stays outside the repository; only the paths and timing summary
# are recorded. Override SCENES_FILE with a newline-delimited list of USD files
# (absolute paths or paths relative to CALDERA_ROOT).

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CALDERA_ROOT="${CALDERA_ROOT:-/mnt/disk1/data/caldera}"
SCENES_FILE="${SCENES_FILE:-}"
INVENTORY="${INVENTORY:-}"
DISCOVER="${DISCOVER:-canonical}"
OUT_DIR="${OUT_DIR:-/tmp/lusdview-caldera-matrix}"
LUSDVIEW="${LUSDVIEW:-${ROOT_DIR}/build_ninja/lusdview}"
TIMEOUT_SECS="${TIMEOUT_SECS:-0}"

if [[ ! -x "${LUSDVIEW}" ]]; then
  echo "FAIL: lusdview not executable: ${LUSDVIEW}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

if [[ -n "${SCENES_FILE}" ]]; then
  mapfile -t REL_SCENES < "${SCENES_FILE}"
elif [[ -n "${INVENTORY}" ]]; then
  # Inventory rows contain absolute paths, so the matrix accepts them without
  # rebasing against CALDERA_ROOT.
  mapfile -t REL_SCENES < <(awk -F '\t' \
    'NR > 1 && $2 == "prefab_candidate" && $4 == "ok" { print $1 }' \
    "${INVENTORY}")
elif [[ "${DISCOVER}" == all ]]; then
  mapfile -t REL_SCENES < <(find "${CALDERA_ROOT}" -type f \
    \( -name '*.usd' -o -name '*.usda' -o -name '*.usdc' \) -print | sort)
else
  REL_SCENES=(
    "map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain/st_main.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain/season_4/st_f.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/superterrrain/season_4/st_j.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/chem_factory/chem_factory_01.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/commercial/hotel_01.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/map_capital/ground/cliffs_set_02.usd"
    "map_source/prefabs/br/wz_vg/mp_wz_island/map_phosphate_mine/terrain/pm_cliffs/pm_cliffs_level_09.usd"
  )
fi

SUMMARY="${OUT_DIR}/summary.tsv"
printf 'scene\tstatus\tearly_preview_s\tcompose_s\tcollection_s\tgeometry_s\tfirst_useful_s\tfull_upload_s\tlog\n' > "${SUMMARY}"

for rel_scene in "${REL_SCENES[@]}"; do
  [[ -z "${rel_scene}" || "${rel_scene}" == \#* ]] && continue
  if [[ "${rel_scene}" = /* ]]; then
    scene="${rel_scene}"
  else
    scene="${CALDERA_ROOT}/${rel_scene}"
  fi
  if [[ ! -f "${scene}" ]]; then
    echo "SKIP: scene not found: ${scene}" >&2
    continue
  fi

  name="$(basename "${scene}")"
  id="$(printf '%s' "${scene}" | sha256sum | cut -c1-16)"
  scene_out="${OUT_DIR}/${id}"
  mkdir -p "${scene_out}"
  log="${scene_out}/${name}.wrapper.log"
  echo "RUN: ${scene}"
  set +e
  if [[ "${TIMEOUT_SECS}" -gt 0 ]]; then
    timeout "${TIMEOUT_SECS}s" env \
      SCENE="${scene}" OUT_DIR="${scene_out}" LUSDVIEW="${LUSDVIEW}" \
      PAYLOAD_MODE=defer PREVIEW_CACHE=off COMPOSE_THREADS="${COMPOSE_THREADS:-8}" \
      CONVERT_THREADS="${CONVERT_THREADS:-8}" \
      "${ROOT_DIR}/examples/lusdview/tests/run-large-scene-prefab-benchmark.sh" > "${log}" 2>&1
  else
    env SCENE="${scene}" OUT_DIR="${scene_out}" LUSDVIEW="${LUSDVIEW}" \
      PAYLOAD_MODE=defer PREVIEW_CACHE=off COMPOSE_THREADS="${COMPOSE_THREADS:-8}" \
      CONVERT_THREADS="${CONVERT_THREADS:-8}" \
      "${ROOT_DIR}/examples/lusdview/tests/run-large-scene-prefab-benchmark.sh" > "${log}" 2>&1
  fi
  rc=$?
  set -e

  # The benchmark writes the detailed log beside its output directory. Use it
  # when available; otherwise retain the wrapper log for timeout/error cases.
  detail="${scene_out}/${name}.log"
  [[ -f "${detail}" ]] && log="${detail}"
  value() { grep -m1 -E "$1" "${log}" 2>/dev/null | sed -E 's/.* ([0-9]+(\.[0-9]+)?) s.*/\1/' || true; }
  early="$(value 'composition preview uploaded')"
  compose="$(value 'core timing: compose ')"
  collection="$(value 'render-prim collection ')"
  geometry="$(value 'geometry estimates ')"
  useful="$(value 'first useful frame ')"
  full="$(value 'full scene uploaded and presented ')"
  status=pass
  [[ ${rc} -eq 0 ]] || status="fail(${rc})"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${scene}" "${status}" "${early:-NA}" "${compose:-NA}" \
    "${collection:-NA}" "${geometry:-NA}" "${useful:-NA}" "${full:-NA}" "${log}" \
    >> "${SUMMARY}"
done

echo "SUMMARY: ${SUMMARY}"
column -t -s $'\t' "${SUMMARY}" 2>/dev/null || sed -n '1,20p' "${SUMMARY}"
