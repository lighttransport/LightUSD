#!/usr/bin/env python3
"""OBJ subdivision tags in LightUSD vs OpenSubdiv.

  run_osd_cross.py OBJ_SUBD_DUMP SRC_DIR WORKDIR

For each fixture OBJ and subdivision level 1..3, compares the refined points
LightUSD renders (obj_subd_dump: OBJ import -> UsdGeomMesh subdivision
attributes -> Tydra/tsd refinement) with OpenSubdiv's (osd_refine.cc, Far,
CPU backend) as point sets (nearest neighbour, both directions).

Needs OSD_ROOT (an OpenSubdiv install prefix with include/ and lib/libosdCPU);
without it the test is skipped (exit code 77).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# multi.obj is not cross-checked: its Loop mesh `b` is a degenerate "pillow"
# (one triangle twice, opposite winding). OpenSubdiv refines it; tsd reports a
# non-manifold edge from level 2. The clean Loop mesh in osd_cross.obj
# (tetra_loop, with a crease and a corner) matches OpenSubdiv exactly.
FIXTURES = ["osd_cross.obj", "torus.obj", "box.obj"]
TOL = 1e-4


def points(out):
    shapes, cur = {}, None
    for line in out.splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == "shape":
            # an unnamed OBJ shape is named "mesh" by the LightUSD importer
            cur = shapes.setdefault(t[1] if len(t) == 3 else "mesh", [])
        else:
            cur.append(tuple(map(float, t)))
    return shapes


def max_nearest(a, b):
    """max over p in a of the distance to the nearest point of b (grid hash)."""
    cell = 0.01
    grid = {}
    for q in b:
        grid.setdefault(tuple(int(c // cell) for c in q), []).append(q)
    worst = 0.0
    for p in a:
        k = tuple(int(c // cell) for c in p)
        best = float("inf")
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q in grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                        best = min(best, sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5)
        if best == float("inf"):
            best = min(sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5 for q in b)
        worst = max(worst, best)
    return worst


def main():
    dump, src, work = sys.argv[1:4]
    root = os.environ.get("OSD_ROOT")
    if not root:
        print("SKIP: set OSD_ROOT to an OpenSubdiv install to run the cross-check")
        return 77
    os.makedirs(work, exist_ok=True)
    ref = os.path.join(work, "osd_refine")
    tobj = os.path.join(src, "external", "tinyobjloader-c")
    objs = []
    for c in ("tiny_obj_c.c", "tobj_tess.c", "tobj_subd.c"):
        o = os.path.join(work, c + ".o")
        subprocess.run(["cc", "-std=c11", "-O1", "-c", os.path.join(tobj, c), "-o", o], check=True)
        objs.append(o)
    subprocess.run(["c++", "-std=c++17", "-O1", f"-I{root}/include", f"-I{src}",
                    os.path.join(HERE, "osd_refine.cc")] + objs +
                   [f"-L{root}/lib", "-losdCPU", f"-Wl,-rpath,{root}/lib", "-lm", "-o", ref],
                   check=True)
    fails = 0
    for fx in FIXTURES:
        obj = os.path.join(HERE, fx)
        scene = os.path.join(work, fx + ".usda")
        with open(scene, "w") as f:
            f.write(f'#usda 1.0\ndef Xform "W" (\n    references = @{obj}@\n)\n{{\n}}\n')
        for level in (1, 2, 3):
            want = points(subprocess.run([ref, obj, str(level)], capture_output=True, text=True,
                                         check=True).stdout)
            got = points(subprocess.run([dump, scene, str(level)], capture_output=True, text=True,
                                        check=True).stdout)
            for name, pts in want.items():
                have = got.get(name)
                if have is None:
                    print(f"FAIL {fx} level {level} {name}: not subdivided by LightUSD")
                    fails += 1
                    continue
                d = max(max_nearest(have, pts), max_nearest(pts, have))
                ok = len(have) == len(pts) and d < TOL
                print(f"{'ok  ' if ok else 'FAIL'} {fx} level {level} {name}: "
                      f"{len(have)}/{len(pts)} points, max distance {d:.1e}")
                fails += not ok
            for name in got:
                if name not in want:
                    print(f"FAIL {fx} level {level} {name}: subdivided by LightUSD only")
                    fails += 1
    print("PASS" if not fails else f"FAIL: {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
