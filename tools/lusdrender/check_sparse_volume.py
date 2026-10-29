#!/usr/bin/env python3
"""CPU sparse-level-set image regression; requires TINYVDBIO_DATA_DIR."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from check_lusdrender_smoke import read_png_rgba


def scene(asset, translation, present, scale=1, foreground=False, rotation=0, xscale=1):
    target = 'sdf' if present else 'missing'
    plane = '''def Mesh "Foreground" {
        point3f[] points = [(-400, -400, 100), (400, -400, 100),
                           (400, 400, 100), (-400, 400, 100)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
    }''' if foreground else ''
    return f'''#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World" {{
    def Volume "Volume" {{
        rel field:sdf = </World/{target}>
    }}
    def OpenVDBAsset "sdf" {{
        asset filePath = @{asset}@
        token fieldName = "ls_utahteapot"
        token fieldClass = "levelSet"
        double3 xformOp:translate = ({translation}, 0, 0)
        double3 xformOp:rotateXYZ = (0, 0, {rotation})
        float3 xformOp:scale = ({scale * xscale}, {scale}, {scale})
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]
    }}
    {plane}
    def Camera "Camera" {{
        double3 xformOp:translate = (0, 0, {150 * scale})
        uniform token[] xformOpOrder = ["xformOp:translate"]
        float focalLength = 30
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (0.01, 1000)
    }}
}}
'''


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--with-qjs'):
        raise SystemExit('usage: check_sparse_volume.py /path/to/lusdrender [--with-qjs]')
    with_qjs = len(sys.argv) == 3
    root = os.environ.get('TINYVDBIO_DATA_DIR')
    asset = Path(root or '') / 'utahteapot.vdb'
    if not root or not asset.is_file():
        print('SKIP: TINYVDBIO_DATA_DIR/utahteapot.vdb unavailable')
        return 77
    renderer = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='lightusd-sparse-render-') as directory:
        tmp = Path(directory)

        def render(name, translation, present, threads, scale=1, foreground=False, rotation=0,
                   xscale=1):
            source = tmp / (name + '.usda')
            output = tmp / (name + '.png')
            source.write_text(scene(asset.resolve().as_posix(), translation, present,
                                    scale, foreground, rotation, xscale))
            run = subprocess.run([renderer, str(source), str(output), '-rtPreview',
                                  '-camera', '/World/Camera', '-w', '64', '-height', '64',
                                  '-threads', str(threads), '-noShadows', '-maxMem', '1'],
                                 capture_output=True, text=True, timeout=120)
            if run.returncode:
                raise RuntimeError(f'{name} failed: {run.stderr}')
            if present and 'using sparse CPU level set' not in run.stderr:
                raise RuntimeError(f'{name} did not use sparse rendering: {run.stderr}')
            width, height, pixels = read_png_rgba(output)
            if (width, height) != (64, 64):
                raise RuntimeError(f'{name}: unexpected image size')
            return pixels

        baseline = render('empty', 0, False, 1)
        image = render('volume', 0, True, 1)
        threaded = render('threaded', 0, True, 2)
        translated = render('translated', 10, True, 1)
        scaled = render('scaled', 0, True, 1, scale=2)
        rotated = render('rotated', 0, True, 1, rotation=180)
        stretched = render('stretched', 0, True, 1, xscale=1.5)
        foreground = render('foreground', 0, False, 1, scale=2, foreground=True)
        occluded = render('occluded', 0, True, 1, scale=2, foreground=True)
        limited = subprocess.run([renderer, str(tmp / 'volume.usda'), str(tmp / 'limited.png'),
                                  '-rtPreview', '-camera', '/World/Camera', '-w', '32',
                                  '-height', '32', '-threads', '1', '-noShadows',
                                  '-maxMem', '0.15'], capture_output=True, text=True, timeout=120)
        if limited.returncode or 'exceeds the memory budget' not in limited.stderr:
            raise RuntimeError(f'low budget was not reported: {limited.stderr}')
        # Distinct paths to the same file force two cache entries. Each field
        # fits by itself but both must share the scene-wide cache allowance.
        duplicate = tmp / 'duplicate.vdb'
        shutil.copyfile(asset, duplicate)
        two_fields = scene(asset.resolve().as_posix(), 0, True).replace(
            '    def Camera "Camera"', f'''    def Volume "Second" {{
        rel field:sdf = </World/secondSdf>
    }}
    def OpenVDBAsset "secondSdf" {{
        asset filePath = @{duplicate.as_posix()}@
        token fieldName = "ls_utahteapot"
        token fieldClass = "levelSet"
    }}
    def Camera "Camera"''')
        (tmp / 'two-fields.usda').write_text(two_fields)
        aggregate = subprocess.run([renderer, str(tmp / 'two-fields.usda'),
                                    str(tmp / 'two-fields.png'), '-rtPreview',
                                    '-camera', '/World/Camera', '-w', '8', '-height', '8',
                                    '-threads', '1', '-noShadows', '-maxMem', '0.35'],
                                   capture_output=True, text=True, timeout=120)
        if (aggregate.returncode or aggregate.stderr.count('using sparse CPU level set') != 1 or
                'exceeds the memory budget' not in aggregate.stderr):
            raise RuntimeError(f'shared budget was not enforced: {aggregate.stderr}')
        icosahedron = Path(root) / 'icosahedron.vdb'
        if icosahedron.is_file():
            # The icosahedron fits the dense budget by itself. Two off-camera
            # fog fields consume enough of that budget to force it sparse.
            reference = scene(icosahedron.resolve().as_posix(), 0, True).replace(
                'token fieldName = "ls_utahteapot"', 'token fieldName = "ls_icosahedron"').replace(
                '(0, 0, 150)', '(0, 0, 20)')
            filler = f'''def Volume "A" {{
        rel field:density = </World/Fog>
        double3 xformOp:translate = (100000, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
    def Volume "AA" {{
        rel field:density = </World/Fog>
        double3 xformOp:translate = (100000, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
    def OpenVDBAsset "Fog" {{
        asset filePath = @{asset.resolve().as_posix()}@
        token fieldClass = "fogVolume"
    }}
    '''
            def render_reference(name, content, expect_sparse):
                source, output = tmp / (name + '.usda'), tmp / (name + '.png')
                source.write_text(content)
                result = subprocess.run([renderer, str(source), str(output), '-rtPreview',
                                         '-camera', '/World/Camera', '-w', '64', '-height', '64',
                                         '-threads', '1', '-noShadows', '-maxMem', '1'],
                                        capture_output=True, text=True, timeout=120)
                sparse = "volume '/World/Volume': using sparse CPU level set" in result.stderr
                if result.returncode or sparse != expect_sparse:
                    raise RuntimeError(f'{name}: unexpected render path: {result.stderr}')
                return read_png_rgba(output)[2]
            dense_ref = render_reference('dense-ref', reference, False)
            sparse_ref = render_reference('sparse-ref', reference.replace(
                '    def Volume "Volume"', '    ' + filler + 'def Volume "Volume"'), True)
            if dense_ref != sparse_ref:
                raise RuntimeError('dense and sparse icosahedron images differ')
            visible = sum(dense_ref[i:i + 3] != b'\x00\x00\x00'
                          for i in range(0, len(dense_ref), 4))
            if visible < 100:
                raise RuntimeError(f'icosahedron reference is too small: {visible} pixels')
            print('PASS: dense and sparse icosahedron images match byte-for-byte')
        else:
            print('SKIP: icosahedron.vdb unavailable for dense/sparse comparison')
        changed = sum(image[i:i + 4] != baseline[i:i + 4] for i in range(0, len(image), 4))
        shifted = sum(image[i:i + 4] != translated[i:i + 4] for i in range(0, len(image), 4))
        animated_source = tmp / 'animated.usda'
        animated_source.write_text(scene(asset.resolve().as_posix(), 0, True).replace(
            'double3 xformOp:translate = (0, 0, 0)',
            'double3 xformOp:translate.timeSamples = { 1: (0, 0, 0), 2: (10, 0, 0) }'))
        animated = subprocess.run([renderer, str(animated_source), str(tmp / 'frame.#.png'),
                                   '-rtPreview', '-camera', '/World/Camera', '-w', '64',
                                   '-height', '64', '-threads', '1', '-noShadows',
                                   '-maxMem', '1', '-frames', '1:2'],
                                  capture_output=True, text=True, timeout=120)
        if animated.returncode:
            raise RuntimeError(f'animated volume failed: {animated.stderr}')
        frame1 = read_png_rgba(tmp / 'frame.1.png')[2]
        frame2 = read_png_rgba(tmp / 'frame.2.png')[2]
        if frame1 != image or frame2 != translated:
            raise RuntimeError('animated field transforms do not match single-time renders')

        def check_scripted_frames(source, name, expected):
            script = tmp / f'{name}.js'
            outputs = [tmp / f'{name}-{frame}.png' for frame in (1, 2)]
            script.write_text('\n'.join(
                f'lusdrender.setTime({frame}); lusdrender.render({json.dumps(str(output))});'
                for frame, output in zip((1, 2), outputs)))
            result = subprocess.run([renderer, str(source), '-js', str(script), '-rtPreview',
                                     '-camera', '/World/Camera', '-w', '64', '-height', '64',
                                     '-threads', '1', '-noShadows', '-maxMem', '1'],
                                    capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(f'{name} scripted rendering failed: {result.stderr}')
            for frame, output, reference in zip((1, 2), outputs, expected):
                if read_png_rgba(output)[2] != reference:
                    raise RuntimeError(f'{name} scripted frame {frame} differs from frame render')

        if with_qjs:
            check_scripted_frames(animated_source, 'script-transform', [frame1, frame2])
        # Two coincident level sets with different colors must keep the first
        # surface at equal depth; a later field must not overwrite it.
        colored = scene(asset.resolve().as_posix(), 0, True).replace(
            'rel field:sdf = </World/sdf>',
            'rel field:sdf = </World/sdf>\n        rel material:binding = </World/Red>')
        materials = '''def Material "Red" {
        token outputs:volume.connect = </World/Red/Shader.outputs:out>
        def Shader "Shader" {
            uniform token info:id = "UsdVolumeShader"
            color3f inputs:scatter_color = (1, 0, 0)
            token outputs:out
        }
    }
    def Material "Green" {
        token outputs:volume.connect = </World/Green/Shader.outputs:out>
        def Shader "Shader" {
            uniform token info:id = "UsdVolumeShader"
            color3f inputs:scatter_color = (0, 1, 0)
            token outputs:out
        }
    }
    '''
        colored = colored.replace('    def Camera "Camera"', '    ' + materials + 'def Camera "Camera"')
        duplicate_volume = '''def Volume "Later" {
        rel field:sdf = </World/sdf>
        rel material:binding = </World/Green>
    }
    '''
        def render_colored(name, content):
            source, output = tmp / (name + '.usda'), tmp / (name + '.png')
            source.write_text(content)
            result = subprocess.run([renderer, str(source), str(output), '-rtPreview',
                                     '-camera', '/World/Camera', '-w', '64', '-height', '64',
                                     '-threads', '1', '-noShadows', '-maxMem', '1'],
                                    capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(f'{name} failed: {result.stderr}')
            return read_png_rgba(output)[2]
        red = render_colored('red', colored)
        coincident = render_colored('coincident', colored.replace(
            '    def Camera "Camera"', '    ' + duplicate_volume + 'def Camera "Camera"'))
        if red == image or coincident != red:
            raise RuntimeError('coincident level-set depth ordering failed')
        if icosahedron.is_file():
            asset_animation = scene(asset.resolve().as_posix(), 0, True).replace(
                f'asset filePath = @{asset.resolve().as_posix()}@',
                f'asset filePath.timeSamples = {{ 1: @{asset.resolve().as_posix()}@, '
                f'2: @{icosahedron.resolve().as_posix()}@ }}').replace(
                '        token fieldName = "ls_utahteapot"\n', '')
            source = tmp / 'animated-assets.usda'
            source.write_text(asset_animation)
            args = ['-rtPreview', '-camera', '/World/Camera', '-w', '64', '-height', '64',
                    '-threads', '1', '-noShadows', '-maxMem', '1']
            result = subprocess.run([renderer, str(source), str(tmp / 'assets.#.png'),
                                     *args, '-frames', '1:2'], capture_output=True, text=True,
                                    timeout=120)
            if result.returncode:
                raise RuntimeError(f'animated file paths failed: {result.stderr}')
            asset_frames = [read_png_rgba(tmp / f'assets.{i}.png')[2] for i in (1, 2)]
            if asset_frames[0] == asset_frames[1]:
                raise RuntimeError('time-sampled VDB paths produced identical frames')
            if with_qjs:
                check_scripted_frames(source, 'script-assets', asset_frames)
            for frame in (1, 2):
                output = tmp / f'asset-single-{frame}.png'
                result = subprocess.run([renderer, str(source), str(output), *args,
                                         '-timecode', str(frame)], capture_output=True,
                                        text=True, timeout=120)
                if result.returncode or read_png_rgba(output)[2] != asset_frames[frame - 1]:
                    raise RuntimeError(f'file path frame {frame} differs from single-time render: '
                                       f'{result.stderr}')
        rotation_errors = sum(image[i:i + 4] != rotated[-i - 4:len(rotated) - i]
                              for i in range(0, len(image), 4))
        def width_of_silhouette(pixels):
            xs = [i % 64 for i in range(64 * 64)
                  if pixels[4 * i:4 * i + 4] != baseline[4 * i:4 * i + 4]]
            return max(xs) - min(xs) + 1 if xs else 0
        width = width_of_silhouette(image)
        stretched_width = width_of_silhouette(stretched)
        if (changed < 10 or shifted < 10 or image != threaded or image != scaled or
                foreground != occluded or rotation_errors > 10 or stretched_width <= width):
            raise RuntimeError(f'changed={changed}, translated={shifted}, '
                               f'threads match={image == threaded}, '
                               f'scale matches={image == scaled}, '
                               f'foreground occludes={foreground == occluded}, '
                               f'rotation errors={rotation_errors}, '
                               f'widths={width}/{stretched_width}')
        print(f'PASS: {changed} volume pixels, {shifted} translated pixels; '
              f'threads, scale and foreground occlusion match; {rotation_errors} rotation errors; '
              'low and shared budgets reported')
    return 0


if __name__ == '__main__':
    sys.exit(main())
