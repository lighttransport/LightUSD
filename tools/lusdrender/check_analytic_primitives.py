#!/usr/bin/env python3
"""Regression for analytic geometry in next-loader CPU and GPU render paths."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess

from check_lusdrender_smoke import read_png_rgba, write_png_rgba

CAMERA = '''
def Camera "Camera" {
 float horizontalAperture = 30
 float verticalAperture = 30
 float focalLength = 30
 double3 xformOp:translate = (0,0,8)
 uniform token[] xformOpOrder = ["xformOp:translate"]
}
'''
HEADER = '#usda 1.0\n(upAxis = "Y")\n'
BACKENDS = {'cpu': '-rtPreview', 'vk': '-vk', 'vkr': '-vkr'}


def pixels(path):
    width, height, rgba = read_png_rgba(path)
    foreground = [(i % width, i // width, tuple(rgba[i * 4:i * 4 + 3]))
                  for i in range(width * height) if sum(rgba[i * 4:i * 4 + 3]) > 15]
    if len(foreground) < 20:
        raise AssertionError('empty analytic render: ' + str(path))
    return width, height, rgba, foreground


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--backends', default='cpu')
    parser.add_argument('--require-hardware', action='store_true')
    args = parser.parse_args()
    args.renderer = args.renderer.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    backends = args.backends.split(',')
    if any(b not in BACKENDS for b in backends):
        parser.error('backends must be cpu, vk, or vkr')

    def scene(name, body, camera=True, metadata=''):
        path = args.output / (name + '.usda')
        header = HEADER if not metadata else '#usda 1.0\n(upAxis = \"Y\"\n ' + metadata + ')\n'
        path.write_text(header + body + (CAMERA if camera else ''))
        return path

    cube = scene('cube-default', 'def Cube "Shape" {}\n')
    cube_explicit = scene('cube-explicit', 'def Cube "Shape" {\n double size = 2\n}\n')
    sphere = scene('sphere-default', 'def Sphere "Shape" {}\n')
    sphere_explicit = scene('sphere-explicit', 'def Sphere "Shape" {\n double radius = 1\n}\n')
    payload_source = scene('payload-source', '''
def Xform "World" {
 def Cube "Shape" {
  double3 xformOp:translate = (1,0,0)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  rel material:binding = </World/Red>
 }
 def Material "Red" {
  token outputs:surface.connect = </World/Red/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0.85,0.1,0.05)
   token outputs:surface
  }
 }
}
''', camera=False, metadata='defaultPrim = "World"')
    payload = scene('payload', 'def Xform "World" (\n prepend payload = @payload-source.usda@\n) {}\n')
    prototype = scene('sphere-prototype', 'def Sphere "Shape" {\n double radius = 0.6\n}\n', camera=False, metadata='defaultPrim = "Shape"')
    instances = scene('instances', '''
def Sphere "Left" (
 prepend references = @sphere-prototype.usda@
 instanceable = true
) {
 double3 xformOp:translate = (-2,0,0)
 uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Sphere "Right" (
 prepend references = @sphere-prototype.usda@
 instanceable = true
) {
 double3 xformOp:translate = (2,0,0)
 uniform token[] xformOpOrder = ["xformOp:translate"]
}
''')
    instancer = scene('point-instancer', '''
def PointInstancer "Instancer" {
 point3f[] positions = [(-2,0,0),(2,0,0)]
 int[] protoIndices = [0,0]
 rel prototypes = [</Instancer/Proto>]
 def Cube "Proto" {
  double size = 1.2
 }
}
''')
    masked = scene('mask-visibility-purpose', '''
def Xform "World" {
 def Sphere "Shape" {}
 def Cube "Hidden" {
  token visibility = "invisible"
  double size = 5
 }
 def Cube "Guide" {
  uniform token purpose = "guide"
  double size = 5
 }
}
''')
    animated = scene('animated', '''
def Sphere "Shape" {
 double radius.timeSamples = {0: 0.35, 1: 1}
}
''')
    rgba = bytearray()
    for y in range(8):
        for x in range(8):
            rgba.extend(((255,0,0,255) if x < 4 else (0,255,0,255)) if y < 4 else
                        ((0,0,255,255) if x < 4 else (255,255,0,255)))
    write_png_rgba(args.output / 'quadrants.png', 8, 8, rgba)
    textured = scene('textured-cube', '''
def Cube "Shape" {
 rel material:binding = </Material>
}
def Material "Material" {
 token outputs:surface.connect = </Material/Surface.outputs:surface>
 def Shader "Surface" {
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor.connect = </Material/Texture.outputs:rgb>
  token outputs:surface
 }
 def Shader "Texture" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @quadrants.png@
  float2 inputs:st.connect = </Material/UV.outputs:result>
  float3 outputs:rgb
 }
 def Shader "UV" {
  uniform token info:id = "UsdPrimvarReader_float2"
  token inputs:varname = "st"
  float2 outputs:result
 }
}
''')

    def render(backend, path, tag, extra=()):
        output = args.output / (backend + '-' + tag + '.png')
        command = [str(args.renderer), str(path), str(output), BACKENDS[backend], '-stats',
                   '-camera', '/Camera', '-w', '96', '-height', '96', '-smooth',
                   '-ambient', '0.2', '-noDisplace', *extra]
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise AssertionError('analytic renderer timed out: ' + tag)
        output.with_suffix('.log').write_text(stdout + stderr)
        if proc.returncode:
            raise AssertionError('analytic render failed: %s/%s\n%s' % (backend, tag, stderr))
        if backend != 'cpu' and args.require_hardware:
            if not re.search(r'Vulkan device:.*(?:AMD|NVIDIA|Intel|Radeon)', stderr, re.I) or re.search(r'llvmpipe|lavapipe|fallback', stderr, re.I):
                raise AssertionError('hardware device not verified: ' + stderr)
            if backend == 'vkr' and 'ray_query' not in stderr:
                raise AssertionError('hardware ray query not verified')
        if '-frames' not in extra:
            match = re.search(r'^triangles:\s*(\d+)', stderr, re.M)
            if match is None:
                match = re.search(r'^instanced: \d+ prototypes \((\d+) unique tris\)', stderr, re.M)
            if not match or int(match[1]) <= 0:
                raise AssertionError('no generated triangles: ' + stderr)
            pixels(output)
        return output, stderr

    def assert_two_placements(output):
        w, _, _, fg = pixels(output)
        left = sum(x < w * 0.4 for x, _, _ in fg)
        right = sum(x > w * 0.6 for x, _, _ in fg)
        middle = sum(w * 0.4 <= x <= w * 0.6 for x, _, _ in fg)
        if min(left, right) < 20 or middle:
            raise AssertionError('analytic prototype placement lost: ' + str(output))

    for backend in backends:
        a, cube_log = render(backend, cube, 'cube')
        assert int(re.search(r'^triangles:\s*(\d+)', cube_log, re.M)[1]) == 12
        b, _ = render(backend, cube_explicit, 'cube-explicit')
        assert pixels(a)[2] == pixels(b)[2], 'Cube schema default size changed'
        a, _ = render(backend, sphere, 'sphere')
        b, _ = render(backend, sphere_explicit, 'sphere-explicit')
        assert pixels(a)[2] == pixels(b)[2], 'Sphere schema default radius changed'
        sphere_count = len(pixels(a)[3])
        p, _ = render(backend, payload, 'payload')
        w, _, _, fg = pixels(p)
        assert sum(x for x, _, _ in fg) / len(fg) > w / 2 + 4, 'payload transform lost'
        assert sum(rgb[0] for _, _, rgb in fg) > sum(rgb[1] for _, _, rgb in fg) * 2, 'bound material lost'
        for path in (instances, instancer):
            p, _ = render(backend, path, path.stem)
            assert_two_placements(p)
        if backend == 'vkr':
            p, _ = render(backend, instances, 'two-level-instances', ['-vkInstanced'])
            assert_two_placements(p)
        p, _ = render(backend, masked, 'mask', ['-mask', '/World/Shape'])
        assert len(pixels(p)[3]) == sphere_count, 'mask failed for analytic geometry'
        p, _ = render(backend, masked, 'purpose-visibility', ['-purpose', 'default,render'])
        assert len(pixels(p)[3]) == sphere_count, 'hidden/guide analytic geometry leaked'
        p, _ = render(backend, textured, 'texture')
        fg = pixels(p)[3]
        # The descriptor ray-query shader currently omits PreviewSurface texture
        # routes even for authored Meshes; test UV sampling on CPU/compute.
        for channel in range(3) if backend != 'vkr' else ():
            count = sum(rgb[channel] > max(rgb[(channel + 1) % 3], rgb[(channel + 2) % 3]) * 1.5 for _, _, rgb in fg)
            assert count > 5, 'generated UVs collapsed: channel %d on %s' % (channel, backend)
        if backend == 'cpu':
            _, log = render(backend, animated, 'anim.####', ['-frames', '0:1'])
            assert 'geometry animated: 1' in log, 'animated dimensions reused a static BVH'
            small = pixels(args.output / 'cpu-anim.0000.png')[3]
            big = pixels(args.output / 'cpu-anim.0001.png')[3]
        else:
            small = pixels(render(backend, animated, 'anim0', ['-timecode', '0'])[0])[3]
            big = pixels(render(backend, animated, 'anim1', ['-timecode', '1'])[0])[3]
        assert len(big) > len(small) * 3, 'animated primitive radius was ignored'
        print('PASS analytic primitives:', backend, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
