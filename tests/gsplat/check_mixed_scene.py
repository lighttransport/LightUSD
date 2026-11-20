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

usage: check_mixed_scene.py LUSDRENDER [--spp N] [--motion|--camera-motion]
                            [--refs | --write-refs]   (LIGHTUSD_BLENDER=blender)
--refs: no Blender. Renders the checked-in scenes
        tests/usda/gsplat-mixed[-VARIANT]-{plain,black,full}.usda and compares
        them with Blender's seed-0 renders stored in
        tests/gsplat/refs/mixed[-VARIANT]/ (float16 npz) and the noise floors
        in its meta.json. --write-refs regenerates those from Blender (and
        checks that Blender's exported scenes still equal the fixtures).
Exit 77 when Blender (or numpy for --refs) is unavailable.
"""
import argparse
import os
import re
import subprocess
import sys
import json
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
KINDS = ("plain", "black", "full")


def metrics_np(ref0, ref7, ours):
    """VIS/EMI (floor, err, mean_ref, mean_ours) as the Blender CMP script."""
    import numpy as np
    mask = ref0["plain"].mean(2) > 0.05

    def vis(b, p):
        return (b / np.maximum(p, 1e-6)).mean(2)[mask]
    V0, Vo = vis(ref0["black"], ref0["plain"]), vis(ours["black"], ours["plain"])
    E0 = ref0["full"] - ref0["black"]
    Eo = ours["full"] - ours["black"]
    out = {"VIS": [None, float(np.abs(Vo - V0).mean()), float(V0.mean()),
                   float(Vo.mean())],
           "EMI": [None, float(np.abs(Eo - E0).mean()), float(E0.mean()),
                   float(Eo.mean())]}
    if ref7 is not None:
        out["VIS"][0] = float(np.abs(vis(ref7["black"], ref7["plain"]) - V0).mean())
        out["EMI"][0] = float(np.abs((ref7["full"] - ref7["black"]) - E0).mean())
    return out


def judge(name, floor, err, mref, mours):
    good = err <= 1.2 * floor and abs(mours - mref) <= 0.02 * max(abs(mref), 1e-3)
    print("%s floor %.5f ours %.5f mean ref %.5f ours %.5f  %s" %
          (name, floor, err, mref, mours, "ok" if good else "FAIL"))
    return good
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
    ap.add_argument("--backend", choices=("cpu", "vk", "hip"), default="cpu")
    ap.add_argument("--spp", type=int, default=256)
    ap.add_argument("--motion", action="store_true",
                    help="animated splats + camera shutter (motion blur)")
    ap.add_argument("--camera-motion", action="store_true",
                    help="animated camera (camera motion blur)")
    ap.add_argument("--refs", action="store_true",
                    help="compare against checked-in references (no Blender)")
    ap.add_argument("--write-refs", action="store_true",
                    help="regenerate the checked-in references with Blender")
    a = ap.parse_args()
    blur = a.motion or a.camera_motion
    bflags = (["--motion"] if a.motion else []) + \
             (["--camera-motion"] if a.camera_motion else [])
    variant = "motion" if a.motion else ("cammotion" if a.camera_motion else "")
    refdir = os.path.join(HERE, "refs", "mixed" + ("-" + variant if variant else ""))
    fixture = lambda k: os.path.join(
        ROOT, "tests/usda/gsplat-mixed-%s%s.usda" % (variant + "-" if variant else "", k))
    def lus(usda, out):
        flags = [] if a.backend == "cpu" else [
            "-vk" if a.backend == "vk" else "-hip", "--path-trace",
            "--pt-samples", str(a.spp), "--pt-max-depth", "2"]
        cmd = [a.lusdrender, usda, out, "--gsplat-kernel", "cycles",
               "--gsplat-colorspace", "srgb", "-camera", "/World/Cam",
               "-width", str(W), "-height", str(H), "-samples", str(a.spp)]
        if blur:
            cmd += ["-timecode", "1"]
        if a.backend == "cpu":
            return run(cmd)
        result = subprocess.run(cmd + flags, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        log = result.stdout
        if result.returncode:
            unavailable = ("Failed to create LightRT Vulkan engine", "HIP runtime not available",
                           "no HIP device", "hipErrorNoDevice", "backend not built in")
            caps = next((line for line in log.splitlines() if line.startswith("Vulkan caps:")), "")
            if any(reason in log for reason in unavailable) or (caps and "ray_query" not in caps):
                print(f"SKIP: {a.backend} device unavailable\n{log[-1500:]}")
                raise SystemExit(77)
            raise SystemExit(log)
        marker = "Vulkan Cycles mixed path:" if a.backend == "vk" else "backend: shared HIP RT"
        if marker not in log:
            raise SystemExit(f"missing {marker} in render log:\n{log}")
        if not usda.endswith("-plain.usda"):
            kernel = ("Vulkan Cycles mixed path:" if a.backend == "vk"
                      else "HIP Cycles gsplat kernel:")
            if not re.search(kernel + r" [1-9][0-9]* splat", log):
                raise SystemExit(f"missing positive splat count in render log:\n{log}")
        return log
    if a.refs:
        try:
            import numpy as np
        except ImportError:
            print("SKIP: numpy not available")
            return 77
        sys.path.insert(0, HERE)
        import exr_np
        if not os.path.exists(os.path.join(refdir, "meta.json")):
            print("SKIP: no references in %s" % refdir)
            return 77
        meta = json.load(open(os.path.join(refdir, "meta.json")))
        a.spp = meta["spp"]
        ref0 = {k: np.load(os.path.join(refdir, "ref.npz"))[k].astype(np.float64)
                for k in KINDS}
        ours = {}
        with tempfile.TemporaryDirectory(prefix="gsplat-mixed-ref-") as d:
            for k in KINDS:
                out = os.path.join(d, k + ".exr")
                lus(fixture(k), out)
                ours[k] = exr_np.read_rgb(out)
        m = metrics_np(ref0, None, ours)
        ok = True
        for q in ("VIS", "EMI"):
            ok = judge(q, meta[q]["floor"], m[q][1], m[q][2], m[q][3]) and ok
        return 0 if ok else 1
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
                extra + bflags, cwd=d)
            lus(os.path.join(sub, "mixed.usda"), os.path.join(sub, "ours.exr"))
            if a.write_refs and open(os.path.join(sub, "mixed.usda")).read() != \
                    open(fixture(name)).read():
                raise SystemExit("Blender's %s scene differs from %s: update the "
                                 "fixture first" % (name, fixture(name)))
        if a.write_refs:
            import numpy as np
            sys.path.insert(0, HERE)
            import exr_np
            r0 = {k: exr_np.read_rgb(os.path.join(d, k, "ref_seed0.exr")) for k in KINDS}
            r7 = {k: exr_np.read_rgb(os.path.join(d, k, "ref_seed7.exr")) for k in KINDS}
            ours = {k: exr_np.read_rgb(os.path.join(d, k, "ours.exr")) for k in KINDS}
            m = metrics_np(r0, r7, ours)
            os.makedirs(refdir, exist_ok=True)
            np.savez_compressed(os.path.join(refdir, "ref.npz"),
                                **{k: r0[k].astype(np.float16) for k in KINDS})
            meta = {q: {"floor": m[q][0], "mean": m[q][2]} for q in ("VIS", "EMI")}
            meta.update({"spp": a.spp, "width": W, "height": H, "blender": "5.3",
                         "note": "regenerate: check_mixed_scene.py LUSDRENDER "
                                 "[--motion|--camera-motion] --write-refs"})
            json.dump(meta, open(os.path.join(refdir, "meta.json"), "w"), indent=1)
            ok = True
            for q in ("VIS", "EMI"):
                ok = judge(q, *m[q]) and ok
            print("wrote %s" % refdir)
            return 0 if ok else 1
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
