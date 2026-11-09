#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CXX_COMPILER="${CXX:-c++}"
OPT_LEVEL="${1:--O2}"
OUTPUT_FILE="$(mktemp "${TMPDIR:-/tmp}/lucia-xatlas.XXXXXX.o")"
trap 'rm -f "$OUTPUT_FILE"' EXIT

"$CXX_COMPILER" -std=c++17 "$OPT_LEVEL" -fno-exceptions -fno-rtti \
  -DXA_MULTITHREADED=0 -c "$ROOT_DIR/src/external/xatlas/xatlas.cpp" \
  -o "$OUTPUT_FILE"
size "$OUTPUT_FILE"
