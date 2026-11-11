#!/usr/bin/env bash
# CPU/TLAS material:binding:back regression. The back material is an unsupported
# shader so this also proves the degraded PreviewSurface reaches hit shading.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${1:-${LUSDRENDER:-$REPO_ROOT/build_ninja/tools/lusdrender/lusdrender}}"
[ -x "$BIN" ] || { echo "SKIP: lusdrender not found ($BIN)"; exit "$SKIP"; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/back.usda" <<'USDA'
#usda 1.0
(upAxis = "Y")
def Xform "World" {
  def Mesh "M" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    uniform token subdivisionScheme = "none"
    rel material:binding = </World/Looks/Front>
    rel material:binding:back = </World/Looks/Back>
  }
  def Scope "Looks" {
    def Material "Front" {
      token outputs:surface.connect = </World/Looks/Front/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.9,0.02,0.01)
        token outputs:surface
      }
    }
    def Material "Back" {
      token outputs:surface.connect = </World/Looks/Back/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "UnknownBackSurface"
        color3f inputs:baseColor = (0.01,0.03,0.95)
        token outputs:surface
      }
    }
  }
}
USDA

dominant() {
python3 - "$1" "$2" <<'PY'
import struct, sys, zlib
d=open(sys.argv[1],'rb').read(); pos=8; w=h=color=None; idat=b''
while pos+8 <= len(d):
    n=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; b=d[pos+8:pos+8+n]
    if t==b'IHDR': w,h,_,color=struct.unpack('>IIBB',b[:10])
    elif t==b'IDAT': idat += b
    elif t==b'IEND': break
    pos += 12+n
if w is None or color not in (2,6): sys.exit(2)
nch=3 if color==2 else 4; raw=zlib.decompress(idat); stride=w*nch
prev=bytearray(stride); pos=0; pixels=[]
for _ in range(h):
    f=raw[pos]; pos+=1; line=bytearray(raw[pos:pos+stride]); pos+=stride
    for i in range(stride):
        a=line[i-nch] if i>=nch else 0; b=prev[i]
        if f==1: line[i]=(line[i]+a)&255
        elif f==2: line[i]=(line[i]+b)&255
        elif f==3: line[i]=(line[i]+((a+b)>>1))&255
        elif f==4:
            c=prev[i-nch] if i>=nch else 0; q=a+b-c
            pa,pb,pc=abs(q-a),abs(q-b),abs(q-c)
            line[i]=(line[i]+(a if pa<=pb and pa<=pc else b if pb<=pc else c))&255
    prev=line; pixels.extend(line)
channel={'red':0,'blue':2}[sys.argv[2]]
n=0
for i in range(0,len(pixels)-2,nch):
    rgb=pixels[i:i+3]; v=rgb[channel]; others=rgb[1:3] if channel==0 else rgb[0:2]
    n += v > 35 and all(v > 2*x for x in others)
print(n)
PY
}

for item in "front:0,0,1:red" "back:0,0,-1:blue"; do
  tag="${item%%:*}"; rest="${item#*:}"; direction="${rest%%:*}"; color="${rest##*:}"
  out="$TMP/$tag.png"; log="$TMP/$tag.log"
  "$BIN" "$TMP/back.usda" "$out" -rtPreview --materialResolver tydra \
    -w 96 -height 96 -viewDir "$direction" -ambient 1 -noShadows \
    -samples 1 -stats >"$log" 2>&1
  rc=$?; cat "$log"
  [ "$rc" -eq 0 ] && [ -s "$out" ] || { echo "FAIL: $tag render failed"; exit 1; }
  count="$(dominant "$out" "$color")" || { echo "FAIL: malformed $tag PNG"; exit 1; }
  echo "$tag $color pixels: $count"
  [ "$count" -ge 100 ] || { echo "FAIL: $tag did not use $color material"; exit 1; }
done

echo "PASS: lusdrender selects the authored degraded back-face material"
