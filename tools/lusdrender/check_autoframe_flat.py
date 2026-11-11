#!/usr/bin/env python3
"""lusdrender `-autoframe` regression: a FLAT scene must not frame edge-on.

MakeUsdRecordCamera picks a fixed axis-aligned view per up-axis (a Z-up scene is
framed from -Y). A flat scene -- a single quad, a ground plane, a card -- has a
near-zero extent along one axis, and when that axis is not the one being looked
down, the camera looks ALONG the plane: the scene is exactly edge-on and the
render comes out BLACK. The common case is a Z-up quad lying in the XY plane.

Renders the Z-up textured plane through the `core` path with -autoframe and
asserts the frame is actually covered. Also renders a solid (non-degenerate) model
to confirm ordinary framing still works.

Usage: check_autoframe_flat.py <lusdrender> <repo_root> <work_dir>
Exits 77 (skip) if the binary or the fixture is missing.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_legacy_texture import read_png_rgb  # noqa: E402

SKIP = 77


def coverage(binary, scene, out_png, extra):
    """Fraction of the frame covered by lit (non-background) pixels."""
    r = subprocess.run([binary, scene, out_png] + extra,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=600)
    if r.returncode != 0 or not os.path.exists(out_png):
        print(f"FAIL: render failed ({scene})\n"
              f"{r.stdout.decode(errors='replace')[:2000]}")
        sys.exit(1)
    px = read_png_rgb(out_png)
    if not px:
        print(f"FAIL: could not decode {out_png}")
        sys.exit(1)
    lit = sum(1 for p in px if sum(p) > 30)
    return lit / float(len(px))


def main():
    if len(sys.argv) < 4:
        print("usage: check_autoframe_flat.py <lusdrender> <repo_root> <work_dir>")
        return SKIP
    binary, repo, work = sys.argv[1], sys.argv[2], sys.argv[3]
    if not os.path.exists(binary):
        print(f"SKIP: lusdrender not found: {binary}")
        return SKIP
    os.makedirs(work, exist_ok=True)

    # Z-up plane lying in the XY plane: the degenerate axis (Z) is NOT the axis the
    # Z-up view looks down (Y), so this framed edge-on and rendered black.
    flat = os.path.join(repo, "models", "texture-cat-plane.usda")
    if not os.path.exists(flat):
        print(f"SKIP: fixture missing: {flat}")
        return SKIP

    cov = coverage(binary, flat, os.path.join(work, "autoframe_flat.png"),
                   ["-autoframe", "-rtPreview"])
    if cov < 0.05:
        print(f"FAIL: -autoframe framed the flat Z-up scene EDGE-ON -- only "
              f"{cov * 100:.1f}% of the frame is covered (expected the plane to "
              f"fill it). The camera must look down the degenerate axis.")
        return 1
    print(f"OK: flat Z-up scene framed face-on ({cov * 100:.1f}% coverage)")

    # A solid model must still frame normally (the degenerate-axis path must not
    # trigger when no extent is zero).
    for name in ("suzanne.usdc", "texturedcube.usdc"):
        solid = os.path.join(repo, "models", name)
        if not os.path.exists(solid):
            continue
        cov = coverage(binary, solid, os.path.join(work, "autoframe_solid.png"),
                       ["-autoframe", "-rtPreview"])
        if cov < 0.05:
            print(f"FAIL: -autoframe lost a solid model ({name}): "
                  f"{cov * 100:.1f}% coverage")
            return 1
        print(f"OK: solid model still framed ({name}, {cov * 100:.1f}% coverage)")

    print("PASS: -autoframe frames flat and solid scenes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
