#!/usr/bin/env bash
set -uo pipefail
LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root}"
SKIP=77
TMP="$(mktemp -d /tmp/lusdview-vk-gaussian-chunks.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

log="$(timeout 90s env LUSDVIEW_GAUSSIAN_CHUNK=4 "$LUSDVIEW" \
  --headless --backend vk --rt --frames 2 \
  --screenshot "$TMP/gaussian.ppm" \
  "$ROOT/tests/usda/lusdview-gaussian-chunks.usda" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan|renderer init failed' <<<"$log"; then
    echo "SKIP: Vulkan backend unavailable"
    exit "$SKIP"
  fi
  echo "$log"
  echo "FAIL: Vulkan Gaussian chunk render failed"
  exit 1
fi
echo "$log"
if grep -Eq 'caps: v1 .*rt=software|pipeline cache is cold' <<<"$log"; then
  echo "SKIP: Vulkan hardware RT pipeline cache is cold"
  exit "$SKIP"
fi
if ! grep -Eq 'Gaussian point BVH: 12 point\(s\) in 3 chunk\(s\)' <<<"$log"; then
  echo "FAIL: expected three Gaussian chunks"
  exit 1
fi
if ! grep -q 'render stats' <<<"$log"; then
  echo "SKIP: Vulkan renderer unavailable"
  exit "$SKIP"
fi
if [ ! -s "$TMP/gaussian.ppm" ]; then
  echo "FAIL: no Gaussian screenshot written"
  exit 1
fi
python3 - "$TMP/gaussian.ppm" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
if not data.startswith(b"P6"):
    raise SystemExit("FAIL: expected binary PPM")
parts = data.split(None, 4)
if len(parts) != 5:
    raise SystemExit("FAIL: malformed PPM")
w, h, maxval = map(int, parts[1:4])
pixels = parts[4]
if len(pixels) < w * h * 3:
    raise SystemExit("FAIL: truncated Gaussian screenshot")
colored = sum(max(pixels[i:i+3]) - min(pixels[i:i+3]) > 40
              for i in range(0, w * h * 3, 3))
if colored < 200:
    raise SystemExit(f"FAIL: Gaussian chunks rendered blank ({colored} colored pixels)")
print(f"PASS: Gaussian chunks rendered ({colored} colored pixels)")
PY

# Half-precision schema aliases must reach the same analytic point path.
half_log="$(timeout 90s "$LUSDVIEW" --core --headless --backend vk --rt \
  --frames 3 --screenshot "$TMP/gaussian-half.ppm" \
  "$ROOT/tests/usda/lusdview-gaussian-half.usda" 2>&1)"
half_rc=$?
if [ "$half_rc" -ne 0 ]; then
  echo "$half_log"
  echo "FAIL: half-precision Gaussian render failed"
  exit 1
fi
echo "$half_log"
if ! grep -q 'Gaussian point BVH: 2 point(s) in 1 chunk(s)' <<<"$half_log"; then
  echo "FAIL: half-precision Gaussian fields did not use the analytic path"
  exit 1
fi

# An ordinary Points-only scene has no triangle TLAS. RT mode must draw its
# carrier fallback immediately instead of clearing forever while waiting for a
# TLAS that will never be built.
points_log="$(timeout 90s "$LUSDVIEW" --core --headless --backend vk --rt \
  --frames 3 --screenshot "$TMP/points.ppm" \
  "$ROOT/tests/usda/lusdrender-points.usda" 2>&1)"
points_rc=$?
if [ "$points_rc" -ne 0 ]; then
  echo "$points_log"
  echo "FAIL: Points-only Vulkan RT render failed"
  exit 1
fi
echo "$points_log"
python3 - "$TMP/points.ppm" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
parts = data.split(None, 4)
if len(parts) != 5 or parts[0] != b"P6":
    raise SystemExit("FAIL: malformed Points PPM")
w, h, maxval = map(int, parts[1:4])
pixels = parts[4][:w * h * 3]
if len(pixels) != w * h * 3:
    raise SystemExit("FAIL: truncated Points PPM")
background = pixels[:3]
changed = sum(pixels[i:i+3] != background
              for i in range(0, len(pixels), 3))
if changed < 100:
    raise SystemExit(f"FAIL: Points-only RT frame is blank ({changed} changed pixels)")
print(f"PASS: Points-only RT carrier rendered ({changed} changed pixels)")
PY
