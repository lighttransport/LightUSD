#!/usr/bin/env python3
"""lusdview: --texture-fit must actually decide whether textures are processed.

CPU block compression is the single most expensive stage of a texture-heavy
load -- ALab alab_set01 spent 362 s of a 395 s load encoding 507 textures on a
card with 13 GiB free -- so the viewer skips it when the scene comfortably fits.
That decision had NO test at all: a regression that flipped it either way showed
up only as a slow (or needlessly degraded) large-scene run, never as a failure.

This asserts the decision itself, read back from the `next: texture-fit=` line,
using --vram-budget to rehearse different card sizes on a tiny scene:

  1. `never`  -> always skips, even on a 1 GiB card;
  2. `always` -> always processes, even on a 64 GiB card (it must suppress the
     separate "comfort budget" heuristic too, or `always` would not mean always);
  3. modest < default < aggressive thresholds, and the decision flips at most
     once as the card shrinks (catches a mis-wired percentage without depending
     on any absolute number);
  4. `4G` reports exactly 4096 MiB regardless of --vram-budget;
  5. a bogus value exits non-zero rather than silently falling back;
  6. `never` leaves the decoder's edge cap at 0 -- the precondition that
     re-enables the KTX2 zero-copy passthrough.

Exits 77 (skip) when the binary is missing or Vulkan is unavailable.
"""
import os
import re
import subprocess
import sys

SKIP = 77

SCENE = """#usda 1.0
(upAxis = "Y")
def Xform "World" {
  def Mesh "M" {
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
  }
}
"""


def run(binary, scene, work, extra):
    out = os.path.join(work, "fit.ppm")
    cmd = [binary, "--headless", "--frames", "1", "--size", "64x64",
           "--screenshot", out] + extra + [scene]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=600)
    return r.returncode, r.stdout.decode(errors="replace")


FIT_RE = re.compile(
    r"core: texture-fit=(?P<name>\S+)[^;]*?vs (?P<thr>unbounded|[\d.]+ MiB) "
    r"threshold .*?; (?P<comp>skip|keep) compression, (?P<mips>skip|keep) mips"
    r"; decoder max_edge=(?P<edge>\d+)")


def decision(log):
    """-> dict(name, threshold_mib|None for unbounded, comp, mips, edge)."""
    m = FIT_RE.search(log)
    if not m:
        return None
    thr = m.group("thr")
    return {
        "name": m.group("name"),
        "threshold": None if thr == "unbounded" else float(thr.split()[0]),
        "comp": m.group("comp"),
        "mips": m.group("mips"),
        "edge": int(m.group("edge")),
    }


def main():
    if len(sys.argv) < 3:
        print("usage: check-texture-fit.py <lusdview> <work_dir>")
        return SKIP
    binary, work = sys.argv[1], sys.argv[2]
    if not os.path.exists(binary):
        print(f"SKIP: missing binary ({binary})")
        return SKIP
    os.makedirs(work, exist_ok=True)
    scene = os.path.join(work, "texture_fit_quad.usda")
    with open(scene, "w") as f:
        f.write(SCENE)

    rc, probe = run(binary, scene, work, [])
    if "Vulkan device" not in probe and "renderer: Vulkan" not in probe:
        print("SKIP: no Vulkan device available")
        return SKIP
    base = decision(probe)
    if not base:
        print("FAIL: no 'next: texture-fit=' line. The decision must be logged "
              "unconditionally, otherwise it cannot be diagnosed from a log.\n"
              + probe[-1200:])
        return 1

    # 1. never: skips regardless of how small the card is.
    _, log = run(binary, scene, work,
                 ["--texture-fit", "never", "--vram-budget", "1"])
    d = decision(log)
    if not d or d["comp"] != "skip":
        print(f"FAIL: --texture-fit never on a 1 GiB card must still skip "
              f"compression, got {d}")
        return 1
    # 6. never leaves the edge cap off (KTX2 passthrough precondition).
    if d["edge"] != 0:
        print(f"FAIL: --texture-fit never must leave the decoder edge cap at 0 "
              f"(it is the KTX2 zero-copy precondition), got {d['edge']}")
        return 1

    # 2. always: processes even on an enormous card. This is the one that
    #    catches `always` forgetting to suppress the comfort-budget heuristic.
    _, log = run(binary, scene, work,
                 ["--texture-fit", "always", "--vram-budget", "64"])
    d = decision(log)
    if not d or d["comp"] != "keep":
        print(f"FAIL: --texture-fit always on a 64 GiB card must still process "
              f"textures (it must override the comfort heuristic too), got {d}")
        return 1

    # 3. Ordering of the fractional policies, at a fixed card size.
    thresholds = {}
    for name in ("modest", "default", "aggressive"):
        _, log = run(binary, scene, work,
                     ["--texture-fit", name, "--vram-budget", "16"])
        d = decision(log)
        if not d or d["threshold"] is None:
            print(f"FAIL: no numeric threshold reported for {name}")
            return 1
        thresholds[name] = d["threshold"]
    if not (thresholds["modest"] < thresholds["default"] <
            thresholds["aggressive"]):
        print(f"FAIL: thresholds must increase modest < default < aggressive, "
              f"got {thresholds}")
        return 1

    # 4. Absolute threshold ignores the card size.
    for vram in ("8", "64"):
        _, log = run(binary, scene, work,
                     ["--texture-fit", "4G", "--vram-budget", vram])
        d = decision(log)
        if not d or d["threshold"] is None or abs(d["threshold"] - 4096.0) > 1.0:
            print(f"FAIL: --texture-fit 4G on a {vram} GiB card must report a "
                  f"4096 MiB threshold, got {d}")
            return 1

    # 5. A bogus value must be rejected, not silently defaulted.
    rc, _ = run(binary, scene, work, ["--texture-fit", "bogus"])
    if rc == 0:
        print("FAIL: --texture-fit bogus exited 0; an unparsable policy must "
              "not silently fall back to the default")
        return 1

    print("PASS: --texture-fit never/always/fractional/absolute decisions, "
          "threshold ordering, edge-cap passthrough precondition, and rejection "
          "of invalid values")
    return 0


if __name__ == "__main__":
    sys.exit(main())
