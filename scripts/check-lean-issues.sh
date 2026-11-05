#!/usr/bin/env bash
# Every `SPEC-ISSUE: LEAN-NNN` tag in verif/lean/UsdSpec must have a row in
# verif/lean/ISSUES.md, and every registry row must be tagged somewhere.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
lean="$root/verif/lean"
tags=$(grep -rhoE 'SPEC-ISSUE: LEAN-[0-9]+' "$lean/UsdSpec" | sed 's/SPEC-ISSUE: //' | sort -u)
rows=$(grep -oE '^\| LEAN-[0-9]+' "$lean/ISSUES.md" | sed 's/| //' | sort -u)
rc=0
for t in $tags; do
  case "$t" in LEAN-[0-9]) continue;; esac
  grep -q "^| $t " "$lean/ISSUES.md" || { echo "untracked tag: $t"; rc=1; }
done
for r in $rows; do
  grep -rq "SPEC-ISSUE: $r" "$lean/UsdSpec" || { echo "registry row without tag: $r"; rc=1; }
done
[ $rc -eq 0 ] && echo "check-lean-issues: $(echo "$tags" | wc -w) tags OK"
exit $rc
