#!/usr/bin/env bash
# Independent nearest-hit check across prototype batches and global instance IDs.
set -euo pipefail
BIN="${1:?lusdrender binary required}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/scene.usda" <<'USD'
#usda 1.0
(upAxis = "Y")
def PointInstancer "I" {
 rel prototypes = [</I/A>, </I/B>]
 int[] protoIndices = [0,1]
 point3f[] positions = [(0,0,0),(0,0,1)]
 def Mesh "A" {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-2,-2,0),(2,-2,0),(0,2,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  normal3f[] normals = [(1,0,1)] (interpolation = "constant")
 }
 def Mesh "B" {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-2,-2,0),(2,-2,0),(0,2,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  normal3f[] normals = [(0,1,1)] (interpolation = "constant")
 }
}
def Camera "C" {
 double3 xformOp:translate = (0,0,5)
 uniform token[] xformOpOrder = ["xformOp:translate"]
 float focalLength = 50
 float horizontalAperture = 20
 float verticalAperture = 20
 float2 clippingRange = (0.1,100)
}
USD
for CHANNEL in depth geometricNormal shadingNormal; do
 if ! "$BIN" "$TMP/scene.usda" "$TMP/resident-$CHANNEL.pfm" -vkInstanced --aov "$CHANNEL" -camera /C -w 32 -height 32 > "$TMP/resident-$CHANNEL.log" 2>&1; then
  if grep -qiE 'ray.query unavailable|Failed to create.*Vulkan|no Vulkan' "$TMP/resident-$CHANNEL.log";then echo 'SKIP: hardware ray query unavailable';exit 77;fi
  cat "$TMP/resident-$CHANNEL.log";exit 1
 fi
 "$BIN" -gpuTriangleChunk 1 "$TMP/scene.usda" "$TMP/flat-chunks-$CHANNEL.pfm" -vkr --aov "$CHANNEL" -camera /C -w 32 -height 32 -stats > "$TMP/flat-chunks-$CHANNEL.log" 2>&1 || { cat "$TMP/flat-chunks-$CHANNEL.log";exit 1; }
 grep -q '2 GPU chunks' "$TMP/flat-chunks-$CHANNEL.log"
 python3 - "$TMP/resident-$CHANNEL.pfm" "$TMP/flat-chunks-$CHANNEL.pfm" <<'COMPARE'
import math,struct,sys
def read(path):
 with open(path,'rb')as f:
  assert f.readline().strip()==b'PF';assert f.readline().strip()==b'32 32';assert float(f.readline())<0
  return struct.unpack('<3072f',f.read())
a,b=map(read,sys.argv[1:])
assert all(math.isfinite(x)and math.isfinite(y)and abs(x-y)<=1e-6 for x,y in zip(a,b)),max(abs(x-y)for x,y in zip(a,b))
COMPARE
 for REPEAT in 0 1 2; do
  LRT_VK_TLAS_SLICE=1 "$BIN" -instBatchTris 1 "$TMP/scene.usda" "$TMP/batch-$CHANNEL-$REPEAT.pfm" -vkInstancedStream --aov "$CHANNEL" -camera /C -w 32 -height 32 -stats > "$TMP/batch-$CHANNEL-$REPEAT.log" 2>&1 || { cat "$TMP/batch-$CHANNEL-$REPEAT.log";exit 1; }
  grep -q '2 BLAS batches' "$TMP/batch-$CHANNEL-$REPEAT.log"
  cmp "$TMP/resident-$CHANNEL.pfm" "$TMP/batch-$CHANNEL-$REPEAT.pfm"
 done
done
python3 - "$TMP" <<'PY'
import math,struct,sys
from pathlib import Path
root=Path(sys.argv[1])
for channel,want in {'depth':(4.000156,)*3,'geometricNormal':(0,0,1),'shadingNormal':(0,1/math.sqrt(2),1/math.sqrt(2))}.items():
 with (root/f'resident-{channel}.pfm').open('rb')as f:
  f.readline();f.readline();f.readline();a=struct.unpack('<3072f',f.read())
 i=(16*32+16)*3
 assert all(abs(x-y)<1e-5 for x,y in zip(a[i:i+3],want)),(channel,a[i:i+3],want)
for p in root.glob('*.log'):assert 'Validation Error' not in p.read_text(),p
print('PASS: nearest global hit across streamed BLAS/TLAS batches, repeated captures')
PY
