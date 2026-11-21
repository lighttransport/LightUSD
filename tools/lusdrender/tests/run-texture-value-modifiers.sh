#!/usr/bin/env bash
# Independent emissive scale/bias and raw/sRGB channel calibration.
set -euo pipefail
BIN="${1:?lusdrender required}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BACKEND="${2:-cpu}"
RENDER_ARGS=(-rtPreview); if [ "$BACKEND" = vkr ]; then RENDER_ARGS=(-vkr); fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1]);(root/'gray.ppm').write_bytes(b'P6\n1 1\n255\n'+bytes([128,128,128]))
for space in ('raw','sRGB'):
 (root/(space+'.usda')).write_text('''#usda 1.0
 def Mesh "M" {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-2,-2,0),(0,-2,0),(0,2,0),(-2,2,0)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  texCoord2f[] primvars:st = [(0.5,0.5)] (interpolation = "constant")
  rel material:binding = </Mat>
 }
 def Material "Mat" {
  token outputs:surface.connect = </Mat/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0,0,0)
   color3f inputs:emissiveColor.connect = </Mat/T.outputs:rgb>
   int inputs:useSpecularWorkflow = 1
   color3f inputs:specularColor = (0,0,0)
   token outputs:surface
  }
  def Shader "T" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @gray.ppm@
   token inputs:sourceColorSpace = "'''+space+'''"
   float4 inputs:scale = (0.2,0.4,0.8,1)
   float4 inputs:bias = (0.05,0.1,0,0)
   float3 outputs:rgb
  }
 }
 def Camera "C" {
  double3 xformOp:translate = (0,0,5)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  float focalLength = 50
  float horizontalAperture = 20
  float verticalAperture = 20
 }
''')
 path=root/(space+'.usda')
 path.write_text(path.read_text()+'''
 def Mesh "M2" {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(0,-2,0),(2,-2,0),(2,2,0),(0,2,0)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  texCoord2f[] primvars:st = [(0.5,0.5)] (interpolation = "constant")
  rel material:binding = </Mat2>
 }
 def Material "Mat2" {
  token outputs:surface.connect = </Mat2/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0,0,0)
   color3f inputs:emissiveColor.connect = </Mat2/T.outputs:rgb>
   int inputs:useSpecularWorkflow = 1
   color3f inputs:specularColor = (0,0,0)
   token outputs:surface
  }
  def Shader "T" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @gray.ppm@
   token inputs:sourceColorSpace = "'''+space+'''"
   float4 inputs:scale = (0.8,0.2,0.3,1)
   float4 inputs:bias = (0,0.2,0.1,0)
   float3 outputs:rgb
  }
 }
''')
PY
for RESOLVER in legacy tydra-next; do
 for SPACE in raw sRGB; do
  "$BIN" "$TMP/$SPACE.usda" "$TMP/$RESOLVER-$SPACE.png" "${RENDER_ARGS[@]}" -diagnosticEmission -materialResolver "$RESOLVER" -camera /C -w 16 -height 16 -threads 1 > "$TMP/$RESOLVER-$SPACE.log" 2>&1 || {
    if [ "$BACKEND" = vkr ] && grep -qiE 'no Vulkan|Failed to create.*Vulkan|ray.query unavailable|not built.*Vulkan' "$TMP/$RESOLVER-$SPACE.log"; then exit 77;fi
    cat "$TMP/$RESOLVER-$SPACE.log";exit 1;
  }
 done
done
python3 - "$TMP" "$ROOT" <<'PY'
from pathlib import Path
import math,sys
sys.path.insert(0,str(Path(sys.argv[2])/"examples/lusdview/tests"))
from asset_fingerprint import _read_image
root=Path(sys.argv[1])
def encode(x):return round((12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055)*255)
for resolver in ('legacy','tydra-next'):
 for space in ('raw','sRGB'):
  decoded=_read_image(str(root/f'{resolver}-{space}.png'))
  assert decoded is not None
  width,height,pixels=decoded;assert (width,height)==(16,16)
  value=128/255
  if space=='sRGB':value=((value+.055)/1.055)**2.4
  for x,scale,bias in ((4,(.2,.4,.8),(.05,.1,0)),(12,(.8,.2,.3),(0,.2,.1))):
   expected=tuple(encode(value*s+b)for s,b in zip(scale,bias))
   actual=tuple(pixels[(8*16+x)*3:(8*16+x)*3+3])
   assert all(abs(a-b)<=2 for a,b in zip(actual,expected)),(resolver,space,x,actual,expected)
print('PASS: emissive texture modifiers, color-space decoding and unlit display encoding')
PY
