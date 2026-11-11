#!/usr/bin/env bash
# Second UV set (multi-UV) on the CPU preview paths.
#
# A mesh may author more than one 2-component UV primvar (`st`, `st1`, `uvSet1`,
# `map2`, ...). A texture's UsdPrimvarReader selects which one it samples, and
# the renderer must honour that selection rather than always using the first set.
#
# Both the viewer's CPU ray tracer (`lusdview --cpu-rt`, the `core` converter)
# and the standalone CPU renderer (`lusdrender`, the core converter) are
# checked against the same scenes.
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
# Neutral base-colour texture for the mixed-lane scenes.
write_bmp(root / 'white.bmp', [[(255, 255, 255)]])
# Tangent-space normal maps: a strong +u tilt and a flat normal.
write_bmp(root / 'tilt.bmp', [[(243, 128, 199)]])
write_bmp(root / 'flat.bmp', [[(128, 128, 255)]])

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


# --- Mixed lanes: one material, two textured lanes on different UV sets. ---
# The base-colour lane reads `st`; the emissive swatch reads `st1`. With only
# two sets on the mesh, the legacy renderer used to sample the base lane's UV
# for every lane, so the emissive texture ignored `st1`. The comparison below
# requires the mixed scene to match the same lookup written into `st`.
MIXED_TEMPLATE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "World"
{
    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "faceVarying"
        )
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
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

            def Shader "baseReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "DIFFVAR"
                float2 outputs:result
            }

            def Shader "emitReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "EMITVAR"
                float2 outputs:result
            }

            def Shader "Base"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./white.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/baseReader.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float3 outputs:rgb
            }

            def Shader "Emit"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./swatch.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/emitReader.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float3 outputs:rgb
            }

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/_materials/M/Base.outputs:rgb>
                color3f inputs:emissiveColor.connect = </World/_materials/M/Emit.outputs:rgb>
                float inputs:roughness = 1.0
                float inputs:metallic = 0.0
                token outputs:surface
            }
        }
    }
}
"""


def mixed_scene(name, diffuse_var, emissive_var, st, st1):
    text = (MIXED_TEMPLATE.replace('DIFFVAR', diffuse_var)
                          .replace('EMITVAR', emissive_var)
                          .replace('ST1', st1)
                          .replace('ST ', st))
    (root / f'{name}.usda').write_text(text)


# Base colour on `st`, emissive on `st1` (transposed).
mixed_scene('mixed', 'st', 'st1', IDENTITY, TRANSPOSED)
# Same two lookups written against `st` only, with `st` transposed.
mixed_scene('mixed_pre', 'st', 'st', TRANSPOSED, TRANSPOSED)
# Non-vacuous control: emissive reads the identity `st1`.
mixed_scene('mixed_ctl', 'st', 'st1', IDENTITY, IDENTITY)

# --- Normal maps on a second UV set: the tangent basis must follow the set the
# normal texture samples. A constant tilted tangent-space normal is rotated by
# the UV mapping, so the same lookup must match whether it is written as `st1`
# or as a pre-transformed `st`.
# An oblique key illuminates both tangent directions. An axis-aligned key
# makes the transposed-UV tilt control vacuous on the CPU renderer.
NORMAL_TEMPLATE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "World"
{
    def DistantLight "Key"
    {
        float inputs:intensity = 6.0
        float3 xformOp:rotateXYZ = (45, -45, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }

    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "faceVarying"
        )
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
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

            def Shader "baseReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }

            def Shader "nrmReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "NRMVAR"
                float2 outputs:result
            }

            def Shader "Base"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./white.bmp@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/baseReader.outputs:result>
                float4 inputs:scale = (1, 1, 1, 1)
                float4 inputs:bias = (0, 0, 0, 0)
                float3 outputs:rgb
            }

            def Shader "Nrm"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./NRMFILE@ (
                    colorSpace = "raw"
                )
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/_materials/M/nrmReader.outputs:result>
                float4 inputs:scale = (2, 2, 2, 1)
                float4 inputs:bias = (-1, -1, -1, 0)
                float3 outputs:rgb
            }

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/_materials/M/Base.outputs:rgb>
                normal3f inputs:normal.connect = </World/_materials/M/Nrm.outputs:rgb>
                float inputs:roughness = 0.6
                float inputs:metallic = 0.0
                token outputs:surface
            }
        }
    }
}
"""


def normal_scene(name, nrm_var, nrm_file, st, st1):
    text = (NORMAL_TEMPLATE.replace('NRMVAR', nrm_var)
                             .replace('NRMFILE', nrm_file)
                             .replace('ST1', st1)
                             .replace('ST ', st))
    (root / f'{name}.usda').write_text(text)


# Normal map reads `st1` (transposed); base colour keeps `st` (identity).
normal_scene('normal_st1', 'st1', 'tilt.bmp', IDENTITY, TRANSPOSED)
# Same normal lookup written into `st` with `st` transposed.
normal_scene('normal_pre', 'st', 'tilt.bmp', TRANSPOSED, TRANSPOSED)
# Non-vacuous control: flat normal map, so the tilt must change the render.
normal_scene('normal_flat', 'st1', 'flat.bmp', IDENTITY, IDENTITY)
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

for name in st1 pretransformed control mixed mixed_pre mixed_ctl normal_st1 normal_pre normal_flat; do
  render "$name"
done

# The same three scenes through the CPU renderer (`lusdrender`). It used to
# ignore the second UV set entirely: the material's UV primvar was not registered
# by the legacy scene converter, so the mesh exported without texcoords and every
# texture lookup collapsed to (0, 0). The check below pins the fix from the
# converter side, independently of the viewer's `core` path.
RENDER="${LUSDRENDER:-$REPO_ROOT/build_ninja/tools/lusdrender/lusdrender}"
[ -x "$RENDER" ] || { echo "SKIP: lusdrender not found: $RENDER"; exit "$SKIP"; }

render_legacy() {
  local name="$1" image="$OUT/lr_$1.png" log="$OUT/lr_$1.log"
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-180s}" \
      "$RENDER" "$OUT/$name.usda" "$image" -w 96 -height 96 \
      -bg 0,0,0 -largeSceneProfile off -smooth >"$log" 2>&1
  else
    "$RENDER" "$OUT/$name.usda" "$image" -w 96 -height 96 \
      -bg 0,0,0 -largeSceneProfile off -smooth >"$log" 2>&1
  fi
  [ -s "$image" ] || {
    echo "FAIL: lusdrender did not write an image for $name"
    tail -30 "$log"
    exit 1
  }
}

for name in st1 pretransformed control mixed mixed_pre mixed_ctl normal_st1 normal_pre normal_flat; do
  render_legacy "$name"
done

# The same scenes through the Vulkan ray-query material path (-vkr), which now
# carries a second UV stream in the vertex ABI and selects it through the graph
# Texcoord node's second-set flag. Requires a device with VK_KHR_ray_query.
VKR=1
render_vk() {
  local name="$1" image="$OUT/vk_$1.png" log="$OUT/vk_$1.log"
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-180s}" \
      "$RENDER" "$OUT/$name.usda" "$image" -vkr -w 96 -height 96 \
      -bg 0,0,0 -largeSceneProfile off -smooth >"$log" 2>&1
  else
    "$RENDER" "$OUT/$name.usda" "$image" -vkr -w 96 -height 96 \
      -bg 0,0,0 -largeSceneProfile off -smooth >"$log" 2>&1
  fi
  [ -s "$image" ] && grep -q 'backend: LightRT VK' "$log"
}
for name in st1 pretransformed control mixed mixed_pre mixed_ctl normal_st1 normal_pre normal_flat; do
  if ! render_vk "$name"; then
    echo "SKIP: -vkr ray-query backend unavailable"
    VKR=0
    break
  fi
done

python3 - "$OUT" "$VKR" <<'PY'
import pathlib
import struct
import sys
import zlib

out = pathlib.Path(sys.argv[1])
vkr_enabled = len(sys.argv) > 2 and sys.argv[2] == '1'


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


def png_rgb(path):
    """Decode a non-interlaced 8-bit PNG to raw RGB bytes (stdlib only)."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit(f'FAIL: {path.name} is not a PNG')
    pos, idat, ihdr = 8, b'', None
    while pos + 8 <= len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', payload)
        elif kind == b'IDAT':
            idat += payload
        elif kind == b'IEND':
            break
    if ihdr is None:
        raise SystemExit(f'FAIL: {path.name} has no IHDR')
    w, h, depth, color, _, _, interlace = ihdr
    if depth != 8 or interlace != 0 or color not in (2, 6):
        raise SystemExit(f'FAIL: {path.name} is not 8-bit RGB/RGBA '
                         f'(depth={depth}, color={color}, interlace={interlace})')
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * channels
    out_bytes = bytearray(h * stride)
    prev = bytearray(stride)
    cur = 0
    for y in range(h):
        filt = raw[cur]
        cur += 1
        line = bytearray(raw[cur:cur + stride])
        cur += stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif filt == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif filt == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        elif filt != 0:
            raise SystemExit(f'FAIL: {path.name} uses filter {filt}')
        out_bytes[y * stride:(y + 1) * stride] = line
        prev = line
    if channels == 4:
        return bytes(v for i in range(0, len(out_bytes), 4)
                     for v in out_bytes[i:i + 3])
    return bytes(out_bytes)


def legacy_pixels(name):
    return png_rgb(out / f'lr_{name}.png')


def legacy_diff(a, b):
    return max(abs(x - y) for x, y in zip(legacy_pixels(a), legacy_pixels(b)))


def vk_pixels(name):
    return png_rgb(out / f'vk_{name}.png')


def vk_diff(a, b):
    return max(abs(x - y) for x, y in zip(vk_pixels(a), vk_pixels(b)))


same = legacy_diff('st1', 'pretransformed')
print(f'lusdrender: st1 vs the same lookup through st: max channel diff {same}')
if same > 1:
    raise SystemExit('FAIL: lusdrender did not resolve the material UV primvar '
                     '(legacy converter dropped the second UV set)')
control = legacy_diff('st1', 'control')
print(f'lusdrender: st1 vs identity-st control: {control}')
if control < 5:
    raise SystemExit('FAIL: lusdrender rendered the same image for st1 and the '
                     'identity control; the check is vacuous')


def check_mixed(label, diff_fn):
    same = diff_fn('mixed', 'mixed_pre')
    print(f'{label}: two lanes on different UV sets vs the same lookups '
          f'through st: max channel diff {same}')
    if same > 1:
        raise SystemExit(f'FAIL: {label} sampled one UV set for every lane; a '
                         'texture that names the second primvar must use it')
    control = diff_fn('mixed', 'mixed_ctl')
    print(f'{label}: mixed vs identity-st1 control: {control}')
    if control < 5:
        raise SystemExit(f'FAIL: {label} rendered the mixed scene and the '
                         'identity control identically; the check is vacuous')


def check_normal(label, diff_fn):
    same = diff_fn('normal_st1', 'normal_pre')
    print(f'{label}: normal map on st1 vs the same lookup through st: '
          f'max channel diff {same}')
    if same > 1:
        raise SystemExit(f'FAIL: {label} derived the normal-map tangent basis '
                         'from the wrong UV set')
    tilt = diff_fn('normal_st1', 'normal_flat')
    print(f'{label}: tilted vs flat normal map: {tilt}')
    if tilt < 5:
        raise SystemExit(f'FAIL: {label} ignored the normal map; the tilt '
                         'control is vacuous')


check_mixed('lusdview', max_diff)
check_mixed('lusdrender', legacy_diff)
check_normal('lusdview', max_diff)
check_normal('lusdrender', legacy_diff)
if vkr_enabled:
    same = vk_diff('st1', 'pretransformed')
    print(f'lusdrender -vkr: st1 vs the same lookup through st: '
          f'max channel diff {same}')
    if same > 1:
        raise SystemExit('FAIL: the vkr material path did not carry the second '
                         'UV set into the sampled texture')
    control = vk_diff('st1', 'control')
    print(f'lusdrender -vkr: st1 vs identity-st control: {control}')
    if control < 5:
        raise SystemExit('FAIL: vkr rendered st1 and the identity control '
                         'identically; the check is vacuous')
    check_mixed('lusdrender -vkr', vk_diff)
    check_normal('lusdrender -vkr', vk_diff)
else:
    print('SKIP: -vkr ray-query backend unavailable')

print('PASS: a texture can select the second UV set and matches the equivalent '
      'pre-transformed lookup, including one material mixing two UV sets')
PY
