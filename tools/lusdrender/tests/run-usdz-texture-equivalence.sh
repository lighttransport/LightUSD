#!/usr/bin/env bash
#
# lusdrender texture resolver regression: a texture referenced from the
# filesystem and the same texture embedded in a USDZ package must render to the
# same pixels through the next-loader path.
set -euo pipefail

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
SCENE="$TMP/scene.usda"
TEX="$TMP/tex.png"
USDZ="$TMP/scene.usdz"
EXT_OUT="$TMP/external.png"
PKG_OUT="$TMP/embedded.png"
EXT_LOG="$TMP/external.log"
PKG_LOG="$TMP/embedded.log"

python3 - "$TEX" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
w = h = 4
pixels = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        row.extend((x * 70 + 20, y * 70 + 30, (x + y) * 35 + 40, 255))
    pixels.append(bytes(row))
raw = b"".join(pixels)

def chunk(typ, data):
    body = typ + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

png = bytearray(b"\x89PNG\r\n\x1a\n")
png.extend(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
png.extend(chunk(b"IDAT", zlib.compress(raw)))
png.extend(chunk(b"IEND", b""))
open(path, "wb").write(png)
PY

cat > "$SCENE" <<'USDA'
#usda 1.0
( upAxis = "Y" )
def Xform "World" {
  def Mesh "M" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (
      interpolation = "vertex"
    )
    rel material:binding = </World/Mats/Mat>
  }
  def Scope "Mats" {
    def Material "Mat" {
      token outputs:surface.connect = </World/Mats/Mat/Preview.outputs:surface>
      def Shader "Preview" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </World/Mats/Mat/Tex.outputs:rgb>
        token outputs:surface
      }
      def Shader "Tex" {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @tex.png@
        token inputs:sourceColorSpace = "sRGB"
        color3f outputs:rgb
      }
    }
  }
}
USDA

python3 - "$SCENE" "$TEX" "$USDZ" <<'PY'
import sys
import zipfile

scene, tex, usdz = sys.argv[1:]
with zipfile.ZipFile(usdz, "w", compression=zipfile.ZIP_STORED) as z:
    z.write(scene, "scene.usda")
    z.write(tex, "tex.png")
from pathlib import Path
with zipfile.ZipFile(Path(usdz).with_name('nested.usdz'), "w", compression=zipfile.ZIP_STORED) as z:
    z.writestr('nested/root.usda', '#usda 1.0\n(subLayers = [@model.usda@])\n')
    z.writestr('nested/model.usda', Path(scene).read_text().replace('@tex.png@','@../images/tex.png@'))
    z.write(tex, 'images/tex.png')
PY

render() {
  local in="$1" out="$2" log="$3"
  "$LUSDRENDER" "$in" "$out" -rtPreview -w 64 -height 64 -autoframe \
    -viewDir 0,0,-1 -ambient 1 -noShadows -samples 1 -stats >"$log" 2>&1
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: lusdrender failed for $in with $rc"
    cat "$log"
    exit 1
  fi
  if [ ! -s "$out" ]; then
    echo "FAIL: no output for $in"
    cat "$log"
    exit 1
  fi
  if ! grep -Eq '^triangles: 2$' "$log"; then
    echo "FAIL: expected two rendered triangles for $in"
    cat "$log"
    exit 1
  fi
}

render "$SCENE" "$EXT_OUT" "$EXT_LOG"
# No unpacked neighbor may rescue an incorrect package anchor.
mv "$TEX" "$TMP/hidden-texture.png"
render "$USDZ" "$PKG_OUT" "$PKG_LOG"
printf '#usda 1.0\n(subLayers = [@scene.usdz@])\n' > "$TMP/overlay.usda"
render "$TMP/overlay.usda" "$TMP/overlay.png" "$TMP/overlay.log"
printf '#usda 1.0\n(subLayers = [@nested.usdz@])\n' > "$TMP/nested-overlay.usda"
render "$TMP/nested-overlay.usda" "$TMP/nested-overlay.png" "$TMP/nested-overlay.log"

python3 - "$EXT_OUT" "$PKG_OUT" "$TMP/overlay.png" "$TMP/nested-overlay.png" <<'PY'
import struct
import sys
import zlib

def decode_png(path):
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
        rows.append(bytes(row))
        prev = row
    return width, height, b"".join(rows)

a = decode_png(sys.argv[1])
for path in sys.argv[2:]:
    b = decode_png(path)
    if a != b:
        if a[:2] != b[:2]:
            raise SystemExit(f"dimension mismatch: {a[:2]} vs {b[:2]}")
        diff = sum(1 for x, y in zip(a[2], b[2]) if x != y)
        raise SystemExit(f"pixel mismatch: {diff} byte(s) differ for {path}")
PY

echo "PASS: filesystem, USDZ, composed and nested-package textures render identically"
