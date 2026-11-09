#!/usr/bin/env bash
#
# lusdrender `-materialShading lightrt-bsdf`: light must be counted ONCE.
#
# Shade() is an analytic direct-lighting evaluator with a single BSDF-sampled
# bounce layered on top. The bounce ray was gathering radiance that the shading
# point had ALREADY collected analytically, twice over:
#
#   1. the dome / environment. The surface integrates the whole lobe through the
#      split-sum IBL term, and then the bounce ray -- sampled from that same lobe
#      -- escaped to the environment and added it again.
#   2. emissive meshes registered as analytic lights (MeshLightAPI). Direct
#      lighting delivers them via LightCache::mesh, and then a bounce ray landing
#      on the emitter added its emission on top.
#
# An escaping bounce now contributes nothing, and an indirect hit on an
# area-light triangle contributes no emission (TriInfo::area_light).
#
# -lightDoubleCount restores the old behavior; the test A/Bs against it,
# and also pins the absolute physics where it can:
#
#   - FURNACE: a white Lambert quad under a uniform dome of radiance 1 must come
#     out at the radiance of the dome. Both shading paths are held to that: the
#     legacy one (no bounce at all) lands at 0.95x, lightrt-bsdf at 1.00x. It used
#     to be 2.00x -- half from re-gathering the dome on the bounce, half from
#     feeding the full BSDF to both split-sum IBL terms.
#   - MESH LIGHT: a floor lit by an emissive quad must get dimmer once the
#     emitter stops being counted twice.
#
# Radiance is recovered from the PNG by inverting the sRGB encode and the
# Reinhard tone map (m = L/(1+L)), so the numbers below are linear.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A uniform environment of radiance exactly 1.0 (RGBE 128,128,128,129).
python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
W, H = 64, 32
hdr = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (H, W)
open(tmp + "/uniform.hdr", "wb").write(hdr + bytes([128, 128, 128, 129]) * W * H)
PY

# White Lambert quad in the uniform dome. The zero-intensity sphere light is not
# decoration: with NO analytic lights the integrator falls back to a camera
# headlight, which would swamp the furnace measurement.
cat > "$TMP/furnace.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    uniform token subdivisionScheme = "none"
    rel material:binding = </World/White>
  }
  def Material "White" {
    token outputs:surface.connect = </World/White/S.outputs:surface>
    def Shader "S" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (1, 1, 1)
      float inputs:roughness = 1
      float inputs:metallic = 0
      token outputs:surface
    }
  }
}
def SphereLight "Off" {
  float inputs:intensity = 0
  float inputs:radius = 0.01
  double3 xformOp:translate = (0, 0, 50)
  uniform token[] xformOpOrder = ["xformOp:translate"]
}
def DomeLight "Sky" {
  float inputs:intensity = 1.0
  color3f inputs:color = (1, 1, 1)
  asset inputs:texture:file = @./uniform.hdr@
}
USDA

# A floor lit by an emissive quad carrying MeshLightAPI, i.e. an emitter that is
# ALSO an analytic light. Winding is chosen so the geometric normal (which is what
# the mesh-light emission cone uses) points AT the floor.
cat > "$TMP/meshlight.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Floor" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-4,-4,0), (4,-4,0), (4,4,0), (-4,4,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(1, 1, 1)]
    uniform token subdivisionScheme = "none"
  }
  def Mesh "Emitter" (prepend apiSchemas = ["MeshLightAPI"]) {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [3, 2, 1, 0]
    point3f[] points = [(-0.5,-0.5,6), (0.5,-0.5,6), (0.5,0.5,6), (-0.5,0.5,6)]
    float inputs:intensity = 40
    color3f inputs:color = (1, 1, 1)
    uniform token subdivisionScheme = "none"
  }
}
USDA

# The dome/IBL is only set up on the `next` rtPreview path, so the furnace runs
# there; the mesh-light half runs on BOTH loaders, since each builds
# LightCache::mesh its own way (the next loader marks TriMat::area_light during the
# flatten and collects the lights afterwards; the legacy flatten pushes them as it
# emits triangles). The next loader used to register no mesh lights at all.
render() {  # $1 = scene, $2 = out, $3 = shading, $4 = DOUBLE_COUNT, $5.. = flags
  local scene="$1" out="$2" shading="$3" dbl="$4"
  shift 4
  if [ "$dbl" = 1 ]; then set -- -lightDoubleCount "$@"; fi
  "$LUSDRENDER" "$scene" "$out" \
    -viewDir 0,0,1 -bg 0,0,0 -ambient 0 -w 96 -height 96 -samples 64 \
    -materialShading "$shading" "$@" > "$out.log" 2>&1
  if [ ! -s "$out" ]; then
    echo "FAIL: render failed for $out"
    cat "$out.log"
    return 1
  fi
  return 0
}

render "$TMP/furnace.usda"   "$TMP/furnace_bsdf.png"   lightrt-bsdf 0 -rtPreview || exit 1
render "$TMP/furnace.usda"   "$TMP/furnace_double.png" lightrt-bsdf 1 -rtPreview || exit 1
render "$TMP/furnace.usda"   "$TMP/furnace_legacy.png" legacy       0 -rtPreview || exit 1
render "$TMP/meshlight.usda" "$TMP/mesh_bsdf.png"      lightrt-bsdf 0 -legacyLoad || exit 1
render "$TMP/meshlight.usda" "$TMP/mesh_double.png"    lightrt-bsdf 1 -legacyLoad || exit 1
render "$TMP/meshlight.usda" "$TMP/mesh_next.png"      lightrt-bsdf 0 -rtPreview -stats || exit 1

# The next loader must register the emissive mesh as an analytic light too, not
# just let a bounce stumble into it.
if ! grep -q "rt mesh light triangles: 2" "$TMP/mesh_next.png.log"; then
  echo "FAIL: the next loader registered no mesh lights for a MeshLightAPI mesh."
  echo "      An emissive area-light mesh must LIGHT the scene, not merely glow."
  grep -i "mesh light" "$TMP/mesh_next.png.log" || true
  exit 1
fi

python3 - "$TMP" <<'PY'
import struct, sys, zlib

def read_png(path):
    d = open(path, "rb").read()
    w = h = color = None
    idat = b""
    pos = 8
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    rows, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append([tuple(line[x * nch:x * nch + 3]) for x in range(w)])
        prev = line
    return w, h, rows

def radiance(px):
    """PNG byte -> linear radiance: undo sRGB, then the Reinhard tone map."""
    acc = 0.0
    for v in px:
        s = v / 255.0
        m = s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4
        m = min(m, 0.999)
        acc += m / (1.0 - m)
    return acc / 3.0

def region(path, pred):
    w, h, rows = read_png(path)
    vals = [radiance(rows[y][x]) for y in range(h) for x in range(w)
            if pred(x - w / 2.0, y - h / 2.0)]
    return sum(vals) / max(1, len(vals))

tmp = sys.argv[1]
center = lambda dx, dy: abs(dx) < 6 and abs(dy) < 6          # the lit object
corner = lambda dx, dy: dx < -42 and dy < -42                # the dome itself
ring = lambda dx, dy: 9.0 <= (dx * dx + dy * dy) ** 0.5 < 16.0  # floor by the emitter

dome = region(f"{tmp}/furnace_legacy.png", corner)
if not (0.8 < dome < 1.25):
    print(f"FAIL: the dome background reads {dome:.2f}, not ~1.0. The harness "
          f"cannot measure anything (bad .hdr, or the tone-map inversion is "
          f"wrong).")
    sys.exit(1)

leg = region(f"{tmp}/furnace_legacy.png", center) / dome
bsdf = region(f"{tmp}/furnace_bsdf.png", center) / dome
dbl = region(f"{tmp}/furnace_double.png", center) / dome

# The anchor: no bounce at all, so the furnace must come out right.
if not (0.75 < leg < 1.25):
    print(f"FAIL: legacy shading puts a white Lambert surface at {leg:.2f}x the "
          f"radiance of the uniform dome lighting it. That is an IBL "
          f"normalization bug, independent of any double counting.")
    sys.exit(1)
if bsdf > 1.15:
    print(f"FAIL: lightrt-bsdf puts the furnace surface at {bsdf:.2f}x the dome "
          f"radiance (legacy: {leg:.2f}x). A white Lambert surface in a uniform "
          f"furnace must come out at the radiance of the furnace. Either the BSDF "
          f"bounce is escaping to the environment and adding it on top of the "
          f"split-sum IBL term that already integrated it (the dome counted "
          f"twice), or the two IBL terms are each being fed the FULL BSDF instead "
          f"of their own lobe (bsdf_eval_lobes), which counts both lobes against "
          f"both prefiltered environments.")
    sys.exit(1)
if dbl < bsdf * 1.3:
    print(f"FAIL: -lightDoubleCount gave {dbl:.2f}x vs {bsdf:.2f}x -- "
          f"the A/B lever is not restoring the old behavior, so this test is not "
          f"actually proving the fix.")
    sys.exit(1)

mesh = region(f"{tmp}/mesh_bsdf.png", ring)
mesh_dbl = region(f"{tmp}/mesh_double.png", ring)
if mesh <= 1e-4:
    print(f"FAIL: the floor is black ({mesh:.5f}); the mesh light is not lighting "
          f"anything, so its double count cannot be measured.")
    sys.exit(1)
if mesh > mesh_dbl * 0.9:
    print(f"FAIL: suppressing the emissive mesh on indirect hits changed the "
          f"floor by less than 10% ({mesh:.4f} vs {mesh_dbl:.4f} double-counted). "
          f"A bounce landing on an analytic mesh light is still adding its "
          f"emission on top of the direct-lighting term.")
    sys.exit(1)

print(f"PASS: light counted once -- furnace {bsdf:.2f}x dome (was {dbl:.2f}x, "
      f"legacy anchor {leg:.2f}x); mesh-light floor {mesh:.4f} "
      f"(was {mesh_dbl:.4f})")
PY
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
exit 0
