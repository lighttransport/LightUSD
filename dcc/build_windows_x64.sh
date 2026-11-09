#!/usr/bin/env bash
set -euo pipefail

# Cross-build LightUSD's stable C API DLL for Windows x64.  The DLL is the
# runtime shared by the Blender Python bridge and the UE editor plugin; UE's
# own module must still be compiled by UBT on Windows because its ABI is
# MSVC/Unreal-specific.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_MINGW_DIR="${LLVM_MINGW_DIR:-${HOME}/local/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build_windows_x64}"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/dist/lightusd-windows-x64}"
WINE_BIN="${WINE_BIN:-}"
RUN_WINE="${RUN_WINE:-ON}"

die() { echo "error: $*" >&2; exit 2; }

[[ -x "${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-clang" ]] \
  || die "llvm-mingw not found: ${LLVM_MINGW_DIR}"
[[ -x "${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-clang++" ]] \
  || die "llvm-mingw C++ compiler not found"

if [[ -z "${WINE_BIN}" ]]; then
  if command -v wine64 >/dev/null 2>&1; then
    WINE_BIN="$(command -v wine64)"
  elif command -v wine >/dev/null 2>&1; then
    # Debian/Ubuntu packages expose the 64-bit loader as `wine` rather than
    # `wine64`; the PE binary remains a 64-bit Windows executable.
    WINE_BIN="$(command -v wine)"
  fi
fi

export LLVM_MINGW_DIR
cmake -S "${ROOT_DIR}/src/next" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/llvm-mingw-cross.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DLIGHTUSD_NEXT_ENABLE_THREAD=OFF \
  -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DLIGHTUSD_NEXT_BUILD_SHARED_C_API=ON
cmake --build "${BUILD_DIR}" --target lightusd_c_shared test_lightusd_c --parallel

DLL="${BUILD_DIR}/liblightusd_c.dll"
[[ -f "${DLL}" ]] || DLL="${BUILD_DIR}/lightusd_c.dll"
[[ -f "${DLL}" ]] || die "shared DLL was not produced"

mkdir -p "${STAGE_DIR}/bin" "${STAGE_DIR}/lib" "${STAGE_DIR}/include" \
  "${STAGE_DIR}/ue/ThirdParty/Win64/include" \
  "${STAGE_DIR}/ue/ThirdParty/Win64/lib" \
  "${STAGE_DIR}/ue/Binaries/Win64" \
  "${STAGE_DIR}/blender"

# Stage a complete source plugin package.  UBT must compile the UE module on
# Windows; this script supplies its ABI-stable LightUSD dependency and keeps
# the package layout identical to an installed UE plugin.
cp -R "${ROOT_DIR}/dcc/ue/Source" "${STAGE_DIR}/ue/"
cp -R "${ROOT_DIR}/dcc/ue/Content" "${STAGE_DIR}/ue/"
cp "${ROOT_DIR}/dcc/ue/LightUSDUE.uplugin" "${STAGE_DIR}/ue/"
mkdir -p "${STAGE_DIR}/ue-python-only/Source" "${STAGE_DIR}/ue-python-only/Content"
cp -R "${ROOT_DIR}/dcc/ue/Source/LightUSDUEPython" "${STAGE_DIR}/ue-python-only/Source/"
rsync -a --delete --delete-excluded --exclude='__pycache__' "${ROOT_DIR}/dcc/ue/Content/Python/" \
  "${STAGE_DIR}/ue-python-only/Content/Python/"
cp "${ROOT_DIR}/dcc/ue/LightUSDUEPython.uplugin" "${STAGE_DIR}/ue-python-only/"
cp "${DLL}" "${STAGE_DIR}/bin/lightusd_c.dll"
cp "${ROOT_DIR}/src/c-api/lightusd-c.h" \
  "${ROOT_DIR}/src/c-api/lightusd-render-c.h" "${STAGE_DIR}/include/"

# llvm-mingw's libc++ runtime is a DLL dependency of the shared C API.  Copy
# the target-architecture runtime beside the DLL and the Wine smoke binary so
# both the packaged bridge and the test use the same runtime closure.
for runtime_dll in libc++.dll libunwind.dll libwinpthread-1.dll; do
  runtime_path="${LLVM_MINGW_DIR}/x86_64-w64-mingw32/bin/${runtime_dll}"
  [[ -f "${runtime_path}" ]] || die "missing llvm-mingw runtime: ${runtime_path}"
  cp "${runtime_path}" "${STAGE_DIR}/bin/"
  cp "${runtime_path}" "${BUILD_DIR}/"
done

# llvm-mingw emits a GNU import archive.  Keep its native name and also place
# it under the UE staging tree; UBT/clang-cl can consume the COFF import
# archive when the Windows plugin is built with the matching toolchain.
for import_lib in "${BUILD_DIR}"/liblightusd_c.dll.a "${BUILD_DIR}"/lightusd_c.dll.a; do
  if [[ -f "${import_lib}" ]]; then
    cp "${import_lib}" "${STAGE_DIR}/lib/lightusd_c.dll.a"
    cp "${import_lib}" "${STAGE_DIR}/ue/ThirdParty/Win64/lib/lightusd_c.lib"
    break
  fi
done
cp "${STAGE_DIR}/include/"* "${STAGE_DIR}/ue/ThirdParty/Win64/include/"
cp "${STAGE_DIR}/bin/lightusd_c.dll" "${STAGE_DIR}/ue/Binaries/Win64/"
for runtime_dll in libc++.dll libunwind.dll libwinpthread-1.dll; do
  cp "${STAGE_DIR}/bin/${runtime_dll}" "${STAGE_DIR}/ue/Binaries/Win64/"
done

if [[ "${RUN_WINE}" == ON ]]; then
  [[ -n "${WINE_BIN}" ]] || die "wine64/wine not found; set RUN_WINE=OFF to skip"
  TEST_EXE="${BUILD_DIR}/test_lightusd_c.exe"
  [[ -f "${TEST_EXE}" ]] || die "C API smoke executable was not produced"
  WINEPREFIX="${WINEPREFIX:-${BUILD_DIR}/wineprefix}" WINEARCH=win64 \
    "${WINE_BIN}" "${TEST_EXE}"
fi

cat > "${STAGE_DIR}/README.txt" <<EOF
LightUSD Windows x64 cross-build

Runtime DLL: bin/lightusd_c.dll
C API headers: include/
UE staging: ue/ThirdParty/Win64 and ue/Binaries/Win64
Blender integration: load bin/lightusd_c.dll from the LightUSD Python bridge.
Toolchain: ${LLVM_MINGW_DIR}
Wine test loader: ${WINE_BIN:-not-run}
EOF
echo "staged Windows x64 LightUSD artifacts: ${STAGE_DIR}"
