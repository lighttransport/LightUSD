#!/usr/bin/env python3
"""USD-backed preparation/inspection worker for immutable headless render jobs."""
import importlib.util
import json
import math
from pathlib import Path
import sys


def preview(value):
    if type(value) is float and not math.isfinite(value): return str(value)
    if value is None or type(value) in (bool,int,float): return value
    if isinstance(value,str): return value[:4096]
    if getattr(value,'isArray',False) or type(value).__module__=='pxr.Vt':
        try:
            count=len(value)
            return {'count':count,'items':[str(value[i])[:256] for i in range(min(count,16))], 'truncated':count>16}
        except TypeError: pass
    return str(value)[:4096]


def inspect(stage, path, time, offset=0, limit=64):
    prim=stage.GetPrimAtPath(path)
    if not prim: raise ValueError('prim not found: '+path)
    names=sorted(prim.GetPropertyNames());rows=[]
    for name in names[offset:offset+limit]:
        prop=prim.GetProperty(name)
        stack=prop.GetPropertyStack(time)
        row={'name':name,'path':str(prop.GetPath()),'provenance':[{'layer':p.layer.identifier,'path':str(p.path)} for p in stack[:8]], 'provenance_count':len(stack)}
        if hasattr(prop,'GetTargets'):
            targets=prop.GetTargets();row.update(kind='relationship',targets=[str(p) for p in targets[:128]],target_count=len(targets))
        else:
            links=prop.GetConnections()
            row.update(kind='attribute',type=str(prop.GetTypeName()),value_preview=preview(prop.Get(time)),
                       connections=[str(p) for p in links[:128]],connection_count=len(links),
                       time_sample_count=prop.GetNumTimeSamples(),authored=prop.HasAuthoredValueOpinion())
        row['metadata']={k:preview(prop.GetMetadata(k)) for k in ('documentation','displayName','displayGroup','interpolation','colorSpace') if prop.HasAuthoredMetadata(k)}
        rows.append(row)
    return {'implementation':'OpenUSD','path':path,'type':prim.GetTypeName(),'properties':rows,'total':len(names),
            'offset':offset,'next_offset':offset+limit if offset+limit<len(names) else None,
            'variants':{name:prim.GetVariantSet(name).GetVariantSelection() for name in prim.GetVariantSets().GetNames()}}


def closed_population_mask(stage, roots):
    """A reference-only working set, closed over relationships and connections."""
    from pxr import Usd,Sdf
    mask=Usd.StagePopulationMask()
    for name in roots:
        path=Sdf.Path(name)
        if not path.IsAbsolutePath() or not path.IsPrimPath() or path==Sdf.Path.absoluteRootPath or not stage.GetPrimAtPath(path):
            raise ValueError('invalid population root: '+name)
        mask.Add(path)
    for prim in stage.GetPseudoRoot().GetChildren():
        if prim.GetName().startswith(('__ViewControl','__view_control')):
            mask.Add(prim.GetPath())
    selected=Usd.Stage.OpenMasked(stage.GetRootLayer(),mask)
    if not selected:raise ValueError('cannot open reference working set')
    selected.ExpandPopulationMask(Usd.TraverseInstanceProxies())
    paths=[str(p) for p in selected.GetPopulationMask().GetPaths()]
    if len(paths)>4096 or len(','.join(paths).encode())>65536:
        raise ValueError('reference population mask exceeds bounded command size')
    return {'paths':paths,'roots':list(roots),'closure':'All relationship targets and attribute connections, including instance proxies. Native source remains unchanged; reference working-set timings are not full-scene timings.'}


def prepare(request):
    from pxr import Ar, Gf, Sdf, Usd, UsdGeom, UsdShade, UsdSkel, Vt
    state=request['state'];directory=Path(request['directory'])
    source=Usd.Stage.Open(state['scene'])
    if not source: raise ValueError('could not open scene')
    # Load the shared editor directly, without importing the usdview Qt package.
    import pxr
    modules=[Path(root)/'Usdviewq/debugOverlay.py' for root in pxr.__path__]
    module=next((p for p in modules if p.is_file()),None)
    if module is None: raise ValueError('the installed fork must include usdviewq/debugOverlay.py')
    spec=importlib.util.spec_from_file_location('batchDebugOverlay',module)
    editorModule=importlib.util.module_from_spec(spec);spec.loader.exec_module(editorModule)
    editor=editorModule.DebugOverlay(source,directory/'debug')
    stage=Usd.Stage.Open(editor.root)
    if state['edits']:
        transaction=editor.prepare(stage,'edit',state['edits'])['transaction']
        editor.commit(transaction);editor.accept(transaction)
    time=Usd.TimeCode(state['time'] if state['time'] is not None else stage.GetStartTimeCode())
    purposes=[getattr(UsdGeom.Tokens,'default_' if name=='default' else name) for name in ('default','render','proxy','guide') if name=='default' or state['display'][name]]
    bounds=UsdGeom.BBoxCache(time,purposes,useExtentsHint=True)
    camera=state['camera']
    if camera is None:
        authored=UsdGeom.Camera(stage.GetPrimAtPath(state['camera_path'])) if state.get('camera_path') else None
        if state.get('camera_path') and not authored: raise ValueError('camera not found')
        if authored:
            resolved=authored.GetCamera(time)
            # Preserve vertical filmback when conforming an authored camera.
            resolved.horizontalAperture=resolved.verticalAperture*state['size'][0]/state['size'][1]
        else:
            box=bounds.ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedRange()
            center=box.GetMidpoint() if not box.IsEmpty() else Gf.Vec3d(0)
            radius=max(.1,box.GetSize().GetLength()*.5) if not box.IsEmpty() else 1
            aspect=state['size'][0]/state['size'][1]
            distance=radius*1.1/min(math.sin(math.radians(30)),math.sin(math.atan(math.tan(math.radians(30))*aspect)))
            zup=UsdGeom.GetStageUpAxis(stage)==UsdGeom.Tokens.z
            eye=center+(Gf.Vec3d(0,-distance,0) if zup else Gf.Vec3d(0,0,distance))
            up=Gf.Vec3d(0,0,1) if zup else Gf.Vec3d(0,1,0)
            resolved=Gf.Camera()
            resolved.transform=Gf.Matrix4d().SetLookAt(eye,center,up).GetInverse()
            resolved.SetPerspectiveFromAspectRatioAndFieldOfView(aspect,60,Gf.Camera.FOVVertical)
            resolved.clippingRange=Gf.Range1f(max(.0001,distance-radius*1.5),max(10,distance+radius*3))
            resolved.focusDistance=distance
        camera={'transform':[float(resolved.transform[r][c]) for r in range(4) for c in range(4)],
                'projection':'orthographic' if resolved.projection==Gf.Camera.Orthographic else 'perspective',
                'aspect':float(resolved.aspectRatio),'fov_y':float(resolved.GetFieldOfView(Gf.Camera.FOVVertical)) if resolved.projection==Gf.Camera.Perspective else 60,
                'ortho_height':float(resolved.verticalAperture*Gf.Camera.APERTURE_UNIT),
                'shift':[float(2*resolved.horizontalApertureOffset/resolved.horizontalAperture),float(2*resolved.verticalApertureOffset/resolved.verticalAperture)],
                'near':float(resolved.clippingRange.GetMin()),'far':float(resolved.clippingRange.GetMax()),
                'distance':max(.001,float(resolved.focusDistance)),'exposure':0}
    else:
        resolved=Gf.Camera()
        if camera['projection']=='orthographic':
            resolved.SetOrthographicFromAspectRatioAndSize(camera['aspect'],camera['ortho_height'],Gf.Camera.FOVVertical)
        else: resolved.SetPerspectiveFromAspectRatioAndFieldOfView(camera['aspect'],camera['fov_y'],Gf.Camera.FOVVertical)
        resolved.transform=Gf.Matrix4d(*camera['transform'])
        resolved.horizontalApertureOffset=camera['shift'][0]*resolved.horizontalAperture*.5
        resolved.verticalApertureOffset=camera['shift'][1]*resolved.verticalAperture*.5
        resolved.clippingRange=Gf.Range1f(camera['near'],camera['far'])
        resolved.focusDistance=camera['distance']
    # One frozen diagnostic layer feeds both renderers. Source layers are read-only.
    layer=Sdf.Layer.CreateNew(str(directory/'diagnostics.usda'))
    root=Sdf.Layer.CreateNew(str(directory/'scene.usda'))
    root.TransferContent(editor.root);root.subLayerPaths=[layer.realPath,editor.root.realPath]
    stage=Usd.Stage.Open(root);stage.SetEditTarget(layer)
    path='/__ViewControl'
    while stage.GetPrimAtPath(path): path+='X'
    UsdGeom.Xform.Define(stage,path)
    cameraPrim=UsdGeom.Camera.Define(stage,path+'/Camera');cameraPrim.SetFromCamera(resolved,Usd.TimeCode.Default())
    # Bake diagnostic annotations into a common, emissive near-plane mesh.
    # This gives both engines the same readable overlay without native HUDs.
    inverse=resolved.transform.GetInverse()
    near=float(resolved.clippingRange.GetMin());far=float(resolved.clippingRange.GetMax())
    targetDepth=near+min(max(.001,near*.01),(far-near)*.25)
    halfHeight=(float(resolved.verticalAperture)*Gf.Camera.APERTURE_UNIT*.5 if resolved.projection==Gf.Camera.Orthographic
                else targetDepth*math.tan(math.radians(camera['fov_y'])*.5))
    halfWidth=halfHeight*2.0/state['size'][1]  # two-pixel-wide strokes
    lineCount=0
    def lines(name,segments,color):
        nonlocal lineCount
        if not segments: return
        if len(segments)>250000: raise ValueError('diagnostic overlay exceeds 250000 segments')
        points=[];indices=[]
        for a,b in segments:
            a=inverse.Transform(Gf.Vec3d(*a));b=inverse.Transform(Gf.Vec3d(*b))
            # Clip the segment in depth before projecting its screen-space annotation.
            da,db=-a[2],-b[2]
            low,high=0.0,1.0
            for plane,sign in ((near,1),(far,-1)):
                va,vb=(da-plane)*sign,(db-plane)*sign
                if va<0 and vb<0: low,high=1,0;break
                if va<0: low=max(low,va/(va-vb))
                if vb<0: high=min(high,va/(va-vb))
            if low>high: continue
            delta=b-a;a,b=a+delta*low,a+delta*high
            projected=[]
            for point in (a,b):
                scale=1.0 if resolved.projection==Gf.Camera.Orthographic else targetDepth/(-point[2])
                projected.append(Gf.Vec3d(point[0]*scale,point[1]*scale,-targetDepth))
            a,b=projected;dx,dy=b[0]-a[0],b[1]-a[1]
            length=math.hypot(dx,dy)
            if length<1e-12: continue
            lateral=Gf.Vec3d(-dy/length*halfWidth,dx/length*halfWidth,0)
            base=len(points)
            points.extend(Gf.Vec3f(*resolved.transform.Transform(point)) for point in (a-lateral,b-lateral,b+lateral,a+lateral))
            indices.extend((base,base+1,base+2,base+3))
        if not points: return
        mesh=UsdGeom.Mesh.Define(stage,path+'/'+name)
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none);mesh.CreateDoubleSidedAttr(True)
        mesh.CreateFaceVertexCountsAttr([4]*(len(points)//4));mesh.CreateFaceVertexIndicesAttr(indices)
        mesh.CreatePointsAttr(Vt.Vec3fArray(points));mesh.CreateDisplayColorAttr([Gf.Vec3f(*color)])
        normal=Gf.Vec3f(*resolved.transform.TransformDir(Gf.Vec3d(0,0,1)))
        mesh.CreateNormalsAttr(Vt.Vec3fArray([normal]*len(points)));mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
        material=UsdShade.Material.Define(stage,path+'/'+name+'Material')
        shader=UsdShade.Shader.Define(stage,str(material.GetPath())+'/Shader')
        shader.CreateIdAttr('UsdPreviewSurface')
        shader.CreateInput('diffuseColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0))
        shader.CreateInput('emissiveColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
        shader.CreateInput('opacity',Sdf.ValueTypeNames.Float).Set(1.0)
        shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
        material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
        UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
        lineCount+=len(points)//4
    if state['display']['skeleton']:
        cache=UsdSkel.Cache()
        for prim in stage.Traverse():
            if prim.IsA(UsdSkel.Root): cache.Populate(UsdSkel.Root(prim),Usd.PrimDefaultPredicate)
        segments=[]
        for prim in list(stage.Traverse()):
            if not prim.IsA(UsdSkel.Skeleton) or UsdGeom.Imageable(prim).ComputeVisibility(time)==UsdGeom.Tokens.invisible: continue
            query=cache.GetSkelQuery(UsdSkel.Skeleton(prim))
            if not query: continue
            matrices=query.ComputeJointSkelTransforms(time)
            world=UsdGeom.XformCache(time).GetLocalToWorldTransform(prim)
            positions=[(m*world).ExtractTranslation() for m in matrices]
            parents=query.GetTopology().GetParentIndices()
            segments.extend((positions[parent],positions[i]) for i,parent in enumerate(parents) if 0<=parent<len(positions))
        lines('Bones',segments,(0.1,.8,.9))
    if state['display']['bboxes'] or state['selection']:
        segments=[]
        for selected in (state['selection'] or ['/'] if state['display']['bboxes'] else state['selection']):
            prim=stage.GetPrimAtPath(selected)
            if not prim: raise ValueError('selected prim not found: '+selected)
            box=bounds.ComputeWorldBound(prim).ComputeAlignedRange()
            if box.IsEmpty(): continue
            corners=[Gf.Vec3d(*(box.GetMax()[axis] if i&(1<<axis) else box.GetMin()[axis] for axis in range(3))) for i in range(8)]
            segments.extend((corners[i],corners[i^(1<<axis)]) for i in range(8) for axis in range(3) if not i&(1<<axis))
        lines('SelectedBounds',segments,(1,.7,.1))
    layer.Save();root.Save()
    dependencies=[];seen=set()
    for used in stage.GetUsedLayers():
        if used.realPath:
            filename=used.realPath
            while Ar.IsPackageRelativePath(filename): filename=Ar.SplitPackageRelativePathOuter(filename)[0]
            p=Path(filename)
            if str(p) in seen: continue
            seen.add(str(p));stat=p.stat()
            dependencies.append({'path':str(p),'size':stat.st_size,'mtime_ns':stat.st_mtime_ns})
    result={'wrapper':root.realPath,'camera_path':str(cameraPrim.GetPath()),'camera':camera,'time':time.GetValue(),
            'dependencies':dependencies,'diagnostic_segments':lineCount,'source':state['scene']}
    if request.get('reference_population_roots'):
        result['reference_population_mask']=closed_population_mask(stage,request['reference_population_roots'])
    if request.get('inspect'):
        args=request['inspect'];result['inspection']=inspect(stage,args['path'],time,args.get('offset',0),args.get('limit',64))
    return result


if __name__=='__main__':
    request=Path(sys.argv[1]);response=Path(sys.argv[2])
    if request.stat().st_size>1024*1024: raise ValueError('preparation request exceeds 1 MiB')
    result=prepare(json.loads(request.read_text()))
    response.write_text(json.dumps(result,allow_nan=False,indent=2)+'\n')
