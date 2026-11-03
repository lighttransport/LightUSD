#!/usr/bin/env python3
"""Headless MCP/render regression with no DISPLAY; requires the installed fork."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def run(args):
    from PIL import Image,ImageChops
    scene=Path('tests/usda/view-control-skeleton.usda').resolve()
    original=hashlib.sha256(scene.read_bytes()).hexdigest()
    environment=dict(os.environ);environment.pop('DISPLAY',None);environment.pop('WAYLAND_DISPLAY',None)
    command=[sys.executable,str(Path(__file__).with_name('control.py')),'headless',str(scene),
             '--out',str(args.out),'--lusdrender',str(args.lusdrender),'--usdrecord',str(args.usdrecord),
             '--usd-python',str(args.usd_python),'--backend',args.backend,'--mcp-stdio']
    process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=environment)
    serial=0
    def rpc(tool,arguments=None,fail=False):
        nonlocal serial
        serial+=1
        request={'jsonrpc':'2.0','id':serial,'method':'tools/call','params':{'name':tool,'arguments':arguments or {}}}
        process.stdin.write(json.dumps(request)+'\n');process.stdin.flush()
        line=process.stdout.readline()
        if not line: raise RuntimeError('MCP process exited: '+process.stderr.read())
        response=json.loads(line);assert response['id']==serial,response
        if fail: assert 'error' in response,response;return response
        assert 'error' not in response,response
        return json.loads(response['result']['content'][0]['text'])
    def bounds(path):
        with Image.open(path) as source:
            image=source.convert('RGB');mask=ImageChops.difference(image,Image.new('RGB',image.size))
            channel=ImageChops.lighter(ImageChops.lighter(mask.getchannel('R'),mask.getchannel('G')),mask.getchannel('B'))
            return channel.point(lambda p:255 if p>12 else 0).getbbox()
    try:
        rpc('configure',{'state':{'size':[256,192]}})
        baseline=rpc('render')
        assert 'AMD' in baseline['usdrecord']['graphics']['vendor'] or not args.require_amd,baseline['usdrecord']['graphics']
        assert baseline['usdrecord']['graphics']['renderer']
        assert all(Path(path).is_file() for path in baseline['images'].values())
        camera=copy.deepcopy(baseline['state']['camera'])
        camera['projection']='orthographic';camera['ortho_height']=4.0
        camera['shift']=[.25,-.15]
        right,up=camera['transform'][0:3],camera['transform'][4:7]
        camera['transform'][0:3]=up;camera['transform'][4:7]=[-v for v in right]
        rpc('configure',{'state':{'camera':camera}})
        rolled=rpc('render')
        a,b=[bounds(rolled['images'][name]) for name in ('usdrecord','lusdrender')]
        assert a and b and max(abs(x-y) for x,y in zip(a,b))<=3,(a,b)
        print('PASS same rolled, shifted orthographic viewport on '+args.backend,flush=True)
        # A near plane beyond the complete scene must eliminate all foreground.
        clipped=copy.deepcopy(camera);clipped['near']=camera['far']-.01
        rpc('configure',{'state':{'camera':clipped}})
        empty=rpc('render')
        assert all(bounds(empty['images'][name]) is None for name in ('usdrecord','lusdrender'))
        rpc('configure',{'state':{'camera':camera}})
        before=rpc('status')
        rpc('configure',{'state':{'display':{'hud':True}}},fail=True)
        assert rpc('status')['revision']==before['revision']
        rpc('overlay',{'action':'edit','edits':[{'op':'set_attribute','path':'/absent','property':'size','value':.1}]},fail=True)
        assert rpc('status')['revision']==before['revision']
        rpc('overlay',{'action':'edit','edits':[{'op':'set_attribute','path':'/Rig/Skeleton/ChildGeometry','property':'size','value':.1}]})
        inspected=rpc('inspect_prim',{'path':'/Rig/Skeleton/ChildGeometry','limit':64})
        size=next(p for p in inspected['properties'] if p['name']=='size')
        assert abs(float(size['value_preview'])-.1)<1e-5,size
        assert inspected['implementation']=='OpenUSD'
        rpc('overlay',{'action':'undo'});rpc('overlay',{'action':'redo'});rpc('overlay',{'action':'clear'})
        rpc('configure',{'state':{'display':{'skeleton':True},'selection':['/Rig/Skeleton/Skin'],'time':6}})
        debug=rpc('render')
        assert debug['diagnostic_segments']>=5,debug['diagnostic_segments']
        for name in ('usdrecord','lusdrender'):
            with Image.open(debug['images'][name]) as source:
                pixels=source.convert('RGB').tobytes()
                colored=sum(1 for r,g,b in zip(pixels[0::3],pixels[1::3],pixels[2::3]) if r>100 and g>100 and b<min(r,g)*.8)
                assert colored>20,(name,'missing visible selection annotation',colored)
        assert debug['state']['time']==6
        process.stdin.write(json.dumps({'jsonrpc':'2.0','id':10000,'method':'tools/call','params':{'name':'image','arguments':{'name':'diff'}}})+'\n');process.stdin.flush()
        image=json.loads(process.stdout.readline())
        assert any(block['type']=='image' for block in image['result']['content'])
        rpc('stop');process.stdin.close();process.wait(timeout=15)
        assert process.returncode==0,process.stderr.read()
        assert hashlib.sha256(scene.read_bytes()).hexdigest()==original
        (args.out/'headless-live-results.json').write_text(json.dumps({'baseline':baseline,'rolled':rolled,'debug':debug},indent=2)+'\n')
        print('PASS clipping, atomic validation, typed edits/history, native inspection, bones, selection, time and image MCP',flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:process.kill();process.wait()


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--lusdrender',type=Path,default=Path('build_ninja/lusdview_lightrt_c_build/lusdrender'))
    parser.add_argument('--usdrecord',type=Path,default=Path('../OpenUSD/dist-v26.11/bin/usdrecord'))
    parser.add_argument('--usd-python',type=Path,default=Path('../OpenUSD/.venv/bin/python3'))
    parser.add_argument('--backend',choices=('cpu','vkr','hip','cuda'),default='cpu')
    parser.add_argument('--require-amd',action='store_true')
    run(parser.parse_args())
