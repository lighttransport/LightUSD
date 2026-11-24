#!/usr/bin/env bash
# Read native resolved scalar fields; do not rewrite the material to emission.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/gpu-test-common.sh"
BIN="${1:?lusdrender required}"
BACKEND="${2:-cpu}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP" <<'PY'
from pathlib import Path
import struct,sys,zlib
root=Path(sys.argv[1])
def chunk(name,data):return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data)&0xffffffff)
root.joinpath('data.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes([64,17,83,192,192,17,83,64])))+chunk(b'IEND',b''))
text='#usda 1.0\n'
for name,x0,x1,u in [('Left',-2,0,.25),('Right',0,2,.75)]:
 text+=f'''def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {{
  uniform token subdivisionScheme = "none"
  point3f[] points = [({x0},-2,0),({x1},-2,0),({x1},2,0),({x0},2,0)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  texCoord2f[] primvars:UV1 = [({u},0.5)] (interpolation = "constant")
  rel material:binding = </M>
 }}\n'''
text+='''def Material "M" {
 token outputs:surface.connect = </M/S.outputs:surface>
 def Shader "S" { uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor = (0.4,0.2,0.1)
  color3f inputs:emissiveColor = (0,1,0)
  float inputs:roughness.connect = </M/T.outputs:r>
  float inputs:metallic.connect = </M/T.outputs:a>
  token outputs:surface
 }
 def Shader "T" { uniform token info:id = "UsdUVTexture"
  float2 inputs:st.connect = </M/UV.outputs:result>
  asset inputs:file = @data.png@
  token inputs:sourceColorSpace = "raw"
  float4 inputs:scale = (0.5,1,1,0.5)
  float4 inputs:bias = (0.1,0,0,0.1)
  float outputs:r
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
for space in ('raw','sRGB','negative'):
 scene=text.replace('sourceColorSpace = "raw"','sourceColorSpace = "'+('raw' if space=='negative' else space)+'"')
 if space=='negative':scene=scene.replace('scale = (0.5,1,1,0.5)','scale = (1,1,1,1)').replace('bias = (0.1,0,0,0.1)','bias = (-0.4,0,0,-0.4)')
 root.joinpath(space+'.usda').write_text(scene)
PY
ARGS=(-rtPreview); if [ "$BACKEND" = vkr ]; then ARGS=(-vkr); fi
for RESOLVER in legacy tydra; do
 for SPACE in raw sRGB negative; do
 for CHANNEL in roughness metallic; do
  "$BIN" "$TMP/$SPACE.usda" "$TMP/$RESOLVER-$SPACE-$CHANNEL.png" "${ARGS[@]}" \
    -diagnosticMaterial "$CHANNEL" -camera /C -w 16 -height 16 -threads 1 \
    -materialResolver "$RESOLVER" > "$TMP/$RESOLVER-$SPACE-$CHANNEL.log" 2>&1 \
    || lusdrender_test_render_failed "$BACKEND" "$TMP/$RESOLVER-$SPACE-$CHANNEL.log" "$?"
 done
 done
done
if grep -Eq 'Validation Error|VUID-' "$TMP"/*.log; then cat "$TMP"/*.log; exit 1; fi
python3 - "$TMP" "$ROOT" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,str(Path(sys.argv[2])/'examples/lusdview/tests'))
from asset_fingerprint import _read_image
root=Path(sys.argv[1])
def encode(x):return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
for resolver in ('legacy','tydra'):
 for space in ('raw','sRGB','negative'):
  for channel in ('roughness','metallic'):
   w,h,pixels=_read_image(str(root/f'{resolver}-{space}-{channel}.png'));assert (w,h)==(16,16)
   for x,raw in [(4,64),(12,192)]:
    if channel=='metallic':raw=256-raw
    value=raw/255
    if space=='sRGB' and channel!='metallic':value=value/12.92 if value<=.04045 else ((value+.055)/1.055)**2.4
    expected=encode(max(0,min(1,value-.4)) if space=='negative' else value*.5+.1)
    actual=tuple(pixels[(8*w+x)*3:(8*w+x)*3+3])
    assert max(abs(a-expected)for a in actual)<=3,(resolver,space,channel,x,actual,expected)
print('PASS: native roughness/metallic fields retain distinct channels, scale/bias and shared-material UV1')
PY
