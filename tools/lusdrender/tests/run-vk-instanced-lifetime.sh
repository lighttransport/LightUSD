#!/usr/bin/env bash
# Completed BLAS inputs can be reclaimed; narrow/wide/multi/compacted hits agree.
set -euo pipefail
BIN="${1:?lusdrender binary required}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$ROOT/tests/usda/view-control-normal-calibration.usda" "$TMP/scene.usda" <<'PY'
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text().replace('protoIndices = [0]','protoIndices = [0,0]').replace('positions = [(0,0,0)]','positions = [(-1,0,0),(1,0,0)]').replace('scales = [(2,1,1)]','scales = [(.5,1,1),(.5,1,1)]')
Path(sys.argv[2]).write_text(s)
PY
if ! env LRT_VK_BLAS_COMPACT=1 "$BIN" "$TMP/scene.usda" "$TMP/narrow.png" -vkInstanced -camera /C -w 64 -height 64 > "$TMP/narrow.log" 2>&1; then
  if grep -qiE 'ray.query unavailable|Failed to create.*Vulkan|no Vulkan' "$TMP/narrow.log"; then
    echo 'SKIP: hardware ray query unavailable';exit 77
  fi
  cat "$TMP/narrow.log";exit 1
fi
for STREAM in 0 1; do
for COMPACT in 0 1; do
  LRT_VK_TLAS_SLICE=1 LRT_VK_BLAS_COMPACT="$COMPACT" LRT_VK_STREAM_TLAS="$STREAM" \
    "$BIN" -forceWide "$TMP/scene.usda" "$TMP/wide-$STREAM-$COMPACT.png" -vkInstanced -camera /C -w 64 -height 64 > "$TMP/wide-$STREAM-$COMPACT.log" 2>&1 || { cat "$TMP/wide-$STREAM-$COMPACT.log";exit 1; }
  grep -q '2-way multi-TLAS' "$TMP/wide-$STREAM-$COMPACT.log"
  cmp "$TMP/narrow.png" "$TMP/wide-$STREAM-$COMPACT.png"
done
done
if grep -q 'Validation Error' "$TMP/"*.log; then cat "$TMP/"*.log;exit 1;fi
python3 - "$ROOT/tools/lusdrender" "$TMP/narrow.png" <<'PYCODE'
import sys
sys.path.insert(0,sys.argv[1])
from check_lusdrender_smoke import read_png_rgba
w,h,pixels=read_png_rgba(sys.argv[2])
hits=sum(any(pixels[i:i+3])for i in range(0,len(pixels),4))
assert w==64 and h==64 and 100<hits<w*h,(w,h,hits)
PYCODE
echo 'PASS: reclaimed BLAS inputs preserve narrow/wide/multi-TLAS captures with and without compaction'
