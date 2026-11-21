#!/usr/bin/env bash
#
# lusdrender RT-preview camera regression. Two authored cameras look at two
# different colored quads; selecting each camera must change the rendered center
# pixel predictably.
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
ASSET="$TMP/cameras.usda"

cat > "$ASSET" <<'USD'
#usda 1.0
def Xform "Root" {
  def Camera "RedCam" {
    matrix4d xformOp:transform = ((1,0,0,0), (0,1,0,0), (0,0,1,0), (-2,0,3,1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    float focalLength = 80
    float horizontalAperture = 20
  }
  def Camera "BlueCam" {
    matrix4d xformOp:transform = ((1,0,0,0), (0,1,0,0), (0,0,1,0), (2,0,3,1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    float focalLength = 80
    float horizontalAperture = 20
  }
  def Mesh "RedQuad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(-2.8,-0.8,0),(-1.2,-0.8,0),(-1.2,0.8,0),(-2.8,0.8,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </Root/Materials/Red>
  }
  def Mesh "BlueQuad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(1.2,-0.8,0),(2.8,-0.8,0),(2.8,0.8,0),(1.2,0.8,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </Root/Materials/Blue>
  }
  def Scope "Materials" {
    def Material "Red" {
      token outputs:surface.connect = </Root/Materials/Red/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 0, 0)
        token outputs:surface
      }
    }
    def Material "Blue" {
      token outputs:surface.connect = </Root/Materials/Blue/PreviewSurface.outputs:surface>
      def Shader "PreviewSurface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0, 0, 1)
        token outputs:surface
      }
    }
  }
}
USD

render_camera() {
  local cam="$1"
  local out="$TMP/${cam}.png"
  local log="$TMP/${cam}.log"
  "$LUSDRENDER" "$ASSET" "$out" -rtPreview -camera "$cam" \
    -w 64 -height 64 -samples 1 -ambient 1 -noShadows -stats \
    >"$log" 2>&1
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: lusdrender $cam exited with $rc"
    cat "$log"
    exit 1
  fi
  if [ ! -s "$out" ]; then
    echo "FAIL: lusdrender $cam produced no image"
    cat "$log"
    exit 1
  fi
  if ! grep -Eq '^triangles: 4$' "$log"; then
    echo "FAIL: expected full triangle stats for $cam"
    cat "$log"
    exit 1
  fi
  center_rgb "$out"
}

center_rgb() {
  python3 - "$1" <<'PY'
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    raise SystemExit("not a PNG")
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
    raise SystemExit(f"expected 8-bit RGBA PNG, got depth={bit_depth} color={color_type}")
raw = zlib.decompress(bytes(idat))
stride = width * 4
rows = []
prev = bytearray(stride)
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
            row[i] = (row[i] + left) & 0xFF
        elif filt == 2:
            row[i] = (row[i] + up) & 0xFF
        elif filt == 3:
            row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
        elif filt == 4:
            pa = abs(up - up_left)
            pb = abs(left - up_left)
            pc = abs(left + up - 2 * up_left)
            pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
            row[i] = (row[i] + pred) & 0xFF
        elif filt != 0:
            raise SystemExit(f"unsupported PNG filter {filt}")
    rows.append(bytes(row))
    prev = row
rgba = b"".join(rows)
o = ((height // 2) * width + (width // 2)) * 4
print(rgba[o], rgba[o + 1], rgba[o + 2])
PY
}

read -r RR RG RB < <(render_camera RedCam | tr -d '\015') || exit 1
read -r BR BG BB < <(render_camera BlueCam | tr -d '\015') || exit 1

echo "RedCam center RGB = $RR $RG $RB"
echo "BlueCam center RGB = $BR $BG $BB"

if [ "$RR" -gt 100 ] && [ "$RR" -gt $((RB * 3)) ] &&
   [ "$BB" -gt 100 ] && [ "$BB" -gt $((BR * 3)) ]; then
  echo "PASS: lusdrender authored camera selection changes the view"
  exit 0
fi

echo "FAIL: expected RedCam to see red and BlueCam to see blue"
exit 1
