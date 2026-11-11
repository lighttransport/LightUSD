#!/usr/bin/env python3
"""lusdview `--core` regression: skinning must reach INSTANCED prototypes.

A skinned rig behind a scenegraph instance (`instanceable = true`) is drawn from a
shared prototype. The core loader used to convert the prototype's geometry but
never skin it, so every instance rendered the REST pose and the animation was
simply absent -- silently, and only for instanced rigs.

Renders the instanced fixture at two time codes with `--skinning gpu` and asserts,
per backend (GL always; Vulkan too when a device is present):
  1. the viewer engages GPU skinning ("skinning: GPU");
  2. BOTH instances survive composition (the native-instance group reports 2) --
     the prototype of a group is itself one of the authored instanceable prims,
     and losing it once made a 2-instance scene render NOTHING;
  3. the geometry is actually ON SCREEN -- instanced prototypes contribute their
     transformed bounding box to the scene bounds, not just their instance
     origins, which otherwise gives a degenerate auto-frame box that aims the
     camera past the geometry (this rendered an empty frame under Vulkan); and
  4. the two frames DIFFER -- the animation reaches the prototype. This is what
     fails on the old behavior (a rest pose at every time code gives identical
     frames).

The CPU and GPU modes are deliberately NOT pixel-compared: the CPU path bakes a
static pose while the GPU path poses per frame, and both draw through the same
flat instanced shader, so an equality check would only restate (4).

Usage: check-instanced-prototype-skinning.py <lusdview> <model> <work_dir>
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
# Fraction of pixels that must change between the two time codes. The rig bends
# ~1.2 scene units; the rest-pose bug pins this at exactly 0.0.
MIN_CHANGED_FRAC = 0.01
CHANNEL_EPS = 8
# The rig covers a few percent of the frame; an empty/mis-framed render has none.
MIN_COVERAGE_FRAC = 0.01
BRIGHT_SUM = 200


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


def render(binary, model, out_png, timecode, backend):
    # Keep the screenshot independent of the developer's persisted ImGui dock
    # layout.  ImGui stores imgui.ini beside the selected config file; without
    # an isolated config a saved, nearly-collapsed Viewport (for example 32 px
    # wide) produces two identical grid-only captures and masquerades as a
    # prototype-skinning failure.
    work = os.path.dirname(out_png)
    config = os.path.join(work, "lusdview-test-config.json")
    if not os.path.exists(config):
        with open(config, "w", encoding="utf-8") as f:
            f.write("{}\n")
    # Config loading deliberately uses a sibling imgui.ini.  The test work
    # directory is reused by CTest, so remove a prior interactive dock layout
    # before each capture; otherwise a collapsed Viewport can survive even
    # though the JSON config is isolated.
    imgui_ini = os.path.join(work, "imgui.ini")
    try:
        os.remove(imgui_ini)
    except FileNotFoundError:
        pass
    args = [binary, "--backend", backend, "--frames", "4", "--time", str(timecode),
            "--skinning", "gpu", "--config", config,
            "--screenshot", out_png, model]
    args[1:1] = vk_device_args(backend)
    xvfb = shutil.which("xvfb-run")
    # Prefer an inherited DISPLAY (that is where a hardware GL device lives) and
    # fall back to Xvfb when there is none, or when the one we inherited cannot
    # be opened (a stale forwarded X11 socket).
    prefixes = []
    if os.environ.get("DISPLAY"):
        prefixes.append([])
    if xvfb:
        prefixes.append([xvfb, "-a"])
    log = ""
    for prefix in prefixes:
        # Xvfb prefix: route GL to the hardware GPU when one is present, else
        # Mesa gives llvmpipe and check_backend can only skip (gpu_backend.py).
        env = gpu_offload_env() if prefix else None
        r = subprocess.run(prefix + args, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=600, env=env)
        log = r.stdout.decode(errors="replace")
        if r.returncode == 0 and os.path.exists(out_png):
            return log, log
    return None, log


def instance_count(log):
    """Instances reported by the core loader, e.g. '-> 1 draws (...), 2 instances'."""
    m = re.search(r"(\d+) instances", log)
    return int(m.group(1)) if m else -1


def visible_instance_count(log):
    """Instances that survived framing/culling, from the final render stats."""
    m = re.search(r"instances (\d+)/(\d+) visible", log)
    return int(m.group(1)) if m else -1


def check_backend(binary, model, work, backend):
    """Returns None when the backend is unavailable (skip), else 0 / 1."""
    a_png = os.path.join(work, f"instanced_skin_{backend}_t0.png")
    b_png = os.path.join(work, f"instanced_skin_{backend}_t12.png")
    log_a, raw_a = render(binary, model, a_png, 0, backend)
    if log_a is None:
        print(f"SKIP: {backend} backend unavailable")
        return None
    # GPU skinning cannot work on a software rasterizer: it fetches no vertex
    # attribute but aPosition, so the joints/weights read as zero and the rig
    # renders its rest pose whatever the code does (see gpu_backend.py).
    if is_software_renderer(raw_a):
        print(f"SKIP: {backend} is a software renderer ({device_name(raw_a)}); "
              f"it does not fetch the skin vertex attributes, so GPU skinning "
              f"cannot be exercised on it")
        return None
    log_b, _ = render(binary, model, b_png, 12, backend)
    if log_b is None:
        print(f"SKIP: {backend} backend unavailable")
        return None

    if "skinning: GPU" not in log_a:
        print(f"FAIL [{backend}]: viewer did not report 'skinning: GPU' on the "
              f"instanced rig -- the prototype's skin data never reached the "
              f"DrawScene.\n"
              + "\n".join(l for l in log_a.splitlines() if "skinning" in l))
        return 1

    ninst = instance_count(log_a)
    if ninst != 2:
        print(f"FAIL [{backend}]: expected 2 instances, loader reported {ninst}. "
              f"A native-instance group's PROTOTYPE is itself one of the authored "
              f"instanceable prims; dropping it loses an instance (and, for a "
              f"2-instance group, the whole scene).")
        return 1

    nvisible = visible_instance_count(log_a)
    if nvisible != 2:
        print(f"FAIL [{backend}]: expected both instances to be drawn, render "
              f"stats reported {nvisible}/2 visible. Check prototype bounds, "
              f"camera framing, and viewport layout.")
        return 1

    wa, ha, pa = read_png_rgb(a_png)
    wb, hb, pb = read_png_rgb(b_png)
    if pa is None or pb is None or (wa, ha) != (wb, hb):
        print(f"FAIL [{backend}]: could not compare renders ({wa}x{ha} vs {wb}x{hb})")
        return 1
    if wa < 256 or ha < 256:
        print(f"FAIL [{backend}]: viewport capture is unexpectedly small "
              f"({wa}x{ha}); persisted UI layout leaked into the test")
        return 1

    lit = sum(1 for p in pa if sum(p) > BRIGHT_SUM)
    cov = lit / float(len(pa))
    if cov < MIN_COVERAGE_FRAC:
        print(f"FAIL [{backend}]: the instanced rig is not on screen "
              f"({cov * 100:.2f}% coverage). Instanced prototypes must contribute "
              f"their transformed BOX to the scene bounds -- bounding only the "
              f"instance origins gives a degenerate auto-frame box.")
        return 1

    changed = sum(1 for p, q in zip(pa, pb)
                  if max(abs(p[i] - q[i]) for i in range(3)) > CHANNEL_EPS)
    frac = changed / float(len(pa))
    if frac < MIN_CHANGED_FRAC:
        print(f"FAIL [{backend}]: the instanced prototype did NOT animate -- "
              f"frames at t=0 and t=12 are the same ({frac * 100:.2f}% of pixels "
              f"differ, need {MIN_CHANGED_FRAC * 100:.0f}%). The prototype is "
              f"rendering its rest pose: skinning is not applied inside the "
              f"instance.")
        return 1

    print(f"PASS [{backend}]: 2 instances, on screen ({cov * 100:.1f}%), skinned "
          f"and animating ({frac * 100:.1f}% of pixels change t=0 -> t=12)")
    return 0


def main():
    if len(sys.argv) < 4:
        print("usage: check-instanced-prototype-skinning.py <lusdview> <model> <work>")
        return SKIP
    binary, model, work = sys.argv[1], sys.argv[2], sys.argv[3]
    if not os.path.exists(binary) or not os.path.exists(model):
        print(f"SKIP: missing binary or fixture ({binary}, {model})")
        return SKIP
    if not shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        print("SKIP: no display and no xvfb-run")
        return SKIP
    os.makedirs(work, exist_ok=True)

    # GL is required; Vulkan is checked too when a device is present (the bounds
    # bug above only showed up there -- GL happened to still frame the geometry).
    rc = check_backend(binary, model, work, "gl")
    if rc is None:
        return SKIP
    if rc != 0:
        return rc
    vk = check_backend(binary, model, work, "vk")
    if vk is not None and vk != 0:
        return vk
    return 0


if __name__ == "__main__":
    sys.exit(main())
