#!/usr/bin/env bash
#
# Regression test: lusdrender's tydra (-rtPreview) path must sample the UV
# set a texture's UsdPrimvarReader names, not always the primary `st`.
#
# AddRTPreviewMeshCore used to read one UV set from a fixed preference list
# (always `st`) and ignore RenderTexture::uv_primvar, so a base-color texture
# bound through `varname="uvSet1"` sampled `st` -- the render was byte-identical
# to one authored with `varname="st"`. The fixture's `uvSet1` covers only the
# lower-left [0,0.25] quarter, so a correct render samples a zoomed sub-tile and
# must differ from the `st` control.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"
MODEL="$REPO_ROOT/models/multi-uv-quad.usda"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"; exit $SKIP
fi
if [ ! -f "$MODEL" ]; then
  echo "SKIP: fixture $MODEL missing"; exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

# The control: the same scene with the reader pointed at the PRIMARY set. Written
# next to the fixture so its relative texture path still resolves.
if ! grep -q 'token inputs:varname = "uvSet1"' "$MODEL"; then
  echo "SKIP: fixture does not bind uvSet1"; exit $SKIP
fi
CTRL="$(dirname "$MODEL")/.uv-set-routing-rt-control.usda"
sed 's/token inputs:varname = "uvSet1"/token inputs:varname = "st"/' "$MODEL" > "$CTRL"
trap 'rm -rf "$TMP"; rm -f "$CTRL"' EXIT

R() {
  "$LUSDRENDER" "$1" "$2" -rtPreview -autoframe -w 128 -height 128 -samples 2 \
      > "$TMP/log" 2>&1 || fail "render failed for $1: $(cat "$TMP/log")"
}
R "$MODEL" "$TMP/uvset1.png"
R "$CTRL"  "$TMP/st.png"

# Assert the two renders differ. A byte-identical pair is the exact bug (uvSet1
# silently falling back to st). Use Python for a robust pixel compare.
python3 - "$TMP/uvset1.png" "$TMP/st.png" <<'PY' || exit $?
import sys, struct, zlib
def read_rgb(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n": return None
    w=h=None; idat=b""; color=None; pos=8
    while pos+8 <= len(d):
        ln=struct.unpack(">I", d[pos:pos+4])[0]; typ=d[pos+4:pos+8]; body=d[pos+8:pos+8+ln]
        if typ==b"IHDR": w,h,_bd,color=struct.unpack(">IIBB", body[:10])
        elif typ==b"IDAT": idat+=body
        elif typ==b"IEND": break
        pos+=12+ln
    if w is None or color not in (2,6): return None
    nch=3 if color==2 else 4; raw=zlib.decompress(idat); stride=w*nch; out=[]; prev=bytearray(stride); p=0
    for _ in range(h):
        f=raw[p]; p+=1; line=bytearray(raw[p:p+stride]); p+=stride
        for i in range(stride):
            a=line[i-nch] if i>=nch else 0; b=prev[i]; c=prev[i-nch] if i>=nch else 0; x=line[i]
            if f==1: x=(x+a)
            elif f==2: x=(x+b)
            elif f==3: x=(x+((a+b)>>1))
            elif f==4:
                p_=a+b-c; pa=abs(p_-a); pb=abs(p_-b); pc=abs(p_-c)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c); x=(x+pr)
            line[i]=x&0xff
        prev=line
        for i in range(0,stride,nch): out.append((line[i],line[i+1],line[i+2]))
    return out
a=read_rgb(sys.argv[1]); b=read_rgb(sys.argv[2])
if a is None or b is None:
    print("FAIL: could not read renders"); sys.exit(1)
if a==b:
    print("FAIL: uvSet1 and st renders are byte-identical -- lusdrender ignored "
          "the texture's uv_primvar and sampled st for both."); sys.exit(1)
diff=sum(abs(x-y) for pa,pb in zip(a,b) for x,y in zip(pa,pb))/(len(a)*3)
if diff < 2.0:
    print(f"FAIL: renders barely differ (mean |diff| {diff:.2f}) -- uvSet1 likely "
          f"still sampling st."); sys.exit(1)
print(f"PASS: uvSet1 render differs from st (mean |diff| {diff:.1f})")
PY
