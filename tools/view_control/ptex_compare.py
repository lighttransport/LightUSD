#!/usr/bin/env python3
"""Run sampled raw PTex checks against an explicitly built OpenPtex reference."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess


def run(args):
    if not math.isfinite(args.timeout) or not 0<args.timeout<=1800:raise ValueError('invalid timeout')
    if args.config.stat().st_size>1024*1024:raise ValueError('config exceeds 1 MiB')
    config=json.loads(args.config.read_text())
    if not isinstance(config,dict) or set(config)!={'version','reference','files'} or config['version']!=1 or not isinstance(config['files'],list) or not 1<=len(config['files'])<=64:raise ValueError('invalid PTex job')
    root=args.assets_root.resolve(strict=True);sources=[]
    for name in config['files']:
        if not isinstance(name,str):raise ValueError('invalid asset name')
        path=(root/name).resolve(strict=True)
        if not path.is_relative_to(root) or path.suffix!='.ptx' or not path.is_file():raise ValueError('PTex file escapes root or is not a file')
        sources.append(path)
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()):raise ValueError('output directory must be empty')
    def digest(path):
        with path.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
    worker=args.worker.resolve(strict=True);worker_hash=digest(worker)
    result=dict(version=1,config=config,reference_build=args.reference_build,worker_sha256=worker_hash,
                scope='16 uniform original faces, up to three stored mip levels, 32 MiB per decoded face. Raw bytes only; no filtered sampling or rendering.',files=[])
    for i,path in enumerate(sources):
        row=dict(file=config['files'][i],sha256=digest(path));result['files'].append(row)
        command=[str(worker),str(path)]
        (out/f'{i:03}.command.json').write_text(json.dumps(command,indent=2)+'\n')
        try:
            if digest(worker)!=worker_hash:raise RuntimeError('worker changed during job')
            r=subprocess.run(command,capture_output=True,text=True,timeout=args.timeout)
            (out/f'{i:03}.stdout.log').write_text(r.stdout);(out/f'{i:03}.stderr.log').write_text(r.stderr)
            data=json.loads(r.stdout)
            if not isinstance(data,dict) or not isinstance(data.get('samples'),list) or not data['samples']:raise ValueError('worker supplied no sample coverage')
            row['result']=data
            row['status']='PASS' if r.returncode==0 and data.get('passed')is True and data.get('checked',0)>0 and all(s.get('status')=='PASS'for s in data['samples']) else 'FAIL' if any(s.get('status')=='FAIL'for s in data['samples']) else 'INCOMPLETE'
            if digest(worker)!=worker_hash:raise RuntimeError('worker changed during capture')
        except (ValueError,RuntimeError,OSError,subprocess.TimeoutExpired)as error:row.update(status='ERROR',error=str(error))
        (out/'results.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        print(row['file'],row['status'],flush=True)
    result['passed']=all(row['status']=='PASS'for row in result['files'])
    (out/'results.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config',type=Path,required=True);p.add_argument('--assets-root',type=Path,required=True)
    p.add_argument('--worker',type=Path,required=True);p.add_argument('--reference-build',required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--timeout',type=float,default=180)
    raise SystemExit(0 if run(p.parse_args())['passed'] else 1)
