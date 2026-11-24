#!/usr/bin/env bash
# Raster + Vulkan ray-query fixed-frame checkpoint regression.
set -uo pipefail

viewer="${LUSDVIEW:-build_ninja/lusdview}"
root="${LUSDVIEW_ROOT:-.}"
out="$(mktemp -d "${TMPDIR:-/tmp}/lusdview-checkpoints.XXXXXX")"
trap 'rm -rf "$out"' EXIT

if [[ ! -x "$viewer" ]]; then
  echo "FAIL: lusdview executable not found: $viewer" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 unavailable"
  exit 77
fi

run_mode() {
  local mode="$1"
  shift
  local log="$out/$mode.log"
  if ! timeout 60s "$viewer" --headless --backend vk "$@" --core \
      --frames 5 --size 192x128 --checkpoint-every 2 \
      --checkpoint-pattern "$out/$mode-{frame}.png" \
      --screenshot "$out/$mode-final.png" --render-report "$out/$mode.json" \
      "$root/models/cube.usdz" >"$log" 2>&1; then
    if grep -Eq 'no compatible Vulkan|ray tracing is unavailable|no Vulkan device|renderer init failed' "$log"; then
      echo "SKIP: Vulkan checkpoint capability unavailable"
      exit 77
    fi
    echo "FAIL: $mode checkpoint run failed" >&2
    cat "$log" >&2
    return 1
  fi
  for frame in 000002 000004; do
    [[ -s "$out/$mode-$frame.png" ]] || {
      echo "FAIL: missing $mode frame $frame checkpoint" >&2
      return 1
    }
  done
  [[ -s "$out/$mode-final.png" ]] || {
    echo "FAIL: missing $mode final image" >&2
    return 1
  }
  python3 - "$out/$mode.json" "$mode" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    report = json.load(f)
render = report["render"]
assert render["checkpoint_every"] == 2, render
assert render["checkpoint_count"] == 2, render
assert "{frame}" in render["checkpoint_pattern"], render
expected_samples = 5 if sys.argv[2] == "rt" else 1
assert render["samples"] == expected_samples, render
PY
}

run_mode raster
run_mode rt --rt

# --rt is a graceful fallback on devices without ray-query support. The
# fallback exits successfully and writes ordinary one-sample checkpoints, but
# that is not an RT convergence result and must be a capability skip.
if grep -Eqi 'ray tracing is unavailable|ray-query unavailable' "$out/rt.log"; then
  echo "SKIP: Vulkan ray-query unavailable"
  exit 77
fi

if cmp -s "$out/rt-000002.png" "$out/rt-000004.png"; then
  echo "FAIL: progressive RT checkpoints are byte-identical" >&2
  exit 1
fi

echo "PASS: raster checkpoints and progressive Vulkan RT convergence"
