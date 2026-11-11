#!/usr/bin/env python3
"""HIP interactive tracer: the per-pose BVH REFIT must trace like the rebuild.

The CUDA/HIP tracers build a CPU 2-level BVH from draw_ geometry. The HIP
interactive path re-poses that geometry per time code; it used to pay a FULL
scene rebuild (flatten + BVH split + every buffer re-uploaded, textures
included). The initial build now retains its host arrays + triangle
permutation, and each re-pose REFITS in place: rewrite tris/nrms in leaf
order, refit BLAS/TLAS node bounds over the unchanged trees, upload only
those four buffers.

The assertion is an A/B over the same deterministic playback run (--play with
--frames steps a fixed 1/60 s per frame). The frame count must land MID-CYCLE
of the fixture's looping animation: at a full loop the pose returns to the
initial one, and a broken refit that keeps tracing the initial-build geometry
would pass the parity by coincidence (a mutation proved exactly that at 120
frames = 2 s = one 48-frame loop):

  1. default (refit) vs LUSDVIEW_NO_BVH_REFIT=1 (rebuild-per-pose) must render
     a byte-identical --screenshot at the last frame. The HIP screenshot path
     REUSES the interactive scene when it is posed at the current time code,
     so the capture traces the refit geometry itself (a window shot composites
     the UI, not the traced pixels -- comparing windows proved vacuous); and
  2. the refit run must log refits and the rebuild run must log per-pose
     rebuilds (LUSDVIEW_RT_TIMING), so (1) cannot pass with both runs
     silently taking the same path -- or no poses happening at all.

Needs a window (xvfb) and a HIP device; exits 77 (skip) without either.
"""
import os
import subprocess
import sys

SKIP = 77


def run(xvfb, binary, model, out, frames, extra_env=None):
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    cmd = [xvfb, "-a", binary, "--core", "--hip", "--play",
           "--frames", str(frames), "--screenshot", out, model]
    env = dict(os.environ)
    env["LUSDVIEW_RT_TIMING"] = "1"
    if extra_env:
        env.update(extra_env)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=env, timeout=900)
    return r.stdout.decode(errors="replace"), r.returncode


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) < 5:
        print("usage: check-hip-bvh-refit.py <xvfb-run> <lusdview> "
              "<skinned.usda> <work_dir>")
        return SKIP
    xvfb, binary, model, work = sys.argv[1:5]
    for p in (xvfb, binary, model):
        if not os.path.exists(p):
            print(f"SKIP: missing {p}")
            return SKIP
    os.makedirs(work, exist_ok=True)

    refit_png = os.path.join(work, "refit.ppm")
    rebuild_png = os.path.join(work, "rebuild.ppm")

    log_a, rc_a = run(xvfb, binary, model, refit_png, frames=90)
    if rc_a != 0 and ("glfwInit failed" in log_a or
                      "glfwCreateWindow failed" in log_a or
                      "Failed to open display" in log_a):
        print("SKIP: no usable X11/GLFW display")
        return SKIP
    if "HIP" in log_a and ("unavailable" in log_a or "no HIP device" in log_a):
        print("SKIP: no HIP device")
        return SKIP
    if not os.path.exists(refit_png):
        print("SKIP: HIP produced no screenshot "
              "(no HIP device or no X?)\n--- log tail ---\n" + log_a[-2000:])
        return SKIP

    log_b, rc_b = run(xvfb, binary, model, rebuild_png, frames=90,
                      extra_env={"LUSDVIEW_NO_BVH_REFIT": "1"})
    if rc_b != 0 and ("glfwInit failed" in log_b or
                      "glfwCreateWindow failed" in log_b or
                      "Failed to open display" in log_b):
        print("SKIP: no usable X11/GLFW display")
        return SKIP
    if not os.path.exists(rebuild_png):
        print("FAIL: rebuild run produced no screenshot")
        return 1

    refits_a = log_a.count("[rt_scene_build] refit:")
    rebuilds_b = log_b.count("phaseA(geom)")
    if refits_a == 0:
        print("FAIL: the default run never refit -- either playback did not "
              "advance (no poses at all: the parity below would be vacuous) or "
              "the refit map was not retained (check canRefit / the "
              "displacement gate in BuildHostScene).\n--- log tail ---\n" +
              log_a[-2000:])
        return 1
    if rebuilds_b < 2:  # initial build + at least one per-pose rebuild
        print("FAIL: LUSDVIEW_NO_BVH_REFIT=1 run did not rebuild per pose -- "
              "the A/B lever is broken, so the parity check compares nothing.")
        return 1

    if read(refit_png) != read(rebuild_png):
        print(f"FAIL: the refit trace ({refits_a} refits) differs from the "
              f"rebuild trace ({rebuilds_b} rebuilds) at the same pose. A "
              f"refit only re-fits node bounds over the same tree, so any "
              f"pixel difference means it is tracing stale or mis-permuted "
              f"geometry -- check RefitHostScene's leaf-order rewrite against "
              f"BuildOneMesh's flatten.")
        return 1

    print(f"PASS: HIP per-pose BVH refit ({refits_a} refits) is "
          f"byte-identical to the rebuild path ({rebuilds_b} rebuilds)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
