#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Points and BasisCurves prims must honour their parent transform in lusdview.

Renders the same cluster as a Mesh, as Points and as BasisCurves under one
translated + rotated + scaled Xform and requires the image centroid of each
to match the mesh's (within 2% of the image size; a dropped translation
moved them by 17-30%). Shading differs between carriers, so the centroid of
every near-white pixel is used (viewer overlays are gray).

usage: check_points_xform.py LUSDVIEW [cpu|vk|hip|cuda]   (exit 77 = skip)
"""
import os
import subprocess
import sys
import tempfile

W, H = 320, 200

HEAD = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def Camera "Cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 22.5
        double3 xformOp:translate = (0, 0, 8)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DistantLight "Sun"
    {
        float inputs:intensity = 3
    }
    def Xform "Group"
    {
        double3 xformOp:translate = (1.4, 0.6, 0)
        float xformOp:rotateZ = 30
        double3 xformOp:scale = (1.5, 1.5, 1.5)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateZ", "xformOp:scale"]
%s
    }
}
"""

# A small asymmetric cluster around local (0.6, 0, 0).
PTS = [(0.4, -0.15, 0), (0.8, -0.15, 0), (0.8, 0.15, 0), (0.4, 0.15, 0)]

BODY = {
    "mesh": """        def Mesh "M"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0.4, -0.15, 0), (0.8, -0.15, 0), (0.8, 0.15, 0), (0.4, 0.15, 0)]
            color3f[] primvars:displayColor = [(1, 1, 1)]
            uniform bool doubleSided = 1
        }""",
    "points": """        def Points "P"
        {
            point3f[] points = [(0.4, -0.15, 0), (0.8, -0.15, 0), (0.8, 0.15, 0), (0.4, 0.15, 0), (0.6, 0, 0)]
            float[] widths = [0.12, 0.12, 0.12, 0.12, 0.12]
            color3f[] primvars:displayColor = [(1, 1, 1)]
        }""",
    "curves": """        def BasisCurves "C"
        {
            uniform token type = "linear"
            int[] curveVertexCounts = [5]
            point3f[] points = [(0.4, -0.15, 0), (0.8, -0.15, 0), (0.8, 0.15, 0), (0.4, 0.15, 0), (0.4, -0.15, 0)]
            float[] widths = [0.06]
            uniform token widths:interpolation = "constant"
            color3f[] primvars:displayColor = [(1, 1, 1)]
        }""",
}


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def centroid(path):
    w, h, px = read_ppm(path)
    sx = sy = n = 0
    for i in range(w * h):
        if px[3 * i] + px[3 * i + 1] + px[3 * i + 2] > 450:
            sx += i % w
            sy += i // w
            n += 1
    if n < 10:
        return None
    return sx / n / w, sy / n / h, n


def main():
    lusdview = sys.argv[1]
    backend = sys.argv[2] if len(sys.argv) > 2 else "cpu"
    flags = {"cpu": ["--cpu-rt", "--rt-samples", "4"], "vk": ["--frames", "8"],
             "hip": ["--hip", "--rt-samples", "4"],
             "cuda": ["--cuda", "--rt-samples", "4"]}[backend]
    env = dict(os.environ, LUSDVIEW_RT_ALLOW_COLD_COMPILE="1")
    res = {}
    with tempfile.TemporaryDirectory(prefix="lusdview-points-xform-") as d:
        for name, body in BODY.items():
            usda = os.path.join(d, name + ".usda")
            open(usda, "w").write(HEAD % body)
            out = os.path.join(d, name + ".ppm")
            cmd = [lusdview, "--headless", "--no-grid", "--background", "black",
                   "--size", "%dx%d" % (W, H), "--camera", "/World/Cam",
                   "--screenshot", out] + flags + [usda]
            r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, env=env, timeout=300)
            if r.returncode != 0 or not os.path.exists(out):
                if backend != "cpu" and "unavailable" in r.stdout.lower():
                    print("SKIP: %s backend unavailable" % backend)
                    return 77
                print(r.stdout[-3000:])
                return "FAIL: lusdview render %s" % name
            res[name] = centroid(out)
            print("%-6s centroid %s" % (name, res[name]))
    if res["mesh"] is None:
        return "FAIL: reference mesh not visible"
    ok = True
    for name in ("points", "curves"):
        c = res[name]
        if c is None:
            print("FAIL: %s not visible (transform dropped?)" % name)
            ok = False
            continue
        dx, dy = abs(c[0] - res["mesh"][0]), abs(c[1] - res["mesh"][1])
        good = dx < 0.02 and dy < 0.02
        print("%-6s offset from mesh (%.4f, %.4f) %s" % (name, dx, dy,
                                                         "ok" if good else "FAIL"))
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
