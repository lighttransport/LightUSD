#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Vulkan descriptor-path Ptex oracle on a real Island mesh.

Renders one Island quad mesh (the isGardeniaA trunk, bound to a PxrPtexture
`.ptx`) with `lusdrender -vkr`, then checks every visible pixel's base colour
against OpenPtex:

  1. render with -rtDebugAov ptex  -> exact (u, v, face) per pixel
  2. render with -rtDebugAov base  -> linear base colour per pixel
     (both captured as raw floats via -vkDumpF32)
  3. tests/verification/repros/ptex-vkr-probe-verify.cc re-samples the same
     (face, u, v) from the OpenPtex-decoded page and compares.

Also asserts the render did not fall back to a flat colour (the PTex texture must
load, every triangle must carry a face id) and that the base colour actually
varies. Skips (77) without the Island corpus, an OpenPtex build, or Vulkan.

usage: check_vkr_ptex.py <lusdrender> [--output DIR]
Environment (defaults in brackets):
  LIGHTUSD_ISLAND         Island root       [/mnt/nvme02/data/island]
  LIGHTUSD_PTEX_INCLUDE   Optional OpenPtex src/ptex (headers + .cpp; >=2.4); unset skips
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SKIP = 77
MASK = '/isGardeniaA/isGardeniaA10/geometry/base_part/geo_base_mod/trunk_base_geo'
PTX_REL = 'textures/isGardeniaA/Color/trunk_base_geo.ptx'
ELEMENT_REL = 'usd/elements/isGardeniaA/element.usda'


def skip(reason):
    print(f'SKIP: {reason}')
    sys.exit(SKIP)


def read_dump(path):
    import numpy as np
    raw = Path(path).read_bytes()
    w, h = struct.unpack('<ii', raw[:8])
    return np.frombuffer(raw[8:], dtype='<f4').reshape(h, w, 4)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('lusdrender')
    parser.add_argument('--output', default=None)
    args = parser.parse_args()
    try:
        import numpy as np  # noqa: F401
    except ImportError:
        skip('numpy not available')

    island = Path(os.environ.get('LIGHTUSD_ISLAND', '/mnt/nvme02/data/island'))
    reference_source = os.environ.get('LIGHTUSD_PTEX_INCLUDE')
    if not reference_source:
        skip('optional WDAS/OpenPtex oracle not enabled; set LIGHTUSD_PTEX_INCLUDE')
    include = Path(reference_source)
    element, ptx = island / ELEMENT_REL, island / PTX_REL
    for what, p in (('Island element', element), ('Island .ptx', ptx),
                    ('OpenPtex header', include / 'Ptexture.h'),
                    ('OpenPtex sources', include / 'PtexReader.cpp')):
        if not p.exists():
            skip(f'{what} not found: {p}')
    version_header = include / 'PtexVersion.h'
    version = version_header.read_text() if version_header.is_file() else ''
    major = re.search(r'#define\s+PtexLibraryMajorVersion\s+(\d+)', version)
    minor = re.search(r'#define\s+PtexLibraryMinorVersion\s+(\d+)', version)
    if not major or not minor or (int(major[1]), int(minor[1])) < (2, 4):
        skip('OpenPtex >=2.4 sources required; 2.3 crashes on the mip oracle')
    if not subprocess.run(['which', 'g++'], capture_output=True).returncode == 0:
        skip('g++ not available')

    out = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='vkr-ptex-'))
    out.mkdir(parents=True, exist_ok=True)

    def render(aov, name):
        dump = out / f'{name}.f32'
        cmd = [args.lusdrender, str(element), str(out / f'{name}.png'),
               '-w', '320', '-height', '240', '-autoframe', '-bg', '0,0,0',
               '-stats', '-vkr', '-mask', MASK, '-vkDumpF32', str(dump)]
        if aov: cmd += ['-rtDebugAov', aov]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        log = run.stdout + run.stderr
        (out / f'{name}.log').write_text(log)
        if any(t in log for t in ('Failed to create LightRT Vulkan engine', 'No Vulkan', 'no Vulkan')):
            skip('Vulkan renderer unavailable')
        if run.returncode != 0 or not dump.is_file():
            print(log[-2000:])
            print(f'FAIL: render {name} exited {run.returncode}')
            sys.exit(1)
        return log

    log = render('ptex', 'ptex')
    render('base', 'base')

    if 'failed to load texture' in log:
        print('FAIL: the Ptex texture failed to load'); return 1
    if 'MaterialX graph fallback' in log:
        print('FAIL: the Ptex material fell back to flat colour (graph compile)'); return 1
    m = re.search(r'Vulkan ptex: textures=(\d+) triangles_with_face=(\d+)/(\d+)', log)
    if not m or int(m.group(1)) < 1 or int(m.group(2)) != int(m.group(3)) or int(m.group(3)) == 0:
        print(f'FAIL: Ptex wiring: {m.group(0) if m else "no ptex stats line"}'); return 1

    import numpy as np
    a, b = read_dump(out / 'ptex.f32'), read_dump(out / 'base.f32')
    face = a[..., 2] - 1.0   # AOV z = face + 1 (0 = miss, -1 = no Ptex data)
    valid = a[..., 2] >= 0.5
    n = int(valid.sum())
    if n < 500:
        print(f'FAIL: only {n} visible Ptex pixels'); return 1
    colors = b[..., :3][valid]
    if float(colors.std(axis=0).max()) < 1e-3:
        print('FAIL: base colour is constant over the mesh (texture not applied)'); return 1

    probe = out / 'probe.txt'
    with open(probe, 'w') as f:
        f.write(f'# texture {ptx}\n# srgb 1\n')
        ys, xs = np.nonzero(valid)
        for y, x in zip(ys, xs):
            f.write('%d %.7f %.7f %.7f %.7f %.7f\n' % (
                int(round(a[y, x, 2])) - 1, a[y, x, 0], a[y, x, 1], *b[y, x, :3]))
    verifier = out / 'ptex-vkr-verify'
    sources = sorted(p for p in include.glob('*.cpp') if p.name not in ('PtexHalfTableGen.cpp', 'PtexMemoryReader.cpp'))
    build = subprocess.run(
        ['g++', '-std=c++17', '-O2', '-pthread', f'-I{include}',
         str(REPO / 'tests/verification/repros/ptex-vkr-probe-verify.cc'),
         *map(str, sources), '-lz', '-o', str(verifier)],
        capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stderr[-2000:]); print('FAIL: could not build the OpenPtex verifier'); return 1
    env = dict(os.environ)
    run = subprocess.run([str(verifier), str(probe)], env=env, capture_output=True, text=True)
    print(run.stdout.strip(), run.stderr.strip())
    if run.returncode != 0:
        print('FAIL: base colour differs from OpenPtex'); return 1
    print(f'descriptor path: {n} visible Ptex pixels match OpenPtex')

    # Chunked path: force chunking (-gpuTriangleChunk) so hits are shaded
    # on the CPU through the lazy Ptex source (per-face mips from the ray-
    # differential footprint, cross-face taps), then verify the dumped
    # (face, mip, u, v, rgb) samples with the full OpenPtex model.
    chunk_probe = out / 'chunk-probe.txt'
    chunk_probe.unlink(missing_ok=True)
    cmd = [args.lusdrender, str(element), str(out / 'chunk.png'), '-w', '320', '-height', '240',
           '-autoframe', '-bg', '0,0,0', '-stats', '-vkr', '-mask', MASK,
           '-gpuTriangleChunk', '3000', '-ptexProbe', str(chunk_probe)]
    run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    clog = run.stdout + run.stderr
    (out / 'chunk.log').write_text(clog)
    if run.returncode != 0 or 'GPU chunks' not in clog:
        print(clog[-1500:]); print('FAIL: chunked render did not run through the chunk path'); return 1
    if not chunk_probe.is_file() or chunk_probe.stat().st_size < 1000:
        print('FAIL: the chunked path produced no Ptex samples'); return 1
    samples = [line.split() for line in chunk_probe.read_text().splitlines()
               if line.strip() and not line.startswith('#')]
    if not any(len(row) == 11 and int(row[4]) > 0 for row in samples):
        print('FAIL: chunked Ptex sampling never selected a nonzero mip'); return 1
    chunk_verifier = out / 'ptex-element-verify'
    build = subprocess.run(
        ['g++', '-std=c++17', '-O2', '-pthread', f'-I{include}',
         str(REPO / 'tests/verification/repros/ptex-element-probe-verify.cc'),
         *map(str, sources), '-lz', '-o', str(chunk_verifier)],
        capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stderr[-2000:]); print('FAIL: could not build the cross-face verifier'); return 1
    run = subprocess.run([str(chunk_verifier), str(chunk_probe)],
                         env=dict(os.environ, PTEX_VERIFY_MIN_TEXTURES='1'),
                         capture_output=True, text=True)
    print('chunked path:', (run.stdout + run.stderr).strip().replace('\n', ' | '))
    if run.returncode != 0:
        print('FAIL: chunked-path Ptex samples differ from OpenPtex'); return 1
    print(f'PASS: descriptor ({n} px) and chunked paths match OpenPtex')
    return 0


if __name__ == '__main__':
    sys.exit(main())
