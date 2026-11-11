#!/usr/bin/env python3
"""Fail when the host, CUDA/HIP, and Vulkan material layouts drift apart."""

from pathlib import Path
import re
import sys


def require(text: str, pattern: str, label: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        raise SystemExit(f"GPU material ABI mismatch: {label}")


root = Path(sys.argv[1]).resolve()
abi = (root / "examples/lusdview/gpu_material_abi.h").read_text()
bridge = (root / "examples/lusdview/lightrt_mtlx_bridge.hh").read_text()
kernel = (root / "examples/lusdview/raytracer_kernel_src.txt").read_text()
cpu = (root / "examples/lusdview/cpu/cpu_raytracer.cc").read_text()
shader = (root / "src/external/lightrt/vk/shaders/trace_materialx_path.comp").read_text()
viewer_vk_shader = (root / "examples/lusdview/vk/shaders/raytrace.comp").read_text()
viewer_swrt_shader = (
    root / "examples/lusdview/vk/shaders/raytrace_swbvh.comp"
).read_text()
viewer_raster_shader = (root / "examples/lusdview/vk/shaders/mesh.frag").read_text()
viewer_environment_shader = (
    root / "examples/lusdview/vk/shaders/environment.frag"
).read_text()
viewer_vk_source = (root / "examples/lusdview/vk/vk_renderer.cc").read_text()
openpbr = (root / "src/tydra/openpbr-params.hh").read_text()
vk_header = (root / "src/external/lightrt/lightrt_c_vk.h").read_text()
vk_source = (root / "src/external/lightrt/lightrt_c_vk.c").read_text()

values = {
    "LIGHTUSD_GPU_OPENPBR_FLOATS": 80,
    "LIGHTUSD_GPU_MATERIAL_TEX_PARAM_FLOATS": 155,
    "LIGHTUSD_GPU_MATERIAL_TEX_SLOTS": 12,
    "LIGHTUSD_GPU_RASTER_PBR_VEC4S": 20,
    "LIGHTUSD_GPU_GRAPH_OUTPUTS": 49,
    "LIGHTUSD_GPU_GRAPH_HEADER_FLOATS": 51,
    "LIGHTUSD_GPU_GRAPH_NODE_FLOATS": 21,
    "LIGHTUSD_GPU_GRAPH_MAX_NODES": 64,
    "LIGHTUSD_GPU_GRAPH_FLOATS": 1395,
    "LIGHTUSD_GPU_LIGHTRT_LIGHT_FLOATS": 16,
    "LIGHTUSD_GPU_LIGHTRT_TEXTURE_DESC_INTS": 8,
}

for name, value in values.items():
    require(abi, rf"^#define\s+{name}\s+{value}\b", f"canonical {name}")

for name in (
    "LIGHTUSD_GPU_OPENPBR_FLOATS",
    "LIGHTUSD_GPU_MATERIAL_TEX_PARAM_FLOATS",
    "LIGHTUSD_GPU_MATERIAL_TEX_SLOTS",
    "LIGHTUSD_GPU_GRAPH_FLOATS",
    "LIGHTUSD_GPU_GRAPH_HEADER_FLOATS",
    "LIGHTUSD_GPU_GRAPH_NODE_FLOATS",
    "LIGHTUSD_GPU_GRAPH_MAX_NODES",
):
    require(kernel, rf"^#define\s+{name}\s+{values[name]}\b", f"CUDA/HIP {name}")

for name, value in {
    "MAT_STRIDE": 80,
    "GRAPH_OUTPUTS": 49,
    "GRAPH_HEADER": 51,
    "GRAPH_NODE": 21,
    "GRAPH_NODES": 64,
    "TEX_DESC_STRIDE": 8,
    "LIGHT_STRIDE": 16,
}.items():
    require(shader, rf"^const uint\s+{name}\s*=\s*{value}u;", f"Vulkan {name}")

require(openpbr, r"kLightRtOpenPBRVec4s\s*=\s*20", "Tydra OpenPBR vec4 count")
require(bridge, r"kLightRtOpenPBRFloats\s*==\s*LIGHTUSD_GPU_OPENPBR_FLOATS", "bridge OpenPBR guard")
require(kernel, r"base\s*\+\s*LIGHTUSD_GPU_GRAPH_HEADER_FLOATS\s*\+\s*i\s*\*\s*LIGHTUSD_GPU_GRAPH_NODE_FLOATS", "CUDA/HIP graph record offset")
require(shader, r"r\s*<\s*GRAPH_OUTPUTS", "Vulkan graph output routing")
require(shader, r"op\s*==\s*21\)\s*value\[i\]\s*=\s*vec4\(graphs\[p\+14u\]\s*>=\s*0\.5\s*\?\s*uvb\s*:\s*uv,\s*0,\s*1\)",
        "Vulkan texcoord selects the UV set from the graph flag")
require(shader, r"vertices\[i0\*10u", "Vulkan 10-float vertex stride")
require(shader, r"vec2 uv; vec2 uvb;", "Vulkan Hit carries the second UV set")
require(shader, r"evalGraphAll\(h\.mat,h\.uv,h\.uvb", "Vulkan graph receives both UV sets")
require(shader, r"vec3 tangent; vec3 tangentb;", "Vulkan Hit carries the second tangent")
require(shader, r"graphs\[gb\+50u\]\s*>=\s*0\.5\s*\?\s*h\.tangentb\s*:\s*h\.tangent",
        "Vulkan normal route selects the UV-set tangent")
if re.search(r"base\s*\+\s*46\s*\+\s*i\s*\*\s*21", kernel):
    raise SystemExit("GPU material ABI mismatch: stale CUDA/HIP graph header")
if ("(int)(graph[base + 1 + route] + 0.5f)" in kernel or
        "(int)(graph[p+1]+0.5f)" in kernel or
        "(int)(graph[p+16]+0.5f)" in kernel):
    raise SystemExit("GPU material ABI mismatch: CUDA/HIP truncates -1 graph sentinels")
require(kernel, r"floorf\(graph\[base \+ 1 \+ route\] \+ 0\.5f\)",
        "CUDA/HIP graph output sentinel decode")
require(kernel, r"floorf\(graph\[p\+1\]\+0\.5f\)",
        "CUDA/HIP graph input sentinel decode")
require(kernel, r"floorf\(graph\[p\+16\]\+0\.5f\)",
        "CUDA/HIP graph texture sentinel decode")
require(kernel, r"float context\[4\]", "CUDA/HIP MaterialX context ABI")
require(kernel, r"graphSceneTime,graphSceneFrame",
        "CUDA/HIP production MaterialX time/frame transport")
require(shader, r"vec4 context;\s*// MaterialX time, frame",
        "Vulkan MaterialX context ABI")
require(shader, r"pc\.context\.x,pc\.context\.y,gr",
        "Vulkan MaterialX time/frame transport")
require(viewer_vk_shader,
        r"evalMaterialXGraphContext\([^)]*graphNormal[^)]*graphPosition[^)]*graphViewDirection",
        "lusdview Vulkan MaterialX hit context")
require(viewer_vk_shader, r"vec4 context;\s*// MaterialX time, frame",
        "lusdview Vulkan MaterialX context ABI")
require(viewer_vk_shader, r"pc\.context\.x,\s*pc\.context\.y",
        "lusdview Vulkan MaterialX time/frame transport")
require(viewer_vk_source,
        r"kMaterialBindingCount\s*=\s*34\s*\+\s*2\s*\*\s*kRasterMaterialGraphImageCount",
        "lusdview raster scene-color descriptor count")
require(viewer_vk_source, r"LUSDVIEW_RT_ALLOW_COLD_COMPILE",
        "lusdview hardware RT cold-compile opt-in")
require(viewer_vk_source,
        r"hardware RT pipeline requires blocking cold compilation",
        "lusdview non-blocking hardware RT fallback")
require(viewer_raster_shader,
        r"binding\s*=\s*48\)\s*uniform\s+sampler2D\s+uOpaqueSceneColor",
        "lusdview weighted-OIT opaque scene color")
require(viewer_raster_shader, r"screenRefractionUv",
        "lusdview screen-space refraction projection")
require(viewer_raster_shader, r"blockerDepth",
        "lusdview high-quality PCSS blocker search")
require(viewer_raster_shader, r"opaqueCoverage\s*=\s*0\.5",
        "lusdview mixed opacity-atlas coverage split")
require(viewer_environment_shader,
        r"binding\s*=\s*49\)\s*uniform\s+samplerCube\s+uEnvironmentMap",
        "lusdview raster environment background")
for rt_shader, label in ((viewer_vk_shader, "hardware ray query"),
                         (viewer_swrt_shader, "compute BVH")):
    require(rt_shader, r"dielectricBlend",
            f"lusdview {label} PreviewSurface dielectric classification")
    require(rt_shader, r"transmission\s*=\s*max\([^;]*1\.0\s*-\s*resolvedOpacity",
            f"lusdview {label} opacity-derived transmission")
    require(rt_shader, r"Visibility\(",
            f"lusdview {label} transparent shadow visibility")
for op in (44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
           58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
           72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85,
           92, 93, 94, 95, 96, 97, 98, 99, 100, 106, 107, 108, 109,
           114, 115, 116, 117, 118, 119, 120, 121):
    require(viewer_vk_shader, rf"op\s*==\s*{op}\b",
            f"lusdview Vulkan graph op {op}")
require(viewer_vk_shader, r"op\s*>=\s*86\s*&&\s*op\s*<=\s*91",
        "lusdview Vulkan compositing op range")
require(viewer_vk_shader, r"op\s*>=\s*101\s*&&\s*op\s*<=\s*105",
        "lusdview Vulkan tiled-pattern op range")
require(viewer_vk_shader, r"op\s*>=\s*110\s*&&\s*op\s*<=\s*113",
        "lusdview Vulkan matrix op range")
for offset, label in (("p \\+ 1", "input"), ("p \\+ 16", "texture")):
    require(cpu, rf"floor\(\s*scene\.matGraph\[{offset}\] \+ 0\.5f\)",
            f"CPU graph {label} sentinel decode")
for operation in ("Arcsine", "Arccosine", "Arctangent", "Contrast", "Screen",
                  "Overlay", "Burn", "Dodge", "RampLR", "RampTB", "SplitLR",
                  "SplitTB", "Saturate", "IfGreater", "IfGreaterEqual",
                  "IfEqual", "RgbToHsv", "HsvToRgb", "Rotate2D", "Distance",
                  "Reflect", "Refract", "Premult", "Unpremult", "MinComponent",
                  "MaxComponent", "LogicalAnd", "LogicalOr", "LogicalXor",
                  "LogicalNot", "Inside", "Outside", "GeomColor", "Bitangent",
                  "Difference", "In", "Mask", "Matte", "Out", "Over",
                  "DisjointOver", "SetAlpha", "CellNoise2D", "CellNoise3D",
                  "Fractal2D", "WorleyNoise2D", "WorleyNoise3D", "Fractal3D",
                  "Cloverleaf", "Hexagon",
                  "Grid", "Crosshatch", "TiledCircles", "TiledCloverleafs",
                  "TiledHexagons",
                  "RampCoordinate", "Ramp", "RampGradient", "Flake",
                  "MatrixTransform", "MatrixTranspose", "MatrixInverse",
                  "MatrixDeterminant",
                  "Swizzle"):
    require(cpu, rf"MaterialXGraphOpCPU::{operation}\b",
            f"CPU graph {operation} parity")
require(cpu,
        r"for\s*\(int lane = 0; lane < vectorComponents; \+\+lane\)\s*d \+= a\[lane\] \* b\[lane\]",
        "CPU graph dot component width")
for label, graph_shader in (
    ("Vulkan RT", viewer_vk_shader),
    ("Vulkan software RT", viewer_swrt_shader),
    ("Vulkan raster", viewer_raster_shader),
):
    require(graph_shader,
            r"op\s*==\s*8\)\s*v\[i\]\s*=\s*mix\(av,\s*bv,\s*cv\)",
            f"{label} MaterialX mix extrapolation")
    require(graph_shader,
            r"op\s*==\s*10\)\s*\{\s*int width = clamp\(int\(floor\([^)]*p \+ 3u\][^)]*\)\), 1, 4\);\s*float d = width == 1 \? av\.x \* bv\.x\s*: width == 2 \? dot\(av\.xy, bv\.xy\)\s*: width == 3 \? dot\(av\.xyz, bv\.xyz\)\s*: dot\(av, bv\)",
            f"{label} MaterialX dot component width")
if "r<44" in shader:
    raise SystemExit("GPU material ABI mismatch: stale Vulkan graph route count")
require(vk_header, r"LRT_VK_MATERIAL_PATH_LIGHT_FLOATS\s+16u", "LightRT host light stride")
require(vk_source, r"nlights\s*\*\s*LRT_VK_MATERIAL_PATH_LIGHT_FLOATS\s*\*\s*sizeof\(float\)", "LightRT light upload")

print("GPU material ABI: host, CUDA/HIP, and Vulkan layouts agree")
