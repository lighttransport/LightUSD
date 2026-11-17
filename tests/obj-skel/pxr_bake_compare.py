#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""LightUSD blend shapes + skinning vs OpenUSD's UsdSkel.BakeSkinning.

  pxr_bake_compare.py FLAT.usda SKIN_DUMP TIME...

SKIN_DUMP prints LightUSD's deformed points per mesh (skin_dump.sh /
skin_compare --dump). OpenUSD bakes the same stage (blend shapes, then linear
blend skinning) into the meshes' points; both are compared per mesh at each
time. Exit 1 on a mismatch over 1e-4.
"""
import subprocess
import sys

from pxr import Gf, Usd, UsdGeom, UsdSkel


def main():
    src, dump, times = sys.argv[1], sys.argv[2], [float(t) for t in sys.argv[3:]]
    stage = Usd.Stage.Open(src)
    stage.SetEditTarget(stage.GetSessionLayer())
    if not UsdSkel.BakeSkinning(stage.Traverse(), Gf.Interval(min(times), max(times))):
        print("FAIL: UsdSkel.BakeSkinning")
        return 1
    fails = 0
    for t in times:
        r = subprocess.run([dump, src, str(t)], capture_output=True, text=True, check=True)
        native = {}
        for line in r.stdout.splitlines():
            f = line.split()
            if f and f[0].startswith("/"):
                native[f[0]] = list(map(float, f[2:]))
        if not native:
            print(f"FAIL t={t}: no deformed meshes from LightUSD")
            fails += 1
        for path, got in sorted(native.items()):
            mesh = UsdGeom.Mesh(stage.GetPrimAtPath(path))
            want = [c for p in mesh.GetPointsAttr().Get(t) for c in p]
            d = max((abs(a - b) for a, b in zip(got, want)), default=float("inf"))
            ok = len(got) == len(want) and d < 1e-4
            fails += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {path} t={t}: max abs {d:.2e}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
