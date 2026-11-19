#!/usr/bin/env bash
# --gsplat-kernel cycles: splats in front of a triangle must be visible
# (saturated red/blue), and splats behind it must be occluded (no red: the red
# splat lies entirely behind the triangle).
#   run-cpu-gsplat-cycles.sh LUSDVIEW REPO_ROOT [cpu|rt|hip|cuda]
# GPU backends exit 77 (skip) when they did not run the Cycles kernel (no
# device / runtime).
set -uo pipefail
LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root [cpu|rt|hip|cuda]}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root [cpu|rt|hip|cuda]}"
BACKEND="${3:-cpu}"
case "$BACKEND" in
  cpu)  FLAGS=(); MARK='CPU RT: Cycles gsplat kernel, 2 splat(s)' ;;
  # The ray-query pipeline is required (the compute-BVH fallback has no
  # Cycles path), so allow its first compile.
  # Vulkan RT averages samples across frames (accumulation), not --rt-samples.
  rt)   FLAGS=(--rt --frames 32); MARK='Vulkan RT: Cycles gsplat kernel, 2 splat(s)'
        export LUSDVIEW_RT_ALLOW_COLD_COMPILE=1 ;;
  hip)  FLAGS=(--hip); MARK='HIP Cycles gsplat kernel: 2 splat(s)' ;;
  cuda) FLAGS=(--cuda); MARK='CUDA Cycles gsplat kernel: 2 splat(s)' ;;
  *) echo "unknown backend $BACKEND"; exit 2 ;;
esac
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lusdview-gsplat-cycles.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
for v in front back; do
  log="$(timeout 300s "$LUSDVIEW" --headless --no-grid --size 240x135 \
    "${FLAGS[@]}" --gsplat-kernel cycles --rt-samples 16 \
    --screenshot "$TMP/$v.ppm" \
    "$ROOT/tests/usda/lusdview-gaussian-cycles-$v.usda" 2>&1)"
  rc=$?
  if ! grep -qF "$MARK" <<<"$log"; then
    if [ "$BACKEND" != cpu ]; then
      echo "SKIP: $BACKEND did not run the Cycles splat kernel"
      exit 77
    fi
    echo "$log"
    echo "FAIL: Cycles splat kernel was not used ($v)"
    exit 1
  fi
  if [ "$rc" -ne 0 ] || [ ! -s "$TMP/$v.ppm" ]; then
    echo "$log"
    echo "FAIL: lusdview $BACKEND Cycles splat render ($v) failed"
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
    return w, [tuple(px[i:i+3]) for i in range(0, len(px), 3)]
red = lambda p: p[0] > 150 and p[1] < 80 and p[2] < 80
blue = lambda p: p[2] > 150 and p[0] < 80 and p[1] < 80
# The red splat projects left of the world origin; restrict red counts to that
# region so the Vulkan viewport's red X-axis gizmo does not count.
def red_region(w, px):
    return sum(1 for i, p in enumerate(px)
               if red(p) and 70 <= i % w < 115 and 50 <= i // w < 90)
(w, front), (_, back) = load(sys.argv[1]), load(sys.argv[2])
fr, fb = red_region(w, front), sum(map(blue, front))
br = red_region(w, back)
print("front red %d blue %d, back red %d" % (fr, fb, br))
if fr < 20 or fb < 20:
    sys.exit("FAIL: splats in front of the mesh are not visible")
if br > 2:
    sys.exit("FAIL: splat behind the mesh is not occluded")
PY
