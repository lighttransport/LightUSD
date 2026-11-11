#!/usr/bin/env bash
set -uo pipefail
SKIP=77
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${LUSDVIEW:-$ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
command -v timeout >/dev/null || { echo "SKIP: timeout missing"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT"
printf '%s\n' '{"window_size":{"width":96,"height":96}}' > "$OUT/config.json"

python3 - "$OUT/dome.ppm" <<'PY'
import sys
w,h=16,8
with open(sys.argv[1],'wb') as f:
 f.write(f'P6\n{w} {h}\n255\n'.encode())
 for y in range(h):
  for x in range(w):
   f.write(bytes((32+x*12,48+y*20,192-x*6)))
PY

cat > "$OUT/lights.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "A" {
    point3f[] points = [(-1,-1,0),(0,-1,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </World/MatA>
  }
  def Mesh "B" {
    point3f[] points = [(0,-1,0),(1,-1,0),(0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    rel material:binding = </World/MatB>
  }
  def Material "MatA" {
    token outputs:surface.connect = </World/MatA/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (1,0,0)
      token outputs:surface
    }
  }
  def Material "MatB" {
    token outputs:surface.connect = </World/MatB/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0,1,0)
      token outputs:surface
    }
  }
  def RectLight "Key" {
    color3f inputs:color = (0.25,0.5,1)
    float inputs:intensity = 4
    float inputs:exposure = 1
    float inputs:diffuse = 0.75
    float inputs:specular = 0.5
    bool inputs:normalize = 1
    float inputs:width = 2
    float inputs:height = 4
    float inputs:shaping:cone:angle = 30
    float inputs:shaping:cone:softness = 0.2
    float inputs:shaping:focus = 0.4
    bool inputs:shadow:enable = 1
    color3f inputs:shadow:color = (0.05,0.06,0.07)
    float inputs:shadow:distance = 12
    float inputs:shadow:falloff = 3
    float inputs:shadow:falloffGamma = 2
    rel collection:lightLink:includes = </World/A>
    uniform token collection:lightLink:expansionRule = "expandPrims"
    rel collection:shadowLink:includes = </World/B>
    uniform token collection:shadowLink:expansionRule = "expandPrims"
    double3 xformOp:translate = (0,4,0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def SphereLight "Fill" {
    color3f inputs:color = (1,0.25,0.125)
    float inputs:intensity = 2
    float inputs:radius = 0.5
    bool inputs:normalize = 1
    bool inputs:shadow:enable = 0
    double3 xformOp:translate = (2,3,1)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def DistantLight "Sun" {
    color3f inputs:color = (0.8,0.7,0.6)
    float inputs:intensity = 1.5
    float inputs:angle = 0.75
    float inputs:diffuse = 0.9
    float inputs:specular = 0.8
    bool inputs:shadow:enable = 1
    float3 xformOp:rotateXYZ = (15,25,5)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
  }
  def DiskLight "Rim" {
    color3f inputs:color = (0.2,0.4,1)
    float inputs:intensity = 3
    float inputs:radius = 1.25
    bool inputs:normalize = 1
    double3 xformOp:translate = (-2,2,1)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def CylinderLight "Tube" {
    color3f inputs:color = (1,0.6,0.2)
    float inputs:intensity = 2.5
    float inputs:radius = 0.2
    float inputs:length = 3
    bool inputs:normalize = 1
    double3 xformOp:translate = (0,2,2)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def DomeLight "Sky" {
    color3f inputs:color = (0.3,0.45,0.8)
    float inputs:intensity = 0.75
    float inputs:exposure = 1
    float inputs:diffuse = 0.65
    float inputs:specular = 0.85
    asset inputs:texture:file = @./dome.ppm@
    token inputs:texture:format = "latlong"
    float3 xformOp:rotateXYZ = (0,35,0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
  }
  def DomeLight "ProbeMirror" {
    float inputs:intensity = 0.25
    token inputs:texture:format = "mirroredBall"
  }
  def DomeLight "ProbeAngular" {
    float inputs:intensity = 0.125
    token inputs:texture:format = "angular"
  }
}
USDA

python3 - "$OUT" "lights" <<'PY'
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
    LUSDVIEW_DUMP_LIGHT_RECORDS=1 timeout --kill-after=5s 30s \
      "$BIN" --headless --backend vk \
      --config "$OUT/config.json" --frames 1 --no-grid "$@" \
      "$scene" >"$OUT/$name.log" 2>&1 && break
    [ "$attempt" = 1 ] || return 1
  done
  sed -n 's/^\[lusdview-light\] //p' "$OUT/$name.log" > "$OUT/$name.records"
  [ -s "$OUT/$name.records" ]
}
run_loader core "$OUT/lights.usda" || { echo "FAIL: default light extraction"; exit 1; }
run_loader packaged "$OUT/lights.usdz" || { echo "FAIL: packaged light extraction"; exit 1; }

python3 - "$OUT/core.records" "$OUT/packaged.records" <<'PY'
import math,sys
def read(p): return [[float(x) for x in line.split()] for line in open(p) if line.strip()]
a,b=read(sys.argv[1]),read(sys.argv[2])
if len(a)!=8 or len(b)!=8 or any(len(x)!=len(y) for x,y in zip(a,b)):
 print('light record shape mismatch',a,b); raise SystemExit(1)
for li,(x,y) in enumerate(zip(a,b)):
 # The same core conversion must preserve all light/link fields in USDZ.
 for fi,(u,v) in enumerate(zip(x,y)):
  if not math.isclose(u,v,rel_tol=1e-5,abs_tol=1e-5):
   print(f'light {li} field {fi}: core={u} packaged={v}'); raise SystemExit(1)
print('PASS: loose/USDZ light records equivalent')
PY
