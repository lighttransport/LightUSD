#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fast-preset conversion budget: a cold `lusdpreview --preset fast` of the
Caldera hotel_01 scene (~100K prims, 24K meshes, 10K instances) must finish
within the time limit and render a non-blank lusdrender image.

usage: check-preset-timing.py LUSDPREVIEW LUSDRENDER [--limit SECONDS]
The scene path comes from LIGHTUSD_PRESET_SCENE (the Caldera
map_source/prefabs/br/wz_vg/mp_wz_island/commercial/hotel_01.usd); exit 77
when it is unset or missing.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lusdpreview")
    ap.add_argument("lusdrender")
    ap.add_argument("--limit", type=float, default=10.0)
    ap.add_argument("--preset", default="fast")
    a = ap.parse_args()
    scene = os.environ.get("LIGHTUSD_PRESET_SCENE", "")
    if not scene or not os.path.exists(scene):
        print("SKIP: scene not found: %s (set LIGHTUSD_PRESET_SCENE)" % scene)
        return 77
    with tempfile.TemporaryDirectory(prefix="lusdpreview-preset-") as d:
        # Best of two cold conversions: one run can be slowed by unrelated
        # host load; the budget is about what the conversion can do.
        best = None
        for attempt in range(2):
            cache = os.path.join(d, "cache%d" % attempt)
            out = os.path.join(d, "export%d" % attempt)
            t0 = time.monotonic()
            r = subprocess.run([a.lusdpreview, scene, out, "--preset", a.preset,
                                "--preview-cache-dir", cache, "--refresh"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=600)
            wall = time.monotonic() - t0
            print(r.stdout[-2000:])
            if r.returncode != 0:
                return "FAIL: lusdpreview exited %d" % r.returncode
            print("attempt %d: %.2f s" % (attempt + 1, wall))
            if best is None or wall < best[0]:
                best = (wall, r)
            if wall <= a.limit:
                break
        wall, r = best
        m = re.search(r"samples=(\d+)", r.stdout)
        samples = int(m.group(1)) if m else 0
        root = [l.strip() for l in r.stdout.splitlines() if l.strip().endswith(".usdc")]
        print("%s preset: %.2f s wall, %d samples" % (a.preset, wall, samples))
        if samples < 1000:
            return "FAIL: preview has too few samples"
        if wall > a.limit:
            return "FAIL: %s preset took %.2f s (limit %.1f s)" % (a.preset, wall, a.limit)
        if not root:
            return "FAIL: lusdpreview did not report the preview root"
        png = os.path.join(d, "preview.png")
        rr = subprocess.run([a.lusdrender, root[-1], png, "-width", "160",
                             "-height", "90", "-samples", "1"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=600)
        if rr.returncode != 0 or not os.path.exists(png):
            print(rr.stdout[-2000:])
            return "FAIL: lusdrender could not render the preview"
        try:
            from PIL import Image
        except ImportError:
            print("Pillow missing: render checked for success only")
            return 0
        values = Image.open(png).convert("L").tobytes()
        spread = max(values) - min(values) if values else 0
        print("preview render luminance spread %d" % spread)
        if spread < 16:
            return "FAIL: preview render is blank"
    return 0


if __name__ == "__main__":
    sys.exit(main())
