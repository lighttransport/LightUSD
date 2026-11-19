#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate lusdrender --gsplat-kernel cycles.

smoke:   render the generated splat fixture twice; require a nonblank,
         deterministic image (no Blender needed).
compare: render the same splats with Blender 5.3 Cycles (LIGHTUSD_BLENDER =
         path to the blender executable) and require our error vs Blender to
         be within 1.25x Blender's own seed-to-seed noise floor (8-bit PNG).
roundtrip: create splats natively in Blender (blender_make_and_export.py),
         export them as OpenUSD ParticleField USDA and as a portable 3DGS PLY,
         then compare scene-linear EXR renders: lusdrender on the USDA, on a
         USD scene referencing the PLY (usdPly reader), and Blender on the
         re-imported PLY, all against Blender's native render. Each must be
         within 1.1x the noise floor in MAE and within 1% in mean brightness.
variants: like roundtrip (USDA only) for SH degrees 0/1/2, a non-uniform
         object scale, an orthographic camera, a coloured world background
         (lusdrender -bg) and motion blur (animated object, camera shutter);
         each within 1.1x the noise floor and 1% mean.
refs:    the variants against references checked in under tests/gsplat/refs/
         (no Blender needed): each case holds the Blender-exported scene
         (scene.usdc), Blender's seed-0 render (ref.npz, float16) and the
         seed-to-seed noise floor (meta.json). Same 1.1x floor / 1% mean criteria.
         Regenerate with: variants --write-refs (needs Blender and --lusdcat).
Exit 77 (skip) when Blender or Pillow (numpy for refs) is unavailable.

--backend vk|hip traces through the LightRT Vulkan / HIP splat kernel (skips
with 77 when that GPU backend is unavailable).

usage: check_cycles_compare.py LUSDRENDER smoke|compare|roundtrip [--spp N]
                               [--backend cpu|vk|hip]
"""
import argparse
import math
import os
import subprocess
import sys
import json
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REFS = os.path.join(HERE, "refs")


def compare_np(ours, ref, floor, mean, tol):
    """(good, mae, ratio) of a render against a stored full-res ref.
    (Box-filtering both would hide noise but magnify small biases.)"""
    import numpy as np
    mae = float(np.abs(ours - ref).mean())
    ratio = ours.mean((0, 1)) / np.maximum(np.asarray(mean), 1e-9)
    good = mae <= tol * floor and all(abs(r - 1) <= 0.01 for r in ratio)
    return good, mae, ratio
W, H = 320, 180


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout)
        raise SystemExit("command failed: %s" % " ".join(cmd))
    return r.stdout


def metrics(a_path, b_path):
    from PIL import Image
    a = Image.open(a_path).convert("RGB").tobytes()
    b = Image.open(b_path).convert("RGB").tobytes()
    n = len(a)
    mae = sum(abs(x - y) for x, y in zip(a, b)) / n
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / n
    return mae, 10 * math.log10(65025.0 / max(mse, 1e-9))


BACKEND = []


def ours(lusdrender, usda, out, spp, colorspace):
    run([lusdrender, usda, out] + BACKEND + ["--gsplat-kernel", "cycles",
         "--gsplat-colorspace", colorspace, "-camera", "/World/Cam",
         "-width", str(W), "-height", str(H), "-samples", str(spp)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lusdrender")
    ap.add_argument("mode", choices=["smoke", "compare", "roundtrip", "variants",
                                     "refs"])
    ap.add_argument("--write-refs", action="store_true",
                    help="variants: (re)write tests/gsplat/refs from Blender")
    ap.add_argument("--lusdcat", default="",
                    help="--write-refs: lusdcat used to store scenes as usdc")
    ap.add_argument("--spp", type=int, default=256)
    ap.add_argument("--backend", choices=["cpu", "vk", "hip"], default="cpu")
    ap.add_argument("--lusdview", default="",
                    help="variants: also check this lusdview binary's path tracer")
    ap.add_argument("--lusdview-backend", choices=["vk", "hip", "cuda"], default="vk")
    a = ap.parse_args()
    if a.backend != "cpu":
        BACKEND.append("-" + a.backend)
        # Probe: the backend must actually trace on the GPU.
        with tempfile.TemporaryDirectory(prefix="gsplat-probe-") as d:
            run([sys.executable, "-I", os.path.join(HERE, "make_cycles_fixture.py"),
                 d, "--count", "8", "--seed", "1", "--degree", "0"])
            log = run([a.lusdrender, os.path.join(d, "splats.usda"),
                       os.path.join(d, "p.png"), "-" + a.backend,
                       "--gsplat-kernel", "cycles", "-width", "16",
                       "-height", "16", "-samples", "2", "-stats"])
            if "LightRT, CPU" in log:
                print("SKIP: %s backend unavailable" % a.backend)
                return 77
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("SKIP: Pillow not available")
        return 77
    if a.mode == "refs":
        try:
            import numpy  # noqa: F401
        except ImportError:
            print("SKIP: numpy not available")
            return 77
        return run_refs(a.lusdrender, a.lusdview, a.lusdview_backend)
    blender = os.environ.get("LIGHTUSD_BLENDER", "")
    if a.mode != "smoke" and not (blender and os.path.exists(blender)):
        print("SKIP: set LIGHTUSD_BLENDER to a Blender 5.3+ executable")
        return 77
    if a.mode == "roundtrip":
        return roundtrip(a.lusdrender, blender, a.spp)
    if a.mode == "variants":
        if a.write_refs and not a.lusdcat:
            raise SystemExit("--write-refs needs --lusdcat")
        return variants(a.lusdrender, blender, a.spp, a.lusdview,
                        a.lusdview_backend, a.lusdcat if a.write_refs else "")
    with tempfile.TemporaryDirectory(prefix="gsplat-cycles-") as d:
        run([sys.executable, "-I", os.path.join(HERE, "make_cycles_fixture.py"),
             d, "--count", "120", "--seed", "5", "--degree", "3"])
        usda = os.path.join(d, "splats.usda")
        if a.mode == "smoke":
            p0, p1 = os.path.join(d, "a.png"), os.path.join(d, "b.png")
            ours(a.lusdrender, usda, p0, 16, "linear")
            ours(a.lusdrender, usda, p1, 16, "linear")
            from PIL import Image
            img = Image.open(p0).convert("RGB")
            lit = sum(1 for px in img.getdata() if sum(px) > 30)
            frac = lit / float(W * H)
            mae, _ = metrics(p0, p1)
            print("lit fraction %.3f, rerun MAE %.4f" % (frac, mae))
            if frac < 0.3:
                print("FAIL: image mostly blank")
                return 1
            if mae != 0.0:
                print("FAIL: render is not deterministic")
                return 1
            return 0
        script = os.path.join(HERE, "blender_render_splats.py")
        ply = os.path.join(d, "splats.ply")
        refs = []
        for seed in (0, 7):
            src = open(script).read().replace("scene.cycles.seed = 0",
                                              "scene.cycles.seed = %d" % seed)
            sp = os.path.join(d, "render_seed%d.py" % seed)
            open(sp, "w").write(src)
            out = os.path.join(d, "blender%d.png" % seed)
            run([blender, "-b", "--factory-startup", "--python", sp, "--",
                 ply, out, str(W), str(H), str(a.spp)], cwd=d)
            refs.append(out)
        mine = os.path.join(d, "ours.png")
        ours(a.lusdrender, usda, mine, a.spp, "srgb")
        floor_mae, floor_psnr = metrics(refs[0], refs[1])
        mae, psnr = metrics(mine, refs[0])
        print("Blender noise floor: MAE %.3f PSNR %.2f dB" % (floor_mae, floor_psnr))
        print("ours vs Blender:     MAE %.3f PSNR %.2f dB" % (mae, psnr))
        if mae > 1.25 * floor_mae:
            print("FAIL: error exceeds 1.25x the Blender noise floor")
            return 1
        return 0


def roundtrip(lusdrender, blender, spp):
    with tempfile.TemporaryDirectory(prefix="gsplat-roundtrip-") as d:
        run([blender, "-b", "--factory-startup", "--python",
             os.path.join(HERE, "blender_make_and_export.py"), "--", d,
             "--width", str(W), "--height", str(H), "--spp", str(spp), "--exr"],
            cwd=d)
        cands = [os.path.join(d, "ref_from_ply.exr")]
        for name in ("splats", "scene_ply"):
            out = os.path.join(d, "ours_%s.exr" % name)
            run([lusdrender, os.path.join(d, name + ".usda"), out] + BACKEND +
                ["--gsplat-kernel", "cycles", "--gsplat-colorspace", "srgb",
                 "-camera", "/World/Cam", "-width", str(W), "-height", str(H),
                 "-samples", str(spp)])
            cands.append(out)
        # The bare .ply must also open directly (no camera: auto-framed).
        run([lusdrender, os.path.join(d, "splats.ply"),
             os.path.join(d, "bare_ply.png"), "--gsplat-kernel", "cycles",
             "-width", "64", "-height", "36", "-samples", "4"])
        log = run([blender, "-b", "--factory-startup", "--python",
                   os.path.join(HERE, "blender_compare_exr.py"), "--",
                   os.path.join(d, "ref_seed0.exr"),
                   os.path.join(d, "ref_seed7.exr")] + cands)
        rows = {}
        for line in log.splitlines():
            if line.startswith("CMP "):
                f = line.split()
                rows[os.path.basename(f[1])] = (float(f[2]),
                                                [float(x) for x in f[3:6]])
        floor = rows["floor"][0]
        ok = True
        for name in ("ref_from_ply.exr", "ours_splats.exr", "ours_scene_ply.exr"):
            mae, ratio = rows[name]
            good = mae <= 1.1 * floor and all(abs(r - 1) <= 0.01 for r in ratio)
            ok = ok and good
            print("%-20s MAE %.5f (floor %.5f, %.2fx)  mean ratio %s  %s" %
                  (name, mae, floor, mae / floor, ["%.4f" % r for r in ratio],
                   "ok" if good else "FAIL"))
        return 0 if ok else 1



VARIANTS = [
    ("degree0", ["--degree", "0"], []),
    ("degree1", ["--degree", "1"], []),
    ("degree2", ["--degree", "2"], []),
    ("nonuniform", ["--scale", "1.6,0.7,1.1"], []),
    ("ortho", ["--ortho"], []),
    ("world", ["--world", "0.20,0.35,0.60"], ["-bg", "0.20,0.35,0.60"]),
    # Animated object, Cycles motion blur (shutter 0.5, centred on frame 1).
    ("motion", ["--motion"], ["-timecode", "1"]),
    # Animated camera; animated DC colour + opacity (Geometry Nodes).
    ("cammotion", ["--camera-motion"], ["-timecode", "1"]),
    ("colourmotion", ["--colour-motion"], ["-timecode", "1"]),
]


def lusdview_render(lusdview, backend, usda, out, spp, time):
    """lusdview path tracer (direct light, no denoise) at Blender's frame."""
    cmd = [lusdview, "--headless", "--no-grid", "--background", "black",
           "--size", "%dx%d" % (W, H), "--path-trace"]
    if backend != "vk":
        cmd += ["--" + backend]
    cmd += ["--pt-samples", str(spp), "--pt-max-depth", "2", "--pt-denoise", "off",
            "--pt-firefly-clamp", "0", "--gsplat-kernel", "cycles",
            "--gsplat-colorspace", "srgb", "--camera", "/World/Cam",
            "--screenshot", out + ".ppm", "--linear-output", out]
    if time is not None:
        cmd += ["--time", time]
    env = dict(os.environ, LUSDVIEW_RT_ALLOW_COLD_COMPILE="1")
    r = subprocess.run(cmd + [usda], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, env=env)
    log = r.stdout
    marker = {"vk": "Vulkan RT: Cycles gsplat", "hip": "HIP Cycles gsplat",
              "cuda": "CUDA Cycles gsplat"}[backend]
    if r.returncode != 0 or marker not in log or not os.path.exists(out):
        return False
    return True


def write_ref(name, d, largs, spp, lusdcat):
    """Store one variant's Blender scene + seed-0 reference."""
    import numpy as np
    import exr_np
    r0 = exr_np.read_rgb(os.path.join(d, "ref_seed0.exr"))
    r7 = exr_np.read_rgb(os.path.join(d, "ref_seed7.exr"))
    out = os.path.join(REFS, name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    run([lusdcat, "--rewrite-layer", "-o", os.path.join(out, "scene.usdc"),
         os.path.join(d, "splats.usda")])
    np.savez_compressed(os.path.join(out, "ref.npz"), ref=r0.astype(np.float16))
    meta = {"floor": float(np.abs(r7 - r0).mean()),
            "mean": [float(x) for x in r0.mean((0, 1))],
            "lusdrender_args": largs, "spp": spp, "width": W, "height": H,
            "blender": "5.3", "note": "regenerate: check_cycles_compare.py "
                                      "LUSDRENDER variants --write-refs"}
    json.dump(meta, open(os.path.join(out, "meta.json"), "w"), indent=1)
    print("wrote refs/%s (floor %.5f)" % (name, meta["floor"]))


def run_refs(lusdrender, lusdview="", lv_backend="vk"):
    """Variants against the checked-in references; no Blender needed."""
    import numpy as np
    sys.path.insert(0, HERE)
    import exr_np
    # Variant cases carry lusdrender_args; refs/mixed* belong to
    # check_mixed_scene.py.
    cases = sorted(c for c in os.listdir(REFS)
                   if os.path.exists(os.path.join(REFS, c, "meta.json")) and
                   "lusdrender_args" in json.load(open(os.path.join(REFS, c,
                                                                    "meta.json"))))
    if not cases:
        print("SKIP: no references under %s" % REFS)
        return 77
    ok = True
    for name in cases:
        c = os.path.join(REFS, name)
        meta = json.load(open(os.path.join(c, "meta.json")))
        ref = np.load(os.path.join(c, "ref.npz"))["ref"].astype(np.float64)
        scene = os.path.join(c, "scene.usdc")
        largs = meta["lusdrender_args"]
        spp, w, h = meta["spp"], meta["width"], meta["height"]
        with tempfile.TemporaryDirectory(prefix="gsplat-ref-") as d:
            out = os.path.join(d, "ours.exr")
            run([lusdrender, scene, out] + BACKEND +
                ["--gsplat-kernel", "cycles", "--gsplat-colorspace", "srgb",
                 "-camera", "/World/Cam", "-width", str(w), "-height", str(h),
                 "-samples", str(spp)] + largs)
            good, mae, ratio = compare_np(exr_np.read_rgb(out), ref,
                                          meta["floor"], meta["mean"], 1.1)
            ok = ok and good
            print("%-12s MAE %.5f (floor %.5f, %.2fx)  mean ratio %s  %s" %
                  (name, mae, meta["floor"], mae / max(meta["floor"], 1e-12),
                   ["%.4f" % r for r in ratio], "ok" if good else "FAIL"))
            if lusdview and "-bg" not in largs:
                t = largs[largs.index("-timecode") + 1] if "-timecode" in largs else None
                lv = os.path.join(d, "lusdview.exr")
                if not lusdview_render(lusdview, lv_backend, scene, lv, spp, t):
                    print("SKIP: lusdview %s did not run the Cycles splat kernel" % lv_backend)
                    return 77
                good, mae, ratio = compare_np(exr_np.read_rgb(lv), ref,
                                              meta["floor"], meta["mean"], 1.3)
                ok = ok and good
                print("%-12s lusdview %s MAE %.5f (%.2fx)  mean ratio %s  %s" %
                      ("", lv_backend, mae, mae / max(meta["floor"], 1e-12),
                       ["%.4f" % r for r in ratio], "ok" if good else "FAIL"))
    return 0 if ok else 1


def variants(lusdrender, blender, spp, lusdview="", lv_backend="vk", lusdcat=""):
    ok = True
    for name, bargs, largs in VARIANTS:
        with tempfile.TemporaryDirectory(prefix="gsplat-variant-") as d:
            run([blender, "-b", "--factory-startup", "--python",
                 os.path.join(HERE, "blender_make_and_export.py"), "--", d,
                 "--width", str(W), "--height", str(H), "--spp", str(spp),
                 "--exr"] + bargs, cwd=d)
            if lusdcat:
                sys.path.insert(0, HERE)
                write_ref(name, d, largs, spp, lusdcat)
            out = os.path.join(d, "ours.exr")
            run([lusdrender, os.path.join(d, "splats.usda"), out] + BACKEND +
                ["--gsplat-kernel", "cycles", "--gsplat-colorspace", "srgb",
                 "-camera", "/World/Cam", "-width", str(W), "-height", str(H),
                 "-samples", str(spp)] + largs)
            cands = [out]
            lv_out = os.path.join(d, "lusdview.exr")
            if lusdview and "-bg" not in largs:
                t = largs[largs.index("-timecode") + 1] if "-timecode" in largs else None
                if lusdview_render(lusdview, lv_backend, os.path.join(d, "splats.usda"),
                                   lv_out, spp, t):
                    cands.append(lv_out)
                else:
                    print("SKIP: lusdview %s did not run the Cycles splat kernel" % lv_backend)
                    return 77
            log = run([blender, "-b", "--factory-startup", "--python",
                       os.path.join(HERE, "blender_compare_exr.py"), "--",
                       os.path.join(d, "ref_seed0.exr"),
                       os.path.join(d, "ref_seed7.exr")] + cands)
            rows = {}
            for line in log.splitlines():
                if line.startswith("CMP "):
                    f = line.split()
                    rows[os.path.basename(f[1])] = (float(f[2]),
                                                    [float(x) for x in f[3:6]])
            floor = rows["floor"][0]
            mae, ratio = rows["ours.exr"]
            good = mae <= 1.1 * floor and all(abs(r - 1) <= 0.01 for r in ratio)
            ok = ok and good
            print("%-11s MAE %.5f (floor %.5f, %.2fx)  mean ratio %s  %s" %
                  (name, mae, floor, mae / max(floor, 1e-12),
                   ["%.4f" % r for r in ratio], "ok" if good else "FAIL"))
            if "lusdview.exr" in rows:
                # The viewer's path tracer is a little noisier (shutter
                # segments, its own sampler): allow 1.3x the floor.
                mae, ratio = rows["lusdview.exr"]
                good = mae <= 1.3 * floor and all(abs(r - 1) <= 0.01 for r in ratio)
                ok = ok and good
                print("%-11s lusdview %s MAE %.5f (%.2fx)  mean ratio %s  %s" %
                      ("", lv_backend, mae, mae / max(floor, 1e-12),
                       ["%.4f" % r for r in ratio], "ok" if good else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
