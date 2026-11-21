#!/usr/bin/env python3
"""Bounded analytic physics checks, independent of SDK agreement.

Each engine reads original MJCF or native-converted USD. Solver differences and
unsupported coverage remain findings; no trajectory alignment or state baking.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

KINDS={'freefall','torque','slide_spring','revolute','contact'}


def load_config(path):
    if path.stat().st_size>1024*1024:raise ValueError('config exceeds 1 MiB')
    config=json.loads(path.read_text())
    if set(config)!={'version','cases'} or config['version']!=1 or not isinstance(config['cases'],list) or not 1<=len(config['cases'])<=64:raise ValueError('invalid dynamics config')
    ids=set()
    for case in config['cases']:
        if not isinstance(case,dict) or set(case)-{'id','kind','dt','steps','sample_every','position_atol','velocity_atol','penetration_max','horizontal_drift_max','contact_gap','contact_solref'}:raise ValueError('invalid dynamics case fields')
        if not isinstance(case.get('id'),str) or not re.fullmatch('[a-z0-9][a-z0-9_-]{0,63}',case['id']) or case['id'] in ids:raise ValueError('invalid/duplicate case id')
        ids.add(case['id'])
        if case.get('kind') not in KINDS:raise ValueError('unknown dynamics kind')
        for key,lo,hi in (('dt',1e-6,.01),('position_atol',0,.01),('velocity_atol',0,.01),('penetration_max',0,.05),('horizontal_drift_max',0,.01),('contact_gap',0,.1)):
            value=case.get(key,.001)
            if type(value) not in (int,float) or not math.isfinite(value) or not lo<=value<=hi:raise ValueError('invalid '+key)
        if type(case.get('steps')) is not int or not 1<=case['steps']<=100000:raise ValueError('invalid steps')
        if type(case.get('sample_every')) is not int or not 1<=case['sample_every']<=case['steps']:raise ValueError('invalid sample_every')
        if case['steps']*case['dt']>2:raise ValueError('bounded dynamics duration exceeds two seconds')
        if 'contact_solref' in case:
            value=case['contact_solref']
            if case['kind']!='contact' or not isinstance(value,list) or len(value)!=2 or any(type(v) not in (int,float) or not math.isfinite(v) for v in value):raise ValueError('contact_solref requires a contact case and two finite numbers')
            if not 2*case['dt']<=value[0]<=.1 or not .1<=value[1]<=10:raise ValueError('contact_solref outside bounded positive timeconstant/damping-ratio contract')
    return config


def controls(case):
    return dict(force=.08 if case['kind']=='torque' else 0,
                initial_q=.2 if case['kind']=='slide_spring' else .1 if case['kind']=='revolute' else 0,
                initial_v=0)


def make_mjcf(case):
    kind=case['kind'];gravity=9.81 if kind in ('freefall','contact','revolute') else 0
    root=ET.Element('mujoco',model=case['id'])
    ET.SubElement(root,'compiler',angle='radian')
    ET.SubElement(root,'option',timestep=str(case['dt']),gravity=f'0 0 {-gravity}',integrator='Euler')
    defaults=ET.SubElement(root,'default');ET.SubElement(defaults,'joint',damping='0',armature='0',frictionloss='0')
    geom_defaults=dict(friction='0 0 0',condim='1')
    if 'contact_gap' in case:geom_defaults['gap']=str(case['contact_gap'])
    if 'contact_solref' in case:geom_defaults['solref']=' '.join(map(str,case['contact_solref']))
    ET.SubElement(defaults,'geom',**geom_defaults)
    world=ET.SubElement(root,'worldbody')
    if kind=='contact':ET.SubElement(world,'geom',name='floor',type='plane',size='5 5 .1')
    body=ET.SubElement(world,'body',name='body',pos='0 0 2' if kind in ('freefall','contact') else '0 0 0')
    if kind in ('freefall','contact'):ET.SubElement(body,'freejoint',name='root')
    else:
        options=dict(name='joint',type='slide' if kind=='slide_spring' else 'hinge',axis='1 0 0' if kind=='slide_spring' else '0 1 0' if kind=='revolute' else '0 0 1',limited='false')
        if kind=='slide_spring':options.update(stiffness='8',springref='0')
        ET.SubElement(body,'joint',**options)
    mass=2 if kind in ('freefall','contact','slide_spring') else 1
    inertia='.008 .008 .008' if kind in ('freefall','contact') else '.02 .03 .04' if kind=='torque' else '.01 .01 .005' if kind=='revolute' else '.02 .02 .02'
    ET.SubElement(body,'inertial',mass=str(mass),pos='0 0 -.5' if kind=='revolute' else '0 0 0',diaginertia=inertia)
    ET.SubElement(body,'geom',name='shape',type='sphere',size='.1',mass=str(mass))
    return ET.tostring(root,encoding='unicode')+'\n'


def assess(case,samples):
    issues=[];checked=0;peak_position_error=0.;peak_velocity_error=0.;observed_contact=False
    if not isinstance(samples,list):return dict(passed=False,issues=['trajectory is not a sample list'],checked=0)
    expected_times=list(range(0,case['steps']+1,case['sample_every']))
    if expected_times[-1]!=case['steps']:expected_times.append(case['steps'])
    if len(samples)!=len(expected_times):return {'passed':False,'issues':['missing trajectory samples'],'checked':0}
    for sample,step in zip(samples,expected_times):
        t=step*case['dt']
        if not isinstance(sample,dict) or type(sample.get('time')) not in (int,float) or not math.isfinite(sample['time']) or abs(sample['time']-t)>1e-10:issues.append('sample time differs from contract');continue
        def finite(value):
            return all(finite(v) for v in value) if isinstance(value,list) else type(value) in (int,float) and math.isfinite(value)
        if not all(isinstance(sample.get(k),list) and finite(sample[k]) for k in ('position','linear_velocity','joint_q','joint_qd')):issues.append('missing/nonfinite trajectory');continue
        kind=case['kind']
        if kind in ('freefall','contact'):
            if len(sample.get('position',[]))!=3 or len(sample.get('linear_velocity',[]))!=3:issues.append('missing body state');continue
            if kind=='contact':
                checked+=1
                observed_contact=observed_contact or sample.get('contact_count',0)>0
                if sample['position'][2]<.1-case.get('penetration_max',.02):issues.append('sphere penetrated frictionless floor beyond tolerance')
                if math.hypot(*sample['position'][:2])>case.get('horizontal_drift_max',.0001):issues.append('frictionless contact drifted horizontally')
                if sample['position'][2]+sum(v*v for v in sample['linear_velocity'])/(2*9.81)>2.02:issues.append('contact gained excessive mechanical energy')
                continue
            expected_q=2-.5*9.81*t*t;expected_v=-9.81*t;actual_q=sample['position'][2];actual_v=sample['linear_velocity'][2]
        else:
            if len(sample.get('joint_q',[]))!=1 or len(sample.get('joint_qd',[]))!=1:issues.append('scalar joint coverage missing');continue
            actual_q=sample['joint_q'][0];actual_v=sample['joint_qd'][0]
            if kind=='torque':expected_q=t*t;expected_v=2*t
            else:
                omega=2 if kind=='slide_spring' else math.sqrt(9.81*.5/.26)
                initial=.2 if kind=='slide_spring' else .1
                expected_q=initial*math.cos(omega*t);expected_v=-initial*omega*math.sin(omega*t)
        qe=abs(actual_q-expected_q);ve=abs(actual_v-expected_v);checked+=1
        peak_position_error=max(peak_position_error,qe);peak_velocity_error=max(peak_velocity_error,ve)
        if qe>case.get('position_atol',.001):issues.append(f'position contract at {t}: {qe}')
        if ve>case.get('velocity_atol',.001):issues.append(f'velocity contract at {t}: {ve}')
    if case['kind']=='contact':
        if not observed_contact:issues.append('no contact was observed; collision coverage is missing')
        final=samples[-1] if samples and isinstance(samples[-1],dict) else {}
        position=final.get('position',[]);velocity=final.get('linear_velocity',[])
        if not isinstance(position,list) or len(position)!=3 or type(position[2]) not in (int,float) or not math.isfinite(position[2]) or position[2]>.12:issues.append('contact never settled near the floor')
        if not isinstance(velocity,list) or len(velocity)!=3 or type(velocity[2]) not in (int,float) or not math.isfinite(velocity[2]) or abs(velocity[2])>.25:issues.append('contact did not settle vertically')
    return dict(passed=not issues and checked==len(expected_times),checked=checked,issues=issues[:64],omitted=max(0,len(issues)-64),
                peak_position_error=peak_position_error,peak_velocity_error=peak_velocity_error,
                contract='frictionless bounded contact, no horizontal drift/energy gain' if case['kind']=='contact' else 'analytic Newtonian motion; revolute uses declared small-angle tolerance')


def newton_worker(case,source,usd):
    import numpy as np
    import warp as wp
    wp.config.kernel_cache_dir=os.environ.get('WARP_CACHE_PATH','/tmp/lightusd-dynamics-warp')
    import newton
    from newton.usd import SchemaResolverMjc,SchemaResolverNewton
    builder=newton.ModelBuilder(up_axis=newton.Axis.Z)
    # Preserve passive MJCF extension properties even when the selected native
    # solver does not implement them. Otherwise a missing registration can be
    # mistaken for a producer/conversion loss.
    newton.solvers.SolverMuJoCo.register_custom_attributes(builder)
    if usd:builder.add_usd(str(source),collapse_fixed_joints=False,apply_up_axis_from_stage=True,schema_resolvers=[SchemaResolverMjc(),SchemaResolverNewton()])
    else:builder.add_mjcf(str(source),collapse_fixed_joints=False)
    model=builder.finalize(device='cpu');control=model.control();inputs=controls(case)
    if model.joint_coord_count==1 and model.joint_dof_count==1:
        model.joint_q.assign(np.array([inputs['initial_q']],dtype=np.float32));model.joint_qd.assign(np.array([inputs['initial_v']],dtype=np.float32))
        if control.joint_f is None:raise RuntimeError('generalized force control unavailable')
        control.joint_f.assign(np.array([inputs['force']],dtype=np.float32))
    elif any(inputs.values()):raise RuntimeError('scalar joint coverage missing')
    labels=[str(label).rstrip('/').split('/')[-1] for label in model.body_label]
    if 'body' not in labels:raise RuntimeError('required body unavailable')
    body=labels.index('body')
    state,next_state=model.state(),model.state();newton.eval_fk(model,model.joint_q,model.joint_qd,state)
    contact=case['kind']=='contact'
    solver=newton.solvers.SolverXPBD(model,iterations=16,angular_damping=0) if contact else newton.solvers.SolverFeatherstone(model,angular_damping=0)
    pipeline=newton.CollisionPipeline(model) if contact else None
    contacts=pipeline.contacts() if pipeline else None
    samples=[];interval_contacts=0
    for step in range(case['steps']+1):
        if step%case['sample_every']==0 or step==case['steps']:
            newton.eval_ik(model,state,state.joint_q,state.joint_qd)
            q=state.body_q.numpy()[body];v=state.body_qd.numpy()[body]
            samples.append(dict(time=step*case['dt'],position=q[:3].tolist(),rotation_wxyz=[float(q[6]),*q[3:6].tolist()],
                                linear_velocity=v[:3].tolist(),angular_velocity=v[3:].tolist(),joint_q=state.joint_q.numpy().tolist(),joint_qd=state.joint_qd.numpy().tolist(),
                                contact_count=interval_contacts))
            interval_contacts=0
        if step<case['steps']:
            state.clear_forces()
            if pipeline:
                pipeline.collide(state,contacts)
                interval_contacts=max(interval_contacts,int(contacts.rigid_contact_count.numpy()[0]))
            solver.step(state,next_state,control,contacts,case['dt']);state,next_state=next_state,state
    diagnostics=dict(articulation_count=model.articulation_count,
                     joint_target_ke=model.joint_target_ke.numpy().tolist(),
                     body_mass=model.body_mass.numpy().tolist())
    for name in ('shape_type','shape_scale','shape_gap'):
        value=getattr(model,name,None)
        if value is not None:diagnostics[name]=value.numpy().tolist()
    namespace=getattr(model,'mujoco',None)
    stiffness=getattr(namespace,'dof_passive_stiffness',None)
    if stiffness is not None:diagnostics['mjc_passive_stiffness']=stiffness.numpy().tolist()
    return dict(engine='newton-usd' if usd else 'newton-mjcf',warp_version=wp.__version__,device='cpu',solver=type(solver).__name__,dt=case['dt'],steps=case['steps'],diagnostics=diagnostics,samples=samples)


def run(args):
    import lightusd
    if not math.isfinite(args.timeout) or not 0<args.timeout<=1800:raise ValueError('timeout must be finite and within (0,1800] seconds')
    config=load_config(args.config);out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()):raise ValueError('output directory must be empty')
    (out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
    environment=dict(os.environ)
    for setting in args.env:
        key,separator,value=setting.partition('=')
        if not separator or not key:raise ValueError('--env requires KEY=VALUE')
        environment[key]=value
    results=dict(version=1,scope='Bounded analytic fixtures, not whole-robot solver equivalence',cases=[])
    for case in config['cases']:
        directory=out/case['id'];directory.mkdir();xml=directory/'source.xml';xml.write_text(make_mjcf(case))
        stage=lightusd.import_mjcf(xml,profile='newton');usd=directory/'converted.usdc';stage.save(str(usd));stage.close()
        request=directory/'case.json';request.write_text(json.dumps(case))
        row=dict(id=case['id'],contract=case,source_sha256=hashlib.sha256(xml.read_bytes()).hexdigest(),usd_sha256=hashlib.sha256(usd.read_bytes()).hexdigest(),engines={});results['cases'].append(row)
        for engine in args.engine:
            inputs=controls(case);target=usd if engine.endswith('usd') else xml
            if engine.startswith('mujoco'):
                command=[str(args.mujoco_worker),str(target),str(case['steps']),str(case['dt']),str(case['sample_every']),str(inputs['force']),str(inputs['initial_q']),str(inputs['initial_v'])]
                if engine.endswith('usd'):command.append(str(args.mujoco_plugin))
            else:command=[str(args.python),str(Path(__file__).resolve()),'--worker',engine,'--case',str(request),'--source',str(target)]
            (directory/(engine+'.command.json')).write_text(json.dumps(command,indent=2)+'\n')
            try:
                completed=subprocess.run(command,env=environment,capture_output=True,text=True,timeout=args.timeout)
                (directory/(engine+'.stdout.log')).write_text(completed.stdout);(directory/(engine+'.stderr.log')).write_text(completed.stderr)
                if completed.returncode:raise RuntimeError(f'worker exited {completed.returncode}; inspect retained logs')
                lines=[line for line in completed.stdout.splitlines() if line.startswith('{')]
                if not lines:raise RuntimeError('worker produced no trajectory')
                trajectory=json.loads(lines[-1]);(directory/(engine+'.json')).write_text(json.dumps(trajectory,indent=2)+'\n')
                verdict=assess(case,trajectory['samples']);row['engines'][engine]=dict(status='PASS' if verdict['passed'] else 'FAIL',**verdict)
            except Exception as error:row['engines'][engine]=dict(status='ERROR',error=str(error))
            (out/'results.json').write_text(json.dumps(results,indent=2,allow_nan=False)+'\n')
        row['status']='PASS' if row['engines'] and all(v['status']=='PASS' for v in row['engines'].values()) else 'FAIL'
        print(row['id'],row['status'],{k:v['status'] for k,v in row['engines'].items()},flush=True)
    results['passed']=bool(results['cases']) and all(row['status']=='PASS' for row in results['cases'])
    (out/'results.json').write_text(json.dumps(results,indent=2,allow_nan=False)+'\n');return results


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path);parser.add_argument('--out',type=Path)
    parser.add_argument('--engine',action='append',choices=('mujoco-mjcf','mujoco-usd','newton-mjcf','newton-usd'),default=[])
    parser.add_argument('--python',type=Path,default=Path(sys.executable));parser.add_argument('--mujoco-worker',type=Path);parser.add_argument('--mujoco-plugin',type=Path)
    parser.add_argument('--env',action='append',default=[]);parser.add_argument('--timeout',type=float,default=180)
    parser.add_argument('--worker',choices=('newton-mjcf','newton-usd'));parser.add_argument('--case',type=Path);parser.add_argument('--source',type=Path)
    args=parser.parse_args()
    if args.worker:print(json.dumps(newton_worker(json.loads(args.case.read_text()),args.source,args.worker.endswith('usd')),allow_nan=False))
    else:
        if not args.config or not args.out:parser.error('--config and --out are required')
        args.engine=args.engine or ['newton-mjcf','newton-usd']
        if any(e.startswith('mujoco') for e in args.engine) and (not args.mujoco_worker or not args.mujoco_plugin):parser.error('MuJoCo engines require --mujoco-worker and --mujoco-plugin')
        raise SystemExit(0 if run(args)['passed'] else 1)
