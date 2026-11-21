#!/usr/bin/env bash
#
# lusdrender MaterialX UDIM resolver regression: a MaterialX ND_image file
# reference with <UDIM> should resolve the same way from a filesystem USDA and
# from the same tiles embedded in a USDZ package.
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
if [ -n "${LUSDR_KEEP_TMP:-}" ]; then
  echo "KEEP_TMP=$TMP"
else
  trap 'rm -rf "$TMP"' EXIT
fi
SCENE="$TMP/scene.usda"
TEX1001="$TMP/tile_1001.png"
TEX1002="$TMP/tile_1002.png"
USDZ="$TMP/scene.usdz"
EXT_OUT="$TMP/external.png"
PKG_OUT="$TMP/embedded.png"
EXT_LOG="$TMP/external.log"
PKG_LOG="$TMP/embedded.log"

python3 - "$TEX1001" "$TEX1002" <<'PY' || exit 1
import struct
import sys
import zlib

def write_png(path, rgba):
    w = h = 4
    raw = b"".join(bytes([0]) + bytes(rgba) * w for _ in range(h))

    def chunk(typ, data):
        body = typ + data
        return (
            struct.pack(">I", len(data))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
    png.extend(chunk(b"IDAT", zlib.compress(raw)))
    png.extend(chunk(b"IEND", b""))
    open(path, "wb").write(png)

write_png(sys.argv[1], (220, 24, 28, 255))
write_png(sys.argv[2], (28, 210, 40, 255))
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
    texCoord2f[] primvars:st = [(0,0),(2,0),(2,1),(0,1)] (
      interpolation = "vertex"
    )
    rel material:binding = </World/Mats/Mat>
  }
  def Scope "Mats" {
    def Material "Mat" {
      token outputs:surface.connect = </World/Mats/Mat/Preview.outputs:surface>
      def Shader "Preview" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </World/Mats/Mat/NG.outputs:diff_out>
        token outputs:surface
      }
      def NodeGraph "NG" {
        color3f outputs:diff_out.connect = </World/Mats/Mat/NG/Image.outputs:out>
        def Shader "Image" {
          uniform token info:id = "ND_image_color3"
          asset inputs:file = @tile_<UDIM>.png@
          color3f outputs:out
        }
      }
    }
  }
}
USDA

python3 - "$SCENE" "$TEX1001" "$TEX1002" "$USDZ" <<'PY' || exit 1
import sys
import zipfile

scene, tex1001, tex1002, usdz = sys.argv[1:]
with zipfile.ZipFile(usdz, "w", compression=zipfile.ZIP_STORED) as z:
    z.write(scene, "scene.usda")
    z.write(tex1001, "tile_1001.png")
    z.write(tex1002, "tile_1002.png")
PY

render() {
  local in="$1" out="$2" log="$3"
  "$LUSDRENDER" "$in" "$out" -rtPreview -w 64 -height 64 -autoframe \
    -viewDir 0,0,-1 -ambient 1 -noShadows -samples 1 \
    --materialResolver tydra -udim sparse -stats >"$log" 2>&1
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
  if grep -Eq 'load summary:.*missing_textures=[1-9]' "$log"; then
    echo "FAIL: MaterialX UDIM texture was reported missing for $in"
    cat "$log"
    exit 1
  fi
}

render "$SCENE" "$EXT_OUT" "$EXT_LOG"
render "$USDZ" "$PKG_OUT" "$PKG_LOG"

python3 - "$EXT_OUT" "$PKG_OUT" <<'PY' || exit 1
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
                raise SystemExit(f"{path}: unsupported PNG filter {filt}")
        rows.append(bytes(row))
        prev = row
    return width, height, b"".join(rows)

def rgba_at(decoded, x, y):
    width, _, pixels = decoded
    i = (y * width + x) * 4
    return pixels[i:i + 4]

def assert_tile_colors(label, decoded):
    width, height, _ = decoded
    left = rgba_at(decoded, width // 4, height // 2)
    right = rgba_at(decoded, (width * 3) // 4, height // 2)
    if not (left[0] > 170 and left[1] < 90 and left[2] < 90):
        raise SystemExit(f"{label}: left UDIM tile is not red-dominant: {tuple(left)}")
    if not (right[1] > 160 and right[0] < 100 and right[2] < 100):
        raise SystemExit(f"{label}: right UDIM tile is not green-dominant: {tuple(right)}")

a = decode_png(sys.argv[1])
b = decode_png(sys.argv[2])
assert_tile_colors(sys.argv[1], a)
assert_tile_colors(sys.argv[2], b)
if a != b:
    if a[:2] != b[:2]:
        raise SystemExit(f"dimension mismatch: {a[:2]} vs {b[:2]}")
    diff = sum(1 for x, y in zip(a[2], b[2]) if x != y)
    raise SystemExit(f"pixel mismatch: {diff} byte(s) differ")
PY

echo "PASS: filesystem and USDZ-embedded MaterialX UDIM textures render identically"
