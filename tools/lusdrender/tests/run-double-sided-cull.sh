#!/usr/bin/env bash
#
# Regression test: doubleSided back-face culling in the lusdrender path tracer
# (audit R12).
#
# The integrator used to faceforward the shading normal at every hit, so a
# single-sided mesh (USD doubleSided=false, the default) rendered identically
# from front and back. It now culls back faces of single-sided triangles in the
# flat-BVH walk: the primary ray sees THROUGH the back face (and the shadow ray
# passes through it), matching the raster backends' back-face culling.
#
# The probe: a red quad facing +Z, cameras in front and behind.
#   single-sided, Front -> visible (red)     single-sided, Back -> culled (no red)
#   doubleSided,  Front -> visible (red)     doubleSided,  Back -> visible (red)
# Verified on BOTH the legacy (default) and next (-rtPreview) loaders.
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
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit $SKIP; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cat > "$TMP/ss.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    color3f[] primvars:displayColor = [(1,0.15,0.15)]
    uniform token subdivisionScheme = "none"
  }
  def DomeLight "D" { float inputs:intensity = 1.0 }
  def Camera "Front" {
    double3 xformOp:translate = (0,0,6)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def Camera "Back" {
    double3 xformOp:translate = (0,0,-6)
    float xformOp:rotateY = 180
    uniform token[] xformOpOrder = ["xformOp:translate","xformOp:rotateY"]
  }
}
USDA
sed 's/def Mesh "Quad" {/def Mesh "Quad" {\n    uniform bool doubleSided = 1/' \
    "$TMP/ss.usda" > "$TMP/ds.usda"

red_count() {
python3 - "$1" <<'PY'
import sys, struct, zlib
d=open(sys.argv[1],'rb').read(); pos=8; idat=b''; w=h=0; color=2
while pos+8<=len(d):
    ln=struct.unpack(">I",d[pos:pos+4])[0]; t=d[pos+4:pos+8]; b=d[pos+8:pos+8+ln]
    if t==b'IHDR': w,h,_,color=struct.unpack(">IIBB",b[:10])
    elif t==b'IDAT': idat+=b
    elif t==b'IEND': break
    pos+=12+ln
nch=3 if color==2 else 4; raw=zlib.decompress(idat); st=w*nch
prev=bytearray(st); p=0; red=0
for _ in range(h):
    f=raw[p]; p+=1; line=bytearray(raw[p:p+st]); p+=st
    for i in range(st):
        a=line[i-nch] if i>=nch else 0; bb=prev[i]
        if f==1: line[i]=(line[i]+a)&255
        elif f==2: line[i]=(line[i]+bb)&255
        elif f==3: line[i]=(line[i]+((a+bb)>>1))&255
        elif f==4:
            c=prev[i-nch] if i>=nch else 0
            pp=a+bb-c; pa,pb,pc=abs(pp-a),abs(pp-bb),abs(pp-c)
            line[i]=(line[i]+(a if (pa<=pb and pa<=pc) else (bb if pb<=pc else c)))&255
    prev=line
    for i in range(0,st,nch):
        if line[i] > 1.4*line[i+1]+20: red+=1
print(red)
PY
}

for spec in "legacy:" "core:-rtPreview"; do
  tag="${spec%%:*}"; flag="${spec#*:}"
  declare -A r
  for v in ss ds; do
    for c in Front Back; do
      # shellcheck disable=SC2086
      "$LUSDRENDER" "$TMP/$v.usda" "$TMP/${tag}_${v}_${c}.png" -w 96 -height 96 \
          -samples 2 -camera "$c" $flag > "$TMP/log" 2>&1 \
        || fail "$tag $v/$c render failed: $(tail -1 "$TMP/log")"
      r[$v/$c]=$(red_count "$TMP/${tag}_${v}_${c}.png")
    done
  done
  echo "$tag red px: ssFront=${r[ss/Front]} ssBack=${r[ss/Back]} dsFront=${r[ds/Front]} dsBack=${r[ds/Back]}"
  [ "${r[ss/Front]}" -gt 2000 ] || fail "$tag single-sided front not visible (${r[ss/Front]}) -- fixture broken"
  [ "${r[ds/Front]}" -gt 2000 ] || fail "$tag doubleSided front not visible (${r[ds/Front]})"
  [ "${r[ds/Back]}" -gt 2000 ] \
    || fail "$tag doubleSided quad culled from behind (${r[ds/Back]}) -- doubleSided not honored"
  # The single fix under test: single-sided must be culled from behind.
  [ "${r[ss/Back]}" -lt $(( ${r[ss/Front]} / 20 )) ] \
    || fail "$tag single-sided quad still visible from behind (${r[ss/Back]}) -- back face not culled"
done

echo "PASS: single-sided back faces culled; doubleSided honored (legacy + next)"
exit 0
