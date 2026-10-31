#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify automatic source resampling, direct grids and render-thread reloads."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageStat


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--viewer', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--owner', choices=('vk','gl'), default='vk')
    ap.add_argument('--threaded', action='store_true', help='Run under xvfb-run with a real hardware owner')
    ap.add_argument('--software', action='store_true', help='Mesa lavapipe/llvmpipe only; skips CUDA and NVIDIA device selection')
    ap.add_argument('--threaded-frames', type=int, default=600,
                    help='Frames to render before a threaded capture; source refinement installs asynchronously')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output / 'source.usda'
    source.write_text('#usda 1.0\ndef Cube "Cube" { double size=4 }\n')
    env = dict(os.environ)
    if args.software:
        env.update(VK_DRIVER_FILES=os.environ.get('VK_DRIVER_FILES', ''),
                   LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe',
                   LIGHTUSD_PREVIEW_SOFTWARE='1')
        env.pop('__NV_PRIME_RENDER_OFFLOAD', None)
        env.pop('LUSDVIEW_VK_DEVICE', None)
        env.pop('LUSDVIEW_VK_GPU_ASSISTED', None)
    else:
        env.update(__NV_PRIME_RENDER_OFFLOAD='1',
                   __GLX_VENDOR_LIBRARY_NAME='nvidia',
                   __EGL_VENDOR_LIBRARY_FILENAMES='/usr/share/glvnd/egl_vendor.d/10_nvidia.json')
    results = []
    for kind in ('gaussian', 'points', 'volume'):
        backends = ((args.owner,) if args.threaded else
                    (('cpu', 'vk') if args.software else ('cpu', 'vk', 'cuda')))
        for backend in backends:
            reports = {}
            for refine in (False, True):
                label = f'{kind}-{backend}-{"refine" if refine else "cached"}'
                image, report = args.output / (label + '.ppm'), args.output / (label + '.json')
                image.unlink(missing_ok=True)
                report.unlink(missing_ok=True)
                cmd = [str(args.viewer.resolve()), '--no-grid', '--scene-preview',
                       '--preview-kind', kind, '--preview-resolution', '32',
                       '--preview-cache-dir', str(args.output / 'cache'),
                       '--preview-budget-mib', '2048', '--cam-dolly', '.5',
                       '--frames', str(args.threaded_frames) if args.threaded else '1',
                       '--size', '320x240',
                       '--screenshot', str(image), '--render-report', str(report)]
                cmd += ['--threaded'] if args.threaded else ['--headless']
                vk_device = 'llvmpipe' if args.software else 'nvidia'
                cmd += {'cpu': ['--cpu-rt'], 'vk': ['--backend', 'vk', '--vk-device', vk_device],
                        'cuda': ['--cuda', '--cuda-rt-backend', 'software'], 'gl': ['--backend','gl']}[backend]
                if not refine:
                    cmd += ['--preview-no-source-refine']
                cmd += [str(source)]
                with (args.output / (label + '.log')).open('w') as log:
                    subprocess.run(cmd, env=env, stdout=log, stderr=log, timeout=240, check=True)
                log = (args.output / (label + '.log')).read_text()
                if 'Validation Error' in log or 'VUID-' in log:
                    raise RuntimeError(label + ': validation failure')
                if kind == 'volume' and 'direct UsdVol grids: backend=' not in log:
                    raise RuntimeError(label + ': did not render grids directly')
                if max(ImageStat.Stat(Image.open(image).convert('RGB')).var) < 1:
                    raise RuntimeError(label + ': blank capture')
                reports[refine] = json.loads(report.read_text())
                if args.threaded and not reports[refine]["render_thread"]:
                    raise RuntimeError(label + ": render thread was not active")
            old, new = reports[False]['preview_source'], reports[True]['preview_source']
            if new['cell_size'] >= old['cell_size'] or new['samples'] <= old['samples']:
                raise RuntimeError(f'{kind}-{backend}: source detail did not increase: {old} -> {new}')
            result = dict(kind=kind, backend=backend, threaded=args.threaded, cached=old, refined=new)
            print(json.dumps(result), flush=True)
            results.append(result)
    (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
