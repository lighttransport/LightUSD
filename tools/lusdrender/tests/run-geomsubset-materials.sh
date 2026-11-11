#!/usr/bin/env bash
#
# Regression test: per-face GeomSubset material bindings must render on the
# tydra (-rtPreview) path.
#
# The streaming path resolved exactly ONE material per mesh
# (ResolveMeshMaterialCached on the mesh prim) and stamped it on every triangle;
# no GeomSubset traversal existed, so a mesh whose materials were bound only via
# face subsets rendered entirely in the default gray. The fix
# (ExpandGeomSubsetJobsCore) splits such a mesh into one job per bound subset
# (face mask + the subset prim as binding source) plus a remainder job.
#
# The fixture is one 3-quad mesh whose faces are bound Red/Green/Blue through
# three materialBind GeomSubsets, with no mesh-level binding. All three hues
# must appear in the render.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"; exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cat > "$TMP/gsub.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
    def Mesh "M" {
        int[] faceVertexCounts = [4, 4, 4]
        int[] faceVertexIndices = [0,1,5,4, 1,2,6,5, 2,3,7,6]
        point3f[] points = [(-3,-1,0),(-1,-1,0),(1,-1,0),(3,-1,0),(-3,1,0),(-1,1,0),(1,1,0),(3,1,0)]
        def GeomSubset "red" (prepend apiSchemas = ["MaterialBindingAPI"]) {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </World/Red>
        }
        def GeomSubset "green" (prepend apiSchemas = ["MaterialBindingAPI"]) {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </World/Green>
        }
        def GeomSubset "blue" (prepend apiSchemas = ["MaterialBindingAPI"]) {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [2]
            rel material:binding = </World/Blue>
        }
    }
    def Material "Red" {
        token outputs:surface.connect = </World/Red/S.outputs:surface>
        def Shader "S" { uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1,0,0)
            float inputs:roughness = 1
            token outputs:surface }
    }
    def Material "Green" {
        token outputs:surface.connect = </World/Green/S.outputs:surface>
        def Shader "S" { uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0,1,0)
            float inputs:roughness = 1
            token outputs:surface }
    }
    def Material "Blue" {
        token outputs:surface.connect = </World/Blue/S.outputs:surface>
        def Shader "S" { uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0,0,1)
            float inputs:roughness = 1
            token outputs:surface }
    }
}
USDA

"$LUSDRENDER" "$TMP/gsub.usda" "$TMP/gsub.png" -rtPreview -autoframe \
    -w 192 -height 96 -samples 4 > "$TMP/log" 2>&1 \
    || fail "render failed: $(cat "$TMP/log")"

python3 - "$TMP/gsub.png" <<'PY' || exit $?
import sys, struct, zlib
d = open(sys.argv[1], "rb").read()
pos = 8; w = h = None; idat = b""; color = None
while pos + 8 <= len(d):
    ln = struct.unpack(">I", d[pos:pos+4])[0]; t = d[pos+4:pos+8]; b = d[pos+8:pos+8+ln]
    if t == b"IHDR": w, h, _, color = struct.unpack(">IIBB", b[:10])
    elif t == b"IDAT": idat += b
    elif t == b"IEND": break
    pos += 12 + ln
nch = 3 if color == 2 else 4
raw = zlib.decompress(idat); stride = w * nch; prev = bytearray(stride); p = 0
red = green = blue = 0
for _ in range(h):
    f = raw[p]; p += 1; line = bytearray(raw[p:p+stride]); p += stride
    for i in range(stride):
        a = line[i-nch] if i >= nch else 0; bb = prev[i]
        if f == 1: line[i] = (line[i] + a) & 0xff
        elif f == 2: line[i] = (line[i] + bb) & 0xff
        elif f == 3: line[i] = (line[i] + ((a + bb) >> 1)) & 0xff
        elif f == 4:
            c = prev[i-nch] if i >= nch else 0
            p_ = a + bb - c; pa, pb, pc = abs(p_-a), abs(p_-bb), abs(p_-c)
            pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else c)
            line[i] = (line[i] + pr) & 0xff
    prev = line
    for i in range(0, stride, nch):
        r, g, b2 = line[i], line[i+1], line[i+2]
        if r + g + b2 < 40: continue  # background
        # Relative dominance: the bound hue must clearly lead. The bug renders
        # every face the same neutral gray, which none of these match.
        if r > g + 30 and r > b2 + 30: red += 1
        elif g > r + 30 and g > b2 + 30: green += 1
        elif b2 > r + 30 and b2 > g + 30: blue += 1
print(f"pixels: red={red} green={green} blue={blue}")
if red < 20 or green < 20 or blue < 20:
    print("FAIL: not all three GeomSubset-bound materials rendered -- per-face "
          "bindings were dropped (whole mesh took one material / default gray).")
    sys.exit(1)
print("PASS: per-face GeomSubset materials render on -rtPreview")
PY
