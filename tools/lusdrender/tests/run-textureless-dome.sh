#!/usr/bin/env bash
#
# Regression test: a DomeLight with no texture:file must still illuminate on the
# tydra (-rtPreview) path.
#
# BuildCoreIbl returned false whenever the dome had no texture, so a textureless
# DomeLight was dropped entirely: no IBL, no background, no fill light. A
# constant-color environment -- one of the most common lighting setups -- added
# nothing. The fix synthesizes a constant environment of color*intensity.
#
# The scene is a gray plane; the test renders it with a textureless DomeLight and
# with the light removed, and asserts the dome makes the frame markedly brighter
# (fill + background). Equal brightness means the dome was dropped.
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

cat > "$TMP/scene.usda" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "Plane"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        color3f[] primvars:displayColor = [(0.8, 0.8, 0.8)] (interpolation = "constant")
    }
    def DomeLight "Dome"
    {
        float inputs:intensity = 1.0
        color3f inputs:color = (1, 1, 1)
    }
}
USDA
# Control: same scene, dome removed.
sed '/def DomeLight/,/^    }/d' "$TMP/scene.usda" > "$TMP/nolight.usda"

R() {
  "$LUSDRENDER" "$1" "$2" -rtPreview -autoframe -w 64 -height 64 -samples 8 \
      > "$TMP/log" 2>&1 || fail "render failed for $1: $(cat "$TMP/log")"
}
R "$TMP/scene.usda"   "$TMP/dome.png"
R "$TMP/nolight.usda" "$TMP/nolight.png"

python3 - "$TMP/dome.png" "$TMP/nolight.png" <<'PY' || exit $?
import sys, struct, zlib
def mean_sum(path):
    d=open(path,"rb").read(); pos=8; w=h=None; idat=b""; color=None
    while pos+8<=len(d):
        ln=struct.unpack(">I",d[pos:pos+4])[0]; t=d[pos+4:pos+8]; b=d[pos+8:pos+8+ln]
        if t==b"IHDR": w,h,_,color=struct.unpack(">IIBB",b[:10])
        elif t==b"IDAT": idat+=b
        elif t==b"IEND": break
        pos+=12+ln
    nch=3 if color==2 else 4; raw=zlib.decompress(idat); stride=w*nch; prev=bytearray(stride); p=0; s=0; n=0
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
                line[i]=(line[i]+(a if (pa<=pb and pa<=pc) else (bb if pb<=pc else c)))&0xff
        prev=line
        for i in range(0,stride,nch): s+=line[i]+line[i+1]+line[i+2]; n+=1
    return s/max(n,1)
dome=mean_sum(sys.argv[1]); nolight=mean_sum(sys.argv[2])
print(f"mean brightness: dome={dome:.1f} nolight={nolight:.1f}")
if dome < nolight + 5.0 or dome < nolight * 1.3:
    print("FAIL: the textureless DomeLight added no illumination -- it was dropped "
          "instead of synthesizing a constant environment."); sys.exit(1)
print("PASS: textureless DomeLight illuminates the scene")
PY
