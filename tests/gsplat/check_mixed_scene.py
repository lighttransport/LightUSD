#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate lusdrender --gsplat-kernel cycles in a mixed mesh + splat scene
against Blender 5.3 Cycles (direct light only), independent of how the mesh
BRDF is shaded:

  visibility V = render(black splats) / render(no splats)
      on plane pixels: sun-shadow transmittance x camera-ray transmittance
  emission   E = render(splats) - render(black splats)
      splat emission seen by the camera, plus the plane lit by it (Cycles
      finds emissive splats with its BSDF-sampled direct-light ray)

Each of V and E must be within 1.2x Blender's own seed-to-seed noise on that
quantity, and their means within 2%.

usage: check_mixed_scene.py LUSDRENDER [--spp N]   (LIGHTUSD_BLENDER=blender)
Exit 77 when Blender is unavailable.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 320, 180

CMP = r'''
import bpy, sys, numpy as np
a = sys.argv[sys.argv.index("--") + 1:]
def L(p):
    im = bpy.data.images.load(p); w, h = im.size
    return np.array(im.pixels[:], dtype=np.float64).reshape(h, w, 4)[:, :, :3]
d = a[0]
ref = {k: [L(os.path.join(d, k, "ref_seed%d.exr" % s)) for s in (0, 7)]
       for k in ("plain", "black", "full")}
ours = {k: L(os.path.join(d, k, "ours.exr")) for k in ("plain", "black", "full")}
mask = ref["plain"][0].mean(2) > 0.05          # lit plane pixels
def vis(b, p): return (b / np.maximum(p, 1e-6)).mean(2)[mask]
V0, V7 = vis(ref["black"][0], ref["plain"][0]), vis(ref["black"][7 if False else 1], ref["plain"][1])
Vo = vis(ours["black"], ours["plain"])
E0, E7 = ref["full"][0] - ref["black"][0], ref["full"][1] - ref["black"][1]
Eo = ours["full"] - ours["black"]
print("VIS floor %.5f ours %.5f mean ref %.4f ours %.4f" %
      (np.abs(V7 - V0).mean(), np.abs(Vo - V0).mean(), V0.mean(), Vo.mean()))
print("EMI floor %.5f ours %.5f mean ref %.5f ours %.5f" %
      (np.abs(E7 - E0).mean(), np.abs(Eo - E0).mean(), E0.mean(), Eo.mean()))
'''.replace("import bpy, sys", "import bpy, os, sys")


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout)
        raise SystemExit("command failed: %s" % " ".join(cmd))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lusdrender")
    ap.add_argument("--spp", type=int, default=256)
    a = ap.parse_args()
    blender = os.environ.get("LIGHTUSD_BLENDER", "")
    if not (blender and os.path.exists(blender)):
        print("SKIP: set LIGHTUSD_BLENDER to a Blender 5.3+ executable")
        return 77
    with tempfile.TemporaryDirectory(prefix="gsplat-mixed-") as d:
        for name, extra in (("plain", ["--no-splats"]), ("black", ["--black"]),
                            ("full", [])):
            sub = os.path.join(d, name)
            run([blender, "-b", "--factory-startup", "--python",
                 os.path.join(HERE, "blender_mixed_scene.py"), "--", sub,
                 "--width", str(W), "--height", str(H), "--spp", str(a.spp)] +
                extra, cwd=d)
            run([a.lusdrender, os.path.join(sub, "mixed.usda"),
                 os.path.join(sub, "ours.exr"), "--gsplat-kernel", "cycles",
                 "--gsplat-colorspace", "srgb", "-camera", "/World/Cam",
                 "-width", str(W), "-height", str(H), "-samples", str(a.spp)])
        script = os.path.join(d, "cmp.py")
        open(script, "w").write(CMP)
        log = run([blender, "-b", "--factory-startup", "--python", script, "--", d])
        ok = True
        for line in log.splitlines():
            if line.startswith(("VIS ", "EMI ")):
                f = line.split()
                floor, err, mref, mours = float(f[2]), float(f[4]), float(f[7]), float(f[9])
                good = err <= 1.2 * floor and abs(mours - mref) <= 0.02 * max(abs(mref), 1e-3)
                ok = ok and good
                print("%s  %s" % (line, "ok" if good else "FAIL"))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
