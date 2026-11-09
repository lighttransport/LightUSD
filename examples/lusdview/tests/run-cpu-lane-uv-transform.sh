#!/usr/bin/env bash
# Per-slot UV transforms on the non-base semantic lanes.
#
# The base-colour and normal lanes are covered by run-cpu-uv-transform.sh. The
# CPU preview also samples the metallic, roughness, occlusion, clearcoat and
# emissive texture lanes, and each of those must apply the slot's packed UV
# transform and UV-set selector exactly like the base lane does.
#
# The roughness lane only became observable once the packed specular colour was
# neutralised for the metallic workflow; before that every preview-surface
# material rendered with a zeroed specular lobe and roughness had no effect.
#
# Oracle per lane:
#   * `<lane>_xform` samples the swatch through UsdTransform2d (translate +0.5
#     in u); `<lane>_shift` samples it with the equivalent pre-transformed `st`.
#     The two renders must match to within one channel step.
#   * `<lane>_const` drops the lane's texture connection entirely and keeps the
#     authored constant, so the comparison above cannot be vacuous: the
#     textured render must differ from the constant render.
#
# The light intensity is deliberately below the saturation point: with the
# previous 3.0 the diffuse term alone clipped to white, which hid every lane
# difference in the 8-bit capture (the occlusion control measured 2 steps).
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

LANES = ('metallic', 'roughness', 'occlusion', 'clearcoat', 'emissive')


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


# High-contrast value ramp: every lane it drives must change shading visibly.
write_bmp(root / 'swatch.bmp', [[(20, 20, 20), (240, 240, 240)],
                                [(240, 240, 240), (20, 20, 20)]])

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
        float inputs:intensity = 0.5
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
                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
                float inputs:roughness = 0.35
                float inputs:metallic = 0.0
                float inputs:occlusion = 1.0
                float inputs:clearcoat = 0.0
                color3f inputs:emissiveColor = (0, 0, 0)
LANECONNECT                token outputs:surface
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
                float outputs:r
            }
        }
    }
}
"""

CONNECT = {
    'metallic': '                float inputs:metallic.connect = </World/_materials/M/Tex.outputs:r>\n',
    'roughness': '                float inputs:roughness.connect = </World/_materials/M/Tex.outputs:r>\n',
    'occlusion': '                float inputs:occlusion.connect = </World/_materials/M/Tex.outputs:r>\n',
    'clearcoat': '                float inputs:clearcoat.connect = </World/_materials/M/Tex.outputs:r>\n',
    'emissive': '                color3f inputs:emissiveColor.connect = </World/_materials/M/Tex.outputs:rgb>\n',
}

BASE_UV = [(0, 0), (0, 1), (1, 1), (1, 0)]
SHIFTED = [(u + 0.5, v) for u, v in BASE_UV]
IDENTITY = [(0.0, 0.0)] * 4


def scene(name, st, st1, src, lane, connect):
    text = HEADER
    text = text.replace('(ST)', ', '.join(f'({u}, {v})' for u, v in st))
    text = text.replace('(ST1)', ', '.join(f'({u}, {v})' for u, v in st1))
    text = text.replace('BASEST', src)
    text = text.replace('LANECONNECT', CONNECT[lane] if connect else '')
    (root / f'{name}.usda').write_text(text)


for lane in LANES:
    scene(f'{lane}_xform', BASE_UV, IDENTITY, 'stXform', lane, True)
    scene(f'{lane}_shift', SHIFTED, IDENTITY, 'stReader', lane, True)
    scene(f'{lane}_const', BASE_UV, IDENTITY, 'stReader', lane, False)
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

for lane in metallic roughness occlusion clearcoat emissive; do
  for variant in xform shift const; do
    render "${lane}_${variant}"
  done
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


for lane in ('metallic', 'roughness', 'occlusion', 'clearcoat', 'emissive'):
    diff = max_diff(f'{lane}_xform', f'{lane}_shift')
    print(f'{lane}: transform vs pre-transformed st, max channel diff {diff}')
    if diff > 1:
        raise SystemExit(f'FAIL: {lane} does not match the pre-transformed-st reference')
    applied = max_diff(f'{lane}_xform', f'{lane}_const')
    print(f'{lane}: texture effect vs constant lane {applied}')
    if applied < 5:
        raise SystemExit(f'FAIL: the {lane} texture did not change shading '
                         f'({applied} < 5); the comparison is vacuous')

print('PASS: non-base semantic lanes apply the per-slot UV transform '
      'and match the pre-transformed-UV reference')
PY
