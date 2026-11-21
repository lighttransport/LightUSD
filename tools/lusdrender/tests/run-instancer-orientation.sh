#!/usr/bin/env bash
#
# Regression test: a PointInstancer's `orientations` quaternion is REAL-FIRST.
#
# The core stage stores quats as (w, x, y, z) -- crate is imaginary-first on disk
# and the reader swizzles on load (crate-reader-unpack.cc). InstanceTRS used to read
# the four floats as (x, y, z, w), which changes an authored Z rotation into a
# different transform: instances came out rotated wrongly, and often upside
# down. In usd-wg/assets' OpenChessSet that flipped every PointInstancer'd PAWN
# through the board, where it rendered hidden underneath.
#
# Asserted as an EQUIVALENCE, not a threshold or a golden image: the same mesh,
# placed once by a PointInstancer with a quaternion, and once by an xformOp:rotateZ
# that expresses the SAME rotation, is the same world geometry -- so through a fixed
# camera the two renders must be pixel-identical. Read the quat wrong and the
# instanced copy lands somewhere the xform copy does not.
#
# The prototype is deliberately asymmetric on every axis (an L of three quads at
# different heights); a symmetric prototype would survive a wrong rotation and make
# the check vacuous.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A half turn about +Z. Real-first: (w=0, x=0, y=0, z=1). Unlike a 90-degree
# quaternion this is exactly representable as quath, so the byte-identical
# comparison remains valid now that USDA quath authoring is correctly rounded
# to binary16. Read imaginary-first and it instead becomes the identity.
cat > "$TMP/instanced.usda" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Camera "Cam"
    {
        float2 clippingRange = (0.1, 1000)
        float focalLength = 24
        float horizontalAperture = 20.955
        double3 xformOp:rotateXYZ = (-30, 25, 0)
        double3 xformOp:translate = (3, 4, 8)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }

    def PointInstancer "PI"
    {
        quath[] orientations = [(0, 0, 0, 1)]
        point3f[] positions = [(0, 0, 0)]
        int[] protoIndices = [0]
        rel prototypes = </World/PI/Proto>

        def Mesh "Proto"
        {
            uniform bool doubleSided = 1
            int[] faceVertexCounts = [4, 4, 4]
            int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
            point3f[] points = [
                (0, 0, 0), (2, 0, 0), (2, 0.2, 0), (0, 0.2, 0),
                (0, 0, 0), (0, 0, 1), (0, 1.5, 1), (0, 1.5, 0),
                (1.6, 0, 0), (2, 0, 0), (2, 0.8, 0), (1.6, 0.8, 0)
            ]
            color3f[] primvars:displayColor = [(0.8, 0.8, 0.8)]
            uniform token subdivisionScheme = "none"
        }
    }
}
USDA

# The same mesh, the same half turn about +Z, expressed as an xformOp. No
# instancing involved, so this render is the reference the instanced one must match.
cat > "$TMP/xformed.usda" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Camera "Cam"
    {
        float2 clippingRange = (0.1, 1000)
        float focalLength = 24
        float horizontalAperture = 20.955
        double3 xformOp:rotateXYZ = (-30, 25, 0)
        double3 xformOp:translate = (3, 4, 8)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }

    def Xform "Placed"
    {
        double xformOp:rotateZ = 180
        uniform token[] xformOpOrder = ["xformOp:rotateZ"]

        def Mesh "Proto"
        {
            uniform bool doubleSided = 1
            int[] faceVertexCounts = [4, 4, 4]
            int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
            point3f[] points = [
                (0, 0, 0), (2, 0, 0), (2, 0.2, 0), (0, 0.2, 0),
                (0, 0, 0), (0, 0, 1), (0, 1.5, 1), (0, 1.5, 0),
                (1.6, 0, 0), (2, 0, 0), (2, 0.8, 0), (1.6, 0.8, 0)
            ]
            color3f[] primvars:displayColor = [(0.8, 0.8, 0.8)]
            uniform token subdivisionScheme = "none"
        }
    }
}
USDA

render() {  # render <asset> <out>
  "$LUSDRENDER" "$1" "$2" -rtPreview -w 160 -height 120 -samples 1 \
      -camera Cam >"$TMP/$(basename "$2").log" 2>&1
  [ -s "$2" ]
}

if ! render "$TMP/instanced.usda" "$TMP/instanced.png"; then
  echo "SKIP: lusdrender wrote no image for the instanced scene"
  cat "$TMP/instanced.png.log" 2>/dev/null
  exit $SKIP
fi
if ! render "$TMP/xformed.usda" "$TMP/xformed.png"; then
  echo "SKIP: lusdrender wrote no image for the xform scene"
  exit $SKIP
fi

# The prototype must actually be on screen -- an empty frame would make the
# comparison below trivially true.
COVER=$(python3 - "$TMP/xformed.png" <<'PY'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
pos, idat, w, h, color = 8, b'', 0, 0, 6
while pos + 8 <= len(d):
    ln = struct.unpack(">I", d[pos:pos+4])[0]
    typ = d[pos+4:pos+8]
    if typ == b'IHDR':
        w, h, _bd, color = struct.unpack(">IIBB", d[pos+8:pos+18])
    elif typ == b'IDAT':
        idat += d[pos+8:pos+8+ln]
    elif typ == b'IEND':
        break
    pos += 12 + ln
nch = 3 if color == 2 else 4
raw = zlib.decompress(idat)
stride = w * nch
prev, p, lit = bytearray(stride), 0, 0
for _y in range(h):
    f = raw[p]; p += 1
    line = bytearray(raw[p:p+stride]); p += stride
    for i in range(stride):
        a = line[i-nch] if i >= nch else 0
        b = prev[i]
        c = prev[i-nch] if i >= nch else 0
        if f == 1: line[i] = (line[i] + a) & 0xFF
        elif f == 2: line[i] = (line[i] + b) & 0xFF
        elif f == 3: line[i] = (line[i] + (a+b)//2) & 0xFF
        elif f == 4:
            pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = (line[i] + pr) & 0xFF
    for x in range(w):
        px = line[x*nch:x*nch+3]
        if px[0] + px[1] + px[2] > 30:
            lit += 1
    prev = line
print(lit)
PY
)
if [ "${COVER:-0}" -lt 200 ]; then
  echo "FAIL: the reference render is (nearly) empty -- $COVER lit pixels. The"
  echo "      comparison below would pass no matter how the quaternion is read."
  exit 1
fi

if ! cmp -s "$TMP/instanced.png" "$TMP/xformed.png"; then
  echo "FAIL: a PointInstancer instance rotated by quath (0, 0, 0, 1) does"
  echo "      NOT land where the same mesh under xformOp:rotateZ=180 lands. Those are"
  echo "      the same rotation, so they are the same world geometry and must render"
  echo "      identically. The core stage stores quats REAL-FIRST (w, x, y, z) -- the"
  echo "      crate reader swizzles them on load -- so InstanceTRS must read"
  echo "      (w, x, y, z), not (x, y, z, w). Reading it imaginary-first turns this"
  echo "      half turn about +Z into the identity, and in real"
  echo "      assets (OpenChessSet) it buried the instanced pawns under the board."
  exit 1
fi

echo "PASS: a quaternion-oriented PointInstancer instance renders identically to the"
echo "      same rotation expressed as an xformOp ($COVER lit pixels, byte-identical)"
