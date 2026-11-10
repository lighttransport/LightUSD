#!/usr/bin/env bash
# Prove linked multi-light accumulation in GL and Vulkan raster. The fixture's
# left panel receives red, center receives red+blue, and right receives blue.
set -uo pipefail

LUSDVIEW="${1:?usage: $0 /path/to/lusdview /repo/root}"
ROOT="${2:?usage: $0 /path/to/lusdview /repo/root}"
SKIP=77
TMP="$(mktemp -d /tmp/lusdview-raster-multilight.XXXXXX)"
xvfb_pid=""
cleanup() {
  if [ -n "$xvfb_pid" ]; then kill "$xvfb_pid" 2>/dev/null || true; fi
  rm -rf "$TMP"
}
trap cleanup EXIT
printf '%s\n' '{"window_size":{"width":640,"height":480}}' >"$TMP/config.json"
asset="$ROOT/tests/usda/lusdview-raster-multilight-links.usda"

use_existing_display=0
if [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1 &&
   xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
  use_existing_display=1
fi
if [ "$use_existing_display" -eq 0 ] && command -v Xvfb >/dev/null 2>&1; then
  # X11 display numbers are one byte in several client paths; stay below 256.
  display_num=$((200 + ($$ % 50)))
  Xvfb ":$display_num" -screen 0 800x600x24 >"$TMP/xvfb.log" 2>&1 &
  xvfb_pid=$!
  export DISPLAY=":$display_num"
  for _ in 1 2 3 4 5; do
    kill -0 "$xvfb_pid" 2>/dev/null || break
    [ -S "/tmp/.X11-unix/X$display_num" ] && break
    sleep 0.1
  done
fi
gl_log="$(timeout --kill-after=5s "${LUSDVIEW_GL_TIMEOUT:-20s}" "$LUSDVIEW" \
  --config "$TMP/config.json" --backend gl --camera /World/Camera \
  --frames 3 --screenshot "$TMP/gl.ppm" "$asset" 2>&1)"
gl_rc=$?
if [ "$gl_rc" -ne 0 ]; then
  # Some NVIDIA GLX/Xvfb combinations block inside window/context creation
  # without producing a GLFW diagnostic. Treat the bounded startup timeout as
  # an unavailable OpenGL environment, consistent with explicit init errors.
  if [ "$gl_rc" -eq 124 ] || [ "$gl_rc" -eq 137 ]; then
    echo "SKIP: OpenGL/X11 backend timed out during initialization"
    exit "$SKIP"
  fi
  if grep -Eqi "OpenGL.*(unavailable|failed)|GLFW.*failed|failed to open display|DISPLAY|xvfb-run: error" <<<"$gl_log"; then
    echo "SKIP: OpenGL/X11 backend unavailable"
    exit "$SKIP"
  fi
  echo "$gl_log"; echo "FAIL: OpenGL linked-light render failed"; exit 1
fi

vk_log="$(timeout 45s "$LUSDVIEW" --config "$TMP/config.json" --backend vk \
  --headless --camera /World/Camera --frames 3 --screenshot "$TMP/vk.ppm" \
  "$asset" 2>&1)"
vk_rc=$?
if [ "$vk_rc" -ne 0 ]; then
  if grep -Eqi "Vulkan.*(unavailable|failed)|no Vulkan|renderer init failed" <<<"$vk_log"; then
    echo "SKIP: Vulkan backend unavailable"
    exit "$SKIP"
  fi
  echo "$vk_log"; echo "FAIL: Vulkan linked-light render failed"; exit 1
fi

python3 - "$TMP/gl.ppm" "$TMP/vk.ppm" <<'PY'
import sys

def read_ppm(path):
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

def classes(path):
    groups = {"red": [], "blue": [], "magenta": []}
    for r, g, b in read_ppm(path):
        if r > 1.4 * g and r > 1.4 * b:
            groups["red"].append((r, g, b))
        elif b > 1.4 * r and b > 1.4 * g:
            groups["blue"].append((r, g, b))
        elif r > 1.4 * g and b > 1.4 * g:
            groups["magenta"].append((r, g, b))
    means = {}
    for name, values in groups.items():
        if len(values) < 1000:
            raise SystemExit(
                f"FAIL: {path} has only {len(values)} {name} pixels; "
                "multi-light/link rendering is missing"
            )
        means[name] = tuple(sum(v[c] for v in values) / len(values)
                            for c in range(3))
    return {name: len(values) for name, values in groups.items()}, means

gl_counts, gl_means = classes(sys.argv[1])
vk_counts, vk_means = classes(sys.argv[2])
for name in gl_means:
    error = max(abs(gl_means[name][c] - vk_means[name][c]) for c in range(3))
    if error > 12:
        raise SystemExit(
            f"FAIL: GL/VK {name} response differs by {error:.1f} levels "
            f"({gl_means[name]} vs {vk_means[name]})"
        )
print("PASS: linked raster multi-light", gl_counts, vk_counts )
PY
