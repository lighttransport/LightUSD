#!/usr/bin/env bash
# CPU preview MaterialX graph *topology*: a multi-node chain must evaluate to
# the same shading as the constant it computes to.
#
# The existing graph tests change one node's value and require the image to
# change (route response). That cannot tell a correctly evaluated chain from a
# renderer that stops after the first node. This test pins the whole chain:
#
#   image(1x1 raw 0.6,0.4,0.2)
#     -> multiply by (0.5,0.5,0.5)
#     -> mix(bg=(0.1,0.2,0.3), factor 0.5)
#     -> OpenPBR base_color
#
# which computes (0.2,0.2,0.2). The reference scene authors that constant
# directly with no node graph at all, so the two renders must match to within
# one channel step. Dropping or mis-evaluating any link changes the colour.
#
# A shallow control (image only, no multiply/mix) must differ from the chain,
# proving the two extra nodes participate rather than being optimised away.
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
import struct
import sys

root = pathlib.Path(sys.argv[1])


def write_bmp(path, rgb):
    r, g, b = rgb
    data = bytes((b, g, r)) + b'\0'
    header = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, 1, 1, 1, 24, 0, len(data),
                       2835, 2835, 0, 0)
    path.write_bytes(header + info + data)


# Pure white: sRGB and linear decoding agree exactly, so the oracle does
# not depend on the image's colour-space interpretation.
write_bmp(root / 'tex.bmp', (255, 255, 255))

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
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
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
BASECOLOR
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
BASECOLOR2
                float inputs:roughness = 0.4
                float inputs:metallic = 0.0
                token outputs:surface
            }
GRAPH        }
    }
}
"""

IMAGE_NODE = '''
                def Shader "Image"
                {
                    uniform token info:id = "ND_image_color3"
                    asset inputs:file = @./tex.bmp@ (
                        colorSpace = "raw"
                    )
                    string inputs:uaddressmode = "periodic"
                    string inputs:vaddressmode = "periodic"
                    color3f outputs:out
                }
'''

CHAIN_GRAPH = '''
            def NodeGraph "NodeGraph"
            {
                color3f outputs:out.connect = </World/_materials/M/NodeGraph/Mix.outputs:out>

                def Shader "Image"
                {
                    uniform token info:id = "ND_image_color3"
                    asset inputs:file = @./tex.bmp@ (
                        colorSpace = "raw"
                    )
                    string inputs:uaddressmode = "periodic"
                    string inputs:vaddressmode = "periodic"
                    color3f outputs:out
                }

                def Shader "Scale"
                {
                    uniform token info:id = "ND_multiply_color3"
                    color3f inputs:in1.connect = </World/_materials/M/NodeGraph/Image.outputs:out>
                    color3f inputs:in2 = (0.5, 0.5, 0.5)
                    color3f outputs:out
                }

                def Shader "Mix"
                {
                    uniform token info:id = "ND_mix_color3"
                    color3f inputs:fg.connect = </World/_materials/M/NodeGraph/Scale.outputs:out>
                    color3f inputs:bg = (0.2, 0.2, 0.2)
                    float inputs:mix = 0.5
                    color3f outputs:out
                }
            }
'''

SHALLOW_GRAPH = '''
            def NodeGraph "NodeGraph"
            {
                color3f outputs:out.connect = </World/_materials/M/NodeGraph/Image.outputs:out>
''' + IMAGE_NODE + '''            }
'''


def scene(name, base_color, graph):
    text = HEADER
    if graph:
        connect = ('                color3f inputs:emission_color.connect = '
                   '</World/_materials/M/NodeGraph.outputs:out>\n')
        text = text.replace('BASECOLOR\n', '').replace('BASECOLOR2\n', '')
        text = text.replace('CONNECT', connect).replace('GRAPH', graph)
    else:
        decl = f'                color3f inputs:emission_color = {base_color}'
        text = text.replace('BASECOLOR', decl)
        text = text.replace('BASECOLOR2', decl)
        text = text.replace('CONNECT', '').replace('GRAPH', '')
    (root / f'{name}.usda').write_text(text)


# white * 0.5 = 0.5 ; mix(0.5, 0.2, 0.5) = 0.35, exactly, under any decode.
scene('chain', None, CHAIN_GRAPH)
scene('constant', '(0.35, 0.35, 0.35)', '')
scene('shallow', None, SHALLOW_GRAPH)
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

for name in chain constant shallow; do
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


def max_diff(a, b):
    return max(abs(x - y) for x, y in zip(pixels(a), pixels(b)))


diff = max_diff('chain', 'constant')
print(f'image->multiply->mix vs constant-equivalent: max channel diff {diff}')
if diff > 1:
    raise SystemExit('FAIL: the MaterialX chain does not evaluate to its '
                     'constant equivalent')
control = max_diff('chain', 'shallow')
print(f'chain vs image-only control: {control}')
if control < 3:
    raise SystemExit('FAIL: the multiply/mix links did not change the render; '
                     'the chain is not being evaluated')
print('PASS: deep MaterialX chain matches its constant equivalent and the '
      'intermediate links participate')
PY
