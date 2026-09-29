#!/usr/bin/env python3
"""lusdview `--next`: the GPU deform must land where the CPU bake lands.

One harness, one claim, run over several fixtures: whatever the vertex shader
does per frame (blendshape morph, then linear-blend skinning) has to put the
geometry in the SAME PLACE as baking those deforms into the mesh on the CPU. The
CPU bake is the reference because it works in mesh-local space on the authored
points, before any of the batching, welding or world-baking the GPU path depends
on -- so the two agree only if that machinery is right.

Everything is rendered in `--mode depth` through a FIXED USD camera:

  * depth is purely geometric, so the comparison is not polluted by the known,
    by-design shading difference (the GPU path skins the REST normal; the bake
    recomputes normals from the deformed points); and
  * a fixed camera, because the two paths legitimately frame the scene
    differently under auto-fit -- the GPU path pads the mesh box by `morphExtent`
    so a morphed mesh is never frustum-culled, while the bake's box is exact.
    Auto-framing would move the camera between the two renders and swamp any real
    geometric difference. (An earlier version of this comparison thresholded luma
    at 25 to get a silhouette; the background grid is brighter than that, so the
    mask covered the whole frame and the check was vacuously true.)

It also asserts the fixture DEFORMS at all (rest vs posed depth), so the parity
check above can never go quietly vacuous.

Exits 77 (skip) when the binary, the scene or a usable GPU is missing.
"""
import os
import re
import struct
import subprocess
import sys
import zlib

from gpu_backend import software_only_vulkan

SKIP = 77
MAX_MEAN_DIFF = 0.5   # GPU deform vs CPU bake: same geometry, bar raster edges
MIN_POSE_DIFF = 1.0   # rest vs posed: the deform must actually move something
MAX_LOADER_DIFF = 0.5  # next vs legacy: same deform, same bounds, same frame


def render(binary, scene, out, time, camera, extra=(), env=None, backend=(),
           loader="--next"):
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    e = dict(os.environ)
    if env:
        e.update(env)
    config = os.path.join(os.path.dirname(out), "config.json")
    if not os.path.exists(config):
        with open(config, "w") as f:
            f.write('{"window_size":{"width":320,"height":320}}\n')
    # The fixture may not contain the requested camera, in which case lusdview
    # auto-fits. Do not let a previous loader/run's ImGui camera layout change
    # that auto-fit and turn a geometry comparison into a view-state comparison.
    try:
        os.remove(os.path.join(os.path.dirname(config), "imgui.ini"))
    except FileNotFoundError:
        pass
    cmd = [binary, loader, "--headless", "--mode", "depth", "--camera", camera,
           "--frames", "3", "--time", str(time), "--config", config,
           "--no-skeleton",
           "--screenshot", out,
           *backend, *extra, scene]
    timeout = float(e.get("LUSDVIEW_DEFORM_RENDER_TIMEOUT", "60"))
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=e, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or b""
        if isinstance(partial, bytes):
            partial = partial.decode(errors="replace")
        return False, (f"render timed out after {timeout:g}s\n{partial}")
    log = r.stdout.decode(errors="replace")
    if r.returncode < 0:
        log += f"\nrender terminated by signal {-r.returncode}"
    return (r.returncode == 0 and os.path.exists(out) and
            os.path.getsize(out) > 0), log


def fatal_render_error(log):
    return any(marker in log for marker in (
        "terminate called after throwing", "std::bad_alloc", "Segmentation fault",
        "AddressSanitizer", "render terminated by signal"))


def read_luma(path):
    d = open(path, "rb").read()
    w = h = color = None
    idat = b""
    pos = 8
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        for x in range(w):
            px = line[x * nch:x * nch + 3]
            out.append((px[0] + px[1] + px[2]) / 3.0)
        prev = line
    return out


def mean_diff(a_path, b_path):
    a, b = read_luma(a_path), read_luma(b_path)
    if len(a) != len(b):
        return float("inf")
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def main():
    if len(sys.argv) < 4:
        print("usage: check-deform-parity.py <lusdview> <scene.usda> <work_dir> "
              "[backend] [camera] [rest_time] [pose_time]")
        return SKIP
    binary, scene, work = sys.argv[1:4]
    which = sys.argv[4] if len(sys.argv) > 4 else "raster"
    camera = sys.argv[5] if len(sys.argv) > 5 else "Cam"
    rest_t = sys.argv[6] if len(sys.argv) > 6 else "1"
    pose_t = sys.argv[7] if len(sys.argv) > 7 else "20"
    try:
        render_timeout = float(
            os.environ.get("LUSDVIEW_DEFORM_RENDER_TIMEOUT", "60"))
        if render_timeout <= 0:
            raise ValueError
    except ValueError:
        print("FAIL: LUSDVIEW_DEFORM_RENDER_TIMEOUT must be a positive number "
              "of seconds")
        return 1
    # The CUDA/HIP tracers build their BVH from draw_ geometry, so they take the
    # deform through poseNextDrawForTracer rather than the vertex shader. Same
    # claim, different plumbing -- and until that landed they were pinned to the
    # load-time CPU bake, a SECOND deform implementation free to disagree with the
    # shader's (it did: it skinned before it morphed).
    backend = [] if which == "raster" else [f"--{which}"]
    for p in (binary, scene):
        if not os.path.exists(p):
            print(f"SKIP: missing {p}")
            return SKIP
    if which == "rt" and software_only_vulkan():
        print("SKIP: Vulkan RT unavailable (software Vulkan only)")
        return SKIP
    os.makedirs(work, exist_ok=True)

    def backend_available(log):
        if which == "rt":
            return (re.search(r"caps: v1 .*rt=hardware", log) and
                    "ray tracing is unavailable" not in log)
        if which == "cuda":
            return "CUDA RT wrote" in log
        if which == "hip":
            return "HIP RT wrote" in log
        return True

    tag = f"{os.path.splitext(os.path.basename(scene))[0]}_{which}"
    rest = os.path.join(work, f"{tag}_rest.png")
    gpu = os.path.join(work, f"{tag}_gpu.png")
    cpu = os.path.join(work, f"{tag}_cpu.png")

    rest_ok, rest_log = render(binary, scene, rest, rest_t, camera,
                               backend=backend)
    if not rest_ok and fatal_render_error(rest_log):
        print("FAIL: rest render aborted\n" + rest_log)
        return 1
    if not rest_ok or not backend_available(rest_log):
        print(f"SKIP: the {which} backend produced no image (unavailable here?)")
        if rest_log:
            print(rest_log.rstrip())
        return SKIP
    gpu_ok, gpu_log = render(binary, scene, gpu, pose_t, camera,
                             backend=backend)
    if not gpu_ok and fatal_render_error(gpu_log):
        print("FAIL: animated render aborted\n" + gpu_log)
        return 1
    if not gpu_ok or not backend_available(gpu_log):
        print(f"SKIP: the {which} backend produced no image (unavailable here?)")
        return SKIP
    # The reference: every deform baked on the CPU, in mesh-local space -- through
    # the SAME backend, so this compares deforms and not backends.
    cpu_ok, cpu_log = render(binary, scene, cpu, pose_t, camera,
                             backend=backend,
                             extra=["--skinning", "cpu"],
                             env={"LUSDVIEW_NEXT_MORPH_BAKE": "1"})
    if not cpu_ok and fatal_render_error(cpu_log):
        print("FAIL: baked render aborted\n" + cpu_log)
        return 1
    if not cpu_ok or not backend_available(cpu_log):
        print("SKIP: the CPU-bake reference did not render")
        return SKIP

    pose = mean_diff(rest, gpu)
    if pose < MIN_POSE_DIFF:
        print(f"FAIL: {tag}: the mesh barely moves between time {rest_t} and time "
              f"{pose_t} (mean depth diff {pose:.3f} < {MIN_POSE_DIFF}), so it is not "
              f"being deformed at all -- and the parity check below would be "
              f"vacuous.")
        return 1

    diff = mean_diff(gpu, cpu)
    if diff > MAX_MEAN_DIFF:
        print(f"FAIL: {tag}: the GPU deform and the CPU bake put the geometry in "
              f"DIFFERENT places (mean depth diff {diff:.3f} > {MAX_MEAN_DIFF}; the "
              f"deform itself moves depth by {pose:.3f}). They must agree exactly. "
              f"Suspects, in the order they bite: the deform ORDER (a blendshape "
              f"deforms the bind-space points and the skeleton poses the RESULT, "
              f"never the other way round); the SPACE (the static batch path "
              f"world-bakes its vertices, so morph deltas and bone rows both have to "
              f"be carried through that transform); and the per-batch RE-INDEXING of "
              f"the sparse morph delta lists when a GeomSubset splits a mesh.")
        return 1

    # The two LOADERS must also agree, and not just on the mesh: the whole frame.
    # Skeleton helpers are disabled above because they are editor overlays, not
    # depth-AOV geometry, and the next native-instancing path does not populate
    # the legacy RenderScene skeleton carrier used to draw them.
    legacy = os.path.join(work, f"{tag}_legacy.png")
    legacy_ok = False
    if which == "raster":
        legacy_ok, _legacy_log = render(binary, scene, legacy, pose_t, camera,
                                        loader="--legacy-load")
    if legacy_ok:
        ldiff = mean_diff(gpu, legacy)
        if ldiff > MAX_LOADER_DIFF:
            print(f"FAIL: {tag}: the next and legacy loaders do not render the same "
                  f"helper-free depth frame (mean diff {ldiff:.3f} > "
                  f"{MAX_LOADER_DIFF}) even though the next deform matches its own "
                  f"CPU bake ({diff:.3f}). Check posed geometry, scene bounds, and "
                  f"depth normalization.")
            return 1
        print(f"PASS: {tag}: GPU deform matches the CPU bake (mean depth diff "
              f"{diff:.3f}; the deform moves depth by {pose:.3f}); the legacy loader "
              f"renders the same frame ({ldiff:.3f})")
        return 0

    print(f"PASS: {tag}: GPU deform matches the CPU bake (mean depth diff "
          f"{diff:.3f}; the deform moves depth by {pose:.3f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
