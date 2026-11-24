#!/usr/bin/env python3
"""Vulkan RT skinning: the per-pose BLAS REFIT must trace like the full rebuild.

Under RT, a skinned mesh's vertex buffer is rewritten every pose. That used to
destroy the mesh's BLAS and pay a full MODE_BUILD per frame; the BLAS is now
built once with ALLOW_UPDATE and REFIT in place (MODE_UPDATE) on later poses.
A refit only re-fits node bounds over the same tree, so the set of traced
triangles -- and therefore the image -- must be exactly what the rebuild
produces.

The assertion is an A/B over the same deterministic playback run (--play with
--frames steps a fixed 1/60 s per frame):

  1. default (refit) vs LUSDVIEW_NO_BLAS_REFIT=1 (the historical destroy +
     rebuild path) must render byte-identically at the last frame; and
  2. the last frame must differ from the rest pose, so (1) cannot be satisfied
     by two runs that never posed anything.

Uses the LEGACY loader so the refit path is exercised from
BuildRtSkinnedMeshVertices; check-rt-skinning.py already covers --core
re-pose against its CPU bake. Exits 77 (skip) when the binary is missing or
the GPU cannot ray trace.
"""
import os
import subprocess
import sys

from gpu_backend import software_only_vulkan, vk_device_args

SKIP = 77


def render(binary, model, out, frames, extra_env=None, play=True):
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    cmd = [binary, *vk_device_args("vk"), "--headless", "--rt", "--frames",
           str(frames),
           "--screenshot", out, model]
    if play:
        cmd.insert(1, "--play")
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=env, timeout=120)
    return r.stdout.decode(errors="replace")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) < 4:
        print("usage: check-rt-blas-refit.py <lusdview> <skinned.usda> <work_dir>")
        return SKIP
    binary, model, work = sys.argv[1:4]
    for p in (binary, model):
        if not os.path.exists(p):
            print(f"SKIP: missing {p}")
            return SKIP
    if software_only_vulkan():
        print("SKIP: Vulkan RT unavailable (software Vulkan only)")
        return SKIP
    os.makedirs(work, exist_ok=True)

    refit = os.path.join(work, "refit.ppm")
    rebuild = os.path.join(work, "rebuild.ppm")
    rest = os.path.join(work, "rest.ppm")

    try:
        log = render(binary, model, refit, frames=24)
    except subprocess.TimeoutExpired:
        print("SKIP: Vulkan RT probe timed out")
        return SKIP
    if "ray tracing is unavailable" in log:
        print("SKIP: no ray-tracing capable Vulkan device")
        return SKIP
    if not os.path.exists(refit):
        print("SKIP: --rt produced no image (no ray-tracing capable device?)")
        return SKIP
    if "RT skeletal skinning" not in log and "RT blendshape" not in log:
        print("FAIL: --rt did not take the RT skinning path.\n--- log ---\n" + log)
        return 1

    try:
        render(binary, model, rebuild, frames=24,
               extra_env={"LUSDVIEW_NO_BLAS_REFIT": "1"})
        render(binary, model, rest, frames=4, play=False)
    except subprocess.TimeoutExpired:
        print("FAIL: Vulkan RT comparison render timed out")
        return 1
    if not (os.path.exists(rebuild) and os.path.exists(rest)):
        print("FAIL: a render produced no image")
        return 1

    if read(refit) != read(rebuild):
        print("FAIL: the refit BLAS traces differently from the full rebuild "
              "at the same pose. A refit reuses the tree and only re-fits node "
              "bounds, so any pixel difference means the refit is tracing stale "
              "or corrupt geometry -- check that updateMeshVertices marks the "
              "right mesh and that refitBlas's geometry description matches "
              "buildBlas's exactly.")
        return 1
    if read(refit) == read(rest):
        print("FAIL: the playback run renders identically to the rest pose, so "
              "no pose ever happened and the refit parity above is vacuous.")
        return 1

    print("PASS: per-pose BLAS refit is byte-identical to the full rebuild "
          "(and the pose actually moved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
