#!/usr/bin/env bash
#
# Regression test: purpose filtering (-showGuide / -purpose), visibility =
# "invisible" pruning, and constant inputs:opacity on the CPU path.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"; exit $SKIP
fi
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit $SKIP; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cat > "$TMP/scene.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
    def Mesh "Visible" {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0,1,2]
        point3f[] points = [(-1,-1,0),(1,-1,0),(0,1,0)]
    }
    def Mesh "GuideM" {
        uniform token purpose = "guide"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0,1,2]
        point3f[] points = [(2,-1,0),(4,-1,0),(3,1,0)]
    }
    def Mesh "InvisM" {
        token visibility = "invisible"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0,1,2]
        point3f[] points = [(-4,-1,0),(-2,-1,0),(-3,1,0)]
    }
    def Camera "Cam" {
        float focalLength = 15
        double3 xformOp:translate = (0, 0, 14)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
USDA

lit() {
python3 - "$1" <<'PY'
import sys, struct, zlib
d=open(sys.argv[1],"rb").read(); pos=8; w=h=None; idat=b""; color=None
while pos+8<=len(d):
    ln=struct.unpack(">I",d[pos:pos+4])[0]; t=d[pos+4:pos+8]; b=d[pos+8:pos+8+ln]
    if t==b"IHDR": w,h,_,color=struct.unpack(">IIBB",b[:10])
    elif t==b"IDAT": idat+=b
    elif t==b"IEND": break
    pos+=12+ln
nch=3 if color==2 else 4; raw=zlib.decompress(idat); stride=w*nch
prev=bytearray(stride); p=0; n=0
for _ in range(h):
    f=raw[p]; p+=1; line=bytearray(raw[p:p+stride]); p+=stride
    for i in range(stride):
        a=line[i-nch] if i>=nch else 0; bb=prev[i]
        if f==1: line[i]=(line[i]+a)&0xff
        elif f==2: line[i]=(line[i]+bb)&0xff
        elif f==3: line[i]=(line[i]+((a+bb)>>1))&0xff
        elif f==4:
            c=prev[i-nch] if i>=nch else 0
            p_=a+bb-c; pa,pb,pc=abs(p_-a),abs(p_-bb),abs(p_-c)
            pr=a if (pa<=pb and pa<=pc) else (bb if pb<=pc else c)
            line[i]=(line[i]+pr)&0xff
    prev=line
    for i in range(0,stride,nch):
        if line[i]+line[i+1]+line[i+2] > 60: n+=1
print(n)
PY
}

R="$LUSDRENDER"
# A fixed camera: -autoframe frames only the visible geometry, so the lit-pixel
# comparison below must not depend on which purposes are shown.
COMMON=(-w 192 -height 96 -samples 1 -camera /World/Cam)

# 1. Default hides guide, -showGuide adds it, invisible never shows.
"$R" "$TMP/scene.usda" "$TMP/def.png"   "${COMMON[@]}"            >/dev/null 2>&1 || fail "render failed"
"$R" "$TMP/scene.usda" "$TMP/guide.png" "${COMMON[@]}" -showGuide >/dev/null 2>&1 || fail "-showGuide failed"
d=$(lit "$TMP/def.png"); g=$(lit "$TMP/guide.png")
[ "$g" -gt $((d + d / 2)) ] \
  || fail "purpose filtering inert: default=$d lit px, -showGuide=$g (guide should add a triangle)"

# 2. -purpose guide shows ONLY the guide triangle.
"$R" "$TMP/scene.usda" "$TMP/gonly.png" "${COMMON[@]}" -purpose guide >/dev/null 2>&1 || fail "-purpose guide failed"
go=$(lit "$TMP/gonly.png")
[ "$go" -gt 0 ] && [ "$go" -lt "$g" ] \
  || fail "-purpose guide wrong: $go lit px (default=$d, showGuide=$g)"

# 3. Next path: the invisible mesh must be pruned from the stats.
tris=$("$R" "$TMP/scene.usda" "$TMP/next.png" -rtPreview -w 32 -height 32 -samples 1 -autoframe -stats 2>&1 | grep -E "^triangles:" | grep -oE "[0-9]+")
[ "$tris" = "2" ] \
  || fail "next path collected $tris triangles; expected 2 (invisible pruned, guide kept-but-tagged)"

# 4. Constant inputs:opacity renders differently from opaque.
cat > "$TMP/op.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
    def Mesh "Q" (prepend apiSchemas = ["MaterialBindingAPI"]) {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
        rel material:binding = </World/M>
    }
    def Material "M" {
        token outputs:surface.connect = </World/M/S.outputs:surface>
        def Shader "S" { uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1,1,1)
            float inputs:opacity = 0.15
            float inputs:roughness = 1
            token outputs:surface }
    }
}
USDA
sed 's/inputs:opacity = 0.15/inputs:opacity = 1.0/' "$TMP/op.usda" > "$TMP/opq.usda"
"$R" "$TMP/op.usda"  "$TMP/op.png"  -w 64 -height 64 -samples 2 -autoframe >/dev/null 2>&1 || fail "opacity render failed"
"$R" "$TMP/opq.usda" "$TMP/opq.png" -w 64 -height 64 -samples 2 -autoframe >/dev/null 2>&1 || fail "opaque render failed"
cmp -s "$TMP/op.png" "$TMP/opq.png" \
  && fail "constant inputs:opacity is dropped (0.15 renders identical to 1.0)"

echo "PASS: purpose/visibility/constant-opacity honored"
exit 0
