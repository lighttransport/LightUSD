#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the shared CUDA/HIP trace kernel (raytracer_kernel_src.txt) to PTX
with nvcc, exactly as the CUDA backend's NVRTC would (no NVIDIA GPU needed).

usage: check_cuda_kernel_compile.py KERNEL_SRC [NVCC]   (exit 77: no nvcc)
"""
import os
import shutil
import subprocess
import sys
import tempfile


def normalize(src):
    """Drop the `)CUDA" ... R"CUDA(` raw-string chunk boundaries (as
    NormalizeKernelSource in cuda_raytracer.cc)."""
    close, open_ = ')CUDA"', 'R"CUDA('
    at = src.find(close)
    while at >= 0:
        nxt = src.find(open_, at + len(close))
        if nxt < 0:
            break
        src = src[:at] + src[nxt + len(open_):]
        at = src.find(close, at)
    return src


def main():
    nvcc = sys.argv[2] if len(sys.argv) > 2 else (
        shutil.which("nvcc") or ("/usr/local/cuda/bin/nvcc"
                                 if os.path.exists("/usr/local/cuda/bin/nvcc") else None))
    if not nvcc or not os.path.exists(nvcc):
        print("SKIP: nvcc not found")
        return 77
    src = normalize(open(sys.argv[1]).read())
    with tempfile.TemporaryDirectory(prefix="lusdview-cuda-ptx-") as d:
        cu, ptx = os.path.join(d, "kernel.cu"), os.path.join(d, "kernel.ptx")
        open(cu, "w").write(src)
        r = subprocess.run([nvcc, "-ptx", "-arch=compute_75", "--use_fast_math",
                            "-o", ptx, cu], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        print(r.stdout[-6000:])
        if r.returncode != 0:
            return "FAIL: nvcc rejected the CUDA trace kernel"
        if ".entry trace(" not in open(ptx).read():
            return "FAIL: PTX has no trace entry point"
    print("CUDA trace kernel compiles to PTX")
    return 0


if __name__ == "__main__":
    sys.exit(main())
