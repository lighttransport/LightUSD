#!/usr/bin/env bash
#
# Regression test: -frames FRAMESPEC must be bounded.
#
# ParseFrameSpec used to loop `for (t=start; t<=end; t+=stride)` with no cap on
# the frame count and only an `==0` guard on the stride, so:
#   -frames 0:100000000      enumerated 100M frames  -> OOM / effectively hangs
#   -frames 0:1000x1e-320    `t += tiny` never progressed at large t -> infinite loop
# Both must now be rejected as invalid (fast, non-zero exit), while an ordinary
# small spec still renders one numbered image per frame.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ASSET="$TMP/tri.usda"
cat > "$ASSET" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Tri"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
    }
}
USDA

fail() { echo "FAIL: $1"; exit 1; }

# GNU coreutils `timeout` is absent on stock macOS runners. Use Python's
# standard subprocess timeout as a portable fallback while preserving status
# 124 for an actual timeout.
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

# 1. A huge range must be rejected quickly (not enumerated). Kill after 20s: if
#    the guard is missing this either OOMs or runs far past the timeout.
run_timeout 20 "$LUSDRENDER" "$ASSET" "$TMP/big.####.png" -rtPreview \
    -frames 0:100000000 -w 16 -height 16 -samples 1 > "$TMP/big.log" 2>&1
rc=$?
if [ $rc -eq 124 ]; then
  fail "-frames 0:100000000 did not terminate (timed out) -- frame count is unbounded"
fi
if [ $rc -eq 0 ]; then
  fail "-frames 0:100000000 was accepted -- should be rejected as invalid"
fi
grep -qi "invalid -frames" "$TMP/big.log" \
  || fail "-frames 0:100000000 failed without the expected 'Invalid -frames' diagnostic"

# 2. A sub-ULP stride at a large offset must be rejected (non-progressing loop).
run_timeout 20 "$LUSDRENDER" "$ASSET" "$TMP/tiny.####.png" -rtPreview \
    -frames "0:1000x1e-320" -w 16 -height 16 -samples 1 > "$TMP/tiny.log" 2>&1
rc=$?
if [ $rc -eq 124 ]; then
  fail "-frames 0:1000x1e-320 did not terminate -- stride too small to progress"
fi
if [ $rc -eq 0 ]; then
  fail "-frames 0:1000x1e-320 was accepted -- should be rejected as invalid"
fi

# 3. An ordinary small spec must still render exactly one image per frame.
run_timeout 120 "$LUSDRENDER" "$ASSET" "$TMP/ok.####.png" -rtPreview \
    -frames 1:3 -w 16 -height 16 -samples 1 -autoframe > "$TMP/ok.log" 2>&1 \
    || fail "-frames 1:3 failed: $(cat "$TMP/ok.log")"
n=$(ls "$TMP"/ok.*.png 2>/dev/null | wc -l)
[ "$n" -eq 3 ] || fail "-frames 1:3 wrote $n images, expected 3"

echo "PASS: -frames rejects unbounded/degenerate specs and renders bounded ones"
exit 0
