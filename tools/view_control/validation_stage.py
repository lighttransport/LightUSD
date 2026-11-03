#!/usr/bin/env python3
"""SDK-only stage inventory and read-only-source diagnostic material preparation."""
import json
from pathlib import Path
import sys


def probe(scene):
    from pxr import Usd, UsdSkel
    stage=Usd.Stage.Open(scene)
    if not stage: raise ValueError('cannot open scene')
    # Declared sampling bounds already determine job times. Enumerating every
    # attribute's clip samples on a large stage only produces advisory counts,
    # and retains work for hidden geometry after resource-preserving isolation.
    if stage.HasAuthoredMetadata('startTimeCode') and stage.HasAuthoredMetadata('endTimeCode'):
        return {'start':stage.GetStartTimeCode(),'end':stage.GetEndTimeCode(),
                'authored_sample_range':None,'animated_attributes':None,
                'skeleton_roots':None,'inventory_mode':'authored-stage-range; attribute samples not enumerated',
                'time_codes_per_second':stage.GetTimeCodesPerSecond()}
    first=last=None;animated=0;roots=0
    for prim in stage.Traverse():
        roots+=prim.IsA(UsdSkel.Root)
        for attr in prim.GetAttributes():
            times=attr.GetTimeSamples()
            if times:
                animated+=1
                first=min(first,times[0]) if first is not None else times[0]
                last=max(last,times[-1]) if last is not None else times[-1]
    start,end=stage.GetStartTimeCode(),stage.GetEndTimeCode()
    if start==end and first is not None: start,end=first,last
    return {'start':start,'end':end,'authored_sample_range':[first,last],
            'animated_attributes':animated,'skeleton_roots':roots,
            'time_codes_per_second':stage.GetTimeCodesPerSecond()}


def geometry_aov(scene, output):
    """An opaque geometry diagnostic; no point/topology/time values are baked."""
    from pxr import Usd,UsdGeom,UsdShade,Sdf,Gf
    source=Usd.Stage.Open(scene)
    if not source:raise ValueError('cannot open geometry diagnostic source')
    scope='/__view_control_opaque'
    if source.GetPrimAtPath(scope):raise ValueError('geometry diagnostic namespace collision')
    layer=Sdf.Layer.CreateNew(str(output));stage=Usd.Stage.Open(layer)
    material=UsdShade.Material.Define(stage,scope+'/Material')
    shader=UsdShade.Shader.Define(stage,scope+'/Shader');shader.CreateIdAttr('UsdPreviewSurface')
    shader.CreateInput('diffuseColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0))
    shader.CreateInput('emissiveColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(1))
    shader.CreateInput('opacity',Sdf.ValueTypeNames.Float).Set(1.)
    shader.CreateOutput('surface',Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),'surface')
    for prim in source.GetPseudoRoot().GetChildren():
        root=stage.OverridePrim(prim.GetPath())
        binding=UsdShade.MaterialBindingAPI.Apply(root)
        binding.Bind(material,UsdShade.Tokens.strongerThanDescendants)
        binding.Bind(material,UsdShade.Tokens.strongerThanDescendants,UsdShade.Tokens.preview)
    layer.subLayerPaths=[str(Path(scene).resolve())];layer.Save()
    failures=[];count=0
    for prim in Usd.PrimRange.Stage(stage,Usd.TraverseInstanceProxies()):
        if not prim.IsA(UsdGeom.Gprim):continue
        count+=1
        for purpose in (UsdShade.Tokens.allPurpose,UsdShade.Tokens.preview):
            bound,_=UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(purpose)
            if not bound or bound.GetPath()!=material.GetPath():
                failures.append(str(prim.GetPath())+': '+str(purpose))
    return dict(geometry_diagnostic_revision=2,wrapper=str(output),gprims=count,
                unsupported=failures,supported=bool(count) and not failures,
                semantics='Strong shared opaque emissive material overrides universal AND preview-purpose bindings: cutout opacity, material normal maps and shader displacement excluded. Both purposes verified on every Gprim. Source points, topology, visibility, purposes, transforms, skeletons and time remain referenced.')


def normal_aov(scene, output, time=0, purposes=None):
    """Remove Storm's display-facing flip without changing source geometry.

    Storm's default CullStyle is NOTHING. Neye nevertheless flips normals on
    double-sided backfaces. Native shadingNormal is deliberately unoriented.
    Detached doubleSided overrides make those channels comparable. Instance
    proxies and non-mesh surfaces cannot silently count as calibrated coverage.
    """
    from pxr import Usd,UsdGeom,Sdf
    source=Usd.Stage.Open(scene)
    if not source:raise ValueError('cannot open normal diagnostic source')
    purposes=purposes or ['default','render']
    def visible(prim):
        imageable=UsdGeom.Imageable(prim)
        return (not imageable or (imageable.ComputeVisibility(Usd.TimeCode(time))!=UsdGeom.Tokens.invisible
                and str(imageable.ComputePurpose()) in purposes))
    layer=Sdf.Layer.CreateNew(str(output))
    layer.subLayerPaths=[str(Path(scene).resolve())]
    stage=Usd.Stage.Open(layer)
    # Instance proxies cannot receive display overrides. Disable only the
    # necessary SDK diagnostic instances in this detached layer; referenced
    # geometry, clips and skeletons remain untouched (no point-array bake).
    deinstanced=[]
    for _ in range(32):
        roots=set()
        for prim in Usd.PrimRange.Stage(stage,Usd.TraverseInstanceProxies()):
            if not visible(prim) or not prim.IsA(UsdGeom.Mesh) or not prim.IsInstanceProxy():continue
            if not UsdGeom.Mesh(prim).GetDoubleSidedAttr().Get():continue
            root=prim
            while root.IsInstanceProxy():root=root.GetParent()
            if root.IsInstance():roots.add(str(root.GetPath()))
        if not roots:break
        if len(deinstanced)+len(roots)>4096:
            raise ValueError('normal diagnostic exceeds 4096 deinstance overrides')
        with Sdf.ChangeBlock():
            for path in sorted(roots):
                Sdf.CreatePrimInLayer(layer,Sdf.Path(path)).SetInfo('instanceable',False)
                deinstanced.append(path)
    else:raise ValueError('normal diagnostic exceeds 32 nested instance levels')
    changed=[];unsupported=[];meshes=0
    for prim in Usd.PrimRange.Stage(stage,Usd.TraverseInstanceProxies()):
        if not visible(prim):continue
        if prim.IsA(UsdGeom.Mesh):
            meshes+=1
            if not UsdGeom.Mesh(prim).GetDoubleSidedAttr().Get():continue
            if prim.IsInstanceProxy():unsupported.append(str(prim.GetPath())+': uneditable double-sided instance proxy');continue
            changed.append(str(prim.GetPath()))
        elif prim.IsA(UsdGeom.Curves):
            unsupported.append(str(prim.GetPath())+': curve normal orientation is not calibrated')
    # Author all attributes in one notification block. Rebuilding a composed
    # scene after each display edit made this quadratic for large instance sets.
    with Sdf.ChangeBlock():
        for path in changed:
            spec=Sdf.CreatePrimInLayer(layer,Sdf.Path(path))
            attribute=Sdf.AttributeSpec(spec,'doubleSided',Sdf.ValueTypeNames.Bool)
            attribute.default=False
    layer.Save()
    return dict(normal_diagnostic_revision=3,wrapper=str(output),mesh_count=meshes,
                double_sided_overrides=changed,deinstanced_for_display_override=deinstanced,
                unsupported=unsupported,supported=bool(meshes) and not unsupported,
                semantics='SDK Neye with detached doubleSided=false overrides and default CullStyle NOTHING; necessary SDK instances deinstanced only for display overrides, never baked. Positions, topology, authored normals and animation remain source-referenced. Native shadingNormal is unadjusted. Material normal maps excluded; diagnostic deinstancing is not comparable performance work.')


def scalar_texture_aov(source,stage,connection,output,time,index):
    """Replicate one encoded channel; both engines sample the same derived image.

    This does not bake evaluated material values or geometry from the SDK.
    Unsupported graphs and image modes remain explicit coverage failures.
    """
    from pxr import Usd,UsdShade,Sdf,Gf,Ar
    import hashlib,io,warnings
    try:
        import PIL
        from PIL import Image
    except ImportError as error:
        raise ValueError('scalar texture adapter requires Pillow in SDK Python') from error
    channel=str(connection).rsplit('.outputs:',1)
    if len(channel)!=2 or channel[1] not in ('r','g','b','a'):
        raise ValueError('scalar texture connection is not a direct r/g/b/a output')
    texture=UsdShade.Shader(source.GetPrimAtPath(channel[0]))
    if not texture or texture.GetIdAttr().Get()!='UsdUVTexture':
        raise ValueError('scalar texture connection is not UsdUVTexture')
    lane='rgba'.index(channel[1]);file_input=texture.GetInput('file')
    if file_input and file_input.GetAttr().GetConnections():
        raise ValueError('connected scalar texture file input is unsupported')
    asset=file_input.Get(Usd.TimeCode(time)) if file_input else None
    if not asset or not asset.path or '<UDIM>' in asset.path:
        raise ValueError('scalar texture has no single image asset')
    resolved=asset.resolvedPath
    if not resolved:
        stack=file_input.GetAttr().GetPropertyStack(Usd.TimeCode(time))
        if not stack:raise ValueError('scalar texture asset has no authored layer')
        resolved=str(Ar.GetResolver().Resolve(Sdf.ComputeAssetPathRelativeToLayer(stack[0].layer,asset.path)))
    handle=Ar.GetResolver().OpenAsset(Ar.ResolvedPath(resolved)) if resolved else None
    if not handle or not 0<handle.GetSize()<=64*1024*1024:
        raise ValueError('scalar texture is missing or exceeds 64MiB encoded limit')
    data=handle.GetBuffer()
    with warnings.catch_warnings():
        warnings.simplefilter('error',Image.DecompressionBombWarning)
        with Image.open(io.BytesIO(data)) as image:
            if image.width*image.height>16*1024*1024 or image.mode not in ('RGB','RGBA','L','LA','P'):
                raise ValueError('scalar texture exceeds 16M pixels or is not an 8-bit image')
            selected=image.convert('RGBA').getchannel('RGBA'[lane])
            derived=Image.merge('RGB',(selected,selected,selected))
            path=Path(output).parent/(Path(output).stem+'-scalar-'+str(index)+'.png')
            derived.save(path)
    target=UsdShade.Shader.Define(stage,'/__ValidationScalarTextures/T'+str(index))
    target.CreateIdAttr('UsdUVTexture')
    for name in ('st','wrapS','wrapT','sourceColorSpace'):
        original=texture.GetInput(name)
        if not original:continue
        inp=target.CreateInput(name,original.GetTypeName())
        links=original.GetAttr().GetConnections()
        if links:inp.GetAttr().SetConnections(links)
        else:
            value=original.Get(Usd.TimeCode(time))
            if value is not None:inp.Set(value)
    # RGB channels retain their authored transfer function; alpha is linear.
    if lane==3:
        space=target.CreateInput('sourceColorSpace',Sdf.ValueTypeNames.Token)
        space.GetAttr().SetConnections([]);space.Set('raw')
    target_file=target.CreateInput('file',Sdf.ValueTypeNames.Asset)
    target_file.Set(Sdf.AssetPath(str(path.resolve())))
    color_space=file_input.GetAttr().GetMetadata('colorSpace')
    if color_space and lane!=3:target_file.GetAttr().SetMetadata('colorSpace',color_space)
    for name,default in [('scale',Gf.Vec4f(1)),('bias',Gf.Vec4f(0)),('fallback',Gf.Vec4f(0,0,0,1))]:
        original=texture.GetInput(name)
        if original and original.GetAttr().GetConnections():
            raise ValueError('connected scalar texture '+name+' is unsupported')
        value=original.Get(Usd.TimeCode(time)) if original else None
        value=default if value is None else value
        target.CreateInput(name,Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(value[lane],value[lane],value[lane],1 if name!='bias' else 0))
    target.CreateOutput('rgb',Sdf.ValueTypeNames.Float3)
    return target,dict(source=resolved,source_sha256=hashlib.sha256(data).hexdigest(),channel=channel[1],
                       derived=str(path),derived_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                       size=[derived.width,derived.height],adapter='encoded-channel-replication-v1',pillow=PIL.__version__,
                       decoder_sha256=hashlib.sha256(Path(Image.__file__).read_bytes()).hexdigest(),
                       decoder_native_sha256=hashlib.sha256(Path(Image.core.__file__).read_bytes()).hexdigest())


def material_aov(scene,output,time,aov,purposes=None):
    from pxr import Usd,UsdGeom,UsdShade,Sdf,Gf
    source=Usd.Stage.Open(scene)
    purposes=purposes or ['default','render']
    layer=Sdf.Layer.CreateNew(str(output))
    # Author detached overrides; attach the source once, avoiding large-stage
    # recomposition after every input edit.
    stage=Usd.Stage.Open(layer)
    for key in ('upAxis','metersPerUnit','timeCodesPerSecond','framesPerSecond','startTimeCode','endTimeCode','defaultPrim'):
        if source.HasAuthoredMetadata(key): stage.SetMetadata(key,source.GetMetadata(key))
    converted=[];unsupported=[];texture_adapters=[]
    field={'baseColor':'diffuseColor','roughness':'roughness','metallic':'metallic'}[aov]
    visible_geometry=[];surface_paths=set()
    for prim in Usd.PrimRange.Stage(source,Usd.TraverseInstanceProxies()):
        if not (prim.IsA(UsdGeom.Gprim) or prim.IsA(UsdGeom.PointInstancer) or prim.IsA(UsdGeom.Subset)):continue
        imageable=UsdGeom.Imageable(prim)
        if imageable and imageable.ComputeVisibility(Usd.TimeCode(time))==UsdGeom.Tokens.invisible:continue
        if imageable and str(imageable.ComputePurpose()) not in purposes:continue
        visible_geometry.append(prim)
        material,_=UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(UsdShade.Tokens.preview)
        if material:
            surface,_,_=material.ComputeSurfaceSource()
            if surface:surface_paths.add(str(surface.GetPath()))
    for path in sorted(surface_paths):
        prim=source.GetPrimAtPath(path)
        if prim.IsInstanceProxy():
            unsupported.append(path+': uneditable material instance proxy');continue
        shader=UsdShade.Shader(prim)
        if not shader: continue
        ident=shader.GetIdAttr().Get()
        if ident!='UsdPreviewSurface':
            if shader.GetOutput('surface'): unsupported.append(str(prim.GetPath()))
            continue
        value=shader.GetInput(field);connections=value.GetAttr().GetConnections() if value else []
        scalar_texture=None
        if aov!='baseColor' and connections:
            try:
                if len(connections)!=1:raise ValueError('multiple scalar texture connections')
                scalar_texture,adapter=scalar_texture_aov(source,stage,connections[0],output,time,len(texture_adapters))
                texture_adapters.append(adapter)
            except (ValueError,OSError) as error:
                unsupported.append(str(prim.GetPath())+': '+str(error));continue
        target=UsdShade.Shader.Define(stage,prim.GetPath())
        emission=target.CreateInput('emissiveColor',Sdf.ValueTypeNames.Color3f)
        emission.GetAttr().SetConnections([])
        if scalar_texture:emission.ConnectToSource(scalar_texture.ConnectableAPI(),'rgb')
        elif connections: emission.GetAttr().SetConnections(connections)
        else:
            raw=value.Get(Usd.TimeCode(time)) if value else None
            if raw is None: raw={'baseColor':Gf.Vec3f(.18),'roughness':.5,'metallic':0}[aov]
            emission.Set(Gf.Vec3f(raw) if aov=='baseColor' else Gf.Vec3f(float(raw)))
        for name,val in [('diffuseColor',Gf.Vec3f(0)),('specularColor',Gf.Vec3f(0))]:
            inp=target.CreateInput(name,Sdf.ValueTypeNames.Color3f);inp.GetAttr().SetConnections([]);inp.Set(val)
        for name,val in [('metallic',0.),('roughness',1.),('useSpecularWorkflow',1)]:
            inp=target.CreateInput(name,Sdf.ValueTypeNames.Int if name=='useSpecularWorkflow' else Sdf.ValueTypeNames.Float)
            inp.GetAttr().SetConnections([]);inp.Set(val)
        source_normal=shader.GetInput('normal')
        if source_normal:
            normal=target.CreateInput('normal',source_normal.GetTypeName())
            normal.GetAttr().SetConnections([]);normal.Set(Gf.Vec3f(0,0,1))
        converted.append(str(prim.GetPath()))
    # Score actual visible bindings, not merely the presence of an unused
    # PreviewSurface somewhere in the stage. Renderer-specific surfaces and
    # prototype materials we could not override are explicitly unsupported.
    coverage=[];converted_set=set(converted)
    for prim in visible_geometry:
        material,_=UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(UsdShade.Tokens.preview)
        if not material:
            # A PointInstancer delegates materials to its prototype geometry.
            if not prim.IsA(UsdGeom.PointInstancer):unsupported.append('unbound '+str(prim.GetPath()))
            continue
        surface,_,_=material.ComputeSurfaceSource()
        path=str(surface.GetPath()) if surface else ''
        coverage.append({'prim':str(prim.GetPath()),'material':str(material.GetPath()),'surface':path})
        if path not in converted_set:unsupported.append('unconverted bound surface '+str(material.GetPath()))
    unsupported=sorted(set(unsupported))
    layer.subLayerPaths=[str(Path(scene).resolve())]
    layer.Save()
    return {'material_diagnostic_revision':6,'wrapper':str(output),'aov':aov,'converted':converted,'unsupported':unsupported,'binding_coverage':coverage,'texture_adapters':texture_adapters,
            'supported':bool(converted) and bool(coverage) and not unsupported,
            'material_binding_purpose':'preview','semantics':'Preview-purpose (with all-purpose fallback) shared UsdPreviewSurface emissive diagnostic, sRGB output; source opacity retained. Not native albedo/BRDF integration.'}


def isolate(scene,output,paths):
    """Hide unrelated imageable prims without removing shared USD resources."""
    from pxr import Usd,Sdf,UsdGeom
    source=Usd.Stage.Open(scene)
    for path in paths:
        if not source.GetPrimAtPath(path):raise ValueError('isolate prim not found: '+path)
    layer=Sdf.Layer.CreateNew(str(output));layer.subLayerPaths=[str(Path(scene).resolve())]
    stage=Usd.Stage.Open(layer)
    for key in ('upAxis','metersPerUnit','timeCodesPerSecond','framesPerSecond','startTimeCode','endTimeCode','defaultPrim'):
        if source.HasAuthoredMetadata(key):stage.SetMetadata(key,source.GetMetadata(key))
    selected=[Sdf.Path(p) for p in paths];protected=set(selected)
    # Follow external geometry/skeleton dependencies before choosing visibility
    # boundaries. Material and shader prims remain active regardless of visibility.
    pending=list(selected);seen=set()
    while pending:
        root=pending.pop()
        if root in seen:continue
        seen.add(root)
        prim=source.GetPrimAtPath(root)
        if not prim:continue
        for child in Usd.PrimRange(prim):
            for rel in child.GetRelationships():
                name=rel.GetName()
                if name=='prototypes' or name in ('skel:skeleton','skel:animationSource','skel:blendShapeTargets'):
                    for target in rel.GetForwardedTargets():
                        target=target.GetPrimPath()
                        if target not in protected:protected.add(target);pending.append(target)
    hidden=[]
    # Keep ancestors of dependency roots visible: visibility is inherited by
    # PointInstancer prototypes and cannot be repaired by a child opinion.
    with Sdf.ChangeBlock():
        for prim in list(stage.Traverse()):
            path=prim.GetPath()
            if any(path.HasPrefix(p) or p.HasPrefix(path) for p in protected):continue
            if any(path.HasPrefix(Sdf.Path(p)) for p in hidden):continue
            imageable=UsdGeom.Imageable(prim)
            if imageable:
                imageable.CreateVisibilityAttr().Set(UsdGeom.Tokens.invisible)
                hidden.append(str(path))
    layer.Save()
    return {'wrapper':str(output),'isolate':paths,'hidden':hidden,
            'protected_dependencies':sorted(str(p) for p in protected),'source':scene}


def evaluated_snapshot(scene,output,time):
    """Explicit SDK-evaluated control; not a native composition validation."""
    from pxr import Usd,Sdf
    source=Usd.Stage.Open(scene);stage=Usd.Stage.CreateNew(str(output));count=0
    for key in ('upAxis','metersPerUnit','timeCodesPerSecond','framesPerSecond','defaultPrim'):
        if source.HasAuthoredMetadata(key):stage.SetMetadata(key,source.GetMetadata(key))
    stage.SetStartTimeCode(time);stage.SetEndTimeCode(time)
    for prim in Usd.PrimRange.Stage(source,Usd.TraverseInstanceProxies()):
        target=stage.DefinePrim(prim.GetPath(),prim.GetTypeName());count+=1
        schemas=prim.GetMetadata('apiSchemas')
        if schemas:target.SetMetadata('apiSchemas',Sdf.TokenListOp.CreateExplicit(schemas.GetAppliedItems()))
        for name in ('kind','customData','assetInfo'):
            if prim.HasAuthoredMetadata(name):target.SetMetadata(name,prim.GetMetadata(name))
        for attr in prim.GetAuthoredAttributes():
            copied=target.CreateAttribute(attr.GetName(),attr.GetTypeName(),attr.IsCustom(),attr.GetVariability())
            for key in ('interpolation','elementSize','colorSpace'):
                if attr.HasAuthoredMetadata(key):copied.SetMetadata(key,attr.GetMetadata(key))
            value=attr.Get(Usd.TimeCode(time))
            if isinstance(value,Sdf.AssetPath) and value.resolvedPath:value=Sdf.AssetPath(value.resolvedPath)
            if value is not None:copied.Set(value)
            elif attr.HasAuthoredValueOpinion():copied.Block()
            connections=attr.GetConnections()
            if connections:copied.SetConnections(connections)
        for rel in prim.GetAuthoredRelationships():target.CreateRelationship(rel.GetName(),rel.IsCustom()).SetTargets(rel.GetTargets())
    stage.GetRootLayer().Save()
    return {'wrapper':str(output),'source':scene,'time':time,'prims':count,
            'control':'SDK-evaluated authored-property snapshot with resolved composition/time; original native load is not validated by this control.'}


if __name__=='__main__':
    request=json.loads(Path(sys.argv[1]).read_text())
    if request['operation']=='probe':result=probe(request['scene'])
    elif request['operation']=='isolate':result=isolate(request['scene'],Path(request['output']),request['paths'])
    elif request['operation']=='snapshot':result=evaluated_snapshot(request['scene'],Path(request['output']),request['time'])
    elif request['operation']=='geometry':result=geometry_aov(request['scene'],Path(request['output']))
    elif request['operation']=='normal':result=normal_aov(request['scene'],Path(request['output']),request.get('time',0),request.get('purposes'))
    elif request['operation']=='material':result=material_aov(request['scene'],Path(request['output']),request['time'],request['aov'],request.get('purposes'))
    elif request['operation']=='population':
        from pxr import Usd
        from headless_stage import closed_population_mask
        result=closed_population_mask(Usd.Stage.Open(request['scene']),request['roots'])
    else:raise ValueError('unknown stage operation')
    Path(sys.argv[2]).write_text(json.dumps(result,indent=2)+'\n')
