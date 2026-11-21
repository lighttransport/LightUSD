#!/usr/bin/env bash
# Verify the default core loader retains render-ready Points and tessellated
# Curves records even for scenes containing no Mesh prims.
set -uo pipefail

LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root}"
SKIP=77

run_case() {
  local asset="$1" expected="$2" log rc
  log="$(timeout 30s "$LUSDVIEW" --headless --backend vk --frames 1 "$asset" 2>&1)"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    if grep -Eq "Vulkan.*(unavailable|failed)|no Vulkan|renderer init failed" <<<"$log"; then
      echo "SKIP: Vulkan headless backend unavailable"
      exit $SKIP
    fi
    echo "$log"
    echo "FAIL: lusdview failed for $asset"
    exit 1
  fi
  if ! grep -Fq -- "$expected" <<<"$log"; then
    echo "$log"
    echo "FAIL: missing extraction summary '$expected'"
    exit 1
  fi
  if grep -Fq -- "no renderable geometry produced" <<<"$log"; then
    echo "$log"
    echo "FAIL: non-mesh-only stage was rejected"
    exit 1
  fi
}

run_case "$ROOT/tests/usda/points-full-001.usda" \
  "retained 5 Points prim(s) / 55 samples"
run_case "$ROOT/tests/usda/basiscurves-full-001.usda" \
  "7 Curves prim(s) / 224 tessellated samples"

opacity_log="$(timeout 30s "$LUSDVIEW" --headless --backend vk --frames 1 \
  "$ROOT/tests/usda/lusdview-nonmesh-points-curves.usda" 2>&1)"
if ! grep -Fq -- \
    "1 Points prim(s) / 3 samples and 3 Curves prim(s) / 43 tessellated samples" \
    <<<"$opacity_log"; then
  echo "$opacity_log"
  echo "FAIL: checked Basis/Hermite/NURBS fixture was not fully retained"
  exit 1
fi
if ! grep -Fq -- "retained displayOpacity for 4 non-mesh prim(s)" \
    <<<"$opacity_log"; then
  echo "$opacity_log"
  echo "FAIL: Points/Curves displayOpacity was not retained"
  exit 1
fi
if ! grep -Fq -- "retained authored constant width for 1 Curves prim(s)" \
    <<<"$opacity_log"; then
  echo "$opacity_log"
  echo "FAIL: tessellation dropped an authored constant curve width"
  exit 1
fi

echo "PASS: next non-mesh geometry extraction"
