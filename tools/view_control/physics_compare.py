#!/usr/bin/env python3
"""Compare independent after-load physics snapshots without hiding missing fields."""
import argparse
import json
import math
from pathlib import Path


def compare(reference,candidate,rtol=2e-5,atol=2e-6,max_issues=1000):
    if type(max_issues) is not int or max_issues<1:raise ValueError('max_issues must be positive')
    for value in (rtol,atol):
        if type(value) not in (int,float) or not math.isfinite(value) or value<0:raise ValueError('tolerances must be finite and nonnegative')
    issues=[];checked=0;omitted=0
    def issue(path,expected,actual):
        nonlocal omitted
        if len(issues)<max_issues:issues.append({'path':path,'reference':expected,'candidate':actual})
        else:omitted+=1
    def walk(a,b,path):
        nonlocal checked
        if isinstance(a,dict) and isinstance(b,dict):
            for key in sorted(set(a)|set(b)):
                if key not in a:issue(path+'/'+key,'<missing>',b[key])
                elif key not in b:issue(path+'/'+key,a[key],'<missing>')
                else:walk(a[key],b[key],path+'/'+key)
        elif isinstance(a,list) and isinstance(b,list):
            if len(a)!=len(b):issue(path+'/length',len(a),len(b))
            for i,(x,y) in enumerate(zip(a,b)):walk(x,y,path+'/'+str(i))
        else:
            checked+=1
            equal=math.isclose(a,b,rel_tol=rtol,abs_tol=atol) if type(a) in (int,float) and type(b) in (int,float) else type(a)==type(b) and a==b
            if not equal:issue(path,a,b)
    for key in ('times','metadata','prims'):walk(reference[key],candidate[key],key)
    has_physics=bool(reference['prims'])
    if not has_physics:issue('prims','nonempty physics coverage','empty reference')
    return {'passed':not issues and omitted==0 and has_physics,'complete':omitted==0 and has_physics,'checked_scalar_values':checked,'issues':issues,'omitted':omitted,'rtol':rtol,'atol':atol,
            'scope':reference['scope']}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('reference',type=Path);p.add_argument('candidate',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();r=compare(json.loads(a.reference.read_text()),json.loads(a.candidate.read_text()));a.out.write_text(json.dumps(r,indent=2)+'\n');raise SystemExit(0 if r['passed'] else 1)
