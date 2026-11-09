#!/usr/bin/env bash
# Analytic opaque geometry channels, including reflected/nonuniform instances.
set -euo pipefail
BIN="${1:?lusdrender binary required}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
sed 's/scales = \[(2,1,1)\]/scales = [(-2,1,1)]/' "$ROOT/tests/usda/view-control-normal-calibration.usda" > "$TMP/reflected.usda"
python3 - "$ROOT/tests/usda/view-control-normal-calibration.usda" "$TMP" <<'PYFIXTURE'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text();root=Path(sys.argv[2])
(root/'left-handed.usda').write_text(source.replace('uniform token subdivisionScheme = "none"','uniform token subdivisionScheme = "none"\n  uniform token orientation = "leftHanded"'))
hole=source.replace('[(-2,-2,0),(2,-2,0),(0,2,0)]','[(-2,-2,0),(2,-2,0),(2,2,0),(-2,2,0)]').replace('faceVertexCounts = [3]','faceVertexCounts = [3,3]').replace('faceVertexIndices = [0,1,2]','faceVertexIndices = [0,1,2,0,2,3]\n  int[] holeIndices = [0]')
(root/'hole.usda').write_text(hole)
PYFIXTURE
for SCENE in original reflected left-handed; do
  SOURCE="$ROOT/tests/usda/view-control-normal-calibration.usda"
  if [ "$SCENE" != original ]; then SOURCE="$TMP/$SCENE.usda"; fi
  for MODE in cpu vkr vkInstanced vkInstancedStream; do
    MODE_ARGS=(); if [ "$MODE" != cpu ]; then MODE_ARGS=("-$MODE"); fi
    for CHANNEL in depth geometricNormal shadingNormal; do
      if ! "$BIN" "$SOURCE" "$TMP/$SCENE-$MODE-$CHANNEL.pfm" "${MODE_ARGS[@]}" --aov "$CHANNEL" -camera /C -w 32 -height 32 > "$TMP/$SCENE-$MODE-$CHANNEL.log" 2>&1; then
        if grep -qiE 'ray.query unavailable|Failed to create.*Vulkan|no Vulkan' "$TMP/$SCENE-$MODE-$CHANNEL.log"; then echo 'SKIP: hardware ray query unavailable';exit 77;fi
        cat "$TMP/$SCENE-$MODE-$CHANNEL.log";exit 1
      fi
    done
  done
done
for MODE in cpu vkr vkInstanced vkInstancedStream; do
  MODE_ARGS=(); if [ "$MODE" != cpu ]; then MODE_ARGS=("-$MODE"); fi
  "$BIN" "$TMP/hole.usda" "$TMP/hole-$MODE.pfm" "${MODE_ARGS[@]}" --aov depth -camera /C -w 32 -height 32 > "$TMP/hole-$MODE.log" 2>&1 || { cat "$TMP/hole-$MODE.log";exit 1; }
done
python3 - "$TMP" <<'PY'
import math,struct,sys
from pathlib import Path
root=Path(sys.argv[1])
for scene in ('original','reflected','left-handed'):
    sign=-1 if scene=='reflected' else 1
    geometric_sign=-1 if scene in ('reflected','left-handed') else 1
    for mode in ('cpu','vkr','vkInstanced','vkInstancedStream'):
        for channel,normal in {'depth':(5.000195,)*3,'geometricNormal':(0,0,geometric_sign),'shadingNormal':(sign/math.sqrt(5),0,2/math.sqrt(5))}.items():
            with (root/f'{scene}-{mode}-{channel}.pfm').open('rb') as f:
                assert f.readline().strip()==b'PF'
                assert f.readline().strip()==b'32 32'
                assert float(f.readline())<0
                values=struct.unpack('<3072f',f.read())
            i=(16*32+16)*3
            actual=values[i:i+3]
            assert all(abs(a-b)<1e-5 for a,b in zip(actual,normal)),(scene,mode,channel,actual,normal)
baseline=None
for mode in ('cpu','vkr','vkInstanced','vkInstancedStream'):
    with (root/f'hole-{mode}.pfm').open('rb') as f:
        f.readline();f.readline();f.readline();values=struct.unpack('<3072f',f.read())
    hits=sum(v>0 for v in values[::3]);assert 400<hits<624,(mode,hits)
    if baseline is not None:assert all(abs(a-b)<1e-5 for a,b in zip(values,baseline)),mode
    baseline=values
for log in root.glob('*.log'):
    assert 'Validation Error' not in log.read_text(),log
print('PASS: flat/instanced/streamed raw depth and distinct normals, including reflection')
PY
