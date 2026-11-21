# SPDX-License-Identifier: Apache-2.0
# Generated, asset-free control: normal-map shading is outside the normal AOV.
import argparse,hashlib,json,math,os,struct,subprocess,sys,zlib
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--usdrecord',type=Path,required=True);parser.add_argument('--lusdrender',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);parser.add_argument('--sdk-source',type=Path);args=parser.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False);args.usdrecord=args.usdrecord.resolve(strict=True);args.lusdrender=args.lusdrender.resolve(strict=True)
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'tools/view_control'));sys.path.insert(0,str(root))
from aov import read_pfm
from tools.ousddiff.images import compare
from headless import reference_render_findings,toolchain_fingerprint
args.usd_python=Path(sys.executable)

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def chunk(name,data):return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data)&0xffffffff)
result={'scope':'Unperturbed normal AOV versus actual normal-map shading, generated plane; no BRDF equivalence assertion. SDK uses software EGL for this oracle interpretation check.','binary_sha256':{'usdrecord':sha(args.usdrecord),'lusdrender':sha(args.lusdrender)},'captures':{},'toolchain':toolchain_fingerprint(args),'worker_sha256':sha(Path(__file__))}
if args.sdk_source:
 result['sdk_source_files']={name:sha(args.sdk_source/name)for name in ('pxr/imaging/hdSt/shaders/mesh.glslfx','pxr/imaging/hdSt/shaders/terminals.glslfx','pxr/imaging/hdSt/shaders/renderPass.glslfx','pxr/usd/plugin/usdShaders/shaders/previewSurface.glslfx')}
for name,rgb in [('neutral',(128,128,255)),('tilted',(218,128,218))]:
 texture=args.out/(name+'.png');texture.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes(rgb)))+chunk(b'IEND',b''))
 source=args.out/(name+'.usda');source.write_text('''#usda 1.0
 def Mesh "Plane" (prepend apiSchemas = ["MaterialBindingAPI"]) {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(-2,-2,0),(2,-2,0),(2,2,0),(-2,2,0)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
  rel material:binding = </M>
 }
 def Material "M" {
  token outputs:surface.connect = </M/S.outputs:surface>
  def Shader "S" { uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor = (0.8,0.8,0.8)
   color3f inputs:specularColor = (0,0,0)
   int inputs:useSpecularWorkflow = 1
   float inputs:roughness = 1
   normal3f inputs:normal.connect = </M/T.outputs:rgb>
   token outputs:surface
  }
  def Shader "T" { uniform token info:id = "UsdUVTexture"
   asset inputs:file = @'''+texture.name+'''@
   float2 inputs:st.connect = </M/UV.outputs:result>
   token inputs:sourceColorSpace = "raw"
   float4 inputs:scale = (2,2,2,1)
   float4 inputs:bias = (-1,-1,-1,0)
   float3 outputs:rgb
  }
  def Shader "UV" { uniform token info:id = "UsdPrimvarReader_float2"
   token inputs:varname = "st"
   float2 outputs:result
  }
 }
 def Camera "C" {
  double3 xformOp:translate = (0,0,5)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  float focalLength = 50
  float horizontalAperture = 20
  float verticalAperture = 20
  float2 clippingRange = (0.1,20)
 }
''')
 length=math.sqrt(sum((v/255*2-1)**2 for v in rgb))
 result['captures'][name]={'source_sha256':sha(source),'texture_sha256':sha(texture),'authored_tangent_normal':[(v/255*2-1)/length for v in rgb]}
 for engine in ('sdk','native'):
  for mode in ('normal','beauty'):
   stem=f'{name}-{engine}-{mode}';output=args.out/(stem+('.pfm'if mode=='normal'else'.png'))
   if engine=='sdk':
    output=args.out/(stem+'.####.###'+output.suffix)
    command=[str(args.usdrecord),str(source),str(output),'--egl','--abortOnError','--ignoreRenderSettings','--camera','/C','--frames=0','--imageWidth','16','--colorCorrectionMode','sRGB','--complexity','low','--background','0,0,0','--diagnostics',str(args.out/(stem+'-diagnostics.json'))]
    if mode=='normal':command+=['--captureAov','Neye']
    env=dict(os.environ,LIBGL_ALWAYS_SOFTWARE='1',HDX_MSAA_SAMPLE_COUNT='1')
   else:
    command=[str(args.lusdrender),str(source),str(output),'-rtPreview','-camera','/C','-w','16','-height','16','-threads','1','-bg','0,0,0']
    if mode=='normal':command+=['--aov','shadingNormal']
    env=dict(os.environ)
   (args.out/(stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
   log=args.out/(stem+'.log')
   with log.open('wb')as f:subprocess.run(command,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=120)
   if engine=='sdk':
    assert not reference_render_findings(log)
    diagnostics=json.loads((args.out/(stem+'-diagnostics.json')).read_text());output=Path(diagnostics['frames'][0]['path']);result['sdk_graphics']=diagnostics['graphics']
    assert 'llvmpipe' in diagnostics['graphics']['renderer'].lower(),diagnostics['graphics']
   result['captures'][name][engine+'-'+mode]={'file':str(output),'sha256':sha(output)}
for engine in ('sdk','native'):
 paths=[Path(result['captures'][name][engine+'-normal']['file'])for name in ('neutral','tilted')];a,b=[read_pfm(p)for p in paths];assert a[0]==b[0];delta=max(abs(x-y)for x,y in zip(a[1],b[1]));assert delta<1e-6,(engine,delta)
 pixels=a[1];middle=(8*16+8)*3;assert abs(pixels[middle+2]-1)<1e-6,(engine,pixels[middle:middle+3])
 beauty=compare(Path(result['captures']['neutral'][engine+'-beauty']['file']),Path(result['captures']['tilted'][engine+'-beauty']['file']),args.out/(engine+'-beauty-diff'));assert beauty['normalized_mae']>.001,(engine,beauty)
 result[engine]={'normal_max_delta':delta,'unperturbed_normal':list(pixels[middle:middle+3]),'beauty_change':beauty}
result['passed']=True;(args.out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({engine:{'normal_max_delta':result[engine]['normal_max_delta'],'beauty_change_mae':result[engine]['beauty_change']['normalized_mae']}for engine in ('sdk','native')},indent=2))
