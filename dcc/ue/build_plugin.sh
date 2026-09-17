#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENGINE_SOURCE=""
ENGINE_BINARY=""
PROJECT=""
INSTALL_ENGINE=""
INSTALL_PROJECT=""
BUILD_DIR="${ROOT_DIR}/build_ue_lightusd"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --engine-source) ENGINE_SOURCE="$2"; shift 2 ;;
    --engine-binary) ENGINE_BINARY="$2"; shift 2 ;;
    --project) PROJECT="$2"; shift 2 ;;
    --install-engine) INSTALL_ENGINE="$2"; shift 2 ;;
    --install-project) INSTALL_PROJECT="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

for required in ENGINE_SOURCE ENGINE_BINARY PROJECT INSTALL_ENGINE INSTALL_PROJECT; do
  [[ -n "${!required}" ]] || { echo "missing argument: ${required}" >&2; exit 2; }
done
[[ -f "${ENGINE_SOURCE}/Engine/Build/Build.version" ]] || { echo "invalid engine source" >&2; exit 2; }
[[ -f "${ENGINE_BINARY}/Build/Build.version" ]] || { echo "invalid engine binary" >&2; exit 2; }

ENGINE_BINARY_REAL="$(readlink -f "${ENGINE_BINARY}")"
UE_CLANGXX="$(find "${ENGINE_BINARY_REAL}/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64" -path '*/x86_64-unknown-linux-gnu/bin/clang++' -type f -print -quit)"
UE_CLANG="$(dirname "${UE_CLANGXX}")/clang"
UE_LLVM_AR="$(dirname "${UE_CLANGXX}")/llvm-ar"
[[ -x "${UE_CLANGXX}" && -x "${UE_CLANG}" && -x "${UE_LLVM_AR}" ]] || {
  echo "could not locate the UE Linux Clang toolchain under ${ENGINE_BINARY_REAL}" >&2
  exit 2
}

LIGHTUSD_BUILD_DIR="${BUILD_DIR}/lightusd-ue-x86_64"
cmake -S "${ROOT_DIR}/src/next" -B "${LIGHTUSD_BUILD_DIR}" -G Ninja \
  -DLIGHTUSD_NEXT_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_C_COMPILER="${UE_CLANG}" \
  -DCMAKE_CXX_COMPILER="${UE_CLANGXX}" \
  -DCMAKE_AR="${UE_LLVM_AR}" \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++"
cmake --build "${LIGHTUSD_BUILD_DIR}" --target lightusd_c

stage_one() {
  local destination="$1"
  mkdir -p "${destination}/Source" "${destination}/ThirdParty/Linux/include" "${destination}/ThirdParty/Linux/lib" "${destination}/Content/Python"
  cp -R "${ROOT_DIR}/dcc/ue/Source" "${destination}/"
  cp -R "${ROOT_DIR}/dcc/ue/Content/Python/lightusd_ue" "${destination}/Content/Python/"
  cp "${ROOT_DIR}/dcc/ue/LightUSDUE.uplugin" "${destination}/"
  cp "${ROOT_DIR}/src/c-api/lightusd-c.h" "${destination}/ThirdParty/Linux/include/"
  cp "${ROOT_DIR}/src/c-api/lightusd-render-c.h" "${destination}/ThirdParty/Linux/include/"
  cp "${LIGHTUSD_BUILD_DIR}/liblightusd_c.a" "${destination}/ThirdParty/Linux/lib/"
  cp "${LIGHTUSD_BUILD_DIR}/liblightusd_next.a" "${destination}/ThirdParty/Linux/lib/"
  cp "${LIGHTUSD_BUILD_DIR}/tydra_next_build/libtydra_next.a" "${destination}/ThirdParty/Linux/lib/"
  echo "staged LightUSDUE: ${destination}"
}

stage_one "${INSTALL_ENGINE}"
stage_one "${INSTALL_PROJECT}"
