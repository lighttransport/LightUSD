#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real GPU volume regression. Run under Xvfb with NVIDIA offload enabled.

Usage: check-level-set-gpu.py VIEWER sphere.vdb OUTPUT_DIR [gl vk cuda optix]
The VDB is produced by tests/volumetric/profile-gpu-level-set.c (grid sdf).
Each renderer runs separately with an 8 GiB planning cap. The runner monitors
aggregate NVIDIA memory and terminates only its renderer if usage rises by
more than 9 GiB or leaves less than 512 MiB free. It never touches other jobs.
"""
import collections
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def memory():
    result = subprocess.run(['nvidia-smi', '--id=0',
        '--query-gpu=memory.used,memory.total', '--format=csv,noheader,nounits'],
        capture_output=True, text=True, check=True)
    return tuple(int(v.strip()) for v in result.stdout.strip().split(','))


def ppm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip() == b'P6'
        line = stream.readline()
        while line.startswith(b'#'):
            line = stream.readline()
        width, height = map(int, line.split())
        assert stream.readline().strip() == b'255'
        data = stream.read()
    assert len(data) == width * height * 3
    return width, height, data


def scene(asset, present=True, foreground=False, scaled=False):
    transform = '''float3 xformOp:scale = (1.2, 0.7, 1)
        float3 xformOp:rotateXYZ = (0, 25, 15)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ", "xformOp:scale"]''' if scaled else ''
    field = f'''def Volume "Volume" {{
      {transform}
      rel field:density = </World/Volume/Field>
      def OpenVDBAsset "Field" {{
        asset filePath = @{asset}@
        token fieldName = "sdf"
        token fieldClass = "levelSet"
      }}
    }}''' if present else ''
    occluder = '''def Mesh "Occluder" {
      point3f[] points = [(-0.4,-2,1.2),(0.4,-2,1.2),(0.4,2,1.2),(-0.4,2,1.2)]
      int[] faceVertexCounts = [4]
      int[] faceVertexIndices = [0,1,2,3]
      color3f[] primvars:displayColor = [(0.8,0.1,0.1)]
      uniform token subdivisionScheme = "none"
    }''' if foreground else ''
    # These meshes keep the auto-fit bounds identical, including without a field.
    return f'''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {{
    def Mesh "Backdrop" {{
      point3f[] points = [(-2,-2,-2),(2,-2,-2),(2,2,-2),(-2,2,-2),
                         (1.99,1.99,2),(2,1.99,2),(2,2,2)]
      int[] faceVertexCounts = [4,3]
      int[] faceVertexIndices = [0,1,2,3,4,5,6]
      color3f[] primvars:displayColor = [(0.1,0.15,0.2)]
      uniform token subdivisionScheme = "none"
    }}
    {field}
    {occluder}
}}
'''


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    viewer, asset, output = map(lambda s: Path(s).resolve(), sys.argv[1:4])
    output.mkdir(parents=True, exist_ok=True)
    backends = sys.argv[4:] or ['gl', 'vk', 'cuda', 'optix']
    config = output / 'config.json'
    config.write_text('{}\n')
    results = {}
    masks = {}
    for backend in backends:
        if backend not in ('gl', 'vk', 'cuda', 'optix'):
            raise ValueError(backend)
        images = {}
        for name, present, foreground, scaled in [
                ('empty', False, False, False), ('sphere', True, False, False),
                ('scaled', True, False, True), ('occluded', True, True, False),
                ('occluder_only', False, True, False)]:
            source = output / f'{backend}-{name}.usda'
            screenshot = output / f'{backend}-{name}.ppm'
            source.write_text(scene(asset.as_posix(), present, foreground, scaled))
            report = output / f'{backend}-{name}.json'
            args = [str(viewer), '--config', str(config), '--legacy-load', '--frames', '2',
                    '--no-grid', '--render-report', str(report),
                    '--size', '128x128', '--no-robust-frame', '--view-dir', '0,0,-1',
                    '--vram-budget', '8', '--max-gpu-mem', '8',
                    '--screenshot', str(screenshot), '--backend',
                    'vk' if backend == 'vk' else 'gl']
            if backend != 'gl':
                args += ['--headless']
            if backend == 'vk':
                args += ['--vk-device', 'nvidia']
            if backend in ('cuda', 'optix'):
                args += ['--cuda', '--cuda-rt-backend',
                         'optix' if backend == 'optix' else 'software']
            args.append(str(source))
            environment = os.environ.copy()
            # The GL and CUDA runs do not need Vulkan validation layers.
            if backend != 'vk':
                environment.pop('VK_INSTANCE_LAYERS', None)
            screenshot.unlink(missing_ok=True)
            report.unlink(missing_ok=True)
            baseline, total = memory()
            maximum = baseline
            log = output / f'{backend}-{name}.log'
            with log.open('w') as stream:
                process = subprocess.Popen(args, stdout=stream, stderr=stream,
                    env=environment, cwd=output, start_new_session=True)
                start = time.monotonic()
                while process.poll() is None:
                    used, total = memory()
                    maximum = max(maximum, used)
                    if used > baseline + 9 * 1024 or used > total - 512 or time.monotonic() - start > 180:
                        os.killpg(process.pid, signal.SIGTERM)
                        process.wait(timeout=15)
                        raise RuntimeError(f'{backend}/{name}: memory or time limit; see {log}')
                    time.sleep(0.2)
                if process.returncode:
                    raise RuntimeError(f'{backend}/{name}: exit {process.returncode}; see {log}')
            text = log.read_text()
            if 'Validation Error' in text or 'VUID-' in text:
                raise RuntimeError(f'{backend}/{name}: Vulkan validation errors; see {log}')
            info = json.loads(report.read_text())['backend']
            active = info['name'].lower()
            expected = 'cuda' if backend in ('cuda', 'optix') else ('vulkan' if backend == 'vk' else 'opengl')
            if not active.startswith(expected) or 'nvidia' not in info['device'].lower():
                raise RuntimeError(f'{backend}/{name}: unexpected backend {info}')
            if backend in ('cuda', 'optix'):
                expected_transport = 'optix' if backend == 'optix' else 'software-bvh'
                assert info.get('transport') == expected_transport, info
            width, height, pixels = ppm(screenshot)
            assert (width, height) == (128, 128)
            images[name] = pixels
            results[f'{backend}/{name}'] = {'peak_total_vram_mib': maximum,
                'peak_additional_vram_mib': maximum - baseline, 'backend': info}
        def changed(a, b):
            return {i for i in range(128*128)
                    if max(abs(a[3*i+c]-b[3*i+c]) for c in range(3)) > 10}
        sphere = changed(images['sphere'], images['empty'])
        scaled = changed(images['scaled'], images['empty'])
        occluded = changed(images['occluded'], images['occluder_only'])
        assert len(sphere) > 150, (backend, 'missing isosurface', len(sphere))
        assert len(sphere ^ scaled) > 40, (backend, 'transform ignored')
        assert len(occluded) < len(sphere) * 0.9, (backend, 'foreground did not occlude')
        masks[backend] = sphere
        results[backend] = {'surface_pixels': len(sphere), 'scaled_pixels': len(scaled),
                            'visible_with_occluder': len(occluded)}
        print(backend, results[backend], flush=True)
    if 'gl' in masks and 'vk' in masks:
        iou = len(masks['gl'] & masks['vk']) / len(masks['gl'] | masks['vk'])
        # Color-difference masks also contain antialiasing/helper-line pixels.
        # Require geometric agreement within one pixel in BOTH directions.
        def nearby(source, target):
            coordinates = {(i % 128, i // 128) for i in target}
            matched = sum(any((i % 128 + dx, i // 128 + dy) in coordinates
                              for dx in (-1, 0, 1) for dy in (-1, 0, 1))
                          for i in source)
            return matched / len(source)
        agreement = min(nearby(masks['gl'], masks['vk']),
                        nearby(masks['vk'], masks['gl']))
        results['gl_vk_raw_mask_iou'] = iou
        results['gl_vk_one_pixel_agreement'] = agreement
        assert agreement > 0.99, ('GL/Vulkan silhouette mismatch', agreement)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('GPU_LEVEL_SET_PASS', flush=True)


if __name__ == '__main__':
    main()
