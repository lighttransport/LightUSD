#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WEB_DIR="$(cd "${DEMO_DIR}/.." && pwd)"
CORE_BUILD_DIR="${WEB_DIR}/build_ninja"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-MinSizeRel}"
JOBS="${JOBS:-16}"
export EM_CACHE="${EM_CACHE:-${WEB_DIR}/build_ninja/emscripten-cache}"
mkdir -p "${EM_CACHE}"

for tool in emcmake cmake ninja; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: ${tool} is required for local LightUSD demo development." >&2
    exit 1
  fi
done

configure_build() {
  local build_dir="$1"
  local target="$2"
  shift 2
  local cmake_refresh=()
  if [[ -f "${build_dir}/CMakeCache.txt" ]] &&
     ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=${WEB_DIR}" "${build_dir}/CMakeCache.txt"; then
    cmake_refresh=(--fresh)
  fi
  emcmake cmake "${cmake_refresh[@]}" -S "${WEB_DIR}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DLIGHTUSD_WASM_OUTPUT_DIRECTORY="${WEB_DIR}/js/src/lightusd" "$@"
  cmake --build "${build_dir}" --target "${target}" --parallel "${JOBS}"
}

# Build the published core distribution, including its optional feature modules.
configure_build "${CORE_BUILD_DIR}" lightusd_core_distribution \
  -DLIGHTUSD_WASM_PRODUCT=core \
  -DLIGHTUSD_WASM_DEMODEV=OFF \
  -DLIGHTUSD_WASM64=OFF

# Lucia authors through the independently built core MCP server module.
configure_build "${CORE_BUILD_DIR}/mcp" lightusd_mcp_wasm \
  -DLIGHTUSD_WASM_PRODUCT=mcp \
  -DLIGHTUSD_WASM_DEMODEV=OFF \
  -DLIGHTUSD_WASM64=OFF

for artifact in lightusd_mcp.js lightusd_mcp.wasm lightusd.js lightusd.wasm lightusd_validator.js lightusd_validator.wasm lightusd_exr.js lightusd_exr.wasm lightusd_geometry.js lightusd_geometry.wasm lightusd_raytrace.js lightusd_raytrace.wasm; do
  if [[ ! -f "${WEB_DIR}/js/src/lightusd/${artifact}" ]]; then
    echo "error: local LightUSD build did not produce ${artifact}." >&2
    exit 1
  fi
done

# The standalone texture codec demo has its own small C11 WASM module.
# It shares textools sources with core but does not link the USD library.
bash "${WEB_DIR}/js/texcomp/build.sh"

echo "Local LightUSD core, MCP and texture codec modules are ready."
