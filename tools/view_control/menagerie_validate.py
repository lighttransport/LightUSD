#!/usr/bin/env python3
"""Convert selected Menagerie models and validate independent after-load physics.

Requires the current LightUSD extension and MuJoCo in this interpreter, and the
OpenUSD SDK Python supplied by --usd-python. Generated assets stay under --out.
"""
import argparse
import dataclasses
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


def source_mapping_status(profile,reference):
    if reference['status']=='PASS':return 'PASS'
    issues=reference.get('mismatches',[])
    dropped={'count:actuators','count:sites','count:tendons','count:equalities','count:sensors','count:keyframes'}
    if profile=='openusd' and issues and all(
            issue.get('field') in dropped and issue.get('usd')==0 and
            type(issue.get('mujoco')) is int and issue['mujoco']>0 for issue in issues):
        return 'EXPECTED_LOSS'
    return 'FAIL'


def compiler_coverage(model,reference):
    expected_bodies=sum(bool(model.body(i).name) for i in range(1,model.nbody))
    expected_joints=sum(bool(model.joint(i).name) and int(model.jnt_type[i]) in (2,3) for i in range(model.njnt))
    expected_limits=sum(bool(model.joint(i).name) and int(model.jnt_type[i]) in (2,3) and bool(model.jnt_limited[i]) for i in range(model.njnt))
    expected={'body_position':expected_bodies,'body_rotation':expected_bodies,'joint_anchor':expected_joints,'joint_lowerLimit':expected_limits,'joint_upperLimit':expected_limits}
    actual={key:reference['checks'].get(key,0) for key in expected}
    return {'passed':actual==expected,'expected':expected,'checked':actual}


def compiler_mass_checks(stage,model):
    import numpy as np
    import mujoco,re
    errors=[];counts={};missing=[]
    mpu=stage.meters_per_unit
    authored_kpu=stage.get_metadata('kilogramsPerUnit')
    kpu=1. if authored_kpu is None else authored_kpu
    if any(type(value) not in (int,float) or not math.isfinite(value) or value<=0 for value in (mpu,kpu)):
        return {'passed':False,'complete':False,'checks':{},'mismatches':[],
                'missing':['invalid stage mass/length units'],'units':'SI'}
    def check(label,actual,expected):
        counts[label.split(':')[0]]=counts.get(label.split(':')[0],0)+1
        if not np.allclose(actual,expected,rtol=2e-5,atol=2e-7):errors.append({'field':label,'usd_SI':np.asarray(actual).tolist(),'mujoco_SI':np.asarray(expected).tolist()})
    def rotation(quat):
        matrix=np.empty(9);mujoco.mju_quat2Mat(matrix,np.asarray(quat,dtype=float));return matrix.reshape(3,3)
    for i in range(1,model.nbody):
        body=model.body(i)
        if not body.name:missing.append('unnamed compiler body '+str(i));continue
        name=re.sub(r'[^a-zA-Z0-9_]','_',body.name)
        if name and name[0].isdigit():name='_'+name
        try:prim=stage.prim_at('/World/Links/'+name)
        except KeyError:missing.append('body '+body.name);continue
        mass=prim.get('physics:mass');com=prim.get('physics:centerOfMass');inertia=prim.get('physics:diagonalInertia');axes=prim.get('physics:principalAxes')
        if mass is None:missing.append('mass '+body.name)
        else:check('mass:'+body.name,mass*kpu,model.body_mass[i])
        if com is None:missing.append('centerOfMass '+body.name)
        else:check('centerOfMass:'+body.name,np.asarray(com)*mpu,model.body_ipos[i])
        if inertia is None or axes is None:missing.append('inertia '+body.name)
        else:
            r=rotation(axes);expected_r=rotation(model.body_iquat[i]);actual=r@np.diag(np.asarray(inertia)*kpu*mpu**2)@r.T
            expected=expected_r@np.diag(model.body_inertia[i])@expected_r.T
            check('inertia_tensor:'+body.name,actual,expected)
    return {'passed':not errors and not missing and bool(counts),'complete':not missing,'checks':counts,'mismatches':errors,'missing':missing,
            'units':'SI mass, local center of mass and body-space inertia tensor; eigenvector signs/permutations compared through tensors',
            'rtol':2e-5,'atol':2e-7}


def run(args):
    import lightusd,mujoco
    from physics_compare import compare
    root=args.root.resolve(strict=True);out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()):raise ValueError('output directory must be empty')
    helper=Path(__file__).resolve().parents[2]/'tests/core/run-menagerie-import.py'
    spec=importlib.util.spec_from_file_location('menagerie_reference',helper);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    sdk_env=dict(os.environ);prefix=args.usd_prefix.resolve();packages=list((prefix/'lib').glob('python*/site-packages'))
    if len(packages)!=1:raise ValueError('expected one SDK Python package directory')
    sdk_env['PYTHONPATH']=str(packages[0]);sdk_env['LD_LIBRARY_PATH']=os.pathsep.join([str(prefix/'lib'),str(prefix.parent/'deps/inst/lib')]);sdk_env['PYTHONDONTWRITEBYTECODE']='1'
    results={'models':[],'mujoco_version':mujoco.__version__,'lightusd_extension':lightusd._core.__file__,'source_revision':subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()}
    def command(argv,log,environment=None):
        log.with_suffix('.command.json').write_text(json.dumps(argv,indent=2)+'\n')
        with log.open('wb') as stream:
            completed=subprocess.run(argv,env=environment,stdout=stream,stderr=subprocess.STDOUT,timeout=args.timeout)
        if completed.returncode:raise RuntimeError('worker failed; inspect '+str(log))
    for rel in args.model:
        source=(root/rel).resolve(strict=True)
        if not source.is_relative_to(root):raise ValueError('model escapes root')
        name=source.parent.name
        for profile in args.profile:
            directory=out/name/profile;directory.mkdir(parents=True);row={'model':rel,'profile':profile,'formats':{}};results['models'].append(row);start=time.monotonic()
            try:
                model=mujoco.MjModel.from_xml_path(str(source));data=mujoco.MjData(model);mujoco.mj_forward(model,data)
                stage=lightusd.import_mjcf(source,asset_dirs=[root],max_memory=4*1024**3,tessellate_collision_shapes=True,profile=profile)
                row['compiler_reference']=module.reference_checks(stage,model,data)
                row['compiler_coverage']=compiler_coverage(model,row['compiler_reference'])
                row['compiler_mass_inertia']=compiler_mass_checks(stage,model)
                row['native_semantic_validation']=dataclasses.asdict(lightusd.physics.validate(stage,samples=True))
                row['counts']=module.metrics(stage);row['warnings']=stage.warnings()
                for ext in ('usda','usdc','usdz'):
                    file=directory/('scene.'+ext);stage.save(str(file));loaded=lightusd.load(file,max_memory=4*1024**3)
                    item={'file':str(file),'counts':module.metrics(loaded),'semantic_validation':dataclasses.asdict(lightusd.physics.validate(loaded,samples=True))};row['formats'][ext]=item;loaded.close()
                    snapshots={}
                    for engine,python,environment in [('lightusd',sys.executable,dict(os.environ)),('openusd',str(args.usd_python.resolve()),sdk_env)]:
                        snapshot=directory/(ext+'-'+engine+'.json')
                        command([python,str(Path(__file__).with_name('physics_snapshot.py')),'--engine',engine,'--scene',str(file),'--out',str(snapshot),'--times','0','0.5','1'],directory/(ext+'-'+engine+'.log'),environment)
                        snapshots[engine]=json.loads(snapshot.read_text())
                    diff=compare(snapshots['openusd'],snapshots['lightusd']);item['after_load_physics']=diff
                    (directory/(ext+'-physics-diff.json')).write_text(json.dumps(diff,indent=2)+'\n')
                stage.close()
                row['roundtrip_counts_passed']=all(x['counts']==row['counts'] for x in row['formats'].values())
                row['after_load_passed']=all(x['after_load_physics']['passed'] for x in row['formats'].values())
                row['semantic_passed']=row['native_semantic_validation']['valid'] and all(x['semantic_validation']['valid'] for x in row['formats'].values())
                row['source_mapping_status']=source_mapping_status(profile,row['compiler_reference'])
                structural=row['roundtrip_counts_passed'] and row['after_load_passed'] and row['semantic_passed'] and row['compiler_mass_inertia']['passed'] and row['compiler_coverage']['passed']
                row['status']='PASS' if structural and row['source_mapping_status']=='PASS' else 'INCOMPLETE' if structural and row['source_mapping_status']=='EXPECTED_LOSS' else 'FAIL'
                row['source_mapping_attribution']='expected_representation' if row['source_mapping_status']=='EXPECTED_LOSS' else 'unresolved'
            except Exception:
                import traceback;row.update(status='ERROR',error=traceback.format_exc())
            row['seconds']=time.monotonic()-start
            (out/'conversion-physics.json').write_text(json.dumps(results,indent=2)+'\n');print(name,profile,row['status'],flush=True)
    return results


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--model',action='append',required=True);p.add_argument('--profile',action='append',choices=('openusd','newton'),default=[]);p.add_argument('--usd-python',type=Path,default=Path('../OpenUSD/.venv/bin/python3'));p.add_argument('--usd-prefix',type=Path,default=Path('../OpenUSD/dist-v26.11'));p.add_argument('--timeout',type=float,default=180);a=p.parse_args();a.profile=a.profile or ['newton'];result=run(a);raise SystemExit(0 if all(r['status']=='PASS' for r in result['models']) else 1)
