#!/usr/bin/env bash
# --gsplat-kernel cycles on the CPU tracer: splats in front of a triangle must
# be visible (saturated red/blue), and splats behind it must be occluded
# (no red: the red splat lies entirely behind the triangle).
set -uo pipefail
LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lusdview-gsplat-cycles.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
for v in front back; do
  log="$(timeout 120s "$LUSDVIEW" --headless --size 240x135 \
    --gsplat-kernel cycles --rt-samples 16 \
    --screenshot "$TMP/$v.ppm" \
    "$ROOT/tests/usda/lusdview-gaussian-cycles-$v.usda" 2>&1)"
  rc=$?
  if [ "$rc" -ne 0 ] || [ ! -s "$TMP/$v.ppm" ]; then
    echo "$log"
    echo "FAIL: lusdview CPU Cycles splat render ($v) failed"
    exit 1
  fi
  if ! grep -q 'Cycles gsplat kernel, 2 splat(s)' <<<"$log"; then
    echo "$log"
    echo "FAIL: Cycles splat kernel was not used ($v)"
    exit 1
  fi
done
python3 -I - "$TMP/front.ppm" "$TMP/back.ppm" <<'PY'
import sys
def load(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos+1].isspace(): pos += 1
        if data[pos:pos+1] == b"#":
            pos = data.index(b"\n", pos) + 1; continue
        end = pos
        while not data[end:end+1].isspace(): end += 1
        parts.append(data[pos:end]); pos = end
    w, h = int(parts[1]), int(parts[2]); pos += 1
    px = data[pos:pos + w * h * 3]
    return [tuple(px[i:i+3]) for i in range(0, len(px), 3)]
red = lambda p: p[0] > 150 and p[1] < 80 and p[2] < 80
blue = lambda p: p[2] > 150 and p[0] < 80 and p[1] < 80
front, back = load(sys.argv[1]), load(sys.argv[2])
fr, fb = sum(map(red, front)), sum(map(blue, front))
br = sum(map(red, back))
print("front red %d blue %d, back red %d" % (fr, fb, br))
if fr < 20 or fb < 20:
    sys.exit("FAIL: splats in front of the mesh are not visible")
if br > 2:
    sys.exit("FAIL: splat behind the mesh is not occluded")
PY
