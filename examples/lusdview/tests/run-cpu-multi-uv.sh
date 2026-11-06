#!/usr/bin/env bash
# Second UV set (multi-UV) on the CPU preview path.
#
# A mesh may author more than one 2-component UV primvar (`st`, `st1`, `uvSet1`,
# `map2`, ...). A texture's UsdPrimvarReader selects which one it samples, and
# the renderer must honour that selection rather than always using the first set.
#
# Oracle, all three scenes share the same geometry and the same 2x2 swatch:
#   * `st1`            material reads `st1`; `st1` holds a transposed copy while
#                      `st` holds the identity mapping.
#   * `pretransformed` material reads `st`; `st` itself holds the transposed
#                      values. This is the same texture lookup written without a
#                      second UV set, so the two renders must match.
#   * `control`        material reads `st`; `st` holds the identity mapping. The
#                      transposed lookup must differ, so a renderer that ignores
#                      `st1` and always samples `st` is caught.
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


# Four distinct quadrants; a transposed lookup visibly rearranges them.
write_bmp(root / 'swatch.bmp', [[(255, 0, 0), (0, 255, 0)],
                                [(0, 0, 255), (255, 255, 255)]])

IDENTITY = '[(0, 0), (1, 0), (1, 1), (0, 1)]'
TRANSPOSED = '[(0, 0), (0, 1), (1, 1), (1, 0)]'

TEMPLATE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "World"
{
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
        texCoord2f[] primvars:st = ST ( interpolation = "faceVarying" )
        texCoord2f[] primvars:st1 = ST1 ( interpolation = "faceVarying" )
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/_materials/M>
    }

    def Scope "_materials"
    {
        def Material "M"
        {
            token outputs:surface.connect = </World/_materials/M/Surface.outputs:surface>

            def Shader "stReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "VARNAME"
                float2 outputs:result
            }

            def Shader "Tex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./swatch.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/stReader.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float3 outputs:rgb
            }

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:emissiveColor.connect = </World/_materials/M/Tex.outputs:rgb>
                float inputs:roughness = 1.0
                float inputs:metallic = 0.0
                token outputs:surface
            }
        }
    }
}
"""


def scene(name, varname, st, st1):
    text = (TEMPLATE.replace('VARNAME', varname)
                    .replace('ST1', st1)
                    .replace('ST ', st))
    (root / f'{name}.usda').write_text(text)


# Material selects the second set; the second set carries the transposed lookup.
scene('st1', 'st1', IDENTITY, TRANSPOSED)
# Same lookup, written against the first set only.
scene('pretransformed', 'st', TRANSPOSED, TRANSPOSED)
# Non-vacuous control: first set only, identity mapping.
scene('control', 'st', IDENTITY, IDENTITY)
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

for name in st1 pretransformed control; do
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


same = max_diff('st1', 'pretransformed')
print(f'st1 vs the same lookup through st: max channel diff {same}')
if same > 1:
    raise SystemExit('FAIL: the st1 primvar reader did not select the second '
                     'UV set (or the second set is not plumbed)')
control = max_diff('st1', 'control')
print(f'st1 vs identity-st control: {control}')
if control < 5:
    raise SystemExit('FAIL: the transposed lookup did not change the render; '
                     'the comparison is vacuous')
print('PASS: a texture can select the second UV set and matches the equivalent '
      'pre-transformed lookup')
PY
