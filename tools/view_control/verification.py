"""Evidence-based assertion aggregation; engine agreement is not an oracle."""
import math

OWNERS={'lightusd','openusd','external_consumer','harness','dataset','expected_representation','unresolved'}
STATES={'PASS','FAIL','ERROR','UNSUPPORTED','INCOMPLETE'}


def adjudicate(assertions,required):
    """Retain every finding and require complete evidence for each required gate."""
    if not isinstance(required,list) or not required or len(set(required))!=len(required):
        raise ValueError('required gates must be a nonempty unique list')
    findings=[]
    for item in assertions:
        if not isinstance(item,dict) or item.get('status') not in STATES or item.get('owner','unresolved') not in OWNERS:
            raise ValueError('invalid assertion')
        row=dict(item)
        if not isinstance(row.get('gate'),str):raise ValueError('gate is required')
        if row['status']=='PASS' and (not row.get('contract') or not row.get('evidence') or row.get('control',False)):
            row.update(status='INCOMPLETE',reason='Original-source pass requires a contract and evidence')
        findings.append(row)
    coverage={gate:[r for r in findings if r['gate']==gate] for gate in required}
    missing=[gate for gate,rows in coverage.items() if not rows]
    passed=not missing and all(rows and all(r['status']=='PASS' for r in rows) for rows in coverage.values())
    return {'passed':passed,'missing_gates':missing,'assertions':findings,
            'complete':not missing and all(r['status'] not in ('UNSUPPORTED','INCOMPLETE','ERROR') for rows in coverage.values() for r in rows)}


def performance_eligibility(reference,candidate):
    """Reject ratios when measured work or successful coverage differs."""
    fields=('work_fingerprint','capture_semantics','purposes','size','time','quality')
    reasons=[field for field in fields if field not in reference or field not in candidate or reference[field]!=candidate[field]]
    for label,row in [('reference',reference),('candidate',candidate)]:
        if row.get('status')!='PASS' or not row.get('correctness_passed'):reasons.append(label+':correctness')
        if row.get('contended',False):reasons.append(label+':contention')
        for metric in ('wall_s','peak_rss_bytes'):
            value=row.get(metric)
            if type(value) not in (float,int) or not math.isfinite(value) or value<=0:reasons.append(label+':'+metric)
    if reasons:return {'comparable':False,'reasons':reasons}
    return {'comparable':True,'reasons':[], 'wall_ratio':candidate['wall_s']/reference['wall_s'],
            'rss_ratio':candidate['peak_rss_bytes']/reference['peak_rss_bytes']}


def verify_catalogue(catalogue,evidence,evidence_root):
    """Execute every declared acceptance gate against hashed local evidence.

    Dataset rendering/conversion is performed by validate/dynamics workers. This
    aggregation never treats an absent run, a control, or an unchecked filename
    as a successful catalogue case.
    """
    import hashlib
    from pathlib import Path
    if not isinstance(catalogue,dict) or catalogue.get('schema')!=1 or not isinstance(catalogue.get('cases'),list) or not catalogue['cases']:
        raise ValueError('invalid catalogue')
    if not isinstance(evidence,dict) or set(evidence)!={'version','assertions'} or evidence['version']!=1 or not isinstance(evidence['assertions'],list):
        raise ValueError('invalid evidence bundle')
    root=Path(evidence_root).resolve(strict=True)
    cases={}
    for case in catalogue['cases']:
        if not isinstance(case,dict) or not isinstance(case.get('id'),str) or case['id'] in cases:raise ValueError('invalid/duplicate catalogue case')
        cases[case['id']]=case
    assertions={key:[] for key in cases}
    for item in evidence['assertions']:
        if not isinstance(item,dict) or item.get('case') not in cases:raise ValueError('unknown evidence case')
        row=dict(item);files=row.get('evidence',[])
        if not isinstance(files,list):raise ValueError('evidence must be a list of hashed artifacts')
        problems=[]
        for artifact in files:
            if not isinstance(artifact,dict) or set(artifact)!={'path','sha256'} or not isinstance(artifact['path'],str) or not isinstance(artifact['sha256'],str):raise ValueError('invalid evidence artifact')
            try:
                path=(root/artifact['path']).resolve(strict=True)
                if not path.is_relative_to(root):raise ValueError('artifact escapes evidence root')
                if not path.is_file() or path.stat().st_size==0:raise ValueError('artifact is empty or not a file')
                with path.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
                if digest!=artifact['sha256']:problems.append('artifact hash differs: '+artifact['path'])
            except (OSError,ValueError) as error:problems.append(str(error))
        if problems:
            row.update(status='INCOMPLETE',owner='harness',reason='; '.join(problems))
        assertions[row['case']].append(row)
    results=[]
    for name,case in cases.items():
        results.append(dict(id=name,job=case.get('job'),**adjudicate(assertions[name],case.get('required'))))
    return dict(version=1,profile=catalogue.get('profile'),passed=all(row['passed']for row in results),cases=results)
