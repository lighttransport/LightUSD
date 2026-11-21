#!/usr/bin/env python3
"""Bounded paired stage load + evaluated mesh-array readback benchmark.

Both engines use the same Python executable and original input. Ratios are
eligible only when every measured sample has the same complete logical mesh-array hash.
This measures Python API work, not rendering or a library-only memory estimate.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import sys
import time


def worker(engine,source,frame,output,max_array_elements=0,max_threads=0,input_policy='untrusted'):
    if engine=='lightusd':
        import lightusd
        started=time.perf_counter()
        stage=lightusd.load(source,load_payloads=True,max_memory=30*1024**3,
                           max_array_elements=max_array_elements,max_threads=max_threads,
                           input_policy=input_policy,preserve_native_instances=True)
        loaded=time.perf_counter()
        meshes=[p for p in stage.traverse() if p.type_name=='Mesh']
        def read(prim,name):
            try:return prim.attribute(name).resolve(time=frame)
            except KeyError:return None
        path=lambda prim:prim.path
    else:
        from pxr import Usd
        started=time.perf_counter()
        stage=Usd.Stage.Open(source,load=Usd.Stage.LoadAll)
        if not stage:raise ValueError('stage open failed')
        loaded=time.perf_counter()
        meshes=[p for p in Usd.PrimRange.Stage(stage,Usd.TraverseInstanceProxies()) if p.GetTypeName()=='Mesh']
        def read(prim,name):return prim.GetAttribute(name).Get(frame)
        path=lambda prim:str(prim.GetPath())
    digest=hashlib.sha256();components=0;mesh_hashes=[]
    def append(value,code):
        nonlocal components
        if isinstance(value,(float,int)):
            number=float(value) if code=='f' else int(value)
            if not math.isfinite(number):raise ValueError('nonfinite mesh array')
            if number==0:number=0
            digest.update(struct.pack('<'+code,number));components+=1
        else:
            for item in value:append(item,code)
    for prim in meshes:
        # Logical instance-proxy paths, not implementation prototype names.
        digest=hashlib.sha256(path(prim).encode()+b'\0')
        for name,code in [('points','f'),('faceVertexCounts','i'),('faceVertexIndices','i')]:
            digest.update(name.encode()+b'\0')
            value=read(prim,name)
            if value is None:digest.update(b'absent\0')
            else:
                before=components
                try:view=memoryview(value)
                except TypeError:view=None
                fmt=view.format.lstrip('@=<>!') if view is not None else ''
                # Both bindings expose USD float[]/int[] as contiguous 32-bit
                # buffers. Hash decoded bytes without constructing a Python
                # object for every scalar, keeping the probe bounded and cheap.
                if (view is not None and view.c_contiguous and view.itemsize==4 and
                        sys.byteorder=='little' and fmt==code):
                    digest.update(view.cast('B'));components+=view.nbytes//4
                else:append(value,code)
                digest.update(struct.pack('<Q',components-before))
            digest.update(b'end\0')
        mesh_hashes.append(digest.digest())
    digest=hashlib.sha256(b''.join(sorted(mesh_hashes)))
    ended=time.perf_counter()
    result=dict(engine=engine,meshes=len(meshes),components=components,
                work_sha256=digest.hexdigest(),load_seconds=loaded-started,
                readback_seconds=ended-loaded,operation_seconds=ended-started)
    Path(output).write_text(json.dumps(result,indent=2)+'\n')


def benchmark(args):
    from types import SimpleNamespace
    from headless import toolchain_fingerprint,file_fingerprint
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'ousddiff'))
    from supervisor import execute
    config=json.loads(args.config.read_text())
    if config.get('version')!=1 or not isinstance(config.get('cases'),list) or not 1<=len(config['cases'])<=32:raise ValueError('invalid benchmark config')
    identifiers=[case.get('id')for case in config['cases']]
    if any(not isinstance(name,str)or not name for name in identifiers)or len(set(identifiers))!=len(identifiers):raise ValueError('case IDs must be unique nonempty strings')
    warmups=config.get('warmups',1);repetitions=config.get('repetitions',5)
    if type(warmups)is not int or not 1<=warmups<=20 or type(repetitions)is not int or not 5<=repetitions<=100:
        raise ValueError('require at least one warmup and five measured samples')
    args.out.mkdir(parents=True,exist_ok=False)
    identity_args=SimpleNamespace(lusdrender=args.native_library,usdrecord=args.usdrecord,usd_python=args.python)
    def identity():
        runtime=toolchain_fingerprint(identity_args)
        library=runtime.pop('lusdrender')
        return dict(sdk=runtime,native_library=library,native_extension=file_fingerprint(args.native_extension),
                    native_python=[file_fingerprint(path) for path in sorted(args.native_extension.parent.rglob('*.py'))],
                    worker=file_fingerprint(__file__))
    pinned=identity()
    env=dict(os.environ)
    env.update(config.get('sdk_env',{}))
    env['PYTHONPATH']=os.pathsep.join([str(args.native_extension.parent.parent),env.get('PYTHONPATH','')])
    env['PYTHONDONTWRITEBYTECODE']='1'
    result=dict(version=1,contract='Original-source LoadAll composition with native in-memory instances in both engines; every default-predicate logical Mesh including instance proxies; exact logical-path and evaluated float32/int32 points/counts/indices hash with multiplicity at numeric time. PointInstancer placement expansion and world transforms are outside this array-read workload',
                scope='Same Python process executable; operation time excludes imports, process RSS includes imports and bindings. No renderer speed claim.',
                warmups=warmups,repetitions=repetitions,cache_policy='fresh-process-warm-filesystem',toolchain=pinned,cases=[])
    (args.out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
    for ci,case in enumerate(config['cases']):
        source=Path(case['path']).absolute()
        if not source.is_file():raise ValueError('missing case source')
        frame=case.get('time',0)
        if type(frame)not in (int,float)or not math.isfinite(frame):raise ValueError('invalid time')
        max_array_elements=case.get('max_array_elements',0)
        if type(max_array_elements)is not int or not 0<=max_array_elements<=2**64-1:
            raise ValueError('invalid max_array_elements')
        max_threads=case.get('max_threads',0);input_policy=case.get('input_policy','untrusted')
        if type(max_threads)is not int or not 0<=max_threads<2**31:raise ValueError('invalid max_threads')
        if input_policy not in ('untrusted','trusted'):raise ValueError('invalid input_policy')
        source_pin=file_fingerprint(source)
        entry=dict(id=case['id'],source=str(source),source_fingerprint=source_pin,time=frame,
                   native_policy=dict(max_threads=max_threads,input_policy=input_policy,max_array_elements=max_array_elements),runs=[]);result['cases'].append(entry)
        for sample in range(warmups+repetitions):
            # Alternate engine order to avoid assigning all first runs to one.
            for engine in (('lightusd','openusd')if sample%2==0 else('openusd','lightusd')):
                if identity()!=pinned:raise RuntimeError('toolchain changed during benchmark')
                directory=args.out/f'case-{ci:02}'/f'{sample:02}-{engine}';directory.mkdir(parents=True)
                output=directory/'worker.json'
                argv=[str(args.python.absolute()),str(Path(__file__).resolve()),'--worker',engine,str(source),str(frame),str(output),str(max_array_elements),str(max_threads),input_policy]
                row=execute(argv,directory/'process',1800,env=env,max_rss_mib=30720,reserve_mib=4096)
                row.update(engine=engine,warmup=sample<warmups,sample=sample)
                if row['status']=='ok':row.update(json.loads(output.read_text()))
                if identity()!=pinned:row.update(status='failed',reason='toolchain changed')
                if file_fingerprint(source)!=source_pin:row.update(status='failed',reason='source root changed')
                entry['runs'].append(row)
                (args.out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
                print(case['id'],sample,engine,row['status'],row.get('operation_seconds'),flush=True)
                if row['status']!='ok':break
            if any(r['status']!='ok' for r in entry['runs']):break
        hashes={r.get('work_sha256')for r in entry['runs']}
        entry['comparable']=len(entry['runs'])==2*(warmups+repetitions)and all(r['status']=='ok'for r in entry['runs'])and len(hashes)==1 and None not in hashes
        if entry['comparable']:
            entry['summary']={}
            for engine in ('lightusd','openusd'):
                runs=[r for r in entry['runs']if r['engine']==engine and not r['warmup']]
                entry['summary'][engine]={key:dict(median=statistics.median(r[key]for r in runs),min=min(r[key]for r in runs),max=max(r[key]for r in runs))for key in ('operation_seconds','load_seconds','readback_seconds','peak_rss_bytes')}
            entry['ratios']={key:entry['summary']['lightusd'][key]['median']/entry['summary']['openusd'][key]['median']for key in ('operation_seconds','load_seconds','readback_seconds','peak_rss_bytes')}
        (args.out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    result['passed']=all(case['comparable']for case in result['cases'])
    (args.out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    return result['passed']


if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--worker':
        worker(sys.argv[2],sys.argv[3],float(sys.argv[4]),sys.argv[5],int(sys.argv[6]) if len(sys.argv)>6 else 0,
               int(sys.argv[7]) if len(sys.argv)>7 else 0,sys.argv[8] if len(sys.argv)>8 else 'untrusted')
    else:
        parser=argparse.ArgumentParser(description=__doc__)
        for name in ('config','out','python','usdrecord','native-library','native-extension'):
            parser.add_argument('--'+name,type=Path,required=True)
        raise SystemExit(0 if benchmark(parser.parse_args())else 1)
