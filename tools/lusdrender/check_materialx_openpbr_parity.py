#!/usr/bin/env python3
"""Headless MaterialX/OpenPBR parity for lusdrender GPU RT backends."""

from __future__ import annotations

import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile


SKIP = 77
BACKENDS = {
    "vkr": ("-vkr", r"backend: LightRT VK \(ray_query, descriptor MaterialX, production path"),
    "cuda": ("-cuda", r"backend: shared CUDA RT \(([^)]+)\), path samples="),
    "hip": ("-hip", r"backend: shared HIP RT \(([^)]+)\), path samples="),
}


def fail(message: str, log: str = "") -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    if log:
        print(log, file=sys.stderr)
    raise RuntimeError(message)


def load_image_reader(repo: pathlib.Path):
    helper_dir = repo / "examples" / "lusdview" / "tests"
    sys.path.insert(0, str(helper_dir))
    try:
        from asset_fingerprint import _read_image  # pylint: disable=import-outside-toplevel
    finally:
        sys.path.pop(0)
    return _read_image


def render(binary: pathlib.Path, asset: pathlib.Path, backend: str,
           output: pathlib.Path, extra_args=()) -> tuple[str, bool]:
    flag, success_pattern = BACKENDS[backend]
    command = [
        str(binary), str(asset), str(output), flag, "-stats", "--path-trace",
        "--pt-samples", "4", "--pt-max-depth", "3", "--pt-rr-depth", "2",
        "-w", "192", "-height", "96", "-autoframe", *extra_args,
    ]
    try:
        run = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=90, check=False)
    except subprocess.TimeoutExpired as exc:
        fail(f"{backend} timed out after 90 seconds", exc.stdout or "")
    log = run.stdout
    if not re.search(success_pattern, log):
        unavailable = (
            "unavailable", "no CUDA", "no HIP", "no Vulkan", "failed to create",
            "Failed to create", "renderer init failed", "WARN: No renderable",
        )
        if any(token in log for token in unavailable):
            print(f"  {backend}: unavailable (skipped)")
            return log, False
        fail(f"{backend} did not report its production RT path", log)
    if run.returncode != 0:
        fail(f"{backend} exited with {run.returncode}", log)
    if not output.is_file() or output.stat().st_size < 500:
        fail(f"{backend} produced a blank/trivial image", log)
    return log, True


def pixels(image, read_image):
    got = read_image(str(image))
    if got is None:
        fail(f"cannot decode {image}")
    width, height, rgb = got
    return width, height, [tuple(rgb[i:i + 3]) for i in range(0, len(rgb), 3)]


def normalized_rmse(a, b, read_image) -> float:
    wa, ha, pa = pixels(a, read_image)
    wb, hb, pb = pixels(b, read_image)
    if (wa, ha) != (wb, hb):
        fail(f"image dimensions differ: {(wa, ha)} vs {(wb, hb)}")
    mse = sum((ca - cb) ** 2 for xa, xb in zip(pa, pb)
              for ca, cb in zip(xa, xb)) / (len(pa) * 3)
    return math.sqrt(mse) / 255.0


def panel_means(image, read_image):
    width, height, px = pixels(image, read_image)
    foreground = [(x, y) for y in range(height) for x in range(width)
                  if max(px[y * width + x]) >= 4]
    if len(foreground) < width * height // 20:
        fail(f"{image.name} has too little rendered foreground")
    xmin = min(x for x, _ in foreground)
    xmax = max(x for x, _ in foreground) + 1
    ymin = min(y for _, y in foreground)
    ymax = max(y for _, y in foreground) + 1
    means = []
    for panel in range(3):
        x0 = xmin + (xmax - xmin) * panel // 3 + 3
        x1 = xmin + (xmax - xmin) * (panel + 1) // 3 - 3
        y0 = ymin + 3
        y1 = ymax - 3
        sample = [px[y * width + x] for y in range(y0, y1)
                  for x in range(x0, x1) if max(px[y * width + x]) >= 4]
        if not sample:
            fail(f"{image.name} panel {panel} is blank")
        means.append(tuple(sum(p[c] for p in sample) / len(sample)
                           for c in range(3)))
    return means


def check_nonblank(image, read_image, require_variation=True) -> None:
    width, height, px = pixels(image, read_image)
    foreground = [value for value in px if max(value) >= 1]
    if len(foreground) < 32:
        fail(f"{image.name} has too little rendered foreground")
    values = [channel for value in foreground for channel in value]
    if require_variation and max(values) - min(values) < 4:
        fail(f"{image.name} has no meaningful image variation")


def center_mean(image, read_image):
    width, height, px = pixels(image, read_image)
    # The single-card fixture is autoframed in the center. Sampling a bounded
    # central region avoids treating the renderer's non-black environment as
    # foreground (which can otherwise swamp the material's channel ordering).
    x0, x1 = width * 3 // 8, width * 5 // 8
    y0, y1 = height * 3 // 8, height * 5 // 8
    sample = [px[y * width + x] for y in range(y0, y1)
              for x in range(x0, x1)]
    if len(sample) < 32:
        fail(f"{image.name} has too little rendered foreground")
    return tuple(sum(value[channel] for value in sample) / len(sample)
                 for channel in range(3))


def quadrant_means(image, read_image):
    width, height, px = pixels(image, read_image)
    means = []
    for cy in (3, 5):
        for cx in (3, 5):
            x0, x1 = width * cx // 8, width * (cx + 1) // 8
            y0, y1 = height * cy // 8, height * (cy + 1) // 8
            sample = [px[y * width + x] for y in range(y0, y1)
                      for x in range(x0, x1)]
            means.append(tuple(sum(value[channel] for value in sample) /
                               len(sample) for channel in range(3)))
    return means


def chroma(rgb):
    total = sum(rgb)
    if total < 1.0:
        return (0.0, 0.0, 0.0)
    return tuple(channel / total for channel in rgb)


def check_semantic_grid(image, backend, read_image):
    means = panel_means(image, read_image)
    left, center, right = means
    if not (left[0] > left[1] > left[2]):
        fail(f"{backend} lost PreviewSurface warm-color ordering: {left}")
    if not (center[2] > center[1] > center[0]):
        fail(f"{backend} lost OpenPBR blue-color ordering: {center}")
    if not (right[0] > right[1] > right[2]):
        fail(f"{backend} lost standard_surface warm-color ordering: {right}")
    if center[2] - center[0] < 8.0:
        fail(f"{backend} flattened the OpenPBR base-color response: {center}")
    print(f"  {backend}: panel means " + " ".join(
        "%.1f,%.1f,%.1f" % value for value in means))
    return means


def require_topology(log: str, backend: str) -> None:
    if backend == "vkr":
        pattern = r"graphMaterials=[1-9][0-9]* graphNodes=[1-9][0-9]*"
    else:
        pattern = r"graphs=[1-9][0-9]* graph_nodes=[1-9][0-9]*"
    if not re.search(pattern, log):
        fail(f"{backend} did not retain executable MaterialX graph topology", log)


def require_hardware_identity(log: str, backend: str) -> None:
    if backend == "cuda" and not re.search(
            r"backend: shared CUDA RT \([^)]*NVIDIA[^)]*\)", log, re.I):
        fail("CUDA did not identify an NVIDIA hardware device", log)
    if backend == "hip" and not re.search(
            r"backend: shared HIP RT \([^)]*(AMD|Radeon)[^)]*\)", log, re.I):
        fail("HIP did not identify an AMD hardware device", log)


def parse_vulkan_devices(value: str):
    devices = []
    for spec in filter(None, value.split(",")):
        fields = spec.split(":", 1)
        if len(fields) != 2 or not fields[0].isdigit() or not fields[1]:
            fail("LUSDR_PARITY_VULKAN_DEVICES must use INDEX:VENDOR entries")
        devices.append((int(fields[0]), fields[1]))
    return devices


def generate_texture_fixtures(repo: pathlib.Path, output: pathlib.Path) -> None:
    env = os.environ.copy()
    env.update({
        "LUSDVIEW": str(repo / "build_ninja" / "lusdview"),
        "LUSDVIEW_TEST_OUT": str(output),
        "LUSDVIEW_SEMANTIC_GENERATE_ONLY": "1",
    })
    run = subprocess.run(
        ["bash", str(repo / "examples" / "lusdview" / "tests" /
                     "run-texture-semantic-aov.sh")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, timeout=30, check=False)
    if run.returncode != 0:
        fail("failed to generate shared semantic texture fixtures", run.stdout)


def write_displacement_fixture(path: pathlib.Path) -> None:
    tex = path.with_name("displacement-checker.ppm")
    pixels = bytearray()
    for y in range(16):
        for x in range(16):
            value = 255 if (x + y) & 1 else 0
            pixels.extend((value, value, value))
    tex.write_bytes(b"P6\n16 16\n255\n" + pixels)
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "BentQuad" {
    uniform token subdivisionScheme = "none"
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (0,-1,0), (1,-1,0), (-1,1,0), (0,1,0), (1,1,0)]
    int[] faceVertexCounts = [4,4]
    int[] faceVertexIndices = [0,1,4,3, 1,2,5,4]
    texCoord2f[] primvars:st = [(0,0), (0.5,0), (1,0), (0,1), (0.5,1), (1,1)] (interpolation = "vertex")
    normal3f[] normals = [(-0.7,0,0.7),(0,0,1),(0.7,0,0.7),(-0.7,0,0.7),(0,0,1),(0.7,0,0.7)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.7,0.3,0.1)
      float inputs:displacement.connect = </World/M/D.outputs:r>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "D" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./displacement-checker.ppm@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float inputs:scale = 4.0
      float outputs:r
    }
  }
  def DistantLight "Key" { float inputs:intensity = 5 }
}
''', encoding="utf-8")


def write_swizzle_fixture(path: pathlib.Path) -> None:
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      float inputs:base_weight = 1
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_roughness = 0.45
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/S.outputs:out>
      def Shader "C" {
        uniform token info:id = "ND_constant_color4"
        color4f inputs:value = (0.02,0.08,0.35,1)
        color4f outputs:out
      }
      def Shader "S" {
        uniform token info:id = "ND_swizzle_color4_color3"
        color4f inputs:in.connect = </World/M/NG/C.outputs:out>
        string inputs:channels = "bgr"
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def write_extended_operator_fixture(path: pathlib.Path) -> None:
    """Author a graph whose expected channel ordering exercises three ops."""
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      float inputs:base_weight = 1
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_roughness = 0.55
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/Pick.outputs:out>
      def Shader "Screen" {
        uniform token info:id = "ND_screen_color3"
        color3f inputs:fg = (0.2,0.4,0.8)
        color3f inputs:bg = (0.5,0.25,0.5)
        color3f outputs:out
      }
      def Shader "Overlay" {
        uniform token info:id = "ND_overlay_color3"
        color3f inputs:fg.connect = </World/M/NG/Screen.outputs:out>
        color3f inputs:bg = (0.25,0.75,0.25)
        color3f outputs:out
      }
      def Shader "Atan" {
        uniform token info:id = "ND_atan_color3"
        color3f inputs:in.connect = </World/M/NG/Overlay.outputs:out>
        color3f outputs:out
      }
      def Shader "Burn" {
        uniform token info:id = "ND_burn_color3"
        color3f inputs:fg.connect = </World/M/NG/Atan.outputs:out>
        color3f inputs:bg = (0.4,0.6,0.8)
        color3f outputs:out
      }
      def Shader "Dodge" {
        uniform token info:id = "ND_dodge_color3"
        color3f inputs:fg.connect = </World/M/NG/Burn.outputs:out>
        color3f inputs:bg = (0.2,0.4,0.6)
        color3f outputs:out
      }
      def Shader "Saturate" {
        uniform token info:id = "ND_saturate_color3"
        color3f inputs:in.connect = </World/M/NG/Dodge.outputs:out>
        float inputs:amount = 0.35
        color3f outputs:out
      }
      def Shader "ToHSV" {
        uniform token info:id = "ND_rgbtohsv_color3"
        color3f inputs:in.connect = </World/M/NG/Saturate.outputs:out>
        color3f outputs:out
      }
      def Shader "ToRGB" {
        uniform token info:id = "ND_hsvtorgb_color3"
        color3f inputs:in.connect = </World/M/NG/ToHSV.outputs:out>
        color3f outputs:out
      }
      def Shader "Other" {
        uniform token info:id = "ND_constant_color3"
        color3f inputs:value = (0.9,0.02,0.02)
        color3f outputs:out
      }
      def Shader "Choose" {
        uniform token info:id = "ND_ifgreater_float"
        float inputs:value1 = 2
        float inputs:value2 = 1
        float inputs:in1 = 1
        float inputs:in2 = 0
        float outputs:out
      }
      def Shader "Pick" {
        uniform token info:id = "ND_switch_color3"
        int inputs:which.connect = </World/M/NG/Choose.outputs:out>
        color3f inputs:in1.connect = </World/M/NG/Other.outputs:out>
        color3f inputs:in2.connect = </World/M/NG/ToRGB.outputs:out>
        color3f inputs:in3 = (0.02,0.02,0.9)
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def write_ramp_fixture(path: pathlib.Path) -> None:
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_roughness = 0.65
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/Ramp.outputs:out>
      def Shader "ST" {
        uniform token info:id = "ND_texcoord_vector2"
        float2 outputs:out
      }
      def Shader "Ramp" {
        uniform token info:id = "ND_ramp4_color3"
        color3f inputs:valuetl = (0.9,0.02,0.02)
        color3f inputs:valuetr = (0.02,0.02,0.9)
        color3f inputs:valuebl = (0.02,0.9,0.02)
        color3f inputs:valuebr = (0.02,0.9,0.02)
        float2 inputs:texcoord.connect = </World/M/NG/ST.outputs:out>
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def write_colorcorrect_fixture(path: pathlib.Path) -> None:
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_weight = 0
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/Correct.outputs:out>
      def Shader "Correct" {
        uniform token info:id = "ND_colorcorrect_color3"
        color3f inputs:in = (0.2,0.4,0.8)
        float inputs:hue = 0.1
        float inputs:saturation = 1
        float inputs:gamma = 1
        float inputs:lift = 0
        float inputs:gain = 1
        float inputs:contrast = 1
        float inputs:contrastpivot = 0.5
        float inputs:exposure = 0
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def write_split_fixture(path: pathlib.Path) -> None:
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_roughness = 0.65
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/TB.outputs:out>
      def Shader "ST" {
        uniform token info:id = "ND_texcoord_vector2"
        float2 outputs:out
      }
      def Shader "LR" {
        uniform token info:id = "ND_splitlr_color3"
        color3f inputs:valuel = (0.9,0.02,0.02)
        color3f inputs:valuer = (0.02,0.02,0.9)
        float inputs:center = 0.5
        float2 inputs:texcoord.connect = </World/M/NG/ST.outputs:out>
        color3f outputs:out
      }
      def Shader "TB" {
        uniform token info:id = "ND_splittb_color3"
        color3f inputs:valuet.connect = </World/M/NG/LR.outputs:out>
        color3f inputs:valueb = (0.02,0.9,0.02)
        float inputs:center = 0.5
        float2 inputs:texcoord.connect = </World/M/NG/ST.outputs:out>
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def write_pattern_fixture(path: pathlib.Path) -> None:
    """Exercise lowered checkerboard, triangle-wave, cell, and fractal nodes."""
    path.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Card" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
  def Material "M" (prepend apiSchemas = ["MaterialXConfigAPI"]) {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    token outputs:mtlx:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      float inputs:base_weight.connect = </World/M/NG.outputs:weight>
      color3f inputs:base_color.connect = </World/M/NG.outputs:base>
      float inputs:specular_weight = 0
      token outputs:surface
    }
    def NodeGraph "NG" {
      color3f outputs:base.connect = </World/M/NG/BaseModulated.outputs:out>
      float outputs:weight.connect = </World/M/NG/Modulated.outputs:out>
      def Shader "ST" {
        uniform token info:id = "ND_texcoord_vector2"
        float2 outputs:out
      }
      def Shader "X" {
        uniform token info:id = "ND_extract_vector2"
        float2 inputs:in.connect = </World/M/NG/ST.outputs:out>
        int inputs:index = 0
        float outputs:out
      }
      def Shader "ScaledX" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/X.outputs:out>
        float inputs:in2 = 2
        float outputs:out
      }
      def Shader "Wave" {
        uniform token info:id = "ND_trianglewave_float"
        float inputs:in.connect = </World/M/NG/ScaledX.outputs:out>
        float outputs:out
      }
      def Shader "CellST" {
        uniform token info:id = "ND_multiply_vector2"
        float2 inputs:in1.connect = </World/M/NG/ST.outputs:out>
        float inputs:in2 = 4
        float2 outputs:out
      }
      def Shader "Cell" {
        uniform token info:id = "ND_cellnoise2d_float"
        float2 inputs:texcoord.connect = </World/M/NG/CellST.outputs:out>
        float outputs:out
      }
      def Shader "CellHalf" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/Cell.outputs:out>
        float inputs:in2 = 0.5
        float outputs:out
      }
      def Shader "CellBias" {
        uniform token info:id = "ND_add_float"
        float inputs:in1.connect = </World/M/NG/CellHalf.outputs:out>
        float inputs:in2 = 0.5
        float outputs:out
      }
      def Shader "Fractal" {
        uniform token info:id = "ND_fractal2d_float"
        float2 inputs:texcoord.connect = </World/M/NG/CellST.outputs:out>
        float inputs:amplitude = 1
        int inputs:octaves = 3
        float inputs:lacunarity = 2
        float inputs:diminish = 0.5
        float outputs:out
      }
      def Shader "FractalAbs" {
        uniform token info:id = "ND_absval_float"
        float inputs:in.connect = </World/M/NG/Fractal.outputs:out>
        float outputs:out
      }
      def Shader "FractalQuarter" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/FractalAbs.outputs:out>
        float inputs:in2 = 0.25
        float outputs:out
      }
      def Shader "FractalBias" {
        uniform token info:id = "ND_add_float"
        float inputs:in1.connect = </World/M/NG/FractalQuarter.outputs:out>
        float inputs:in2 = 0.75
        float outputs:out
      }
      def Shader "NoiseModulation" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/CellBias.outputs:out>
        float inputs:in2.connect = </World/M/NG/FractalBias.outputs:out>
        float outputs:out
      }
      def Shader "Worley" {
        uniform token info:id = "ND_unifiednoise2d_float"
        float2 inputs:texcoord.connect = </World/M/NG/CellST.outputs:out>
        float2 inputs:freq = (1,1)
        float2 inputs:offset = (0,0)
        float inputs:jitter = 1
        float inputs:outmin = 0
        float inputs:outmax = 1
        bool inputs:clampoutput = 1
        int inputs:type = 2
        int inputs:style = 0
        float outputs:out
      }
      def Shader "WorleyQuarter" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/Worley.outputs:out>
        float inputs:in2 = 0.25
        float outputs:out
      }
      def Shader "WorleyBias" {
        uniform token info:id = "ND_add_float"
        float inputs:in1.connect = </World/M/NG/WorleyQuarter.outputs:out>
        float inputs:in2 = 0.75
        float outputs:out
      }
      def Shader "AllNoise" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/NoiseModulation.outputs:out>
        float inputs:in2.connect = </World/M/NG/WorleyBias.outputs:out>
        float outputs:out
      }
      def Shader "Position" {
        uniform token info:id = "ND_position_vector3"
        float3 outputs:out
      }
      def Shader "Fractal3D" {
        uniform token info:id = "ND_fractal3d_float"
        float3 inputs:position.connect = </World/M/NG/Position.outputs:out>
        float inputs:amplitude = 1
        int inputs:octaves = 3
        float inputs:lacunarity = 2
        float inputs:diminish = 0.5
        float outputs:out
      }
      def Shader "Fractal3DAbs" {
        uniform token info:id = "ND_absval_float"
        float inputs:in.connect = </World/M/NG/Fractal3D.outputs:out>
        float outputs:out
      }
      def Shader "Fractal3DBias" {
        uniform token info:id = "ND_add_float"
        float inputs:in1.connect = </World/M/NG/Fractal3DAbs.outputs:out>
        float inputs:in2 = 0.75
        float outputs:out
      }
      def Shader "SpatialNoise" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/AllNoise.outputs:out>
        float inputs:in2.connect = </World/M/NG/Fractal3DBias.outputs:out>
        float outputs:out
      }
      def Shader "Modulated" {
        uniform token info:id = "ND_multiply_float"
        float inputs:in1.connect = </World/M/NG/Wave.outputs:out>
        float inputs:in2.connect = </World/M/NG/SpatialNoise.outputs:out>
        float outputs:out
      }
      def Shader "Checker" {
        uniform token info:id = "ND_checkerboard_color3"
        color3f inputs:color1 = (0.9,0.02,0.02)
        color3f inputs:color2 = (0.02,0.02,0.9)
        float2 inputs:uvtiling = (2,2)
        float2 inputs:uvoffset = (0,0)
        float2 inputs:texcoord.connect = </World/M/NG/ST.outputs:out>
        color3f outputs:out
      }
      def Shader "Grid" {
        uniform token info:id = "ND_grid_color3"
        float2 inputs:texcoord.connect = </World/M/NG/ST.outputs:out>
        float2 inputs:uvtiling = (8,8)
        float2 inputs:uvoffset = (0,0)
        float inputs:thickness = 0.15
        bool inputs:staggered = 1
        color3f outputs:out
      }
      def Shader "GridHalf" {
        uniform token info:id = "ND_multiply_color3FA"
        color3f inputs:in1.connect = </World/M/NG/Grid.outputs:out>
        float inputs:in2 = 0.5
        color3f outputs:out
      }
      def Shader "GridBias" {
        uniform token info:id = "ND_add_color3FA"
        color3f inputs:in1.connect = </World/M/NG/GridHalf.outputs:out>
        float inputs:in2 = 0.5
        color3f outputs:out
      }
      def Shader "BaseModulated" {
        uniform token info:id = "ND_multiply_color3"
        color3f inputs:in1.connect = </World/M/NG/Checker.outputs:out>
        color3f inputs:in2.connect = </World/M/NG/GridBias.outputs:out>
        color3f outputs:out
      }
    }
  }
  def DistantLight "Key" { float inputs:intensity = 0.5 }
}
''', encoding="utf-8")


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} LUSDRENDER [REPO_ROOT]", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    repo = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else pathlib.Path(__file__).resolve().parents[2]
    if not binary.is_file():
        print(f"SKIP: lusdrender not found: {binary}")
        return SKIP
    read_image = load_image_reader(repo)
    grid = repo / "tests" / "usda" / "lusdview-material-semantic-grid.usda"
    graph = repo / "tests" / "feat" / "node-mtlx" / "ChainTest.usda"
    if not grid.is_file() or not graph.is_file():
        fail("MaterialX parity fixtures are missing")

    required = {item for item in os.environ.get(
        "LUSDR_PARITY_REQUIRE_BACKENDS", "").split(",") if item}
    require_hardware = os.environ.get("LUSDR_PARITY_REQUIRE_HARDWARE") == "1"
    cuda_cache_expect = os.environ.get("LUSDR_PARITY_CUDA_CACHE_EXPECT", "")
    vulkan_devices = parse_vulkan_devices(os.environ.get(
        "LUSDR_PARITY_VULKAN_DEVICES", ""))
    unknown = required.difference(BACKENDS)
    if unknown:
        fail(f"unknown required backend(s): {sorted(unknown)}")
    if cuda_cache_expect not in ("", "cold", "warm"):
        fail("LUSDR_PARITY_CUDA_CACHE_EXPECT must be 'cold' or 'warm'")
    if cuda_cache_expect and "cuda" not in required:
        fail("CUDA cache validation requires cuda in "
             "LUSDR_PARITY_REQUIRE_BACKENDS")

    with tempfile.TemporaryDirectory(prefix="lusdrender-mtlx-parity-") as tmp:
        out_dir = pathlib.Path(tmp)
        fixture_dir = out_dir / "semantic-fixtures"
        fixture_dir.mkdir()
        lobe_grid = repo / "tests" / "usda" / \
            "lusdrender-openpbr-lobe-golden.usda"
        displacement = out_dir / "displacement.usda"
        swizzle = out_dir / "swizzle.usda"
        extended_ops = out_dir / "extended-operators.usda"
        ramps = out_dir / "ramps.usda"
        splits = out_dir / "splits.usda"
        patterns = out_dir / "patterns.usda"
        colorcorrect = out_dir / "colorcorrect.usda"
        write_displacement_fixture(displacement)
        write_swizzle_fixture(swizzle)
        write_extended_operator_fixture(extended_ops)
        write_ramp_fixture(ramps)
        write_split_fixture(splits)
        write_pattern_fixture(patterns)
        write_colorcorrect_fixture(colorcorrect)
        available = set()
        grid_images = {}
        grid_means = {}
        grid_logs = {}
        print("=== lusdrender MaterialX/OpenPBR semantic grid ===")
        for backend in BACKENDS:
            output = out_dir / f"grid-{backend}.png"
            log, ok = render(binary, grid, backend, output)
            if not ok:
                continue
            available.add(backend)
            grid_logs[backend] = log
            if require_hardware:
                require_hardware_identity(log, backend)
            grid_images[backend] = output
            grid_means[backend] = check_semantic_grid(output, backend, read_image)

        missing = required.difference(available)
        if missing:
            fail(f"required backend(s) unavailable: {sorted(missing)}")
        if not available:
            print("SKIP: no production GPU RT backend is available")
            return SKIP

        generate_texture_fixtures(repo, fixture_dir)

        if cuda_cache_expect:
            cuda_log = grid_logs["cuda"]
            if cuda_cache_expect == "cold":
                if ("CUDA kernel cache miss" not in cuda_log or
                        "CUDA kernel cached:" not in cuda_log):
                    fail("CUDA cold-cache run did not compile and persist PTX",
                         cuda_log)
            elif "CUDA kernel cache hit:" not in cuda_log:
                fail("CUDA warm-cache run did not reuse PTX", cuda_log)
            print(f"  cuda: {cuda_cache_expect} cache behavior verified")

        if {"cuda", "hip"}.issubset(available):
            error = normalized_rmse(grid_images["cuda"], grid_images["hip"], read_image)
            print(f"  CUDA/HIP semantic-grid normalized RMSE: {error:.6f}")
            if error > 0.015:
                fail(f"CUDA/HIP OpenPBR semantic grid diverged (RMSE={error:.6f})")

        if "vkr" in available:
            reference_name = "cuda" if "cuda" in available else "hip" if "hip" in available else ""
            if reference_name:
                for panel, (vk_rgb, ref_rgb) in enumerate(zip(
                        grid_means["vkr"], grid_means[reference_name])):
                    delta = max(abs(a - b) for a, b in zip(chroma(vk_rgb), chroma(ref_rgb)))
                    if delta > 0.12:
                        fail(f"Vulkan/{reference_name} panel {panel} chromaticity diverged ({delta:.4f})")

        for device_index, vendor in vulkan_devices:
            output = out_dir / f"grid-vkr-device-{device_index}.png"
            log, ok = render(binary, grid, "vkr", output,
                             ("-vkDevice", str(device_index)))
            if not ok:
                fail(f"Vulkan device {device_index} ({vendor}) is unavailable")
            if not re.search(rf"Vulkan device: [^\n]*{re.escape(vendor)}", log, re.I):
                fail(f"Vulkan device {device_index} did not identify vendor {vendor}", log)
            if not re.search(r"Vulkan caps:.*\bray_query\b", log):
                fail(f"Vulkan device {device_index} lacks hardware ray query", log)
            check_semantic_grid(output, f"vkr[{device_index}:{vendor}]", read_image)
            print(f"  Vulkan device {device_index}: {vendor} hardware RT verified")

        print("=== lusdrender executable MaterialX graph ===")
        graph_images = {}
        for backend in sorted(available):
            output = out_dir / f"graph-{backend}.png"
            log, ok = render(binary, graph, backend, output)
            if not ok:
                fail(f"{backend} disappeared between semantic and graph renders")
            require_topology(log, backend)
            check_nonblank(output, read_image)
            graph_images[backend] = output
            print(f"  {backend}: executable graph retained and rendered")

        if {"cuda", "hip"}.issubset(graph_images):
            error = normalized_rmse(graph_images["cuda"], graph_images["hip"], read_image)
            print(f"  CUDA/HIP graph normalized RMSE: {error:.6f}")
            # CUDA and HIP share the same interpreter, but their transcendental
            # implementations and stochastic path decisions are not bitwise
            # identical. Keep the same strict 3% normalized image bound used by
            # the texture cases below.
            if error > 0.03:
                fail(f"CUDA/HIP executable MaterialX graph diverged (RMSE={error:.6f})")

        print("=== lusdrender typed MaterialX swizzle ===")
        for backend in sorted(available):
            output = out_dir / f"swizzle-{backend}.png"
            log, ok = render(binary, swizzle, backend, output)
            if not ok:
                fail(f"{backend} disappeared during swizzle validation")
            require_topology(log, backend)
            mean = center_mean(output, read_image)
            if not (mean[0] > mean[1] > mean[2] and mean[0] - mean[2] > 8.0):
                fail(f"{backend} did not execute bgr swizzle: mean={mean}", log)
            print(f"  {backend}: bgr swizzle mean=" +
                  "%.1f,%.1f,%.1f" % mean)

        print("=== lusdrender extended MaterialX operators ===")
        for backend in sorted(available):
            output = out_dir / f"extended-operators-{backend}.png"
            log, ok = render(binary, extended_ops, backend, output)
            if not ok:
                fail(f"{backend} disappeared during extended-operator validation")
            require_topology(log, backend)
            mean = center_mean(output, read_image)
            if not (mean[2] > mean[1] > mean[0] and mean[2] - mean[0] > 8.0):
                fail(f"{backend} did not execute extended color/conditional chain: mean={mean}",
                     log)
            print(f"  {backend}: blend/saturate/HSV/conditional mean=" +
                  "%.1f,%.1f,%.1f" % mean)

        print("=== lusdrender MaterialX color correction ===")
        for backend in sorted(available):
            output = out_dir / f"colorcorrect-{backend}.png"
            log, ok = render(binary, colorcorrect, backend, output)
            if not ok:
                fail(f"{backend} disappeared during colorcorrect validation")
            require_topology(log, backend)
            mean = center_mean(output, read_image)
            if not (mean[2] > mean[0] > mean[1] and mean[2] - mean[1] > 8.0):
                fail(f"{backend} did not execute colorcorrect hue rotation: mean={mean}",
                     log)
            print(f"  {backend}: hue-rotated colorcorrect mean=" +
                  "%.1f,%.1f,%.1f" % mean)

        print("=== lusdrender spatial MaterialX ramps ===")
        for backend in sorted(available):
            output = out_dir / f"ramps-{backend}.png"
            log, ok = render(binary, ramps, backend, output)
            if not ok:
                fail(f"{backend} disappeared during ramp validation")
            require_topology(log, backend)
            means = quadrant_means(output, read_image)
            # Rows are (left,right). LR must trade red for blue in each row;
            # TB must change the green contribution between the two rows.
            horizontal = all(
                means[row * 2][0] - means[row * 2 + 1][0] > 3.0 and
                means[row * 2 + 1][2] - means[row * 2][2] > 3.0
                for row in range(2))
            top_green = (means[0][1] + means[1][1]) * 0.5
            bottom_green = (means[2][1] + means[3][1]) * 0.5
            if not horizontal or abs(top_green - bottom_green) < 5.0:
                fail(f"{backend} did not preserve LR/TB ramp regions: {means}", log)
            print(f"  {backend}: LR/TB ramp quadrants=" + " ".join(
                "%.1f,%.1f,%.1f" % value for value in means))

        print("=== lusdrender connected-texcoord MaterialX splits ===")
        for backend in sorted(available):
            output = out_dir / f"splits-{backend}.png"
            log, ok = render(binary, splits, backend, output)
            if not ok:
                fail(f"{backend} disappeared during split validation")
            require_topology(log, backend)
            means = quadrant_means(output, read_image)
            dominant = {max(range(3), key=lambda channel: value[channel])
                        for value in means
                        if max(value) - min(value) > 5.0}
            if dominant != {0, 1, 2}:
                fail(f"{backend} did not preserve LR/TB split regions: {means}", log)
            print(f"  {backend}: LR/TB split quadrants=" + " ".join(
                "%.1f,%.1f,%.1f" % value for value in means))

        print("=== lusdrender lowered MaterialX patterns ===")
        for backend in sorted(available):
            output = out_dir / f"patterns-{backend}.png"
            log, ok = render(binary, patterns, backend, output)
            if not ok:
                fail(f"{backend} disappeared during pattern validation")
            require_topology(log, backend)
            means = quadrant_means(output, read_image)
            dominant = [max(range(3), key=lambda channel: value[channel])
                        for value in means]
            if dominant[0] != dominant[3] or dominant[1] != dominant[2] or \
                    dominant[0] == dominant[1] or set(dominant) != {0, 2}:
                fail(f"{backend} did not execute checkerboard lowering: {means}", log)
            check_nonblank(output, read_image)
            print(f"  {backend}: checkerboard/trianglewave quadrants=" + " ".join(
                "%.1f,%.1f,%.1f" % value for value in means))

        print("=== lusdrender OpenPBR lobe golden images ===")
        golden_dir = repo / "tests" / "golden"
        for backend in sorted(available):
            output = out_dir / f"openpbr-lobes-golden-{backend}.png"
            log, ok = render(binary, lobe_grid, backend, output,
                             ("--pt-samples", "16"))
            if not ok:
                fail(f"{backend} disappeared during OpenPBR golden validation")
            # Independent geometric-normal control: six planar +Z panels must
            # shade the same with explicit normals or with the missing-normal
            # fallback. normalize(0) previously hid direct light in the golden.
            if backend == "vkr":
                control = out_dir / "lobe-explicit-normals.usda"
                panels = ("PanelBase", "PanelCoat", "PanelTransmission",
                          "PanelSubsurface", "PanelEmission", "PanelOpacity")
                control.write_text('#usda 1.0\n(subLayers = [@' + str(lobe_grid) + '@])\nover "World" {\n' +
                    ''.join(' over "' + panel + '" { uniform token subdivisionScheme = "none"\n normal3f[] normals = [(0,0,1)] (interpolation = "constant")\n }\n' for panel in panels) + '}\n')
                control_output = out_dir / "lobe-explicit-normals.png"
                control_log, control_ok = render(binary, control, backend,
                    control_output, ("--pt-samples", "16"))
                if not control_ok or normalized_rmse(output, control_output, read_image) > 0.01:
                    fail("Vulkan missing-normal fallback differs from analytic +Z panel normals", control_log)
            golden_name = ("lusdrender-openpbr-lobes-vkr.png" if backend == "vkr"
                           else "lusdrender-openpbr-lobes-shared-gpu.png")
            golden = golden_dir / golden_name
            if not golden.is_file():
                fail(f"missing OpenPBR golden image: {golden}")
            error = normalized_rmse(golden, output, read_image)
            if error > 0.01:
                fail(f"{backend} OpenPBR lobe golden diverged (RMSE={error:.6f})",
                     log)
            print(f"  {backend}: six-lobe golden RMSE={error:.6f}")

        print("=== lusdrender extended OpenPBR and texture semantics ===")
        semantic_cases = [
            ("core-textures", fixture_dir / "core-openpbr.usda", True),
            ("core-udim", fixture_dir / "core-openpbr-udim.usda", True),
            ("coat", fixture_dir / "coat.usda", True),
            ("coat-udim", fixture_dir / "coat-udim.usda", True),
            ("coat-normal", fixture_dir / "coat-normal.usda", True),
            ("coat-normal-udim", fixture_dir / "coat-normal-udim.usda", True),
            ("opacity", fixture_dir / "opacity-openpbr.usda", True),
            ("opacity-udim", fixture_dir / "opacity-openpbr-udim.usda", True),
            ("normal", fixture_dir / "normal-openpbr.usda", True),
            ("normal-udim", fixture_dir / "normal-openpbr-udim.usda", True),
            ("uv-routing", repo / "models" / "multi-uv-quad.usda", True),
            ("colorspace", repo / "tests" / "usda" /
             "colorspace-materialx-config-texture-render.usda", True),
        ]
        for case_name, case_asset, expects_textures in semantic_cases:
            case_images = {}
            for backend in sorted(available):
                output = out_dir / f"{case_name}-{backend}.png"
                log, ok = render(binary, case_asset, backend, output)
                if not ok:
                    fail(f"{backend} disappeared during {case_name}")
                if expects_textures:
                    if backend == "vkr":
                        texture_pattern = r"Vulkan material ABI:.*"
                    else:
                        texture_pattern = r"textures=[1-9][0-9]*"
                    if not re.search(texture_pattern, log):
                        fail(f"{backend} did not retain {case_name} texture data", log)
                # Some combined texture fixtures intentionally saturate under
                # shaded path tracing (the diagnostic lusdview AOV test checks
                # their individual channels). Here the loader/backend contract
                # is texture retention plus cross-backend parity.
                check_nonblank(output, read_image, require_variation=False)
                case_images[backend] = output
            if {"cuda", "hip"}.issubset(case_images):
                error = normalized_rmse(
                    case_images["cuda"], case_images["hip"], read_image)
                if error > 0.03:
                    fail(f"CUDA/HIP {case_name} diverged (RMSE={error:.6f})")
            print(f"  {case_name}: {len(case_images)} backend(s) passed")

        print("=== lusdrender displacement response ===")
        displacement_images = {}
        for backend in sorted(available):
            enabled = out_dir / f"displacement-on-{backend}.png"
            disabled = out_dir / f"displacement-off-{backend}.png"
            _, ok_on = render(binary, displacement, backend, enabled)
            _, ok_off = render(binary, displacement, backend, disabled,
                               ("-noDisplace",))
            if not ok_on or not ok_off:
                fail(f"{backend} disappeared during displacement validation")
            response = normalized_rmse(enabled, disabled, read_image)
            if response < 0.002:
                fail(f"{backend} displacement had no visible effect (RMSE={response:.6f})")
            displacement_images[backend] = enabled
            print(f"  {backend}: displacement response RMSE={response:.6f}")
        if {"cuda", "hip"}.issubset(displacement_images):
            error = normalized_rmse(displacement_images["cuda"],
                                    displacement_images["hip"], read_image)
            if error > 0.03:
                fail(f"CUDA/HIP displacement diverged (RMSE={error:.6f})")

    print("PASS: headless lusdrender MaterialX/OpenPBR lobe, texture, graph, and displacement parity")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError:
        sys.exit(1)
