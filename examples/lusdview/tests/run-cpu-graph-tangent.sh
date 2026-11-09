#!/usr/bin/env bash
# CPU MaterialX graph frame: `tangent` and `bitangent` nodes must report the
# hit triangle's actual frame, not fixed constants.
#
# The generated plane uses UVs with U along +X and V along +Z, so the frame is
# T=+X, B=N x T=-Z, N=+Y. A graph whose base color is `tangent * (1,0,0)` must
# therefore shade exactly like a constant base color of (1,0,0), and
# `bitangent * (0,0,-1)` like (0,0,1). The old fixed constants would produce
# (1,0,0) for both and fail the bitangent control.
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

out = pathlib.Path(sys.argv[1])
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
        float inputs:intensity = 3.0
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
        texCoord2f[] primvars:st = [(0, 0), (0, 1), (1, 1), (1, 0)] (
            interpolation = "faceVarying"
        )
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
                color3f inputs:base_color = (0.8, 0.8, 0.8)
                float inputs:base_weight = 1.0
                float inputs:base_metalness = 0.0
                float inputs:specular_weight = 0.0
                float inputs:specular_roughness = 1.0
BASECONNECT                token outputs:surface
            }

            def Shader "FallbackShader"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.8, 0.8, 0.8)
                float inputs:roughness = 1.0
                float inputs:metallic = 0.0
                token outputs:surface
            }
GRAPH        }
    }
}
"""

GRAPH = """
            def NodeGraph "NodeGraph"
            {
                float3 outputs:base_out.connect = </World/_materials/M/NodeGraph/Mul.outputs:out>

                def Shader "Node"
                {
                    uniform token info:id = "NODEID"
                    string inputs:space = "world"
                    float3 outputs:out
                }

                def Shader "Mul"
                {
                    uniform token info:id = "ND_multiply_vector3"
                    float3 inputs:in1.connect = </World/_materials/M/NodeGraph/Node.outputs:out>
                    float3 inputs:in2 = (PROJX, PROJY, PROJZ)
                    float3 outputs:out
                }
            }
"""

BASECONNECT = ("                color3f inputs:base_color.connect = "
               "</World/_materials/M/NodeGraph.outputs:base_out>\n")

cases = [
    # (name, node id or None for a constant, projection / constant color)
    ('graph_tangent', 'ND_tangent_vector3', (1.0, 0.0, 0.0)),
    ('ctrl_tangent', None, (1.0, 0.0, 0.0)),
    ('graph_bitangent', 'ND_bitangent_vector3', (0.0, 0.0, -1.0)),
    ('ctrl_bitangent', None, (0.0, 0.0, 1.0)),
]
for name, node_id, value in cases:
    text = HEADER
    if node_id:
        text = text.replace('BASECONNECT', BASECONNECT)
        graph = GRAPH.replace('NODEID', node_id)
        for axis, number in zip(('PROJX', 'PROJY', 'PROJZ'), value):
            graph = graph.replace(axis, str(number))
        text = text.replace('GRAPH', graph)
    else:
        text = text.replace('BASECONNECT', '').replace('GRAPH', '')
        text = text.replace('color3f inputs:base_color = (0.8, 0.8, 0.8)',
                            f'color3f inputs:base_color = '
                            f'({value[0]}, {value[1]}, {value[2]})')
    (out / f'{name}.usda').write_text(text)
PY

# The CPU tracer still needs a Vulkan context for the headless presentation.
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

for name in graph_tangent ctrl_tangent graph_bitangent ctrl_bitangent; do
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

for graph, ctrl in (('graph_tangent', 'ctrl_tangent'),
                    ('graph_bitangent', 'ctrl_bitangent')):
    a, b = pixels(graph), pixels(ctrl)
    diff = max(abs(x - y) for x, y in zip(a, b))
    print(f'{graph} vs {ctrl}: max channel diff {diff}')
    if diff > 1:
        raise SystemExit(f'FAIL: {graph} does not match the expected world vector')
print('PASS: MaterialX tangent/bitangent nodes use the triangle frame')
PY
