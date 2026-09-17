#!/usr/bin/env bash
set -euo pipefail

# UE 5.8's bundled UBA can race while renaming a shared PCH on Linux.  Keep
# the reliable path as the default, while allowing an explicit UBA opt-in for
# installations where Epic has fixed the detour.
ENGINE_BINARY=""
PROJECT=""
TARGET=""
USE_UBA=0
LOG_FILE="${TMPDIR:-/tmp}/lightusd-ue-build.log"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --engine-binary) ENGINE_BINARY="$2"; shift 2 ;;
    --project) PROJECT="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --uba) USE_UBA=1; shift ;;
    --log) LOG_FILE="$2"; shift 2 ;;
    --) shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${ENGINE_BINARY}" && -f "${ENGINE_BINARY}/Build/BatchFiles/Linux/Build.sh" ]] || {
  echo "--engine-binary must point at the UE installation" >&2; exit 2;
}
[[ -n "${PROJECT}" && -f "${PROJECT}" ]] || {
  echo "--project must point at a .uproject" >&2; exit 2;
}
TARGET="${TARGET:-$(basename "${PROJECT}" .uproject)Editor}"
BUILD_ARGS=("${TARGET}" Linux Development "${PROJECT}" -NoHotReloadFromIDE)
if [[ "${USE_UBA}" -eq 0 ]]; then
  BUILD_ARGS+=( -NoUBA )
fi
BUILD_ARGS+=("${EXTRA_ARGS[@]}")

set +e
"${ENGINE_BINARY}/Build/BatchFiles/Linux/Build.sh" "${BUILD_ARGS[@]}" 2>&1 | tee "${LOG_FILE}"
status=${PIPESTATUS[0]}
set -e

if [[ "${status}" -ne 0 && "${USE_UBA}" -eq 1 ]] && \
   rg -qi 'shared.?PCH|rename|MoveFile|UBA' "${LOG_FILE}"; then
  echo "UBA shared-PCH failure detected; retrying once with -NoUBA" >&2
  "${ENGINE_BINARY}/Build/BatchFiles/Linux/Build.sh" \
    "${TARGET}" Linux Development "${PROJECT}" -NoHotReloadFromIDE -NoUBA \
    "${EXTRA_ARGS[@]}" 2>&1 | tee "${LOG_FILE}.no-uba"
  status=${PIPESTATUS[0]}
fi
exit "${status}"
