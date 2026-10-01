#!/usr/bin/env bash
#
# Run test: the Vulkan backend must come up on a Vulkan device and render a
# non-blank frame in both rasterization (--backend vk) and ray-query (--rt) mode.
#
# This is the cross-platform "the VK path works on this device" check. The Vulkan
# GPU backends were historically only exercised on a Windows AMD Radeon RX 570 /
# amdvlk where they mis-render; this asserts a correct render on the host GPU
# (verified working on NVIDIA GeForce RTX 5060 Ti / Linux, see doc/lusdrender.md).
#
# lusdview's --headless path is Vulkan-only and needs no window/X server (it
# renders into an offscreen image and reads it back), so this test does NOT need
# xvfb. If the Vulkan backend cannot be created (no Vulkan device/driver in this
# environment) lusdview exits nonzero without printing "render stats"; the test
# then SKIPs (exit 77, ctest SKIP_RETURN_CODE) rather than failing, so headless
# CI without a GPU does not report a failure.
#
# Exit codes: 0 = pass, 1 = fail (renderer up but frame blank), 77 = skip.
#
# Usage (from the repo root, after building build/lusdview):
#   examples/lusdview/tests/run-vk-render.sh
# Overrides: LUSDVIEW=<binary>  ASSET=<usd file>
#            CURVES_ASSET=<usd file>  NURBS_ASSET=<usd file>
#            LUSDVIEW_RENDER_TIMEOUT=180s
# Also run via ctest: `ctest -R lusdview-vk-render`.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build/lusdview}"
ASSET="${ASSET:-$REPO_ROOT/models/suzanne-pbr.usda}"
CURVES_ASSET="${CURVES_ASSET:-$REPO_ROOT/models/lusdview-curves-ribbons.usda}"
NURBS_ASSET="${NURBS_ASSET:-$REPO_ROOT/models/lusdview-nurbs-patch.usda}"
# A cold full-MaterialX ray-query pipeline takes roughly 155 seconds to compile
# with current NVIDIA drivers; subsequent cache hits take milliseconds. Keep the
# isolated hardware smoke test bounded to three minutes instead of misclassifying
# a healthy RT device as unavailable after 30 seconds.
LUSDVIEW_RENDER_TIMEOUT="${LUSDVIEW_RENDER_TIMEOUT:-180s}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW (set LUSDVIEW=...)"
  exit $SKIP
fi
if [ ! -f "$ASSET" ]; then
  echo "SKIP: asset not found at $ASSET (set ASSET=...)"
  exit $SKIP
fi
if [ ! -f "$CURVES_ASSET" ]; then
  echo "SKIP: curves asset not found at $CURVES_ASSET (set CURVES_ASSET=...)"
  exit $SKIP
fi
if [ ! -f "$NURBS_ASSET" ]; then
  echo "SKIP: NURBS asset not found at $NURBS_ASSET (set NURBS_ASSET=...)"
  exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Keep the regression independent of the user's saved window size and device
# preferences. A large interactive viewport can make llvmpipe spend longer than
# the process timeout on the very first frame, incorrectly classifying a usable
# software Vulkan device as unavailable.
CONFIG="$TMP/config.json"
cat > "$CONFIG" <<'JSON'
{"window_size":{"width":320,"height":320}}
JSON

# llvmpipe currently advertises ray query but does not complete the offscreen
# RT capture before the process timeout. Detect an all-CPU Vulkan installation
# up front so the raster pass can still run and the RT pass can skip promptly.
VK_SOFTWARE_ONLY=0
if command -v vulkaninfo >/dev/null 2>&1 &&
   command -v timeout >/dev/null 2>&1 &&
   timeout 10s vulkaninfo --summary >"$TMP/vulkaninfo.log" 2>&1; then
  if grep -q 'PHYSICAL_DEVICE_TYPE_CPU' "$TMP/vulkaninfo.log" &&
     ! grep -Eq 'PHYSICAL_DEVICE_TYPE_(DISCRETE|INTEGRATED|VIRTUAL)_GPU' \
       "$TMP/vulkaninfo.log"; then
    VK_SOFTWARE_ONLY=1
  fi
fi

MASK_ASSET="$TMP/vk_mask_opacity.usda"
cat > "$MASK_ASSET" <<'USD'
#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Mesh "Low" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Materials/CutLow>
        point3f[] points = [(-1.2, -1, 0), (-0.1, -1, 0), (-0.1, 1, 0), (-1.2, 1, 0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        uniform token subdivisionScheme = "none"
    }
    def Mesh "High" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Materials/CutHigh>
        point3f[] points = [(0.1, -1, 0), (1.2, -1, 0), (1.2, 1, 0), (0.1, 1, 0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        uniform token subdivisionScheme = "none"
    }
    def Scope "Materials"
    {
        def Material "CutLow"
        {
            token outputs:surface.connect = </World/Materials/CutLow/Preview.outputs:surface>
            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.8, 0.1, 0.1)
                float inputs:opacity = 0.25
                float inputs:opacityThreshold = 0.5
                token outputs:surface
            }
        }
        def Material "CutHigh"
        {
            token outputs:surface.connect = </World/Materials/CutHigh/Preview.outputs:surface>
            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.1, 0.3, 0.9)
                float inputs:opacity = 0.75
                float inputs:opacityThreshold = 0.5
                token outputs:surface
            }
        }
    }
}
USD

# Custom primvars cross the public render API before being flattened by the
# viewer. Exercise scalar float/int storage and the vector lane used by common
# rest-position primvars. Container-sized byte arithmetic used to reject these
# small buffers and silently discard the entire mesh.
CUSTOM_PRIMVARS_ASSET="$TMP/custom_primvars.usda"
cat > "$CUSTOM_PRIMVARS_ASSET" <<'USD'
#usda 1.0
def Xform "World"
{
    def Mesh "Float"
    {
        point3f[] points = [(-2, 0, 0), (-1, 0, 0), (-1.5, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        uniform token subdivisionScheme = "none"
        float[] primvars:temperature = [10, 20, 30] (interpolation = "vertex")
    }
    def Mesh "Int"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0.5, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        uniform token subdivisionScheme = "none"
        int[] primvars:label = [1, 2, 3] (interpolation = "vertex")
    }
    def Mesh "Rest"
    {
        point3f[] points = [(0, -2, 0), (1, -2, 0), (0.5, -1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        uniform token subdivisionScheme = "none"
        float3[] primvars:rest = [(0, -2, 0), (1, -2, 0), (0.5, -1, 0)] (
            interpolation = "vertex"
        )
        int[] primvars:rest:indices = None
    }
}
USD

run_lusdview() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "$LUSDVIEW_RENDER_TIMEOUT" "$LUSDVIEW" \
      --config "$CONFIG" "$@"
  else
    "$LUSDVIEW" --config "$CONFIG" "$@"
  fi
}

# Assert a screenshot has more than one distinct pixel (i.e. is not a flat blank
# clear). PPM (binary P6) is trivial to parse without external packages; fall
# back to a size>header sanity check if python3 is unavailable.
nonblank() {
  local img="$1"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$img" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
# Parse a binary PPM (P6) header: magic, width, height, maxval, then raw bytes.
if not data.startswith(b"P6"):
    print("not-ppm"); sys.exit(2)
tok, i, vals = [], 2, []
while len(vals) < 3 and i < len(data):
    c = data[i:i+1]
    if c.isspace():
        if tok: vals.append(b"".join(tok)); tok = []
    elif c == b"#":
        while i < len(data) and data[i:i+1] != b"\n": i += 1
    else:
        tok.append(c)
    i += 1
w, h, mx = (int(v) for v in vals)
i += 0
px = data[i+1:]
stride = 3
distinct = {px[p:p+stride] for p in range(0, min(len(px), w*h*3), stride)}
sys.exit(0 if len(distinct) > 1 else 1)
PY
    return $?
  fi
  # No python3: just require the file to be larger than a trivial header.
  [ "$(wc -c < "$img")" -gt 64 ]
}

binary_opacity() {
  local img="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    return 0
  fi
  python3 - "$img" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
if not data.startswith(b"P6"):
    sys.exit(2)
tok, i, vals = [], 2, []
while len(vals) < 3 and i < len(data):
    c = data[i:i+1]
    if c.isspace():
        if tok: vals.append(b"".join(tok)); tok = []
    elif c == b"#":
        while i < len(data) and data[i:i+1] != b"\n": i += 1
    else:
        tok.append(c)
    i += 1
w, h, mx = (int(v) for v in vals)
px = data[i+1:]
vals = [px[p] for p in range(0, min(len(px), w*h*3), 3)
        if len(px[p:p+3]) == 3]
dark = sum(1 for v in vals if v <= 16)
bright = sum(1 for v in vals if v >= 240)
sys.exit(0 if dark > 0 and bright > 0 else 1)
PY
}

run_pass() {
  # $1 = label, rest = extra lusdview flags
  local label="$1"; shift
  run_asset_pass "$label" "$ASSET" "$@"
}

run_asset_pass() {
  # $1 = label, $2 = asset, rest = extra lusdview flags
  local label="$1"; shift
  local asset="$1"; shift
  local out="$TMP/vk_${label}.ppm"
  echo "=== lusdview VK pass: $label ==="
  local log
  log="$(run_lusdview --headless "$@" --frames 4 --screenshot "$out" "$asset" 2>&1)"
  echo "$log"
  if ! echo "$log" | grep -q "render stats"; then
    echo "SKIP: Vulkan backend unavailable in this environment ($label)"
    return $SKIP
  fi
  if [ "$label" = rt ] &&
     ! echo "$log" | grep -Eq "caps: v1 .*rt=hardware"; then
    echo "SKIP: Vulkan ray query unavailable; raster fallback is not an RT pass"
    return $SKIP
  fi
  if echo "$log" | grep -q "load failed" ||
     ! echo "$log" | grep -Eq 'drawn tris [1-9][0-9]*'; then
    echo "FAIL: $label produced no renderable triangles"
    return 1
  fi
  if [ "$label" = custom-primvars ] &&
     ! echo "$log" | grep -Eq 'loaded .*: 3 mesh\(es\), 3 tri\(s\)'; then
    echo "FAIL: custom float/int/vector primvars did not retain all meshes"
    return 1
  fi
  if [ "$label" = "profile" ]; then
    # --max-gpu-mem is DERIVED from the device's VRAM (half of it, floor 8 GiB),
    # so assert that a positive budget was resolved, not a number that only holds
    # on a 16 GiB card.
    for expected in "large-scene-profile instance-heavy resolved" "backend=vk" \
                    "--next=on" "--raster-lod=on" "--rt-lod=on" \
                    "--max-gpu-mem=[1-9][0-9]*\.[0-9]"; do
      if ! echo "$log" | grep -Eq -- "$expected"; then
        echo "FAIL: profile pass did not apply expected default: $expected"
        return 1
      fi
    done
  elif [ "$label" = "profile-final" ]; then
    for expected in "large-scene-profile instance-heavy resolved" \
                    "--rt-lod=off"; do
      if ! echo "$log" | grep -Eq -- "$expected"; then
        echo "FAIL: final path tracing retained implicit RT LOD: $expected"
        return 1
      fi
    done
    if echo "$log" | grep -Eq '\[rt-lod\].*proxy=[1-9]'; then
      echo "FAIL: final path tracing rendered an implicit bounding-box proxy"
      return 1
    fi
  fi
  if [ ! -s "$out" ]; then
    echo "FAIL: no screenshot written ($label)"
    return 1
  fi
  if ! nonblank "$out"; then
    echo "FAIL: $label screenshot is blank (renderer came up but rendered nothing)"
    return 1
  fi
  echo "PASS: $label render is non-blank"
  return 0
}

# 1) Vulkan rasterization.
run_pass raster --backend vk
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

# 2) Vulkan hardware ray query. Raster fallback is an explicit skip, not proof
#    that the ray-query pipeline works.
if [ "$VK_SOFTWARE_ONLY" -ne 0 ]; then
  echo "SKIP: Vulkan ray query unavailable (software Vulkan only)"
  exit $SKIP
fi
run_pass rt --backend vk --rt
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

# 3) Masked opacity AOV over generated PreviewSurface constants. This guards the
#    raster material alphaMode/alphaCutoff push-constant path without private
#    assets: opacity 0.25 below cutoff becomes black, 0.75 becomes white.
run_asset_pass opacity-aov "$MASK_ASSET" --backend vk --mode opacity
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc
if ! binary_opacity "$TMP/vk_opacity-aov.ppm"; then
  echo "FAIL: opacity AOV did not contain binary masked opacity"
  exit 1
fi

# 4) Generated raster mesh coverage for unsupported-but-renderable schemas.
run_asset_pass curves-ribbons "$CURVES_ASSET" --backend vk --legacy-load --time 0
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

run_asset_pass nurbs-patch "$NURBS_ASSET" --backend vk --legacy-load --time 0
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

# 5) Large-scene realtime profile wiring. Uses the same tiny fixture so CI does
#    not need Caldera/Island/ALab, but exercises the preset resolver, --next,
#    Vulkan headless rendering, raster LOD defaults, and startup diagnostics.
run_pass profile --large-scene-profile instance-heavy
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

# 6) Final path-trace quality must override a profile's implicit RT LOD. The
# generic LOD carrier is an unmaterialed AABB, so retaining it would produce
# camera-dependent white boxes in a final render.
run_pass profile-final --large-scene-profile instance-heavy --path-trace \
  --pt-quality final --pt-samples 1
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

run_asset_pass custom-primvars "$CUSTOM_PRIMVARS_ASSET" --backend vk
rc=$?
[ $rc -eq $SKIP ] && exit $SKIP
[ $rc -ne 0 ] && exit $rc

echo "PASS: Vulkan raster + ray-query + large-scene profile render correctly on this device"
exit 0
