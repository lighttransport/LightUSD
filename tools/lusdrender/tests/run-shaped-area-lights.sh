#!/usr/bin/env bash
#
# lusdrender: RectLight / DiskLight / CylinderLight must be AREA lights.
#
# Only SphereLight was ever area-sampled. The other three shapes collapsed to
# their center, so a 4x4 rect cast exactly the same razor-sharp shadow a bare
# point light does, no matter how large it was. They are now sampled over their
# surface with the same next-event estimation + power-heuristic MIS.
#
# Worse, they lit nothing at all when pointed AT a surface: PreviewLight::normal
# was stored as the NEGATION of the emission direction for UsdLux lights, while
# eval_light's emission-cone test (and the mesh lights) read it as the outward
# normal of the emitting face. A rect light aimed at a wall therefore emitted
# away from it. That is why the punctual baseline below is asserted to be lit at
# all -- it used to be black.
#
# For each shape this asserts:
#
#   1. a penumbra: markedly more mid-gradient pixels than the punctual fallback
#      (-noLightNee), and fewer hard steps; and
#   2. ENERGY: a SMALL, DISTANT light -- exactly where the punctual approximation
#      is valid -- must render to within a few percent of the punctual result.
#      Without this an estimator can produce a beautiful penumbra and still be off
#      by a constant factor. (Cylinder is excluded from the energy check: the
#      punctual path scales it by its full lateral area, but only the half facing
#      the surface can emit toward it, so the two do not agree even at infinity --
#      the area estimator is the correct one.)
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

# Wall + small blocker + a LARGE light in front of it, so the penumbra is several
# pixels wide. The camera sits on +z (-viewDir points target -> eye), so the light
# must be on +z as well.
scene() {  # $1 = light block, $2 = out path
  cat > "$2" <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Wall" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-3,-3,0), (3,-3,0), (3,3,0), (-3,3,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(0.9, 0.9, 0.9)]
    uniform token subdivisionScheme = "none"
  }
  def Mesh "Blocker" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-0.5,-0.5,1), (0.5,-0.5,1), (0.5,0.5,1), (-0.5,0.5,1)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(0.1, 0.1, 0.1)]
    uniform token subdivisionScheme = "none"
  }
$1
}
USDA
}

# Small + far: the punctual approximation is valid, so the area estimator must agree.
far_scene() {  # $1 = light block, $2 = out path
  cat > "$2" <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(0.8, 0.8, 0.8)]
    uniform token subdivisionScheme = "none"
  }
$1
}
USDA
}

scene '  def RectLight "Key" {
    float inputs:width = 4
    float inputs:height = 4
    float inputs:intensity = 8
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/rect.usda"
scene '  def DiskLight "Key" {
    float inputs:radius = 2
    float inputs:intensity = 8
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/disk.usda"
scene '  def CylinderLight "Key" {
    float inputs:radius = 1
    float inputs:length = 4
    float inputs:intensity = 8
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/cylinder.usda"

far_scene '  def RectLight "Key" {
    float inputs:width = 1
    float inputs:height = 1
    float inputs:intensity = 400
    double3 xformOp:translate = (0, 0, 20)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/far_rect.usda"
far_scene '  def DiskLight "Key" {
    float inputs:radius = 0.564
    float inputs:intensity = 400
    double3 xformOp:translate = (0, 0, 20)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/far_disk.usda"

render() {  # $1 = scene, $2 = out, $3 = NEE enabled, $4.. = extra flags
  local scene="$1" out="$2" nee="$3"
  shift 3
  if [ "$nee" = 0 ]; then set -- -noLightNee "$@"; fi
  "$LUSDRENDER" "$scene" "$out" -rtPreview \
    -viewDir 0,0,1 -bg 0,0,0 -ambient 0 \
    -materialShading lightrt-bsdf -materialResolver tydra-next "$@" \
    > "$out.log" 2>&1
  if [ ! -s "$out" ]; then
    echo "FAIL: lusdrender failed for $out"
    cat "$out.log"
    return 1
  fi
  return 0
}

for shape in rect disk cylinder; do
  render "$TMP/$shape.usda" "$TMP/${shape}_soft.png" 1 -w 128 -height 128 -samples 96 || exit 1
  render "$TMP/$shape.usda" "$TMP/${shape}_hard.png" 0 -w 128 -height 128 -samples 96 || exit 1
done
for shape in rect disk; do
  render "$TMP/far_$shape.usda" "$TMP/far_${shape}_area.png" 1 -w 64 -height 64 -samples 64 -noShadows || exit 1
  render "$TMP/far_$shape.usda" "$TMP/far_${shape}_pt.png"   0 -w 64 -height 64 -samples 64 -noShadows || exit 1
done

python3 - "$TMP" <<'PY'
import struct, sys, zlib

def read_png_gray(path):
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
    out, prev, p = [], bytearray(stride), 0
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
        for x in range(w):
            px = line[x * nch:x * nch + 3]
            out.append((px[0] + px[1] + px[2]) / 3.0)
        prev = line
    return w, h, out

def edges(path):
    w, h, lum = read_png_gray(path)
    sharp = soft = 0
    for y in range(h):
        for x in range(w - 1):
            d = abs(lum[y * w + x] - lum[y * w + x + 1])
            if d > 40: sharp += 1
            elif d > 6: soft += 1
    return sharp, soft

def mean_lit(path):
    _, _, lum = read_png_gray(path)
    lit = [v for v in lum if v > 6]
    return sum(lit) / max(1, len(lit))

tmp = sys.argv[1]
for shape in ("rect", "disk", "cylinder"):
    s_sharp, s_soft = edges(f"{tmp}/{shape}_soft.png")
    h_sharp, h_soft = edges(f"{tmp}/{shape}_hard.png")
    lit = mean_lit(f"{tmp}/{shape}_hard.png")
    if lit < 10.0:
        print(f"FAIL: {shape}: the punctual baseline is black (mean {lit:.1f}). The "
              f"light is not illuminating the wall it points at -- PreviewLight::"
              f"normal is inverted again (it must be the OUTWARD normal of the "
              f"emitting face, i.e. the emission direction).")
        sys.exit(1)
    if s_soft < 1.5 * h_soft:
        print(f"FAIL: {shape}: still casting a hard shadow -- {s_soft} mid-gradient "
              f"pixels vs {h_soft} punctual (need >= 1.5x). An area light of this "
              f"size must produce a penumbra; a point light cannot.")
        sys.exit(1)
    if s_sharp >= h_sharp:
        print(f"FAIL: {shape}: area sampling did not reduce the hard shadow steps "
              f"({s_sharp} vs {h_sharp} punctual).")
        sys.exit(1)

# Energy, where the punctual model is right to begin with.
for shape in ("rect", "disk"):
    area = mean_lit(f"{tmp}/far_{shape}_area.png")
    pt = mean_lit(f"{tmp}/far_{shape}_pt.png")
    if pt < 10.0:
        print(f"FAIL: {shape}: the far-field control is black (mean {pt:.1f}); the "
              f"energy check would be vacuous.")
        sys.exit(1)
    rel = abs(area - pt) / pt
    if rel > 0.05:
        print(f"FAIL: {shape}: area sampling changed the far-field brightness by "
              f"{rel * 100:.1f}% (area {area:.1f} vs punctual {pt:.1f}). A small, "
              f"distant light is exactly where the punctual approximation is "
              f"correct, so the area estimator must reproduce it -- this is an "
              f"energy bug in the radiance or the solid-angle pdf "
              f"(pdf_A * d^2 / cos_emit).")
        sys.exit(1)

print("PASS: rect/disk/cylinder lights are area-sampled (penumbra), and the "
      "far-field energy of rect/disk is preserved")
PY
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
exit 0
