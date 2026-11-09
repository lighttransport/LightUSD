#!/usr/bin/env bash
set -euo pipefail

SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"
VK_DEVICE_ARGS=()
if [ -n "${LUSDVIEW_VK_DEVICE:-}" ]; then
  VK_DEVICE_ARGS=(--vk-device "$LUSDVIEW_VK_DEVICE")
fi

cat >"$OUT/motion.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" startTimeCode = 0 endTimeCode = 2)
def Xform "World" {
  def Mesh "MovingQuad" {
    double3 xformOp:translate.timeSamples = { 0: (-2, 0, 0), 2: (2, 0, 0) }
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
    double shutter:open = -0.5
    double shutter:close = 0.5
    double3 xformOp:translate = (0, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA

if ! env XDG_CONFIG_HOME="$OUT/config-home-probe" "$BIN" --next --headless \
    --backend vk "${VK_DEVICE_ARGS[@]}" --frames 1 --size 16x16 --camera Shot --time 1 \
    --render-report "$OUT/probe.json" "$OUT/motion.usda" >"$OUT/probe.log" 2>&1; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/probe.log"; then
    echo "SKIP: Vulkan unavailable"; exit "$SKIP"
  fi
  cat "$OUT/probe.log"; exit 1
fi
if grep -Eqi 'caps:.*device=cpu|renderer: Vulkan .*GPU:.*(llvmpipe|SwiftShader)' "$OUT/probe.log"; then
  echo "SKIP: Vulkan RT temporal test needs a non-CPU Vulkan adapter"
  exit "$SKIP"
fi

if ! env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next --headless \
    --backend vk "${VK_DEVICE_ARGS[@]}" --rt --frames 4 --size 32x32 --camera Shot --time 1 \
    --pt-motion-segments 2 --render-report "$OUT/open.json" \
    --screenshot "$OUT/open.ppm" "$OUT/motion.usda" >"$OUT/open.log" 2>&1; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/open.log"; then
    echo "SKIP: Vulkan unavailable"; exit "$SKIP"
  fi
  cat "$OUT/open.log"; exit 1
fi

# CPU Vulkan implementations can take minutes to run the software-BVH shader;
# this focused CTest is intended for a hardware Vulkan RT adapter. The software
# path is still covered manually and by the report contract on capable hosts.
if grep -Eqi 'caps:.*device=cpu|renderer: Vulkan .*GPU:.*(llvmpipe|SwiftShader)' "$OUT/open.log"; then
  echo "SKIP: Vulkan RT temporal test needs a non-CPU Vulkan adapter"
  exit "$SKIP"
fi

cp "$OUT/motion.usda" "$OUT/closed.usda"
sed -i 's/double shutter:open = -0.5/double shutter:open = 0/; s/double shutter:close = 0.5/double shutter:close = 0/' "$OUT/closed.usda"
env XDG_CONFIG_HOME="$OUT/config-home-closed" "$BIN" --next --headless \
  --backend vk "${VK_DEVICE_ARGS[@]}" --rt --frames 1 --size 32x32 \
  --camera Shot --time 1 --pt-motion-segments 2 \
  --render-report "$OUT/closed.json" --screenshot "$OUT/closed.ppm" \
  "$OUT/closed.usda" >"$OUT/closed.log" 2>&1

# Keep a deforming skeletal + blendshape mesh in the same temporal contract. The
# fixture's camera is static, so add an authored shutter to the copied test input;
# the RT path must pose the two midpoint samples before rebuilding/refitting its
# acceleration data.
cp "$ROOT/examples/lusdview/tests/deform-morph-skin.usda" "$OUT/deform.usda"
sed -i '/float focalLength = 24/a\        double shutter:open = -0.5\n        double shutter:close = 0.5' "$OUT/deform.usda"
env XDG_CONFIG_HOME="$OUT/config-home-deform" "$BIN" --next --headless \
  --backend vk "${VK_DEVICE_ARGS[@]}" --rt --frames 4 --size 32x32 --camera /root/Cam --time 10 \
  --pt-motion-segments 2 --render-report "$OUT/deform.json" \
  --screenshot "$OUT/deform.ppm" "$OUT/deform.usda" >"$OUT/deform.log" 2>&1

python3 - "$OUT/open.json" "$OUT/open.ppm" "$OUT/closed.json" "$OUT/closed.ppm" <<'PY'
import json, pathlib, sys
r = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
s = r.get("camera_shutter", {})
if s.get("vulkan_interactive_segments_sampled") != 2:
    raise SystemExit(f"FAIL: Vulkan RT shutter mask is {s}")
if r.get("render", {}).get("samples") != 4:
    raise SystemExit(
        f"FAIL: Vulkan RT accumulation reset between temporal poses: "
        f"{r.get('render', {}).get('samples')}")
data = pathlib.Path(sys.argv[2]).read_bytes()
header, pixels = data.split(b"\n255\n", 1)
if header != b"P6\n64 64" or len(pixels) != 64 * 64 * 3:
    raise SystemExit("FAIL: Vulkan RT temporal screenshot dimensions changed")
closed = pathlib.Path(sys.argv[4]).read_bytes().split(b"\n255\n", 1)[1]
if len(closed) != len(pixels):
    raise SystemExit("FAIL: closed-shutter Vulkan RT dimensions changed")
mad = sum(abs(a - b) for a, b in zip(pixels, closed)) / len(pixels)
if mad < 1.0:
    raise SystemExit(f"FAIL: Vulkan RT open shutter image did not differ from closed (MAD {mad:.3f})")
print("PASS: Vulkan RT temporal pose accumulation")
PY

python3 - "$OUT/deform.json" "$OUT/deform.log" <<'PY'
import json, pathlib, sys
r = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
s = r.get("camera_shutter", {})
if s.get("vulkan_interactive_segments_sampled") != 2:
    raise SystemExit(f"FAIL: Vulkan RT deformation shutter mask is {s}")
if r.get("render", {}).get("samples") != 4:
    raise SystemExit("FAIL: Vulkan RT deformation accumulation reset")
log = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
if "RT skeletal + blendshape skinning" not in log:
    raise SystemExit("FAIL: Vulkan RT deformation fixture did not use skeletal + blendshape path")
print("PASS: Vulkan RT temporal deformation pose accumulation")
PY
