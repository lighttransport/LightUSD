#!/usr/bin/env python3
"""Assert failure type and finding, so missing fixtures cannot pass a test."""
import argparse
import json
import subprocess
p = argparse.ArgumentParser()
p.add_argument('--exit', type=int, required=True)
p.add_argument('--rule', required=True)
p.add_argument('command', nargs=argparse.REMAINDER)
a = p.parse_args()
command = a.command[1:] if a.command[:1] == ['--'] else a.command
r = subprocess.run([command[0], '--json', *command[1:]], capture_output=True, text=True, timeout=60)
assert r.returncode == a.exit, (command, r.returncode, r.stdout, r.stderr)
report = json.loads(r.stdout)
assert any(i['ruleId'] == a.rule for i in report['issues']), (a.rule, report)
