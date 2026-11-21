#!/usr/bin/env python3
"""CPU-preview regression for UsdPreviewSurface transparent vs presence opacity.

`opacityMode="transparent"` (the schema default) attenuates only the
diffuse/subsurface lobes and keeps the surface opaque; `"presence"` is coverage,
so it also attenuates the specular response. The probe renders a diffuse quad and
a metal quad side by side at opacity 0.25 and 1.0 and compares the per-quad
response ratio:

  transparent: metal ratio ~1.0 (specular untouched), diffuse ratio < 0.75
  presence:    metal ratio < 0.75 (coverage attenuates the whole shade)

Before the transparent-opacity fix the CPU preview used the see-through
continuation for both modes, so the metal ratio collapsed to the diffuse ratio
and the transparent assertion failed.
"""
import argparse
from pathlib import Path
import subprocess

from check_lusdrender_smoke import read_png_rgba

TEMPLATE = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "Diffuse" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        point3f[] points = [(-1.6,-0.9,0),(-0.2,-0.9,0),(-0.2,0.9,0),(-1.6,0.9,0)]
        rel material:binding = </World/MatDiffuse>
    }
    def Mesh "Metal" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        point3f[] points = [(0.2,-0.9,0),(1.6,-0.9,0),(1.6,0.9,0),(0.2,0.9,0)]
        rel material:binding = </World/MatMetal>
    }
    def Material "MatDiffuse"
    {
        token outputs:surface.connect = </World/MatDiffuse/P.outputs:surface>
        def Shader "P"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.8,0.8,0.8)
            float inputs:metallic = 0
            float inputs:roughness = 0.5
            float inputs:opacity = __OPACITY__
            token inputs:opacityMode = "__MODE__"
            token outputs:surface
        }
    }
    def Material "MatMetal"
    {
        token outputs:surface.connect = </World/MatMetal/P.outputs:surface>
        def Shader "P"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.8,0.8,0.8)
            float inputs:metallic = 1
            float inputs:roughness = 0.3
            float inputs:opacity = __OPACITY__
            token inputs:opacityMode = "__MODE__"
            token outputs:surface
        }
    }
    def DistantLight "Key"
    {
        float inputs:intensity = 3
        float3 xformOp:rotateXYZ = (-35, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def Camera "Cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 27
        float2 clippingRange = (0.01, 1000)
        double3 xformOp:translate = (0, 0, 6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
'''


def scene(opacity, mode):
    return (TEMPLATE.replace('__OPACITY__', repr(float(opacity)))
            .replace('__MODE__', mode))


def luminance(path):
    width, height, rgba = read_png_rgba(path)
    out = []
    for i in range(width * height):
        r, g, b = rgba[i * 4:i * 4 + 3]
        out.append(0.2126 * r + 0.7152 * g + 0.0722 * b)
    return width, height, out


def half_ratio(low, high, width, height, x0, x1):
    """Mean low/high over pixels lit in both renders (excludes the background)."""
    low_sum = high_sum = 0.0
    count = 0
    for y in range(height):
        for x in range(x0, x1):
            i = y * width + x
            if low[i] > 4.0 and high[i] > 4.0:
                low_sum += low[i]
                high_sum += high[i]
                count += 1
    if count < 100:
        raise AssertionError('transparent-opacity probe rendered too few lit pixels')
    return low_sum / high_sum, count


def render(renderer, path, output):
    cmd = [str(renderer), str(path), str(output),
           '-camera', '/World/Cam', '-w', '160', '-height', '120',
           '-timecode', '0', '-purpose', 'default,render', '-bg', '0,0,0',
           '-smooth', '-rtPreview', '-materialShading', 'lightrt-bsdf',
           '-ambient', '0']
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def measure(renderer, root, mode):
    low_scene = root / (mode + '-025.usda')
    high_scene = root / (mode + '-100.usda')
    low_scene.write_text(scene(0.25, mode))
    high_scene.write_text(scene(1.0, mode))
    low_png = root / (mode + '-025.png')
    high_png = root / (mode + '-100.png')
    render(renderer, low_scene, low_png)
    render(renderer, high_scene, high_png)
    wl, hl, low = luminance(low_png)
    wh, hh, high = luminance(high_png)
    if (wl, hl) != (wh, hh):
        raise AssertionError('probe image dimensions differ')
    diffuse, _ = half_ratio(low, high, wl, hl, 0, wl // 2)
    metal, _ = half_ratio(low, high, wl, hl, wl // 2, wl)
    return diffuse, metal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.renderer = args.renderer.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)

    diffuse_t, metal_t = measure(args.renderer, args.output, 'transparent')
    diffuse_p, metal_p = measure(args.renderer, args.output, 'presence')
    print('transparent: diffuse ratio %.3f, metal ratio %.3f'
          % (diffuse_t, metal_t))
    print('presence:    diffuse ratio %.3f, metal ratio %.3f'
          % (diffuse_p, metal_p))

    assert metal_t >= 0.98, 'transparent opacity must not attenuate the specular lobe'
    assert diffuse_t <= 0.75, 'transparent opacity must attenuate the diffuse lobe'
    assert metal_t - diffuse_t >= 0.20, 'transparent opacity did not split diff/spec'
    assert metal_p <= 0.75, 'presence opacity must attenuate the whole shade'
    print('PASS: CPU preview honours UsdPreviewSurface transparent vs presence opacity')


if __name__ == '__main__':
    main()
