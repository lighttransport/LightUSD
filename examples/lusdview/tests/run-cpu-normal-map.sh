#!/usr/bin/env bash
# CPU RT tangent-space normal mapping: a MaterialX normal map must shade exactly
# like the same world-space normal authored as geometry normals, and a flat map
# must be neutral.
#
# The fixture is generated here (no binary texture assets): a ground plane whose
# UV frame is T=+X, B=-Z, N=+Y. A `normalmap(constant (0.8, 0.5, 0.9))` graph
# decodes to (0.6, 0, 0.8), i.e. world normal (0.6, 0.8, 0), which is also
# authored directly on a second scene. Both must render the same pixels.
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
        normal3f[] normals = [NORMAL, NORMAL, NORMAL, NORMAL] (
            interpolation = "faceVarying"
        )
        point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        texCoord2f[] primvars:st = [(ST)] (
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
CONNECT                token outputs:surface
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
                float3 outputs:normal_out.connect = </World/_materials/M/NodeGraph/NormalMapNode.outputs:out>

                def Shader "NormalConstant"
                {
                    uniform token info:id = "ND_constant_color3"
                    color3f inputs:value = (RVALUE)
                    color3f outputs:out
                }

                def Shader "NormalMapNode"
                {
                    uniform token info:id = "ND_normalmap_float"
                    float3 inputs:in.connect = </World/_materials/M/NodeGraph/NormalConstant.outputs:out>
                    float inputs:scale = 1.0
                    float3 outputs:out
                }
            }
"""

CONNECT = ("                float3 inputs:geometry_normal.connect = "
           "</World/_materials/M/NodeGraph.outputs:normal_out>\n")

STD = [(0, 0), (0, 1), (1, 1), (1, 0)]
# UV set with the U/V axis swapped, so the UV determinant is negative.
MIRRORED = [(0, 0), (1, 0), (1, 1), (0, 1)]

cases = {
    # No normal opinion at all: the reference for a flat map.
    'none': ('(0, 1, 0)', None, STD),
    # Flat tangent-space map (decodes to (0, 0, 1)): must be neutral.
    'flat': ('(0, 1, 0)', (0.5, 0.5, 1.0), STD),
    # Decodes to (0.6, 0, 0.8) -> world (0.6, 0.8, 0) for T=+X, B=-Z, N=+Y.
    'map_tilt': ('(0, 1, 0)', (0.8, 0.5, 0.9), STD),
    # The same world normal authored as geometry normals.
    'geom_tilt': ('(0.6, 0.8, 0)', None, STD),
    # Mirrored UVs make the UV frame T=-Z, B=-X after the determinant flip, so
    # the tangent-space (0, 0.6, 0.8) maps to (-0.6, 0.8, 0). Without the flip
    # it would be (+0.6, 0.8, 0).
    'map_mirror': ('(0, 1, 0)', (0.5, 0.8, 0.9), MIRRORED),
    'geom_mirror': ('(-0.6, 0.8, 0)', None, MIRRORED),
}
for name, (normal, value, st) in cases.items():
    text = HEADER.replace('NORMAL', normal)
    text = text.replace('(ST)', ', '.join(f'({u}, {v})' for u, v in st))
    if value is None:
        text = text.replace('CONNECT', '').replace('GRAPH', '')
    else:
        text = text.replace('CONNECT', CONNECT)
        text = text.replace('GRAPH', GRAPH.replace(
            'RVALUE', f'{value[0]}, {value[1]}, {value[2]}'))
    (out / f'{name}.usda').write_text(text)
PY

render() {
  local name="$1" image="$OUT/$1.ppm" log="$OUT/$1.log"
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

# The CPU tracer still needs a Vulkan context for the headless presentation, so
# provide a virtual display when the session has none.
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  XVFB_PREFIX="xvfb-run -a"
else
  XVFB_PREFIX=""
fi

for name in none flat map_tilt geom_tilt map_mirror geom_mirror; do
  render "$name"
done

python3 - "$OUT" <<'PY'
import pathlib
import sys

out = pathlib.Path(sys.argv[1])

def pixels(name):
    data = (out / f'{name}.ppm').read_bytes()
    parts = data.split(maxsplit=4)
    if parts[0] != b'P6':
        raise SystemExit(f'FAIL: {name} is not a binary PPM')
    w, h, maxv = int(parts[1]), int(parts[2]), int(parts[3])
    if maxv != 255:
        raise SystemExit(f'FAIL: unexpected PPM maxval {maxv}')
    body = parts[4]
    if len(body) != w * h * 3:
        raise SystemExit(f'FAIL: truncated PPM for {name}')
    return body

base = pixels('none')
flat = pixels('flat')
tilt = pixels('map_tilt')
geom = pixels('geom_tilt')
mirror = pixels('map_mirror')
geom_mirror = pixels('geom_mirror')

def max_channel_diff(a, b):
    return max(abs(x - y) for x, y in zip(a, b))

def mean_channel_diff(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)

flat_diff = max_channel_diff(base, flat)
tilt_diff = max_channel_diff(tilt, geom)
mirror_diff = max_channel_diff(mirror, geom_mirror)
applied = mean_channel_diff(base, tilt)
print(f'flat-vs-none max={flat_diff} tilt-vs-geom max={tilt_diff} '
      f'mirrored-vs-geom max={mirror_diff} map-vs-none mean={applied:.3f}')
if flat_diff > 1:
    raise SystemExit('FAIL: a flat normal map changed the shading')
if tilt_diff > 1:
    raise SystemExit('FAIL: the normal map does not match the same world normal '
                     'authored as geometry normals')
if mirror_diff > 1:
    raise SystemExit('FAIL: mirrored UVs do not flip the bitangent')
if applied < 5.0:
    raise SystemExit('FAIL: the normal map had no visible effect')
print('PASS: CPU RT normal map matches the equivalent geometric normal')
PY
