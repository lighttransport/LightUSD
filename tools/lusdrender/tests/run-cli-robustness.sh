#!/usr/bin/env bash
#
# Regression test: hostile or unsupported CLI input must fail cleanly, never
# abort or silently no-op.
#
#   -fitScale abc          used std::stof -> uncaught std::invalid_argument -> SIGABRT.
#   -w / -samples <huge>   were accepted up to INT_MAX; the framebuffer/sample
#                          loop had no other guard, so they were an OOM/hang.
#   -frames on a plain .usda (no -rtPreview) was silently ignored: one image
#                          named literally with the #### token. It now renders
#                          every frame (all CPU renders use the core path).
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"; exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

# GNU coreutils `timeout` is not present on stock macOS runners. Keep the
# timeout contract portable without requiring Homebrew: Python is already a
# test prerequisite and can terminate the child with the same 124 status.
run_timeout() {
  local duration="$1"; shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$duration" "$@"
    return $?
  fi
  python3 - "$duration" "$@" <<'PY'
import subprocess
import sys

seconds = float(sys.argv[1].rstrip("s"))
try:
    result = subprocess.run(sys.argv[2:], timeout=seconds)
except subprocess.TimeoutExpired:
    sys.exit(124)
sys.exit(result.returncode)
PY
}

cat > "$TMP/tri.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World"; upAxis = "Y")
def Xform "World" {
    def Mesh "Tri" {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
    }
}
USDA

# Each hostile input must exit non-zero WITHOUT being killed by a signal
# (128+N = signal; 134 = the old SIGABRT from std::stof).
expect_clean_failure() {
  local desc="$1"; shift
  run_timeout 20 "$LUSDRENDER" "$@" > "$TMP/log" 2>&1
  local rc=$?
  if [ $rc -eq 0 ]; then fail "$desc was accepted (exit 0)"; fi
  if [ $rc -ge 128 ]; then fail "$desc crashed/hung (exit $rc): $(tail -1 "$TMP/log")"; fi
}

expect_clean_failure "-fitScale abc" "$TMP/tri.usda" "$TMP/x.png" -fitScale abc
expect_clean_failure "invalid material diagnostic" "$TMP/tri.usda" "$TMP/x.png" -diagnosticMaterial other
expect_clean_failure "material diagnostic with data AOV" "$TMP/tri.usda" "$TMP/x.png" -diagnosticMaterial roughness --aov depth
expect_clean_failure "unsupported instanced material diagnostic" "$TMP/tri.usda" "$TMP/x.png" -diagnosticMaterial metallic -vkInstanced
expect_clean_failure "-fitScale 1e999" "$TMP/tri.usda" "$TMP/x.png" -fitScale 1e999
for option in -maxMem -maxVram; do
  for value in nan inf 1garbage -1; do
    expect_clean_failure "$option $value" "$TMP/tri.usda" "$TMP/x.png" "$option" "$value"
  done
done
for value in 0 -1 nan 18446744073709551616 16garbage; do
  expect_clean_failure "-maxArrayElements $value" "$TMP/tri.usda" "$TMP/x.png" -maxArrayElements "$value"
done
for option in -gpuTriangleChunk -gpuMaterialPathTriangles -triangleChunk -curveChunk -gaussianMax -gaussianChunk -hermiteSegments -instChunkTris -instBatchTris -instBudget; do
  for value in 0 -1 18446744073709551616 3garbage; do
    expect_clean_failure "$option $value" "$TMP/tri.usda" "$TMP/x.png" "$option" "$value"
  done
done
expect_clean_failure "Hermite limit" "$TMP/tri.usda" "$TMP/x.png" -hermiteSegments 1025
expect_clean_failure "BLAS count overflow" "$TMP/tri.usda" "$TMP/x.png" -instChunkTris 4294967296
expect_clean_failure "unknown debug AOV" "$TMP/tri.usda" "$TMP/x.png" -rtDebugAov unknown
cat > "$TMP/missing.usda" <<'USDA'
#usda 1.0
def Xform "World" (references = @absent.usda@</Asset>) {}
USDA
expect_clean_failure "missing referenced geometry" "$TMP/missing.usda" "$TMP/x.png" -rtPreview -w 16 -height 16
expect_clean_failure "-w 2000000000" "$TMP/tri.usda" "$TMP/x.png" -w 2000000000
expect_clean_failure "-height 2000000000" "$TMP/tri.usda" "$TMP/x.png" -height 2000000000
expect_clean_failure "-samples 2000000000" "$TMP/tri.usda" "$TMP/x.png" -samples 2000000000
expect_clean_failure "--pt-samples 2000000000" "$TMP/tri.usda" "$TMP/x.png" \
    -cuda --path-trace --pt-samples 2000000000
expect_clean_failure "--path-trace without a supported backend" \
    "$TMP/tri.usda" "$TMP/x.png" --path-trace
# -frames works on every CPU render (all inputs use the core path).
"$LUSDRENDER" "$TMP/tri.usda" "$TMP/f.####.png" -frames 1:3 -w 16 -height 16 \
    >"$TMP/log" 2>&1 || fail "-frames on a plain .usda failed: $(tail -1 "$TMP/log")"
[ -s "$TMP/f.0001.png" ] && [ -s "$TMP/f.0003.png" ] \
  || fail "-frames 1:3 did not write per-frame images"

# Sane inputs must still work, including the largest 32-bit array-count cap.
run_timeout 60 "$LUSDRENDER" "$TMP/tri.usda" "$TMP/ok.png" -w 16 -height 16 \
    -samples 1 -fitScale 1.5 -autoframe -maxArrayElements 4294967295 -triangleChunk 1 -curveChunk 1 -hermiteSegments 8 > "$TMP/log" 2>&1 \
  || fail "a valid render regressed: $(cat "$TMP/log")"
[ -f "$TMP/ok.png" ] || fail "valid render wrote no image"

env LUSDR_TRIANGLE_CHUNK=0 LUSDR_GAUSSIAN_MAX=bad LUSDR_LIGHT_NEE=0 \
  LUSDR_PTEX_PROBE="$TMP/no-directory/probe" \
  "$LUSDRENDER" "$TMP/tri.usda" "$TMP/clean.png" -rtPreview -w 16 -height 16 \
  -triangleChunk 1 > "$TMP/log" 2>&1 || fail "inherited environment changed the render"
[ -f "$TMP/clean.png" ] || fail "isolated render wrote no image"

echo "PASS: hostile CLI inputs fail cleanly; valid ones still render"
exit 0
