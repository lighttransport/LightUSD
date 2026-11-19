#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""lusdview --gsplat-kernel cycles in a mixed mesh + splat scene
(tests/usda/gsplat-mixed-*.usda: Lambert plane, sun, 160 splats, made by
tests/gsplat/blender_mixed_scene.py).

Preview backends (cpu, rt, hip, cuda): splats must cast sun shadows. The
black-splat scene is rendered with the sun's shadows on and off; camera-ray
occlusion is identical in both, so their ratio on the lit plane isolates the
splat shadows (Cycles: ~0.83 of the plane falls in shadow-weighted light).

Path-trace backends (pt-vk, pt-hip, pt-cuda; --pt-max-depth 2 = Cycles'
direct light): scene-linear EXRs must reproduce Blender 5.3's
  visibility V = black / plain on the plane         (Cycles 0.7218)
  emission   E = full - black on the plane          (Cycles 0.06014)
where E includes the plane lit by splat emission through the BSDF bounce.

With "motion" (animated splats) or "cammotion" (animated camera) the animated
fixtures (tests/usda/gsplat-mixed-motion-*.usda,
camera shutter 0.5 frames) are used. Path-trace backends then also compare
10x10-pixel block maps of V and E with Blender's
(tests/gsplat/mixed_scene_blocks.json); a static render of the motion scene
fails that check, so it verifies the blur itself.

usage: check_gsplat_mixed.py LUSDVIEW REPO_ROOT BACKEND [motion]
Exit 77 when the backend did not run the Cycles splat kernel.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy as np

CYCLES_V = 0.7218
CYCLES_E_PLANE = 0.06014
W, H = 320, 180


def read_exr_zip(path):
    """Minimal reader for lusdview's ZIP-compressed RGBA EXR (half/float)."""
    d = open(path, "rb").read()
    p, channels, comp, ptype = 8, [], None, 1
    while d[p] != 0:
        e = d.index(b"\0", p); name = d[p:e].decode(); p = e + 1
        e = d.index(b"\0", p); typ = d[p:e].decode(); p = e + 1
        n = struct.unpack("<i", d[p:p + 4])[0]; p += 4
        val = d[p:p + n]; p += n
        if name == "channels":
            q = 0
            while val[q] != 0:
                e = val.index(b"\0", q)
                channels.append(val[q:e].decode())
                ptype = struct.unpack("<i", val[e + 1:e + 5])[0]
                q = e + 1 + 16
        elif name == "compression":
            comp = val[0]
        elif name == "dataWindow":
            x0, y0, x1, y1 = struct.unpack("<4i", val)
    p += 1
    w, h = x1 - x0 + 1, y1 - y0 + 1
    lines = 16 if comp == 3 else 1
    nblocks = (h + lines - 1) // lines
    offsets = struct.unpack("<%dQ" % nblocks, d[p:p + 8 * nblocks])
    img = np.zeros((h, w, len(channels)), np.float32)
    for off in offsets:
        y, size = struct.unpack("<ii", d[off:off + 8])
        raw = d[off + 8:off + 8 + size]
        rows = min(lines, h - (y - y0))
        dt = np.float32 if ptype == 2 else np.float16
        expect = rows * w * len(channels) * np.dtype(dt).itemsize
        if comp == 3 and size < expect:
            t = bytearray(zlib.decompress(raw))
            for i in range(1, len(t)):          # undo the delta predictor
                t[i] = (t[i - 1] + t[i] - 128) & 0xff
            half = (len(t) + 1) // 2             # undo the byte interleave
            raw = bytes(b for pair in zip(t[:half], t[half:]) for b in pair)
        a = np.frombuffer(raw, dt).reshape(rows, len(channels), w)
        img[y - y0:y - y0 + rows] = a.transpose(0, 2, 1)
    order = {c: i for i, c in enumerate(channels)}
    return img[:, :, [order["R"], order["G"], order["B"]]]


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace(): pos += 1
        end = pos
        while not data[end:end + 1].isspace(): end += 1
        parts.append(data[pos:end]); pos = end
    w, h = int(parts[1]), int(parts[2])
    a = np.frombuffer(data[pos + 1:pos + 1 + w * h * 3], np.uint8)
    c = a.reshape(h, w, 3).astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def main():
    lusdview, root, backend = sys.argv[1], sys.argv[2], sys.argv[3]
    # "" (static), "motion" (animated splats) or "cammotion" (animated camera)
    variant = sys.argv[4] if len(sys.argv) > 4 else ""
    motion = variant in ("motion", "cammotion")
    pt = backend.startswith("pt-")
    dev = backend[3:] if pt else backend
    flags = {"cpu": ["--cpu-rt", "--rt-samples", "32"],
             "rt": ["--rt", "--frames", "32"],
             "vk": ["--frames", "48"],
             "hip": ["--hip", "--rt-samples", "32"],
             "cuda": ["--cuda", "--rt-samples", "32"]}[dev]
    if pt:
        flags = (["--path-trace"] + ([] if dev == "vk" else ["--" + dev]) +
                 ["--pt-samples", "96", "--pt-max-depth", "2",
                  "--pt-denoise", "off", "--pt-firefly-clamp", "0"])
    env = dict(os.environ, LUSDVIEW_RT_ALLOW_COLD_COMPILE="1")
    marker = {"cpu": "CPU RT: Cycles gsplat kernel", "rt": "Vulkan RT: Cycles gsplat",
              "vk": "Vulkan RT: Cycles gsplat", "hip": "HIP Cycles gsplat",
              "cuda": "CUDA Cycles gsplat"}[dev]
    with tempfile.TemporaryDirectory(prefix="lusdview-gsplat-mixed-") as d:
        def render(name, usda):
            out = os.path.join(d, name)
            cmd = [lusdview, "--headless", "--no-grid", "--size", "%dx%d" % (W, H)]
            if motion:
                cmd += ["--time", "1"]  # Blender rendered frame 1
            cmd += flags + ["--gsplat-kernel", "cycles", "--gsplat-colorspace",
                            "srgb", "--camera", "/World/Cam",
                            "--screenshot", out + ".ppm"]
            if pt:
                cmd += ["--linear-output", out + ".exr"]
            r = subprocess.run(cmd + [usda], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, env=env,
                               timeout=600)
            if r.returncode != 0:
                print(r.stdout[-4000:])
                raise SystemExit("FAIL: lusdview render %s" % name)
            return r.stdout, out
        src = lambda n: os.path.join(
            root, "tests/usda/gsplat-mixed-%s%s.usda" % (variant + "-" if variant else "", n))
        if pt:
            imgs = {}
            for n in ("black", "full", "plain"):  # splat scene first: skip early
                log, out = render(n, src(n))
                if n != "plain" and marker not in log:
                    print("SKIP: %s did not run the Cycles splat kernel" % backend)
                    return 77
                imgs[n] = read_exr_zip(out + ".exr").astype(np.float64)
            mask = imgs["plain"].mean(2) > 0.05
            V = (imgs["black"] / np.maximum(imgs["plain"], 1e-6)).mean(2)[mask].mean()
            E = (imgs["full"] - imgs["black"])[mask].mean()
            print("%s: visibility %.4f (Cycles %.4f), plane emission %.5f "
                  "(Cycles %.5f)" % (backend, V, CYCLES_V, E, CYCLES_E_PLANE))
            if not variant and abs(V - CYCLES_V) > 0.015:
                return "FAIL: visibility differs from Cycles"
            if not variant and abs(E - CYCLES_E_PLANE) > 0.05 * CYCLES_E_PLANE:
                return "FAIL: splat emission (incl. bounce light) differs from Cycles"
            # Spatial check against Blender's block maps (motion blur shape).
            import json
            ref = json.load(open(os.path.join(root, "tests/gsplat/mixed_scene_blocks.json")))
            ref = ref[variant or "static"]
            Vp = np.where(mask, (imgs["black"] / np.maximum(imgs["plain"], 1e-6)).mean(2), np.nan)
            Ep = np.where(mask, (imgs["full"] - imgs["black"]).mean(2), np.nan)
            def blocks(x, B=10):
                h, w = x.shape
                out = []
                for by in range(h // B):
                    row = []
                    for bx in range(w // B):
                        t = x[by * B:(by + 1) * B, bx * B:(bx + 1) * B]
                        row.append(None if np.isnan(t).mean() > 0.5 else float(np.nanmean(t)))
                    out.append(row)
                return out
            # Only blocks entirely on the lit plane: its blurred outline is
            # mesh (camera) motion blur, sampled by the app in 16 discrete
            # shutter segments (~2% off a continuous shutter at the edge);
            # this check is about the splats.
            fullmask = np.where(mask, 1.0, np.nan)
            full = [[(None if np.isnan(fullmask[by * 10:(by + 1) * 10,
                                                bx * 10:(bx + 1) * 10]).any() else 1.0)
                     for bx in range(fullmask.shape[1] // 10)]
                    for by in range(fullmask.shape[0] // 10)]
            def mad(a, b):
                v = [abs(p - q) for ra, rb, rf in zip(a, b, full)
                     for p, q, f in zip(ra, rb, rf)
                     if p is not None and q is not None and f is not None and
                     not np.isnan(f)]
                return sum(v) / max(len(v), 1)
            dV, dE = mad(blocks(Vp), ref["V"]), mad(blocks(Ep), ref["E"])
            print("%s: block MAD vs Cycles  V %.4f  E %.5f" % (backend, dV, dE))
            # Static vs motion maps differ by ~0.012 in V; noise is ~0.002.
            if dV > 0.005:
                return "FAIL: visibility map differs from Cycles"
            if dE > 0.003:
                return "FAIL: emission map differs from Cycles"
            return 0
        # Preview: shadows on vs off (same camera occlusion).
        noshadow = os.path.join(d, "noshadow.usda")
        text = open(src("black")).read().replace(
            "float inputs:angle = 0\n",
            "float inputs:angle = 0\n        bool inputs:shadow:enable = 0\n", 1)
        open(noshadow, "w").write(text)
        log, on = render("on", src("black"))
        if marker not in log:
            print("SKIP: %s did not run the Cycles splat kernel" % backend)
            return 77
        _, off = render("off", noshadow)
        _, plain = render("plain", src("plain"))
        a, b, p = read_ppm(on + ".ppm"), read_ppm(off + ".ppm"), read_ppm(plain + ".ppm")
        bg = p[2, 2]
        mask = np.abs(p - bg).sum(2) > 0.05
        ratio = (a.mean(2)[mask].sum()) / max(b.mean(2)[mask].sum(), 1e-9)
        print("%s: lit-plane energy with/without splat shadows %.4f" % (backend, ratio))
        if ratio > 0.95:
            return "FAIL: splats cast no shadow"
        return 0


if __name__ == "__main__":
    sys.exit(main())
