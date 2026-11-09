#!/usr/bin/env python3
"""Build and render the Disney Island full scene for Storm without the
xgen payloads that reset the AMD GPU.

`island.usda` references `elements/<E>/element.usda`, whose roots are
`instanceable = true`.  Instance proxies cannot take `active = false`
overrides, so the payloads that exceed the GPU's per-frame budget cannot be
turned off from a wrapper.  Referencing the non-instanceable `instance.usda`
for each element instead keeps the same composed geometry and materials and
exposes real prims that a wrapper can deactivate.

Measured on RX 9070 XT / radeonsi / Mesa 26.2.0: with the five deactivations
below the 20-element scene renders (640-wide depth: 164269 of 171520 pixels
covered, no reset; 960-wide colour renders).  Each deactivated payload on its
own resets the GPU (`amdgpu ... ring gfx_0.0.0 timeout`), so the full scene
without them cannot be rendered on this stack.

See doc/validation/storm-instancer-gpu-reset-2026-11-05.md.

Usage:
    island-storm-full-wrapper.py --out DIR [--width 640] [--aov depth|color]
                                 [--all]      # keep every payload (resets GPU)
                                 [--dist DIR] # OpenUSD dist root
"""
import argparse
import array
import os
import subprocess
import sys

ELEMENTS = [
    'isBayCedarA1', 'isCoral', 'isGardeniaA', 'isHibiscus', 'isHibiscusYoung',
    'isIronwoodA1', 'isIronwoodB', 'isKava', 'isNaupakaA', 'isPalmDead',
    'isPalmRig', 'isPandanusA', 'isBeach', 'isCoastline', 'isDunesA',
    'isDunesB', 'isLavaRocks', 'isMountainA', 'isMountainB', 'osOcean',
]

# (element, geometry-relative prim path) whose payload expands past the
# per-frame GPU budget.  Each is verified individually to reset the GPU.
DEACTIVATE = [
    ('isBeach', 'geometry/xgGroundCover'),   # 683 MB payload, ~21.4M instances
    ('isDunesB', 'geometry/xgTreeFill'),     # 64724 draw items in 2 batches
    ('isMountainB', 'geometry/xgFoliageAd'),
    ('isMountainB', 'geometry/xgFern'),
    ('isMountainB', 'geometry/xgLowGrowth'),
]

CAMERA = '''    def Xform "cam"
    {
        def Camera "beachCam"
        {
            float2 clippingRange = (12, 1000000)
            float focalLength = 35
            float focusDistance = 712.39124
            float horizontalAperture = 36.00002
            float horizontalApertureOffset = 0
            token projection = "perspective"
            float verticalAperture = 15.082039
            float verticalApertureOffset = 0
            matrix4d xformOp:transform:lookat = ( (0.3583679495452892, 1.3877787807814457e-17, 0.9335804264972063, 0), (0.07387085455276614, 0.9968645913319032, -0.028356364300138125, 0), (-0.930653270335601, 0.0791263960298842, 0.35724431956991665, 0), (0, 0, 0, 1) )
            matrix4d xformOp:transform:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (-510.5239066714681, 87.30874393630907, 181.77019700660784, 1) )
            uniform token[] xformOpOrder = ["xformOp:transform:transform", "xformOp:transform:lookat"]
        }
    }'''


def emit_tree(node, indent, lines):
    for name, child in node.items():
        pad = ' ' * indent
        if child is None:
            lines.append(f'{pad}over "{name}" ( active = false )')
            lines.append(f'{pad}{{')
            lines.append(f'{pad}}}')
        else:
            lines.append(f'{pad}over "{name}"')
            lines.append(f'{pad}{{')
            emit_tree(child, indent + 4, lines)
            lines.append(f'{pad}}}')


def build(scene_root, stage_path, deactivate):
    lines = ['#usda 1.0', '(', '    defaultPrim = "island"',
             '    metersPerUnit = 0.0254', '    upAxis = "Y"', ')', '',
             'def Xform "island"', '{']
    for e in ELEMENTS:
        lines += [f'    def Xform "{e}" (',
                  f'        prepend references = @{scene_root}/elements/{e}/instance.usda@',
                  '    )', '    {']
        tree = {}
        for de, inner in deactivate:
            if de != e:
                continue
            node = tree
            parts = inner.split('/')
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = None
        if tree:
            emit_tree(tree, 8, lines)
        lines += ['    }', '']
    lines += [CAMERA, '}', '']
    with open(stage_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def gpu_resets():
    try:
        out = subprocess.run(['dmesg'], capture_output=True, text=True).stdout
    except Exception:
        return None
    return out.count('ring gfx_0.0.0 timeout')


def depth_stats(path):
    if not os.path.exists(path):
        return None
    d = open(path, 'rb').read()
    i = d.find(b'\n') + 1
    rest = d[i:]
    parts = rest.split(b'\n', 2)
    w, h = map(int, parts[0].split())
    off = len(parts[0]) + 1 + len(parts[1]) + 1
    vals = array.array('f')
    vals.frombytes(rest[off:])
    depth = vals[0::3]
    return {
        'width': w, 'height': h,
        'covered': sum(1 for x in depth if 0.0 < x < 0.999),
        'zero': sum(1 for x in depth if x == 0.0),
        'background': sum(1 for x in depth if x >= 0.999),
        'max': max(vals),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='/mnt/nvme02/data/island/usd')
    ap.add_argument('--out', default='/tmp/island-storm-full')
    ap.add_argument('--dist', default='/mnt/nvme02/work/OpenUSD/dist-v26.11')
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--aov', choices=('depth', 'color'), default='depth')
    ap.add_argument('--all', action='store_true',
                    help='keep every payload (expected to reset the GPU)')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    stage = os.path.join(args.out, 'island_full.usda')
    build(args.scene, stage, [] if args.all else DEACTIVATE)

    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = f'{args.dist}/lib:/mnt/nvme02/work/OpenUSD/deps/inst/lib'
    env['PYTHONPATH'] = f'{args.dist}/lib/python3.11/site-packages'
    env['PXR_USDC_EMIT_DEPRECATION_WARNINGS'] = '0'

    for f in os.listdir(args.out):
        if f.startswith('island_full.') and f != 'island_full.usda' \
                and f != 'island_full.log':
            os.unlink(os.path.join(args.out, f))

    ext = '.pfm' if args.aov == 'depth' else '.png'
    out = os.path.join(args.out, 'island_full.###' + ext)
    cmd = [f'{args.dist}/bin/usdrecord',
           '--camera', '/island/cam/beachCam',
           '--imageWidth', str(args.width), '--frames', '0',
           '--renderer', 'Storm']
    if args.aov == 'depth':
        cmd += ['--captureAov', 'depth']
    cmd += [stage, out]

    before = gpu_resets()
    rc = subprocess.call(cmd, env=env,
                         stdout=open(os.path.join(args.out, 'render.log'), 'w'),
                         stderr=subprocess.STDOUT)
    after = gpu_resets()

    print(f'stage: {stage}')
    print(f'dropped: {[] if args.all else DEACTIVATE}')
    print(f'rc={rc} gpu_resets {before}->{after}')
    if args.aov == 'depth':
        stats = depth_stats(os.path.join(args.out, 'island_full.000.pfm'))
        print(f'depth: {stats}')
        if stats and stats['zero'] == stats['width'] * stats['height']:
            print('ALL-ZERO frame: the render target was never written '
                  '(GPU device reset)')
            return 1
    else:
        print(f'image: {out.replace("###", "000")}')
    if after is not None and before is not None and after > before:
        print('GPU RESET during this frame')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
