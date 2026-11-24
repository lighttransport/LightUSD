#!/usr/bin/env bash
#
# Regression test: a GPU-morphed instanced PointInstancer prototype must NOT be
# wrongly frustum-culled. The morph instance's REST-pose box is off-screen but
# its morphed geometry is in view; per-instance culling pads protoAabb by the
# morph displacement (DrawMeshCPU::morphExtent) so the instance survives.
#
# Asserts the render stats report all 4 instances visible. Without the morph
# protoAabb pad the morph instance culls and this drops to 3/4 -> test fails.
#
# Needs a GL (or VK) context: runs under xvfb-run when no $DISPLAY. Software GL
# (llvmpipe) is fine -- culling is CPU-side, independent of rasterization. If no
# renderer can be created the test SKIPs (exit 77, ctest SKIP_RETURN_CODE) rather
# than failing, so headless CI without a GPU/display does not report a failure.
#
# Exit codes: 0 = pass, 1 = fail (instance wrongly culled), 77 = skip.
#
# Usage (from the repo root, after building build/lusdview):
#   examples/lusdview/tests/run-inst-morph-cull.sh
# Overrides: LUSDVIEW=<binary>  BACKEND=gl|vk
# Also run via ctest: `ctest -R lusdview-inst-morph-cull`.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUSDVIEW="${LUSDVIEW:-$SCRIPT_DIR/../../../build/lusdview}"
BACKEND="${BACKEND:-gl}"
ASSET="$SCRIPT_DIR/inst_morph_cull.usda"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW (set LUSDVIEW=...)"
  exit $SKIP
fi

TMP_DIR="$(mktemp -d)"
OUT="$TMP_DIR/inst_morph_cull.png"

# Pin the window size. Without it the window falls back to whatever the display
# offers, and under xvfb-run that leaves a ~32px-wide 3D viewport once the
# docked panels take their share -- narrow enough that the instances fall
# outside the frustum and are (correctly) culled, so the stat this test asserts
# reflects the window geometry rather than the culling logic under test.
CONFIG="$TMP_DIR/config.json"
printf '%s\n' '{"window_size":{"width":800,"height":600}}' > "$CONFIG"
RUN=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a)
fi

# --frames loads synchronously (deterministic); the morph weight is 1.0 (static),
# so no timeline is needed -- this exercises the culling path at full morph.
LOG="$("${RUN[@]}" "$LUSDVIEW" --backend "$BACKEND" --core --config "$CONFIG" \
       --frames 4 --screenshot "$OUT" "$ASSET" 2>&1)"
echo "$LOG"

if ! echo "$LOG" | grep -q "render stats"; then
  echo "SKIP: no renderer (backend '$BACKEND' unavailable in this environment)"
  exit $SKIP
fi

if echo "$LOG" | grep -q "instances 4/4 visible"; then
  echo "PASS: all 4 instances visible (morphed prototype not culled)"
  exit 0
fi

echo "FAIL: expected 'instances 4/4 visible' (the morphed instance was culled --"
echo "      protoAabb is likely not padded by the GPU morph displacement)."
exit 1
