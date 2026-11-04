"""Configurable, replayable multi-scene/time/AOV headless validation jobs."""
import copy
import html
import json
import math
from pathlib import Path
import random
from types import SimpleNamespace
from headless import HeadlessSession,number,toolchain_fingerprint

AOVS={'color','depth','worldNormal','baseColor','roughness','metallic'}
DEFAULT_GATES={'coverage_iou_min':.99,'depth_relative_p95_max':.01,'normal_p95_degrees_max':5.,'color_mae_max':.05,'color_ssim_min':.95}


def atomic_text(path,text):
    """Keep the last valid report if storage fills during the next update."""
    temporary=path.with_name(path.name+'.tmp')
    try:
        temporary.write_text(text)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def require_visible_surfaces(frame):
    """Matching background pixels cannot certify beauty or material fields."""
    aovs=frame.get('aovs',{})
    metrics=aovs.get('depth',{}).get('metrics',{})
    visible=(metrics.get('reference_coverage_pixels',0)>0 and
             metrics.get('candidate_coverage_pixels',0)>0 and
             metrics.get('common_pixels',0)>0)
    if visible:return
    for name in ('color','baseColor','roughness','metallic'):
        item=aovs.get(name,{})
        if item.get('verdict')=='PASS':
            item.update(verdict='INCOMPLETE',reason='No independent depth coverage witness in both views; matching background cannot validate visible surfaces.')
            if item.get('native_parameter',{}).get('verdict')=='PASS':
                item['native_parameter']['verdict']='INCOMPLETE'


def sample_times(config,start,end):
    if not isinstance(config,dict) or set(config)-{'mode','count','seed','start','end','values'}: raise ValueError('invalid time_sampling fields')
    mode=config.get('mode','uniform');start=config.get('start',start);end=config.get('end',end)
    number(start,'start');number(end,'end')
    if start>end: raise ValueError('sampling start exceeds end')
    if mode=='explicit':
        values=config.get('values')
        if not isinstance(values,list) or not 1<=len(values)<=128: raise ValueError('explicit sampling requires 1..128 values')
        for value in values:number(value,'time')
        return values[:]
    if mode not in ('uniform','random'): raise ValueError('time sampling mode must be uniform, random or explicit')
    count=config.get('count',3)
    if type(count) is not int or not 1<=count<=128: raise ValueError('sample count must be 1..128')
    if start==end:return [start]
    if mode=='uniform':return [start+(end-start)*i/(count-1) for i in range(count)] if count>1 else [(start+end)/2]
    seed=config.get('seed',0)
    if type(seed) is not int or not 0<=seed<2**64: raise ValueError('random seed must be an integer in 0..2^64-1')
    rng=random.Random(seed)
    return sorted(rng.uniform(start,end) for _ in range(count))


def beauty_lighting_rig(value):
    """Validate the opt-in shared beauty rig (see validation_stage.beauty_lighting)."""
    if value is None:return None
    if not isinstance(value,dict) or set(value)-{'key','fill','dome','key_rotate','fill_rotate','force'}:
        raise ValueError('invalid beauty_lighting fields')
    rig={}
    if type(value.get('force',False)) is not bool:raise ValueError('beauty_lighting force must be boolean')
    rig['force']=value.get('force',False)
    for name,default,low,high in (('key',1.0,0,100),('fill',0.25,0,100),('dome',0.0,0,100)):
        item=value.get(name,default);number(item,'beauty_lighting '+name,low,high);rig[name]=item
    for name,default in (('key_rotate',[-45.,0.,0.]),('fill_rotate',[30.,0.,160.])):
        item=value.get(name,default)
        if not isinstance(item,(list,tuple)) or len(item)!=3:raise ValueError('beauty_lighting '+name+' must have 3 values')
        for part in item:number(part,'beauty_lighting '+name)
        rig[name]=[float(part) for part in item]
    return rig


def load_config(path):
    if path.stat().st_size>1024*1024:raise ValueError('job config exceeds 1 MiB')
    config=json.loads(path.read_text())
    allowed={'version','assets_root','size','backend','time_sampling','aovs','thresholds','scenes','camera_policy','data_backend','native_curve_segments','native_load_limits','process_limits','reference_complexity','beauty_lighting','reference_camera_light','reference_allow_nonfatal'}
    if not isinstance(config,dict) or set(config)-allowed or config.get('version')!=1:raise ValueError('invalid version 1 job config')
    scenes=config.get('scenes')
    if not isinstance(scenes,list) or not 1<=len(scenes)<=128:raise ValueError('scenes must contain 1..128 jobs')
    for scene in scenes:
        if not isinstance(scene,dict) or set(scene)-{'path','time_sampling','aovs','camera_path','state','isolate','native_load_limits','process_limits','reference_population_mask','reference_complexity','beauty_lighting'} or not isinstance(scene.get('path'),str):raise ValueError('invalid scene job')
        beauty_lighting_rig(scene.get('beauty_lighting'))
        from headless import native_load_arguments,process_limits,reference_complexity
        reference_complexity(config.get('reference_complexity','low'))
        reference_complexity(scene.get('reference_complexity',config.get('reference_complexity','low')))
        native_load_arguments(config.get('native_load_limits',{}))
        native_load_arguments(scene.get('native_load_limits',config.get('native_load_limits',{})))
        process_limits(config.get('process_limits',{}))
        process_limits(scene.get('process_limits',config.get('process_limits',{})))
        aovs=scene.get('aovs',config.get('aovs',['color','depth','worldNormal']))
        if not isinstance(aovs,list) or not 1<=len(aovs)<=len(AOVS) or any(not isinstance(aov,str) for aov in aovs) or len(set(aovs))!=len(aovs):raise ValueError('aovs must be a nonempty unique list')
        if isinstance(scene.get('state'),dict) and scene['state'].get('edits') and set(aovs)&{'baseColor','roughness','metallic'}:
            raise ValueError('material AOVs with state edits require an exported source overlay')
        for aov in aovs:
            if aov not in AOVS:raise ValueError('unsupported AOV: '+str(aov))
        paths=scene.get('isolate',[])
        if not isinstance(paths,list) or len(paths)>128 or any(not isinstance(p,str) or not p.startswith('/') or p=='/' for p in paths):raise ValueError('invalid isolate paths')
        if type(scene.get('reference_population_mask',False)) is not bool or scene.get('reference_population_mask') and not paths:raise ValueError('reference_population_mask requires isolated roots and a boolean')
        sampling=scene.get('time_sampling',config.get('time_sampling',{}));sample_times(sampling,0,1)
    size=config.get('size',[640,480])
    if not isinstance(size,list) or len(size)!=2 or any(type(v) is not int or not 16<=v<=4096 for v in size):raise ValueError('invalid size')
    if config.get('backend','cpu') not in ('cpu','vkr','hip','cuda','vkr-instanced','vkr-streamed'):raise ValueError('invalid backend')
    if config.get('data_backend','cpu') not in ('cpu','vkr','vkr-instanced','vkr-streamed'):raise ValueError('invalid data_backend')
    segments=config.get('native_curve_segments',8)
    if type(segments) is not int or not 1<=segments<=64:raise ValueError('native_curve_segments must be an integer in 1..64')
    if config.get('camera_policy','fixed_first') not in ('fixed_first','per_frame'):raise ValueError('invalid camera_policy')
    gates=dict(DEFAULT_GATES);extra=config.get('thresholds',{})
    if not isinstance(extra,dict) or set(extra)-set(gates):raise ValueError('invalid thresholds')
    gates.update(extra)
    for gate_name,value in gates.items():
        number(value,gate_name,0,180 if gate_name=='normal_p95_degrees_max' else 1)
    config['thresholds']=gates
    beauty_lighting_rig(config.get('beauty_lighting'))
    if type(config.get('reference_camera_light',False)) is not bool:
        raise ValueError('reference_camera_light must be boolean')
    if config.get('reference_camera_light') and config.get('beauty_lighting'):
        raise ValueError('reference_camera_light and beauty_lighting are alternative beauty contracts')
    return config


def _run(args):
    from aov import compare_data
    number(args.timeout,'timeout',.01,86400)
    config=load_config(args.config);out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()):raise ValueError('validation output directory must be empty')
    (out/'job-config.json').write_text(json.dumps(config,indent=2)+'\n')
    base=args.assets_root or Path(config.get('assets_root','../usd-assets'))
    base=base.resolve(strict=True)
    for scene in config['scenes']:
        source=(base/scene['path']).resolve(strict=True)
        if not source.is_relative_to(base):raise ValueError('scene escapes assets_root')
    toolchain=toolchain_fingerprint(args)
    result={'version':1,'config':config,'assets_root':str(base),'scenes':[],
            'toolchain':toolchain,
            'aov_policy':'Beauty uses requested backend. Native data depth/normal AOVs use the explicitly configured data_backend (default CPU); SDK AOVs use EGL GPU. Material AOVs use shared emissive diagnostic overrides. Roughness/metallic additionally gate original-source native resolved fields against SDK diagnostics; BRDF integration remains outside the material-channel contract.'}
    def save():
        atomic_text(out/'validation.json',json.dumps(result,indent=2,allow_nan=False)+'\n')
        body=['<!doctype html><meta charset="utf-8"><title>USD validation</title><style>body{font:16px system-ui}img{max-width:32%}pre{white-space:pre-wrap}</style><h1>USD asset / time / AOV validation</h1><p><a href="validation.json">Full manifest</a> · <a href="job-config.json">Replay config</a></p>']
        for scene in result['scenes']:
            body.append('<h2>'+html.escape(scene['scene'])+'</h2>')
            for frame in scene.get('frames',[]):
                body.append('<h3>Time '+str(frame['time'])+': '+frame.get('verdict','ERROR')+'</h3>')
                if 'error' in frame:body.append('<pre>'+html.escape(frame['error'])+'</pre>')
                for name,item in frame.get('aovs',{}).items():
                    body.append('<p>'+html.escape(name+': '+str(item.get('verdict','ERROR')))+' '+html.escape(str(item.get('metrics',{})))+'</p>')
                    if item.get('error'):body.append('<pre>'+html.escape(item['error'])+'</pre>')
                    if item.get('report'):body.append('<a href="'+html.escape(str(Path(item['report']).relative_to(out)))+'">Interactive comparison</a>')
                    for image in item.get('images',{}).values():
                        rel=html.escape(str(Path(image).relative_to(out)));body.append(f'<a href="{rel}"><img src="{rel}"></a>')
                    if item.get('native_parameter'):
                        parameter=item['native_parameter']
                        body.append('<p>Original-source native parameter: '+html.escape(parameter['verdict'])+' '+html.escape(parameter['semantics'])+'</p>')
                        body.append('<pre>'+html.escape(json.dumps(parameter['metrics'],indent=2))+'</pre>')
                        for image in (parameter['reference'],parameter['candidate'],parameter['metrics']['heatmap']):
                            rel=html.escape(str(Path(image).relative_to(out)));body.append(f'<a href="{rel}"><img src="{rel}"></a>')
        atomic_text(out/'index.html','\n'.join(body))
    for si,scene in enumerate(config['scenes']):
        source=(base/scene['path']).resolve(strict=True)
        if not source.is_relative_to(base):raise ValueError('scene escapes assets_root')
        directory=out/f'scene-{si:03}';directory.mkdir()
        ns=SimpleNamespace(**vars(args));ns.scene=source;ns.out=directory/'probe-session';ns.state=None;ns.size='x'.join(map(str,config.get('size',[640,480])));ns.time=None;ns.camera_path=scene.get('camera_path');ns.backend=config.get('backend','cpu');ns.session_subdir=False
        ns.curve_segments=config.get('native_curve_segments',8)
        ns.native_load_limits=scene.get('native_load_limits',config.get('native_load_limits',{}))
        ns.process_limits=scene.get('process_limits',config.get('process_limits',{}))
        ns.reference_population_roots=scene.get('isolate') if scene.get('reference_population_mask') else None
        ns.reference_complexity=scene.get('reference_complexity',config.get('reference_complexity','low'))
        session=HeadlessSession(ns)
        if config.get('reference_allow_nonfatal'):
            session.reference_allow_nonfatal=True
        if config.get('reference_camera_light'):
            # usdrecord only evaluates its camera light; both tools then share
            # that one light model for the beauty AOV.
            session.state['reference_camera_light']=True
        def worker(request,name):
            (directory/(name+'-request.json')).write_text(json.dumps(request)+'\n')
            output=directory/(name+'.json')
            session._run([str(args.usd_python.absolute()),str(Path(__file__).with_name('validation_stage.py')),str(directory/(name+'-request.json')),str(output)],directory,name,session.environment)
            return json.loads(output.read_text())
        entry={'scene':scene['path'],'frames':[]};result['scenes'].append(entry)
        if config.get('reference_camera_light'):
            entry['reference_camera_light']=True
        try:
            if session.toolchain!=toolchain:
                raise RuntimeError('toolchain changed during validation job; start a new job')
            if scene.get('isolate'):
                isolated=worker({'operation':'isolate','scene':str(source),'output':str(directory/'isolated.usda'),'paths':scene['isolate']},'isolate')
                entry['isolation']=isolated;source=Path(isolated['wrapper']);session.state['scene']=str(source)
            rig=scene.get('beauty_lighting',config.get('beauty_lighting'))
            if rig is not None:
                rig=beauty_lighting_rig(rig)
                lighting=worker({'operation':'lighting','scene':str(source),'output':str(directory/'beauty-lighting.usda'),
                                 'key':rig['key'],'fill':rig['fill'],'dome':rig['dome'],
                                 'key_rotate':rig['key_rotate'],'fill_rotate':rig['fill_rotate'],
                                 'force':rig['force']},'beauty-lighting')
                entry['beauty_lighting']=lighting
                if lighting.get('applied',True):
                    source=Path(lighting['wrapper']);session.state['scene']=str(source)
            inventory=worker({'operation':'probe','scene':str(source)},'inventory');entry['inventory']=inventory
            times=sample_times(scene.get('time_sampling',config.get('time_sampling',{})),inventory['start'],inventory['end']);entry['sampled_times']=times
            if scene.get('state'):
                from headless import import_state
                session.state=import_state(scene['state'],source)
            frozen_camera=None
            for fi,time in enumerate(times):
                frame={'time':time,'aovs':{}};entry['frames'].append(frame)
                frame_dir=directory/f'frame-{fi:03}';frame_dir.mkdir()
                try:
                    session.state['time']=time
                    if frozen_camera is not None:session.state['camera']=copy.deepcopy(frozen_camera)
                    beauty=session.render();frame['beauty_manifest']=beauty['manifest'];frame['camera']=beauty['state']['camera'];frame['process_wall_seconds']=beauty['process_wall_seconds'];frame['hardware']={'usdrecord':beauty['usdrecord'],'lusdrender':beauty['lusdrender']}
                    frame['reference_beauty_findings']=beauty.get('reference_render_findings',[])
                    if config.get('camera_policy','fixed_first')=='fixed_first':frozen_camera=beauty['state']['camera']
                    aovs=list(scene.get('aovs',config.get('aovs',['color','depth','worldNormal'])))
                    if any(a in aovs for a in ('color','baseColor','roughness','metallic')) and 'depth' not in aovs:
                        aovs.append('depth');frame['implicit_aovs']=['depth']
                    for aov in aovs:
                        item={};frame['aovs'][aov]=item
                        try:
                            if aov=='color':
                                metrics=beauty['metrics'];item.update(metrics=metrics,report=beauty['report'],images=beauty['images'])
                                if frame['reference_beauty_findings']:
                                    item.update(verdict='INCOMPLETE',reason='Reference beauty has shader/GL failures despite a successful exit',reference_findings=frame['reference_beauty_findings']);save();continue
                            elif aov in ('depth','worldNormal'):
                                work=Path(beauty['manifest']).parent
                                ref=json.loads((work/'usdrecord.command.json').read_text());cand=json.loads((work/'lusdrender.command.json').read_text())
                                geometry=worker({'operation':'geometry','scene':beauty['wrapper'],'output':str(frame_dir/(aov+'-geometry.usda'))},f'geometry-{fi}-{aov}')
                                item['geometry_diagnostic']=geometry
                                if not geometry['supported']:
                                    item.update(verdict='UNSUPPORTED',error='Opaque geometry binding lacks complete coverage')
                                    save();continue
                                ref[1]=geometry['wrapper'];cand[1]=geometry['wrapper']
                                ref[2]=str(frame_dir/(aov+'-reference.####.###.pfm'));ref+=['--captureAov','depth' if aov=='depth' else 'Neye']
                                ref[ref.index('--diagnostics')+1]=str(frame_dir/(aov+'-diagnostics.json'))
                                if aov=='worldNormal':
                                    normal=worker({'operation':'normal','scene':geometry['wrapper'],'output':str(frame_dir/'normal-diagnostic.usda'),'time':time,'purposes':['default']+[name for name in ('render','proxy','guide') if beauty['state']['display'][name]]},f'normal-{fi}')
                                    item['normal_diagnostic']=normal
                                    if not normal['supported']:
                                        item.update(verdict='UNSUPPORTED',error='Normal orientation calibration lacks complete surface coverage')
                                        save();continue
                                    ref[1]=normal['wrapper']
                                cand[2]=str(frame_dir/(aov+'-candidate.pfm'))
                                cand=[v for v in cand if v not in ('-vkr','-hip','-cuda','-rtPreview','-vkInstanced','-vkInstancedStream')]
                                data_backend=config.get('data_backend','cpu')
                                from headless import backend_arguments
                                cand+=backend_arguments(data_backend)
                                if aov=='depth':cand=[argument for argument in cand if argument!='-smooth']
                                cand+=['--aov','shadingNormal' if aov=='worldNormal' else aov]
                                session._dependencies(beauty)
                                reference_environment=dict(session.environment)
                                # Storm defaults to 4x MSAA. Native raw AOVs use
                                # one center ray, so match sample coverage rather
                                # than comparing a resolved edge against a ray.
                                reference_environment['HDX_MSAA_SAMPLE_COUNT']='1'
                                item['reference_environment']={'HDX_MSAA_SAMPLE_COUNT':'1'}
                                if aov=='worldNormal':
                                    # OpenUSD defaults authored-normal skinning OFF
                                    # (usdSkelImaging/extComputations.cpp). Raw
                                    # normal validation requires posed normals.
                                    reference_environment['USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS']='1'
                                    item['reference_environment']['USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS']='1'
                                if ns.reference_population_roots:
                                    population=worker({'operation':'population','scene':ref[1],'roots':ns.reference_population_roots},f'population-{fi}-{aov}')
                                    if '--mask' in ref:
                                        index=ref.index('--mask');del ref[index:index+2]
                                    ref+=['--mask',','.join(population['paths'])]
                                    item['reference_population_mask']=population
                                session._run(ref,frame_dir,aov+'-reference',reference_environment)
                                from headless import reference_render_findings
                                findings=reference_render_findings(frame_dir/(aov+'-reference.log'),lighting_sensitive=False)
                                if findings:
                                    item.update(verdict='INCOMPLETE',reason='Reference AOV has shader/GL failures despite a successful exit',reference_findings=findings);save();continue
                                diag=json.loads((frame_dir/(aov+'-diagnostics.json')).read_text())
                                if len(diag['frames'])!=1 or diag['frames'][0]['time']!=time:raise ValueError('reference AOV time differs from requested USD time code')
                                reference=Path(diag['frames'][0]['path']).resolve(strict=True)
                                if reference.parent!=frame_dir:raise ValueError('AOV reference escaped frame directory')
                                session._run(cand,frame_dir,aov+'-candidate')
                                session._dependencies(beauty)
                                depth=frame['aovs'].get('depth',{}) if aov=='worldNormal' else {}
                                metrics=compare_data(reference,Path(cand[2]),frame['camera'],aov,frame_dir/aov,config['thresholds'],
                                    depth_reference=depth.get('reference'),depth_candidate=depth.get('candidate'))
                                metrics['semantics']+=' Single center sample in each engine (SDK MSAA disabled). '+geometry['semantics']
                                if aov=='worldNormal':metrics['semantics']=normal['semantics']+' SDK smooth display shading matched by native smooth mode; smooth subdivision normals generated rather than authored; polygon normals use authored interpolation when available, otherwise remain flat for none/bilinear schemes. Authored-normal skinning enabled; SDK MSAA disabled to match native center rays. '+geometry['semantics']
                                item.update(metrics=metrics,images=metrics['images'],reference=str(reference),candidate=cand[2],native_backend=data_backend,reference_backend='EGL Storm')
                            else:
                                if session.args.backend in ('vkr-instanced','vkr-streamed','cuda','hip'):
                                    item.update(verdict='UNSUPPORTED',reason='Native backend lacks calibrated unlit material-channel output');continue
                                prepared=worker({'operation':'material','scene':str(source),'output':str(frame_dir/(aov+'.usda')),'time':time,'aov':aov,'purposes':['default']+[name for name in ('render','proxy','guide') if beauty['state']['display'][name]]},f'material-{fi}-{aov}')
                                item['material_diagnostic']=prepared
                                if not prepared['supported']:
                                    item.update(verdict='UNSUPPORTED',reason='No complete UsdPreviewSurface diagnostic coverage');continue
                                original=session.state;previous_diagnostic=session.diagnostic_emission
                                try:
                                    session.diagnostic_emission=True
                                    session.state=copy.deepcopy(beauty['state']);session.state['scene']=prepared['wrapper']
                                    material=session.render()
                                finally:session.state=original;session.diagnostic_emission=previous_diagnostic
                                if material.get('reference_render_findings'):
                                    item.update(verdict='INCOMPLETE',reason='Reference material diagnostic has shader/GL failures',reference_findings=material['reference_render_findings']);save();continue
                                metrics=material['metrics'];item.update(metrics=metrics,report=material['report'],images=material['images'],manifest=material['manifest'])
                                if aov in ('roughness','metallic'):
                                    # The shared wrapper checks sampling/routing, but
                                    # cannot prove the native roughness/metallic field.
                                    # Render the original beauty wrapper through the
                                    # actual native material resolver as a second gate.
                                    from tools.ousddiff.images import compare
                                    native=json.loads((Path(beauty['manifest']).parent/'lusdrender.command.json').read_text())
                                    native[2]=str(frame_dir/(aov+'-native-parameter.png'))
                                    native+=['-diagnosticMaterial',aov]
                                    session._dependencies(beauty);session._dependencies(material)
                                    session._run(native,frame_dir,aov+'-native-parameter')
                                    session._dependencies(beauty);session._dependencies(material)
                                    parameter_metrics=compare(Path(material['images']['usdrecord']),Path(native[2]),frame_dir/(aov+'-native-parameter'))
                                    item['native_parameter']={'metrics':parameter_metrics,'reference':material['images']['usdrecord'],'candidate':native[2],
                                        'semantics':'SDK-side emissive channel adapter versus original-source native resolved '+aov+' field, including native scalar texture channel/scale/bias and UV routing; sRGB without tone mapping. No BRDF integration or lighting equivalence claim.'}
                            if aov in ('depth','worldNormal'):passed=metrics['passed']
                            else:passed=metrics['normalized_mae']<=config['thresholds']['color_mae_max'] and metrics['ssim']>=config['thresholds']['color_ssim_min']
                            if 'native_parameter' in item:
                                parameter=item['native_parameter']['metrics']
                                parameter_passed=parameter['normalized_mae']<=config['thresholds']['color_mae_max'] and parameter['ssim']>=config['thresholds']['color_ssim_min']
                                item['native_parameter']['verdict']='PASS' if parameter_passed else 'FAIL'
                                passed=passed and parameter_passed
                            item['verdict']='PASS' if passed else 'FAIL'
                            if aov=='worldNormal' and metrics.get('reference_invalid_normal_pixels',0):
                                item['reference_coverage_finding']='SDK Neye is zero on interior depth geometry; these pixels cannot validate native normals'
                                item['attribution']='unresolved'
                                if metrics.get('comparison_passed') and not metrics.get('candidate_invalid_normal_pixels',0):
                                    item['verdict']='INCOMPLETE'
                            if aov in ('depth','worldNormal') and metrics['reference_empty']:
                                item.update(verdict='INCOMPLETE',reason='Reference AOV has no foreground samples; inspect beauty and transparency/renderer support.')
                        except Exception as error:item.update(verdict='ERROR',error=str(error))
                    require_visible_surfaces(frame)
                    verdicts=[v.get('verdict') for v in frame['aovs'].values()]
                    frame['verdict']='PASS' if verdicts and all(v=='PASS' for v in verdicts) else 'ERROR' if 'ERROR' in verdicts else 'FAIL' if 'FAIL' in verdicts else 'INCOMPLETE'
                except Exception as error:frame.update(verdict='ERROR',error=str(error))
                save()
        except Exception as error:entry['error']=str(error);save()
        finally:session.close()
    result['passed']=all(s.get('frames') and all(f.get('verdict')=='PASS' for f in s['frames']) and 'error' not in s for s in result['scenes'])
    save();return {'passed':result['passed'],'manifest':str(out/'validation.json'),'report':str(out/'index.html')}


def run(args):
    import signal
    def interrupted(signum,frame):
        raise KeyboardInterrupt('validation interrupted')
    previous={sig:signal.signal(sig,interrupted) for sig in (signal.SIGINT,signal.SIGTERM)}
    try:return _run(args)
    finally:
        for sig,handler in previous.items():signal.signal(sig,handler)
