#!/usr/bin/env python3
"""Compare original evaluated LBS points, independently of image coverage.

Run with an SDK-enabled Python: scene time output --native-dump executable.
Analytic native regressions remain the independent contract; SDK agreement alone
is supporting evidence, not proof of USD correctness. No source is rewritten.
"""
import json,subprocess,sys,math
import argparse
from pxr import Usd,UsdGeom,UsdSkel
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("source");parser.add_argument("time",type=float);parser.add_argument("output");parser.add_argument("--native-dump",required=True)
args=parser.parse_args()
source,time,output=args.source,args.time,args.output
if not math.isfinite(time):parser.error("time must be finite")
r=subprocess.run([args.native_dump,source,str(time)],capture_output=True,text=True,timeout=300)
if r.returncode:raise RuntimeError(r.stderr)
native={}
for line in r.stdout.splitlines():
 fields=line.split()
 if fields and fields[0].startswith('/'):
  assert len(fields)==int(fields[1])+2
  native[fields[0]]=list(map(float,fields[2:]))
stage=Usd.Stage.Open(source);cache=UsdSkel.Cache();rows=[]
for prim in stage.Traverse():
 if prim.IsA(UsdSkel.Root):cache.Populate(UsdSkel.Root(prim),Usd.PrimDefaultPredicate)
for prim in stage.Traverse():
 if not prim.IsA(UsdSkel.Root):continue
 for binding in cache.ComputeSkelBindings(UsdSkel.Root(prim),Usd.PrimDefaultPredicate):
  skeleton=binding.GetSkeleton();skel=cache.GetSkelQuery(skeleton)
  palette=skel.ComputeSkinningTransforms(time)
  for query in binding.GetSkinningTargets():
   mesh=query.GetPrim();points=UsdGeom.Mesh(mesh).GetPointsAttr().Get(time)
   assert query.ComputeSkinnedPoints(palette,points,time)
   to_mesh=UsdGeom.Xformable(skeleton).ComputeLocalToWorldTransform(time)*UsdGeom.Xformable(mesh).ComputeLocalToWorldTransform(time).GetInverse()
   values=[float(c)for p in points for c in to_mesh.Transform(p)]
   actual=native.pop(str(mesh.GetPath()),None)
   delta=[abs(a-b)for a,b in zip(actual or [],values)]
   rows.append(dict(path=str(mesh.GetPath()),subdivision=UsdGeom.Mesh(mesh).GetSubdivisionSchemeAttr().Get(),components=len(values),native_components=len(actual or []),max_abs=max(delta,default=None),rms=math.sqrt(sum(d*d for d in delta)/len(delta))if delta else None,passed=actual is not None and len(actual)==len(values)and max(delta,default=0)<1e-4))
result=dict(source=source,time=time,contract='Standard LBS evaluated mesh-local points, no subdivision or image rasterization',meshes=rows,unexpected_native=list(native),passed=bool(rows)and all(row['passed']for row in rows)and not native)
open(output,'w').write(json.dumps(result,indent=2)+'\n');print(json.dumps(dict(passed=result['passed'],meshes=len(rows),max_abs=max((row['max_abs'] for row in rows if row['max_abs'] is not None),default=None),failed=[row['path'] for row in rows if not row['passed']][:10])))

raise SystemExit(0 if result["passed"] else 1)
