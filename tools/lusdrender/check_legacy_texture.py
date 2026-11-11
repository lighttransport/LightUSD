#!/usr/bin/env python3
"""lusdrender legacy-path texture regression.

The legacy (eager tydra) loader handles every .usda/.usdz -- only .usdc,
-rtPreview and the GPU backends route to the `core` path. It used to flatten each
material to a constant `base_color` and leave every TriInfo::tex_id at -1, so a
photo-textured quad rendered as a UNIFORM GRAY: tydra had resolved and decoded the
UsdUVTexture, but nothing consumed it.

Renders a colour-textured plane and asserts the output is actually textured:
  * saturated  -- a flat/dropped texture shades to pure grayscale (saturation 0)
  * varied     -- many distinct colours, not one flat fill

Also covers the purpose-scoped material binding (material:binding:preview / :full
on an ancestor, as production assets bind), which legacy resolved to nothing --
dropping the material and its texture along with it.

Usage: check_legacy_texture.py <lusdrender> <repo_root> <work_dir>
Exits 77 (skip) if the binary or the fixtures are missing.
"""
import os
import struct
import subprocess
import sys
import zlib

SKIP = 77


def read_png_rgb(path):
    """Minimal PNG reader (no Pillow): returns a list of (r,g,b)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if depth != 8 or ctype not in (2, 6):
        return None
    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        for i in range(0, stride, nch):
            out.append((line[i], line[i + 1], line[i + 2]))
        prev = line
    return out


def surface_spread(px):
    """Widest single-channel spread across the dominant lit colors.

    A checkerboard-textured quad shows two clearly separated plateaus; a flat quad
    shows one, plus a couple of antialiasing edge shades around it -- so a mere
    "more than one distinct color" test would pass on a dropped texture. Measured
    per channel rather than on luminance: the fixture's checker is blue-on-dark, so
    its plateaus differ almost entirely in one channel and barely in luminance.
    """
    surf = {}
    for p in px:
        if sum(p) > 30:
            surf[p] = surf.get(p, 0) + 1
    dominant = [c for c, n in surf.items() if n >= 8]
    if len(dominant) < 2:
        return 0.0
    return max(max(c[ch] for c in dominant) - min(c[ch] for c in dominant)
               for ch in (0, 1, 2))


def render(binary, scene, out_png):
    r = subprocess.run([binary, scene, out_png, "-autoframe"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=600)
    if r.returncode != 0 or not os.path.exists(out_png):
        print(f"FAIL: render failed ({scene})\n{r.stdout.decode(errors='replace')[:2000]}")
        sys.exit(1)
    px = read_png_rgb(out_png)
    if px is None:
        print(f"FAIL: could not decode {out_png}")
        sys.exit(1)
    return px


def main():
    if len(sys.argv) < 4:
        print("usage: check_legacy_texture.py <lusdrender> <repo_root> <work_dir>")
        return SKIP
    binary, repo, work = sys.argv[1], sys.argv[2], sys.argv[3]
    if not os.path.exists(binary):
        print(f"SKIP: lusdrender not found: {binary}")
        return SKIP
    os.makedirs(work, exist_ok=True)

    # (1) A COLOUR-textured plane: the render must carry saturation. A dropped
    #     texture leaves the constant base color, which shades to pure gray.
    scene = os.path.join(repo, "models", "texture-cat-plane.usda")
    if not os.path.exists(scene):
        print(f"SKIP: fixture missing: {scene}")
        return SKIP
    px = render(binary, scene, os.path.join(work, "legacy_tex_color.png"))
    lit = [p for p in px if sum(p) > 30]
    if not lit:
        print("FAIL: render is empty")
        return 1
    max_sat = max(max(p) - min(p) for p in lit)
    distinct = len(set(lit))
    if max_sat < 20:
        print(f"FAIL: legacy render is grayscale (max saturation {max_sat}) -- the "
              f"UsdUVTexture was decoded but never sampled; the material collapsed "
              f"to a constant base_color.")
        return 1
    if distinct < 64:
        print(f"FAIL: legacy render is flat ({distinct} distinct colors) -- texture not sampled")
        return 1
    print(f"OK: colour texture sampled (saturation {max_sat}, {distinct} distinct colors)")

    # (2) Purpose-scoped binding on an ANCESTOR (how production assets bind).
    #     Legacy only ever looked at the all-purpose `material:binding`, so the
    #     material -- and its texture -- was dropped.
    scene2 = os.path.join(repo, "models", "lusdview-material-binding-inheritance.usda")
    if os.path.exists(scene2):
        spread = surface_spread(render(binary, scene2,
                                       os.path.join(work, "legacy_tex_binding.png")))
        if spread < 20:
            print(f"FAIL: purpose-scoped/inherited binding dropped the material -- "
                  f"the quad is flat (channel spread {spread:.0f}, expected the "
                  f"checkerboard's two plateaus).")
            return 1
        print(f"OK: purpose-scoped inherited binding resolves and its texture is "
              f"sampled (channel spread {spread:.0f})")

    # (3) The material is reachable ONLY through a composition arc (the look layer
    #     is referenced). The legacy loader used to call LoadUSDFromFile, which
    #     expands no arcs, so the Material simply did not exist and the mesh fell
    #     back to the default gray -- texture and all.
    scene3 = os.path.join(repo, "models", "nested-look-texture", "root.usda")
    if os.path.exists(scene3):
        spread = surface_spread(render(binary, scene3,
                                       os.path.join(work, "legacy_tex_composed.png")))
        if spread < 20:
            print(f"FAIL: the referenced look layer was not composed -- its Material "
                  f"is missing and the quad is flat (channel spread {spread:.0f}).")
            return 1
        print(f"OK: material composed through a reference and its texture is "
              f"sampled (channel spread {spread:.0f})")

    print("PASS: legacy path composes arcs and samples textures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
