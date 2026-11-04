"""Agent-oriented, window-free paired lusdrender/usdrecord batch control."""
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time


BACKEND_FLAGS = {
    'cpu': '-rtPreview', 'vkr': '-vkr', 'hip': '-hip', 'cuda': '-cuda',
    'vkr-instanced': '-vkInstanced', 'vkr-streamed': '-vkInstancedStream',
}


def reference_complexity(value):
    if value not in ('low','medium','high','veryhigh'):
        raise ValueError('reference_complexity must be low, medium, high or veryhigh')
    return value


def native_load_arguments(limits):
    if not isinstance(limits,dict) or set(limits)-{'max_memory_gib','max_array_elements'}:
        raise ValueError('invalid native_load_limits')
    arguments=[]
    if 'max_memory_gib' in limits:
        value=limits['max_memory_gib']
        if type(value) not in (int,float) or not math.isfinite(value) or not 0<value<=1024:
            raise ValueError('max_memory_gib must be finite and within (0,1024]')
        arguments+=['-maxMem',str(value)]
    if 'max_array_elements' in limits:
        value=limits['max_array_elements']
        if type(value) is not int or not 1<=value<=2**32-1:
            raise ValueError('max_array_elements must be an integer in 1..2^32-1')
        arguments+=['-maxArrayElements',str(value)]
    return arguments


def process_limits(limits):
    if not isinstance(limits,dict) or set(limits)-{'max_rss_mib','reserve_mib','gpu_vram_path','gpu_budget_mib'}:
        raise ValueError('invalid process_limits')
    result=dict(max_rss_mib=30720,reserve_mib=4096)
    result.update(limits)
    for key in ('max_rss_mib','reserve_mib','gpu_budget_mib'):
        if key not in result:continue
        value=result[key]
        if type(value) is not int or not (0 if key=='reserve_mib' else 1)<=value<=1048576:
            raise ValueError('invalid '+key)
    if ('gpu_vram_path' in result)!=('gpu_budget_mib' in result):
        raise ValueError('GPU supervision requires a counter path and budget')
    if 'gpu_vram_path' in result:
        path=result['gpu_vram_path']
        if not isinstance(path,str) or not path or not Path(path).is_absolute():
            raise ValueError('gpu_vram_path must be an absolute counter path')
    return result


def backend_arguments(backend):
    if backend not in BACKEND_FLAGS:
        raise ValueError('unsupported native backend: '+str(backend))
    return [BACKEND_FLAGS[backend]]


_FILE_HASH_CACHE = {}


def file_fingerprint(path):
    path=Path(path).resolve()
    if not path.is_file():return {'path':str(path),'sha256':None}
    stat=path.stat()
    identity=(stat.st_dev,stat.st_ino,stat.st_size,stat.st_mtime_ns,stat.st_ctime_ns)
    cached=_FILE_HASH_CACHE.get(str(path))
    if cached is None or cached[0]!=identity:
        with path.open('rb') as source:digest=hashlib.file_digest(source,'sha256').hexdigest()
        after=path.stat()
        if identity!=(after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns):
            raise RuntimeError('toolchain file changed while hashing: '+str(path))
        _FILE_HASH_CACHE[str(path)]=(identity,digest)
    return {'path':str(path),'sha256':_FILE_HASH_CACHE[str(path)][1]}


def toolchain_fingerprint(args):
    result={name:file_fingerprint(getattr(args,name))
            for name in ('lusdrender','usdrecord','usd_python')}
    # Executable hashes alone do not pin shared USD libraries, Python modules,
    # schemas, plugin manifests or shaders. Pin the SDK install's runtime tree.
    prefix=Path(args.usdrecord).absolute().parent.parent
    library=prefix/'lib'
    paths=list(library.glob('libusd*'))
    roots=[library/'usd']+list(library.glob('python*/site-packages/pxr'))
    for root in roots:
        if root.is_dir():paths.extend(root.rglob('*'))
    runtime=[file_fingerprint(path) for path in sorted(set(paths))
             if path.is_file() and '__pycache__' not in path.parts and path.suffix!='.pyc']
    result['sdk_runtime']={'scope':'SDK libusd libraries, lib/usd resources and pxr Python packages, excluding bytecode; system/driver dependencies reported separately',
                           'files':runtime}
    return result


def number(value, name, minimum=-1e30, maximum=1e30):
    if type(value) not in (int,float) or not math.isfinite(value) or not minimum<=value<=maximum:
        raise ValueError(name+' must be a finite number in range')
    return value


def normalize(state):
    """Validate all representable state before running either renderer."""
    if not isinstance(state,dict): raise ValueError('state must be an object')
    result=copy.deepcopy(state)
    if not isinstance(result.get('scene'),str): raise ValueError('scene path is required')
    result['scene']=str(Path(result['scene']).resolve(strict=True))
    size=result.setdefault('size',[640,480])
    if not isinstance(size,list) or len(size)!=2 or any(type(v) is not int or not 16<=v<=4096 for v in size):
        raise ValueError('size must be [width,height] in 16..4096')
    result.setdefault('time',None)
    if result['time'] is not None: number(result['time'],'time')
    if result.get('camera_path') is not None and (not isinstance(result['camera_path'],str) or not result['camera_path'].startswith('/')):
        raise ValueError('camera_path must be an absolute camera prim path')
    result.setdefault('camera',None)
    camera=result['camera']
    if camera is not None:
        if not isinstance(camera,dict): raise ValueError('camera must be an object')
        transform=camera.get('transform');shift=camera.get('shift')
        if not isinstance(transform,list) or len(transform)!=16 or not isinstance(shift,list) or len(shift)!=2:
            raise ValueError('camera requires transform[16] and shift[2]')
        for v in transform+shift: number(v,'camera vector')
        basis=[transform[i:i+3] for i in (0,4,8)]
        dot=lambda a,b:sum(x*y for x,y in zip(a,b))
        cross=lambda a,b:[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
        if (any(abs(dot(v,v)-1)>1e-3 for v in basis) or
            any(abs(dot(basis[a],basis[b]))>1e-3 for a,b in ((0,1),(0,2),(1,2))) or
            dot(cross(basis[1],basis[2]),basis[0])<.999 or
            any(abs(transform[i])>1e-6 for i in (3,7,11)) or abs(transform[15]-1)>1e-6):
            raise ValueError('camera transform must be rigid and right handed')
        for key in ('fov_y','aspect','ortho_height','near','far','distance','exposure'): number(camera.get(key),'camera '+key)
        if camera.get('projection') not in ('perspective','orthographic'): raise ValueError('invalid projection')
        if not 5<=camera['fov_y']<=175 or not camera['ortho_height']>=1e-5 or not 1e-5<=camera['near']<camera['far'] or camera['distance']<.001 or any(abs(v)>10 for v in shift):
            raise ValueError('invalid camera range')
        if abs(camera['aspect']-size[0]/size[1])>1e-4: raise ValueError('camera aspect must match output dimensions')
        if camera['exposure']!=0: raise ValueError('headless comparison currently requires zero exposure')
    display=result.setdefault('display',{})
    if not isinstance(display,dict): raise ValueError('display must be an object')
    for name,default in [('render',True),('proxy',False),('guide',False),('skeleton',False),('bboxes',False),('hud',False)]:
        display.setdefault(name,default)
        if type(display[name]) is not bool: raise ValueError('display flags must be boolean')
    display.setdefault('mode','shaded');display.setdefault('background','black')
    if display['mode']!='shaded': raise ValueError('paired batch rendering currently supports shaded mode only')
    if display['background'] not in ('black','white'): raise ValueError('background must be black or white')
    if display['hud']: raise ValueError('HUDs are not part of headless render output')
    # Native capability annotations from interactive snapshots are descriptive.
    allowed={'render','proxy','guide','skeleton','bboxes','hud','mode','background','skeleton_control','skeleton_policy'}
    if set(display)-allowed: raise ValueError('unsupported display fields: '+', '.join(sorted(set(display)-allowed)))
    selection=result.setdefault('selection',[])
    if not isinstance(selection,list) or len(selection)>128 or any(not isinstance(v,str) or not v.startswith('/') or '.' in v for v in selection):
        raise ValueError('selection must contain at most 128 absolute prim paths')
    edits=result.setdefault('edits',[])
    if not isinstance(edits,list) or len(edits)>128 or any(not isinstance(v,dict) for v in edits): raise ValueError('edits must contain at most 128 operation objects')
    if len(json.dumps(result,allow_nan=False).encode())>1024*1024: raise ValueError('state exceeds 1 MiB')
    return result


def import_state(value, scene):
    """Accept a native view_state, paired capture, or standalone batch state."""
    if not isinstance(value,dict): raise ValueError('state JSON must be an object')
    if isinstance(value.get('state'),dict): value=value['state']
    if 'states' in value: value=value['states'].get('usdview',value['states'].get('usdrecord',{}))
    keys=('camera','camera_path','size','display','selection','edits','time')
    if value and not any(key in value for key in (*keys,'timeline')):
        raise ValueError('unrecognized state document; use view_state, capture.json, render.json or state.json')
    result={k:copy.deepcopy(value[k]) for k in keys if k in value}
    if 'timeline' in value: result['time']=value['timeline']['time']
    result['scene']=str(scene)
    return normalize(result)


def reference_render_findings(path, *, lighting_sensitive=True):
    """Known partial-reference failures can be SDK warnings with exit status zero."""
    findings=[]
    with Path(path).open(errors='replace') as stream:
        for line in stream:
            lower=line.lower()
            resource_failure = ('loading ' in lower and ' failed!' in lower)
            truncated_lights = (lighting_sensitive and 'hydra storm supports up to ' in lower and 'truncating ' in lower)
            if resource_failure or truncated_lights or any(token in lower for token in (
                    'failed to compile shader','failed to link shader','gl error:',
                    'recording aborted due to','[pluginload] unknown field data type')):
                findings.append(line.strip()[:512])
                if len(findings)==8:break
    return findings


class HeadlessSession:
    def __init__(self,args):
        self.args=args;self.out=args.out.resolve()
        self.curve_segments=getattr(args,'curve_segments',8)
        if type(self.curve_segments) is not int or not 1<=self.curve_segments<=64:
            raise ValueError('curve_segments must be an integer in 1..64')
        self.toolchain=toolchain_fingerprint(args)
        if getattr(args,'session_subdir',False): self.out=self.out/('session-'+str(time.time_ns()))
        self.out.mkdir(parents=True,exist_ok=True,mode=0o700)
        if any(self.out.iterdir()): raise ValueError('headless output directory must be empty')
        os.chmod(self.out,0o700)
        if args.state and args.state.stat().st_size>1024*1024: raise ValueError('state file exceeds 1 MiB')
        self.state=import_state(json.loads(args.state.read_text()) if args.state else {},args.scene)
        if getattr(args,'size',None):
            import re
            if not re.fullmatch(r'[0-9]+x[0-9]+',args.size): raise ValueError('size must be WIDTHxHEIGHT')
            self.state['size']=list(map(int,args.size.split('x')))
            if self.state['camera']: self.state['camera']['aspect']=self.state['size'][0]/max(1,self.state['size'][1])
        if getattr(args,'time',None) is not None: self.state['time']=args.time
        if getattr(args,'camera_path',None): self.state.update(camera=None,camera_path=args.camera_path)
        self.state=normalize(self.state)
        self.diagnostic_emission=False
        self.reference_complexity=reference_complexity(getattr(args,'reference_complexity','low'))
        self.native_load_arguments=native_load_arguments(getattr(args,'native_load_limits',{}))
        self.process_limits=process_limits(getattr(args,'process_limits',{}))
        self.revision=0;self.latest=None;self.closed=False;self.active=None
        self.history=[copy.deepcopy(self.state['edits'])];self.position=0
        self.environment=dict(os.environ)
        prefix=args.usdrecord.absolute().parent.parent
        packages=list((prefix/'lib').glob('python*/site-packages'))
        if len(packages)==1:
            self.environment['PYTHONPATH']=os.pathsep.join([str(packages[0])]+[p for p in self.environment.get('PYTHONPATH','').split(os.pathsep) if p])
        libraries=[prefix/'lib',prefix.parent/'deps/inst/lib']
        self.environment['LD_LIBRARY_PATH']=os.pathsep.join([str(p) for p in libraries if p.is_dir()]+[p for p in self.environment.get('LD_LIBRARY_PATH','').split(os.pathsep) if p])
        for setting in args.usd_env:
            key,separator,value=setting.partition('=')
            if not separator or not key: raise ValueError('--usd-env requires NAME=VALUE')
            self.environment[key]=value
        self.environment['PYTHONDONTWRITEBYTECODE']='1'
        self._save()

    def _save(self):
        temporary=self.out/'state.pending.json'
        temporary.write_text(json.dumps(self.status(),allow_nan=False,indent=2)+'\n')
        temporary.replace(self.out/'state.json')

    def status(self):
        return {'protocol_version':1,'kind':'headless-batch','directory':str(self.out),'revision':self.revision,'state':self.state,
                'toolchain':self.toolchain,
                'overlay':{'can_undo':self.position>0,'can_redo':self.position+1<len(self.history)},'latest':None if self.latest is None else {k:self.latest[k] for k in ('manifest','report','revision','state_sha256','images','metrics')},
                'capabilities':{'backends':list(BACKEND_FLAGS),'display':['purposes','background','bones','selection_bounds'],
                    'inspection':'OpenUSD composed stage; not an independent LightUSD property inspection',
                    'timing':'sequential fresh-process wall time, including startup/load/build/render/write',
                    'graphics':'usdrecord surfaceless EGL; no Qt, desktop, X11 or Xvfb'}}

    def _run(self,command,directory,name,environment=None):
        if self.closed: raise RuntimeError('headless session is closed')
        if toolchain_fingerprint(self.args)!=self.toolchain:
            raise RuntimeError('toolchain changed during paired session; start a new session')
        (directory/(name+'.command.json')).write_text(json.dumps(command,indent=2)+'\n')
        if sys.platform.startswith('linux'):
            from tools.ousddiff.supervisor import execute
            import shutil
            try:
                row=execute(command,directory/(name+'.process'),self.args.timeout,
                            env=environment,on_start=lambda child:setattr(self,'active',child),
                            **self.process_limits)
            finally:
                self.active=None
            (directory/(name+'.resources.json')).write_text(json.dumps(row,indent=2)+'\n')
            with (directory/(name+'.log')).open('wb') as log:
                for key in ('stdout','stderr'):
                    path=Path(row[key])
                    if path.is_file():
                        with path.open('rb') as source:shutil.copyfileobj(source,log)
            if row['status']=='timeout':
                raise subprocess.TimeoutExpired(command,self.args.timeout)
            if row['status']!='ok':
                raise RuntimeError(name+' '+row['status']+': '+row.get('reason',str(row.get('returncode')))+'; inspect '+str(directory/(name+'.resources.json')))
            if toolchain_fingerprint(self.args)!=self.toolchain:
                raise RuntimeError('toolchain changed during capture; result is incomplete')
            return row['wall_s']
        if getattr(self.args,'process_limits',{}):
            raise RuntimeError('explicit process supervision requires Linux')
        started=time.perf_counter()
        with (directory/(name+'.log')).open('wb') as log:
            child=subprocess.Popen(command,env=environment,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            self.active=child
            try:
                code=child.wait(timeout=self.args.timeout)
                if code: raise RuntimeError(name+' exited '+str(code)+'; inspect '+str(directory/(name+'.log')))
                if toolchain_fingerprint(self.args)!=self.toolchain:
                    raise RuntimeError('toolchain changed during capture; result is incomplete')
            finally:
                if child.poll() is None:
                    self._terminate(child)
                self.active=None
        return time.perf_counter()-started

    @staticmethod
    def _terminate(child):
        try:
            if os.name=='posix': os.killpg(child.pid,signal.SIGTERM)
            else: child.terminate()
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name=='posix': os.killpg(child.pid,signal.SIGKILL)
            else: child.kill()
            child.wait()
        except ProcessLookupError: pass

    def close(self):
        self.closed=True
        if self.active is not None: self._terminate(self.active)

    def _prepare(self,state,inspection=None):
        directory=self.out/('job-%d-%d'%(self.revision,time.time_ns()));directory.mkdir(mode=0o700)
        request={'state':state,'directory':str(directory)}
        if getattr(self.args,'reference_population_roots',None):
            request['reference_population_roots']=self.args.reference_population_roots
        if inspection is not None: request['inspect']=inspection
        (directory/'request.json').write_text(json.dumps(request,allow_nan=False)+'\n')
        self._run([str(self.args.usd_python.absolute()),str(Path(__file__).with_name('headless_stage.py')),
                   str(directory/'request.json'),str(directory/'prepared.json')],directory,'prepare',self.environment)
        prepared=json.loads((directory/'prepared.json').read_text())
        for dependency in prepared['dependencies']:
            dependency['sha256']=file_fingerprint(dependency['path'])['sha256']
        prepared['dependency_scope']='Used USD layers/packages; external texture payloads are not included in this layer fingerprint'
        (directory/'prepared.json').write_text(json.dumps(prepared,indent=2)+'\n')
        return directory,prepared

    def configure(self,changes):
        if not isinstance(changes,dict) or set(changes)-{'camera','camera_path','size','display','selection','time'}:
            raise ValueError('configure accepts camera, camera_path, size, display, selection and time')
        candidate=copy.deepcopy(self.state)
        for name,value in changes.items():
            if name=='display':
                if not isinstance(value,dict): raise ValueError('display must be an object')
                candidate['display'].update(value)
            else: candidate[name]=value
        candidate=normalize(candidate)
        # Validate selected paths/camera and diagnostics before accepting state.
        self._prepare(candidate)
        self.state=candidate;self.revision+=1;self._save()
        return self.status()

    def overlay(self,action='status',edits=None):
        if action=='status': return self.status()['overlay']
        candidate=copy.deepcopy(self.state);position=self.position
        if action=='edit':
            if not isinstance(edits,list) or not edits: raise ValueError('edit requires nonempty operation list')
            candidate['edits']+=edits
        elif action=='clear': candidate['edits']=[]
        elif action in ('undo','redo'):
            position+=-1 if action=='undo' else 1
            if not 0<=position<len(self.history): raise ValueError('no '+action+' available')
            candidate['edits']=copy.deepcopy(self.history[position])
        else: raise ValueError('unsupported overlay action')
        candidate=normalize(candidate);self._prepare(candidate)
        if action in ('edit','clear'):
            self.history=self.history[:self.position+1]+[copy.deepcopy(candidate['edits'])];position=len(self.history)-1
        self.position=position;self.state=candidate;self.revision+=1;self._save()
        return self.status()['overlay']

    def inspect(self,arguments):
        path=arguments.get('path');offset=arguments.get('offset',0);limit=arguments.get('limit',64)
        if not isinstance(path,str) or not path.startswith('/') or type(offset) is not int or offset<0 or type(limit) is not int or not 1<=limit<=256:
            raise ValueError('inspection requires absolute path, nonnegative offset and limit 1..256')
        _,prepared=self._prepare(self.state,{'path':path,'offset':offset,'limit':limit})
        return prepared['inspection']

    @staticmethod
    def _dependencies(prepared):
        for item in prepared['dependencies']:
            stat=Path(item['path']).stat()
            if (stat.st_size!=item['size'] or stat.st_mtime_ns!=item['mtime_ns'] or
                    ('sha256' in item and file_fingerprint(item['path'])['sha256']!=item['sha256'])):
                raise RuntimeError('scene dependency changed during paired job: '+item['path'])

    def render(self):
        from tools.ousddiff.images import compare,validate
        from report import write_report
        directory,prepared=self._prepare(self.state)
        frozen=copy.deepcopy(self.state);frozen['camera']=prepared['camera'];frozen['time']=prepared['time']
        canonical=json.dumps(frozen,sort_keys=True,allow_nan=False)
        digest=hashlib.sha256(canonical.encode()).hexdigest()
        (directory/'render-state.json').write_text(canonical+'\n')
        width,height=frozen['size'];display=frozen['display']
        purposes=','.join(name for name in ('render','proxy','guide') if display[name])
        reference=[str(self.args.usdrecord.absolute()),prepared['wrapper'],str(directory/'usdrecord.####.###.png'),
            '--egl','--abortOnError','--ignoreRenderSettings','--skeletonDisplay','0','--camera',prepared['camera_path'],
            '--frames='+str(prepared['time']),'--imageWidth',str(width),'--purposes',purposes,
            '--colorCorrectionMode','sRGB','--complexity',self.reference_complexity,'--background',('0,0,0' if display['background']=='black' else '1,1,1'),
            '--diagnostics',str(directory/'usdrecord-diagnostics.json')]
        # usdrecord aborts on any TF error by default. Some field assets (for
        # example Island's ocean .vdb) emit a non-fatal plugin error yet still
        # write a usable image, so allow an explicit opt-out for that capture.
        if (getattr(self, 'reference_allow_nonfatal', False) or
                self.state.get('reference_allow_nonfatal')):
            reference.remove('--abortOnError')
        candidate=[str(self.args.lusdrender.absolute()),prepared['wrapper'],str(directory/'lusdrender.png'),
            '-camera',prepared['camera_path'],'-w',str(width),'-height',str(height),'-timecode',str(prepared['time']),
            '-purpose','default'+(','+purposes if purposes else ''),'-bg',('0,0,0' if display['background']=='black' else '1,1,1'),
            '-largeSceneProfile','off','-smooth','-stats']
        # Opt-in default-contract capture: usdrecord uses only its camera light,
        # so make the native candidate evaluate that same light.
        if self.state.get('reference_camera_light'):
            candidate.append('-referenceCameraLight')
        if prepared.get('reference_population_mask'):
            reference+=['--mask',','.join(prepared['reference_population_mask']['paths'])]
        if self.diagnostic_emission:candidate.append('-diagnosticEmission')
        candidate+=self.native_load_arguments
        candidate+=backend_arguments(self.args.backend)
        if self.args.backend in ('vkr-instanced','vkr-streamed'):candidate += ['-curveSegments',str(self.curve_segments)]
        timings={};process_errors={}
        try:
            reference_environment=dict(self.environment)
            if self.diagnostic_emission:reference_environment['HDX_MSAA_SAMPLE_COUNT']='1'
            for name,command,environment in [('usdrecord',reference,reference_environment),
                                              ('lusdrender',candidate,dict(os.environ))]:
                self._dependencies(prepared)
                try:timings[name]=self._run(command,directory,name,environment)
                except (RuntimeError,subprocess.TimeoutExpired) as error:process_errors[name]=str(error)
                self._dependencies(prepared)
            if process_errors:raise RuntimeError('; '.join(name+': '+error for name,error in process_errors.items()))
            diagnostics=json.loads((directory/'usdrecord-diagnostics.json').read_text())
            if len(diagnostics['frames'])!=1: raise RuntimeError('usdrecord did not emit exactly one frame')
            if diagnostics['frames'][0]['time']!=prepared['time']: raise RuntimeError('usdrecord time differs from paired job time')
            recorded=Path(diagnostics['frames'][0]['path']).resolve(strict=True)
            if recorded.parent!=directory.resolve(): raise RuntimeError('usdrecord image escaped the job directory')
            validate(recorded,frozen['size'])
            import shutil
            shutil.copyfile(recorded,directory/'usdrecord.png')
            for name in ('usdrecord','lusdrender'): validate(directory/(name+'.png'),frozen['size'])
            metrics=compare(directory/'usdrecord.png',directory/'lusdrender.png',directory)
            findings=reference_render_findings(directory/'usdrecord.log',lighting_sensitive=not self.diagnostic_emission)
            metrics['reference_complete']=not findings
            report=write_report(directory,directory/'usdrecord.png',directory/'lusdrender.png',metrics,
                                reference_label='usdrecord / reference',candidate_label='lusdrender / candidate')
            with (directory/'lusdrender.log').open('rb') as log:
                log.seek(0,2);length=log.tell();log.seek(max(0,length-65536))
                tail=log.read().decode('utf-8','replace')
            native_summary=[line for line in tail.splitlines() if line.startswith(('backend:','Vulkan device:','Vulkan caps:','HIP device:','CUDA device:'))][-16:]
            result={'protocol_version':1,'revision':self.revision,'state_sha256':digest,'state':frozen,
                    'reference_render_findings':findings,
                    'toolchain':self.toolchain,
                    'lusdrender':{'requested_backend':self.args.backend,'native_summary':native_summary,'log':str(directory/'lusdrender.log')},
                    'wrapper':prepared['wrapper'],'camera_path':prepared['camera_path'],'dependencies':prepared['dependencies'],
                    'dependency_scope':prepared.get('dependency_scope'),'native_emission_diagnostic':self.diagnostic_emission,
                    'reference_complexity':self.reference_complexity,
                    'reference_population_mask':prepared.get('reference_population_mask'),
                    'diagnostic_segments':prepared['diagnostic_segments'],'timing_policy':self.status()['capabilities']['timing'],
                    'process_wall_seconds':timings,'usdrecord':json.loads((directory/'usdrecord-diagnostics.json').read_text()),
                    'images':{name:str(directory/(name+'.png')) for name in ('usdrecord','lusdrender','diff','heatmap')},
                    'metrics':metrics,'report':report,'manifest':str(directory/'render.json')}
            (directory/'render.json').write_text(json.dumps(result,allow_nan=False,indent=2)+'\n')
        except BaseException as error:
            (directory/'failure.json').write_text(json.dumps({'error':str(error),'state_sha256':digest,'process_wall_seconds':timings,'process_errors':process_errors,'commands':{'usdrecord':reference,'lusdrender':candidate}})+'\n')
            raise
        self.latest=result;self._save()
        reply=dict(result);reply['dependency_count']=len(result['dependencies'])
        reply['dependencies']=result['dependencies'][:128]
        reply['dependencies_truncated']=len(result['dependencies'])>128
        return reply

    def image(self,name='diff'):
        if not self.latest or name not in self.latest['images']: raise ValueError('render first; choose usdrecord, lusdrender, diff or heatmap')
        from PIL import Image
        import io
        with Image.open(self.latest['images'][name]) as image:
            original=list(image.size);image.thumbnail((1024,1024))
            image=image.convert('RGB');output=io.BytesIO();image.save(output,format='PNG')
            return {'content':[{'type':'text','text':json.dumps({'original_size':original,'preview_size':list(image.size),'path':self.latest['images'][name],
                    'revision':self.latest['revision'],'current_revision':self.revision,'state_sha256':self.latest['state_sha256']})},
                               {'type':'image','mimeType':'image/png','data':base64.b64encode(output.getvalue()).decode()}]}


def dispatch(session,request):
    response={'jsonrpc':'2.0','id':request.get('id') if isinstance(request,dict) else None}
    try:
        if not isinstance(request,dict) or request.get('jsonrpc')!='2.0': raise ValueError('invalid JSON-RPC request')
        method=request.get('method')
        if method=='initialize': result={'protocolVersion':'2025-03-26','serverInfo':{'name':'view-control-headless','version':'0.1'},'capabilities':{'tools':{}}}
        elif method=='ping': result={}
        elif method=='tools/list':
            from server import EDIT_SCHEMA
            cameraProperties={k:{'type':'number'} for k in ('fov_y','aspect','ortho_height','near','far','distance','exposure')}
            cameraProperties.update(transform={'type':'array','items':{'type':'number'},'minItems':16,'maxItems':16},
                shift={'type':'array','items':{'type':'number'},'minItems':2,'maxItems':2},
                projection={'type':'string','enum':['perspective','orthographic']})
            stateSchema={'type':'object','additionalProperties':False,'properties':{
                'camera':{'type':['object','null'],'description':'Complete resolved camera; null selects automatic framing.',
                          'properties':cameraProperties,'required':list(cameraProperties),'additionalProperties':False},
                'camera_path':{'type':['string','null'],'description':'Authored absolute camera path, used when camera is null.'},
                'size':{'type':'array','items':{'type':'integer','minimum':16,'maximum':4096},'minItems':2,'maxItems':2},
                'time':{'type':['number','null'],'description':'USD time code; null selects stage start.'},
                'selection':{'type':'array','items':{'type':'string'},'maxItems':128},
                'display':{'type':'object','additionalProperties':False,'properties':{
                    **{key:{'type':'boolean'} for key in ('render','proxy','guide','skeleton','bboxes')},
                    'background':{'type':'string','enum':['black','white']},'mode':{'type':'string','enum':['shaded']}}}}}
            definitions=[('status',{},[]),('configure',{'state':stateSchema},['state']),('render',{},[]),
                ('inspect_prim',{'path':{'type':'string'},'offset':{'type':'integer','minimum':0},'limit':{'type':'integer','minimum':1,'maximum':256}},['path']),
                ('overlay',{'action':{'type':'string','enum':['status','edit','undo','redo','clear']},'edits':{'type':'array','maxItems':128,'items':EDIT_SCHEMA}},[]),
                ('image',{'name':{'type':'string','enum':['usdrecord','lusdrender','diff','heatmap']}},[]),('stop',{},[])]
            descriptions={'status':'Read immutable batch configuration and capabilities','configure':'Validate and update shared camera/size/display/selection/time',
                'render':'Run lusdrender and usdrecord sequentially on one frozen scene and emit images, diff, metrics and timings',
                'inspect_prim':'Inspect the composed diagnostic stage with OpenUSD; bounded properties, targets, connections and provenance',
                'overlay':'Apply/undo/redo sparse debug edits; source USD files remain unchanged','image':'Return an image preview for agent visual inspection',
                'stop':'Close the headless control process'}
            result={'tools':[{'name':name,'description':descriptions[name],'inputSchema':{'type':'object','properties':properties,'required':required,'additionalProperties':False}} for name,properties,required in definitions]}
        elif method=='tools/call':
            params=request['params'];name=params['name'];arguments=params.get('arguments',{})
            if not isinstance(arguments,dict): raise ValueError('arguments must be an object')
            allowed={'status':set(),'configure':{'state'},'render':set(),'inspect_prim':{'path','offset','limit'},
                     'overlay':{'action','edits'},'image':{'name'},'stop':set()}
            if name in allowed and set(arguments)-allowed[name]: raise ValueError('unsupported arguments for '+name)
            if name=='status': value=session.status()
            elif name=='configure': value=session.configure(arguments['state'])
            elif name=='render': value=session.render()
            elif name=='inspect_prim': value=session.inspect(arguments)
            elif name=='overlay': value=session.overlay(arguments.get('action','status'),arguments.get('edits'))
            elif name=='image': response['result']=session.image(arguments.get('name','diff'));return response
            elif name=='stop': session.close();value={'stopping':True}
            else: raise ValueError('unknown headless tool')
            result={'content':[{'type':'text','text':json.dumps(value,allow_nan=False)}]}
        else: raise ValueError('unsupported method')
        response['result']=result
    except Exception as error: response['error']={'code':-32000,'message':str(error)}
    return response


def run(args):
    number(args.timeout,'timeout',.01,86400)
    sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
    session=HeadlessSession(args)
    def stop(signum,frame): raise KeyboardInterrupt
    previous={s:signal.signal(s,stop) for s in (signal.SIGINT,signal.SIGTERM)}
    try:
        if args.mcp_stdio:
            while not session.closed:
                line=sys.stdin.readline(1024*1024+1)
                if not line: break
                try:
                    if len(line)>1024*1024 or len(line.encode('utf-8'))>1024*1024:
                        while line and not line.endswith('\n'): line=sys.stdin.readline(1024*1024+1)
                        raise ValueError('MCP request exceeds 1 MiB')
                    request=json.loads(line)
                    if isinstance(request,dict) and request.get('method','').startswith('notifications/'): continue
                    print(json.dumps(dispatch(session,request),allow_nan=False),flush=True)
                except (ValueError,RecursionError) as error:
                    print(json.dumps({'jsonrpc':'2.0','id':None,'error':{'code':-32700,'message':str(error)}}),flush=True)
        else: print(json.dumps(session.render(),allow_nan=False,indent=2),flush=True)
    finally:
        session.close()
        for sig,handler in previous.items(): signal.signal(sig,handler)
