#!/usr/bin/env python3
"""Opt-in native comparison UI, command queue and GL/Vulkan switching regression."""
import argparse
import json
from pathlib import Path
import time
from control import ViewerClient


def run(root, prim):
    info=json.loads((root/'session.json').read_text())
    token=Path(info['token_file']).read_text().strip()
    controller=ViewerClient(info['endpoint'],token,180)
    native=ViewerClient(info['viewers']['lusdview'],token,60)
    controller.call('playback',{'action':'pause'})
    initial=controller.call('status')
    size=initial['viewers']['usdview']['size']
    for backend,key in [('vk','V'),('gl','G'),('vk','V')]:
        controller.call('sync',{'enabled':False})
        native.call('input',{'key':key})
        controller.call('sync',{'enabled':True,'source':'usdview'})
        result=controller.call('capture')
        assert [result['captures']['lusdview'][k] for k in ('width','height')]==size
        native.call('compare_panel',{'op':'open','window_size':[1440,1000]})
        time.sleep(2)
        for mode in range(5):
            native.call('compare_panel',{'op':'open','mode':mode})
            time.sleep(.15)
            native.call('compare_panel',{'op':'window_capture'})
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                receipt=native.call('compare_panel',{'op':'status'})['window_capture']
                if not receipt.get('pending'): break
                time.sleep(.03)
            assert receipt.get('written'),receipt
            assert receipt['width']>=640 and receipt['height']>=480
            (root/('native-'+backend+'-mode-'+str(mode)+'.png')).write_bytes(Path(receipt['path']).read_bytes())
        print('PASS all native preview modes and paired capture on '+backend,flush=True)
    for request in [{'tool':'inspect_pair','arguments':{'path':prim,'limit':8}},
                    {'tool':'capture','arguments':{}}]:
        identifier=str(time.monotonic_ns())+'-1'
        base=root/'control-requests'/identifier
        pending=base.with_suffix('.pending.json')
        pending.write_text(json.dumps(request))
        pending.replace(base.with_suffix('.request.json'))
        response=base.with_suffix('.response.json')
        deadline=time.monotonic()+60
        while not response.exists() and time.monotonic()<deadline: time.sleep(.1)
        result=json.loads(response.read_text())
        assert result['ok'],result
        assert not base.with_suffix('.processing.json').exists()
        print('PASS queued '+request['tool'],flush=True)
    (root/'native-panel-results.json').write_text(json.dumps(controller.call('status'),indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--prim',required=True)
    args=parser.parse_args()
    run(args.session,args.prim)
