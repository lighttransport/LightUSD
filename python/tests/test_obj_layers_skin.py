# SPDX-License-Identifier: Apache-2.0
import base64
import zipfile
from pathlib import Path
import pytest
import lightusd

OBJ = b'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\nvw 0 0 .25 1 .75\nvw 1 1 2\nvw 2 0 -.5 1 1.5\ng front\nf 1/1 2/2 3/3\nf 1/2 3/1 4/3\n'

def values(v):
    return memoryview(v).tolist()


def test_skin_seams_and_missing_weights():
    stage = lightusd.load_bytes(OBJ, format='obj')
    mesh = stage.prims_of_type('Mesh')[0]
    ids = values(mesh['primvars:skel:jointIndices'])
    weights = values(mesh['primvars:skel:jointWeights'])
    assert mesh.attribute('primvars:skel:jointIndices').metadata('elementSize') == 2
    assert mesh.attribute('primvars:skel:jointWeights').interpolation == 'vertex'
    assert len(ids) == len(weights) == len(mesh['points']) * 2
    assert 'SkelBindingAPI' in mesh.metadata('apiSchemas')
    for point, pair in zip(values(mesh['points']), zip(weights[::2], weights[1::2])):
        assert pair == pytest.approx({(0, 0, 0): (.25, .75), (1, 0, 0): (2, 0),
                                     (0, 1, 0): (-.5, 1.5), (0, 0, 1): (0, 0)}[tuple(point)])
    assert 'missing skin weights' in stage.warnings()
    assert 'skel:skeleton' not in mesh.relationships


@pytest.mark.parametrize('row', ['vw -1 0 1', 'vw 4 0 1', 'vw 0 -1 1', 'vw 0 0 nan',
                                'vw 0 0 inf', 'vw 0 .5 1', 'vw 0 0', 'vw 0 0 1\nvw 0 1 1'])
def test_invalid_skin(row):
    source = b'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n' + row.encode() + b'\n'
    with pytest.raises(lightusd.UsdError): lightusd.load_bytes(source, format='obj')


def test_mixed_primitives_and_all_influences(tmp_path):
    source = b'v 0 0 0\nv 1 0 0\nv 0 1 0\n' + b'vw 0 ' + b' '.join(f'{i} {i/10}'.encode() for i in range(12)) + b'\nf 1 2 3\nl 1 2\np 3 1\n'
    stage = lightusd.load_bytes(source, format='obj')
    assert len(stage.prims_of_type('Mesh')) == len(stage.prims_of_type('BasisCurves')) == len(stage.prims_of_type('Points')) == 1
    curves = stage.prims_of_type('BasisCurves')[0]
    assert curves['type'] == 'linear' and values(curves['curveVertexCounts']) == [2]
    for p in stage:
        if p.type_name not in ('Mesh', 'BasisCurves', 'Points'): continue
        assert p.attribute('primvars:skel:jointWeights').metadata('elementSize') == 12
        assert values(p['extent']) == [[0, 0, 0], [1, 1, 0]] or p.type_name != 'Mesh'
    for ext in ('usda', 'usdc', 'usdz'):
        path = str(tmp_path / ('mesh.' + ext)); stage.save(path)
        again = lightusd.load(path)
        assert values(again.prims_of_type('Mesh')[0]['primvars:skel:jointWeights']) == values(stage.prims_of_type('Mesh')[0]['primvars:skel:jointWeights'])


@pytest.mark.parametrize('arc', ['references', 'payload', 'subLayers'])
def test_composed_obj(tmp_path, arc):
    (tmp_path / 'mesh.obj').write_bytes(OBJ)
    if arc == 'subLayers': text = '#usda 1.0\n( subLayers = [@mesh.obj@] )\n'
    else: text = '#usda 1.0\ndef SkelRoot "Rig" ( prepend '+arc+' = @mesh.obj@ ) { }\n'
    path = tmp_path / 'root.usda'; path.write_text(text)
    stage = lightusd.load(path)
    mesh = stage.prims_of_type('Mesh')[0]
    assert mesh.attribute('primvars:skel:jointWeights').metadata('elementSize') == 2
    assert mesh['subdivisionScheme'] == 'none'


def test_obj_in_package(tmp_path):
    path = tmp_path / 'package.usdz'
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('root.usda', '#usda 1.0\ndef Xform "Asset" ( prepend references = @geo/mesh.obj@ ) { }\n')
        archive.writestr('geo/mesh.obj', OBJ)
    stage = lightusd.load(path)
    assert len(stage.prims_of_type('Mesh')) == 1

@pytest.mark.parametrize('packaged', [False, True])
def test_composed_mtl_dependency_lifetime(tmp_path, packaged):
    png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=')
    obj = b'mtllib mat.mtl\nusemtl material\n' + OBJ
    mtl = 'newmtl material\nmap_Kd tex.png\n'
    usda = '#usda 1.0\ndef Xform "Asset" ( prepend references = @mesh.obj@ ) { }\n'
    if packaged:
        source = tmp_path / 'input.usdz'
        with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_STORED) as z:
            for name, data in [('root.usda', usda), ('mesh.obj', obj), ('mat.mtl', mtl), ('tex.png', png)]: z.writestr(name, data)
    else:
        source = tmp_path / 'input.usda'; source.write_text(usda)
        (tmp_path / 'mesh.obj').write_bytes(obj); (tmp_path / 'mat.mtl').write_text(mtl)
        (tmp_path / 'tex.png').write_bytes(png)
    stage = lightusd.load(source)
    assert len(stage.prims_of_type('Material')) == 1
    if not packaged: (tmp_path / 'tex.png').unlink()
    out = tmp_path / 'out.usdz'; stage.save_usdz(str(out))
    with zipfile.ZipFile(out) as archive:
        assert any(archive.read(n) == png for n in archive.namelist() if n.endswith('.png'))


def test_openusd_skel_deformation(tmp_path):
    """Optional oracle: run against an OpenUSD installation in its own Python."""
    import os
    import subprocess
    executable = os.environ.get('OPENUSD_PYTHON')
    if not executable:
        pytest.skip('set OPENUSD_PYTHON and OPENUSD_PYTHONPATH for the UsdSkel oracle')
    source = tmp_path / 'skin.obj'
    source.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nvw 0 0 .25 1 .75\nvw 1 1 1\nvw 2 0 1\nf 1 2 3\n')
    root = tmp_path / 'root.usda'
    root.write_text('''#usda 1.0
    def SkelRoot "Rig" (prepend references = @skin.obj@; prepend apiSchemas = ["SkelBindingAPI"]) {
        rel skel:skeleton = </Rig/Skeleton>
        def Skeleton "Skeleton" {
            uniform token[] joints = ["root", "root/child"]
            uniform matrix4d[] bindTransforms = [( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1) ), ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1) )]
            uniform matrix4d[] restTransforms = [( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1) ), ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1) )]
        }
    }
    ''')
    stage = lightusd.load(root)
    output = tmp_path / 'bound.usdc'; stage.save(str(output))
    env = dict(os.environ, PYTHONPATH=os.environ.get('OPENUSD_PYTHONPATH', ''), PYTHONDONTWRITEBYTECODE='1')
    oracle = r'''
import sys
from pxr import Usd, UsdSkel, UsdGeom, Vt, Gf
stage = Usd.Stage.Open(sys.argv[1])
root = UsdSkel.Root(stage.GetPrimAtPath('/Rig'))
skel = UsdSkel.Skeleton(stage.GetPrimAtPath('/Rig/Skeleton'))
anim = UsdSkel.Animation.Define(stage, '/Rig/Animation')
anim.CreateJointsAttr(['root', 'root/child'])
anim.CreateTranslationsAttr([(2,0,0), (0,3,0)])
anim.CreateRotationsAttr([Gf.Quatf(1), Gf.Quatf(1)])
anim.CreateScalesAttr([(1,1,1), (1,1,1)])
UsdSkel.BindingAPI.Apply(skel.GetPrim()).CreateAnimationSourceRel().SetTargets([anim.GetPath()])
cache = UsdSkel.Cache(); assert cache.Populate(root, Usd.PrimDefaultPredicate)
mesh = next(p for p in stage.Traverse() if p.GetTypeName() == 'Mesh')
query = cache.GetSkinningQuery(mesh)
assert query and query.HasJointInfluences() and query.GetNumInfluencesPerComponent() == 2
transforms = cache.GetSkelQuery(skel).ComputeSkinningTransforms()
points = UsdGeom.Mesh(mesh).GetPointsAttr().Get()
assert query.ComputeSkinnedPoints(transforms, points)
for actual, expected in zip(points, [(2,2.25,0), (3,3,0), (2,1,0)]):
    assert Gf.IsClose(actual, Gf.Vec3f(*expected), 1e-5), (actual, expected)
print('OpenUSD UsdSkel.Cache deformation passed')
'''
    result = subprocess.run([executable, '-c', oracle, str(output)], env=env, text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_composed_obj_resolver_dependencies(tmp_path):
    resolver = lightusd.AssetResolver()
    root = tmp_path / 'root.usda'
    root.write_text('#usda 1.0\ndef Xform "Asset" (prepend references = @demo:mesh.obj@) {}\n')
    resolver.register_memory('demo:mesh.obj', b'mtllib demo:mat.mtl\nusemtl m\n' + OBJ)
    resolver.register_memory('demo:mat.mtl', b'newmtl m\nKd .1 .2 .3\n')
    stage = lightusd.load(root, resolver=resolver)
    assert len(stage.prims_of_type('Material')) == 1
    assert len(stage.prims_of_type('Mesh')) == 1


def test_package_obj_dependency_cannot_escape(tmp_path):
    source = tmp_path / 'input.usdz'
    with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_STORED) as z:
        z.writestr('root.usda', '#usda 1.0\ndef Xform "Asset" (prepend references = @mesh.obj@) {}\n')
        z.writestr('mesh.obj', b'mtllib ../outside.mtl\n' + OBJ)
    (tmp_path / 'outside.mtl').write_text('newmtl outside\nKd 1 0 0\n')
    stage = lightusd.load(source)
    assert not stage.prims_of_type('Material')
    assert 'MTL' in stage.warnings()
