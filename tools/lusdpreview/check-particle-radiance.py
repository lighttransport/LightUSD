#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Converted ParticleField colors must not be relit by extreme scene lights."""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageChops, ImageStat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--viewer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, __NV_PRIME_RENDER_OFFLOAD='1',
               __GLX_VENDOR_LIBRARY_NAME='nvidia',
               __EGL_VENDOR_LIBRARY_FILENAMES='/usr/share/glvnd/egl_vendor.d/10_nvidia.json')
    images = []
    for power in (1, 1000000):
        source = args.output / f'source-{power}.usda'
        source.write_text('#usda 1.0\ndef Cube "Cube" { double size=4 rel material:binding=</Material> }\n'
                          'def Material "Material" { token outputs:surface.connect=</Material/Surface.outputs:surface> '
                          'def Shader "Surface" { uniform token info:id="UsdPreviewSurface" '
                          'color3f inputs:diffuseColor=(0.02,0.05,0.1) } }\n'
                          f'def DistantLight "Sun" {{ float inputs:intensity={power} }}\n')
        capture = args.output / f'radiance-{power}.ppm'
        capture.unlink(missing_ok=True)
        command = [str(args.viewer.resolve()), str(source), '--scene-preview',
                   '--preview-kind', 'points', '--preview-no-source-refine',
                   '--preview-cache-dir', str(args.output / 'cache'),
                   '--preview-resolution', '16', '--headless', '--no-grid',
                   '--backend', 'vk', '--vk-device', 'nvidia', '--frames', '1',
                   '--size', '160x120', '--screenshot', str(capture)]
        with (args.output / f'radiance-{power}.log').open('w') as log:
            subprocess.run(command, env=env, stdout=log, stderr=log, check=True, timeout=120)
        log = (args.output / f'radiance-{power}.log').read_text()
        if 'VUID-' in log or 'Validation Error' in log:
            raise RuntimeError('Vulkan validation error')
        image = Image.open(capture).convert('RGB')
        if max(ImageStat.Stat(image).var) < 1:
            raise RuntimeError('blank ParticleField radiance image')
        pixels = image.tobytes()
        if sum(pixels[i+2] > pixels[i] + 15 for i in range(0, len(pixels), 3)) < 100:
            raise RuntimeError('particle color was saturated or lost (check exposure UBO slot)')
        images.append(image)
    if ImageChops.difference(*images).getbbox():
        raise RuntimeError('scene lighting changed converted point colors')
    print('PASS: Converted point shading is invariant under a millionfold light change')


if __name__ == '__main__':
    main()
