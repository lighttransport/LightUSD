#!/usr/bin/env bash
# CPU preview MaterialX graph lanes.
#
# The shaders override the constant/texture metallic, roughness and emission
# with the MaterialX graph outputs (routes 1, 2, 4). The CPU preview used to
# evaluate only the base-colour (0) and normal (5) routes, so a MaterialX
# material with a constant-driven metalness/roughness/emission lane rendered
# with the shader's fallback constant. Each pair here changes only that route's
# node value and requires the image to change; an ignored route would render
# both variants identically.
#
# Exit codes: 0 = pass, 1 = mismatch, 77 = skip (no lusdview/Vulkan).
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found: $BIN"; exit "$SKIP"; }

OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
if [ -z "${LUSDVIEW_TEST_OUT:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi

python3 - "$OUT" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

HEADER = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "World"
{
    def DistantLight "KeyLight"
    {
        float inputs:intensity = 1.0
        float3 xformOp:rotateXYZ = (-45, 0, 45)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }

    def Mesh "Plane" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 0
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (
            interpolation = "faceVarying"
        )
        point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/_materials/M>
    }

    def Scope "_materials"
    {
        def Material "M" (
            prepend apiSchemas = ["MaterialXConfigAPI"]
        )
        {
            string config:mtlx:version = "1.39"
            token outputs:mtlx:surface.connect = </World/_materials/M/OpenPBRShader.outputs:surface>
            token outputs:surface.connect = </World/_materials/M/FallbackShader.outputs:surface>

            def Shader "OpenPBRShader"
            {
                uniform token info:id = "ND_open_pbr_surface_surfaceshader"
                color3f inputs:base_color = (0.1, 0.05, 0.02)
                float inputs:base_weight = 1.0
                float inputs:base_metalness = 0.0
                float inputs:specular_weight = 1.0
                float inputs:specular_roughness = 0.4
                float inputs:emission_luminance = 1.0
CONNECT                token outputs:surface
            }

            def Shader "FallbackShader"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.1, 0.05, 0.02)
                float inputs:roughness = 0.4
                float inputs:metallic = 0.0
                token outputs:surface
            }
GRAPH        }
    }
}
"""

SCALAR_GRAPH = '''
            def NodeGraph "NodeGraph"
            {
                float outputs:lane_out.connect = </World/_materials/M/NodeGraph/Constant.outputs:out>

                def Shader "Constant"
                {
                    uniform token info:id = "ND_constant_float"
                    float inputs:value = LANEVALUE
                    float outputs:out
                }
            }
'''

COLOR_GRAPH = '''
            def NodeGraph "NodeGraph"
            {
                color3f outputs:lane_out.connect = </World/_materials/M/NodeGraph/Constant.outputs:out>

                def Shader "Constant"
                {
                    uniform token info:id = "ND_constant_color3"
                    color3f inputs:value = LANEVALUE
                    color3f outputs:out
                }
            }
'''

LANES = {
    # lane: (shader input, graph type, low value, high value)
    'metal': ('inputs:base_metalness', 'float', '0.0', '0.8'),
    'rough': ('inputs:specular_roughness', 'float', '0.05', '0.9'),
    'emit': ('inputs:emission_color', 'color3f', '(0, 0, 0)', '(0, 1, 0)'),
}


def scene(name, lane, high):
    decl, ty, low, hi = LANES[lane]
    text = HEADER
    connect = (f'                {ty} {decl}.connect = '
               '</World/_materials/M/NodeGraph.outputs:lane_out>\n')
    graph = (SCALAR_GRAPH if ty == 'float' else COLOR_GRAPH).replace(
        'LANEVALUE', hi if high else low)
    text = text.replace('CONNECT', connect).replace('GRAPH', graph)
    (root / f'{name}.usda').write_text(text)


for lane in ('metal', 'rough', 'emit'):
    scene(f'{lane}_low', lane, False)
    scene(f'{lane}_high', lane, True)
PY

if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  XVFB_PREFIX="xvfb-run -a"
else
  XVFB_PREFIX=""
fi

render() {
  local name="$1" image="$OUT/$1.ppm" log="$OUT/$1.log"
  # shellcheck disable=SC2086
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-180s}" \
      ${XVFB_PREFIX} "$BIN" --headless --backend vk --cpu-rt --frames 1 \
      --size 96x96 --screenshot "$image" "$OUT/$name.usda" >"$log" 2>&1
  else
    ${XVFB_PREFIX} "$BIN" --headless --backend vk --cpu-rt --frames 1 \
      --size 96x96 --screenshot "$image" "$OUT/$name.usda" >"$log" 2>&1
  fi
  if grep -Eiq 'renderer init failed: no Vulkan|no Vulkan physical device|Vulkan.*unavailable' "$log"; then
    echo "SKIP: Vulkan unavailable"; exit "$SKIP"
  fi
  grep -q 'CPU RT wrote' "$log" || {
    echo "FAIL: CPU RT did not own the screenshot for $name"
    tail -30 "$log"
    exit 1
  }
  [ -s "$image" ] || { echo "FAIL: missing screenshot for $name"; exit 1; }
}

for name in metal_low metal_high rough_low rough_high emit_low emit_high; do
  render "$name"
done

python3 - "$OUT" <<'PY'
import pathlib
import sys

out = pathlib.Path(sys.argv[1])


def pixels(name):
    data = (out / f'{name}.ppm').read_bytes()
    parts = data.split(maxsplit=4)
    if parts[0] != b'P6' or int(parts[3]) != 255:
        raise SystemExit(f'FAIL: {name} is not a binary PPM')
    body = parts[4]
    if len(body) != int(parts[1]) * int(parts[2]) * 3:
        raise SystemExit(f'FAIL: truncated PPM for {name}')
    return body


def mean_abs_diff(a, b):
    pa, pb = pixels(a), pixels(b)
    return sum(abs(x - y) for x, y in zip(pa, pb)) / len(pa)


for lane in ('metal', 'rough', 'emit'):
    diff = mean_abs_diff(f'{lane}_low', f'{lane}_high')
    print(f'{lane}: graph-lane low/high mean abs diff {diff:.3f}')
    if diff < 1.0:
        raise SystemExit(f'FAIL: the {lane} graph lane did not change the render')
print('PASS: CPU preview consumes MaterialX metallic/roughness/emission routes')
PY
