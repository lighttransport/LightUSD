#!/usr/bin/env bash
set -euo pipefail

SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"
MOTION_BACKEND="${LUSDVIEW_MOTION_BACKEND:-cpu}"
case "$MOTION_BACKEND" in
  cpu) BACKEND_ARGS=(--cpu-rt); BACKEND_LABEL=CPU ;;
  cuda) BACKEND_ARGS=(--cuda); BACKEND_LABEL=CUDA ;;
  hip) BACKEND_ARGS=(--hip); BACKEND_LABEL=HIP ;;
  *) echo "FAIL: unknown motion backend '$MOTION_BACKEND'"; exit 2 ;;
esac

write_scene() {
  local path="$1" open="$2" close="$3"
  cat >"$path" <<USDA
#usda 1.0
(
  defaultPrim = "World"
  startTimeCode = 0
  endTimeCode = 2
)
def Xform "World" {
  def Mesh "MovingQuad" {
    double3 xformOp:translate.timeSamples = {
      0: (-2, 0, 0),
      2: (2, 0, 0)
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    color3f[] primvars:displayColor = [(0.9, 0.15, 0.05)] (
      interpolation = "constant"
    )
  }
  def Camera "Shot" {
    float focalLength = 50
    float horizontalAperture = 25
    float verticalAperture = 18
    double shutter:open = $open
    double shutter:close = $close
    double3 xformOp:translate = (0, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA
}

write_scene "$OUT/closed.usda" 0 0
write_scene "$OUT/open.usda" -0.5 0.5
cat >"$OUT/camera-open.usda" <<'USDA'
#usda 1.0
(
  defaultPrim = "World"
  startTimeCode = 0
  endTimeCode = 2
)
def Xform "World" {
  def Mesh "StaticQuad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    color3f[] primvars:displayColor = [(0.9, 0.15, 0.05)] (
      interpolation = "constant"
    )
  }
  def Camera "Shot" {
    float focalLength = 50
    float horizontalAperture = 25
    float verticalAperture = 18
    double shutter:open = -0.5
    double shutter:close = 0.5
    double3 xformOp:translate.timeSamples = {
      0: (-2, 0, 8),
      2: (2, 0, 8)
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA

run_capture() {
  local scene="$1" image="$2" log="$3"
  if ! env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next --headless \
      "${BACKEND_ARGS[@]}" --frames 1 --size 96x64 --camera Shot --time 1 \
      --screenshot "$image" "$scene" >"$log" 2>&1; then
    if [ "$MOTION_BACKEND" != cpu ] &&
        grep -Eqi '(CUDA|HIP).*(unavailable|failed)|no (CUDA|HIP)' "$log"; then
      echo "SKIP: $BACKEND_LABEL unavailable"
      exit "$SKIP"
    fi
    cat "$log"
    exit 1
  fi
}

run_capture "$OUT/closed.usda" "$OUT/closed.ppm" "$OUT/closed.log"
run_capture "$OUT/open.usda" "$OUT/open.ppm" "$OUT/open.log"
run_capture "$OUT/camera-open.usda" "$OUT/camera-open.ppm" \
  "$OUT/camera-open.log"
if [ "$MOTION_BACKEND" != cpu ] &&
    grep -Eqi '(CUDA|HIP).*(unavailable|failed)|no (CUDA|HIP)' \
      "$OUT/open.log"; then
  echo "SKIP: $BACKEND_LABEL unavailable"
  exit "$SKIP"
fi
grep -q "$BACKEND_LABEL motion blur: 2 shutter samples" "$OUT/open.log" || {
  cat "$OUT/open.log"
  echo "FAIL: CPU shutter sampling was not reported"
  exit 1
}
grep -q "$BACKEND_LABEL motion blur: 2 shutter samples" \
  "$OUT/camera-open.log" || {
  cat "$OUT/camera-open.log"
  echo "FAIL: animated-camera shutter sampling was not reported"
  exit 1
}

python3 - "$OUT/closed.ppm" "$OUT/open.ppm" "$OUT/camera-open.ppm" <<'PY'
import pathlib, sys

def ppm(path):
    data = pathlib.Path(path).read_bytes()
    header, pixels = data.split(b"\n255\n", 1)
    if not header.startswith(b"P6\n96 64"):
        raise SystemExit(f"FAIL: unexpected PPM header in {path}: {header!r}")
    return pixels

closed, geometry, camera = map(ppm, sys.argv[1:])
def compare(label, image):
    if closed == image:
        raise SystemExit(f"FAIL: {label} open shutter equals closed shutter")
    mad = sum(abs(a - b) for a, b in zip(closed, image)) / len(closed)
    if mad < 0.5:
        raise SystemExit(f"FAIL: {label} motion difference too small (MAD {mad:.3f})")
    return mad
geometry_mad = compare("geometry", geometry)
camera_mad = compare("camera", camera)
print("PASS: authored-shutter motion blur "
      f"(geometry MAD {geometry_mad:.3f}, camera MAD {camera_mad:.3f})")
PY

if [ "$MOTION_BACKEND" != cpu ]; then
  if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "INFO: $BACKEND_LABEL interactive shutter leg skipped (xvfb-run unavailable)"
    exit 0
  fi
  if xvfb-run -a env XDG_CONFIG_HOME="$OUT/config-home-interactive" \
      "$BIN" --next --backend gl --frames 240 --size 320x240 \
      --camera Shot --time 1 "${BACKEND_ARGS[@]}" --path-trace \
      --pt-samples 4 --pt-motion-segments 2 \
      --render-report "$OUT/interactive.json" "$OUT/open.usda" \
      >"$OUT/interactive.log" 2>&1; then
    python3 - "$OUT/interactive.json" "$BACKEND_LABEL" <<'PY'
import json, sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
backend = sys.argv[2].lower()
key = f"{backend}_interactive_segments_sampled"
sampled = report.get("camera_shutter", {}).get(key, 0)
if sampled != 2:
    raise SystemExit(
        f"FAIL: {sys.argv[2]} interactive accumulator sampled {sampled}/2 shutter segments")
scene_key = f"{backend}_interactive_scene_segments_sampled"
scene_sampled = report.get("camera_shutter", {}).get(scene_key, 0)
if scene_sampled != 2:
    raise SystemExit(
        f"FAIL: {sys.argv[2]} interactive scene sampled {scene_sampled}/2 shutter segments")
print(f"PASS: {sys.argv[2]} interactive camera and scene cycled both shutter segments")
PY
  elif grep -Eqi 'OpenGL.*(unavailable|failed)|GLFW.*failed|failed to open display|xvfb-run: error' \
      "$OUT/interactive.log"; then
    echo "INFO: $BACKEND_LABEL interactive shutter leg skipped (no usable X/GL context)"
  else
    cat "$OUT/interactive.log"
    echo "FAIL: $BACKEND_LABEL interactive shutter regression failed"
    exit 1
  fi
fi
