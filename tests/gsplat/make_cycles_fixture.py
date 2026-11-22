#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the same Gaussian splat set as USDA (ParticleField3DGaussianSplat +
camera) and as a standard 3DGS PLY for Blender 5.3 import, so lusdrender
--gsplat-kernel cycles can be compared against Blender Cycles.

Usage: make_cycles_fixture.py OUT_DIR [--count N] [--seed S] [--degree D]
"""
import argparse
import math
import os
import random
import struct

FOCAL_MM = 50.0
APERTURE_MM = 36.0
CAM_Z = 3.0


def make_splats(count, seed, degree):
    rng = random.Random(seed)
    ncoef = (degree + 1) ** 2
    splats = []
    for _ in range(count):
        p = (rng.uniform(-0.8, 0.8), rng.uniform(-0.45, 0.45), rng.uniform(-0.5, 0.5))
        s = (rng.uniform(0.02, 0.15), rng.uniform(0.02, 0.15), rng.uniform(0.005, 0.08))
        q = [rng.gauss(0, 1) for _ in range(4)]
        n = math.sqrt(sum(v * v for v in q))
        q = [v / n for v in q]  # (w, x, y, z)
        o = rng.uniform(0.2, 0.98)
        sh = [(rng.uniform(-1.5, 1.5), rng.uniform(-1.5, 1.5), rng.uniform(-1.5, 1.5))]
        sh += [tuple(rng.uniform(-0.4, 0.4) for _ in range(3)) for _ in range(ncoef - 1)]
        splats.append((p, s, q, o, sh))
    return splats


def write_usda(path, splats, degree):
    f3 = lambda v: "(%r, %r, %r)" % v
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    upAxis = "Y"\n    defaultPrim = "World"\n)\n\n')
        f.write('def Xform "World"\n{\n')
        f.write('    def Camera "Cam"\n    {\n')
        f.write("        float focalLength = %r\n" % FOCAL_MM)
        f.write("        float horizontalAperture = %r\n" % APERTURE_MM)
        f.write("        float verticalAperture = %r\n" % (APERTURE_MM * 9.0 / 16.0))
        f.write("        float2 clippingRange = (0.01, 1000)\n")
        f.write("        double3 xformOp:translate = (0, 0, %r)\n" % CAM_Z)
        f.write('        uniform token[] xformOpOrder = ["xformOp:translate"]\n    }\n')
        f.write('    def ParticleField3DGaussianSplat "Splats"\n    {\n')
        f.write("        point3f[] positions = [%s]\n" % ", ".join(f3(s[0]) for s in splats))
        f.write("        float3[] scales = [%s]\n" % ", ".join(f3(s[1]) for s in splats))
        f.write("        quatf[] orientations = [%s]\n" % ", ".join("(%r, %r, %r, %r)" % tuple(s[2]) for s in splats))
        f.write("        float[] opacities = [%s]\n" % ", ".join(repr(s[3]) for s in splats))
        f.write("        int radiance:sphericalHarmonicsDegree = %d\n" % degree)
        coeffs = [c for s in splats for c in s[4]]
        f.write("        float3[] radiance:sphericalHarmonicsCoefficients = [%s]\n" % ", ".join(f3(c) for c in coeffs))
        f.write("    }\n}\n")


def write_ply(path, splats, degree):
    # Reference 3DGS layout: f_dc_* = DC SH, f_rest_* channel-major
    # (all R bands, then G, then B), logit opacity, log scale, rot = wxyz.
    ncoef = (degree + 1) ** 2
    nrest = (ncoef - 1) * 3
    props = ["x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"]
    props += ["f_rest_%d" % i for i in range(nrest)]
    props += ["opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"]
    with open(path, "wb") as f:
        hdr = "ply\nformat binary_little_endian 1.0\nelement vertex %d\n" % len(splats)
        hdr += "".join("property float %s\n" % p for p in props) + "end_header\n"
        f.write(hdr.encode())
        for p, s, q, o, sh in splats:
            rest = []
            for ch in range(3):
                rest += [sh[k][ch] for k in range(1, ncoef)]
            vals = list(p) + [0, 0, 0] + list(sh[0]) + rest
            vals += [math.log(o / (1 - o))] + [math.log(v) for v in s] + list(q)
            f.write(struct.pack("<%df" % len(vals), *vals))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--count", type=int, default=120)
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--degree", type=int, default=3)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    splats = make_splats(a.count, a.seed, a.degree)
    write_usda(os.path.join(a.out, "splats.usda"), splats, a.degree)
    write_ply(os.path.join(a.out, "splats.ply"), splats, a.degree)


if __name__ == "__main__":
    main()
