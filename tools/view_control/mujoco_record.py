#!/usr/bin/env python3
"""Render original MJCF with the paired USD camera as an independent conversion oracle."""
import argparse
import json
import math
import os
from pathlib import Path
import time


def run(args):
    # Select EGL before importing MuJoCo/PyOpenGL. No X11/Qt dependency.
    os.environ['MUJOCO_GL']='egl'
    import mujoco
    import numpy as np
    from PIL import Image
    from headless import normalize
    payload=json.loads(args.state.read_text());state=normalize(payload.get('state',payload));camera=state['camera']
    if camera is None:raise ValueError('state must contain a resolved camera')
    if camera['shift']!=[0,0]:raise ValueError('source oracle currently requires zero lens shift')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True,mode=0o700)
    if any(out.iterdir()):raise ValueError('output directory must be empty')
    model=mujoco.MjModel.from_xml_path(str(args.scene.resolve()));data=mujoco.MjData(model);mujoco.mj_forward(model,data)
    if abs(state.get('time') or 0)>1e-12:raise ValueError('source oracle is rest-state only; time must be zero')
    near,far=camera['near'],camera['far'];width,height=state['size'];transform=camera['transform']
    model.vis.map.znear=near/model.stat.extent;model.vis.map.zfar=far/model.stat.extent
    model.vis.global_.offwidth=width;model.vis.global_.offheight=height
    started=time.perf_counter()
    with mujoco.Renderer(model,height=height,width=width) as renderer:
        options=mujoco.MjvOption();options.geomgroup[:]=[1,1,1,0,0,0]
        renderer.update_scene(data,scene_option=options)
        half=camera['ortho_height']/2 if camera['projection']=='orthographic' else near*math.tan(math.radians(camera['fov_y'])/2)
        for gl_camera in renderer.scene.camera:
            gl_camera.pos[:]=transform[12:15];gl_camera.forward[:]=[-v for v in transform[8:11]];gl_camera.up[:]=transform[4:7]
            gl_camera.orthographic=int(camera['projection']=='orthographic');gl_camera.frustum_near=near;gl_camera.frustum_far=far
            gl_camera.frustum_top=half;gl_camera.frustum_bottom=-half;gl_camera.frustum_center=0
        Image.fromarray(renderer.render()).save(out/'color.png')
        from OpenGL.GL import glGetString,GL_VENDOR,GL_RENDERER,GL_VERSION
        graphics={key:glGetString(token).decode() for key,token in [('vendor',GL_VENDOR),('renderer',GL_RENDERER),('version',GL_VERSION)]}
        if args.require_amd and not ('amd' in graphics['renderer'].lower() or 'radeon' in graphics['renderer'].lower()):raise RuntimeError('AMD hardware required: '+str(graphics))
        renderer.enable_depth_rendering();axial=renderer.render().astype(np.float64)
    valid=(axial>0)&(axial<far*(1-1e-5));distance=axial.copy()
    if camera['projection']=='perspective':
        y,x=np.mgrid[0:height,0:width];tan=math.tan(math.radians(camera['fov_y'])/2)
        px=(2*(x+.5)/width-1)*tan*camera['aspect'];py=(1-2*(y+.5)/height)*tan
        distance*=np.sqrt(1+px*px+py*py)
    distance[~valid]=0
    rgb=np.repeat(distance[:,:,None],3,axis=2).astype('<f4')
    with (out/'depth.pfm').open('wb') as stream:stream.write(f'PF\n{width} {height}\n-1.0\n'.encode());stream.write(rgb[::-1].tobytes())
    result={'source':str(args.scene.resolve()),'state':state,'graphics':graphics,'mujoco_version':mujoco.__version__,
            'capture_seconds':time.perf_counter()-started,'color':str(out/'color.png'),'depth':str(out/'depth.pfm'),
            'camera_coordinate_assumption':'USD camera is Z-up in meters, as produced by the tested default conversions.',
            'scope':'Original MJCF rest state from MuJoCo compiler, explicit GL camera matching USD extrinsics/frustum. Geometry groups 0..2; no simulation trajectory. Depth converted from axial distance to primary-ray distance; far-plane background is zero. Beauty lighting/materials differ.'}
    (out/'capture.json').write_text(json.dumps(result,indent=2)+'\n');return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('scene',type=Path);p.add_argument('--state',type=Path,required=True,help='Resolved Z-up meter USD camera state; rest time zero');p.add_argument('--out',type=Path,required=True);p.add_argument('--require-amd',action='store_true');a=p.parse_args();print(json.dumps(run(a),indent=2))
