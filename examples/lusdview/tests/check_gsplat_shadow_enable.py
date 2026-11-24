#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A light with inputs:shadow:enable = 0 must not be shadowed by Cycles splats.

Uses the mixed-scene fixtures (black splats over a sunlit plane): with the
sun's shadow enabled, plane visibility black/plain is well below 1; with it
disabled the plane must render as if the splats cast no shadow (camera-ray
transmittance still darkens the plane where splats are in front of it, so
only plane pixels the splats do not cover are compared).

usage: check_gsplat_shadow_enable.py LUSDVIEW REPO_ROOT pt-vk|pt-hip|pt-cuda
Exit 77 when the backend does not run the Cycles splat kernel.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "tests", "gsplat"))
import exr_np  # noqa: E402

W, H = 160, 90


def main():
    lusdview, root, backend = sys.argv[1], sys.argv[2], sys.argv[3]
    dev = backend[3:]  # path tracer only: it writes linear EXR
    flags = (["--path-trace"] + ([] if dev == "vk" else ["--" + dev]) +
             ["--pt-samples", "64", "--pt-max-depth", "2",
              "--pt-denoise", "off", "--pt-firefly-clamp", "0"])
    marker = {"vk": "Vulkan RT: Cycles gsplat", "hip": "HIP Cycles gsplat",
              "cuda": "CUDA Cycles gsplat"}[dev]
    env = dict(os.environ, LUSDVIEW_RT_ALLOW_COLD_COMPILE="1")
    src = {n: open(os.path.join(root, "tests/usda/gsplat-mixed-%s.usda" % n)).read()
           for n in ("plain", "black")}
    with tempfile.TemporaryDirectory(prefix="lusdview-shadow-enable-") as d:
        def render(name, text):
            usda = os.path.join(d, name + ".usda")
            open(usda, "w").write(text)
            out = os.path.join(d, name)
            cmd = [lusdview, "--headless", "--no-grid", "--size", "%dx%d" % (W, H),
                   "--background", "black"] + flags + [
                   "--gsplat-kernel", "cycles", "--gsplat-colorspace", "srgb",
                   "--camera", "/World/Cam", "--screenshot", out + ".ppm",
                   "--linear-output", out + ".exr", usda]
            r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, env=env, timeout=600)
            if marker not in r.stdout and "plain" not in name:
                print("SKIP: %s did not run the Cycles splat kernel" % backend)
                raise SystemExit(77)
            if r.returncode != 0 or not os.path.exists(out + ".exr"):
                print(r.stdout[-3000:])
                raise SystemExit("FAIL: lusdview render %s" % name)
            return r.stdout, exr_np.read_rgb(out + ".exr")
        off = lambda t: t.replace('float inputs:intensity = 3.0',
                                  'float inputs:intensity = 3.0\n'
                                  '        bool inputs:shadow:enable = 0', 1)
        log, black_on = render("black_on", src["black"])
        if marker not in log:
            print("SKIP: %s did not run the Cycles splat kernel" % backend)
            return 77
        _, black_off = render("black_off", off(src["black"]))
        _, plain_on = render("plain_on", src["plain"])
        _, plain_off = render("plain_off", off(src["plain"]))
    lit = plain_on.mean(2) > 0.05
    # Visibility = sun-shadow transmittance x camera-ray transmittance; with
    # shadow:enable = 0 only the camera-ray part may remain.
    v_on = (black_on / np.maximum(plain_on, 1e-6)).mean(2)[lit]
    v_off = (black_off / np.maximum(plain_off, 1e-6)).mean(2)[lit]
    print("%s: plane visibility with shadow %.4f, shadow:enable=0 %.4f" %
          (backend, v_on.mean(), v_off.mean()))
    if v_on.mean() > 0.95:
        return "FAIL: fixture splats cast no visible shadow (test is inert)"
    # Without splat shadows only camera-ray transmittance remains.
    if v_off.mean() < v_on.mean() + 0.25 * (1.0 - v_on.mean()):
        return "FAIL: splats still shadow a light with shadow:enable = 0"
    return 0


if __name__ == "__main__":
    sys.exit(main())
