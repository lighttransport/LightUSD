#!/usr/bin/env python3
"""Own two native viewers and test shared overlay editing and reload recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from session import Session


def run(args):
    original=hashlib.sha256(args.scene.read_bytes()).hexdigest()
    session=Session(args)
    checks=[]
    try:
        session.start()
        def check(value):
            usd=session.clients['usdview'].call('inspect_prim',{'path':'/hello'})
            row=next(p for p in usd['properties'] if p['name']=='size')
            assert abs(float(row['value_preview'])-value)<1e-5
            lusd=session.clients['lusdview'].call('attr_get',{'path':'/hello','attr_name':'size'})
            assert abs(float(lusd['value'])-value)<1e-5,lusd
            bounds=session.clients['lusdview'].call('get_scene_info')
            assert abs(bounds['aabb_max'][0]-bounds['aabb_min'][0]-value)<1e-4
            assert hashlib.sha256(args.scene.read_bytes()).hexdigest()==original
        session.debug_edit('edit',[{'op':'set_attribute','path':'/hello','property':'size','value':2.4}])
        check(2.4);checks.append('typed edit visible in both native stages and rendered bounds')
        # Fail the first peer reload after publication; all recovery calls use the real endpoint.
        client=session.clients['lusdview'];real_call=client.call;failures=[]
        def fail_once(tool,arguments=None):
            if tool=='reload_layer' and not failures:
                failures.append(tool);raise RuntimeError('injected peer reload failure')
            return real_call(tool,arguments)
        client.call=fail_once
        try:
            try:
                session.debug_edit('edit',[{'op':'set_attribute','path':'/hello','property':'size','value':3.6}])
                raise AssertionError('injected failure did not fail the transaction')
            except RuntimeError as error:
                assert 'injected peer reload failure' in str(error),error
        finally:
            client.call=real_call
        assert failures and session.error is None
        check(2.4);checks.append('failed peer reload restores both viewers without accepting history')
        session.debug_edit('undo');check(1.2)
        session.debug_edit('redo');check(2.4)
        checks.append('undo and redo restore both native stages')
        try:
            session.debug_edit('edit',[{'op':'set_attribute','path':'/hello','property':'size','value':9},
                                       {'op':'set_attribute','path':'/absent','property':'size','value':9}])
            raise AssertionError('invalid batch accepted')
        except RuntimeError:
            pass
        check(2.4);checks.append('invalid batch leaves both stages and undo history intact')
        session.debug_edit('clear');check(1.2)
        session.debug_edit('undo');check(2.4)
        checks.append('clear opinions and undo clear restore source and edited values')
        session.debug_edit('edit',[
            {'op':'set_attribute','path':'/hello','property':'debug:array','type':'float[]','value':[1.0]*10000},
            {'op':'set_attribute','path':'/hello','property':'debug:text','type':'string','value':'a'+'é'*3000},
            {'op':'set_attribute','path':'/hello','property':'debug:vector','type':'float3','value':[1,2,3]},
            {'op':'set_attribute','path':'/hello','property':'debug:source','type':'double','value':2.4},
            {'op':'set_attribute','path':'/hello','property':'debug:target','type':'double','value':2.4},
            {'op':'connections','path':'/hello','property':'debug:target','targets':['/hello.debug:source']},
            {'op':'relationship','path':'/hello','property':'debug:link','targets':['/hello']},
            {'op':'property_metadata','path':'/hello','property':'size','key':'documentation','value':'Debug size'}])
        for name,client in session.clients.items():
            inspected=client.call('inspect_prim',{'path':'/hello','limit':64})
            properties={p['name']:p for p in inspected['properties']}
            assert properties['debug:link']['targets']==['/hello']
            assert properties['debug:target']['connections']==['/hello.debug:source']
            array=properties['debug:array']
            assert (array['value']['count'] if name=='lusdview' else array['value_preview']['count'])==10000
            if name=='lusdview':
                assert properties['debug:vector']['value']==[1,2,3],properties['debug:vector']
                assert len(properties['debug:text']['value'].encode('utf-8'))<=4096
                assert properties['debug:text']['value'].endswith('é')
            else:
                assert properties['debug:target']['provenance']
            assert len(json.dumps(inspected,ensure_ascii=False))<24000
            page=client.call('inspect_prim',{'path':'/hello','limit':2})
            assert len(page['properties'])==2 and page['next_offset']==2
        checks.append('both native inspectors paginate relationships/connections and bound large arrays')
        exported=session.debug_edit('export')
        assert Path(exported['exported']).is_file() and Path(exported['exported_wrapper']).is_file()
        checks.append('sparse layer and source-referencing wrapper export')
        capture=session.capture()
        checks.append('paired completed-frame capture after overlay edits')
        result={'checks':checks,'export':exported,'capture':capture,'source_sha256':original}
        (session.out/'overlay-results.json').write_text(json.dumps(result,indent=2)+'\n')
        print('\n'.join('PASS: '+check for check in checks),flush=True)
    finally:
        session.stop()
    assert hashlib.sha256(args.scene.read_bytes()).hexdigest()==original


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene',type=Path,default=Path(__file__).resolve().parents[2]/'tests/usda/cube-with-xform.usda')
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--lusdview',type=Path,default=Path('build_ninja/lusdview'))
    parser.add_argument('--usdview',type=Path,default=Path('../OpenUSD/dist/bin/usdview'))
    parser.add_argument('--usdview-python',type=Path)
    parser.add_argument('--bridge-source',type=Path)
    parser.add_argument('--usdview-env',action='append',default=[])
    parser.add_argument('--backend',choices=('vk','gl'),default='vk')
    parser.add_argument('--size',default='320x240')
    parser.add_argument('--load-timeout',type=float,default=120)
    parser.add_argument('--headless-lusdview',action='store_true')
    run(parser.parse_args())
