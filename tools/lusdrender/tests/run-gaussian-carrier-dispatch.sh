#!/usr/bin/env bash
# Verify that Vulkan and HIP keep native analytic Gaussian rendering for a pure
# splat stage, but route Gaussian data through the bounded flat fallback when
# native Points are present as well.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run() {
  # Force one splat per chunk so the regression covers deferred GPU BVH
  # materialization and release, rather than only the single-chunk fast path.
  # The eight-triangle GPU cap also makes the mixed fallback flush its distinct
  # Gaussian color buckets through one global pending budget.
  "$LUSDRENDER" -gaussianChunk 1 -gpuTriangleChunk 8 "$1" "$2" \
    -vkr -w 32 -height 32 -autoframe -stats \
    >"$3" 2>&1
}

run "$REPO_ROOT/tests/usda/lusdview-gaussian-splat.usda" \
    "$TMP/pure.png" "$TMP/pure.log"
if [ "$?" -ne 0 ]; then
  if grep -qiE "failed to create .*vulkan|no vulkan|vulkan.*unavailable|requested gpu backend not built" \
      "$TMP/pure.log"; then
    echo "SKIP: no usable Vulkan LightRT backend"
    exit "$SKIP"
  fi
  cat "$TMP/pure.log"
  echo "FAIL: pure Gaussian Vulkan render failed"
  exit 1
fi
if ! grep -q "native Gaussian ellipses: 2 in 2 Vulkan chunk(s)" "$TMP/pure.log"; then
  cat "$TMP/pure.log"
  echo "FAIL: pure Gaussian scene did not use native Vulkan ellipses"
  exit 1
fi

run "$REPO_ROOT/tests/usda/lusdview-gaussian-points.usda" \
    "$TMP/mixed.png" "$TMP/mixed.log"
if [ "$?" -ne 0 ]; then
  cat "$TMP/mixed.log"
  echo "FAIL: Points+Gaussian Vulkan render failed"
  exit 1
fi
if ! grep -q "triangles: 144" "$TMP/mixed.log"; then
  cat "$TMP/mixed.log"
  echo "FAIL: mixed scene did not retain Points and Gaussian fallback geometry"
  exit 1
fi

"$LUSDRENDER" -gaussianChunk 1 -gpuTriangleChunk 8 "$REPO_ROOT/tests/usda/lusdview-gaussian-splat.usda" \
  "$TMP/pure-hip.png" -hip -w 32 -height 32 -autoframe -stats \
  >"$TMP/pure-hip.log" 2>&1
if [ "$?" -ne 0 ]; then
  if grep -qiE "HIP ray tracing unavailable|no ROCm|no HIP device|requested GPU backend not built|hiprtc.*not available" \
      "$TMP/pure-hip.log"; then
    if grep -q "\[gpu\] gaussian splats" "$TMP/pure-hip.log"; then
      cat "$TMP/pure-hip.log"
      echo "FAIL: pure HIP Gaussian scene built the tessellated fallback before backend availability was known"
      exit 1
    fi
    echo "SKIP: no usable HIP/ROCm device (Vulkan coverage passed)"
  else
    cat "$TMP/pure-hip.log"
    echo "FAIL: pure Gaussian HIP render failed"
    exit 1
  fi
else
  if ! grep -q "native Gaussian ellipses: 2 in 2 HIP chunk(s)" "$TMP/pure-hip.log"; then
    cat "$TMP/pure-hip.log"
    echo "FAIL: pure Gaussian HIP scene did not use native ellipses"
    exit 1
  fi
  echo "PASS: native Gaussian HIP dispatch verified"
fi

echo "PASS: pure native Gaussian and mixed-carrier Vulkan dispatch verified"
