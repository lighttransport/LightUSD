#!/usr/bin/env python3
"""lusdview: a texture must sample the UV set its UsdPrimvarReader names.

Three bugs, one behind the other:

  1. Tydra derived the SECONDARY UV set as `<primary> + "1"`, so only st1 /
     UVMap1 / uv1 were ever extracted. A mesh whose second set is named `uvSet1`,
     `map2` or `UVMap.001` (all common) had no second set at all, and
     RenderTexture::uv_primvar pointed at a set that did not exist.
  2. Even when it did exist, nothing routed it: every texture sampled uv0. The
     second set reached the GPU only as a debug AOV.
  3. Even once it was extracted AND routed, it never reached the GPU on the --core
     path: uv0 rides inside DrawVertex, but uv1 is a parallel array, and the batch
     builder appended the vertices without it. Every batched draw got an EMPTY uv1,
     so the sampler fetched one fixed coordinate and the crop came out FLAT.

The fixture is one quad with `primvars:st` (the full [0,1] square) and
`primvars:uvSet1` (the lower-left quarter). Its base-color texture reads uvSet1,
so a correctly-routed render samples a 4x ZOOMED crop of the checkerboard -- the
same black/white contrast, in 4x bigger cells -- while a mis-routed one samples st
and shows the full fine checker. The control renders the scene with the reader
pointed at `st`.

Asserts, per backend (GL always, Vulkan when present):
  1. the two renders DIFFER -- under bug 1 or 2 the uvSet1 texture silently falls
     back to st and the images are identical; and
  2. the uvSet1 render still has CONTRAST -- under bug 3 it collapses to a flat
     fill. (Do NOT re-add the old "uvSet1 must be flatter than st" assertion: a
     zoomed checkerboard is not flat, and that check passed only because bug 3
     made it flat. It was pinning the bug.)

Exits 77 (skip) if the binary/fixture is missing or no display is available.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpu_backend import (  # noqa: E402
    device_name,
    gpu_offload_env,
    is_software_renderer,
    vk_device_args,
)

SKIP = 77
# This test used to demand the uvSet1 render be 3x FLATTER than the st one, on the
# theory that a zoomed crop is nearly uniform. That is wrong for a checkerboard --
# a 4x zoom of one is still a checkerboard, just with 4x bigger cells and the SAME
# black/white contrast -- and the assertion only ever passed because uv1 was broken:
# the batched draw carried an empty uv1 buffer, so every fetch used one coordinate
# and the render came out perfectly flat. The test was pinning the bug.
#
# What actually distinguishes the three outcomes:
#   uvSet1 routed correctly -> differs from st, and still has checker contrast
#   uvSet1 falls back to st -> identical to st            (caught by the != check)
#   uv1 never reaches the GPU -> flat                     (caught by MIN_CROP_STDEV)
# The hardware GL output is tone-mapped, so its full-checker standard deviation
# is typically about 96. 80 remains safely above a smooth/flat texture result
# while avoiding a false failure solely from that presentation transform.
MIN_FULL_STDEV = 80    # the st control must show the checker, else nothing is proven
MIN_CROP_STDEV = 40    # the uvSet1 crop must NOT be flat
BRIGHT_SUM = 60


def read_png_rgb(path):
    """Minimal RGB(A)8 PNG reader (no external deps); returns (w, h, [(r,g,b)])."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None, None, None
    w = h = None
    idat = b""
    color = None
    pos = 8
    while pos + 8 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if w is None or color not in (2, 6):
        return None, None, None
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out = []
    prev = bytearray(stride)
    p = 0
    for _y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):  # undo the PNG row filter
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
        for x in range(w):
            out.append(tuple(line[x * nch:x * nch + 3]))
        prev = line
    return w, h, out


def stdev_of_lit(px):
    lit = [sum(p) for p in px if sum(p) > BRIGHT_SUM]
    if not lit:
        return 0.0
    mean = sum(lit) / len(lit)
    return (sum((v - mean) ** 2 for v in lit) / len(lit)) ** 0.5


def render(binary, model, out_png, backend):
    args = [binary, "--backend", backend, "--core", "--frames", "4",
            "--no-grid", "--view-dir", "0,0,-1",
            "--screenshot", out_png, model]
    args[1:1] = vk_device_args(backend)
    xvfb = shutil.which("xvfb-run")
    # Prefer an inherited DISPLAY -- that is where a HARDWARE GL device lives,
    # and this test only means something there (see gpu_backend.py). Fall back to
    # Xvfb when there is no DISPLAY, or when the one we inherited cannot be
    # opened (a stale forwarded X11 socket, common under ssh/ctest).
    prefixes = []
    if os.environ.get("DISPLAY"):
        prefixes.append([])
    if xvfb:
        prefixes.append([xvfb, "-a"])
    for prefix in prefixes:
        # Xvfb prefix: route GL to the hardware GPU when one is present, else
        # Mesa gives llvmpipe and check_backend can only skip (gpu_backend.py).
        env = gpu_offload_env() if prefix else None
        r = subprocess.run(prefix + args, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=600, env=env)
        if r.returncode == 0 and os.path.exists(out_png):
            return r.stdout.decode(errors="replace")
    return None


def check_backend(binary, secondary, primary, work, backend):
    """None = backend cannot answer (unavailable, or software), else 0 / 1."""
    sec_png = os.path.join(work, f"uvset1_{backend}.png")
    pri_png = os.path.join(work, f"uvset0_{backend}.png")
    out = render(binary, secondary, sec_png, backend)
    if out is None:
        print(f"SKIP: {backend} backend unavailable")
        return None
    # A software rasterizer fetches no vertex attribute but aPosition, so every
    # texture samples uv (0,0) and both renders come out identically wrong --
    # see gpu_backend.py. Nothing to learn here; let the caller try another
    # backend.
    if is_software_renderer(out):
        print(f"SKIP: {backend} is a software renderer ({device_name(out)}); "
              f"it cannot fetch UVs, so this test cannot run on it")
        return None
    if render(binary, primary, pri_png, backend) is None:
        print(f"SKIP: {backend} backend unavailable")
        return None

    _, _, sec = read_png_rgb(sec_png)
    _, _, pri = read_png_rgb(pri_png)
    if sec is None or pri is None:
        print(f"FAIL [{backend}]: could not read the renders back")
        return 1

    if sec == pri:
        print(f"FAIL [{backend}]: routing a texture to `uvSet1` rendered exactly "
              f"the same image as routing it to `st`. The secondary UV set is "
              f"either not extracted (Tydra used to hard-code it as "
              f"<primary>+'1') or not routed (every texture sampled uv0).")
        return 1

    s_sec = stdev_of_lit(sec)
    s_pri = stdev_of_lit(pri)
    if s_pri < MIN_FULL_STDEV:
        print(f"FAIL [{backend}]: the `st` control is not showing the checker "
              f"(stdev {s_pri:.0f}, expected > {MIN_FULL_STDEV}). The texture "
              f"probably failed to load, so this test proves nothing.")
        return 1
    if s_sec < MIN_CROP_STDEV:
        print(f"FAIL [{backend}]: the `uvSet1` render is FLAT (stdev {s_sec:.0f}, "
              f"expected > {MIN_CROP_STDEV}). uvSet1 is a 4x zoom of a checkerboard, "
              f"so it must still show big black/white cells. A flat render means the "
              f"uv1 attribute never reached the shader and every texel fetch used the "
              f"same coordinate -- the batched draw's uv1 buffer is empty.")
        return 1

    print(f"PASS [{backend}]: uvSet1 -> zoomed crop (stdev {s_sec:.0f}), "
          f"st -> full checker (stdev {s_pri:.0f}); the two differ")
    return 0


def main():
    if len(sys.argv) < 4:
        print("usage: check-uv-set-routing.py <lusdview> <model> <work_dir>")
        return SKIP
    binary, model, work = sys.argv[1], sys.argv[2], sys.argv[3]
    if not os.path.exists(binary) or not os.path.exists(model):
        print(f"SKIP: missing binary or fixture ({binary}, {model})")
        return SKIP
    if not shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        print("SKIP: no display and no xvfb-run")
        return SKIP
    os.makedirs(work, exist_ok=True)

    # The control is the same scene with the reader pointed at the PRIMARY set.
    # Generated next to the fixture so its relative texture path still resolves.
    src = open(model).read()
    if 'token inputs:varname = "uvSet1"' not in src:
        print("SKIP: fixture does not bind uvSet1; nothing to compare")
        return SKIP
    primary = os.path.join(os.path.dirname(os.path.abspath(model)),
                           ".uv-set-routing-control.usda")
    with open(primary, "w") as f:
        f.write(src.replace('token inputs:varname = "uvSet1"',
                            'token inputs:varname = "st"'))
    try:
        # Every backend that can answer must agree. GL may be unable to (absent,
        # or a software rasterizer that fetches no UVs) -- then Vulkan carries
        # the test. If neither can answer, skip rather than assert on garbage.
        answered = False
        for backend in ("gl", "vk"):
            rc = check_backend(binary, model, primary, work, backend)
            if rc is None:
                continue
            if rc != 0:
                return rc
            answered = True
        return 0 if answered else SKIP
    finally:
        if os.path.exists(primary):
            os.remove(primary)


if __name__ == "__main__":
    sys.exit(main())
