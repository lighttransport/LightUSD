#!/usr/bin/env bash
set -uo pipefail
SKIP=77
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${LUSDVIEW:-$ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
command -v zip >/dev/null || { echo "SKIP: zip missing"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT"
printf '%s\n' '{"window_size":{"width":256,"height":256}}' > "$OUT/config.json"

python3 - "$OUT" <<'PY'
import os,sys
for stem,values in [('flat',(0,0)),('raised',(0,255))]:
 for tile,value in zip((1001,1002),values):
  with open(os.path.join(sys.argv[1],f'{stem}.{tile}.ppm'),'wb') as f:
   f.write(b'P6\n8 8\n255\n'+bytes((value,value,value))*64)
PY

write_scene() {
  local file="$1" stem="$2"
  cat > "$file" <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Grid" {
    uniform bool doubleSided = 1
    uniform token subdivisionScheme = "none"
    point3f[] points = [(-1,-1,0),(0,-1,0),(1,-1,0),(-1,1,0),(0,1,0),(1,1,0)]
    normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1),(0,0,1),(0,0,1)] (interpolation = "vertex")
    texCoord2f[] primvars:st = [(0,0),(1,0),(2,0),(0,1),(1,1),(2,1)] (interpolation = "vertex")
    int[] faceVertexCounts = [4,4]
    int[] faceVertexIndices = [0,1,4,3,1,2,5,4]
    rel material:binding = </World/M>
  }
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_standard_surface_surfaceshader"
      color3f inputs:base_color = (0.8,0.15,0.05)
      float inputs:specular_roughness = 0.7
      float inputs:displacement.connect = </World/M/D.outputs:out>
      token outputs:surface
    }
    def Shader "D" {
      uniform token info:id = "ND_image_float"
      asset inputs:file = @./$stem.<UDIM>.ppm@
      float outputs:out
    }
  }
}
USDA
}
write_scene "$OUT/standard-flat.usda" flat
write_scene "$OUT/standard-raised.usda" raised

write_preview_scene() {
  local file="$1" stem="$2"
  cat > "$file" <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Grid" {
    uniform bool doubleSided = 1
    uniform token subdivisionScheme = "none"
    point3f[] points = [(-1,-1,0),(0,-1,0),(1,-1,0),(-1,1,0),(0,1,0),(1,1,0)]
    normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1),(0,0,1),(0,0,1)] (interpolation = "vertex")
    texCoord2f[] primvars:st = [(0,0),(1,0),(2,0),(0,1),(1,1),(2,1)] (interpolation = "vertex")
    int[] faceVertexCounts = [4,4]
    int[] faceVertexIndices = [0,1,4,3,1,2,5,4]
    rel material:binding = </World/M>
  }
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.8,0.15,0.05)
      float inputs:roughness = 0.7
      float inputs:displacement.connect = </World/M/D.outputs:r>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "D" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$stem.<UDIM>.ppm@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
    }
  }
}
USDA
}
write_preview_scene "$OUT/preview-flat.usda" flat
write_preview_scene "$OUT/preview-raised.usda" raised

package_scene() {
  local name="$1" stem="$2" dir="$OUT/pkg-$1"
  mkdir -p "$dir"
  cp "$OUT/$name.usda" "$dir/$name.usda"
  cp "$OUT/$stem.1001.ppm" "$OUT/$stem.1002.ppm" "$dir/"
  (cd "$dir" && zip -0 -q "$OUT/$name.usdz" \
    "$name.usda" "$stem.1001.ppm" "$stem.1002.ppm")
}
package_scene standard-flat flat
package_scene standard-raised raised
package_scene preview-flat flat
package_scene preview-raised raised

render() {
  local name="$1" scene="$2"; shift 2
  "$BIN" --headless --backend vk --config "$OUT/config.json" --frames 4 \
    --size 256x256 \
    --no-grid --mode albedo --view-dir '1,0,-1' --screenshot "$OUT/$name.ppm" \
    "$@" "$scene" >"$OUT/$name.log" 2>&1
}
render standard-flat "$OUT/standard-flat.usda" || { echo "SKIP: Vulkan raster unavailable"; exit "$SKIP"; }
render standard-raised "$OUT/standard-raised.usda" || { echo "FAIL: Standard displacement UDIM render"; exit 1; }
render preview-flat "$OUT/preview-flat.usda" || { echo "FAIL: Preview flat displacement render"; exit 1; }
render preview-raised "$OUT/preview-raised.usda" || { echo "FAIL: Preview displacement UDIM render"; exit 1; }
for family in standard preview; do
  for state in flat raised; do
    render "$family-$state-usdz" "$OUT/$family-$state.usdz" || { echo "FAIL: packaged $family $state render"; exit 1; }
  done
done

python3 - "$OUT/standard-flat.ppm" "$OUT/standard-raised.ppm" \
  "$OUT/preview-flat.ppm" "$OUT/preview-raised.ppm" \
  "$OUT/standard-flat-usdz.ppm" "$OUT/standard-raised-usdz.ppm" \
  "$OUT/preview-flat-usdz.ppm" "$OUT/preview-raised-usdz.ppm" <<'PY'
import re,sys
def ppm(path):
 d=open(path,'rb').read(); m=re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s',d)
 if not m: raise SystemExit(f'invalid PPM: {path}')
 return tuple(map(int,m.groups())),d[m.end():]
def mad(a,b): return sum(abs(x-y) for x,y in zip(a,b))/len(a)
imgs=[ppm(p) for p in sys.argv[1:]]
if len({x[0] for x in imgs}) != 1: raise SystemExit('image size mismatch')
sf,sr,pf,pr,sfu,sru,pfu,pru=(x[1] for x in imgs)
standard=mad(sf,sr); preview=mad(pf,pr)
package=max(mad(sf,sfu),mad(sr,sru),mad(pf,pfu),mad(pr,pru))
print(f'Standard response MAD={standard:.4f}, Preview response MAD={preview:.4f}, package MAD={package:.4f}')
if standard < 2.0: raise SystemExit('FAIL: Standard UDIM displacement did not change silhouette')
if preview < 2.0: raise SystemExit('FAIL: Preview UDIM displacement did not change silhouette')
if package > 2.0: raise SystemExit('FAIL: displacement external/USDZ parity')
print('PASS: core Standard/Preview displacement UDIM affects geometry with package parity')
PY
