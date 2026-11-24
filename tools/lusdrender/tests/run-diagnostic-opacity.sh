#!/usr/bin/env bash
# Analytic two-layer emissive compositing, including texture alpha scale/bias.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/gpu-test-common.sh"
BIN="${1:?lusdrender required}"
BACKEND="${2:-cpu}"
if [ "$#" -ge 2 ]; then shift 2; else shift 1; fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP" <<'PY'
from pathlib import Path
import struct,sys,zlib
root=Path(sys.argv[1])
def chunk(name,data):return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data)&0xffffffff)
(root/'alpha.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes([255,0,0,64,255,0,0,192])))+chunk(b'IEND',b''))
for threshold in (0,.3):
 text='#usda 1.0\n'
 for name,x0,x1,u in [('Left',-2,0,.25),('Right',0,2,.75)]:
  text+=f'''def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {{
   uniform token subdivisionScheme = "none"
   point3f[] points = [({x0},-2,0),({x1},-2,0),({x1},2,0),({x0},2,0)]
   int[] faceVertexCounts = [4]
   int[] faceVertexIndices = [0,1,2,3]
   texCoord2f[] primvars:UV1 = [({u},0.5)] (interpolation = "constant")
   rel material:binding = </Front>
  }}\n'''
 text+='''def Mesh "Back" (prepend apiSchemas = ["MaterialBindingAPI"]) {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-3,-3,-1),(3,-3,-1),(3,3,-1),(-3,3,-1)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  rel material:binding = </Background>
 }
 def Material "Background" {
  token outputs:surface.connect = </Background/S.outputs:surface>
  def Shader "S" { uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0,0,0)
   color3f inputs:specularColor = (0,0,0)
   int inputs:useSpecularWorkflow = 1
   color3f inputs:emissiveColor = (0,0,1)
   token outputs:surface
  }
 }
 def Material "Front" {
  token outputs:surface.connect = </Front/S.outputs:surface>
  def Shader "S" { uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0,0,0)
   color3f inputs:specularColor = (0,0,0)
   int inputs:useSpecularWorkflow = 1
   color3f inputs:emissiveColor = (1,0,0)
   float inputs:opacity.connect = </Front/T.outputs:a>
   float inputs:opacityThreshold = '''+str(threshold)+'''
   token outputs:surface
  }
  def Shader "T" { uniform token info:id = "UsdUVTexture"
   float2 inputs:st.connect = </Front/UV.outputs:result>
   asset inputs:file = @alpha.png@
   token inputs:sourceColorSpace = "raw"
   float4 inputs:scale = (1,1,1,0.5)
   float4 inputs:bias = (0,0,0,0.1)
   float outputs:a
  }
  def Shader "UV" { uniform token info:id = "UsdPrimvarReader_float2"
   token inputs:varname = "UV1"
   float2 outputs:result
  }
 }
 def Camera "C" {
  double3 xformOp:translate = (0,0,5)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  float focalLength = 50
  float horizontalAperture = 20
  float verticalAperture = 20
 }
'''
 for opacity_mode in ('transparent','presence'):
  variant=text.replace('float inputs:opacityThreshold', 'token inputs:opacityMode = "'+opacity_mode+'"\n   float inputs:opacityThreshold')
  (root/(( 'blend' if threshold==0 else 'cutout')+'-'+opacity_mode+'.usda')).write_text(variant)
overflow='#usda 1.0\n'
for i in range(20):
 z=-i*.1
 overflow+=f'''def Mesh "Layer{i}" {{
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-3,-3,{z}),(3,-3,{z}),(3,3,{z}),(-3,3,{z})]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  rel material:binding = </M>
 }}\n'''
overflow+='''def Material "M" {
 token outputs:surface.connect = </M/S.outputs:surface>
 def Shader "S" { uniform token info:id = "UsdPreviewSurface"
  color3f inputs:emissiveColor = (0.2,0.2,0.2)
  float inputs:opacity = 0.25
  token inputs:opacityMode = "presence"
  token outputs:surface
 }
}
def Camera "C" {
 double3 xformOp:translate = (0,0,5)
 uniform token[] xformOpOrder = ["xformOp:translate"]
 float focalLength = 50
 float horizontalAperture = 20
 float verticalAperture = 20
}
'''
(root/'overflow.usda').write_text(overflow)
(root/'clipped.usda').write_text((root/'blend-presence.usda').read_text().replace(
 'float focalLength = 50', 'float2 clippingRange = (0.1,5.5)\n  float focalLength = 50'))
PY
ARGS=(-rtPreview); if [ "$BACKEND" = vkr ]; then ARGS=(-vkr); fi
for RESOLVER in legacy tydra; do
 for MODE in blend-transparent blend-presence cutout-transparent cutout-presence; do
  "$BIN" "$TMP/$MODE.usda" "$TMP/$RESOLVER-$MODE.png" "${ARGS[@]}" "$@" \
   -diagnosticEmission -camera /C -w 16 -height 16 -threads 1 -materialResolver "$RESOLVER" \
   > "$TMP/$RESOLVER-$MODE.log" 2>&1 || lusdrender_test_render_failed "$BACKEND" "$TMP/$RESOLVER-$MODE.log" "$?"
 done
 "$BIN" "$TMP/clipped.usda" "$TMP/$RESOLVER-clipped.png" "${ARGS[@]}" "$@" \
  -diagnosticEmission -camera /C -w 16 -height 16 -threads 1 -bg 0,0,0 \
  -materialResolver "$RESOLVER" > "$TMP/$RESOLVER-clipped.log" 2>&1 \
  || lusdrender_test_render_failed "$BACKEND" "$TMP/$RESOLVER-clipped.log" "$?"
done
if "$BIN" "$TMP/overflow.usda" "$TMP/overflow.png" "${ARGS[@]}" "$@" \
  -diagnosticEmission -camera /C -w 4 -height 4 -threads 1 \
  > "$TMP/overflow.log" 2>&1; then
  echo 'FAIL: excessive diagnostic transparency layers were silently truncated'; exit 1
fi
if ! grep -Eq 'maximum 16 (transparency )?layers' "$TMP/overflow.log"; then
  cat "$TMP/overflow.log"; exit 1
fi
if [ -e "$TMP/overflow.png" ]; then echo 'FAIL: incomplete diagnostic image was written'; exit 1; fi
if grep -Eq 'Validation Error|VUID-' "$TMP"/*.log; then
  cat "$TMP"/*.log
  exit 1
fi
python3 - "$TMP" "$ROOT" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,str(Path(sys.argv[2])/'examples/lusdview/tests'))
from asset_fingerprint import _read_image
root=Path(sys.argv[1])
def encode(x):return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
for resolver in ('legacy','tydra'):
 for mode in ('blend-transparent','blend-presence','cutout-transparent','cutout-presence'):
  w,h,pixels=_read_image(str(root/f'{resolver}-{mode}.png'));assert (w,h)==(16,16)
  for x,alpha in [(4,64/255*.5+.1),(12,192/255*.5+.1)]:
   if mode.startswith('cutout'):alpha=float(alpha>=.3)
   emission_alpha=alpha if mode.endswith('presence') or mode.startswith('cutout') else 1
   expected=(encode(emission_alpha),0,encode(1-alpha));actual=tuple(pixels[(8*w+x)*3:(8*w+x)*3+3])
   assert max(abs(a-b)for a,b in zip(actual,expected))<=3,(resolver,mode,x,actual,expected)
 w,h,pixels=_read_image(str(root/f'{resolver}-clipped.png'))
 for x,alpha in [(4,64/255*.5+.1),(12,192/255*.5+.1)]:
  expected=(encode(alpha),0,0);actual=tuple(pixels[(8*w+x)*3:(8*w+x)*3+3])
  assert max(abs(a-b)for a,b in zip(actual,expected))<=3,(resolver,'far-clipped continuation',x,actual,expected)
print('PASS: emissive transparent/presence blending and cutout match independent two-layer math')
PY
