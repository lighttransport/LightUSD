#!/usr/bin/env python3
"""MCP endpoint inspection and capture comparison for persistent USD viewers."""
import argparse
import json
from pathlib import Path
import sys
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError('MCP redirects are disabled')


class ViewerClient:
    def __init__(self, endpoint, token=None, timeout=30):
        from urllib.parse import urlparse
        parsed = urlparse(endpoint)
        if parsed.scheme != 'http' or parsed.hostname not in ('localhost', '127.0.0.1', '::1'):
            raise ValueError('viewer endpoint must be loopback HTTP')
        if parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError('endpoint credentials, query and fragments are not supported')
        if isinstance(timeout,bool) or not isinstance(timeout,(int,float)) or not 0 < timeout < 86400:
            raise ValueError('timeout must be positive and bounded')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        self.endpoint, self.token, self.timeout = endpoint, token, timeout
        self.serial = 0

    def request(self, method, params=None):
        self.serial += 1
        request_id = self.serial
        payload = {'jsonrpc': '2.0', 'id': request_id, 'method': method}
        if params is not None:
            payload['params'] = params
        headers = {'Content-Type': 'application/json'}
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        request = urllib.request.Request(self.endpoint, json.dumps(payload, allow_nan=False).encode(), headers)
        with self.opener.open(request, timeout=self.timeout) as response:
            raw = response.read(16 * 1024 * 1024 + 1)
        if len(raw) > 16 * 1024 * 1024:
            raise RuntimeError('MCP response exceeds 16 MiB')
        result = json.loads(raw)
        if not isinstance(result,dict) or type(result.get('id')) is not int or result.get('id') != request_id or result.get('jsonrpc') != '2.0':
            raise RuntimeError('invalid MCP response identity')
        if 'error' in result:
            raise RuntimeError('MCP error: ' + json.dumps(result['error']))
        return result['result']

    def call(self, name, arguments=None):
        result = self.request('tools/call', {'name': name, 'arguments': arguments or {}})
        if result.get('isError'):
            raise RuntimeError('viewer tool failed: ' + json.dumps(result))
        if 'structuredContent' in result:
            return result['structuredContent']
        text = [c['text'] for c in result.get('content', []) if c.get('type') == 'text']
        if len(text) != 1:
            raise RuntimeError('expected one JSON tool result')
        return json.loads(text[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('tools', 'call'):
        p = sub.add_parser(name)
        p.add_argument('--endpoint', required=True)
        p.add_argument('--token-file', type=Path)
        p.add_argument('--timeout', type=float, default=30)
        if name == 'call':
            p.add_argument('tool')
            p.add_argument('--arguments', type=Path, help='JSON arguments file')
    p = sub.add_parser('compare')
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p = sub.add_parser('headless', help='Agent batch/MCP control using lusdrender and surfaceless usdrecord')
    p.add_argument('scene',type=Path)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--state',type=Path,help='Native view_state, paired capture.json, or batch state JSON')
    p.add_argument('--session-subdir',action='store_true',help='Create a fresh session below --out on every launch')
    p.add_argument('--lusdrender',type=Path,default=Path('build_ninja/lusdview_lightrt_c_build/lusdrender'))
    p.add_argument('--usdrecord',type=Path,default=Path('../OpenUSD/dist-v26.11/bin/usdrecord'))
    p.add_argument('--usd-python',type=Path,default=Path('../OpenUSD/.venv/bin/python3'))
    p.add_argument('--usd-env',action='append',default=[],metavar='NAME=VALUE')
    p.add_argument('--backend',choices=('cpu','vkr','hip','cuda','vkr-instanced','vkr-streamed'),default='cpu')
    p.add_argument('--curve-segments',type=int,default=8,help='Instanced GPU spline samples per span, 1..64')
    p.add_argument('--size',help='Override output WIDTHxHEIGHT (16..4096)')
    p.add_argument('--time',type=float,help='Override USD time code')
    p.add_argument('--camera',dest='camera_path',help='Resolve and freeze this authored camera')
    p.add_argument('--timeout',type=float,default=600,help='Bound each owned subprocess in seconds')
    p.add_argument('--mcp-stdio',action='store_true')
    p = sub.add_parser('verify', help='Run catalogue acceptance gates against hashed evidence')
    p.add_argument('--catalogue',type=Path,default=Path('tests/verification/dataset-gaps.json'))
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--evidence-root',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p = sub.add_parser('validate', help='Run JSON-configured multi-scene time and AOV comparisons')
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--assets-root',type=Path)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--lusdrender',type=Path,default=Path('build_ninja/lusdview_lightrt_c_build/lusdrender'))
    p.add_argument('--usdrecord',type=Path,default=Path('../OpenUSD/dist-v26.11/bin/usdrecord'))
    p.add_argument('--usd-python',type=Path,default=Path('../OpenUSD/.venv/bin/python3'))
    p.add_argument('--usd-env',action='append',default=[],metavar='NAME=VALUE')
    p.add_argument('--timeout',type=float,default=600)
    p = sub.add_parser('launch')
    p.add_argument('scene', type=Path)
    p.add_argument('--lusdview', type=Path, default=Path('build_ninja/lusdview'))
    p.add_argument('--usdview', type=Path, default=Path('../OpenUSD/dist/bin/usdview'))
    p.add_argument('--usdview-env', action='append', default=[], metavar='NAME=VALUE')
    p.add_argument('--usdview-python', type=Path, help='Explicit development bootstrap interpreter')
    p.add_argument('--bridge-source', type=Path, help='Fork remoteControl.py for development bootstrap')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--backend', choices=('vk','gl'), default='vk')
    p.add_argument('--size', default='640x480')
    p.add_argument('--headless-lusdview', action='store_true')
    p.add_argument('--load-timeout', type=float, default=600)
    p.add_argument('--mcp-stdio', action='store_true')
    for name in ('status','capture','benchmark','overlay','playback','stop'):
        p = sub.add_parser(name)
        p.add_argument('--session', required=True, type=Path)
        if name == 'playback':
            p.add_argument('--action',choices=('status','play','pause'),default='status')
            p.add_argument('--fps',type=float,default=30)
            p.add_argument('--no-loop',action='store_true')
        if name == 'overlay':
            p.add_argument('--action',choices=('status','edit','undo','redo','clear','export'),default='status')
            p.add_argument('--edits',type=Path,help='JSON array of edit operations')
        if name == 'benchmark':
            p.add_argument('--samples',type=int,default=30)
            p.add_argument('--warmup',type=int,default=10)
    args = parser.parse_args()
    if args.command == 'verify':
        from verification import verify_catalogue
        if args.catalogue.stat().st_size>1024*1024 or args.evidence.stat().st_size>16*1024*1024:
            parser.error('catalogue/evidence input exceeds size limit')
        result=verify_catalogue(json.loads(args.catalogue.read_text()),json.loads(args.evidence.read_text()),args.evidence_root)
        args.out.parent.mkdir(parents=True,exist_ok=True)
        args.out.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        print(json.dumps(dict(passed=result['passed'],report=str(args.out)),indent=2))
        raise SystemExit(0 if result['passed'] else 1)
    if args.command == 'validate':
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        from validation import run
        result=run(args)
        print(json.dumps(result,indent=2))
        if not result['passed']: sys.exit(1)
        return
    if args.command == 'headless':
        from headless import run
        run(args)
        return
    if args.command == 'launch':
        import re
        if not re.fullmatch(r'[1-9][0-9]{0,4}x[1-9][0-9]{0,4}',args.size):
            raise ValueError('size must be WIDTHxHEIGHT')
        if any(n < 16 or n > 8192 for n in map(int,args.size.split('x'))):
            raise ValueError('viewport dimensions must be 16..8192')
        import math
        if not math.isfinite(args.load_timeout) or not 0 < args.load_timeout <= 86400:
            raise ValueError('load timeout must be finite and in (0,86400]')
        if bool(args.usdview_python) != bool(args.bridge_source):
            raise ValueError('--usdview-python and --bridge-source must be supplied together')
        sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
        from session import Session
        from server import start, dispatch
        import signal
        session=Session(args)
        server=None
        def stop_signal(signum, frame): session.stopped.set()
        signal.signal(signal.SIGTERM,stop_signal)
        signal.signal(signal.SIGINT,stop_signal)
        try:
            session.start()
            server=start(session)
            info={'endpoint':'http://127.0.0.1:%d/mcp'%server.server_port,
                  'control_url':'http://127.0.0.1:%d/'%server.server_port,
                  'token_file':str(session.token_file), 'directory':str(session.out),
                  'viewers':{name:client.endpoint for name,client in session.clients.items()}}
            session.control_url=info['control_url']
            session.publish_control_state(force=True)
            (session.out/'session.json').write_text(json.dumps(info,indent=2)+'\n')
            if args.mcp_stdio:
                import queue
                import threading
                incoming = queue.Queue(maxsize=16)
                def read_stdin():
                    while not session.stopped.is_set():
                        line = sys.stdin.readline(1024*1024+1)
                        while not session.stopped.is_set():
                            try:
                                incoming.put(line, timeout=.2)
                                break
                            except queue.Full:
                                pass
                        if not line: break
                threading.Thread(target=read_stdin, daemon=True).start()
                while not session.stopped.is_set():
                    if any(p.poll() is not None for p in session.children):
                        raise RuntimeError('owned viewer exited; closing the paired session')
                    try:
                        line = incoming.get(timeout=.2)
                    except queue.Empty:
                        continue
                    if not line: break
                    try:
                        if len(line)>1024*1024: raise ValueError('MCP line exceeds 1 MiB')
                        request=json.loads(line)
                        if request.get('method','').startswith('notifications/'): continue
                        print(json.dumps(dispatch(session,request)),flush=True)
                    except ValueError as error:
                        print(json.dumps({'jsonrpc':'2.0','id':None,'error':{'code':-32700,'message':str(error)}}),flush=True)
            else:
                print(json.dumps(info,indent=2),flush=True)
                while not session.stopped.wait(.2):
                    if any(p.poll() is not None for p in session.children):
                        raise RuntimeError('owned viewer exited; closing the paired session')
        finally:
            if server:
                server.shutdown(); server.server_close()
            session.stop()
        return
    elif args.command in ('status','capture','benchmark','overlay','playback','stop'):
        info=json.loads((args.session/'session.json').read_text())
        token=Path(info['token_file']).read_text().strip()
        arguments={'samples':args.samples,'warmup':args.warmup} if args.command=='benchmark' else {}
        if args.command=='playback':
            arguments={'action':args.action,'fps':args.fps,'loop':not args.no_loop}
        if args.command=='overlay':
            arguments={'action':args.action}
            if args.edits: arguments['edits']=json.loads(args.edits.read_text())
        result=ViewerClient(info['endpoint'],token,120).call(args.command,arguments)
    elif args.command == 'compare':
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        from tools.ousddiff.images import compare
        result = compare(args.reference, args.candidate, args.out)
        from report import write_report
        result["report"] = write_report(args.out,args.reference,args.candidate,result)
        (args.out / 'metrics.json').write_text(json.dumps(result, indent=2) + '\n')
    else:
        token = args.token_file.read_text().strip() if args.token_file else None
        client = ViewerClient(args.endpoint, token, args.timeout)
        client.request('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {},
                                     'clientInfo': {'name': 'view-control', 'version': '0.1'}})
        if args.command == 'tools':
            result = client.request('tools/list')
        else:
            arguments = json.loads(args.arguments.read_text()) if args.arguments else {}
            if not isinstance(arguments, dict):
                raise ValueError('arguments must be a JSON object')
            result = client.call(args.tool, arguments)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
