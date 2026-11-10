#!/usr/bin/env bash
#
# Regression test: environment specular in -materialShading lightrt-bsdf must be
# bounded across roughness.
#
# EvalMaterialIblSpecular evaluated the analytic microfacet BRDF at the exact
# mirror direction and multiplied it by the already-prefiltered environment
# radiance. The prefilter has integrated the NDF, so at the mirror direction
# D = 1/(pi*alpha) blows the estimate up without bound as roughness -> 0 (a
# ~280x-too-bright highlight at roughness 0.05). It now uses the bounded
# split-sum spec_env*(F0*A+B), like the non-bsdf path.
#
# A smooth (roughness 0.05) metal surface reflecting a constant dome must not be
# dramatically brighter than a rough (roughness 0.9) one -- both reflect the same
# uniform environment.
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

scene() { # $1 = roughness -> stdout
cat <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
    def Mesh "Q" (prepend apiSchemas = ["MaterialBindingAPI"]) {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
        normal3f[] primvars:normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1)] (interpolation="vertex")
        rel material:binding = </World/M>
    }
    def Material "M" {
        token outputs:surface.connect = </World/M/S.outputs:surface>
        def Shader "S" {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.02, 0.02, 0.02)
            float inputs:metallic = 1.0
            float inputs:roughness = $1
            token outputs:surface
        }
    }
    def DomeLight "D" { float inputs:intensity = 1.0; color3f inputs:color = (1,1,1) }
}
USDA
}
scene 0.05 > "$TMP/smooth.usda"
scene 0.9  > "$TMP/rough.usda"

R() {
  "$LUSDRENDER" "$1" "$2" -rtPreview -materialShading lightrt-bsdf \
      -materialResolver tydra-next -autoframe -w 48 -height 48 -samples 8 \
      > "$TMP/log" 2>&1 || fail "render failed for $1: $(cat "$TMP/log")"
}
R "$TMP/smooth.usda" "$TMP/smooth.png"
R "$TMP/rough.usda"  "$TMP/rough.png"

python3 - "$TMP/smooth.png" "$TMP/rough.png" <<'PY' || exit $?
import sys, struct, zlib
def mean(path):
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
sm=mean(sys.argv[1]); rg=mean(sys.argv[2])
print(f"mean brightness: smooth(r=0.05)={sm:.1f} rough(r=0.9)={rg:.1f}")
# With the bug the smooth surface reflects the env at ~280x and clips to white
# (mean near 765, ~25x the rough surface). Bounded, the two are within a small
# factor: a mirror metal IS a little brighter than a rough one under a uniform
# dome (single-scatter GGX loses energy as roughness rises -- after the
# energy-conservation fixes the measured ratio is ~1.7), so the gate is 2.5x:
# far above physical spread, far below the failure mode.
if sm > rg * 2.5:
    print("FAIL: smooth-metal environment specular is unbounded -- reflecting the "
          "dome far brighter than a rough surface (analytic BRDF at the mirror "
          "direction instead of the split-sum)."); sys.exit(1)
# Absolute backstop: a unit dome must never drive the reflection into clipped
# white (the bug's signature), whatever happens to the rough reference.
if sm > 400:
    print("FAIL: smooth-metal reflection of a unit dome is clipped-white bright "
          f"(mean {sm:.1f}) -- environment specular is unbounded."); sys.exit(1)
print("PASS: environment specular is bounded across roughness in bsdf mode")
PY
