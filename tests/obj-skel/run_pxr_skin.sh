#!/bin/sh
# Skinning of the imported FBX and OBJ (+ FBX sidecar) scenes, LightUSD vs
# OpenUSD's UsdSkel (tools/view_control/skinning_compare.py), and blend shapes
# + skinning vs UsdSkel.BakeSkinning (pxr_bake_compare.py).
#
#   run_pxr_skin.sh LUSDCAT SKIN_COMPARE WORKDIR
#
# Needs OPENUSD_ROOT (an OpenUSD install with lib/python/pxr; OPENUSD_PYTHON
# selects the matching interpreter, default python3). Exit 77 (skip) without it.
set -eu
[ -n "${OPENUSD_ROOT:-}" ] || { echo "SKIP: set OPENUSD_ROOT to run the OpenUSD cross-check"; exit 77; }
LUSDCAT=$1; SKIN_COMPARE=$2; WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$WORK"
export SKIN_COMPARE PYTHONPATH="$OPENUSD_ROOT/lib/python${PYTHONPATH:+:$PYTHONPATH}"
export LD_LIBRARY_PATH="$OPENUSD_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fail=0
for s in fbx_scene obj_scene; do
  "$LUSDCAT" -f "$HERE/$s.usda" > "$WORK/$s.flat.usda"
  for t in 1 6 12 18 24; do
    if ! "${OPENUSD_PYTHON:-python3}" "$ROOT/tools/view_control/skinning_compare.py" \
        "$WORK/$s.flat.usda" "$t" "$WORK/$s.$t.json" --native-dump "$HERE/skin_dump.sh"; then
      echo "FAIL $s t=$t"; fail=1
    fi
  done
done
# blend shapes + skinning (FBX blend shape channels): UsdSkel.BakeSkinning
"$LUSDCAT" -f "$HERE/blend_scene.usda" > "$WORK/blend_scene.flat.usda"
"${OPENUSD_PYTHON:-python3}" "$HERE/pxr_bake_compare.py" "$WORK/blend_scene.flat.usda" \
    "$HERE/skin_dump.sh" 1 6 12 18 24 || { echo "FAIL blend_scene"; fail=1; }
# in-betweens, extrapolated weights, dense shapes (hand-written; not from FBX)
"${OPENUSD_PYTHON:-python3}" "$HERE/pxr_bake_compare.py" "$HERE/inbetween_scene.usda" \
    "$HERE/skin_dump.sh" 1 2 3 4 5 6 || { echo "FAIL inbetween_scene"; fail=1; }
exit $fail
