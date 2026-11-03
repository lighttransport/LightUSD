#!/usr/bin/env python3
"""Opt-in integration checks against an already launched paired viewer session."""
import argparse
import copy
import json
from pathlib import Path
import time
import urllib.error

from control import ViewerClient
from session import camera_key, equivalent


def run(directory, prim):
    info=json.loads((directory/'session.json').read_text())
    token=Path(info['token_file']).read_text().strip()
    client=ViewerClient(info['endpoint'],token,60)
    original=client.call('status')['viewers']['usdview']['camera']
    results=[]
    def settled():
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            state=client.call('status')
            if state['error']: raise AssertionError(state['error'])
            viewers=state['viewers']
            if equivalent(camera_key(viewers['usdview']['camera']),camera_key(viewers['lusdview']['camera'])):
                return state
            time.sleep(.05)
        raise AssertionError('camera did not synchronize')
    def native(viewer,tool,args):
        result=client.call('viewer_call',{'viewer':viewer,'tool':tool,'arguments':args})
        time.sleep(.3)
        return result
    for viewer,endpoint in info['viewers'].items():
        try:
            ViewerClient(endpoint,timeout=3).request('tools/list')
            raise AssertionError('unauthenticated request accepted: '+viewer)
        except urllib.error.HTTPError as error:
            assert error.code==401
    results.append('both viewer endpoints reject missing authentication')
    before=settled()['revision']
    native('lusdview','viewport',{'op':'orbit','dx':30,'dy':15})
    assert settled()['revision']>before
    results.append('lusdview navigation propagates to usdview')
    camera=client.call('status')['viewers']['usdview']['camera']
    camera=copy.deepcopy(camera)
    right,up=camera['transform'][0:3],camera['transform'][4:7]
    camera['transform'][0:3]=up
    camera['transform'][4:7]=[-v for v in right]
    native('usdview','viewport',{'op':'set_camera','camera':camera})
    state=settled()
    assert equivalent(camera_key(camera),camera_key(state['viewers']['lusdview']['camera']))
    results.append('rolled usdview pose propagates to lusdview')
    camera=copy.deepcopy(camera)
    camera.update(projection='orthographic',ortho_height=3.0,shift=[.2,-.1])
    native('usdview','viewport',{'op':'set_camera','camera':camera})
    state=settled()
    assert equivalent(camera_key(camera),camera_key(state['viewers']['lusdview']['camera']))
    for viewer in ('usdview','lusdview'):
        bad=copy.deepcopy(camera);bad['transform'][0]*=4;bad['transform'][1]*=4;bad['transform'][2]*=4
        try:
            native(viewer,'viewport',{'op':'set_camera','camera':bad})
            raise AssertionError('nonrigid pose accepted')
        except RuntimeError: pass
        assert equivalent(camera_key(camera),camera_key(native(viewer,'viewport',{'op':'get'})['camera']))
    results.append('orthographic shift matches and invalid poses leave state intact')
    for viewer in ('lusdview','usdview'):
        before=native(viewer,'render_settings',{})
        try:
            native(viewer,'render_settings',{'mode':'invalid-mode','guide':not before['guide'],'background':'white'})
            raise AssertionError('invalid display update accepted')
        except RuntimeError: pass
        after=native(viewer,'render_settings',{})
        assert after==before, 'invalid display update changed state'
    results.append('invalid mixed display updates leave native state intact')
    native('usdview','render_settings',{'mode':'wireframe','guide':True})
    state=settled()
    assert state['viewers']['lusdview']['display']['mode']=='wireframe'
    assert state['viewers']['lusdview']['display']['guide']
    native('lusdview','render_settings',{'mode':'shaded','guide':False})
    assert settled()['viewers']['usdview']['display']['mode']=='shaded'
    results.append('shading and purpose changes propagate both ways')
    native('usdview','select',{'op':'set','paths':[prim]})
    assert settled()['viewers']['lusdview']['selection']==[prim]
    native('lusdview','select',{'op':'clear'})
    assert settled()['viewers']['usdview']['selection']==[]
    results.append('selection and clearing propagate both ways')
    native('lusdview','viewport',{'op':'set_camera','camera':original})
    state=settled();revision=state['revision']
    time.sleep(.5)
    assert client.call('status')['revision']==revision
    results.append('stable views do not produce echo revisions')
    timelines=state['viewers']
    if all(v['scene'].get('has_animation') for v in timelines.values()):
        timeline=timelines['usdview']['timeline']
        start,end=timeline['start'],timeline['end']
        for source,fraction in (('lusdview',.33),('usdview',.66)):
            target=start+(end-start)*fraction
            native(source,'timeline',{'op':'seek','time':target})
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                state=client.call('status')
                if state['error']: raise AssertionError(state['error'])
                if all(abs(v['timeline']['time']-target)<1e-5 and not v['scene']['loading'] for v in state['viewers'].values()): break
                time.sleep(.1)
            else: raise AssertionError('paused timeline did not synchronize')
        results.append('paused timeline changes propagate both ways')
    if all(v['scene'].get('has_animation') for v in state['viewers'].values()):
        start_time=state['viewers']['usdview']['timeline']['time']
        client.call('playback',{'action':'play','fps':12,'loop':False})
        try:
            client.call('capture')
            raise AssertionError('capture accepted during linked playback')
        except RuntimeError as error:
            assert 'pause linked playback' in str(error)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            state=client.call('status')
            if state['error']: raise AssertionError(state['error'])
            if state['playback']['time']>start_time: break
            time.sleep(.1)
        else: raise AssertionError('linked playback did not advance')
        client.call('playback',{'action':'pause'})
        results.append('shared-clock playback advances acknowledged frames and excludes active capture')
    capture=client.call('capture')
    assert capture['captures']['lusdview']['width']==capture['captures']['usdview']['width']
    assert capture['captures']['lusdview']['height']==capture['captures']['usdview']['height']
    assert all(v['frame_serial']>capture['states'][n]['frame_serial'] for n,v in capture['captures'].items())
    assert Path(capture['report']).is_file()
    assert all(equivalent(v['rendered_time'],capture['states'][n]['timeline']['time']) for n,v in capture['captures'].items())
    results.append('equal-sized captures come from newer matching camera frames')
    result={'checks':results,'capture':capture}
    (directory/'integration-results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('\n'.join('PASS: '+r for r in results))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',required=True,type=Path)
    parser.add_argument('--prim',required=True)
    args=parser.parse_args()
    run(args.session,args.prim)
