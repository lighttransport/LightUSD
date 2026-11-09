#!/usr/bin/env bash
# CPU OpenPBR coat-normal lane.
#
# The CPU preview sampled the surface normal map but not the separate OpenPBR
# coat normal (`matTex` slot 11). A material that authored a coat normal map
# therefore shaded its coat lobe with the surface normal, so a tilted coat map
# was indistinguishable from no map.
#
# Oracle: with the base and specular lobes disabled, the image is the coat
# response. A tilted coat map must shade exactly like the same world normal
# authored as geometry normals, a flat coat map must be neutral, and the map
# must have no effect when the coat weight is zero.
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
    """2x2 24-bit BMP with a uniform colour (bilinear sampling is constant)."""
    r, g, b = rgb
    rows = [[(r, g, b)] * 2] * 2
    data = b''
    for row in reversed(rows):
        line = b''.join(bytes((bb, gg, rr)) for rr, gg, bb in row)
        data += line + b'\0' * ((4 - len(line) % 4) % 4)
    header = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, 2, 2, 1, 24, 0, len(data),
                       2835, 2835, 0, 0)
    path.write_bytes(header + info + data)


write_bmp(root / 'coat_flat.bmp', (128, 128, 255))   # decodes to (0, 0, 1)
write_bmp(root / 'coat_pos.bmp', (204, 128, 230))    # decodes to (0.6, 0, 0.8)
write_bmp(root / 'coat_neg.bmp', (51, 128, 230))     # decodes to (-0.6, 0, 0.8)

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
        normal3f[] normals = [(NX,NY,NZ), (NX,NY,NZ), (NX,NY,NZ), (NX,NY,NZ)] (
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
            token outputs:surface.connect = </World/_materials/M/P.outputs:surface>

            def Shader "P"
            {
                uniform token info:id = "ND_open_pbr_surface_surfaceshader"
                color3f inputs:base_color = (0, 0, 0)
                float inputs:base_weight = 1.0
                float inputs:specular_weight = 0.0
                float inputs:specular_roughness = 1.0
                float inputs:coat_weight = @COATWEIGHT@
                float inputs:coat_roughness = 0.2
                color3f inputs:coat_color = (1, 1, 1)
@COATCONNECT@                token outputs:surface
            }
            def Shader "CoatNormal"
            {
                uniform token info:id = "ND_image_color3"
                asset inputs:file = @./COATFILE@
                color3f outputs:out
            }
        }
    }
}
"""

cases = {
    # Reference: no coat map, surface normal only.
    'coat_none': ((0, 1, 0), None, 1.0),
    # Flat coat map must be neutral.
    'coat_flat': ((0, 1, 0), 'coat_flat.bmp', 1.0),
    # Tilted coat maps on a flat surface.
    'coat_pos': ((0, 1, 0), 'coat_pos.bmp', 1.0),
    'coat_neg': ((0, 1, 0), 'coat_neg.bmp', 1.0),
    # The same world normal authored as geometry normals, no coat map.
    'geom_pos': ((0.6, 0.8, 0), None, 1.0),
    # Coat weight zero: the tilted map must not change the image.
    'coat_pos_zero': ((0, 1, 0), 'coat_pos.bmp', 0.0),
    'coat_neg_zero': ((0, 1, 0), 'coat_neg.bmp', 0.0),
}

CONNECT = ("                normal3f inputs:geometry_coat_normal.connect = "
           "</World/_materials/M/CoatNormal.outputs:out>\n")
for name, (normal, texture, coat) in cases.items():
    text = HEADER.replace('NX,NY,NZ', ', '.join(str(v) for v in normal))
    text = text.replace('@COATWEIGHT@', str(coat))
    text = text.replace('COATFILE', texture or 'coat_flat.bmp')
    text = text.replace('@COATCONNECT@', CONNECT if texture else '')
    (root / f'{name}.usda').write_text(text)
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

for name in coat_none coat_flat coat_pos coat_neg geom_pos coat_pos_zero coat_neg_zero; do
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


def mean_luma(name):
    body = pixels(name)
    return sum(0.2126 * body[i] + 0.7152 * body[i + 1] + 0.0722 * body[i + 2]
               for i in range(0, len(body), 3)) / (len(body) // 3)


flat = max_diff('coat_flat', 'coat_none')
tilt = max_diff('coat_pos', 'coat_neg')
geom = max_diff('coat_pos', 'geom_pos')
zero = max_diff('coat_pos_zero', 'coat_neg_zero')
luma = mean_luma('coat_pos')
print(f'flat coat map vs none: {flat}')
print(f'tilted coat map +/-: {tilt}')
print(f'tilted coat map vs geometry control: {geom}')
print(f'coat weight 0 +/-: {zero} (image luma {luma:.2f})')
if flat > 1:
    raise SystemExit('FAIL: a flat coat normal map changed the shading')
if tilt < 5:
    raise SystemExit('FAIL: the coat normal map did not change the coat lobe')
if geom > 1:
    raise SystemExit('FAIL: the coat map does not match the same world normal '
                     'authored as geometry normals')
if zero > 1:
    raise SystemExit('FAIL: the coat normal map changed a non-coat lobe')
if luma < 4.0:
    raise SystemExit('FAIL: the coat image is too dark to be meaningful')
print('PASS: CPU coat-normal lane uses its own map/frame and only the coat lobe')
PY
