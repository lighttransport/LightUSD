#!/usr/bin/env bash
# Nested curve placements share one tessellated Bezier prototype.
set -euo pipefail
BIN="${1:?lusdrender binary required}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$ROOT/tests/usda/view-control-instanced-curves.usda" "$TMP/ribbon.usda" <<'PYCODE'
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text().replace('float[] widths = [0.2] (interpolation = "constant")','float[] widths = [0, 0, .2, .2] (interpolation = "vertex")\n    normal3f[] normals = [(0,0,1)] (interpolation = "constant")')
Path(sys.argv[2]).write_text(s)
PYCODE
for KIND in round ribbon; do
 SOURCE="$ROOT/tests/usda/view-control-instanced-curves.usda"
 if [ "$KIND" = ribbon ]; then SOURCE="$TMP/ribbon.usda"; fi
for MODE in vkInstanced vkInstancedStream; do
 if ! "$BIN" "$SOURCE" "$TMP/$KIND-$MODE.pfm" "-$MODE" --aov depth -camera /C -w 128 -height 128 -stats > "$TMP/$KIND-$MODE.log" 2>&1; then
  if grep -qiE 'ray.query unavailable|Failed to create.*Vulkan|no Vulkan' "$TMP/$KIND-$MODE.log";then echo 'SKIP: hardware ray query unavailable';exit 77;fi
  cat "$TMP/$KIND-$MODE.log";exit 1
 fi
 grep -q '1 prototypes .* x 2 instances' "$TMP/$KIND-$MODE.log" || { cat "$TMP/$KIND-$MODE.log";exit 1; }
done
cmp "$TMP/$KIND-vkInstanced.pfm" "$TMP/$KIND-vkInstancedStream.pfm"
done
python3 - "$TMP/round-vkInstanced.pfm" "$TMP/ribbon-vkInstanced.pfm" <<'PY'
import struct,sys
with open(sys.argv[2],'rb') as f:
 f.readline();f.readline();f.readline();ribbon=struct.unpack('<49152f',f.read())
assert sum(v>0 for v in ribbon[::3])>20
with open(sys.argv[1],'rb')as f:
 assert f.readline().strip()==b'PF'
 assert f.readline().strip()==b'128 128'
 assert float(f.readline())<0
 pixels=struct.unpack('<49152f',f.read())
hits=[(i%128,i//128)for i,v in enumerate(pixels[::3])if v>0]
assert len(hits)>100,len(hits)
# Image is stored bottom-up. The cubic arch peaks at y=1.5, not at the
# y=2 control polygon. Camera center is y=.75, image half-height is 3.2.
assert 77<=max(y for x,y in hits)<=81,max(y for x,y in hits)
assert any(x<64 for x,y in hits)and any(x>64 for x,y in hits)
print('PASS: nested curve instancing, cubic evaluation and streamed capture')
PY
