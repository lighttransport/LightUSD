#!/usr/bin/env bash
set -euo pipefail

SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"

cat >"$OUT/stereo.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
  }
  def Xform "Rig" {
    def Camera "Left" {
      uniform token stereoRole = "left"
      float focalLength = 50
      double3 xformOp:translate = (-0.8, 0, 8)
      uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Camera "Right" {
      uniform token stereoRole = "right"
      float focalLength = 50
      double3 xformOp:translate = (0.8, 0, 8)
      uniform token[] xformOpOrder = ["xformOp:translate"]
    }
  }
}
USDA

run_report() {
  local mode="$1" report="$2" shot="$3" log="$4"
  local args=()
  [ "$mode" = next ] && args+=(--next)
  if ! env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" "${args[@]}" \
      --headless --backend vk --frames 1 --size 64x48 --stereo \
      --camera /World/Rig/Left --render-report "$report" \
      --screenshot "$shot" \
      "$OUT/stereo.usda" >"$log" 2>&1; then
    if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$log"; then
      echo "SKIP: Vulkan unavailable"
      exit "$SKIP"
    fi
    cat "$log"
    exit 1
  fi
}

run_report legacy "$OUT/legacy.json" "$OUT/legacy.ppm" "$OUT/legacy.log"
run_report next "$OUT/next.json" "$OUT/next.ppm" "$OUT/next.log"

python3 - "$OUT/legacy.json" "$OUT/next.json" \
  "$OUT/legacy.ppm" "$OUT/next.ppm" <<'PY'
import json, pathlib, sys

reports = [json.load(open(path, encoding="utf-8")) for path in sys.argv[1:3]]
for label, report in zip(("legacy", "next"), reports):
    stereo = report.get("stereo", {})
    if stereo.get("requested") is not True or stereo.get("resolved") is not True:
        raise SystemExit(f"FAIL: {label} stereo pair did not resolve: {stereo}")
    if stereo.get("left") != "/World/Rig/Left" or stereo.get("right") != "/World/Rig/Right":
        raise SystemExit(f"FAIL: {label} resolved the wrong pair: {stereo}")
    if stereo.get("rendering") != "side-by-side-capture":
        raise SystemExit(f"FAIL: {label} native stereo capture missing: {stereo}")
if reports[0]["stereo"] != reports[1]["stereo"]:
    raise SystemExit("FAIL: legacy/next stereo records differ")
for label, report, path in zip(("legacy", "next"), reports, sys.argv[3:5]):
    data = pathlib.Path(path).read_bytes()
    header, pixels = data.split(b"\n255\n", 1)
    magic, dims = header.splitlines()
    width, height = map(int, dims.split())
    if magic != b"P6" or width != 128 or len(pixels) != width * height * 3:
        raise SystemExit(f"FAIL: {label} native stereo dimensions are wrong")
    resolution = report.get("resolution", {})
    if resolution.get("width") != width or resolution.get("height") != height:
        raise SystemExit(f"FAIL: {label} report/capture dimensions differ")
    left = bytearray()
    right = bytearray()
    stride = width * 3
    eye = (width // 2) * 3
    for y in range(height):
        row = pixels[y * stride:(y + 1) * stride]
        left += row[:eye]
        right += row[eye:]
    if left == right:
        raise SystemExit(f"FAIL: {label} native stereo eyes are identical")
print("PASS: legacy/next native stereo capture contract")
PY

if command -v xvfb-run >/dev/null 2>&1; then
  if xvfb-run -a env \
      XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next \
      --backend gl --frames 1 --size 64x48 --stereo \
      --camera /World/Rig/Left --render-report "$OUT/gl.json" \
      --screenshot "$OUT/gl.ppm" "$OUT/stereo.usda" \
      >"$OUT/gl.log" 2>&1; then
    python3 - "$OUT/gl.json" "$OUT/gl.ppm" <<'PY'
import json, pathlib, sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
if report.get("backend", {}).get("name") != "OpenGL":
    raise SystemExit("FAIL: OpenGL stereo leg selected the wrong backend")
if report.get("stereo", {}).get("rendering") != "side-by-side-capture":
    raise SystemExit("FAIL: OpenGL native stereo capture was not reported")
if report.get("stereo", {}).get("live_viewport") is not True:
    raise SystemExit("FAIL: OpenGL live stereo viewport was not composed")
data = pathlib.Path(sys.argv[2]).read_bytes()
header, pixels = data.split(b"\n255\n", 1)
width, height = map(int, header.splitlines()[1].split())
if width != 128 or len(pixels) != width * height * 3:
    raise SystemExit("FAIL: OpenGL native stereo dimensions are wrong")
stride, eye = width * 3, (width // 2) * 3
left = b"".join(pixels[y * stride:y * stride + eye] for y in range(height))
right = b"".join(pixels[y * stride + eye:(y + 1) * stride] for y in range(height))
if left == right:
    raise SystemExit("FAIL: OpenGL native stereo eyes are identical")
print("PASS: OpenGL native stereo capture")
PY
  elif grep -Eqi 'OpenGL.*(unavailable|failed)|GLFW.*failed|failed to open display|xvfb-run: error' "$OUT/gl.log"; then
    echo "INFO: OpenGL stereo leg skipped because no usable X/GL context is available"
  else
    cat "$OUT/gl.log"
    exit 1
  fi

  if xvfb-run -a env \
      XDG_CONFIG_HOME="$OUT/config-home" "$BIN" \
      --legacy-load --backend gl --frames 1 --size 64x48 --stereo \
      --camera /World/Rig/Left --render-report "$OUT/gl-legacy.json" \
      --screenshot "$OUT/gl-legacy.ppm" "$OUT/stereo.usda" \
      >"$OUT/gl-legacy.log" 2>&1; then
    python3 - "$OUT/gl-legacy.json" "$OUT/gl-legacy.ppm" <<'PY'
import json, pathlib, sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
if report.get("stereo", {}).get("live_viewport") is not True:
    raise SystemExit("FAIL: legacy OpenGL live stereo viewport was not composed")
data = pathlib.Path(sys.argv[2]).read_bytes()
header, pixels = data.split(b"\n255\n", 1)
width, height = map(int, header.splitlines()[1].split())
if width != 128 or len(pixels) != width * height * 3:
    raise SystemExit("FAIL: legacy OpenGL live stereo dimensions are wrong")
print("PASS: legacy OpenGL live stereo viewport")
PY
  elif grep -Eqi 'OpenGL.*(unavailable|failed)|GLFW.*failed|failed to open display|xvfb-run: error' "$OUT/gl-legacy.log"; then
    echo "INFO: legacy OpenGL stereo leg skipped because no usable X/GL context is available"
  else
    cat "$OUT/gl-legacy.log"
    exit 1
  fi

  if xvfb-run -a env \
      XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next \
      --backend vk --frames 1 --size 64x48 --stereo \
      --camera /World/Rig/Left --render-report "$OUT/vk-live.json" \
      --screenshot "$OUT/vk-live.ppm" "$OUT/stereo.usda" \
      >"$OUT/vk-live.log" 2>&1; then
    python3 - "$OUT/vk-live.json" "$OUT/vk-live.ppm" <<'PY'
import json, pathlib, sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
if report.get("stereo", {}).get("live_viewport") is not True:
    raise SystemExit("FAIL: Vulkan live stereo viewport was not composed")
data = pathlib.Path(sys.argv[2]).read_bytes()
header, pixels = data.split(b"\n255\n", 1)
width, height = map(int, header.splitlines()[1].split())
if width != 128 or len(pixels) != width * height * 3:
    raise SystemExit("FAIL: Vulkan live stereo dimensions are wrong")
print("PASS: Vulkan live stereo viewport")
PY
  elif grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan|failed to open display|xvfb-run: error' "$OUT/vk-live.log"; then
    echo "INFO: Vulkan live stereo leg skipped because no usable window surface is available"
  else
    cat "$OUT/vk-live.log"
    exit 1
  fi
else
  echo "INFO: OpenGL stereo leg skipped because xvfb-run is unavailable"
fi

python3 "$ROOT/examples/lusdview/render-stereo.py" \
  --lusdview "$BIN" --next --backend vk --viewer-arg=--cpu-rt \
  --size 48x32 --output "$OUT/stereo.ppm" "$OUT/stereo.usda"
python3 - "$OUT/stereo.ppm" <<'PY'
import pathlib, sys
data = pathlib.Path(sys.argv[1]).read_bytes()
header, pixels = data.split(b"\n255\n", 1)
if header != b"P6\n96 32" or len(pixels) != 96 * 32 * 3:
    raise SystemExit("FAIL: unexpected side-by-side PPM dimensions")
left = bytearray()
right = bytearray()
stride = 96 * 3
eye = 48 * 3
for y in range(32):
    row = pixels[y * stride:(y + 1) * stride]
    left += row[:eye]
    right += row[eye:]
if left == right:
    raise SystemExit("FAIL: stereo left/right images are identical")
print("PASS: side-by-side stereo export")
PY
