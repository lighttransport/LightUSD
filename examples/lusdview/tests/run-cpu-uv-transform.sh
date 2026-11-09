#!/usr/bin/env bash
# CPU texture UV transforms and UV-set routing.
#
# The Vulkan shaders apply each semantic slot's packed UV transform and UV-set
# selector. The CPU reference path sampled raw UV0 for every lane, so an
# authored texture transform (or a texture bound to `st1`) previewed
# differently from the GPU.
#
# Oracle: a transformed slot must shade exactly like the same texture sampled
# with pre-transformed `st` values, and a slot routed to `st1` must shade
# exactly like the equivalent `st`. The normal-map scenes also prove the
# tangent frame follows the lane's transformed UVs, and a no-normal control
# proves the comparison is not vacuous.
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


def write_bmp(path, rows):
    """24-bit BMP; rows are top->bottom of (r, g, b)."""
    h = len(rows)
    w = len(rows[0])
    data = b''
    for row in reversed(rows):
        line = b''.join(bytes((b, g, r)) for r, g, b in row)
        data += line + b'\0' * ((4 - len(line) % 4) % 4)
    header = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(data),
                       2835, 2835, 0, 0)
    path.write_bytes(header + info + data)


write_bmp(root / 'swatch.bmp', [[(255, 0, 0), (0, 255, 0)],
                                [(0, 0, 255), (255, 255, 255)]])
write_bmp(root / 'normal.bmp', [[(128, 128, 255), (255, 128, 128)],
                                [(128, 255, 128), (255, 255, 128)]])

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
        texCoord2f[] primvars:st = [(ST)] (
            interpolation = "faceVarying"
        )
        texCoord2f[] primvars:st1 = [(ST1)] (
            interpolation = "faceVarying"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/_materials/M>
    }

    def Scope "_materials"
    {
        def Material "M"
        {
            token outputs:surface.connect = </World/_materials/M/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/_materials/M/Tex.outputs:rgb>
NORMALCONNECT                float inputs:roughness = 1.0
                float inputs:metallic = 0.0
                token outputs:surface
            }

            def Shader "stReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }

            def Shader "st1Reader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st1"
                float2 outputs:result
            }

            def Shader "stXform"
            {
                uniform token info:id = "UsdTransform2d"
                float2 inputs:in.connect = </World/_materials/M/stReader.outputs:result>
                float inputs:rotation = 0
                float2 inputs:scale = (1, 1)
                float2 inputs:translation = (0.5, 0)
                float2 outputs:result
            }

            def Shader "Tex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./swatch.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/BASEST.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float3 outputs:rgb
            }

            def Shader "NormalTex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./normal.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/NORMALST.outputs:result>
                float4 inputs:scale = (2, 2, 2, 1)
                float4 inputs:bias = (-1, -1, -1, 0)
                float3 outputs:rgb
            }
        }
    }
}
"""

NORMAL_CONNECT = ('                normal3f inputs:normal.connect = '
                  '</World/_materials/M/NormalTex.outputs:rgb>\n')
BASE_UV = [(0, 0), (0, 1), (1, 1), (1, 0)]
SHIFTED = [(u + 0.5, v) for u, v in BASE_UV]
IDENTITY = [(0.0, 0.0)] * 4


def scene(name, st, st1, base_src, normal_src, use_normal):
    text = HEADER
    text = text.replace('(ST)', ', '.join(f'({u}, {v})' for u, v in st))
    text = text.replace('(ST1)', ', '.join(f'({u}, {v})' for u, v in st1))
    text = text.replace('BASEST', base_src).replace('NORMALST', normal_src)
    text = text.replace('NORMALCONNECT', NORMAL_CONNECT if use_normal else '')
    (root / f'{name}.usda').write_text(text)


# Base-color lane: transform vs pre-shifted st, then UV set 1 vs pre-shifted st.
scene('base_xform', BASE_UV, IDENTITY, 'stXform', 'stXform', False)
scene('base_shift', SHIFTED, IDENTITY, 'stReader', 'stReader', False)
scene('base_uv1', BASE_UV, SHIFTED, 'st1Reader', 'st1Reader', False)
# Normal lane: same two comparisons; the frame must use the same UVs.
scene('norm_xform', BASE_UV, IDENTITY, 'stXform', 'stXform', True)
scene('norm_shift', SHIFTED, IDENTITY, 'stReader', 'stReader', True)
scene('norm_uv1', BASE_UV, SHIFTED, 'st1Reader', 'st1Reader', True)
# Control: no normal map, so the normal comparisons must not be vacuous.
scene('norm_none', SHIFTED, IDENTITY, 'stReader', 'stReader', False)
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

for name in base_xform base_shift base_uv1 norm_xform norm_shift norm_uv1 norm_none; do
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


pairs = [('base_xform', 'base_shift'), ('base_uv1', 'base_shift'),
         ('norm_xform', 'norm_shift'), ('norm_uv1', 'norm_shift')]
for a, b in pairs:
    diff = max_diff(a, b)
    print(f'{a} vs {b}: max channel diff {diff}')
    if diff > 1:
        raise SystemExit(f'FAIL: {a} does not match {b}')
applied = max_diff('norm_shift', 'norm_none')
print(f'normal-map effect: {applied}')
if applied < 5:
    raise SystemExit('FAIL: the normal map did not change shading')
print('PASS: CPU texture UV transforms and UV-set routing match the GPU contract')
PY
