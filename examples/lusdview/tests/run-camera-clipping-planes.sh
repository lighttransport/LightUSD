#!/usr/bin/env bash
# Raster regression for authored UsdGeomCamera clippingPlanes. The same camera
# and quad are rendered with and without x>=0 clipping on GL and Vulkan through
# both the default and legacy camera loaders.
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
command -v xvfb-run >/dev/null || { echo "SKIP: xvfb-run required"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT"
printf '%s\n' '{"window_size":{"width":160,"height":160}}' > "$OUT/config.json"

cat > "$OUT/clip.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)]
    color3f[] primvars:displayColor = [(0.8, 0.2, 0.1)] (
      interpolation = "constant"
    )
  }
  def Camera "Full" {
    float focalLength = 50
    float horizontalAperture = 40
    float verticalAperture = 40
    float2 clippingRange = (0.1, 100)
    double3 xformOp:translate = (0, 0, 5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def Camera "Clipped" {
    float focalLength = 50
    float horizontalAperture = 40
    float verticalAperture = 40
    float2 clippingRange = (0.1, 100)
    float4[] clippingPlanes = [(1, 0, 0, 0)]
    double3 xformOp:translate = (0, 0, 5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA

render() {
  local backend="$1" loader="$2" camera="$3" image="$4" log="$5"
  local args=(--backend "$backend" --config "$OUT/config.json" --camera "$camera"
              --frames 3 --no-grid --screenshot "$image" "$OUT/clip.usda")
  [ "$loader" = legacy ] && args=(--legacy-load "${args[@]}")
  if [ "$backend" = vk ]; then
    "$BIN" --headless "${args[@]}" >"$log" 2>&1
  else
    xvfb-run -a "$BIN" "${args[@]}" >"$log" 2>&1
  fi
}

for backend in gl vk; do
  for loader in next legacy; do
    render "$backend" "$loader" /World/Full "$OUT/$backend-$loader-full.ppm" "$OUT/$backend-$loader-full.log" || {
      if [ "$backend" = vk ] && grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/$backend-$loader-full.log"; then
        echo "SKIP: Vulkan unavailable"; exit "$SKIP"
      fi
      cat "$OUT/$backend-$loader-full.log"; exit 1
    }
    render "$backend" "$loader" /World/Clipped "$OUT/$backend-$loader-clipped.ppm" "$OUT/$backend-$loader-clipped.log" || {
      cat "$OUT/$backend-$loader-clipped.log"; exit 1
    }
  done
done

python3 - "$OUT" <<'PY'
import pathlib, sys

def ppm(path):
    raw = pathlib.Path(path).read_bytes()
    head, dims, maxv, pixels = raw.split(b"\n", 3)
    if head != b"P6" or maxv != b"255":
        raise SystemExit(f"FAIL: invalid PPM {path}")
    w, h = map(int, dims.split())
    return w, h, pixels[:w*h*3]

def foreground(path):
    w, h, px = ppm(path)
    bg = tuple(px[:3])
    # Lighting differs slightly across drivers, but the clear color is uniform.
    count = sum(1 for i in range(0, len(px), 3)
                if max(abs(px[i+j] - bg[j]) for j in range(3)) > 8)
    return w, h, count

root = pathlib.Path(sys.argv[1])
counts = {}
foreground_counts = {}
for backend in ("gl", "vk"):
    for loader in ("next", "legacy"):
        wf, hf, full = foreground(root / f"{backend}-{loader}-full.ppm")
        wc, hc, clipped = foreground(root / f"{backend}-{loader}-clipped.ppm")
        if (wf, hf) != (wc, hc) or full < 1000:
            raise SystemExit(
                f"FAIL: {backend}/{loader} invalid/full render: full={full}")
        ratio = clipped / full
        if not 0.35 <= ratio <= 0.65:
            raise SystemExit(
                f"FAIL: {backend}/{loader} clipping did not retain half the quad: "
                f"full={full} clipped={clipped} ratio={ratio:.3f}")
        counts[f"{backend}-{loader}"] = ratio
        foreground_counts[f"{backend}-{loader}-full"] = full
        foreground_counts[f"{backend}-{loader}-clipped"] = clipped
    if abs(counts[f"{backend}-next"] - counts[f"{backend}-legacy"]) > 0.02:
        raise SystemExit(f"FAIL: {backend} next/legacy clipping ratios differ: {counts}")
    for camera in ("full", "clipped"):
        a = foreground_counts[f"{backend}-next-{camera}"]
        b = foreground_counts[f"{backend}-legacy-{camera}"]
        if abs(a-b) > max(a, b) * 0.02:
            raise SystemExit(
                f"FAIL: {backend} next/legacy {camera} coverage differs: {a} vs {b}")
if abs(counts["gl-next"] - counts["vk-next"]) > 0.08:
    raise SystemExit(f"FAIL: GL/Vulkan clipping ratios differ: {counts}")
print(f"PASS: camera clipping planes GL={counts['gl-next']:.3f} "
      f"VK={counts['vk-next']:.3f}; default/legacy images equivalent")
PY
