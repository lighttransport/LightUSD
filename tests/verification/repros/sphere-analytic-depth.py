# SPDX-License-Identifier: Apache-2.0
# Independent unit-sphere control; requires the audited untransformed default-radius scene.
import sys,json,math,hashlib,argparse
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools/view_control'))
from aov import read_pfm
from PIL import Image,ImageFilter
parser=argparse.ArgumentParser();parser.add_argument('--validation',type=Path,required=True);parser.add_argument('--scene-index',type=int,required=True);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
source=args.validation;q=json.loads(source.read_text());scene=q['scenes'][args.scene_index];frame=scene['frames'][0];
source_scene=Path(q['assets_root'])/scene['scene'];authored=source_scene.read_text();assert authored.count('def Sphere')==1 and 'def Mesh'not in authored and 'xformOp:'not in authored and 'radius ='not in authored
complexity=q['config']['scenes'][args.scene_index].get('reference_complexity',q['config'].get('reference_complexity','low'));
aov=frame['aovs']['depth'];camera=frame['camera'];rfile=Path(aov['reference']);cfile=Path(aov['candidate']);size,r=read_pfm(rfile);_,c=read_pfm(cfile);w,h=size
assert camera['projection']=='perspective' and camera['shift']==[0,0]
assert camera['transform'][:12]==[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.]
origin=camera['transform'][12:15];n=camera['near'];f=camera['far'];tan=math.tan(math.radians(camera['fov_y'])/2);truth=[];mask=[]
for y in range(h):
 for x in range(w):
  direction=[(2*(x+.5)/w-1)*tan*camera['aspect'],(1-2*(y+.5)/h)*tan,-1.];length=math.sqrt(sum(a*a for a in direction));direction=[a/length for a in direction];b=sum(a*d for a,d in zip(origin,direction));disc=b*b-(sum(a*a for a in origin)-1);t=-b-math.sqrt(max(0,disc));mask.append(disc>=0 and t>0);truth.append(t)
result={'scope':'Independent analytic unit Sphere ray intersection in stage units, 320x240, original unmodified MaterialXTest/basic.usda default radius1; no SDK geometry oracle. Numeric approximation error is measured, not a declaration of malformed USD.','camera':camera,'engines':{}}
for name,values in [('openusd-storm-'+complexity,r),('lightusd-native',c)]:
 m=[0<=values[3*i]<1-1e-7 if name.startswith('openusd')else values[3*i]>0 for i in range(w*h)]
 common=[a and b for a,b in zip(mask,m)];interior=Image.frombytes('L',size,bytes(255 if b else 0 for b in common)).filter(ImageFilter.MinFilter(3)).tobytes();errors=[]
 for i,valid in enumerate(interior):
  if valid:
   t=values[3*i]
   if name.startswith('openusd'):
    x,y=i%w,i//w;px=(2*(x+.5)/w-1)*tan*camera['aspect'];py=(1-2*(y+.5)/h)*tan;z=n*f/(f-t*(f-n));t=z*math.sqrt(1+px*px+py*py)
   errors.append(abs(t-truth[i])/truth[i])
 errors.sort();result['engines'][name]={'coverage_iou':sum(common)/sum(a or b for a,b in zip(mask,m)),'reference_hits':sum(mask),'engine_hits':sum(m),'interior_samples':len(errors),'relative_depth_p95':errors[int(.95*len(errors))],'relative_depth_max':max(errors)}
result['binary_sha256']={name:q['toolchain'][name]['sha256']for name in ('lusdrender','usdrecord')}
result['artifacts']=[{'name':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}for p in [source,source_scene,rfile,cfile,Path(__file__)]]
args.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['engines'],indent=2))
