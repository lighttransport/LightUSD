"""Authenticated local controller MCP service."""
import hmac
import json
import re
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading


# Paired scene edits must pass through the shared overlay transaction.
VIEWER_TOOLS = frozenset((
    'compare_panel','view_state','get_scene_info','get_render_stats','viewport','render_settings',
    'select','get_focused_prim','set_focus','inspect_prim','timeline','skinning',
    'stage_info','prim_list','list_prims','prim_get','prim_get_metadata',
    'attr_list','attr_get','variant_list_sets','variant_get_selection',
    'search','query_prims_by_type'))


EDIT_SCHEMA = {
    'type':'object',
    'properties':{
        'op':{'type':'string','enum':['set_attribute','block_attribute','clear_property','relationship','connections','variant','metadata','property_metadata']},
        'path':{'type':'string','description':'Existing absolute prim path'},
        'property':{'type':'string','description':'USD property name, including namespace if present'},
        'type':{'type':'string','description':'USD declaration required for new attributes, e.g. double, float3 or color3f[]'},
        'value':{'description':'Native JSON value; vectors/matrices are numeric lists, quaternions are [real,x,y,z]'},
        'time':{'type':'number','description':'Optional time code; omit to author the default'},
        'targets':{'type':'array','maxItems':4096,'items':{'type':'string'}},
        'set':{'type':'string','description':'Existing variant set name'},
        'key':{'type':'string','description':'Supported diagnostic metadata key'}},
    'required':['op','path'],
    'additionalProperties':False}


def dispatch(session, request):
    if not isinstance(request,dict) or request.get('jsonrpc')!='2.0':
        raise ValueError('invalid JSON-RPC request')
    method=request.get('method')
    response={'jsonrpc':'2.0','id':request.get('id')}
    try:
        if method=='initialize':
            result={'protocolVersion':'2025-03-26','serverInfo':{'name':'view-control','version':'0.2'},'capabilities':{'tools':{}}}
        elif method=='ping': result={}
        elif method=='tools/list':
            definitions = (
                ('status','Read synchronized native viewer states and diagnostics',{},()),
                ('sync','Enable/disable synchronization or resynchronize from viewer',
                 {'enabled':{'type':'boolean'},'source':{'type':'string','enum':['lusdview','usdview']}},()),
                ('viewer_tools','List supported native read/navigation/display tools for a viewer',
                 {'viewer':{'type':'string','enum':['lusdview','usdview']}},('viewer',)),
                ('inspect_pair','Read the same prim through both native paginated inspectors',
                 {'path':{'type':'string'},'offset':{'type':'integer','minimum':0},'limit':{'type':'integer','minimum':1,'maximum':256}},('path',)),
                ('viewer_call','Call native diagnostics/navigation/display tools; use overlay for scene edits',
                 {'viewer':{'type':'string','enum':['lusdview','usdview']},
                  'tool':{'type':'string'},'arguments':{'type':'object'}},('viewer','tool')),
                ('playback','Drive one shared animation clock, advancing after both completed-frame acknowledgments',
                 {'action':{'type':'string','enum':['status','play','pause']},'fps':{'type':'number','minimum':1,'maximum':60},'loop':{'type':'boolean'}},()),
                ('overlay','Edit shared sparse debug opinions, undo/redo, clear or export; source assets remain unchanged',
                 {'action':{'type':'string','enum':['status','edit','undo','redo','clear','export']},
                  'edits':{'type':'array','items':EDIT_SCHEMA,'minItems':1,'maxItems':128}},()),
                ('capture','Capture matching completed camera/time frames and write an offline comparison report',{},()),
                ('benchmark','Sample native CPU timings with both windows retained and the owned peer suspended',
                 {'samples':{'type':'integer','minimum':2,'maximum':1000},'warmup':{'type':'integer','minimum':1,'maximum':100}},()),
                ('stop','Stop this session and its owned viewers',{},()))
            result={'tools':[{'name':name,'description':description,
                     'inputSchema':{'type':'object','properties':properties,
                                    'required':list(required),'additionalProperties':False}}
                    for name,description,properties,required in definitions]}
        elif method=='tools/call':
            params=request['params']; name=params['name']; args=params.get('arguments',{})
            if not isinstance(args,dict): raise ValueError('arguments must be an object')
            if name=='stop': session.stopped.set()
            with session.lock:
                if name=='status': value=session.status()
                elif name=='sync':
                    if 'source' in args:
                        source=args['source']
                        if source not in session.clients: raise ValueError('invalid source viewer')
                        state=session.clients[source].call('view_state')
                        target='usdview' if source=='lusdview' else 'lusdview'
                        for domain in ('camera','display','selection','timeline'): session.apply(target,domain,state)
                        session.last[source]=state
                    enabled=args.get('enabled',True)
                    if not isinstance(enabled,bool): raise ValueError('enabled must be boolean')
                    session.sync_enabled=enabled
                    session.error=None
                    value=session.status()
                elif name=='inspect_pair':
                    value={viewer:client.call('inspect_prim',args) for viewer,client in session.clients.items()}
                elif name=='viewer_tools':
                    viewer=args['viewer']
                    if viewer not in session.clients: raise ValueError('invalid viewer')
                    native=session.clients[viewer].request('tools/list')
                    value={'tools':[t for t in native['tools'] if t['name'] in VIEWER_TOOLS]}
                elif name=='viewer_call':
                    viewer=args['viewer']
                    if viewer not in session.clients: raise ValueError('invalid viewer')
                    if args['tool'] not in VIEWER_TOOLS:
                        raise ValueError('tool is outside paired diagnostics; use overlay for shared scene edits')
                    value=session.clients[viewer].call(args['tool'],args.get('arguments',{}))
                elif name=='playback': value=session.control_playback(args.get('action','status'),args.get('fps',30),args.get('loop',True))
                elif name=='overlay': value=session.debug_edit(args.get('action','status'),args.get('edits'))
                elif name=='capture': value=session.capture()
                elif name=='benchmark': value=session.benchmark(args.get('samples',30),args.get('warmup',10))
                elif name=='stop':
                    session.stopped.set(); value={'stopping':True}
                else: raise ValueError('unknown controller tool')
            result={'content':[{'type':'text','text':json.dumps(value,allow_nan=False)}]}
        else: raise ValueError('unsupported method')
        response['result']=result
    except Exception as error:
        response['error']={'code':-32000,'message':str(error)}
    return response


def start(session):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args): pass
        def do_GET(self):
            if self.path=='/':
                data=Path(__file__).with_name('dashboard.html').read_bytes()
                mime='text/html; charset=utf-8'
            else:
                if not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+session.token):
                    self.send_error(401); return
                match=re.fullmatch(r'/capture/(capture-[0-9]+)/(usdview|lusdview|diff|heatmap)\.png',self.path)
                if not match:
                    self.send_error(404); return
                target=(session.out/match[1]/(match[2]+'.png')).resolve()
                if not target.is_relative_to(session.out) or not target.is_file():
                    self.send_error(404); return
                if target.stat().st_size>64*1024*1024:
                    self.send_error(413); return
                data=target.read_bytes();mime='image/png'
            self.send_response(200)
            self.send_header('Content-Type',mime)
            self.send_header('Content-Length',str(len(data)))
            self.send_header('Cache-Control','no-store')
            self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob:; frame-ancestors 'none'")
            self.end_headers();self.wfile.write(data)

        def do_POST(self):
            if self.path!='/mcp':
                self.send_error(404); return
            if not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+session.token):
                self.send_error(401); return
            try:
                self.connection.settimeout(30)
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<=1024*1024: raise ValueError('invalid body length')
                body=self.rfile.read(size)
                if len(body)!=size: raise ValueError('incomplete body')
                request=json.loads(body)
                if not isinstance(request,dict): raise ValueError('request must be an object')
                if request.get('method')=='notifications/initialized':
                    self.send_response(202); self.send_header('Content-Length','0'); self.end_headers(); return
                result=dispatch(session,request)
                data=json.dumps(result,allow_nan=False).encode()
                self.send_response(200)
                self.send_header('Content-Type','application/json')
                self.send_header('Content-Length',str(len(data)))
                self.end_headers(); self.wfile.write(data)
            except (ValueError,OSError,RecursionError) as error:
                self.send_error(400,str(error))
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    server.daemon_threads=True
    threading.Thread(target=server.serve_forever,daemon=True).start()
    return server
