#!/bin/bash
# OpenUSD minimal build WITH command-line tools (for usdcat).
# Mirrors setup_openusd_nopython.sh but ENABLES tools (drops --no-tools) so the
# C++ usdcat binary is built, and installs to dist_minimal. usdcat only needs USD
# core + TBB; we still disable python/imaging/usdview/materialx/etc. to keep it
# small and fast. Used to cross-check lusdview's --core flatten vs `usdcat
# --flatten`.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENUSD_DIR="${SCRIPT_DIR}/OpenUSD"
DIST_DIR="${SCRIPT_DIR}/dist_minimal"

echo "== Clone/refresh OpenUSD (release) =="
if [ -d "${OPENUSD_DIR}" ]; then
    cd "${OPENUSD_DIR}"
    git fetch origin && git checkout release && git pull origin release
else
    git clone -b release https://github.com/lighttransport/OpenUSD.git "${OPENUSD_DIR}"
fi

export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"
echo "CC=$CC CXX=$CXX"

cd "${OPENUSD_DIR}"

BUILD_ARGS=(
    "${DIST_DIR}"
    --no-tests
    --no-examples
    --no-tutorials
    --no-docs
    --no-python
    --no-imaging
    --no-usdview
    --no-materialx
    --no-embree
    --no-ptex
    --no-openvdb
    --no-draco
    --tools
    --build-args
    "USD,\"-DPXR_BUILD_ALEMBIC_PLUGIN=OFF\""
)

echo "== Build (install -> ${DIST_DIR}) =="
python3 build_scripts/build_usd.py "${BUILD_ARGS[@]}"

echo "== Done. usdcat: ${DIST_DIR}/bin/usdcat =="
ls -la "${DIST_DIR}/bin/usdcat" 2>/dev/null || echo "WARNING: usdcat not found in ${DIST_DIR}/bin"
