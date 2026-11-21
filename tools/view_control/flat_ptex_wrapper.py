#!/usr/bin/env python3
"""Author a flat-material wrapper for paired SDK/native beauty captures.

Island's look layers terminate in RenderMan (`outputs:ri:surface`); the stock
Storm build has no surface source for them, so it renders its fallback gray.
This tool writes a wrapper that sublayers the source and overrides *only*
`outputs:surface` on every Material with a shared constant UsdPreviewSurface.
Both renderers then evaluate the same flat material, which makes the beauty AOV
a lighting/BRDF/tonemap comparison. It deliberately does not compare PTex
sampling; that stays a native-only OpenPtex check.
"""
import argparse
from pathlib import Path

from pxr import Sdf, Usd, UsdGeom, UsdShade

HEADER = '''#usda 1.0
(
__DEFAULT_PRIM____UNITS__    subLayers = [
        @__SOURCE__@
    ]
)
'''

FLAT = '''
def Scope "__ViewControlFlatPtex"
{
    def Shader "Flat"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (__COLOR__)
        float inputs:metallic = 0
        float inputs:roughness = 0.5
        float inputs:ior = 1.5
        token outputs:surface
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--color', default='0.5,0.5,0.5',
                        help='constant diffuseColor (default 0.5,0.5,0.5)')
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    stage = Usd.Stage.Open(str(source))
    default_prim = stage.GetDefaultPrim()
    roots = [p.GetName() for p in stage.GetPseudoRoot().GetChildren()]
    if not roots:
        raise SystemExit('source stage has no root prims')
    root_names = [default_prim.GetName()] if default_prim else roots

    materials = []
    for prim in stage.Traverse():
        if prim.IsA(UsdShade.Material):
            materials.append(prim.GetPath().pathString)
    if not materials:
        raise SystemExit('source stage has no UsdShade.Material prims')

    # One shared flat shader; overriding each material's surface output keeps
    # the original bindings and every mesh on the same material.
    default_line = ('    defaultPrim = "%s"\n' % default_prim.GetName()
                    if default_prim else '')
    # Layer metadata does not inherit through subLayers: a wrapper that omits
    # metersPerUnit makes the stage default to 1.0 while Island is authored in
    # centimetres, which mis-scales the harness camera and renders nothing.
    units = ''
    meters = UsdGeom.GetStageMetersPerUnit(stage)
    if meters and abs(meters - 1.0) > 1e-9:
        units += '    metersPerUnit = %s\n' % repr(meters)
    up = UsdGeom.GetStageUpAxis(stage)
    if up:
        units += '    upAxis = "%s"\n' % up
    lines = [HEADER.replace('__DEFAULT_PRIM__', default_line)
             .replace('__UNITS__', units)
             .replace('__SOURCE__', str(source))]
    # Group material paths into a nested over tree rooted at the root prims.
    tree = {}
    for path in materials:
        parts = path.strip('/').split('/')
        node = tree
        for part in parts:
            node = node.setdefault(part, {})
    def emit(node, depth):
        pad = '    ' * depth
        for name in sorted(node):
            lines.append(pad + 'over "%s"\n' % name + pad + '{\n')
            if not node[name]:
                # `outputs:surface` is the universal/native terminal;
                # `outputs:glslfx:surface` is the Storm UsdPreviewSurface
                # terminal authored by the Island look layers. Override both so
                # neither renderer reaches the original PTex shader network.
                lines.append(
                    pad + '    token outputs:surface.connect = '
                    '</__ViewControlFlatPtex/Flat.outputs:surface>\n')
                lines.append(
                    pad + '    token outputs:glslfx:surface.connect = '
                    '</__ViewControlFlatPtex/Flat.outputs:surface>\n')
                # Drop the RenderMan displacement/surface terminals so the
                # original PTex displacement/Disney chains are unreachable.
                lines.append(
                    pad + '    token outputs:ri:displacement.connect = None\n')
                lines.append(
                    pad + '    token outputs:ri:surface.connect = None\n')
            else:
                emit(node[name], depth + 1)
            lines.append(pad + '}\n')
    emit(tree, 0)
    lines.append(FLAT.replace('__COLOR__', args.color))
    args.output.write_text(''.join(lines))
    print('wrote %s: %d materials overridden' % (args.output, len(materials)))


if __name__ == '__main__':
    main()
