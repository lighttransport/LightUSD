#!/usr/bin/env bash
#
# lusdrender: sphere lights must be AREA-sampled (next-event estimation), not
# collapsed to a point.
#
# Every finite light used to be treated as punctual -- irradiance = I / d², with
# an ad-hoc max(1, area) fudge -- so an area light cast a perfectly hard shadow
# no matter how large it was. A sphere light now gets next-event estimation with
# a power-heuristic MIS weight against the BSDF sampler, which is what produces a
# penumbra.
#
# The scene is a wall, a small blocker in front of it, and a LARGE sphere light
# further in front (radius 2 at distance 4, blocker at 1) -- geometry chosen so
# the penumbra is several pixels wide. It asserts, in `-materialShading
# lightrt-bsdf`:
#
#   1. the shadow edge is SOFTER with NEE than with the punctual fallback:
#      markedly more mid-gradient pixels, and fewer hard steps; and
#   2. NEE preserves ENERGY -- a small, distant sphere (where the punctual
#      approximation is valid) renders to within a few percent of the punctual
#      result. This is the guard that matters: an estimator can produce a
#      beautiful penumbra and still be off by a constant factor, and nothing else
#      in the suite would notice.
#
# -noLightNee restores the punctual path; the test A/Bs against it.
#
# The lights author `normalize = true` with 4x the original intensity: under the
# normalize-aware convention (R10) that is radiance I/(pi r^2) -- exactly the
# implicit convention these energies were originally pinned against.
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
  echo "SKIP: python3 not found (needed to measure the shadow edge)"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The camera sits on +z (-viewDir points target -> eye), so the light and the
# blocker must be on +z too or the wall is lit from behind and renders black.
cat > "$TMP/penumbra.usda" <<'USDA'
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
  def SphereLight "Key" {
    float inputs:radius = 2.0
    float inputs:intensity = 480
    bool inputs:normalize = true
    color3f inputs:color = (1, 1, 1)
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA

# Small + far: the punctual approximation is valid here, so NEE must agree.
cat > "$TMP/farfield.usda" <<'USDA'
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
  def SphereLight "Key" {
    float inputs:radius = 0.5
    float inputs:intensity = 1600
    bool inputs:normalize = true
    color3f inputs:color = (1, 1, 1)
    double3 xformOp:translate = (0, 0, 20)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA

render() {  # $1 = scene, $2 = out, $3 = NEE enabled, $4.. = extra flags
  local scene="$1" out="$2" nee="$3"
  shift 3
  if [ "$nee" = 0 ]; then set -- -noLightNee "$@"; fi
  "$LUSDRENDER" "$scene" "$out" -rtPreview \
    -viewDir 0,0,1 -bg 0,0,0 -ambient 0 \
    -materialShading lightrt-bsdf -materialResolver tydra-next "$@" \
    > "$out.log" 2>&1
  if [ $? -ne 0 ] || [ ! -s "$out" ]; then
    echo "FAIL: lusdrender failed for $out"
    cat "$out.log"
    return 1
  fi
  return 0
}

render "$TMP/penumbra.usda" "$TMP/soft.png" 1 -w 128 -height 128 -samples 96 || exit 1
render "$TMP/penumbra.usda" "$TMP/hard.png" 0 -w 128 -height 128 -samples 96 || exit 1
render "$TMP/farfield.usda" "$TMP/far_nee.png" 1 -w 64 -height 64 -samples 64 -noShadows || exit 1
render "$TMP/farfield.usda" "$TMP/far_pt.png"  0 -w 64 -height 64 -samples 64 -noShadows || exit 1

python3 - "$TMP" <<'PY'
import struct, sys, zlib

def read_png_gray(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return None, None, None
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
    if w is None or color not in (2, 6):
        return None, None, None
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
    """(hard steps, soft gradient) across horizontally adjacent pixels."""
    w, h, lum = read_png_gray(path)
    if lum is None:
        return None
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
soft_e = edges(f"{tmp}/soft.png")
hard_e = edges(f"{tmp}/hard.png")
if soft_e is None or hard_e is None:
    print("FAIL: could not read the renders back")
    sys.exit(1)
s_sharp, s_soft = soft_e
h_sharp, h_soft = hard_e

# 1. The shadow edge must actually soften.
if s_soft < 1.5 * h_soft:
    print(f"FAIL: the sphere light is still casting a hard shadow. NEE gave "
          f"{s_soft} mid-gradient pixels vs {h_soft} for the punctual fallback "
          f"(need >= 1.5x). A large area light must produce a penumbra; a point "
          f"light cannot.")
    sys.exit(1)
if s_sharp >= h_sharp:
    print(f"FAIL: NEE did not reduce the hard shadow steps ({s_sharp} vs "
          f"{h_sharp} punctual).")
    sys.exit(1)

# 2. ... without changing the energy where the punctual model was already right.
far_nee = mean_lit(f"{tmp}/far_nee.png")
far_pt = mean_lit(f"{tmp}/far_pt.png")
rel = abs(far_nee - far_pt) / max(far_pt, 1e-6)
if far_pt < 10.0:
    print(f"FAIL: the far-field control is black (mean {far_pt:.1f}); the energy "
          f"check would be vacuous.")
    sys.exit(1)
if rel > 0.05:
    print(f"FAIL: NEE changed the far-field brightness by {rel * 100:.1f}% "
          f"(NEE {far_nee:.1f} vs punctual {far_pt:.1f}). A small, distant sphere "
          f"is exactly where the punctual approximation is correct, so the area "
          f"estimator must reproduce it: this is an energy bug (the radiance "
          f"conversion L = I / (pi r^2), or the pdf).")
    sys.exit(1)

print(f"PASS: sphere light area-sampled -- penumbra {s_soft} vs {h_soft} "
      f"mid-gradient px ({s_soft / max(h_soft, 1):.1f}x softer), far-field energy "
      f"preserved to {rel * 100:.1f}%")
PY
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
exit 0
