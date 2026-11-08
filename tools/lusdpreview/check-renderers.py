#!/usr/bin/env python3
"""Headless preview qualification; logs and images stay in the supplied directory.

Hardware mode requires NVIDIA NVML and checks CPU, Vulkan and CUDA.
Use --software for CPU and Mesa Vulkan development without hardware probing.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import Image

from memory_guard import MIB, available_bytes, group_rss_bytes, stop_group


class Process(C.Structure):
    _fields_ = [("pid", C.c_uint), ("memory", C.c_ulonglong),
                ("gpu_instance", C.c_uint), ("compute_instance", C.c_uint)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path)
    ap.add_argument("--viewer", required=True, type=Path)
    ap.add_argument("--renderer", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--cache", required=True, type=Path)
    ap.add_argument("--timeout", default=1800, type=int)
    ap.add_argument("--camera", default="")
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--no-source-refine", action="store_true")
    ap.add_argument("--software", action="store_true", help="Develop with Mesa software Vulkan/OpenGL; skip CUDA and NVML")
    ap.add_argument("--native-volume", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="Collect all failures before exiting")
    ap.add_argument("--volume-bounces", type=int, default=1)
    ap.add_argument("--volume-samples", type=int, default=2)
    ap.add_argument("--light-samples", type=int, default=8)
    ap.add_argument("--volume-mode", choices=("surface", "medium"), default="surface")
    ap.add_argument("--time", type=float)
    ap.add_argument("--kinds", nargs="+", default=["gaussian", "points", "volume"])
    ap.add_argument("--max-rss-mib", type=int, default=8192)
    ap.add_argument("--reserve-mib", type=int, default=4096)
    ap.add_argument("--preview-host-mib", type=int, default=6144)
    ap.add_argument('--preview-vulkan-library', type=Path, help='Absolute Vulkan loader filename for preview compute')
    ap.add_argument('--preview-cuda-driver-library', type=Path, help='Absolute CUDA driver filename for preview compute')
    ap.add_argument('--preview-cuda-compiler-library', type=Path, help='Absolute NVRTC filename for preview compute')
    args = ap.parse_args()
    host_args = []
    for name in ('vulkan', 'cuda-driver', 'cuda-compiler'):
        path = getattr(args, 'preview_' + name.replace('-', '_') + '_library')
        if path:
            if not path.is_absolute():
                ap.error('preview library filenames must be absolute')
            host_args += ['--preview-' + name + '-library', str(path)]
    if host_args:
        host_args.insert(0, '--preview-native-host')

    if min(args.max_rss_mib, args.reserve_mib, args.preview_host_mib, args.timeout) <= 0:
        ap.error("memory limits and timeout must be positive")
    if args.native_volume: args.kinds = ["volume"]
    if args.frames < 1 or args.frames > 4096: ap.error("frame count must be in 1..4096")
    if args.width < 1 or args.height < 1: ap.error("image dimensions must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    nv = None
    device = C.c_void_p()
    if args.software:
        print("Software: Mesa llvmpipe/lavapipe (hardware qualification disabled)", flush=True)
    else:
        nv = C.CDLL("libnvidia-ml.so.1")
        if nv.nvmlInit_v2():
            raise RuntimeError("NVIDIA NVML initialization failed")
        device = C.c_void_p()
        if nv.nvmlDeviceGetHandleByIndex_v2(0, C.byref(device)):
            raise RuntimeError("NVIDIA device 0 unavailable")
        name = C.create_string_buffer(256)
        nv.nvmlDeviceGetName(device, name, len(name))
        print("Hardware:", name.value.decode(), flush=True)
    env = dict(os.environ, __NV_PRIME_RENDER_OFFLOAD="1",
               __GLX_VENDOR_LIBRARY_NAME="nvidia",
               __EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json")
    results = []
    for kind in args.kinds:
        for app in ("viewer", "renderer"):
            for backend in (("cpu", "vk") if args.software else ("cpu", "vk", "cuda")):
                label = f"{kind}-{app}-{backend}"
                image = args.output / (label + ".ppm" if app == "viewer" else label + ".png")
                image.unlink(missing_ok=True)
                common = ["--scene-preview", "--preview-kind", kind,
                          "--preview-cache-dir", str(args.cache),
                          "--preview-budget-mib", "2048", "--preview-host-mib", str(args.preview_host_mib),
                          "--preview-accumulation-frames", str(args.frames)]
                common += host_args
                if args.software: common += ["--preview-software"]
                if args.no_source_refine: common += ["--preview-no-source-refine"]
                if args.native_volume: common += ["--preview-volume-grids"]
                if kind == "volume": common += ["--preview-volume-mode", args.volume_mode, "--preview-volume-bounces", str(args.volume_bounces), "--preview-volume-samples", str(args.volume_samples), "--preview-light-samples", str(args.light_samples)]
                if app == "viewer":
                    cmd = [str(args.viewer.resolve()), *common, "--headless", "--no-grid",
                           "--frames", str(args.frames), "--size", f"{args.width}x{args.height}", "--screenshot", str(image),
                           "--render-report", str(args.output / (label + ".json"))]
                    cmd += {"cpu": ["--cpu-rt"], "vk": ["--backend", "vk", "--vk-device", "llvmpipe" if args.software else "nvidia"],
                            "cuda": ["--cuda", "--cuda-rt-backend", "software"]}[backend]
                    if args.camera: cmd += ["--camera",args.camera]
                    if args.time is not None: cmd += ["--time",str(args.time)]
                    cmd += [str(args.source)]
                else:
                    cmd = [str(args.renderer.resolve()), str(args.source), str(image), *common,
                           "-w", str(args.width), "-height", str(args.height), "-autoframe", "-stats", "-samples", str(args.frames)]
                    cmd += {"cpu": [], "vk": ["-vkr", "-vkDevice", "0"],
                            "cuda": ["-cuda"]}[backend]
                    if args.camera: cmd += ["-camera",args.camera]
                    if args.time is not None: cmd += ["-timecode",str(args.time)]
                if args.software:
                    cmd = [str(Path(__file__).resolve().parents[2] / "scripts/with-software-rendering.sh"), *cmd]
                if available_bytes() < args.reserve_mib * MIB:
                    raise RuntimeError("insufficient available memory to start renderer")
                started = time.monotonic()
                peak_gpu = peak_rss = 0
                minimum_available = available_bytes()
                reason = None
                with (args.output / (label + ".log")).open("w") as log:
                    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=log, start_new_session=True)
                    try:
                        while proc.poll() is None:
                            for query in ((nv.nvmlDeviceGetGraphicsRunningProcesses_v2,
                                           nv.nvmlDeviceGetComputeRunningProcesses_v2) if nv else ()):
                                n = C.c_uint(128)
                                records = (Process * n.value)()
                                if query(device, C.byref(n), records) == 0:
                                    for item in records[:n.value]:
                                        if item.pid == proc.pid and item.memory < 2**63:
                                            peak_gpu = max(peak_gpu, item.memory)
                            peak_rss = max(peak_rss, group_rss_bytes(proc.pid))
                            available = available_bytes()
                            minimum_available = min(minimum_available, available)
                            if peak_gpu > 2 * 1024**3:
                                reason = "renderer exceeded 2 GiB VRAM"
                            elif peak_rss > args.max_rss_mib * MIB or available < args.reserve_mib * MIB:
                                reason = "host memory guard"
                            elif time.monotonic() - started > args.timeout:
                                reason = "timeout"
                            if reason:
                                break
                            time.sleep(.05)
                    finally:
                        stop_group(proc)
                    code = proc.wait()
                with (args.output / (label + ".log")).open(errors="replace") as log:
                    validation_failure = any("Validation Error" in line or "VUID-" in line for line in log)
                if not args.software and backend != "cpu" and peak_gpu == 0:
                    reason = reason or "no measured hardware GPU allocation"
                if validation_failure:
                    reason = reason or "Vulkan validation failure"
                if not image.is_file() or image.stat().st_size < 100:
                    reason = reason or "missing rendered image"
                if image.is_file():
                    with Image.open(image) as rendered:
                        rendered.thumbnail((320, 240))
                        colors=rendered.convert("RGB").getcolors(rendered.width*rendered.height)
                        foreground=rendered.width*rendered.height-max(n for n,_ in colors)
                        if foreground<100:
                            reason=reason or "blank or nearly blank rendered image"
                result = dict(label=label, exit_code=code, failure=reason, software=args.software,
                              seconds=round(time.monotonic()-started, 3),
                              peak_vram_mib=round(peak_gpu/1024**2, 2),
                              peak_rss_mib=round(peak_rss/1024**2, 2),
                              minimum_available_mib=round(minimum_available/MIB, 2), command=cmd)
                results.append(result)
                (args.output / "summary.json").write_text(json.dumps(results, indent=2)+"\n")
                print(json.dumps(result), flush=True)
                if (code or reason) and not args.keep_going:
                    raise SystemExit(1)
    if nv: nv.nvmlShutdown()
    if any(row["exit_code"] or row["failure"] for row in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
