#!/usr/bin/env bash
#
# lusdrender: the two holes left in MeshLightAPI support on the `core` loader.
#
#   1. inputs:normalize was ignored. A normalized light holds its POWER fixed as
#      its area changes, so its radiance is divided by the emitting (world) area.
#      Without that, a normalized mesh light gets BRIGHTER the larger it is --
#      exactly backwards. Asserted as an equivalence a constant factor cannot
#      fake: a normalized light of area 4 and intensity 40 must render like an
#      un-normalized one of intensity 10.
#
#   2. an INSTANCEABLE emissive mesh registered no light at all. Mesh lights were
#      collected from the flat triangle list only, and an instanced prototype is
#      deliberately not in it -- its geometry lives in a BLAS, placed by the TLAS.
#      However bright, it lit nothing. Asserted as an equivalence too: two
#      instanced lamps must light the floor exactly as the same two lamps written
#      out in full do. (This also pins the per-PLACEMENT emission: each instance is
#      its own light, so registering the prototype once would come out half as
#      bright.)
#
# Both are compared as ratios against a control, so neither can pass by rendering
# a plausible-looking constant. A no-emitter baseline guards against the whole
# thing being satisfied by the fallback camera headlight.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

floor() {  # $1 = out path; a white floor in the z=0 plane, camera on +z
  cat <<'USDA'
  def Mesh "Floor" {
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-4,-4,0), (4,-4,0), (4,4,0), (-4,4,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(1, 1, 1)]
    uniform token subdivisionScheme = "none"
  }
USDA
}

# A 2x2 emitter (area 4) facing the floor. $1 = extra light attrs.
emitter() {
  cat <<USDA
  def Mesh "Emitter" (prepend apiSchemas = ["MeshLightAPI"]) {
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [3, 2, 1, 0]
    point3f[] points = [(-1,-1,6), (1,-1,6), (1,1,6), (-1,1,6)]
    color3f inputs:color = (1, 1, 1)
$1
    uniform token subdivisionScheme = "none"
  }
USDA
}

scene() {  # $1 = body, $2 = out path
  { echo '#usda 1.0'
    echo '(defaultPrim = "World" upAxis = "Y")'
    echo 'def Xform "World" {'
    floor
    printf '%s\n' "$1"
    echo '}'
  } > "$2"
}

# 1. normalize.
scene "$(emitter '    float inputs:intensity = 40')"                            "$TMP/plain40.usda"
scene "$(emitter '    float inputs:intensity = 10')"                            "$TMP/plain10.usda"
scene "$(emitter '    float inputs:intensity = 40
    bool inputs:normalize = 1')"                                                "$TMP/norm40.usda"
scene ''                                                                        "$TMP/dark.usda"

# 2. instanced vs flattened. Same two lamps, once through a referenced prototype
# (native instancing -> TLAS) and once written out in place.
cat > "$TMP/lamp.usda" <<'USDA'
#usda 1.0
(defaultPrim = "Lamp" upAxis = "Y")
def Xform "Lamp" {
  def Mesh "Emitter" (prepend apiSchemas = ["MeshLightAPI"]) {
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [3, 2, 1, 0]
    point3f[] points = [(-1,-1,6), (1,-1,6), (1,1,6), (-1,1,6)]
    float inputs:intensity = 40
    color3f inputs:color = (1, 1, 1)
    uniform token subdivisionScheme = "none"
  }
}
USDA
scene '  def Xform "LampA" (instanceable = true prepend references = @./lamp.usda@</Lamp>) {
    double3 xformOp:translate = (-2, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def Xform "LampB" (instanceable = true prepend references = @./lamp.usda@</Lamp>) {
    double3 xformOp:translate = (2, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }' "$TMP/inst.usda"
scene "$(printf '  def Xform "LampA" {\n    double3 xformOp:translate = (-2, 0, 0)\n    uniform token[] xformOpOrder = ["xformOp:translate"]\n%s  }\n  def Xform "LampB" {\n    double3 xformOp:translate = (2, 0, 0)\n    uniform token[] xformOpOrder = ["xformOp:translate"]\n%s  }' "$(emitter '    float inputs:intensity = 40')" "$(emitter '    float inputs:intensity = 40')")" "$TMP/flat.usda"

render() {  # $1 = scene, $2 = out
  "$LUSDRENDER" "$1" "$2" -rtPreview -viewDir 0,0,1 -bg 0,0,0 -ambient 0 \
    -w 96 -height 96 -samples 32 -materialShading lightrt-bsdf > "$2.log" 2>&1
  if [ ! -s "$2" ]; then
    echo "FAIL: render failed for $2"
    cat "$2.log"
    return 1
  fi
  return 0
}

for s in plain40 plain10 norm40 dark inst flat; do
  render "$TMP/$s.usda" "$TMP/$s.png" || exit 1
done

python3 - "$TMP" <<'PY'
import struct, sys, zlib

def read_png_gray(path):
    d = open(path, "rb").read()
    w = h = color = None
    idat = b""
    pos = 8
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        for x in range(w):
            px = line[x * nch:x * nch + 3]
            out.append((px[0] + px[1] + px[2]) / 3.0)
        prev = line
    return out

def mean_lit(path):
    lum = read_png_gray(path)
    lit = [v for v in lum if v > 6]
    return sum(lit) / max(1, len(lit))

tmp = sys.argv[1]
m = {n: mean_lit(f"{tmp}/{n}.png") for n in
     ("plain40", "plain10", "norm40", "dark", "inst", "flat")}

# Not vacuous: the emitter must actually be lighting the floor, i.e. the lit scene
# must differ from the one with no emitter at all (which the headlight still lights).
if abs(m["plain40"] - m["dark"]) < 5.0:
    print(f"FAIL: the mesh light does not light the floor at all (emitter "
          f"{m['plain40']:.1f} vs no emitter {m['dark']:.1f}). Every check below "
          f"would be vacuous.")
    sys.exit(1)

# 1. normalize == dividing the radiance by the emitting area (4 here).
rel = abs(m["norm40"] - m["plain10"]) / m["plain10"]
if rel > 0.03:
    print(f"FAIL: inputs:normalize is not dividing by the emitting area "
          f"({rel * 100:.1f}% off). A normalized light of area 4 and intensity 40 "
          f"must match an un-normalized one of intensity 10 (normalized "
          f"{m['norm40']:.1f}, expected {m['plain10']:.1f}; un-normalized "
          f"{m['plain40']:.1f}).")
    sys.exit(1)
if abs(m["norm40"] - m["plain40"]) < 5.0:
    print(f"FAIL: inputs:normalize changed nothing ({m['norm40']:.1f} vs "
          f"{m['plain40']:.1f}) -- it is being ignored.")
    sys.exit(1)

# 2. an instanced emissive mesh lights exactly as the flattened one does.
rel = abs(m["inst"] - m["flat"]) / m["flat"]
if rel > 0.01:
    print(f"FAIL: two INSTANCED mesh lights do not light the floor the way the "
          f"same two lamps written out in full do ({m['inst']:.1f} vs "
          f"{m['flat']:.1f}, {rel * 100:.1f}% off). Mesh lights are collected from "
          f"the flat triangle list, which an instanced prototype is not in -- its "
          f"geometry lives in a BLAS placed by the TLAS. Each PLACEMENT has to "
          f"register its own light (CollectMeshLightsCore).")
    sys.exit(1)

print(f"PASS: inputs:normalize divides by the emitting area (normalized "
      f"{m['norm40']:.1f} == intensity/4 {m['plain10']:.1f}, un-normalized "
      f"{m['plain40']:.1f}), and instanced mesh lights match flattened ones "
      f"({m['inst']:.1f} vs {m['flat']:.1f})")
PY
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
exit 0
