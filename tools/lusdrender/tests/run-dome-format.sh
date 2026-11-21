#!/usr/bin/env bash
#
# Regression test: DomeLight `inputs:texture:format` (R10).
#
# lusdrender sampled every dome texture as latlong. A "mirroredBall" or
# "angular" probe must be remapped first (RemapProbeToLatlong, the same probe
# projection lusdview's TexToolsProbeToEquirect uses).
#
# The probe: green everywhere except RED outside the unit disc (the square's
# corners, uv radius > 0.52). The mirrored-ball projection only ever samples
# inside the disc (radius <= 0.5), so a correctly remapped probe shows NO red
# from any direction. Read as latlong (the bug) the corner red sits at the
# poles, so a camera looking straight up sees a large red cap.
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
command -v cygpath >/dev/null 2>&1 && TMP="$(cygpath -m "$TMP")"  # native exes need a Windows path
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

# 128x128 probe PNG: green disc, red outside it (unreachable by the ball
# mapping, the poles under latlong).
python3 - "$TMP/probe.png" <<'PY'
import struct, sys, zlib
W = H = 128
rows = b""
for y in range(H):
    row = bytearray([0])
    for x in range(W):
        dx = (x + 0.5) / W - 0.5
        dy = (y + 0.5) / H - 0.5
        outside = (dx * dx + dy * dy) ** 0.5 > 0.52
        row += bytes([255, 0, 0] if outside else [0, 255, 0])
    rows += bytes(row)
def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(rows))
png += chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY

scene() { # $1 = texture:format attribute line (or empty)
cat <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Floor" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-101,-1), (1,-101,-1), (1,-101,1), (-1,-101,1)]
    uniform token subdivisionScheme = "none"
  }
  def DomeLight "Sky" {
    float inputs:intensity = 1.0
    asset inputs:texture:file = @$TMP/probe.png@
    $1
  }
}
USDA
}
scene 'uniform token inputs:texture:format = "mirroredBall"' > "$TMP/ball.usda"
scene '' > "$TMP/latlong.usda"

# Fraction of red / green pixels over the whole frame (background = the dome).
stats() {
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
prev=bytearray(stride); p=0; red=0; green=0; n=0
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
        r,g = line[i], line[i+1]
        if r > 2*g + 20: red += 1
        elif g > 2*r + 20: green += 1
        n += 1
print(f"{red/n:.4f} {green/n:.4f}")
PY
}

# Camera below the marker looking straight UP (-viewDir is target->eye, so
# 0,-1,0 puts the eye underneath), zoomed out so the tiny marker mesh leaves the
# background visible. -autoframe would fill the frame with the mesh -- use the
# fitScale framing instead.
COMMON=(-w 64 -height 64 -samples 1 -fitScale 8 -viewDir 0,-1,0)
"$LUSDRENDER" "$TMP/ball.usda" "$TMP/ball.png" "${COMMON[@]}" -rtPreview \
    >/dev/null 2>&1 || fail "mirroredBall render failed"
"$LUSDRENDER" "$TMP/latlong.usda" "$TMP/latlong.png" "${COMMON[@]}" -rtPreview \
    >/dev/null 2>&1 || fail "latlong render failed"

read BALL_R BALL_G <<< "$(stats "$TMP/ball.png" | tr -d '\015')"
read LL_R LL_G <<< "$(stats "$TMP/latlong.png" | tr -d '\015')"
echo "mirroredBall: red=$BALL_R green=$BALL_G; latlong: red=$LL_R green=$LL_G"

# 1. The ball mapping never reaches outside the disc: no red anywhere.
python3 -c "import sys; sys.exit(0 if float('$BALL_R') < 0.02 else 1)" \
  || fail "mirroredBall shows red fraction $BALL_R -- the outside-disc region leaked in (sampled as latlong?)"
# 2. Read as latlong the corner red IS visible at the pole -- this guards the
#    probe/camera setup itself (an all-green probe would pass check 1 vacuously).
python3 -c "import sys; sys.exit(0 if float('$LL_R') > 0.15 else 1)" \
  || fail "latlong control shows red fraction $LL_R -- the test camera no longer sees the pole; fixture broken"
# 3. The format attribute must actually change the interpretation.
[ "$BALL_R $BALL_G" != "$LL_R $LL_G" ] \
  || fail "texture:format=mirroredBall renders identically to latlong (format ignored)"

echo "PASS: DomeLight texture:format=mirroredBall is remapped before sampling"
exit 0
