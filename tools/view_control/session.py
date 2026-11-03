"""Persistent two-viewer lifecycle and semantic state synchronization."""
import json
import math
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import threading
import time

from control import ViewerClient


def equivalent(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(a, b, rel_tol=2e-6, abs_tol=2e-5)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(equivalent(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(equivalent(x,y) for x,y in zip(a,b))
    return a == b


def camera_key(camera):
    keys = ('transform','projection','aspect','shift','near','far','exposure')
    result = {k: camera[k] for k in keys}
    key = 'fov_y' if camera['projection'] == 'perspective' else 'ortho_height'
    result[key] = camera[key]
    return result


def domain(state, name):
    if name == 'camera': return camera_key(state['camera'])
    if name == 'selection':
        paths=state['selection']
        return {'paths':sorted(paths),'focus':state.get('focus',paths[0] if paths else '')}
    if name == 'timeline':
        return {k:state['timeline'][k] for k in ('time','playing')} if state['scene'].get('has_animation') else {'time':0,'playing':False}
    result={key: state['display'][key] for key in ('guide','proxy','render','mode','background')}
    result['skeleton']=state['display'].get('skeleton',False)
    return result


def time_equivalent(a, b):
    return type(a) in (int,float) and type(b) in (int,float) and math.isclose(a,b,rel_tol=0.0,abs_tol=1e-7)


def states_equal(a, b, field):
    left,right=domain(a,field),domain(b,field)
    if field=='timeline':
        return left['playing']==right['playing'] and time_equivalent(left['time'],right['time'])
    return equivalent(left,right)


def frame_matches(rendered, expected):
    if any(not states_equal(rendered,expected,d) for d in ('camera','display','selection','timeline')): return False
    if not equivalent(rendered['display'],expected['display']) or rendered['size']!=expected['size']: return False
    if rendered['scene'].get('loading') or not rendered['scene'].get('loaded'): return False
    return equivalent(rendered['scene'],expected['scene'])


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Session:
    def __init__(self, args):
        self.args = args
        self.out = args.out.resolve()
        self.out.mkdir(parents=True, exist_ok=True, mode=0o700)
        if any(self.out.iterdir()):
            raise ValueError('session directory must be empty; use a fresh directory')
        os.chmod(self.out, 0o700)
        self.token = secrets.token_urlsafe(32)
        self.token_file = self.out/'token'
        self.token_file.write_text(self.token)
        os.chmod(self.token_file, 0o600)
        self.children = []
        self.logs = []
        self.clients = {}
        self.last = {}
        self.revision = 0
        self.error = None
        self.lock = threading.RLock()
        self.stopped = threading.Event()
        self.sync_enabled = True
        self.overlay = None
        self.playback = {'active':False,'fps':30,'loop':True}
        self.control_requests=self.out/'control-requests'
        self.control_requests.mkdir(mode=0o700)
        self.control_url=None
        self.latest_capture=None
        self.control_operation=None
        self.control_error=None
        self._control_written=0.0
        self._control_text=None

    def spawn(self, name, command, env):
        log = (self.out/(name+'.log')).open('w')
        self.logs.append(log)
        child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append(child)
        (self.out/(name+'.command.json')).write_text(json.dumps(command, indent=2))
        return child

    def start(self):
        args = self.args
        source = args.scene.resolve(strict=True)
        env = dict(os.environ)
        env['LUSDVIEW_MCP_TOKEN_FILE'] = str(self.token_file)
        env['LUSDVIEW_CONTROL_DIR'] = str(self.out)
        port = free_port()
        (self.out/'lusdview.config.json').write_text('{}\n')
        lusd = [str(args.lusdview.absolute()), '--config', str(self.out/'lusdview.config.json'), '--mcp-http='+str(port), '--no-threaded',
                '--backend', args.backend, '--size', args.size, '--no-grid', '--no-skeleton', '--no-adaptive-quality', '--large-scene-profile', 'off', '--preview-cache', 'off', '--full-fidelity', '--load-payloads', str(source)]
        if args.headless_lusdview: lusd.insert(1,'--headless')
        self.spawn('lusdview', lusd, env)
        usd = [str(args.usdview.absolute())] if not args.usdview_python else [str(args.usdview_python.absolute()), str(Path(__file__).with_name('bootstrap_usdview.py')), '--bridge-source', str(args.bridge_source.resolve())]
        usd += ['--defaultsettings', '--remote-control', '--remote-token-file', str(self.token_file), '--remote-info-file', str(self.out/'usdview.endpoint.json'), str(source)]
        usd_env = dict(os.environ)
        usd_env.setdefault('PYTHONDONTWRITEBYTECODE','1')
        if os.name == 'posix' and usd_env.get('DISPLAY'):
            usd_env.setdefault('QT_QPA_PLATFORM', 'xcb')
            usd_env.setdefault('PYOPENGL_PLATFORM', 'glx')
        for item in args.usdview_env:
            key, separator, value = item.partition('=')
            if not separator or not key: raise ValueError('--usdview-env requires NAME=VALUE')
            usd_env[key] = value
        self.spawn('usdview', usd, usd_env)
        self.clients['lusdview'] = ViewerClient('http://127.0.0.1:%d/mcp'%port, self.token, 5)
        deadline = time.monotonic() + args.load_timeout
        ready = set()
        while time.monotonic() < deadline and not self.stopped.is_set():
            for child in self.children:
                if child.poll() is not None: raise RuntimeError('viewer exited during startup; inspect session logs')
            endpoint = self.out/'usdview.endpoint.json'
            if endpoint.exists() and 'usdview' not in self.clients:
                self.clients['usdview'] = ViewerClient(json.loads(endpoint.read_text())['endpoint'], self.token, 5)
            for name, client in self.clients.items():
                if name in ready: continue
                try:
                    state = client.call('view_state')
                    if state['scene'].get('loaded') and not state['scene'].get('loading') and state.get('rendered_camera'):
                        self.last[name] = state
                        ready.add(name)
                except (OSError, ValueError, RuntimeError): pass
            if len(ready) == 2: break
            self.stopped.wait(.1)
        if len(ready) != 2: raise RuntimeError('viewer startup timeout; inspect session logs')
        for name in self.clients: self.clients[name].timeout = 30
        # One authoritative initial fit; never independently fit both cameras.
        width,height=map(int,args.size.split('x'))
        for client in self.clients.values():
            client.call('viewport',{'op':'resize','width':width,'height':height})
        profile={'mode':'shaded','guide':False,'proxy':False,'render':True,'background':'black','hud':False,'bboxes':False}
        if self.last['usdview']['display'].get('skeleton_control'): profile['skeleton']=False
        self.clients['usdview'].call('render_settings',profile)
        initial = self.clients['usdview'].call('view_state')
        for name in ('camera','display','selection','timeline'):
            self.apply('lusdview',name,initial)
        self.last = {name:client.call('view_state') for name,client in self.clients.items()}
        self.thread = threading.Thread(target=self.poll, daemon=True)
        self.thread.start()

    def apply(self, target, name, state):
        client = self.clients[target]
        if name == 'camera':
            client.call('viewport', {'op':'set_camera', 'camera':state['camera']})
        elif name == 'display':
            settings=domain(state,'display')
            if target=='usdview' and not self.last.get(target,{}).get('display',{}).get('skeleton_control'):
                if settings.pop('skeleton'): raise ValueError('usdview cannot independently control skeleton display; rebuild the fork')
            client.call('render_settings', settings)
        elif name == 'timeline':
            if state['timeline']['playing']:
                raise ValueError('linked timeline requires paused playback; pause both viewers and resynchronize')
            if not states_equal(client.call('view_state'),state,'timeline'):
                client.call('timeline',{'op':'pause'})
                client.call('timeline',{'op':'seek','time':state['timeline']['time']})
        else:
            paths = state['selection']
            focus=state.get('focus',paths[0] if paths else '')
            if focus:
                if focus not in paths: raise ValueError('focused prim is outside selection')
                rest=[p for p in paths if p!=focus]
                paths=[focus]+rest if target=='usdview' else rest+[focus]
            client.call('select', {'op':'clear'})
            for path in paths: client.call('select', {'op':'add','path':path})
        # Read back native effective state to absorb harmless float/conform changes.
        self.last[target] = client.call('view_state')
        if not states_equal(self.last[target],state,name):
            raise ValueError(target+' could not represent synchronized '+name+'; inspect native state before retrying')
        self.revision += 1

    def poll(self):
        while not self.stopped.wait(1/30):
            with self.lock:
                try:
                    self._process_control_requests()
                    self.publish_control_state()
                except (OSError,ValueError) as error:
                    self.control_error=str(error)
                if not self.sync_enabled: continue
                try:
                    observed = {name: client.call('view_state') for name,client in self.clients.items()}
                    changes = {}
                    for source in ('lusdview','usdview'):
                        current = observed[source]
                        if current['scene'].get('loading'): continue
                        for name in ('camera','display','selection','timeline'):
                            if not states_equal(current,self.last[source],name):
                                # Deterministic last-observed source wins simultaneous changes.
                                changes[name] = (source,current)
                    self.last = observed
                    for name,(source,state) in changes.items():
                        self.apply('usdview' if source=='lusdview' else 'lusdview',name,state)
                    self._advance_playback()
                    self.error = None
                except (OSError, ValueError, RuntimeError, KeyError) as error:
                    self.error = str(error)
                    self.sync_enabled = False
                    self.playback['active']=False

    def status(self):
        with self.lock:
            return {'protocol_version':1,'revision':self.revision,'synchronizing':self.sync_enabled,
                    'error':self.error,'control_error':getattr(self,'control_error',None),'viewers':self.last,'overlay':self.overlay,'playback':{k:v for k,v in self.playback.items() if k not in ('pending_frames','due')}}

    def control_playback(self, action='status', fps=30, loop=True):
        with self.lock:
            if action=='status': return self.status()['playback']
            if action=='pause':
                self.playback['active']=False
                return self.status()['playback']
            if action!='play': raise ValueError('playback action must be status, play or pause')
            if type(fps) not in (float,int) or not math.isfinite(fps) or not 1<=fps<=60 or type(loop) is not bool:
                raise ValueError('playback requires fps in [1,60] and a boolean loop flag')
            if self.error or not self.sync_enabled: raise ValueError('resynchronize viewers before linked playback')
            states={n:c.call('view_state') for n,c in self.clients.items()}
            if any(not v['scene'].get('has_animation') or v['scene'].get('loading') for v in states.values()):
                raise ValueError('both viewers require a loaded animated stage')
            a,b=states['lusdview'],states['usdview']
            if a['scene']['filepath']!=b['scene']['filepath'] or any(not time_equivalent(a['timeline'][k],b['timeline'][k]) for k in ('start','end','fps')):
                raise ValueError('linked playback requires matching scene time ranges and time-code rates')
            if not a['timeline']['start']<a['timeline']['end'] or not a['timeline']['fps']>0:
                raise ValueError('invalid animation time range or time-code rate')
            for name,client in self.clients.items():
                if states[name]['timeline']['playing']: client.call('timeline',{'op':'pause'})
            source=self.clients['usdview'].call('view_state')
            source['timeline']['playing']=False
            for name in self.clients: self.apply(name,'timeline',source)
            self.last={n:c.call('view_state') for n,c in self.clients.items()}
            self.playback={'active':True,'fps':fps,'loop':loop,
                           'time':source['timeline']['time'],'start':source['timeline']['start'],
                           'end':source['timeline']['end'],'step':source['timeline']['fps']/fps,
                           'pending_frames':{n:v['frame_serial'] for n,v in self.last.items()},
                           'due':time.monotonic()}
            for client in self.clients.values(): client.call('viewport',{'op':'refresh'})
            return self.status()['playback']

    def _advance_playback(self):
        playback=self.playback
        if not playback['active'] or time.monotonic()<playback['due']: return
        # A single shared time code advances only after both native renderers acknowledge it.
        for name,state in self.last.items():
            if state['scene'].get('loading') or state['frame_serial']<=playback['pending_frames'][name]: return
            if not time_equivalent(state.get('rendered_time'),state['timeline']['time']): return
        current=self.last['usdview']['timeline']['time']
        target=current+playback['step']
        if current>=playback['end']:
            if playback['loop']: target=playback['start']
            else:
                playback['active']=False
                return
        else:
            target=min(target,playback['end'])
        source=dict(self.last['usdview'])
        source['timeline']=dict(source['timeline'],time=target,playing=False)
        pending={n:v['frame_serial'] for n,v in self.last.items()}
        for name in self.clients: self.apply(name,'timeline',source)
        playback.update(time=target,pending_frames=pending,due=time.monotonic()+1/playback['fps'])

    def capture(self):
        with self.lock:
            if getattr(self,'playback',{}).get('active'): raise ValueError('pause linked playback before capture')
            if getattr(self,'overlay',{}) and self.overlay.get('transaction'): raise ValueError('finish the pending overlay transaction before capture')
            if self.error: raise RuntimeError(self.error)
            states = {n:c.call('view_state') for n,c in self.clients.items()}
            if any(not v['scene'].get('loaded') or v['scene'].get('loading') for v in states.values()):
                raise RuntimeError('scene update is still loading')
            if len({v['scene']['filepath'] for v in states.values()}) != 1:
                raise RuntimeError('viewers have different scene roots')
            if any(v['timeline']['playing'] for v in states.values()):
                raise RuntimeError('pause both timelines before paired capture')
            if any(not states_equal(states['lusdview'],states['usdview'],d) for d in ('camera','display','selection','timeline')):
                raise RuntimeError('cameras differ; resynchronize before capturing')
            capture_id = 'capture-%d'%time.time_ns()
            directory = self.out/capture_id
            directory.mkdir()
            results = {}
            tokens={name:secrets.token_hex(16) for name in self.clients}
            armed=[]
            cleanup=[]
            try:
                for name,client in self.clients.items():
                    armed.append(name)
                    client.call('frame_barrier',{'token':tokens[name]})
                for name,client in self.clients.items():
                    deadline=time.monotonic()+30
                    while time.monotonic()<deadline:
                        if self.stopped.is_set(): raise RuntimeError('capture interrupted')
                        state=client.call('view_state')
                        barrier=state.get('barrier')
                        if barrier and barrier.get('token')==tokens[name]:
                            if not barrier.get('ready'): raise RuntimeError('frame readback failed: '+str(barrier.get('error','not ready')))
                            if barrier['frame_serial']<=states[name]['frame_serial'] or not frame_matches(barrier['state'],states[name]):
                                raise RuntimeError(name+' captured state differs from the requested full view state')
                            result=client.call('screenshot',{'path':str(directory/(name+'.png')),
                                'barrier':tokens[name],'after_frame':states[name]['frame_serial']})
                            if not result.get('written') or result.get('barrier')!=barrier:
                                raise RuntimeError('retained frame changed before screenshot: '+name)
                            if [result['width'],result['height']]!=states[name]['size']:
                                raise RuntimeError('captured physical dimensions differ: '+name)
                            results[name]=result
                            break
                        time.sleep(.03)
                    if name not in results: raise RuntimeError('capture barrier timed out: '+name)
                after={n:c.call('view_state') for n,c in self.clients.items()}
                for name in states:
                    if not frame_matches(after[name],states[name]):
                        raise RuntimeError('native state or scene changed during paired capture: '+name)
            finally:
                for name in armed:
                    try: self.clients[name].call('frame_barrier',{'token':tokens[name],'cancel':True})
                    except (OSError,RuntimeError,ValueError) as error: cleanup.append(name+': '+str(error))
                if cleanup:
                    self.error='could not release retained frame: '+'; '.join(cleanup)
                    self.sync_enabled=False
            if cleanup: raise RuntimeError(self.error)
            from tools.ousddiff.images import compare
            metrics=compare(directory/'usdview.png',directory/'lusdview.png',directory)
            from report import write_report
            report=write_report(directory,directory/'usdview.png',directory/'lusdview.png',metrics)
            result={'report':report,'revision':self.revision,'states':states,'captures':results,'metrics':metrics}
            (directory/'capture.json').write_text(json.dumps(result,indent=2)+'\n')
            self.latest_capture={'directory':capture_id,'revision':self.revision,'metrics':metrics}
            self.publish_control_state(force=True)
            return result

    def publish_control_state(self, force=False):
        now=time.monotonic()
        if not force and now-self._control_written<.5: return
        self._control_written=now
        body={'protocol_version':1,'revision':self.revision,'synchronizing':self.sync_enabled,
              'error':self.error,'control_error':self.control_error,'control_url':self.control_url,'overlay':self.overlay,
              'playback':{k:v for k,v in self.playback.items() if k not in ('pending_frames','due')},
              'capture':self.latest_capture,'operation':self.control_operation}
        data=json.dumps(body,allow_nan=False)
        if data==self._control_text: return
        temporary=self.out/'control-state.pending.json'
        try:
            temporary.write_text(data+'\n')
            temporary.replace(self.out/'control-state.json')
            self._control_text=data
            self.control_error=None
        except OSError as error:
            self.control_error=str(error)

    def _process_control_requests(self):
        import re
        from server import dispatch
        # Claim one bounded command per poll; native UI never waits on the GUI thread.
        paths=(p for p in self.control_requests.glob('*.request.json')
               if re.fullmatch(r'[0-9]+-[0-9]+\.request\.json',p.name))
        for path in sorted(paths)[:1]:
            identifier=path.name[:-len('.request.json')]
            claimed=path.with_name(identifier+'.processing.json')
            try: path.replace(claimed)
            except FileNotFoundError: continue
            self.control_operation={'id':identifier,'running':True}
            self.publish_control_state(force=True)
            try:
                import stat
                metadata=claimed.lstat()
                if not stat.S_ISREG(metadata.st_mode) or metadata.st_size>1024*1024:
                    raise ValueError('native control request must be a regular file smaller than 1 MiB')
                request=json.loads(claimed.read_text())
                if not isinstance(request,dict) or request.get('tool') not in ('capture','sync','viewer_call','overlay','playback','inspect_pair'):
                    raise ValueError('unsupported native control command')
                reply=dispatch(self,{'jsonrpc':'2.0','id':identifier,'method':'tools/call',
                    'params':{'name':request['tool'],'arguments':request.get('arguments',{})}})
                if 'error' in reply: raise RuntimeError(reply['error']['message'])
                result=json.loads(reply['result']['content'][0]['text'])
                if request['tool']=='capture':
                    result={k:result[k] for k in ('report','revision','metrics')}
                response={'id':identifier,'ok':True,'result':result}
            except Exception as error:
                response={'id':identifier,'ok':False,'error':str(error)}
            self.control_operation={'id':identifier,'running':False,'ok':response['ok'],
                                    'message':request.get('tool','command') if response['ok'] else response['error']}
            target=path.with_name(identifier+'.response.json')
            temporary=path.with_name(identifier+'.response.pending.json')
            try:
                temporary.write_text(json.dumps(response,allow_nan=False)+'\n')
                temporary.replace(target)
                claimed.unlink()
            except OSError as error:
                self.control_error=str(error)
            self.publish_control_state(force=True)

    def _wait_scene(self, name, path, before):
        deadline=time.monotonic()+self.args.load_timeout
        client=self.clients[name]
        while time.monotonic()<deadline:
            if self.stopped.is_set(): raise RuntimeError('session stopping')
            state=client.call('view_state')
            scene=state['scene']
            changed=any(scene.get(k)!=before['scene'].get(k) for k in ('filepath','scene_generation','stage_revision'))
            if scene.get('loaded') and not scene.get('loading') and scene['filepath']==path and changed:
                return state
            time.sleep(.05)
        raise RuntimeError(name+' scene update timeout')

    def _restore_views(self, states):
        for name in self.clients:
            for field in ('camera','display','timeline','selection'):
                self.apply(name,field,states[name])
        self.last={n:c.call('view_state') for n,c in self.clients.items()}

    def _enable_overlay(self):
        states={n:c.call('view_state') for n,c in self.clients.items()}
        if any(not v['scene'].get('loaded') or v['scene'].get('loading') or v['timeline']['playing'] for v in states.values()):
            raise ValueError('debug editing requires loaded scenes and paused playback')
        if len({v['scene']['filepath'] for v in states.values()})!=1:
            raise ValueError('debug editing requires matching scene roots')
        usd=self.clients['usdview']
        try:
            info=usd.call('debug_overlay',{'op':'configure','directory':str(self.out/('debug-%d'%time.time_ns()))})
            self.clients['lusdview'].call('load_usd',{'path':info['wrapper']})
            self._wait_scene('lusdview',info['wrapper'],states['lusdview'])
            self._restore_views(states)
            self.overlay=info
        except Exception as failure:
            try:
                current=self.clients['lusdview'].call('view_state')
                usd_state=usd.call('view_state')
                if usd_state['scene']['filepath']!=states['usdview']['scene']['filepath']:
                    usd.call('debug_overlay',{'op':'detach'})
                self.clients['lusdview'].call('load_usd',{'path':states['lusdview']['scene']['filepath']})
                self._wait_scene('lusdview',states['lusdview']['scene']['filepath'],current)
                self._restore_views(states)
            except Exception as rollback:
                self.error='overlay configuration failed and recovery failed: '+str(rollback)
                self.sync_enabled=False
            raise RuntimeError('overlay configuration failed: '+str(failure)) from failure

    def debug_edit(self, action='status', edits=None):
        with self.lock:
            if getattr(self,'playback',{}).get('active') and action!='status': raise ValueError('pause linked playback before editing')
            if action=='status':
                if self.overlay: self.overlay=self.clients['usdview'].call('debug_overlay',{'op':'status'})
                return self.overlay or {'configured':False}
            if self.error: raise RuntimeError(self.error)
            if action not in ('edit','undo','redo','clear','export'):
                raise ValueError('unknown debug overlay action')
            if action=='edit' and (not isinstance(edits,list) or not 1<=len(edits)<=128):
                raise ValueError('edit requires 1..128 operations')
            if self.overlay is None:
                if action!='edit': raise ValueError('no debug overlay configured')
                self._enable_overlay()
            usd=self.clients['usdview']
            if action=='export': return usd.call('debug_overlay',{'op':'export'})
            states={n:c.call('view_state') for n,c in self.clients.items()}
            if any(v['scene'].get('loading') or v['timeline']['playing'] for v in states.values()):
                raise ValueError('pause playback and wait for scene updates before editing')
            try:
                prepared=usd.call('debug_overlay',{'op':'prepare','action':action,'edits':edits})
            except Exception as failure:
                # A timed-out prepare may have completed on the GUI thread.
                try:
                    self.overlay=usd.call('debug_overlay',{'op':'status'})
                    if self.overlay.get('transaction'):
                        self.overlay=usd.call('debug_overlay',{'op':'rollback','transaction':self.overlay['transaction']})
                except Exception as recovery:
                    self.error='overlay prepare failed; transaction state uncertain: '+str(recovery)
                    self.sync_enabled=False
                raise RuntimeError('overlay preparation failed: '+str(failure)) from failure
            identifier=prepared['transaction']
            committed=False
            try:
                self.overlay=usd.call('debug_overlay',{'op':'commit','transaction':identifier})
                committed=True
                self.clients['lusdview'].call('reload_layer',{'path':self.overlay['overlay']})
                self._wait_scene('lusdview',self.overlay['wrapper'],states['lusdview'])
                self._restore_views(states)
                self.overlay=usd.call('debug_overlay',{'op':'accept','transaction':identifier})
                self.revision+=1
                return self.overlay
            except Exception as failure:
                try:
                    current=self.clients['lusdview'].call('view_state')
                    self.overlay=usd.call('debug_overlay',{'op':'rollback','transaction':identifier})
                    if committed:
                        # A full reload also recovers a failed incremental update.
                        self.clients['lusdview'].call('load_usd',{'path':self.overlay['wrapper']})
                        self._wait_scene('lusdview',self.overlay['wrapper'],current)
                    self._restore_views(states)
                except Exception as rollback:
                    self.error='overlay edit failed and recovery failed: '+str(rollback)
                    self.sync_enabled=False
                    raise RuntimeError('overlay edit failed; recovery incomplete: '+str(failure)+'; '+str(rollback)) from failure
                raise RuntimeError('overlay edit rolled back: '+str(failure)) from failure

    def benchmark(self, samples=30, warmup=10):
        """Shared-session samples with the owned peer process suspended on POSIX."""
        if os.name != 'posix':
            raise ValueError('peer suspension currently requires POSIX')
        if type(samples) is not int or not 2 <= samples <= 1000 or type(warmup) is not int or not 1 <= warmup <= 100:
            raise ValueError('samples must be 2..1000 and warmup 1..100')
        with self.lock:
            if getattr(self,'playback',{}).get('active'): raise ValueError('pause linked playback before benchmarking')
            if getattr(self,'overlay',{}) and self.overlay.get('transaction'): raise ValueError('finish the pending overlay transaction before benchmarking')
            if self.error: raise RuntimeError(self.error)
            states={n:c.call('view_state') for n,c in self.clients.items()}
            if any(not v['scene'].get('loaded') or v['scene'].get('loading') or v['timeline']['playing'] for v in states.values()):
                raise ValueError('benchmark requires loaded scenes and paused playback')
            if any(not states_equal(states['lusdview'],states['usdview'],d) for d in ('camera','display','selection','timeline')):
                raise ValueError('resynchronize viewers before benchmarking')
            if states['lusdview']['size'] != states['usdview']['size']:
                raise ValueError('benchmark requires equal physical viewport sizes')
            if len({v['scene']['filepath'] for v in states.values()}) != 1:
                raise ValueError('benchmark requires matching scene roots')
            import statistics
            results={}
            for active,peer_index in (('lusdview',1),('usdview',0)):
                peer=self.children[peer_index]
                if peer.poll() is not None: raise RuntimeError('peer viewer exited')
                client=self.clients[active]
                rows=[]
                os.killpg(peer.pid,signal.SIGSTOP)
                try:
                    time.sleep(.2)
                    serial=client.call('view_state')['frame_serial']
                    for index in range(warmup+samples):
                        if self.stopped.is_set(): raise RuntimeError('benchmark interrupted')
                        client.call('viewport',{'op':'refresh'})
                        deadline=time.monotonic()+30
                        while time.monotonic()<deadline:
                            stats=client.call('get_render_stats')
                            current=stats.get('frame_serial',0)
                            if current>serial:
                                serial=current
                                break
                            time.sleep(.01)
                        else: raise RuntimeError('benchmark frame timeout: '+active)
                        value=stats.get('cpu_frame_s')
                        if not isinstance(value,(float,int)) or not math.isfinite(value) or value<=0:
                            raise ValueError('viewer returned invalid CPU timing: '+active)
                        if index>=warmup: rows.append(stats)
                    after=client.call('view_state')
                    if any(not states_equal(states[active],after,d) for d in ('camera','display','selection','timeline')) or after['scene'].get('loading') or any(after['scene'].get(k)!=states[active]['scene'].get(k) for k in ('filepath','stage_revision')):
                        raise RuntimeError('view changed during benchmark: '+active)
                finally:
                    try: os.killpg(peer.pid,signal.SIGCONT)
                    except ProcessLookupError: pass
                values=sorted(row['cpu_frame_s'] for row in rows)
                results[active]={'samples':rows,'median_cpu_frame_s':statistics.median(values),
                                 'min_cpu_frame_s':values[0],'max_cpu_frame_s':values[-1],
                                 'timer_scope':rows[0]['timer_scope']}
            result={'kind':'shared-session','peer_pause':'SIGSTOP of owned process group; both windows and GPU allocations retained',
                    'timing_policy':'native CPU scopes differ; no GPU-only or isolated throughput claim',
                    'warmup_frames':warmup,'sample_count':samples,'states':states,'viewers':results}
            path=self.out/('benchmark-%d.json'%time.time_ns())
            result['path']=str(path)
            path.write_text(json.dumps(result,indent=2)+'\n')
            return result

    def stop(self):
        self.stopped.set()
        for child in self.children:
            try: os.killpg(child.pid,signal.SIGTERM)
            except ProcessLookupError: pass
        for child in self.children:
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid,signal.SIGKILL)
                child.wait()
        for log in self.logs: log.close()
