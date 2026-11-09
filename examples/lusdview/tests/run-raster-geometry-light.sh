#!/usr/bin/env bash
# Verify resolved GeometryLight mesh samples in both raster backends.
set -uo pipefail

LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root}"
SKIP=77
TMP="$(mktemp -d /tmp/lusdview-raster-geometry-light.XXXXXX)"
cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT
asset="$ROOT/tests/usda/lusdview-raster-geometry-light.usda"

vk_log="$(LUSDVIEW_DEBUG_LIGHTS=1 timeout 45s "$LUSDVIEW" --headless \
  --backend vk --camera /World/Camera --frames 3 --size 128x128 \
  --screenshot "$TMP/vk.ppm" "$asset" 2>&1)"
vk_rc=$?
if [ "$vk_rc" -ne 0 ]; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan|renderer init failed' \
      <<<"$vk_log"; then
    echo "SKIP: Vulkan backend unavailable"
    exit "$SKIP"
  fi
  echo "$vk_log"
  echo "FAIL: Vulkan GeometryLight raster render failed"
  exit 1
fi
if ! grep -q 'type=7' <<<"$vk_log" ||
   grep -q 'omitted .*GeometryLight' <<<"$vk_log"; then
  echo "$vk_log"
  echo "FAIL: resolved GeometryLight was not packed for raster sampling"
  exit 1
fi

xvfb_server=50
for candidate in $(seq 50 89); do
  if [ ! -S "/tmp/.X11-unix/X$candidate" ] &&
     [ ! -e "/tmp/.X${candidate}-lock" ]; then
    xvfb_server=$candidate
    break
  fi
done
gl_log="$(xvfb-run --server-num="$xvfb_server" --error-file="$TMP/xvfb.log" \
  -s '-screen 0 640x480x24' timeout 45s "$LUSDVIEW" \
  --backend gl --camera /World/Camera --frames 3 --size 128x128 \
  --screenshot "$TMP/gl.ppm" "$asset" 2>&1)"
gl_rc=$?
if [ "$gl_rc" -ne 0 ]; then
  if [ "$gl_rc" -eq 124 ] || [ "$gl_rc" -eq 137 ] ||
     grep -Eqi 'OpenGL.*(unavailable|failed)|GLFW.*failed|failed to open display' \
       <<<"$gl_log"; then
    echo "SKIP: OpenGL/X11 backend unavailable"
    exit "$SKIP"
  fi
  echo "$gl_log"
  echo "FAIL: OpenGL GeometryLight raster render failed"
  exit 1
fi

python3 - "$TMP/gl.ppm" "$TMP/vk.ppm" <<'PY'
import sys

def pixels(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise SystemExit(f"FAIL: {path} is not binary PPM")
        width, height = map(int, f.readline().split())
        if int(f.readline()) != 255:
            raise SystemExit(f"FAIL: {path} has an unexpected range")
        data = f.read()
    if len(data) != width * height * 3:
        raise SystemExit(f"FAIL: {path} is truncated")
    return [tuple(data[i:i + 3]) for i in range(0, len(data), 3)]

def signature(path):
    image = pixels(path)
    warm = [p for p in image if p[0] - p[2] >= 25 and p[0] >= 120]
    if len(warm) < 1000:
        raise SystemExit(
            f"FAIL: {path} has only {len(warm)} GeometryLight-lit pixels")
    return tuple(sum(p[c] for p in warm) / len(warm) for c in range(3))

gl = signature(sys.argv[1])
vk = signature(sys.argv[2])
error = max(abs(gl[c] - vk[c]) for c in range(3))
if error > 5:
    raise SystemExit(
        f"FAIL: GL/VK GeometryLight response differs by {error:.1f} levels "
        f"({gl} vs {vk})")
print("PASS: raster GeometryLight mesh sampling", gl, vk)
PY
