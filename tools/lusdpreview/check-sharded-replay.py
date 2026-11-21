#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare cold serial and sharded top-level replay, including budget crossings."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, fixture, root, name, options):
    env = os.environ.copy()
    for key in ("SERIAL", "SHARD"):
        env.pop(f"LIGHTUSD_PREVIEW_{key}_REPLAY", None)
    if name == "serial":
        env["LIGHTUSD_PREVIEW_SERIAL_REPLAY"] = "1"
    elif name == "grouped":
        env["LIGHTUSD_PREVIEW_SHARD_REPLAY"] = "0"
    return subprocess.run(
        [binary, fixture, root / name, "--preset", "fast", "--refresh",
         "--preview-cache-dir", root / "cache", *options],
        env=env, capture_output=True, text=True, timeout=90)


def main():
    binary, source = sys.argv[1:]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Coincident copies with different colours have equal positional ranks.
        scene = Path(source).read_text()
        start = scene.index('            def Xform "rock"')
        end = scene.index('\n        }', start)
        second = scene[start:end].replace('"rock"', '"other"').replace(
            '(0.6, 0.5, 0.4)', '(0.2, 0.8, 0.1)')
        n = 4096
        positions = [f"({(i // 2) % 64}, 0, {(i // 2) // 64})" for i in range(n)]
        scene = (scene[:end] + '\n' + second + scene[end:]).replace(
            '= </World/clutter/proto/rock>', '= [</World/clutter/proto/rock>, </World/clutter/proto/other>]').replace(
            '[0, 0, 0]', '[' + ', '.join(str(i % 2) for i in range(n)) + ']').replace(
            '[(0, 0, 0), (5, 0, 0), (10, 0, 0)]', '[' + ', '.join(positions) + ']')
        fixture = root / "instances.usda"
        fixture.write_text(scene)
        sizes = {}
        for case, extra in [('ample', []), ('coarsen', ['--preview-budget-mib', '64']),
                            ('explicit', ['--preview-budget-mib', '64', '--preview-resolution', '1024'])]:
            directory = root / case
            directory.mkdir()
            for mode in ('serial', 'grouped', 'shard'):
                result = run(binary, fixture, directory, mode, extra)
                if case == 'explicit':
                    if result.returncode == 0 or 'explicit preview resolution exceeds' not in result.stderr:
                        raise RuntimeError(result.stdout + result.stderr)
                elif result.returncode or 'cache_hit=0' not in result.stdout:
                    raise RuntimeError(result.stdout + result.stderr)
            if case != 'explicit':
                sizes[case] = float(re.search(r' cell=([^ ]+)', result.stdout).group(1))
                def artifacts(mode):
                    return {p.relative_to(directory / mode): p.read_bytes()
                            for p in (directory / mode).rglob('*') if p.is_file()}
                expected = artifacts('serial')
                if not expected or any(expected != artifacts(mode) for mode in ('grouped', 'shard')):
                    raise RuntimeError(f'{case}: sidecars differ')
                paths = [entry['path'] for entry in
                         json.loads(expected[Path('manifest.json')])['outputs']]
                if paths != sorted(paths):
                    raise RuntimeError(f'{case}: manifest outputs are unordered')
        if sizes['coarsen'] < 4 * sizes['ample']:
            raise RuntimeError(f"fixture did not coarsen at least twice: {sizes}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
