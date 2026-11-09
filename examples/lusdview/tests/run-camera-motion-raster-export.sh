#!/usr/bin/env bash
set -euo pipefail

SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
BACKEND="${3:-vk}"
LOADER_MODE="${4:-next}"
case "$BACKEND" in
  gl) DIRECT_WINDOW_ARGS=(); TEST_SIZE=320x240 ;;
  vk) DIRECT_WINDOW_ARGS=(--headless); TEST_SIZE=64x64 ;;
  *) echo "FAIL: backend must be gl or vk"; exit 1 ;;
esac
case "$LOADER_MODE" in
  next) LOADER_ARGS=(--next); COMPANION_LOADER_ARGS=(--next) ;;
  legacy) LOADER_ARGS=(--legacy-load); COMPANION_LOADER_ARGS=(--viewer-arg=--legacy-load) ;;
  *) echo "FAIL: loader must be next or legacy"; exit 1 ;;
esac
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"

write_scene() {
  local path="$1" open="$2" close="$3"
  local mesh_xform='double3 xformOp:translate.timeSamples = { 0: (-2, 0, 0), 2: (2, 0, 0) }'
  if [ "$LOADER_MODE" = legacy ]; then
    mesh_xform='double3 xformOp:translate = (0, 0, 0)'
  fi
  cat >"$path" <<USDA
#usda 1.0
(defaultPrim = "World" startTimeCode = 0 endTimeCode = 2)
def Xform "World" {
  def Mesh "MovingQuad" {
    $mesh_xform
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
    double3 xformOp:translate.timeSamples = { 0: (-1, 0, 8), 2: (1, 0, 8) }
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA
}
write_scene "$OUT/closed.usda" 0 0
write_scene "$OUT/open.usda" -0.5 0.5

if ! env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" "${LOADER_ARGS[@]}" \
    "${DIRECT_WINDOW_ARGS[@]}" --backend "$BACKEND" --frames 1 --size "$TEST_SIZE" \
    --camera Shot --time 1 \
    --screenshot "$OUT/closed.ppm" "$OUT/closed.usda" \
    >"$OUT/closed.log" 2>&1; then
  if [ "$BACKEND" = vk ] &&
      grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/closed.log"; then
    echo "SKIP: Vulkan unavailable"; exit "$SKIP"
  fi
  cat "$OUT/closed.log"; exit 1
fi

python3 "$ROOT/examples/lusdview/render-motion.py" \
  --lusdview "$BIN" "${COMPANION_LOADER_ARGS[@]}" --backend "$BACKEND" --camera Shot --time 1 \
  --segments 2 --size "$TEST_SIZE" --output "$OUT/open.ppm" "$OUT/open.usda" \
  >"$OUT/motion.log"
grep -q '2 shutter sample(s), 0.5..1.5' "$OUT/motion.log" || {
  cat "$OUT/motion.log"; echo "FAIL: raster shutter schedule missing"; exit 1;
}

# The native interactive raster path advances one deterministic shutter segment
# per frame, then holds the completed linear-light average. It must match the
# companion exporter, which launches one explicitly unblurred viewer process
# per midpoint sample.
env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" "${LOADER_ARGS[@]}" \
  "${DIRECT_WINDOW_ARGS[@]}" --backend "$BACKEND" --frames 3 --size "$TEST_SIZE" \
  --camera Shot --time 1 \
  --pt-motion-segments 2 --render-report "$OUT/native-report.json" \
  --screenshot "$OUT/native.ppm" "$OUT/open.usda" \
  >"$OUT/native.log" 2>&1
python3 - "$OUT/native-report.json" <<'PY'
import json, pathlib, sys
report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
sampled = report.get("camera_shutter", {}).get(
    "raster_interactive_segments_sampled")
if sampled != 2:
    raise SystemExit(
        f"FAIL: native raster sampled {sampled!r} shutter segments, expected 2")
PY
python3 - "$OUT/native.ppm" "$OUT/open.ppm" "$BACKEND" <<'PY'
import pathlib, sys
def pixels(path):
    return pathlib.Path(path).read_bytes().split(b"\n255\n", 1)[1]
a, b = map(pixels, sys.argv[1:3])
if len(a) != len(b):
    raise SystemExit("FAIL: native and companion raster dimensions differ")
mad = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
limit = 0.0 if sys.argv[3] == "vk" else 0.75
if mad > limit:
    raise SystemExit(
        f"FAIL: native {sys.argv[3]} raster differs from companion "
        f"(MAD {mad:.3f}, limit {limit:.3f})")
print(f"native/companion {sys.argv[3]} MAD {mad:.3f}")
PY

python3 "$ROOT/examples/lusdview/render-motion.py" \
  --lusdview "$BIN" "${COMPANION_LOADER_ARGS[@]}" --backend "$BACKEND" --camera Shot --time 1 \
  --segments 8 --size "$TEST_SIZE" --output "$OUT/closed-tool.ppm" \
  "$OUT/closed.usda" >"$OUT/closed-tool.log"
grep -q '1 shutter sample(s)' "$OUT/closed-tool.log" || {
  cat "$OUT/closed-tool.log"; echo "FAIL: closed shutter did not use one sample";
  exit 1;
}

python3 - "$OUT/closed.ppm" "$OUT/open.ppm" "$BACKEND" <<'PY'
import pathlib, sys
def pixels(path):
    return pathlib.Path(path).read_bytes().split(b"\n255\n", 1)[1]
closed, opened = map(pixels, sys.argv[1:3])
mad = sum(abs(a - b) for a, b in zip(closed, opened)) / len(closed)
if mad < 1.0:
    raise SystemExit(f"FAIL: raster motion difference too small (MAD {mad:.3f})")
print(f"PASS: {sys.argv[3]} native/companion raster shutter motion (MAD {mad:.3f})")
PY
python3 - "$OUT/closed.ppm" "$OUT/closed-tool.ppm" "$BACKEND" <<'PY'
import pathlib, sys
def pixels(path):
    return pathlib.Path(path).read_bytes().split(b"\n255\n", 1)[1]
a, b = map(pixels, sys.argv[1:3])
if len(a) != len(b):
    raise SystemExit("FAIL: closed-shutter raster dimensions differ")
mad = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
limit = 0.0 if sys.argv[3] == "vk" else 0.75
if mad > limit:
    raise SystemExit(
        f"FAIL: closed-shutter {sys.argv[3]} companion differs from direct "
        f"render (MAD {mad:.3f}, limit {limit:.3f})")
PY
