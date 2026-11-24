#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Strict real-asset native/Python import sweep. Outputs stay under --out.

Run with the isolated installed wheel and mujoco==3.16.0. Every tracked XML
and OBJ is accounted for; XML fragments are dependencies, not standalone robots.
"""
import argparse
import concurrent.futures
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
import xml.etree.ElementTree as ET


def command(args, **kwargs):
    timeout = kwargs.pop('timeout', None)
    isolated = kwargs.get('start_new_session', False)
    with subprocess.Popen(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs) as process:
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if isolated: os.killpg(process.pid, signal.SIGKILL)
            else: process.kill()
            process.communicate()
            raise
        return subprocess.CompletedProcess(args, process.returncode, output)


def metrics(stage):
    from lightusd import physics
    info = physics.inspect(stage)
    result = {k: len(getattr(info, k)) for k in info.__dataclass_fields__}
    result.update(meshes=0, points=0, faces=0)
    for p in stage:
        if p.type_name == 'Mesh':
            result['meshes'] += 1
            result['points'] += len(p['points'])
            result['faces'] += len(p['faceVertexCounts'])
            counts = memoryview(p['faceVertexCounts']).tolist()
            indices = memoryview(p['faceVertexIndices']).tolist()
            assert sum(counts) == len(indices), p.path
            assert min(indices, default=0) >= 0 and max(indices, default=-1) < len(p['points']), p.path
    return result


def reference_checks(stage, model, data):
    """Compare authored rest frames, DOF limits and engine record counts.

    MuJoCo compiler-derived mass/inertia (including mesh convex hull inference)
    is intentionally measured only when USD authored the corresponding value.
    """
    import numpy as np
    from lightusd import physics
    info = physics.inspect(stage)
    errors, counts = [], {}
    def check(label, actual, expected, atol=2e-5):
        counts[label.split(':')[0]] = counts.get(label.split(':')[0], 0) + 1
        if actual is None or not np.allclose(actual, expected, rtol=2e-5, atol=atol):
            errors.append({'field': label, 'usd': np.asarray(actual).tolist(),
                           'mujoco': np.asarray(expected).tolist()})
    def name(value):
        value = re.sub(r'[^a-zA-Z0-9_]', '_', value)
        return '_' + value if value and value[0].isdigit() else value
    def prim(path):
        try: return stage.prim_at(path)
        except KeyError: return None
    for i in range(1, model.nbody):
        body = model.body(i)
        if not body.name: continue
        p = prim('/World/Links/' + name(body.name))
        if p is None: continue  # compiler fusion / unnamed generated bodies
        m = np.asarray(p.local_transform())
        check('body_position:' + body.name, m[3, :3], data.xpos[i])
        check('body_rotation:' + body.name, m[:3, :3].T, data.xmat[i].reshape(3, 3))
    for group, expected in [('actuators', model.nu), ('sites', model.nsite),
                            ('tendons', model.ntendon), ('equalities', model.neq)]:
        check('count:' + group, len(getattr(info, group)), expected, 0)
    for i in range(model.njnt):
        joint = model.joint(i)
        if not joint.name or int(model.jnt_type[i]) not in (2, 3): continue
        p = prim('/World/Joints/' + name(joint.name))
        if p is None: continue
        if model.jnt_limited[i]:
            limit = model.jnt_range[i] * (180 / np.pi if int(model.jnt_type[i]) == 3 else 1)
            for attr, value in zip(('lowerLimit', 'upperLimit'), limit):
                check('joint_' + attr + ':' + joint.name, p.get('physics:' + attr), value)
        targets = p.relationship('physics:body1').targets
        if not targets: continue
        body = prim(targets[0]); m = np.asarray(body.local_transform())
        pos = np.asarray(p['physics:localPos1'])
        check('joint_anchor:' + joint.name, pos @ m[:3, :3] + m[3, :3], data.xanchor[i])
    return {'checks': counts, 'mismatches': errors, 'status': 'PASS' if not errors else 'FAIL'}


def worker(args):
    import lightusd
    source = args.menagerie / args.case
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    row = {'file': args.case, 'status': 'FAIL'}
    try:
        if source.suffix == '.obj':
            stage = lightusd.import_obj(source, max_memory=args.memory)
            row['python'] = metrics(stage)
            row['warnings'] = stage.warnings()
            if args.validate_physics:
                row['physics_validation'] = dataclasses.asdict(lightusd.physics.validate(stage))
            stage.close()
        else:
            import mujoco
            model = mujoco.MjModel.from_xml_path(str(source))
            data = mujoco.MjData(model)
            mujoco.mj_forward(model, data)
            row['reference'] = {k: int(getattr(model, k)) for k in ('nbody', 'njnt', 'ngeom', 'nu', 'nsite', 'ntendon', 'neq')}
            stage = lightusd.import_mjcf(source, max_memory=args.memory, asset_dirs=[args.menagerie],
                                        tessellate_collision_shapes=args.tessellate_collision_shapes)
            row['python'] = metrics(stage)
            row['reference_checks'] = reference_checks(stage, model, data)
            row['warnings'] = stage.warnings()
            if args.validate_physics:
                row['physics_validation'] = dataclasses.asdict(lightusd.physics.validate(stage))
            expected = row['python']
            for cli in ([] if args.reference_only else args.cli):
                label = Path(cli).name
                destination = out / (label + '.usda')
                run = command([cli, str(source), '--format', 'all', '-o', str(destination),
                               '--asset-dir', str(args.menagerie), '--max-memory', str(args.memory)] +
                              (['--tessellate-collision-shapes'] if args.tessellate_collision_shapes else []), timeout=args.timeout)
                (out / (label + '.log')).write_text(run.stdout)
                if run.returncode:
                    raise RuntimeError(label + ': ' + run.stdout[-3000:])
                for ext in ('usda', 'usdc', 'usdz'):
                    path = destination.with_suffix('.' + ext)
                    reopened = lightusd.load(path)
                    got = metrics(reopened)
                    reopened.close()
                    assert got == expected, (label, ext, expected, got)
                    if args.usdcat:
                        check = command([args.usdcat, str(path), '-o', str(out / 'oracle.usda')], timeout=args.timeout)
                        assert check.returncode == 0, check.stdout[-3000:]
                        (out / 'oracle.usda').unlink(missing_ok=True)
                    path.unlink()
            for ext in (() if args.reference_only else ('usda', 'usdc', 'usdz')):
                path = out / ('python.' + ext)
                stage.save(str(path))
                reopened = lightusd.load(path)
                assert metrics(reopened) == expected, ext
                reopened.close()
                path.unlink()
            stage.close()
        row['status'] = 'PASS'
    except Exception:
        row['error'] = traceback.format_exc()
    row['seconds'] = round(time.monotonic() - started, 3)
    import resource
    row['peak_rss_kib'] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    (out / 'result.json').write_text(json.dumps(row, indent=2))
    print(json.dumps(row))
    return 0 if row['status'] == 'PASS' else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--menagerie', type=lambda v: Path(v).expanduser().resolve(), required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cli', action='append', default=[])
    p.add_argument('--usdcat')
    p.add_argument('--workers', type=int, default=2)
    p.add_argument('--timeout', type=int, default=300)
    p.add_argument('--memory', type=int, default=4 * 1024**3)
    p.add_argument('--case')
    p.add_argument('--reference-only', action='store_true', help='Import and compare MuJoCo without output roundtrips')
    p.add_argument('--validate-physics', action='store_true', help='Also report native semantic physics/core validation separately')
    p.add_argument('--tessellate-collision-shapes', action='store_true', help='Use mesh representations of analytic collision shapes')
    p.add_argument('--limit', type=int)
    p.add_argument('--cases-from', type=Path, help='JSON list of source paths to rerun')
    p.add_argument('--only', choices=['xml', 'obj', 'all'], default='all')
    a = p.parse_args()
    a.cli = [str(Path(v).resolve()) for v in a.cli]
    a.out = a.out.resolve()
    if a.case:
        return worker(a)
    a.out.mkdir(parents=True, exist_ok=True)
    files = command(['git', '-C', str(a.menagerie), 'ls-files']).stdout.splitlines()
    xml = [f for f in files if f.endswith('.xml')]
    entries, fragments, includes = [], [], set()
    for f in xml:
        root = ET.parse(a.menagerie / f).getroot()
        (entries if root.find('worldbody') is not None or root.find('include') is not None else fragments).append(f)
        for include in root.iter('include'):
            includes.add(str(((a.menagerie / f).parent / include.get('file', '')).resolve().relative_to(a.menagerie)))
    # Auxiliary actuator/site fragment: it cannot compile without its parent bodies.
    actuator_fragment = 'aloha/filtered_cartesian_actuators.xml'
    if actuator_fragment in entries:
        entries.remove(actuator_fragment); fragments.append(actuator_fragment)
    objs = [f for f in files if f.endswith('.obj')]
    cases = (entries if a.only != 'obj' else []) + (objs if a.only != 'xml' else [])
    if a.cases_from: cases = json.loads(a.cases_from.read_text())
    if a.limit: cases = cases[:a.limit]
    report = {'dataset_revision': command(['git', '-C', str(a.menagerie), 'rev-parse', 'HEAD']).stdout.strip(),
              'python_version': sys.version, 'reference_only': a.reference_only,
              'validate_physics': a.validate_physics,
              'tessellate_collision_shapes': a.tessellate_collision_shapes,
              'cli_sha256': {v: hashlib.sha256(Path(v).read_bytes()).hexdigest() for v in a.cli},
              'source_revision': command(['git', 'rev-parse', 'HEAD']).stdout.strip(),
              'inventory': {'xml': len(xml), 'entry_points': len(entries), 'obj': len(objs),
                            'fragments': fragments, 'orphan_fragments': sorted(set(fragments) - includes)},
              'cases': []}
    import mujoco
    import lightusd
    report['python_module_sha256'] = hashlib.sha256(Path(lightusd._core.__file__).read_bytes()).hexdigest()
    report['mujoco_version'] = mujoco.__version__
    def run(case):
        directory = a.out / (re.sub(r'[^a-zA-Z0-9_.-]', '_', case) + '_' + hashlib.sha256(case.encode()).hexdigest()[:8])
        cmd = [sys.executable, __file__, '--menagerie', str(a.menagerie), '--out', str(directory),
               '--case', case, '--timeout', str(a.timeout), '--memory', str(a.memory)]
        for cli in a.cli: cmd += ['--cli', cli]
        if a.usdcat: cmd += ['--usdcat', a.usdcat]
        if a.reference_only: cmd += ['--reference-only']
        if a.validate_physics: cmd += ['--validate-physics']
        if a.tessellate_collision_shapes: cmd += ['--tessellate-collision-shapes']
        try:
            result = command(cmd, timeout=a.timeout, start_new_session=True)
            if (directory / 'result.json').exists():
                row = json.loads((directory / 'result.json').read_text())
                if row['status'] == 'PASS' and row.get('physics_validation', {}).get('valid', True): shutil.rmtree(directory)
                return row
            return {'file': case, 'status': 'FAIL', 'error': result.stdout[-4000:] or 'Worker exited before reporting a result', 'returncode': result.returncode}
        except subprocess.TimeoutExpired:
            return {'file': case, 'status': 'TIMEOUT'}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as executor:
        for row in executor.map(run, cases):
            report['cases'].append(row)
            print(row['status'], row['file'], row.get('seconds', ''), flush=True)
            (a.out / 'report.json').write_text(json.dumps(report, indent=2))
    failed = [r for r in report['cases'] if r['status'] != 'PASS']
    parity = [r for r in report['cases'] if r.get('reference_checks', {}).get('status') == 'FAIL']
    validation = [r for r in report['cases'] if not r.get('physics_validation', {}).get('valid', True)]
    report['summary'] = {'passed': len(cases) - len(failed), 'failed': len(failed),
                         'physics_parity_failures': len(parity), 'physics_validation_failures': len(validation)}
    (a.out / 'report.json').write_text(json.dumps(report, indent=2))
    summary = [f"# Menagerie import results", '', f"Dataset: `{report['dataset_revision']}`", '',
               f"{len(cases) - len(failed)}/{len(cases)} cases passed; {len(fragments)} XML fragments accounted for.",
               f"Physics reference mismatches in {len(parity)} entries (see JSON for field-level diagnostics).", '',
               f"Native physics validation failures in {len(validation)} entries (requested: {a.validate_physics}).", '',
               '| File | Conversion | Physics reference | Seconds |', '|---|---|---|---:|']
    summary += [f"| {r['file']} | {r['status']} | {r.get('reference_checks', {}).get('status', 'n/a')} | {r.get('seconds', '')} |" for r in report['cases']]
    (a.out / 'report.md').write_text('\n'.join(summary) + '\n')
    return bool(failed or parity or validation)


if __name__ == '__main__':
    sys.exit(main())
