#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MuJoCo consumer matrix: source MJCF vs LightUSD-converted USD.

Runs tools/view_control/dynamics.py with `--engine mujoco-mjcf --engine
mujoco-usd` over the bounded analytic fixtures and asserts the documented
verdicts (doc/validation/physics-item7-closure-2026-11-06.md):

  freefall, torque-inertia, slide-spring, revolute-pendulum  PASS on both paths
  frictionless-contact (MuJoCo default solref 0.02 1)        FAIL on both paths
      (compliant default contact; the raw MJCF control fails identically)
  frictionless-stiff-contact (solref 0.005 1)                PASS on both paths

Needs the USD-enabled MuJoCo consumer built by setup_mujoco_usd.sh:
  LIGHTUSD_MUJOCO_WORKER       bounded-dynamics worker executable
  LIGHTUSD_MUJOCO_USD_PLUGIN   libusd_decoder_plugin.so
Exit 77 (ctest SKIP) when either is absent or `lightusd` cannot be imported.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

EXPECTED = {
    'physics-dynamics': {
        'freefall': 'PASS',
        'torque-inertia': 'PASS',
        'slide-spring': 'PASS',
        'revolute-pendulum': 'PASS',
        'frictionless-contact': 'FAIL',
    },
    'physics-contact-stiff': {'frictionless-stiff-contact': 'PASS'},
}
ENGINES = ('mujoco-mjcf', 'mujoco-usd')


def skip(reason):
    print(f'SKIP: {reason}')
    sys.exit(SKIP)


def main():
    worker = os.environ.get('LIGHTUSD_MUJOCO_WORKER')
    plugin = os.environ.get('LIGHTUSD_MUJOCO_USD_PLUGIN')
    if not worker or not plugin:
        skip('set LIGHTUSD_MUJOCO_WORKER and LIGHTUSD_MUJOCO_USD_PLUGIN '
             '(build them with tools/view_control/setup_mujoco_usd.sh)')
    for label, path in (('worker', worker), ('USD decoder plugin', plugin)):
        if not Path(path).is_file():
            skip(f'{label} not found: {path}')

    environment = dict(os.environ)
    python_dir = REPO / 'python'
    if python_dir.is_dir():
        environment['PYTHONPATH'] = os.pathsep.join(
            [str(python_dir)] + ([environment['PYTHONPATH']] if environment.get('PYTHONPATH') else []))
    probe = subprocess.run([sys.executable, '-c', 'import lightusd'], env=environment,
                           capture_output=True, text=True)
    if probe.returncode:
        skip('lightusd Python module not importable (build the extension or set PYTHONPATH)')

    failures = []
    for job, expected in EXPECTED.items():
        with tempfile.TemporaryDirectory(prefix='mujoco-usd-matrix-') as tmp:
            out = Path(tmp) / 'out'
            command = [sys.executable, str(HERE / 'dynamics.py'),
                       '--config', str(HERE / 'jobs' / f'{job}.json'), '--out', str(out),
                       '--mujoco-worker', worker, '--mujoco-plugin', plugin, '--timeout', '300']
            for engine in ENGINES:
                command += ['--engine', engine]
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            results_path = out / 'results.json'
            if not results_path.is_file():
                failures.append(f'{job}: no results.json (exit {run.returncode})\n{run.stdout}\n{run.stderr}')
                continue
            rows = {row['id']: row for row in json.loads(results_path.read_text())['cases']}
            for case_id, want in expected.items():
                row = rows.get(case_id)
                if row is None:
                    failures.append(f'{job}/{case_id}: case missing from results')
                    continue
                for engine in ENGINES:
                    got = row['engines'].get(engine, {}).get('status', 'MISSING')
                    mark = 'ok ' if got == want else 'BAD'
                    print(f'  {mark} {case_id:28s} {engine:12s} {got} (expected {want})')
                    if got != want:
                        detail = row['engines'].get(engine, {}).get('error', '')
                        failures.append(f'{job}/{case_id}/{engine}: expected {want}, got {got} {detail}')
    if failures:
        print('FAIL:\n  ' + '\n  '.join(failures))
        return 1
    print('PASS: MuJoCo MJCF/USD matrix matches the documented verdicts')
    return 0


if __name__ == '__main__':
    sys.exit(main())
