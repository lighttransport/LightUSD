#!/usr/bin/env bash
#
# AOUSD conformance suite: load every AOUSD fixture through the core loader and
# assert no crashes.  External data — skip when the suite is not mounted.
set -uo pipefail
SKIP=77

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${LUSDVIEW:-$ROOT/build/lusdview}"
[ -x "$BIN" ] || BIN="$ROOT/build_ninja/lusdview"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }

AOUSD_ROOT="${AOUSD_ROOT:-$ROOT/aousd}"
[ -d "$AOUSD_ROOT" ] || { echo "SKIP: AOUSD_ROOT=$AOUSD_ROOT not found"; exit "$SKIP"; }
[ -f "$AOUSD_ROOT/README.md" ] || { echo "SKIP: $AOUSD_ROOT does not look like the AOUSD suite"; exit "$SKIP"; }

RUN=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a)
fi

err=0
while IFS= read -r -d '' f; do
  base="$(basename "$f")"
  [[ "$base" == "test_output.usdc" ]] && continue
  for loader in core; do
    args=()
    LOG="$("${RUN[@]}" "$BIN" --headless --backend vk --frames 1 "${args[@]}" "$f" 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
      if grep -Eiq 'no Vulkan|Vulkan.*unavailable|renderer init failed' <<<"$LOG"; then
        echo "SKIP: $loader $f (no Vulkan backend)"
        exit "$SKIP"
      fi
      echo "FAIL: $loader $f (exit $rc)"
      echo "$LOG"
      err=1
    fi
  done
  [ "$err" -eq 0 ] && echo "PASS: $base (core loader)"
done < <(find "$AOUSD_ROOT" -type f \( -name '*.usda' -o -name '*.usdc' \) \
  ! -path '*/OpenUSD/*' ! -path '*/dist_minimal/*' ! -path '*/cpp_cmake/*' \
  ! -path '*/cpp_makefile/*' ! -path '*/build/*' -print0)

[ "$err" -eq 0 ] || { echo "FAIL: AOUSD conformance suite — $err test(s) failed"; exit 1; }
echo "PASS: AOUSD conformance suite"
exit 0
