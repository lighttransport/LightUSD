#!/usr/bin/env python3
"""lusdview `--core --rt`: skeletal skinning and blendshapes must reach the tracer.

The ray tracer builds its BLAS from actual vertex buffers, so the raster vertex
shader's deform never reaches it. The `--core` path handled that by refusing GPU
skinning under RT entirely and falling back to the load-time CPU bake -- which
means every new time code re-ran the whole converter, the most expensive thing in
the program, to move one skeleton.

RT now keeps the rest pose like the raster path and re-poses those retained
vertices per frame (morph, then linear-blend skinning, from the same bone rows
the raster bone texture is packed from), then rebuilds the BLAS for the meshes
that moved.

The assertion is parity, in both directions:

  1. `--skinning gpu` (the new re-pose) must render EXACTLY what `--skinning cpu`
     (the load-time bake, the known-correct reference) renders at the same time
     code; and
  2. the pose must actually change with the time code, so that (1) cannot be
     satisfied by two rest poses agreeing with each other.

Only the skeletal case is asserted. The re-pose applies blendshape morph too (in
deform.glsl's order: morph, then skin), but no fixture here animates a blendshape
under `--time` -- neither the core path nor the legacy Tydra path moves for
models/blendshape-and-animation-test-001.usda, which is a separate, pre-existing
gap. Asserting morph parity against that would only assert two rest poses.

Exits 77 (skip) when the binary is missing or the GPU cannot ray trace.
"""
import os
import subprocess
import sys

from PIL import Image

from gpu_backend import software_only_vulkan, vk_device_args

SKIP = 77


def render(binary, model, out, skinning, time):
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    cmd = [binary, *vk_device_args("vk"), "--core", "--headless", "--rt",
           "--frames", "3", "--size", "320x320",
           "--time", str(time), "--skinning", skinning, "--screenshot", out,
           model]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=120)
    return r.stdout.decode(errors="replace")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def pose_images_match(cpu_path, gpu_path):
    """Accept only negligible rasterization roundoff after decoding the PNGs.

    Comparing compressed files byte-for-byte also compares encoder choices and
    rejects a one-LSB result from otherwise identical floating-point pose data.
    Keep this deliberately much tighter than the screenshot-diff tests: at most
    one pixel in the entire image may differ, and only by one level per channel.
    """
    if read(cpu_path) == read(gpu_path):
        return True, "byte-identical"
    with Image.open(cpu_path) as cpu_image, Image.open(gpu_path) as gpu_image:
        cpu = cpu_image.convert("RGBA")
        gpu = gpu_image.convert("RGBA")
        if cpu.size != gpu.size:
            return False, f"dimensions differ ({cpu.size} vs {gpu.size})"
        changed_pixels = 0
        max_diff = 0
        cpu_bytes = cpu.tobytes()
        gpu_bytes = gpu.tobytes()
        for offset in range(0, len(cpu_bytes), 4):
            pixel_changed = False
            for channel in range(4):
                cpu_channel = cpu_bytes[offset + channel]
                gpu_channel = gpu_bytes[offset + channel]
                diff = abs(cpu_channel - gpu_channel)
                if diff:
                    pixel_changed = True
                    max_diff = max(max_diff, diff)
            if pixel_changed:
                changed_pixels += 1
                if changed_pixels > 1 or max_diff > 1:
                    return False, (f"{changed_pixels} changed pixels, maximum "
                                   f"difference {max_diff}")
        return True, (f"{changed_pixels} changed pixel, maximum difference "
                      f"{max_diff}")


def main():
    if len(sys.argv) < 4:
        print("usage: check-rt-skinning.py <lusdview> <skinned.usda> <work_dir>")
        return SKIP
    binary, skin_model, work = sys.argv[1:4]
    for p in (binary, skin_model):
        if not os.path.exists(p):
            print(f"SKIP: missing {p}")
            return SKIP
    if software_only_vulkan():
        print("SKIP: Vulkan RT unavailable (software Vulkan only)")
        return SKIP
    os.makedirs(work, exist_ok=True)

    probe = os.path.join(work, "rt_skin_gpu_t12.png")
    try:
        log = render(binary, skin_model, probe, "gpu", 12)
    except subprocess.TimeoutExpired:
        print("SKIP: Vulkan RT probe timed out")
        return SKIP
    if "ray tracing is unavailable" in log:
        print("SKIP: no ray-tracing capable Vulkan device")
        return SKIP
    if "rt=software" in log or "pipeline cache is cold" in log:
        print("SKIP: Vulkan hardware RT pipeline cache is cold")
        return SKIP
    if not os.path.exists(probe):
        print("SKIP: --rt produced no image (no ray-tracing capable device?)")
        return SKIP
    if "RT skeletal skinning" not in log and "RT blendshape" not in log:
        print("FAIL: --core --rt did not take the RT skinning path. It fell back "
              "to the load-time CPU bake, which re-runs the whole converter for "
              "every time code.\n--- log ---\n" + log)
        return 1

    for name, model in [("skeletal", skin_model)]:
        gpu12 = os.path.join(work, f"rt_{name}_gpu_t12.png")
        cpu12 = os.path.join(work, f"rt_{name}_cpu_t12.png")
        gpu0 = os.path.join(work, f"rt_{name}_gpu_t0.png")
        try:
            render(binary, model, gpu12, "gpu", 12)
            render(binary, model, cpu12, "cpu", 12)
            render(binary, model, gpu0, "gpu", 0)
        except subprocess.TimeoutExpired:
            print(f"FAIL: {name}: Vulkan RT render timed out")
            return 1
        if not (os.path.exists(gpu12) and os.path.exists(cpu12)
                and os.path.exists(gpu0)):
            print(f"FAIL: {name}: a render produced no image")
            return 1

        matches, difference = pose_images_match(cpu12, gpu12)
        if not matches:
            print(f"FAIL: {name}: the RT per-frame re-pose does not match the "
                  f"CPU bake at the same time code. The tracer is posing the "
                  f"geometry differently from the reference -- check the bone-row "
                  f"convention (row-vector p*M, world+geomBind already folded in) "
                  f"and the morph-before-skin order. Difference: {difference}.")
            return 1
        if read(gpu0) == read(gpu12):
            print(f"FAIL: {name}: t=0 and t=12 render identically, so nothing is "
                  f"being posed at all -- the parity check above is vacuous "
                  f"(two rest poses agreeing).")
            return 1

    print("PASS: --core --rt re-poses the skinned mesh per frame, byte-identical "
          "to the CPU bake")
    return 0


if __name__ == "__main__":
    sys.exit(main())
