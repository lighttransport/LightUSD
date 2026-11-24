#!/usr/bin/env python3
"""Compare numeric/default/held/linear sample results against a pinned OpenUSD build."""
import argparse
import math
import os
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--test', required=True)
parser.add_argument('--root', required=True)
parser.add_argument('--pxr-python', required=True)
args = parser.parse_args()
if not pathlib.Path(args.root, 'value_resolution/tests/assets').is_dir():
    print('SKIP: AOUSD supplemental corpus unavailable')
    sys.exit(77)
# Relocated OpenUSD builds retain their original compiled-in plug paths.
# Anchor plugin discovery at the selected Python installation as well.
pxr_root = pathlib.Path(args.pxr_python).resolve().parent
os.environ['PXR_PLUGINPATH_NAME'] = str(pxr_root / 'usd') + os.pathsep + os.environ.get('PXR_PLUGINPATH_NAME', '')
sys.path.insert(0, args.pxr_python)
try:
    from pxr import Usd
except ImportError:
    print('SKIP: pinned OpenUSD Python package unavailable')
    sys.exit(77)
result = subprocess.run([args.test, args.root, '--dump-samples'], capture_output=True,
                        text=True, check=True, timeout=120)
stages = {}
failures = []
count = 0
aousd_differences = 0
for line in result.stdout.splitlines():
    case, path, attr, interpolation, time, valued, numeric = line.split('\t')
    key = case, interpolation
    if key not in stages:
        stage = Usd.Stage.Open(str(pathlib.Path(args.root, 'value_resolution/tests/assets', case, 'entry.usd')))
        if not stage:
            raise RuntimeError(f'OpenUSD could not open {case}')
        stage.SetInterpolationType(Usd.InterpolationTypeLinear if interpolation == 'linear' else Usd.InterpolationTypeHeld)
        stages[key] = stage
    query = Usd.TimeCode.Default() if time == 'Default' else Usd.TimeCode(float(time))
    expected = stages[key].GetPrimAtPath(path).GetAttribute(attr).Get(query)
    actual = float(numeric) if valued == '1' else None
    same = expected is None and actual is None or isinstance(expected, (int, float)) and actual is not None and math.isclose(float(expected), actual, rel_tol=1e-6, abs_tol=1e-6)
    count += 1
    # The pinned AOUSD corpus explicitly expects these extrapolated results
    # (test_clip_timings), while this OpenUSD build clamps the time mapping.
    # Keep this narrow, value-checked difference visible; do not normalize it.
    if case == 'clip_timings' and float(time if time != 'Default' else 'nan') in (-1, 50, 60):
        aousd_expected = 0.0 if float(time) == -1 else 25.0
        pxr_expected = 10.0 if float(time) == -1 else 20.0
        if actual == aousd_expected and expected == pxr_expected:
            aousd_differences += 1
            continue
    if not same:
        failures.append(f'{case} {path}.{attr} {interpolation} t={time}: next={actual}, OpenUSD={expected}')
for failure in failures:
    print(failure, file=sys.stderr)
print(f'OpenUSD {Usd.GetVersion()}: {count} sampled values, {len(failures)} unexpected mismatches, {aousd_differences} AOUSD/OpenUSD timing differences')
if count != 304:
    raise RuntimeError(f'incomplete sample matrix: {count}')
sys.exit(bool(failures))
