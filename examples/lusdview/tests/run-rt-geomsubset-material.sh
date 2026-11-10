#!/usr/bin/env bash
# One mesh, three materialBind GeomSubsets. Verify that every RT backend and
# both scene loaders shade the committed triangle's material instead of reusing
# the mesh's first one.
# Exit 77 when no RT backend is available; 1 on an observed regression.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if [ -n "${LUSDVIEW:-}" ]; then BIN="$LUSDVIEW"
elif [ -x "$REPO_ROOT/build_ninja/lusdview" ]; then BIN="$REPO_ROOT/build_ninja/lusdview"
else BIN="$REPO_ROOT/build/lusdview"; fi
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }

OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
if [ -z "${LUSDVIEW_TEST_OUT:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi

# Isolate the test from the user's saved window/device preferences. `--size` is
# not a lusdview CLI option; headless dimensions come from the config. Keep the
# target small enough for software ray-query implementations such as llvmpipe;
# the color probe needs coverage, not interactive resolution.
cat > "$OUT/config.json" <<'JSON'
{"window_size":{"width":320,"height":320}}
JSON

cat > "$OUT/rt-geomsubset.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Strip" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    uniform bool doubleSided = 1
    uniform token subdivisionScheme = "none"
    point3f[] points = [(-3,-1,0),(-1,-1,0),(1,-1,0),(3,-1,0),
                        (-3, 1,0),(-1, 1,0),(1, 1,0),(3, 1,0)]
    int[] faceVertexCounts = [4,4,4]
    int[] faceVertexIndices = [0,1,5,4, 1,2,6,5, 2,3,7,6]
    uniform token subsetFamily:materialBind:familyType = "partition"
    def GeomSubset "RedFace" (prepend apiSchemas = ["MaterialBindingAPI"]) {
      uniform token elementType = "face"
      uniform token familyName = "materialBind"
      int[] indices = [0]
      rel material:binding = </World/Red>
    }
    def GeomSubset "GreenFace" (prepend apiSchemas = ["MaterialBindingAPI"]) {
      uniform token elementType = "face"
      uniform token familyName = "materialBind"
      int[] indices = [1]
      rel material:binding = </World/Green>
    }
    def GeomSubset "BlueFace" (prepend apiSchemas = ["MaterialBindingAPI"]) {
      uniform token elementType = "face"
      uniform token familyName = "materialBind"
      int[] indices = [2]
      rel material:binding = </World/Blue>
    }
  }
  def Material "Red" {
    token outputs:surface.connect = </World/Red/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (1,0.01,0.01)
      float inputs:roughness = 1
      token outputs:surface
    }
  }
  def Material "Green" {
    token outputs:surface.connect = </World/Green/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.01,1,0.01)
      float inputs:roughness = 1
      token outputs:surface
    }
  }
  def Material "Blue" {
    token outputs:surface.connect = </World/Blue/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.01,0.01,1)
      float inputs:roughness = 1
      token outputs:surface
    }
  }
}
USDA

probe() {
  python3 - "$1" "$2" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
if not m or int(m.group(3)) != 255: sys.exit(2)
p = d[m.end():]
counts = [0, 0, 0]
for i in range(0, len(p) - 2, 3):
    c = p[i:i + 3]
    hi = max(range(3), key=lambda k: c[k])
    lo = max(c[(hi + 1) % 3], c[(hi + 2) % 3])
    if c[hi] > 35 and c[hi] > lo + 25:
        counts[hi] += 1
print(f"{sys.argv[2]} red={counts[0]} green={counts[1]} blue={counts[2]}")
sys.exit(0 if min(counts) >= 300 else 1)
PY
}

# Mesa llvmpipe may advertise the ray-query extensions yet fail to finish even
# this tiny capture before the process timeout. When vulkaninfo proves that all
# visible Vulkan devices are CPU implementations, classify Vulkan RT as an
# explicit capability skip instead of paying the timeout once per loader. If a
# hardware/virtual GPU is also visible, let lusdview's normal device selection
# and the render assertions decide.
vk_software_only=0
if command -v vulkaninfo >/dev/null 2>&1 &&
   command -v timeout >/dev/null 2>&1 &&
   timeout 10s vulkaninfo --summary >"$OUT/vulkaninfo.log" 2>&1; then
  if grep -q 'PHYSICAL_DEVICE_TYPE_CPU' "$OUT/vulkaninfo.log" &&
     ! grep -Eq 'PHYSICAL_DEVICE_TYPE_(DISCRETE|INTEGRATED|VIRTUAL)_GPU' \
       "$OUT/vulkaninfo.log"; then
    vk_software_only=1
  fi
fi

ran=0
python3 - "$OUT" <<'PY'
import pathlib,sys,zipfile
root=pathlib.Path(sys.argv[1])
with zipfile.ZipFile(root/"rt-geomsubset.usdz",'w',compression=zipfile.ZIP_STORED) as archive:
 archive.write(root/"rt-geomsubset.usda","rt-geomsubset.usda")
 for image in sorted(root.iterdir()):
  if image.suffix.lower() in ('.png','.ppm','.jpg','.jpeg','.exr'): archive.write(image,image.name)
PY

fail=0
for spec in "vk:--backend vk --rt:caps: v1 .*rt=hardware" \
            "cuda:--cuda:CUDA RT wrote" "hip:--hip:HIP RT wrote"; do
  backend="${spec%%:*}"
  rest="${spec#*:}"
  args="${rest%%:*}"
  marker="${rest#*:}"
  if [ "$backend" = vk ] && [ "$vk_software_only" -ne 0 ]; then
    echo "SKIP: next-vk RT unavailable (software Vulkan only)"
    echo "SKIP: packaged-vk RT unavailable (software Vulkan only)"
    continue
  fi
  next_ok=0
  packaged_ok=0
  for loader_spec in "next:--next" "packaged:--next"; do
    loader="${loader_spec%%:*}"
    loader_args="${loader_spec#*:}"
    tag="$loader-$backend"
    source="$OUT/rt-geomsubset.usda"
    [ "$loader" != packaged ] || source="$OUT/rt-geomsubset.usdz"
    img="$OUT/rt-geomsubset-$tag.ppm"
    # shellcheck disable=SC2086
    if command -v timeout >/dev/null 2>&1; then
      # shellcheck disable=SC2086
      timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-60s}" \
        "$BIN" --headless $loader_args $args --frames 4 \
        --config "$OUT/config.json" --screenshot "$img" \
        "$source" >"$OUT/rt-geomsubset-$tag.log" 2>&1
    else
      # shellcheck disable=SC2086
      "$BIN" --headless $loader_args $args --frames 4 \
        --config "$OUT/config.json" --screenshot "$img" \
        "$source" >"$OUT/rt-geomsubset-$tag.log" 2>&1
    fi
    if grep -Fq "$marker" "$OUT/rt-geomsubset-$tag.log" && [ -s "$img" ]; then
      ran=$((ran + 1))
      if [ "$loader" = next ]; then next_ok=1; else packaged_ok=1; fi
      probe "$img" "$tag" || {
        echo "FAIL: $tag GeomSubset materials"; fail=1;
      }
    else
      echo "SKIP: $tag RT unavailable"
    fi
  done
  # If either input format proved this backend is available, the other input format must
  # also reach RT and render. Otherwise a format-specific failure would be
  # mislabeled as an unavailable GPU and silently pass via the other input format.
  if [ "$next_ok" -ne "$packaged_ok" ]; then
    echo "FAIL: $backend RT available for only one input format"
    fail=1
  fi
done

[ "$ran" -gt 0 ] || exit "$SKIP"
[ "$fail" -eq 0 ] || exit 1
echo "PASS: per-triangle GeomSubset materials on $ran format/backend runs"
