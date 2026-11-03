#!/usr/bin/env bash
# A varying texture detects dropped UVs in both indexed and sequential chunks.
set -euo pipefail
BIN="${1:?lusdrender required}"
HELPERS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);(p/'colors.ppm').write_bytes(b'P6\n2 2\n255\n'+bytes([255,0,0,0,255,0,0,0,255,255,255,255]))
for mode in ('indexed','sequential'):
 points='[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]'
 indices='[0,1,2,0,2,3]';uv='[(0,0),(1,0),(1,1),(0,1)]'
 if mode=='sequential':
  points='[(-1,-1,0),(1,-1,0),(1,1,0),(-1,-1,0),(1,1,0),(-1,1,0)]';indices='[0,1,2,3,4,5]';uv='[(0,0),(1,0),(1,1),(0,0),(1,1),(0,1)]'
 (p/(mode+'.usda')).write_text('''#usda 1.0
 def Mesh "M" {
  uniform token subdivisionScheme = "none"
  point3f[] points = '''+points+'''
  int[] faceVertexCounts = [3,3]
  int[] faceVertexIndices = '''+indices+'''
  texCoord2f[] primvars:st = '''+uv+''' (interpolation = "vertex")
  rel material:binding = </Mat>
 }
 def Material "Mat" {
  token outputs:surface.connect = </Mat/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:emissiveColor.connect = </Mat/T.outputs:rgb>
   token outputs:surface
  }
  def Shader "T" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @colors.ppm@
   token inputs:sourceColorSpace = "raw"
   token inputs:wrapS = "clamp"
   token inputs:wrapT = "clamp"
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
PY
for MODE in indexed sequential; do
  "$BIN" "$TMP/$MODE.usda" "$TMP/$MODE-cpu.png" -rtPreview -diagnosticEmission -camera /C -w 64 -height 64
  env LUSDR_GPU_TRIANGLE_CHUNK=1 "$BIN" "$TMP/$MODE.usda" "$TMP/$MODE-gpu.png" -vkr -diagnosticEmission -camera /C -w 64 -height 64
  env LUSDR_GPU_TRIANGLE_CHUNK=8 "$BIN" "$TMP/$MODE.usda" "$TMP/$MODE-full.png" -vkr -diagnosticEmission -camera /C -w 64 -height 64
done
python3 - "$TMP" "$HELPERS" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[2])
from check_legacy_texture import read_png_rgb
root=Path(sys.argv[1])
for mode in ('indexed','sequential'):
 reference=read_png_rgb(root/(mode+'-cpu.png'));assert len(reference)==64*64
 values=[reference[y*64+x]for y in range(8,56)for x in range(8,56)]
 assert len(set(values))>16,'constant reference cannot validate UVs'
 for backend in ('gpu','full'):
  candidate=read_png_rgb(root/(mode+'-'+backend+'.png'));assert len(candidate)==64*64
  error=max(abs(a-b)for y in range(8,56)for x in range(8,56)for a,b in zip(reference[y*64+x],candidate[y*64+x]))
  assert error<=2,(mode,backend,error)
print('PASS: varying-texture UVs retained through indexed and sequential GPU chunks')
PY
