#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Analytic oracle for the ColorSpaceTests/UsdPreviewSurface/usduvtexture_color_test asset.

The asset (usd-assets, CC-BY-ND, not copied here) is a 5x5 grid of emissive
UsdUVTexture tiles that exercise every way of assigning a color space. Its README
states the target: with the opinion that `colorSpace` (metadata, the
`inputs:file:colorSpace:name` attribute, or an applied UsdColorSpaceAPI on an
enclosing prim) takes precedence over `inputs:sourceColorSpace`, EVERY tile must
render {200, 100, 50} after the sRGB transfer function (scene-linear
{0.577580, 0.127438, 0.0318960}).

Hydra Storm is documented as incorrect for rows 3-5 and for sRGB on .hdr, so the
Storm comparison frame cannot certify color management; this check asserts the
asset's own target instead. Rendering uses `-vkr -diagnosticEmission` (unlit
emissive channel, sRGB-encoded, no tone map).

Asset location: argv[2], $LIGHTUSD_USD_ASSETS, or <repo>/../usd-assets.
Exit 77 (ctest SKIP) when the asset or a usable Vulkan renderer is missing.

usage: check_uvtexture_colorspace_tiles.py <lusdrender> [usd-assets-root] [--output DIR]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from check_colorspace_regression import read_png  # noqa: E402

SKIP = 77
TARGET = (200, 100, 50)
TOLERANCE = 3
WIDTH, HEIGHT = 800, 600
ASSET = Path('test_assets/ColorSpaceTests/UsdPreviewSurface/usduvtexture_color_test.usda')
COLUMNS = ('raw', 'sRGB', 'auto', 'omitted', 'lin_ap1_scene(invalid)')
ROWS = ('.hdr + sourceColorSpace', '.png + sourceColorSpace', 'file colorSpace metadata',
        'inputs:file:colorSpace:name', 'enclosing-prim ColorSpaceAPI')


def skip(reason):
    print(f'SKIP: {reason}')
    sys.exit(SKIP)


def tile_center(row, col):
    # Fixed by the asset's own /Camera and its 5x5 patch layout (800x600 frame).
    return int(round(WIDTH * 0.2375 + WIDTH * 0.13125 * col)), \
        int(round(HEIGHT * 0.1533 + HEIGHT * 0.1737 * row))


def tile_median(width, channels, rows, cx, cy, half=10):
    samples = [[], [], []]
    for y in range(cy - half, cy + half + 1):
        row = rows[y]
        for x in range(cx - half, cx + half + 1):
            for c in range(3):
                samples[c].append(row[x * channels + c])
    return tuple(sorted(s)[len(s) // 2] for s in samples)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('lusdrender')
    parser.add_argument('assets', nargs='?')
    parser.add_argument('--output', default=None)
    args = parser.parse_args()

    root = Path(args.assets or os.environ.get('LIGHTUSD_USD_ASSETS')
                or HERE.parent.parent.parent / 'usd-assets')
    asset = root / ASSET
    if not asset.is_file():
        skip(f'usd-assets not found: {asset}')
    out_dir = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='uvtexture-cs-'))
    out_dir.mkdir(parents=True, exist_ok=True)
    image = out_dir / 'tiles.png'
    command = [args.lusdrender, str(asset), str(image), '-camera', '/Camera',
               '-w', str(WIDTH), '-height', str(HEIGHT), '-bg', '0,0,0',
               '-diagnosticEmission', '-vkr']
    run = subprocess.run(command, capture_output=True, text=True, timeout=180)
    log = run.stdout + run.stderr
    if run.returncode != 0 or not image.is_file():
        if any(t in log for t in ('Failed to create LightRT Vulkan engine', 'No Vulkan', 'unavailable')):
            skip('Vulkan renderer unavailable')
        print(log[-2000:])
        print('FAIL: lusdrender did not produce an image')
        return 1

    width, height, channels, rows = read_png(str(image))
    if (width, height) != (WIDTH, HEIGHT):
        print(f'FAIL: unexpected image size {width}x{height}')
        return 1
    failures = []
    print(f'target {TARGET} +/-{TOLERANCE}; columns: {", ".join(COLUMNS)}')
    for r, name in enumerate(ROWS):
        cells = []
        for c in range(5):
            cx, cy = tile_center(r, c)
            value = tile_median(width, channels, rows, cx, cy)
            ok = all(abs(v - t) <= TOLERANCE for v, t in zip(value, TARGET))
            cells.append(('ok ' if ok else 'BAD') + str(value))
            if not ok:
                failures.append(f'row {r} ({name}) col {c} ({COLUMNS[c]}): {value}')
        print(f'  {name:32s} ' + '  '.join(cells))
    if failures:
        print('FAIL: tiles outside the asset target:\n  ' + '\n  '.join(failures))
        return 1
    print('PASS: all 25 tiles match the asset target')
    return 0


if __name__ == '__main__':
    sys.exit(main())
