#!/usr/bin/env bash
# Verify HermiteCurves are collected by the core loader and reach the bounded
# Vulkan curve tessellation path together with Basis/Nurbs curves and Points.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"
ASSET="$REPO_ROOT/tests/usda/lusdview-nonmesh-points-curves.usda"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$LUSDRENDER" -hermiteSegments 8 "$ASSET" "$TMP/curves-cpu.png" \
  -rtPreview -w 32 -height 32 -autoframe -stats >"$TMP/curves-cpu.log" 2>&1
if [ "$?" -ne 0 ] || ! grep -q "rt native curve chunks: round 3 (22 segments)" \
    "$TMP/curves-cpu.log"; then
  cat "$TMP/curves-cpu.log"
  echo "FAIL: Hermite tangents were not tessellated in the CPU core path"
  exit 1
fi
"$LUSDRENDER" -hermiteSegments 8 "$ASSET" "$TMP/curves.png" -vkr -w 32 -height 32 \
  -autoframe -stats >"$TMP/curves.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  if grep -qiE "failed to create .*vulkan|no vulkan|vulkan.*unavailable|requested gpu backend not built" \
      "$TMP/curves.log"; then
    echo "SKIP: no usable Vulkan LightRT backend"
    exit "$SKIP"
  fi
  cat "$TMP/curves.log"
  echo "FAIL: Vulkan curve render failed (rc=$rc)"
  exit 1
fi
if ! grep -q "native curves: round 3 chunk(s)" "$TMP/curves.log" ||
   ! grep -q "triangles: 360" "$TMP/curves.log"; then
  cat "$TMP/curves.log"
  echo "FAIL: HermiteCurves were not retained in the Vulkan curve path"
  exit 1
fi
echo "PASS: HermiteCurves reach the bounded Vulkan curve path"
