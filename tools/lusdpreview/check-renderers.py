#!/usr/bin/env python3
"""Headless preview qualification; logs and images stay in the supplied directory.

Requires NVIDIA NVML so hardware execution cannot silently pass on software.
Runs every representation through both applications on CPU, Vulkan and CUDA.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from PIL import Image


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
    ap.add_argument("--kinds", nargs="+", default=["gaussian", "points", "volume"])
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
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
            for backend in ("cpu", "vk", "cuda"):
                label = f"{kind}-{app}-{backend}"
                image = args.output / (label + ".ppm" if app == "viewer" else label + ".png")
                image.unlink(missing_ok=True)
                common = ["--scene-preview", "--preview-kind", kind,
                          "--preview-cache-dir", str(args.cache),
                          "--preview-budget-mib", "2048"]
                if app == "viewer":
                    cmd = [str(args.viewer.resolve()), *common, "--headless", "--no-grid",
                           "--frames", "1", "--size", "320x240", "--screenshot", str(image),
                           "--render-report", str(args.output / (label + ".json"))]
                    cmd += {"cpu": ["--cpu-rt"], "vk": ["--backend", "vk", "--vk-device", "nvidia"],
                            "cuda": ["--cuda", "--cuda-rt-backend", "software"]}[backend]
                    cmd += [str(args.source)]
                else:
                    cmd = [str(args.renderer.resolve()), str(args.source), str(image), *common,
                           "-w", "320", "-height", "240", "-autoframe", "-stats"]
                    cmd += {"cpu": [], "vk": ["-vkr", "-vkDevice", "0"],
                            "cuda": ["-cuda"]}[backend]
                started = time.monotonic()
                peak_gpu = peak_rss = 0
                reason = None
                with (args.output / (label + ".log")).open("w") as log:
                    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=log, start_new_session=True)
                    while proc.poll() is None:
                        for query in (nv.nvmlDeviceGetGraphicsRunningProcesses_v2,
                                      nv.nvmlDeviceGetComputeRunningProcesses_v2):
                            n = C.c_uint(128)
                            records = (Process * n.value)()
                            if query(device, C.byref(n), records) == 0:
                                for item in records[:n.value]:
                                    if item.pid == proc.pid and item.memory < 2**63:
                                        peak_gpu = max(peak_gpu, item.memory)
                        try:
                            status = Path(f"/proc/{proc.pid}/status").read_text()
                            for line in status.splitlines():
                                if line.startswith("VmRSS:"):
                                    peak_rss = max(peak_rss, int(line.split()[1]) * 1024)
                        except FileNotFoundError:
                            pass
                        available = next(int(x.split()[1]) * 1024 for x in
                                         Path("/proc/meminfo").read_text().splitlines()
                                         if x.startswith("MemAvailable:"))
                        if peak_gpu > 2 * 1024**3:
                            reason = "renderer exceeded 2 GiB VRAM"
                        elif peak_rss > 16 * 1024**3 or available < 4 * 1024**3:
                            reason = "host memory guard"
                        elif time.monotonic() - started > args.timeout:
                            reason = "timeout"
                        if reason:
                            os.killpg(proc.pid, signal.SIGTERM)
                            break
                        time.sleep(.05)
                    code = proc.wait()
                text = (args.output / (label + ".log")).read_text(errors="replace")
                if backend != "cpu" and peak_gpu == 0:
                    reason = reason or "no measured hardware GPU allocation"
                if "Validation Error" in text or "VUID-" in text:
                    reason = reason or "Vulkan validation failure"
                if not image.is_file() or image.stat().st_size < 100:
                    reason = reason or "missing rendered image"
                if image.is_file():
                    with Image.open(image) as rendered:
                        colors=rendered.convert("RGB").getcolors(rendered.width*rendered.height)
                        foreground=rendered.width*rendered.height-max(n for n,_ in colors)
                        if foreground<100:
                            reason=reason or "blank or nearly blank rendered image"
                result = dict(label=label, exit_code=code, failure=reason,
                              seconds=round(time.monotonic()-started, 3),
                              peak_vram_mib=round(peak_gpu/1024**2, 2),
                              peak_rss_mib=round(peak_rss/1024**2, 2), command=cmd)
                results.append(result)
                (args.output / "summary.json").write_text(json.dumps(results, indent=2)+"\n")
                print(json.dumps(result), flush=True)
                if code or reason:
                    raise SystemExit(1)
    nv.nvmlShutdown()


if __name__ == "__main__":
    main()
