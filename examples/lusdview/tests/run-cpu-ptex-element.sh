#!/usr/bin/env bash
# Element-scale Island PTex material routing.
#
# Renders the real quad-topology isGardeniaA element through the CPU path and
# checks every visible PTex base-colour sample against OpenPtex: the renderer
# dumps (face, mip, u, v, sampled RGB) per pixel via
# LIGHTUSD_CPU_PTEX_PROBE, and the verifier re-samples the same texels with the
# independent decoder. This exercises authored quads, per-face mip selection and
# per-face Ptex pages on real element geometry rather than a synthetic plane.
#
# The render must also not fall back to the representative-face path, so the log
# is checked for the "unsupported non-quad" warning.
#
# Requires the Island corpus, a C++ compiler and an OpenPtex 2.4.3 build.
# Skips (77) when any is absent.
#
# Exit codes: 0 = pass, 1 = mismatch, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
ELEMENT="${LIGHTUSD_PTEX_ELEMENT:-/mnt/nvme02/data/island/usd/elements/isGardeniaA/instance.usda}"
PTEX_SRC="${LIGHTUSD_PTEX_SOURCE:-/tmp/gap-ptex-upstream}"
PTEX_LIB="${LIGHTUSD_PTEX_LIBRARY:-$REPO_ROOT/build_ninja/ptex_upstream_debug/src/ptex}"
CXX="${CXX:-g++}"

[ -x "$BIN" ] || { echo "SKIP: lusdview not found: $BIN"; exit "$SKIP"; }
[ -f "$ELEMENT" ] || { echo "SKIP: Island element not found: $ELEMENT"; exit "$SKIP"; }
[ -f "$PTEX_SRC/src/ptex/Ptexture.h" ] || {
  echo "SKIP: OpenPtex source not found: $PTEX_SRC"; exit "$SKIP"; }
[ -f "$PTEX_LIB/libPtex.so" ] || {
  echo "SKIP: OpenPtex library not found: $PTEX_LIB"; exit "$SKIP"; }
command -v "$CXX" >/dev/null 2>&1 || { echo "SKIP: compiler missing"; exit "$SKIP"; }

OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
if [ -z "${LUSDVIEW_TEST_OUT:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi

if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  XVFB_PREFIX="xvfb-run -a"
else
  XVFB_PREFIX=""
fi

LOG="$OUT/render.log"
# shellcheck disable=SC2086
LIGHTUSD_CPU_PTEX_PROBE="$OUT/probe.txt" \
  timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-1800s}" \
  ${XVFB_PREFIX} "$BIN" --headless --backend vk --cpu-rt --frames 1 \
  --size 64x64 --screenshot "$OUT/element.ppm" "$ELEMENT" >"$LOG" 2>&1
status=$?
if grep -Eiq 'renderer init failed: no Vulkan|no Vulkan physical device|Vulkan.*unavailable' "$LOG"; then
  echo "SKIP: Vulkan unavailable"; exit "$SKIP"
fi
[ "$status" -eq 0 ] || { echo "FAIL: render exited $status"; tail -40 "$LOG"; exit 1; }
grep -q 'CPU RT wrote' "$LOG" || {
  echo "FAIL: CPU RT did not own the screenshot"; tail -40 "$LOG"; exit 1; }
if grep -q 'unsupported non-quad or mismatched topology' "$LOG"; then
  echo "FAIL: the real element fell back to the representative-face path"
  grep 'unsupported non-quad' "$LOG" | head -3
  exit 1
fi
[ -s "$OUT/probe.txt" ] || {
  echo "FAIL: no PTex probe dump (the element has no PTex base colour?)"
  tail -40 "$LOG"
  exit 1
}
[ -s "$OUT/element.ppm" ] || { echo "FAIL: missing screenshot"; exit 1; }

"$CXX" -std=c++17 -O2 -I"$PTEX_SRC/src/ptex" \
  "$REPO_ROOT/tests/verification/repros/ptex-element-probe-verify.cc" \
  -L"$PTEX_LIB" -lPtex -lz -o "$OUT/ptex-element-verify" || {
  echo "FAIL: could not build the OpenPtex verifier"; exit 1; }
LD_LIBRARY_PATH="$PTEX_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$OUT/ptex-element-verify" "$OUT/probe.txt"
