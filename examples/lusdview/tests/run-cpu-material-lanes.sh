#!/usr/bin/env bash
# CPU preview semantic material lanes.
#
# The CPU reference renderer used to consume only base colour, normal and
# opacity textures; a textured metallic/roughness/emissive/specular/coat/
# occlusion lane silently used the neutralized constant (usually 1), so the
# preview disagreed with the shaders. The loader neutralizes a lane's constant
# to 1 whenever a texture is bound, so a uniform texture of value V must shade
# exactly like the same material with a constant V.
#
# The occlusion lane has no constant equivalent; a fully white map must be
# neutral and a 0.4 map must darken the ambient term.
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


for name, rgb in (('metal', (153, 153, 153)),      # 0.6
                  ('rough', (102, 102, 102)),      # 0.4
                  ('emit', (0, 255, 0)),
                  ('spec', (255, 0, 0)),
                  ('coat', (153, 153, 153)),       # 0.6
                  ('occl_white', (255, 255, 255)),
                  ('occl_dark', (102, 102, 102))):  # 0.4
    write_bmp(root / f'{name}.bmp', rgb)

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
        def Material "M"
        {
            token outputs:surface.connect = </World/_materials/M/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.8, 0.2, 0.1)
                float inputs:roughness = 0.4
                float inputs:metallic = 0.0
                float inputs:clearcoat = 0.0
                float inputs:clearcoatRoughness = 0.2
                float inputs:specular = 1.0
EXTRA                token outputs:surface
            }

            def Shader "stReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }
SCALARS        }
    }
}
"""

TEXTURE = '''            def Shader "{name}"
            {{
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./{file}@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/stReader.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float outputs:r
                float outputs:g
                float outputs:b
                float3 outputs:rgb
            }}
'''

LANES = {
    # lane: (input declaration, type, texture shader, texture file, constant)
    'metal': ('inputs:metallic', 'float', 'MetalTex', 'metal', '0.6', 'r'),
    'rough': ('inputs:roughness', 'float', 'RoughTex', 'rough', '0.4', 'r'),
    'emit': ('inputs:emissiveColor', 'color3f', 'EmitTex', 'emit',
             '(0, 1, 0)', 'rgb'),
    'spec': ('inputs:specularColor', 'color3f', 'SpecTex', 'spec',
             '(1, 0, 0)', 'rgb'),
    'coat': ('inputs:clearcoat', 'float', 'CoatTex', 'coat', '0.6', 'r'),
    'occl': ('inputs:occlusion', 'float', 'OcclTex', 'occl_white', '1.0', 'r'),
}


def scene(name, lane, textured, file_override=None, ambient_only=False,
          value_override=None):
    decl, ty, shader, file, value, channel = LANES[lane]
    text = HEADER
    if lane == 'spec':
        # inputs:specularColor is consulted only when useSpecularWorkflow is on;
        # without the switch both scenes ignore the lane and the row is vacuous.
        # The schema type is int, not bool.
        text = text.replace(
            'float inputs:specular = 1.0',
            'float inputs:specular = 1.0\n'
            '                int inputs:useSpecularWorkflow = 1')
    if ambient_only:
        # Light points up, so the +Y plane receives no direct light: the render
        # is the ambient term only and a 0.4 occlusion map must scale it to 0.4.
        text = text.replace('(-45, 0, 45)', '(90, 0, 0)')
    if textured:
        extra = (f'                {ty} {decl}.connect = '
                 f'</World/_materials/M/{shader}.outputs:{channel}>\n')
        texture = TEXTURE.format(name=shader,
                                 file=file_override or f'{file}.bmp')
    else:
        extra = f'                {ty} {decl} = {value_override or value}\n'
        texture = ''
    text = text.replace('EXTRA', extra).replace('SCALARS', texture)
    (root / f'{name}.usda').write_text(text)


for lane in ('metal', 'rough', 'emit', 'spec', 'coat'):
    scene(f'{lane}_tex', lane, True)
    scene(f'{lane}_const', lane, False)
# Roughness must be *observable*, not merely equal to its constant: if the
# lane were ignored both of these would render identically. The packed
# specular colour used to zero the whole specular term, which made 0.03 and
# 1.0 byte-identical; this control keeps that regression from returning.
scene('rough_low', 'rough', False, value_override='0.03')
scene('rough_high', 'rough', False, value_override='1.0')
scene('occl_neutral', 'occl', True, 'occl_white.bmp', ambient_only=True)
scene('occl_none', 'occl', False, ambient_only=True)
scene('occl_dark', 'occl', True, 'occl_dark.bmp', ambient_only=True)
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

for name in metal_tex metal_const rough_tex rough_const emit_tex emit_const \
            spec_tex spec_const coat_tex coat_const \
            occl_neutral occl_none occl_dark rough_low rough_high; do
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


def center_luma(name):
    data = (out / f'{name}.ppm').read_bytes()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    body = parts[4]
    i = ((h // 2) * w + w // 2) * 3
    return 0.2126 * body[i] + 0.7152 * body[i + 1] + 0.0722 * body[i + 2]


for lane in ('metal', 'rough', 'emit', 'spec', 'coat'):
    diff = max_diff(f'{lane}_tex', f'{lane}_const')
    print(f'{lane}: texture-vs-constant max diff {diff}')
    if diff > 1:
        raise SystemExit(f'FAIL: the {lane} texture lane does not match its constant')

rough_range = max_diff('rough_low', 'rough_high')
print(f'roughness: low(0.03) vs high(1.0) max diff {rough_range}')
if rough_range < 20:
    raise SystemExit('FAIL: roughness does not change the render')

neutral = max_diff('occl_neutral', 'occl_none')
dark = center_luma('occl_dark') / max(center_luma('occl_neutral'), 1e-6)
print(f'occlusion: neutral diff {neutral}, dark/neutral centre luma {dark:.3f}')
if neutral > 1:
    raise SystemExit('FAIL: a fully white occlusion map changed the shading')
if dark > 0.8:
    raise SystemExit('FAIL: a 0.4 occlusion map did not darken the shaded point')
print('PASS: CPU semantic material lanes match their constant equivalents')
PY
