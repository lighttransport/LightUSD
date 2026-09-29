#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WEB_DIR="$(cd "${DEMO_DIR}/.." && pwd)"
LEGACY_BUILD_DIR="${WEB_DIR}/build_ninja"
NEXT_BUILD_DIR="${WEB_DIR}/build_next_ninja"
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

# Build the next product for the demo's default backend. Keep the combined
# product for explicit legacy comparison and compatibility checks.
configure_build "${LEGACY_BUILD_DIR}" lightusd_combined \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy \
  -DLIGHTUSD_WASM_PRODUCT=combined \
  -DLIGHTUSD_WASM_DEMODEV=OFF \
  -DLIGHTUSD_WASM64=OFF
configure_build "${NEXT_BUILD_DIR}" lightusd_next_wasm \
  -DLIGHTUSD_NATIVE_PRODUCT=next \
  -DLIGHTUSD_WASM_PRODUCT=next \
  -DLIGHTUSD_WASM_DEMODEV=OFF \
  -DLIGHTUSD_WASM64=OFF

for artifact in lightusd_combined.js lightusd_combined.wasm lightusd_next.js lightusd_next.wasm; do
  if [[ ! -f "${WEB_DIR}/js/src/lightusd/${artifact}" ]]; then
    echo "error: local LightUSD build did not produce ${artifact}." >&2
    exit 1
  fi
done

# The combined CMake target has a descriptive output name, while the browser
# loader's stable entrypoint is lightusd.js. Rewrite only the generated glue's
# adjacent WASM filename; both generated files remain ignored build artifacts.
cp -f "${WEB_DIR}/js/src/lightusd/lightusd_combined.wasm" \
  "${WEB_DIR}/js/src/lightusd/lightusd.wasm"
sed -i 's/lightusd_combined\.wasm/lightusd.wasm/g' \
  "${WEB_DIR}/js/src/lightusd/lightusd_combined.js"
cp -f "${WEB_DIR}/js/src/lightusd/lightusd_combined.js" \
  "${WEB_DIR}/js/src/lightusd/lightusd.js"

for artifact in lightusd.js lightusd.wasm; do
  if [[ ! -f "${WEB_DIR}/js/src/lightusd/${artifact}" ]]; then
    echo "error: combined WASM compatibility artifact missing ${artifact}." >&2
    exit 1
  fi
done

echo "Local LightUSD next and legacy compatibility modules are ready."
