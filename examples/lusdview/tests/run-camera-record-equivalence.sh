#!/usr/bin/env bash
#
# Regression: loose and USDZ loaders must produce equivalent camera records
# (world pose, lens properties, projection) for authored USD cameras.
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build/lusdview}}"
[ -x "$BIN" ] || BIN="$ROOT/build_ninja/lusdview"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
command -v xvfb-run >/dev/null || { echo "SKIP: xvfb-run required for deterministic GL headless"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT"
printf '%s\n' '{"window_size":{"width":96,"height":96}}' > "$OUT/config.json"

cat > "$OUT/cameras.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    float3[] extent = [(-1,-1,0),(1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
  }
  # Perspective camera with full lens params, offset apertures, and rotation.
  def Camera "MainCam" {
    float2 clippingRange = (0.1, 10000)
    float exposure = 0.5
    float focusDistance = 8
    float fStop = 2.8
    double shutter:open = -0.25
    double shutter:close = 0.5
    uniform token stereoRole = "left"
    float4[] clippingPlanes = [(1, 0, 0, -2), (0, 1, 0, -3)]
    float focalLength = 35
    float horizontalAperture = 36
    float horizontalApertureOffset = 0.1
    float verticalAperture = 24
    float verticalApertureOffset = -0.05
    token projection = "perspective"
    double3 xformOp:translate = (2, 3, 5)
    float3 xformOp:rotateXYZ = (-10, 25, 5)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
  }
  # Orthographic camera with different settings, no rotation.
  def Camera "OrthoCam" {
    float2 clippingRange = (1, 500)
    float exposure = 0
    float focusDistance = 20
    float fStop = 8
    double shutter:open = 0
    double shutter:close = 0
    uniform token stereoRole = "right"
    float4[] clippingPlanes = [(0, 0, 1, -4)]
    float focalLength = 50
    float horizontalAperture = 20.955
    float verticalAperture = 15.291
    token projection = "orthographic"
    double3 xformOp:translate = (-3, 0, 10)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  # Camera nested inside an Xform.
  def Xform "Rig" {
    double3 xformOp:rotateXYZ = (45, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    def Camera "RigCam" {
      float2 clippingRange = (0.01, 50)
      float exposure = -1
      float focalLength = 85
      float horizontalAperture = 30
      float verticalAperture = 18
      token projection = "perspective"
      double3 xformOp:translate = (0, 2, 0)
      uniform token[] xformOpOrder = ["xformOp:translate"]
    }
  }
}
USDA

RUN_PREFIX=(xvfb-run -a)

python3 - "$OUT" "cameras" <<'PY'
import pathlib,sys,zipfile
root=pathlib.Path(sys.argv[1]); name=sys.argv[2]
with zipfile.ZipFile(root/(name+'.usdz'),'w',compression=zipfile.ZIP_STORED) as archive:
 archive.write(root/(name+'.usda'),name+'.usda')
 for image in sorted(root.glob('*.ppm')): archive.write(image,image.name)
PY

run_loader() {
  local name="$1"; local scene="$2"; shift 2
  local attempt
  for attempt in 1 2; do
    LUSDVIEW_DUMP_CAMERA_RECORDS=1 "${RUN_PREFIX[@]}" "$BIN" --backend gl \
      --config "$OUT/config.json" --frames 1 --no-grid "$@" \
      "$scene" >"$OUT/$name.log" 2>&1 && break
    [ "$attempt" = 1 ] || return 1
  done
  sed -n 's/^\[lusdview-camera\] //p' "$OUT/$name.log" > "$OUT/$name.records"
  [ -s "$OUT/$name.records" ]
}
run_loader core "$OUT/cameras.usda" || { echo "FAIL: default camera extraction"; exit 1; }
run_loader packaged "$OUT/cameras.usdz" || { echo "FAIL: packaged camera extraction"; exit 1; }

python3 - "$OUT/core.records" "$OUT/packaged.records" <<'PY'
import math, sys
def read(p):
    rows = []
    for line in open(p):
        if not line.strip(): continue
        tokens = line.split()
        if len(tokens) < 29:
            print(f"short record ({len(tokens)} tokens): {line.strip()}")
            raise SystemExit(1)
        rows.append(tokens)
    return rows

a = read(sys.argv[1])
b = read(sys.argv[2])

if len(a) != 3 or len(b) != 3:
    print(f"expected 3 camera records, got core={len(a)} packaged={len(b)}")
    raise SystemExit(1)

# Field layout:
# 0  idx
# 1  absPath (string)
# 2  projection (string)
# 3  projection (int)
# 4  focalLength
# 5  horizontalAperture
# 6  verticalAperture
# 7  horizontalApertureOffset
# 8  verticalApertureOffset
# 9  exposure
# 10 zNear
# 11 zFar
# 12 fovYDeg
# 13 eye[0]
# 14 eye[1]
# 15 eye[2]
# 16 up[0]
# 17 up[1]
# 18 up[2]
# 19 forward[0]
# 20 forward[1]
# 21 forward[2]
# 22 focusDistance
# 23 fStop
# 24 shutterOpen
# 25 shutterClose
# 26 stereoRole (0 mono, 1 left, 2 right)
# 27 clipping plane count
# 28 clipping plane weighted checksum

for ci, (x, y) in enumerate(zip(a, b)):
    # Compare absolute paths
    if x[1] != y[1]:
        print(f"camera {ci} path: core={x[1]} packaged={y[1]}")
        raise SystemExit(1)
    # Compare projection strings
    if x[2] != y[2]:
        print(f"camera {ci} projection: core={x[2]} packaged={y[2]}")
        raise SystemExit(1)
    # Compare numeric fields (indices 3..28)
    for fi in range(3, 29):
        u = float(x[fi])
        v = float(y[fi])
        if not math.isclose(u, v, rel_tol=1e-4, abs_tol=1e-4):
            print(f"camera {ci} field {fi} ({x[1]}): core={u} packaged={v}")
            raise SystemExit(1)

expected_advanced = {
    "/World/MainCam": (8.0, 2.8, -0.25, 0.5, 1, 2),
    "/World/OrthoCam": (20.0, 8.0, 0.0, 0.0, 2, 1),
    "/World/Rig/RigCam": (5.0, 0.0, 0.0, 0.0, 0, 0),
}
for row in a:
    got = tuple(float(row[i]) for i in range(22, 28))
    want = expected_advanced[row[1]]
    if any(not math.isclose(g, w, rel_tol=1e-4, abs_tol=1e-4)
           for g, w in zip(got, want)):
        print(f"advanced camera fields {row[1]}: got={got} want={want}")
        raise SystemExit(1)

print(f"PASS: {len(a)} loose/USDZ camera records equivalent")
PY
rc=$?
[ "$rc" -eq 0 ] || exit 1

echo "PASS: camera record equivalence (loose and USDZ)"
exit 0
