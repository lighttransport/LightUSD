#!/usr/bin/env bash
# material:binding:back regression: whole-mesh and GeomSubset back bindings,
# including degraded (unknown implementation) surfaces, must survive --core
# batching and render with the authored recovered constants.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found ($BIN)"; exit "$SKIP"; }

OUT="$(mktemp -d)"
mkdir -p "$OUT/config"
if [ "${LUSDVIEW_KEEP_TEST_OUTPUT:-0}" = 1 ]; then
  echo "test output: $OUT"
else
  trap 'rm -rf "$OUT"' EXIT
fi
RUN=()
if [ "${LUSDVIEW_XVFB:-0}" = 1 ] && command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a --server-num="${LUSDVIEW_XVFB_BASE:-300}")
elif [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1 &&
   xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
  :
elif command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a --server-num="${LUSDVIEW_XVFB_BASE:-300}")
fi

run_bounded() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_BACKFACE_RENDER_TIMEOUT:-60s}" "$@"
  else
    "$@"
  fi
}

cat > "$OUT/backface.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Panels" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    point3f[] points = [(-2,-1,0),(-0.1,-1,0),(-0.1,1,0),(-2,1,0),
                         (0.1,-1,0),(2,-1,0),(2,1,0),(0.1,1,0)]
    int[] faceVertexCounts = [4,4]
    int[] faceVertexIndices = [0,1,2,3, 4,5,6,7]
    uniform token subdivisionScheme = "none"
    rel material:binding = </World/Looks/FrontRed>
    rel material:binding:back = </World/Looks/BackBlue>
    def GeomSubset "Right" (prepend apiSchemas = ["MaterialBindingAPI"]) {
      uniform token elementType = "face"
      uniform token familyName = "materialBind"
      int[] indices = [1]
      rel material:binding = </World/Looks/FrontGreen>
      rel material:binding:back = </World/Looks/BackYellow>
    }
  }
  def Scope "Looks" {
    def Material "FrontRed" {
      token outputs:surface.connect = </World/Looks/FrontRed/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.9,0.03,0.02)
        float inputs:roughness = 0.8
        token outputs:surface
      }
    }
    def Material "FrontGreen" {
      token outputs:surface.connect = </World/Looks/FrontGreen/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.02,0.85,0.03)
        float inputs:roughness = 0.8
        token outputs:surface
      }
    }
    def Material "BackBlue" {
      token outputs:surface.connect = </World/Looks/BackBlue/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UnknownBackSurface"
        color3f inputs:baseColor = (0.02,0.04,0.95)
        float inputs:opacity = 0.65
        float inputs:roughness = 0.8
        token outputs:surface
      }
    }
    def Material "BackYellow" {
      token outputs:surface.connect = </World/Looks/BackYellow/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UnknownSubsetBackSurface"
        color3f inputs:baseColor = (0.95,0.75,0.02)
        float inputs:opacity = 0.9
        float inputs:opacityThreshold = 0.2
        float inputs:roughness = 0.8
        token outputs:surface
      }
    }
  }
  def Camera "Front" {
    double3 xformOp:translate = (0,0,5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def Camera "Back" {
    double3 xformOp:translate = (0,0,-5)
    float xformOp:rotateY = 180
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
  }
}
USDA

# A handedness-changing object transform reverses which authored side the same
# camera observes. Keep a second fixture so every backend proves that its
# front/back decision follows transformed triangle winding, not merely the
# untransformed index order. The back-blue material is blended while all front
# materials are opaque, which also exercises different alpha-pass classes on
# the two complementary draws.
sed '/def Mesh "Panels"/a\    double3 xformOp:scale = (-1, 1, 1)\
    uniform token[] xformOpOrder = ["xformOp:scale"]' \
  "$OUT/backface.usda" > "$OUT/backface_mirrored.usda"

classify() {
python3 - "$1" <<'PY'
import re, sys
d = open(sys.argv[1], 'rb').read()
m = re.match(rb'P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(\d+)\s+(\d+)\s', d)
if not m: sys.exit(2)
w, h = int(m.group(1)), int(m.group(2)); p = d[m.end():m.end()+w*h*3]
c = dict(red=0, green=0, blue=0, yellow=0)
for i in range(0, len(p)-2, 3):
    r,g,b = p[i:i+3]
    c['red'] += r > 45 and r > 2*g and r > 2*b
    c['green'] += g > 40 and g > 1.7*r and g > 1.7*b
    c['blue'] += b > 45 and b > 2*r and b > 2*g
    c['yellow'] += r > 45 and g > 35 and r > 1.8*b and g > 1.8*b
print(c['red'], c['green'], c['blue'], c['yellow'])
PY
}

tested=0
for spec in "gl:--backend gl" "vk:--backend vk" "vkrt:--backend vk --rt" \
            "cuda:--cuda" "hip:--hip"; do
  backend="${spec%%:*}"
  backend_args="${spec#*:}"
  headless_args=(--headless)
  [ "$backend" = gl ] && headless_args=()
  if [ -n "${LUSDVIEW_BACKFACE_BACKEND:-}" ] &&
     [ "$backend" != "$LUSDVIEW_BACKFACE_BACKEND" ]; then
    continue
  fi
  ok=1
  persistent=0
  if [ "$backend" = vkrt ]; then
    capability_log="$OUT/vkrt-capability.log"
    run_bounded env XDG_CONFIG_HOME="$OUT/config" "$BIN" --headless \
      --backend vk --rt --frames 1 --size 64x64 \
      --screenshot "$OUT/vkrt-capability.ppm" "$OUT/backface.usda" \
      >"$capability_log" 2>&1
    if grep -Eq 'caps: v1 .*rt=software|pipeline cache is cold' \
        "$capability_log"; then
      echo "SKIP: Vulkan hardware RT pipeline cache is cold"
      continue
    fi
  fi
  if [ "$backend" = gl ] || [ "$backend" = vk ] || [ "$backend" = vkrt ]; then
    manifest="$OUT/$backend-batch.json"
    python3 - "$manifest" "$OUT/backface.usda" \
      "$OUT/backface_mirrored.usda" "$backend" <<'PY'
import json, sys
out, normal, mirrored, backend = sys.argv[1:]
viewer_args = ["--backend", "gl" if backend == "gl" else "vk",
               "--size", "1280x720"]
if backend == "vkrt":
    viewer_args.append("--rt")
cases = []
for fixture, path in (("normal", normal), ("mirrored", mirrored)):
    for camera in ("Front", "Back"):
        cases.append({
            "name": f"{fixture}-{camera}", "path": path,
            "load_settings": {"camera": camera},
            "settings": {"mode": "shaded", "grid": False},
            "viewport": None,
            "expect": {
                "mesh_count": 2,
                "triangle_count": 4,
                "backend": "gl" if backend == "gl" else "vk",
                "ray_tracing": backend == "vkrt",
            },
        })
json.dump({"viewer_args": viewer_args, "cases": cases}, open(out, "w"))
PY
    batch_log="$OUT/$backend-batch.log"
    batch_cmd=(python3 "$REPO_ROOT/tests/lusdview/mcp_render_batch.py"
               "$BIN" "$manifest" "$OUT/$backend-batch-images")
    if [ "$backend" = gl ]; then
      batch_cmd+=(--windowed --backend gl)
      "${RUN[@]}" env XDG_CONFIG_HOME="$OUT/config" \
        "${batch_cmd[@]}" >"$batch_log" 2>&1
    else
      env XDG_CONFIG_HOME="$OUT/config" \
        "${batch_cmd[@]}" >"$batch_log" 2>&1
    fi
    batch_rc=$?
    cat "$batch_log"
    if [ "$batch_rc" -eq "$SKIP" ]; then
      echo "SKIP: $backend backend unavailable"
      ok=0
    elif [ "$batch_rc" -ne 0 ]; then
      echo "FAIL: $backend persistent MCP batch failed"
      exit 1
    elif [ "$backend" = vkrt ] &&
         grep -Eq 'caps: v1 .*rt=software|pipeline cache is cold' "$batch_log"; then
      echo "SKIP: Vulkan hardware RT pipeline cache is cold"
      ok=0
    else
      persistent=1
    fi
  fi
  case_index=0
  for fixture in normal mirrored; do
  asset="$OUT/backface.usda"
  [ "$fixture" = mirrored ] && asset="$OUT/backface_mirrored.usda"
  for camera in Front Back; do
    img="$OUT/${backend}_${fixture}_${camera}.ppm"
    log="$OUT/${backend}_${fixture}_${camera}.log"
    if [ "$persistent" -eq 1 ]; then
      printf -v batch_prefix '%03d' "$case_index"
      img="$OUT/$backend-batch-images/$batch_prefix-$fixture-$camera-capture.ppm"
    else
      if [ "$ok" -eq 0 ]; then break; fi
      # CUDA/HIP own their screenshots at process shutdown and therefore remain
      # isolated launches; raster/Vulkan-RT cases use the persistent batch above.
      # shellcheck disable=SC2086
      run_bounded "${RUN[@]}" env XDG_CONFIG_HOME="$OUT/config" \
        "$BIN" "${headless_args[@]}" $backend_args --frames 2 \
        --size 1280x720 \
        --camera "$camera" --screenshot "$img" "$asset" >"$log" 2>&1
      launch_rc=$?
      marker='render stats'
      [ "$backend" = cuda ] && marker='CUDA RT wrote'
      [ "$backend" = hip ] && marker='HIP RT wrote'
      if ! grep -q "$marker" "$log" || [ ! -s "$img" ]; then
        if [ "$launch_rc" -eq 124 ] || [ "$launch_rc" -eq 137 ]; then
          echo "SKIP: $backend backend probe timed out"
        else
          echo "SKIP: $backend backend unavailable"
        fi
        grep -Ei 'CUDA|HIP|ray tracing|ray trace|failed|unavailable' "$log" | tail -8 || true
        ok=0
        break
      fi
    fi
    read -r red green blue yellow < <(classify "$img") || {
      echo "FAIL: malformed $backend/$camera screenshot"; exit 1; }
    echo "$backend $fixture $camera: red=$red green=$green blue=$blue yellow=$yellow"
    expect_front=0
    if { [ "$fixture" = normal ] && [ "$camera" = Front ]; } ||
       { [ "$fixture" = mirrored ] && [ "$camera" = Back ]; }; then
      expect_front=1
    fi
    if [ "$expect_front" -eq 1 ]; then
      if [ "$red" -lt 100 ] || [ "$green" -lt 100 ]; then
        echo "FAIL: $backend front bindings did not render both face groups"
        exit 1
      fi
    elif [ "$blue" -lt 100 ] || [ "$yellow" -lt 100 ]; then
      echo "FAIL: $backend back bindings did not render mesh/subset materials"
      exit 1
    fi
    case_index=$((case_index + 1))
  done
  done
  if [ "$ok" -eq 1 ]; then tested=$((tested + 1)); fi
done

[ "$tested" -gt 0 ] || exit "$SKIP"
echo "PASS: front/back material bindings and GeomSubset fallback rendered"
