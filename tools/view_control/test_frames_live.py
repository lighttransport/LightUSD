#!/usr/bin/env python3
"""Opt-in hardware regression; launch view-control-skeleton.usda before running."""
import argparse,json,time,secrets
from pathlib import Path
from control import ViewerClient


def run(root):
    from PIL import Image,ImageChops
    info=json.loads((root/'session.json').read_text());token=Path(info['token_file']).read_text().strip();c=ViewerClient(info['endpoint'],token,180)
    native={n:ViewerClient(url,token,60) for n,url in info['viewers'].items()}
    def equal(a,b):
     with Image.open(a) as x,Image.open(b) as y:return ImageChops.difference(x.convert('RGB'),y.convert('RGB')).getbbox() is None
    def settle(bones):
     end=time.monotonic()+12
     while time.monotonic()<end:
      s=c.call('status');assert not s['error'],s['error']
      if all(v['display'].get('skeleton')==bones for v in s['viewers'].values()):return s
      time.sleep(.05)
     raise AssertionError('bone display did not synchronize')
    native['usdview'].call('render_settings',{'skeleton':False,'guide':False,'background':'black','mode':'shaded'})
    c.call('sync',{'source':'usdview','enabled':True})
    settle(False)
    off=c.call('capture')
    native['usdview'].call('render_settings',{'skeleton':True})
    s=settle(True);assert all(not v['display']['guide'] for v in s['viewers'].values())
    on=c.call('capture');stats=native['lusdview'].call('get_render_stats');assert stats['core_skeleton_overlay_vertices']>0,stats
    for name in native:
     assert not equal(off['captures'][name]['path'],on['captures'][name]['path']),name+' bone display did not change pixels'
    print('PASS independent bones affect both native images with guide purpose disabled',flush=True)
    native['lusdview'].call('render_settings',{'skeleton':False});settle(False)
    off2=c.call('capture')
    for name in native:assert equal(off['captures'][name]['path'],off2['captures'][name]['path']),name+' geometry changed after bone toggle'
    print('PASS reverse bone toggle restores skin and descendant geometry pixels',flush=True)
    c.call('sync',{'enabled':False})
    receipts={}
    for name,client in native.items():
     before=client.call('view_state');identifier=secrets.token_hex(16)
     client.call('frame_barrier',{'token':identifier})
     deadline=time.monotonic()+10
     while time.monotonic()<deadline:
      state=client.call('view_state');receipt=state.get('barrier')
      if receipt and receipt['token']==identifier:
       assert receipt['ready'],receipt
       break
      time.sleep(.02)
     else:raise AssertionError('barrier timeout')
     a=root/(name+'-held-a.png');b=root/(name+'-held-b.png');ordinary=root/(name+'-ordinary.png')
     client.call('screenshot',{'path':str(a),'barrier':identifier})
     client.call('screenshot',{'path':str(ordinary)})
     assert equal(a,ordinary),name+' retained readback differs from native framebuffer'
     client.call('render_settings',{'background':'white','mode':'wireframe','skeleton':True,'guide':True})
     client.call('select',{'op':'set','path':'/Rig/Skeleton/Skin'})
     deadline=time.monotonic()+10
     while time.monotonic()<deadline:
      later=client.call('view_state')
      if later['frame_serial']>receipt['frame_serial'] and later['display']['background']=='white':break
      time.sleep(.02)
     again=client.call('frame_barrier',{'token':identifier});assert again==receipt
     cached=client.call('screenshot',{'path':str(b),'barrier':identifier});assert cached['barrier']==receipt
     assert equal(a,b),name+' retained pixels changed after native state changed'
     client.call('screenshot',{'path':str(ordinary)})
     assert not equal(a,ordinary),name+' later native image did not change'
     client.call('render_settings',{'background':'black','mode':'shaded','skeleton':False,'guide':False})
     client.call('select',{'op':'clear'})
     client.call('viewport',{'op':'set_camera','camera':before['camera']})
     client.call('frame_barrier',{'token':identifier,'cancel':True})
     assert client.call('view_state')['barrier'] is None
     try:
      client.call('screenshot',{'path':str(b),'barrier':identifier});raise AssertionError('cancelled barrier readable')
     except RuntimeError:pass
     receipts[name]=receipt
     print('PASS retained exact-frame pixels, full state, idempotency and release',name,flush=True)
    c.call('sync',{'source':'usdview','enabled':True})
    (root/'bones-barriers-results.json').write_text(json.dumps({'on':on,'off':off2,'receipts':receipts},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    run(parser.parse_args().session)
