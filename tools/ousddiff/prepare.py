#!/usr/bin/env python3
"""Isolated OpenUSD preparation worker. Invoked using the OpenUSD interpreter."""
import json
import math
from pathlib import Path
import struct
import sys


def float32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def prepare(request):
    from pxr import Gf, Sdf, Usd, UsdGeom, UsdUtils
    source = Path(request['scene']).resolve()
    directory = Path(request['directory']).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    settings = request['render']
    # Root metadata (up-axis, units and time range) is not inherited from sublayers.
    original = Usd.Stage.Open(str(source), load=Usd.Stage.LoadNone)
    if not original:
        raise ValueError('OpenUSD could not open source stage')
    metadata = {key: original.GetMetadata(key) for key in
                ('upAxis', 'metersPerUnit', 'startTimeCode', 'endTimeCode', 'timeCodesPerSecond', 'framesPerSecond')}
    wrapper = Sdf.Layer.CreateNew(str(directory / 'scene.usda'))
    wrapper.subLayerPaths = [str(source)]
    stage = Usd.Stage.Open(wrapper, load=Usd.Stage.LoadNone)
    for key, value in metadata.items():
        if value is not None:
            stage.SetMetadata(key, value)
    # Keep authored render products from escaping the artifact directory.
    stage.SetMetadata('renderSettingsPrimPath', '')
    time_value = settings.get('frame', original.GetStartTimeCode())
    code = Usd.TimeCode.Default() if time_value == 'default' else Usd.TimeCode(float(time_value))
    original = None
    stage.SetEditTarget(wrapper)
    overrides = []
    rules = request.get('variant_rules', [])
    # Rules operate on the currently composed hierarchy, then load payloads and
    # repeat to discover variant sets introduced by the selected payloads.
    for iteration in range(8):
        changed = False
        for prim in list(stage.TraverseAll()):
            for rule in rules:
                if rule.get('path') and str(prim.GetPath()) != rule['path']:
                    continue
                variant = prim.GetVariantSets().GetVariantSet(rule['set'])
                if rule['selection'] not in variant.GetVariantNames():
                    continue
                if variant.GetVariantSelection() != rule['selection']:
                    variant.SetVariantSelection(rule['selection'])
                    overrides.append({'path': str(prim.GetPath()), **rule})
                    changed = True
        stage.Load()
        if not changed and iteration > 0:
            break
    else:
        raise ValueError('variant selections did not stabilize')
    composition_errors = [str(error) for error in stage.GetCompositionErrors()]
    if composition_errors:
        raise ValueError('composition errors: ' + '\n'.join(composition_errors[:20]))
    renderables = 0
    renderable_types = {}
    for prim in Usd.PrimRange.Stage(stage, Usd.TraverseInstanceProxies()):
        if prim.IsA(UsdGeom.Gprim) or prim.IsA(UsdGeom.PointInstancer) or prim.GetTypeName() in ('Volume', 'ParticleField3DGaussianSplat'):
            renderables += 1
            name = str(prim.GetTypeName())
            renderable_types[name] = renderable_types.get(name, 0) + 1
    dependencies = []
    unresolved = []
    # This includes texture/sidecar assets and layers, so resume cannot miss a
    # texture changed beneath an unchanged entry layer.
    layers, assets, missing = UsdUtils.ComputeAllDependencies(str(wrapper.identifier))
    unresolved.extend(str(v) for v in missing)
    for name in sorted({str(layer.realPath) for layer in layers if layer.realPath} | {str(v) for v in assets}):
        path = Path(name)
        if path.is_file() and path.resolve() != Path(wrapper.identifier).resolve():
            stat = path.stat()
            dependencies.append({'path': str(path.resolve()), 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns})
    if unresolved:
        raise ValueError('unresolved dependencies: ' + ', '.join(unresolved[:20]))
    result = {'status': 'ok', 'scene': wrapper.identifier, 'frame': time_value,
              'classification': 'renderable' if renderables else 'support_or_empty',
              'renderable_count': renderables, 'renderable_types': renderable_types, 'variant_overrides': overrides,
              'dependencies': dependencies, 'purposes': settings.get('purposes', ['default', 'render'])}
    if not renderables:
        wrapper.Save()
        return result
    width = settings.get('width', 512)
    height = settings.get('height', width)
    camera_path = settings.get('camera')
    if camera_path:
        if not camera_path.startswith('/'):
            raise ValueError('camera must be an absolute prim path')
        camera = UsdGeom.Camera(stage.GetPrimAtPath(camera_path))
        if not camera:
            raise ValueError('configured camera does not exist: ' + camera_path)
        gf = camera.GetCamera(code)
        aspect = float32(gf.aspectRatio)
        if not math.isfinite(aspect) or aspect <= 0:
            raise ValueError('invalid camera aspect')
        height = max(1, int(float32(float32(width) / aspect)))
    else:
        purposes = result['purposes']
        bbox = UsdGeom.BBoxCache(code, purposes, useExtentsHint=True).ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedRange()
        if bbox.IsEmpty():
            result['classification'] = 'empty_for_purposes'
            wrapper.Save()
            return result
        lo, hi = bbox.GetMin(), bbox.GetMax()
        if not all(math.isfinite(v) for v in (*lo, *hi)):
            raise ValueError('nonfinite scene bounds')
        center = (lo + hi) * 0.5
        size = hi - lo
        # Front view, except planar stages which must be viewed face-on.
        axis = 2 if UsdGeom.GetStageUpAxis(stage) == 'Y' else 1
        extent = max(size)
        flat = min(range(3), key=lambda i: size[i])
        if extent > 0 and size[flat] <= extent * 1e-4:
            axis = flat
        direction = Gf.Vec3d(0)
        direction[axis] = -1 if axis == 1 else 1
        up = Gf.Vec3d(0, 1, 0) if axis != 1 else Gf.Vec3d(0, 0, 1)
        if axis == 0:
            up = Gf.Vec3d(0, 0, 1)
        gf = Gf.Camera()
        gf.horizontalAperture = 20.0
        gf.verticalAperture = 20.0 * height / width
        # A bounding sphere fits at every view direction and respects aspect.
        radius = max(size.GetLength() * 0.5, 0.001)
        half_fov = min(gf.GetFieldOfView(Gf.Camera.FOVHorizontal), gf.GetFieldOfView(Gf.Camera.FOVVertical)) * math.pi / 360
        distance = radius / math.sin(half_fov) * 1.05
        eye = center + direction * distance
        gf.transform = Gf.Matrix4d().SetLookAt(eye, center, up).GetInverse()
        gf.clippingRange = Gf.Range1f(max(0.00001, distance - radius * 1.1), distance + radius * 1.1)
        camera_path = '/__ousddiff_camera'
        while stage.GetPrimAtPath(camera_path):
            camera_path += '_'
        camera = UsdGeom.Camera.Define(stage, camera_path)
        camera.SetFromCamera(gf, code)
        # Mirror usdrecord's float division and truncation exactly.
        height = max(1, int(float32(float32(width) / float32(gf.aspectRatio))))
    result.update(camera=camera_path, width=width, height=height,
                  camera_transform=list(gf.transform), camera_aspect=gf.aspectRatio)
    # Gf vectors need explicit JSON conversion.
    result['camera_transform'] = [[float(v) for v in row] for row in gf.transform]
    wrapper.Save()
    return result


def main():
    request = json.loads(Path(sys.argv[1]).read_text())
    result_path = Path(sys.argv[2])
    try:
        result = prepare(request)
    except Exception as exc:
        result = {'status': 'preparation_error', 'error': str(exc)}
    result_path.write_text(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['status'] == 'ok' else 1


if __name__ == '__main__':
    raise SystemExit(main())
