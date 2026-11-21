#!/usr/bin/env bash
# CPU visible PTex material routing.
#
# The CPU reference tracer used to return white for every `isPtex` texture: it
# had no way to map an intrinsic Ptex face coordinate through the atlas face
# rectangle. A Ptex-bound Island material therefore previewed as a flat
# untextured surface while Vulkan RT showed the decoded face. This test binds a
# quad to the real Island PTex material chain (`PxrPtexture` /
# `HwPtexTexture` -> UsdPreviewSurface fallback), renders with the CPU and
# Vulkan RT backends, and checks the visible centre colour against the
# independently decoded face mean. A flat material control proves the colour
# difference comes from the texture.
#
# Island binds PTex through a referenced look layer: the texture shader lives in
# `looks/material.usda` but the `inputs:surfaceMap` asset value is authored by
# the *referencing* material. A relative path must anchor to that authoring
# layer, not to the referenced look layer. The generated layout reproduces that
# shape (`looks/`, `textures/`, `scene.usda`) so the test also guards the
# anchor-through-material-interface fix.
#
# The asset is pinned by hash and the test SKIPs when the Island corpus is not
# available, so CI without the dataset stays green.
#
# Exit codes: 0 = pass, 1 = mismatch, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found: $BIN"; exit "$SKIP"; }

PTX="${LIGHTUSD_PTEX_TEST_ASSET:-/mnt/nvme02/data/island/textures/isMountainA/Color/mountain_geo.ptx}"
PTX_SHA="9cdedfb99ab4462b934e125e196a8be08a079ad30c8b2e7896cd0b648d1c11c8"
# Decoded face 0 mean (RGB bytes) from the native PTex reader; see the
# independent six-file decode report for the raw comparison method.
FACE0_MEAN="52,77,40"

[ -f "$PTX" ] || { echo "SKIP: Island PTex asset not found: $PTX"; exit "$SKIP"; }
command -v sha256sum >/dev/null 2>&1 || { echo "SKIP: sha256sum unavailable"; exit "$SKIP"; }
[ "$(sha256sum "$PTX" | cut -d' ' -f1)" = "$PTX_SHA" ] || {
  echo "SKIP: Island PTex asset hash does not match the pinned corpus"; exit "$SKIP"; }

OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
if [ -z "${LUSDVIEW_TEST_OUT:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi

python3 - "$OUT" "$PTX" <<'PY'
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
ptx = sys.argv[2]

MATERIAL = f"""#usda 1.0
(
    defaultPrim = "BaseMaterial"
    upAxis = "Z"
)

def Material "BaseMaterial"
{{
    float inputs:alpha = 1
    color3f inputs:baseColor = (0.8, 0.2, 0.1)
    float inputs:roughness = 0.8
    float inputs:metallic = 0
    float inputs:ior = 1.5
    asset inputs:surfaceMap
    token outputs:ri:surface.connect = </BaseMaterial/PxrDisneyBsdf.outputs:bxdf_out>
    token outputs:surface.connect = </BaseMaterial/UsdPreviewSurface.outputs:surface>

    def Shader "PxrDisneyBsdf"
    {{
        uniform token info:id = "PxrDisneyBsdf"
        color3f inputs:baseColor.connect = </BaseMaterial/PxrPtexture_surface.outputs:resultRGB>
        float inputs:metallic.connect = </BaseMaterial.inputs:metallic>
        float inputs:roughness.connect = </BaseMaterial.inputs:roughness>
        float inputs:ior.connect = </BaseMaterial.inputs:ior>
        float inputs:opacity.connect = </BaseMaterial.inputs:alpha>
        token outputs:bxdf_out
    }}

    def Shader "PxrPtexture_surface"
    {{
        uniform token info:id = "PxrPtexture"
        asset inputs:filename.connect = </BaseMaterial.inputs:surfaceMap>
        int inputs:linearize = 0
        color3f outputs:resultRGB
    }}

    def Shader "UsdPreviewSurface"
    {{
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </BaseMaterial/HwPtexTexture_1.outputs:color>
        float inputs:roughness = 0.8
        float inputs:metallic = 0
        token outputs:surface
    }}

    def Shader "HwPtexTexture_1"
    {{
        uniform token info:id = "HwPtexTexture_1"
        asset inputs:file.connect = </BaseMaterial.inputs:surfaceMap>
        token inputs:sourceColorSpace = "sRGB"
        color3f outputs:color
    }}
}}
"""

SCENE = """#usda 1.0
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
        float3 xformOp:rotateXYZ = (0, 0, 0)
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
        point3f[] points = [(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/_materials/M>
    }

    def Scope "_materials"
    {
        def Material "M" (
            prepend references = @./mat.usda@</BaseMaterial>
        )
        {
        }
    }
}
"""

FLAT = """#usda 1.0
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
        float3 xformOp:rotateXYZ = (0, 0, 0)
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
        point3f[] points = [(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)]
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
                float inputs:roughness = 0.8
                float inputs:metallic = 0
                token outputs:surface
            }
        }
    }
}
"""

import shutil

(out / 'looks').mkdir()
(out / 'textures').mkdir()
(out / 'looks' / 'material.usda').write_text(MATERIAL)
# Relative texture binding authored by the referencing material, exactly like
# the Island element layers: `looks/` is not the anchor directory.
scene = SCENE.replace('@./mat.usda@</BaseMaterial>',
                      '@./looks/material.usda@</BaseMaterial>')
scene = scene.replace(
    'references = @./looks/material.usda@</BaseMaterial>\n        )\n        {',
    'references = @./looks/material.usda@</BaseMaterial>\n        )\n        {\n'
    '            asset inputs:surfaceMap = @./textures/mountain.ptx@', 1)
(out / 'scene.usda').write_text(scene)
(out / 'flat.usda').write_text(FLAT)
# Same PTex quad with a large dark backdrop so the auto camera frames the
# backdrop and the quad is minified: the CPU PTex sampler must select a
# non-base face mip.
backdrop = scene.replace(
    'def Xform "World"\n{',
    '''def Xform "World"
{
    def Mesh "Backdrop" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-120, -120, -40), (120, -120, -40),
                            (120, 120, -40), (-120, 120, -40)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/_materials/Backdrop>
    }
''', 1)
backdrop = backdrop.replace(
    '    def Scope "_materials"\n    {',
    '''    def Scope "_materials"
    {
        def Material "Backdrop"
        {
            token outputs:surface.connect = </World/_materials/Backdrop/S.outputs:surface>
            def Shader "S"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.02, 0.02, 0.02)
                float inputs:roughness = 1.0
                token outputs:surface
            }
        }
''', 1)
(out / 'mip_scene.usda').write_text(backdrop)
aniso = scene.replace(
    'point3f[] points = [(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)]',
    'point3f[] points = [(-32, -1, 0), (32, -1, 0), (32, 1, 0), (-32, 1, 0)]')
aniso = aniso.replace(
    '    def Scope "_materials"',
    '''    def Camera "Cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 27
        float2 clippingRange = (0.01, 1000)
        double3 xformOp:translate = (0, 0, 12)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Scope "_materials"''', 1)
(out / 'aniso_scene.usda').write_text(aniso)
shutil.copyfile(ptx, out / 'textures' / 'mountain.ptx')
PY

if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  XVFB_PREFIX="xvfb-run -a"
else
  XVFB_PREFIX=""
fi

render() {
  local backend="$1" name="$2" asset="$3" image="$OUT/$2.ppm" log="$OUT/$2.log"
  local extra=()
  [ "$backend" = "cpu" ] && extra=(--cpu-rt) || extra=(--rt)
  # shellcheck disable=SC2086
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-180s}" \
      ${XVFB_PREFIX} "$BIN" --headless --backend vk "${extra[@]}" --frames 1 \
      --size 128x128 --view-dir 0,0,-1 --screenshot "$image" "$asset" >"$log" 2>&1
  else
    ${XVFB_PREFIX} "$BIN" --headless --backend vk "${extra[@]}" --frames 1 \
      --size 128x128 --view-dir 0,0,-1 --screenshot "$image" "$asset" >"$log" 2>&1
  fi
  if grep -Eiq 'renderer init failed: no Vulkan|no Vulkan physical device|Vulkan.*unavailable|CUDA ray tracing unavailable' "$log"; then
    echo "SKIP: backend unavailable for $backend"; exit "$SKIP"
  fi
  [ -s "$image" ] || {
    echo "FAIL: no screenshot for $name"
    tail -30 "$log"
    exit 1
  }
}

render cpu ptex_cpu "$OUT/scene.usda"
render cpu flat_cpu "$OUT/flat.usda"
LIGHTUSD_CPU_PTEX_STATS=1 render cpu mip_quad "$OUT/mip_scene.usda"
# Anisotropic footprint wiring: the production path must take multiple taps at
# the minor-axis mip, and LIGHTUSD_CPU_PTEX_ISOTROPIC must restore the previous
# single-tap max-gradient behavior.
render_aniso() {
  local name="$1"; shift
  env "$@" LIGHTUSD_CPU_PTEX_STATS=1 xvfb-run -a -s "-screen 0 1280x800x24" \
    "$BIN" --headless --backend vk --cpu-rt --frames 1 --size 128x128 \
    --camera /World/Cam --screenshot "$OUT/$name.ppm" \
    "$OUT/aniso_scene.usda" >"$OUT/$name.log" 2>&1
}
if ! command -v xvfb-run >/dev/null 2>&1; then
  printf '%s\n' "SKIP: xvfb-run unavailable for the anisotropic render"
  exit "$SKIP"
fi
render_aniso aniso_cpu X=1
render_aniso aniso_cpu_iso LIGHTUSD_CPU_PTEX_ISOTROPIC=1
aniso_line=$(grep "cpu-ptex" "$OUT/aniso_cpu.log" | head -1)
iso_line=$(grep "cpu-ptex" "$OUT/aniso_cpu_iso.log" | head -1)
[ -n "$aniso_line" ] && [ -n "$iso_line" ] || {
  echo "FAIL: missing PTex sampler stats for the anisotropic render"
  tail -20 "$OUT/aniso_cpu.log"
  exit 1
}
case "$aniso_line" in
  *"aniso=0 "*) echo "FAIL: anisotropic footprint did not take extra taps"; exit 1;;
esac
case "$iso_line" in
  *"aniso=0 "*) ;;
  *) echo "FAIL: isotropic switch still used the anisotropic footprint"; exit 1;;
esac
echo "aniso wiring: $(echo "$aniso_line" | grep -o 'aniso=[0-9]* taps=[0-9]*') (forced iso: $(echo "$iso_line" | grep -o 'aniso=[0-9]* taps=[0-9]*'))"
render vk ptex_vk "$OUT/scene.usda"

python3 - "$OUT" "$FACE0_MEAN" <<'PY'
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
mean = [float(v) for v in sys.argv[2].split(',')]


def center(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=4)
    if parts[0] != b'P6' or int(parts[3]) != 255:
        raise SystemExit(f'FAIL: {path} is not a binary PPM')
    w, h = int(parts[1]), int(parts[2])
    body = parts[4]
    i = ((h // 2) * w + w // 2) * 3
    return [float(body[i]), float(body[i + 1]), float(body[i + 2])]


cpu = center(out / 'ptex_cpu.ppm')
flat = center(out / 'flat_cpu.ppm')
vk = center(out / 'ptex_vk.ppm')
print(f'decoded face mean {mean}')
print(f'cpu centre {cpu}  flat control {flat}  vk centre {vk}')


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


if not close(cpu, mean, 35.0):
    raise SystemExit('FAIL: the CPU preview does not show the decoded PTex face')
if not close(vk, mean, 45.0):
    raise SystemExit('FAIL: the Vulkan RT reference does not show the decoded PTex face')
if max(abs(x - y) for x, y in zip(cpu, flat)) < 50.0:
    raise SystemExit('FAIL: the flat control is too close; the texture may be ignored')
if max(abs(x - y) for x, y in zip(cpu, vk)) > 60.0:
    raise SystemExit('FAIL: CPU and Vulkan disagree on the visible Ptex material')

# A minified quad must stream a non-base face mip.
log = (out / 'mip_quad.log').read_text()
stats = [line for line in log.splitlines() if line.startswith('[cpu-ptex]')]
if not stats:
    raise SystemExit('FAIL: the CPU renderer did not report PTex page stats')
line = stats[0]
fields = dict(part.split('=', 1) for part in line.split() if '=' in part)
hist = [int(v) for v in fields.get('mips', '').split(',') if v.isdigit()]
if len(hist) < 2 or sum(hist[1:]) == 0:
    raise SystemExit('FAIL: the minified quad did not sample a non-base PTex mip: '
                     + line)
print('mip stats:', line)
print('PASS: visible PTex material routing matches the decoded face on CPU and VK')
PY
