#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD32_DIR="${WEB_DIR}/cmake-build"
BUILD64_DIR="${WEB_DIR}/cmake-build64"
BUILD_NEXT32_DIR="${WEB_DIR}/cmake-build-next"
BUILD_NEXT64_DIR="${WEB_DIR}/cmake-build-next64"
BUILD_MCP32_DIR="${WEB_DIR}/cmake-build-mcp"
BUILD_MCP64_DIR="${WEB_DIR}/cmake-build-mcp64"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-MinSizeRel}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
PARALLEL_JOBS="${PARALLEL_JOBS:-}"
CLEAN_BUILD_DIRS="${CLEAN_BUILD_DIRS:-1}"

source "${SCRIPT_DIR}/setup-nodejs.sh"
source "${SCRIPT_DIR}/setup-emsdk.sh"

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake not found." >&2
  exit 1
fi

if ! command -v emcmake >/dev/null 2>&1; then
  echo "error: emcmake not found after sourcing local emsdk." >&2
  exit 1
fi

build_target() {
  local build_dir="$1"
  shift

  if [[ "${CLEAN_BUILD_DIRS}" == "1" ]]; then
    rm -rf "${build_dir}"
  fi

  echo "[build-wasm] Configuring ${build_dir}"
  emcmake cmake -S "${WEB_DIR}" -B "${build_dir}" -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" "$@"

  echo "[build-wasm] Building ${build_dir}"
  if [[ -n "${PARALLEL_JOBS}" ]]; then
    cmake --build "${build_dir}" --parallel "${PARALLEL_JOBS}"
  else
    cmake --build "${build_dir}" --parallel
  fi
}

build_target "${BUILD32_DIR}" -DLIGHTUSD_WASM_PRODUCT=legacy
build_target "${BUILD64_DIR}" -DLIGHTUSD_WASM_PRODUCT=legacy -DLIGHTUSD_WASM64=1
build_target "${BUILD_NEXT32_DIR}" -DLIGHTUSD_WASM_PRODUCT=next
build_target "${BUILD_NEXT64_DIR}" -DLIGHTUSD_WASM_PRODUCT=next -DLIGHTUSD_WASM64=1
build_target "${BUILD_MCP32_DIR}" -DLIGHTUSD_WASM_PRODUCT=mcp
build_target "${BUILD_MCP64_DIR}" -DLIGHTUSD_WASM_PRODUCT=mcp -DLIGHTUSD_WASM64=1

cat <<EOF
[build-wasm] Complete.
[build-wasm] Expected outputs:
  ${WEB_DIR}/js/src/lightusd/lightusd.js
  ${WEB_DIR}/js/src/lightusd/lightusd.wasm
  ${WEB_DIR}/js/src/lightusd/lightusd_64.js
  ${WEB_DIR}/js/src/lightusd/lightusd_64.wasm
  ${WEB_DIR}/js/src/lightusd/lightusd_next.js
  ${WEB_DIR}/js/src/lightusd/lightusd_next.wasm
  ${WEB_DIR}/js/src/lightusd/lightusd_next_64.js
  ${WEB_DIR}/js/src/lightusd/lightusd_next_64.wasm
EOF
