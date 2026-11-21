#!/usr/bin/env bash
# Independent analytic check of geometric/interpolated normal channel semantics.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:?lusdrender binary required}"
command -v python3 >/dev/null || exit 77
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for CHANNEL in geometricNormal shadingNormal; do
  "$LUSDRENDER" "$REPO_ROOT/tests/usda/view-control-normal-calibration.usda" \
    "$TMP/$CHANNEL.pfm" --aov "$CHANNEL" -w 32 -height 32 -camera /C -threads 1 \
    > "$TMP/$CHANNEL.log" 2>&1 || { cat "$TMP/$CHANNEL.log"; exit 1; }
done
python3 - "$TMP" <<'PY'
from pathlib import Path
import math
import struct
import sys
root=Path(sys.argv[1])
expected={'geometricNormal':(0,0,1),'shadingNormal':(1/math.sqrt(5),0,2/math.sqrt(5))}
for channel,normal in expected.items():
    with (root/(channel+'.pfm')).open('rb') as f:
        assert f.readline().strip()==b'PF'
        assert f.readline().strip()==b'32 32'
        assert f.readline().strip()==b'-1.0'
        samples=struct.unpack('<'+str(32*32*3)+'f',f.read())
    # Interior pixels all hit the constant-normal plane; test several rays.
    for x,y in ((16,16),(12,12),(20,18)):
        actual=samples[((31-y)*32+x)*3:((31-y)*32+x)*3+3]
        assert all(abs(a-b)<1e-5 for a,b in zip(actual,normal)),(channel,x,y,actual,normal)
print('PASS: distinct raw normal channels and inverse-transpose instancing')
PY
