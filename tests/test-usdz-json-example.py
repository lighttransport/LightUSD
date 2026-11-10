#!/usr/bin/env python3
"""Verify shared core JSON export and byte-exact USDZ sidecars."""
import base64
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile
app, cat = map(lambda value:str(pathlib.Path(value).resolve()), sys.argv[1:])
def run(*args, good=True, cwd=None):
    result = subprocess.run(args,capture_output=True,text=True,cwd=cwd)
    assert (result.returncode == 0) == good, result.stdout+result.stderr
    return result.stdout
with tempfile.TemporaryDirectory(prefix='lightusd-usdz-json-') as directory:
    root = pathlib.Path(directory)
    source = root/'root.usda'
    source.write_text('''#usda 1.0
(defaultPrim="World"; upAxis="Z"; customLayerData={string note="retained"})
def Xform "World" (customData={int tag=7}) {
 double3 xformOp:translate:offset=(1,2,3)
 double3 xformOp:scale=(2,2,2)
 uniform token[] xformOpOrder=["xformOp:translate:offset","xformOp:scale"]
 custom float gain=2
 float gain.timeSamples={0:1, 2:3}
 asset texture=@textures/check.png@
 rel link=</World/Child>
 def Scope "Child" {}
 def Mesh "Mesh" {
  point3f[] points=[(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts=[3]
  int[] faceVertexIndices=[0,1,2]
  normal3f[] normals=[(0,0,1),(0,0,1),(0,0,1)] (interpolation="vertex")
 }
 def BasisCurves "Curve" {
  uniform token type="linear"
  point3f[] points=[(0,0,0),(1,0,0),(2,0,0),(3,0,0)]
  int[] curveVertexCounts=[4]
  float[] widths=[0.5,0.75,1,1.25]
 }
 def Sphere "Sphere" {}
 def Cube "Cube" {double size=3;}
 def Cone "Cone" {double height=5; uniform token axis="Y";}
 def Cylinder "Cylinder" {}
 def Capsule "Capsule" {}
 def Plane "Plane" {}
}
''')
    crate = root/'root.usdc'
    run(cat,'--rewrite-layer',str(source),'-o',str(crate))
    reports=[]
    for binary in [False,True]:
        root_name='nested/root.'+('usdc' if binary else 'usda')
        files={root_name:(crate if binary else source).read_bytes(),
               'dependency.usdc':crate.read_bytes(), 'empty.bin':b'',
               'assets/quote"é.bin':bytes(range(256))*12+b'!',
               **{'padding'+str(n)+'.bin':bytes(range(n)) for n in [1,2,3,4]}}
        package=root/('sample'+str(binary)+'.usdz')
        with zipfile.ZipFile(package,'w',compression=zipfile.ZIP_STORED) as archive:
            for name,data in files.items():archive.writestr(name,data)
        prefix=root/('output'+str(binary))
        stdout=run(app,"--layer-json",str(package),str(prefix))
        assert 'Main USD file: '+root_name in stdout, stdout
        scene=json.loads(prefix.with_name(prefix.name+'_usd.json').read_text())
        reports.append(scene)
        assert scene['typeName']=='Layer' and scene['metas']['defaultPrim']=='World'
        assert scene['primSpecs']['World']['children']['Child']['typeName']=='Scope'
        assert 'gain' in scene['primSpecs']['World']['properties']
        assets=json.loads(prefix.with_name(prefix.name+'_assets.json').read_text())
        assert set(assets)==set(files), assets.keys()
        for name,data in files.items():
            assert assets[name]['filename']==name and assets[name]['size']==len(data)
            assert base64.b64decode(assets[name]['data'],validate=True)==data
        run(app,str(package),cwd=root)
        runtime=json.loads((root/(package.stem+'_usd.json')).read_text())
        assert runtime['version']==1 and runtime['properties']['upAxis']=='Z'
        assert runtime['primChildren']['World']['xformOpOrder']==['offset','']
        children=runtime['primChildren']['World']['primChildren']
        mesh=children['Mesh']
        assert mesh['typeName']=='GeomMesh' and mesh['points']['type']=='point3f[]'
        import struct
        assert struct.unpack('<9f',base64.b64decode(mesh['points']['data']))==(0,0,0,1,0,0,0,1,0)
        assert struct.unpack('<3i',base64.b64decode(mesh['faceVertexIndices']['data']))==(0,1,2)
        assert mesh['normals']['metadata']['interpolation']=='\"vertex\"'
        assert children['Curve']['widths']['count']==4
        assert children['Sphere']['radius']==1 and children['Cube']['size']==3
        assert children['Cone']['height']==5 and children['Cone']['axis']=='Y'
        assert children['Capsule']['radius']==0.5 and children['Plane']['width']==2
        if os.environ.get('LIGHTUSD_LEGACY_JSON_ORACLE'):
            baseline=json.loads(run(os.environ['LIGHTUSD_LEGACY_JSON_ORACLE'],str(source)))
            def subset(expected,actual):
                if isinstance(expected,dict):
                    for key,value in expected.items():
                        assert key in actual, key
                        subset(value,actual[key])
                elif expected is None and isinstance(actual,dict) and actual.get('typeName')=='Scope':
                    # The legacy runtime serializer emits null for Scope; core
                    # preserves the authored prim instead of discarding it.
                    assert actual.get('specifier')=='def'
                else: assert expected==actual,(expected,actual)
            subset(baseline,runtime)
    assert reports[0]==reports[1], 'USDA and USDC authored JSON differ'
    for args in [[], [str(source)], [str(root/'absent.usdz')], ['--unknown'], [str(package),'one','extra']]:
        run(app,*args,good=False)
    broken=root/'broken.usdz';broken.write_bytes(b'PK\x03\x04')
    run(app,str(broken),good=False)
    run(app,str(package),str(root/'absent'/'out'),good=False)
    assert 'Usage:' in run(app,'--help')
print('PASS: core USDZ JSON, USDA/USDC root parity, correct first entry, exact binary assets and CLI errors')
