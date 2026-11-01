#!/usr/bin/env python3
"""A static vertex specialization must retain wireframe's color-write mask."""
import os
from pathlib import Path
import subprocess
import sys


def read_ppm(path):
    with path.open('rb') as f:
        if f.readline().strip() != b'P6':
            raise ValueError('expected binary PPM')
        size = f.readline()
        while size.startswith(b'#'):
            size = f.readline()
        width, height = map(int, size.split())
        if f.readline().strip() != b'255':
            raise ValueError('expected 8-bit PPM')
        pixels = f.read()
    if len(pixels) != width * height * 3:
        raise ValueError('incomplete PPM')
    return width, height, pixels


def main():
    binary, directory = sys.argv[1:3]
    out = Path(directory)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'config.json').write_text('{}\n')
    scene = out / 'quad.usda'
    scene.write_text('''#usda 1.0
(
    upAxis = "Y"
)
def Mesh "Quad" {
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    uniform token subdivisionScheme = "none"
    uniform bool doubleSided = true
}
def Camera "Cam" {
    float focalLength = 50
    float horizontalAperture = 40
    float verticalAperture = 40
    double3 xformOp:translate = (0,0,5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
''')
    results = {}
    for mode in ('shaded', 'wireframe'):
        image = out / (mode + '.ppm')
        image.unlink(missing_ok=True)
        cmd = [binary, '--headless', '--backend', 'vk', '--config',
               str(out / 'config.json'), '--size', '128x128', '--frames', '3',
               '--no-grid', '--no-skeleton', '--camera', 'Cam', '--mode', mode,
               '--screenshot', str(image), str(scene)]
        device = os.environ.get('LUSDVIEW_VK_DEVICE')
        if device:
            cmd[1:1] = ['--vk-device', device]
        run = subprocess.run(cmd, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=90)
        log = run.stdout.decode(errors='replace')
        (out / (mode + '.log')).write_text(log)
        if 'caps: v1 backend=vulkan' not in log:
            print('SKIP: Vulkan renderer unavailable')
            return 77
        if run.returncode or not image.exists():
            print(log)
            return 1
        w, h, pixels = read_ppm(image)
        if (w, h) != (128, 128):
            raise ValueError('unexpected viewport size')
        def pixel(x, y):
            start = (y * w + x) * 3
            return pixels[start:start + 3]
        # Interior of the quad, away from authored perimeter/axis helpers.
        results[mode] = sum(abs(a-b) for a, b in
                            zip(pixel(74, 74), pixel(8, 8)))
    if results['shaded'] < 60 or results['wireframe'] > 6:
        print('FAIL: interior/background channel distance:', results)
        return 1
    print('PASS: shaded quad visible; wire depth fill leaves background intact')
    return 0


if __name__ == '__main__':
    sys.exit(main())
