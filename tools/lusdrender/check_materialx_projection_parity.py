#!/usr/bin/env python3
"""Headless execution test for MaterialX latlong and triplanar projections."""

from __future__ import annotations

import binascii
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zlib

BACKENDS = {
    "vkr": ("-vkr", r"backend: LightRT VK \(ray_query"),
    "cuda": ("-cuda", r"backend: shared CUDA RT"),
    "hip": ("-hip", r"backend: shared HIP RT"),
}


def write_png(path: pathlib.Path, width: int = 16, height: int = 16) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", binascii.crc32(kind + payload) & 0xffffffff))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend((int(255 * x / (width - 1)),
                         int(255 * y / (height - 1)),
                         64 if ((x // 2 + y // 2) & 1) else 224))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(rows), 9)) +
                     chunk(b"IEND", b""))


SCENE = '''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:roughness = 0.7
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/Combine.outputs:out>
      def Shader "LatLong" {
        uniform token info:id = "ND_latlongimage"
        asset inputs:file = @projection.png@
        color3f inputs:default = (0.1,0.2,0.3)
        float3 inputs:viewdir = (1,0,0)
        float inputs:rotation = 45
        color3f outputs:out
      }
      def Shader "Triplanar" {
        uniform token info:id = "ND_triplanarprojection_color3"
        asset inputs:filex = @projection.png@
        asset inputs:filey = @projection.png@
        asset inputs:filez = @projection.png@
        color3f inputs:default = (0.1,0.2,0.3)
        float3 inputs:normal = (0,0,1)
        int inputs:upaxis = 1
        float inputs:blend = 0.5
        color3f outputs:out
      }
      def Shader "Combine" {
        uniform token info:id = "ND_multiply_color3"
        color3f inputs:in1.connect = </World/M/NG/LatLong.outputs:out>
        color3f inputs:in2.connect = </World/M/NG/Triplanar.outputs:out>
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 1 }
}
'''


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    cuda = (os.environ.get("LIGHTUSD_MTLX_PROJECTION_CUDA") == "1" and
            pathlib.Path("/dev/nvidia0").exists() and
            pathlib.Path("/dev/nvidia-uvm").exists())
    hip = (os.environ.get("LIGHTUSD_MTLX_PROJECTION_HIP") == "1" and
           pathlib.Path("/dev/kfd").exists())
    ran = 0
    with tempfile.TemporaryDirectory(prefix="lusdrender-mtlx-projection-") as tmp:
        root = pathlib.Path(tmp)
        write_png(root / "projection.png")
        asset = root / "projection.usda"
        asset.write_text(SCENE, encoding="utf-8")
        for backend, (flag, success) in BACKENDS.items():
            if backend == "cuda" and not cuda:
                continue
            if backend == "hip" and not hip:
                continue
            output = root / f"projection-{backend}.png"
            try:
                result = subprocess.run(
                    [str(binary), str(asset), str(output), flag, "-stats",
                     "--path-trace", "--pt-samples", "1", "-w", "64",
                     "-height", "64", "-autoframe"], stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, timeout=30, check=False)
            except subprocess.TimeoutExpired:
                print(f"{backend}: timed out", file=sys.stderr)
                return 1
            if not re.search(success, result.stdout):
                lower_log = result.stdout.lower()
                if any(word in lower_log for word in
                       ("unavailable", "no cuda", "no hip", "no vulkan",
                        "renderer init failed", "failed to create")) or (
                        backend == "vkr" and
                        re.search(r"Vulkan caps:(?!.*ray_query)", result.stdout)):
                    # compute-only Vulkan device: no hardware ray query
                    continue
                print(result.stdout, file=sys.stderr)
                return 1
            ran += 1
            if (result.returncode or
                    not re.search(r"graphMaterials=[1-9][0-9]* graphNodes=[1-9][0-9]*",
                                  result.stdout) or
                    not output.is_file() or output.stat().st_size < 500):
                print(result.stdout, file=sys.stderr)
                return 1
    if not ran:
        return 77
    print(f"MaterialX projection parity: {ran} backend(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
