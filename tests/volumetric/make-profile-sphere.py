#!/usr/bin/env python3
"""Write the closed, consistently wound sphere used for CPU SDF profiling."""
import math
import sys

if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} output.obj")

nlat, nlon = 24, 48
with open(sys.argv[1], "w", encoding="ascii", newline="\n") as out:
    out.write("v 0 0 1\nv 0 0 -1\n")
    for i in range(1, nlat):
        for j in range(nlon):
            theta, phi = math.pi * i / nlat, 2 * math.pi * j / nlon
            out.write(f"v {math.sin(theta) * math.cos(phi):.9g} "
                      f"{math.sin(theta) * math.sin(phi):.9g} {math.cos(theta):.9g}\n")

    def ring(i, j):
        return 3 + (i - 1) * nlon + j % nlon

    for j in range(nlon):
        out.write(f"f 1 {ring(1, j)} {ring(1, j + 1)}\n"
                  f"f 2 {ring(nlat - 1, j + 1)} {ring(nlat - 1, j)}\n")
    for i in range(1, nlat - 1):
        for j in range(nlon):
            a, b = ring(i, j), ring(i + 1, j)
            c, d = ring(i + 1, j + 1), ring(i, j + 1)
            out.write(f"f {a} {b} {c}\nf {a} {c} {d}\n")
