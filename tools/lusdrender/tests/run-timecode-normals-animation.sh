#!/usr/bin/env bash
#
# lusdrender core-loader animation regression for time-sampled authored normals.
# With -smooth, frame 1 uses camera-facing normals and receives direct light;
# frame 2 turns normals sideways and should render dark. This also guards the
# -frames animated-geometry predicate: normals must rebuild the smooth-shading
# side buffer even when points/topology are static.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ASSET="$TMP/timecode_normals.usda"
OUT="$TMP/frame.####.png"
LOG="$TMP/timecode_normals.log"

cat > "$ASSET" <<'USDA'
#usda 1.0
(
  startTimeCode = 1
  endTimeCode = 2
  upAxis = "Y"
)
def Xform "World" {
  def Camera "Cam" {
    matrix4d xformOp:transform = ((1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,4,1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    token projection = "orthographic"
    float horizontalAperture = 4
    float verticalAperture = 4
  }
  def DistantLight "Key" {
    float intensity = 1
    color3f color = (1,1,1)
  }
  def Mesh "M" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    uniform token subdivisionScheme = "none"
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1)]
    normal3f[] normals.timeSamples = {
      1: [(0,0,1),(0,0,1),(0,0,1),(0,0,1)],
      2: [(1,0,0),(1,0,0),(1,0,0),(1,0,0)],
    }
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/Mats/White>
  }
  def Scope "Mats" {
    def Material "White" {
      token outputs:surface.connect = </World/Mats/White/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 1, 1)
        token outputs:surface
      }
    }
  }
}
USDA

"$LUSDRENDER" "$ASSET" "$OUT" -rtPreview -camera /World/Cam -frames 1:2 \
  -w 64 -height 64 -ambient 0 -noShadows -smooth -samples 1 -stats \
  >"$LOG" 2>&1
rc=$?
cat "$LOG"
if [ "$rc" -ne 0 ]; then
  echo "FAIL: lusdrender exited with $rc"
  exit 1
fi
if ! grep -Eq 'rt frames: 2, geometry animated: 1' "$LOG"; then
  echo "FAIL: expected normals animation to force per-frame rebuild"
  exit 1
fi
if [ ! -s "$TMP/frame.0001.png" ] || [ ! -s "$TMP/frame.0002.png" ]; then
  echo "FAIL: expected two rendered animation frames"
  exit 1
fi

read -r R1 G1 B1 R2 G2 B2 < <(
  python3 - "$TMP/frame.0001.png" "$TMP/frame.0002.png" <<'PY'
import struct
import sys
import zlib

def center_rgb(path):
    data = open(path, "rb").read()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"{path}: not a PNG")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos + 8 <= len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[:10])
        elif typ == b"IDAT":
            idat.extend(payload)
        elif typ == b"IEND":
            break
    if bit_depth != 8 or color_type != 6:
        raise SystemExit(f"{path}: expected 8-bit RGBA PNG")
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    prev = bytearray(stride)
    rgba = bytearray()
    p = 0
    for _ in range(height):
        filt = raw[p]
        p += 1
        row = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            left = row[i - 4] if i >= 4 else 0
            up = prev[i]
            up_left = prev[i - 4] if i >= 4 else 0
            if filt == 1:
                row[i] = (row[i] + left) & 0xff
            elif filt == 2:
                row[i] = (row[i] + up) & 0xff
            elif filt == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xff
            elif filt == 4:
                pa = abs(up - up_left)
                pb = abs(left - up_left)
                pc = abs(left + up - 2 * up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                row[i] = (row[i] + pred) & 0xff
            elif filt != 0:
                raise SystemExit(f"{path}: unsupported PNG filter {filt}")
        rgba.extend(row)
        prev = row
    o = ((height // 2) * width + (width // 2)) * 4
    return rgba[o], rgba[o + 1], rgba[o + 2]

print(*center_rgb(sys.argv[1]), *center_rgb(sys.argv[2]))
PY
)

echo "frame1 center RGB = $R1 $G1 $B1"
echo "frame2 center RGB = $R2 $G2 $B2"
if [ "$R1" -gt 150 ] && [ "$G1" -gt 150 ] && [ "$B1" -gt 150 ] && \
   [ "$R2" -lt 30 ] && [ "$G2" -lt 30 ] && [ "$B2" -lt 30 ]; then
  echo "PASS: time-sampled normals change smooth-shaded frame color"
  exit 0
fi

echo "FAIL: expected lit frame 1 and dark frame 2"
exit 1
