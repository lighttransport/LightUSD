#!/usr/bin/env python3
"""Exercise the migrated retained RenderScene diagnostics and debug exporters."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
app, repository = map(pathlib.Path, sys.argv[1:])
app=app.resolve();repository=repository.resolve()
def run(*args, good=True, cwd=None):
    result=subprocess.run([str(app), *map(str,args)],capture_output=True,text=True,cwd=cwd)
    assert (result.returncode==0)==good,result.stderr+result.stdout
    return result

def png():
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\x00\xff\x00\x00\xff'))+chunk(b'IEND',b'')
source='''#usda 1.0
(defaultPrim="World")
def Xform "World" {
 double3 xformOp:translate=(3,0,0)
 uniform token[] xformOpOrder=["xformOp:translate"]
 def Mesh "Quad" {
  uniform token subdivisionScheme="none"
  point3f[] points=[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
  int[] faceVertexCounts=[4]
  int[] faceVertexIndices=[0,1,2,3]
  texCoord2f[] primvars:st=[(0,0),(1,0),(1,1),(0,1)] (interpolation="vertex")
  rel material:binding=</World/Look>
 }
 def Material "Look" {
  token outputs:surface.connect=</World/Look/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id="UsdPreviewSurface"
   color3f inputs:diffuseColor.connect=</World/Look/Tex.outputs:rgb>
   token outputs:surface
  }
  def Shader "Tex" {
   uniform token info:id="UsdUVTexture"
   asset inputs:file=@red.png@
   float3 outputs:rgb
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='lightusd-render-example-') as directory:
    root=pathlib.Path(directory);scene=root/'scene.usda';scene.write_text(source);(root/'red.png').write_bytes(png())
    def load(*options,input=scene):return json.loads(run(input,'--json',*options,cwd=root).stdout)
    base=load();assert base['configuration']['backend']=='core'
    assert base['summary']['num_meshes']==1 and base['meshes'][0]['num_points']==4
    assert base['meshes'][0]['num_faceVertexIndices']==6
    assert base['meshes'][0]['tangents']['count']==0
    assert base['meshes'][0]['normals']['count']==48
    normal=root/'normal.usda';normal.write_text(source.replace('color3f inputs:diffuseColor.connect','normal3f inputs:normal.connect'))
    assert load(input=normal)['meshes'][0]['tangents']['count']>0
    assert load('--notangent',input=normal)['meshes'][0]['tangents']['count']==0
    animated=root/'animated.usda';animated.write_text(source.replace('double3 xformOp:translate=(3,0,0)', 'double3 xformOp:translate=(3,0,0)\n double3 xformOp:translate.timeSamples={0:(3,0,0),1:(9,0,0)}'))
    evaluated=load('--timecode','1',input=animated)
    assert json.loads(evaluated['nodes'][0]['global_matrix'])[12]==9
    forced=load('--calctangent');assert forced['meshes'][0]['tangents']['count']==64
    assert load('--calctangent','--notangent')['meshes'][0]['tangents']['count']==0
    packed=load('--snorm8','--calctangent','--lowmem')
    assert packed['meshes'][0]['normals']['count']==12
    assert packed['meshes'][0]['tangents']['count']==16
    assert packed['meshes'][0]['normals']['values']==[0,0,127]*4
    plain=load('--notri','--calctangent');assert plain['meshes'][0]['num_faceVertexIndices']==4
    assert plain['meshes'][0]['num_faceVertexCounts']==1
    for method in ('lengyel','mikktspace','fast-mikktspace','hybrid'):
        assert load('--calctangent','--tangent-method',method)['meshes'][0]['tangents']['count']>0
    seam=root/'seam.usda'
    seam.write_text(source.replace('faceVertexCounts=[4]','faceVertexCounts=[3,3]').replace('faceVertexIndices=[0,1,2,3]','faceVertexIndices=[0,1,2,0,2,3]').replace('texCoord2f[] primvars:st=[(0,0),(1,0),(1,1),(0,1)] (interpolation="vertex")','texCoord2f[] primvars:st=[(0,0),(1,0),(1,1),(0.5,0.5),(1,1),(0,1)] (interpolation="faceVarying")'))
    assert load(input=seam)['meshes'][0]['num_points']==5
    assert load('--noidxbuild',input=seam)['meshes'][0]['num_points']==4
    assert load('--fast-index-build',input=seam)['meshes'][0]['num_points']==6
    assert load('--noidxbuild','--fast-index-build',input=seam)['meshes'][0]['num_points']==4
    decoded=load('--texload');assert decoded['images'][0]['decoded'] is True and decoded['buffers'][0]['bytes']==4
    assert load('--texload','--noar')['images'][0]['decoded'] is False
    yaml=run(scene,'--yaml',cwd=root).stdout
    assert 'metadata:\n' in yaml and 'meshes:\n' in yaml and 'summary:\n' in yaml
    profiled=run(scene,'--nodump','--memstat','--profile',cwd=root)
    assert 'Total vertices: 4, total triangles: 2' in profiled.stdout
    assert '[timing] USD load:' in profiled.stderr
    run(scene,'--snorm8','--calctangent','--nodump','--dumpobj','--dumpusd',cwd=root)
    assert (root/'0Quad.obj').is_file() and (root/'0Quad.mtl').is_file()
    assert 'f ' in (root/'0Quad.obj').read_text()
    exported=root/'sceneexport.usda';assert exported.is_file();assert load(input=exported)['summary']['num_meshes']==1
    binary=root/'scene.usdc';cat=app.parent/'lusdcat'
    if cat.is_file():
        converted=subprocess.run([str(cat),'--rewrite-layer',str(scene),'-o',str(binary)],capture_output=True,text=True)
        assert converted.returncode==0,converted.stderr
        assert load('--mmap-lowmem',input=binary)['summary']['num_meshes']==1
    archive=root/'scene.usdz'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_STORED) as z:
        for name,data in [('nested/root.usda',source.encode()),('nested/red.png',png())]:
            info=zipfile.ZipInfo(name);pad=(-z.fp.tell()-30-len(name.encode())-4)%64
            info.extra=struct.pack('<HH',0x1986,pad)+b'\0'*pad;z.writestr(info,data)
    archived=load('--texload',input=archive);assert archived['buffers'][0]['bytes']==4
    dependent=root/'ref.usda';dependent.write_text('#usda 1.0\ndef Xform "Ref" (references=@scene.usda@</World>) {}\n')
    assert load(input=dependent)['summary']['num_meshes']==1
    payload=root/'payload.usda';payload.write_text('#usda 1.0\ndef Xform "Payload" (prepend payload=@scene.usda@</World>) {}\n')
    assert load('--load-payloads',input=payload)['summary']['num_meshes']==1
    assert load('--defer-payloads',input=payload)['summary']['num_meshes']==0
    animation=repository/'tests/usda/c-core-animation-queries.usda'
    assert load(input=animation)['summary']['num_animations']==2
    assert load('--no-animation',input=animation)['summary']['num_animations']==0
    samples=run(animation,'--nodump','--dump-timesamples',cwd=root).stdout
    assert 'Timesamples:' in samples and 't=' in samples
    for args in [[],[scene,'--wat'],[scene,'--timecode','no'],[scene,'--timecode'],[scene,'--compose-threads','0'],[scene,'--tangent-method','unknown'],[root/'missing.usda']]:run(*args,good=False,cwd=root)
print('PASS core RenderScene example diagnostics, packed attributes, exporters, textures, composition and animation')
