#!/usr/bin/env bash
# Verify persistent CUDA PTX caching. The cold run uses the platform-default
# XDG cache path; the warm run names that same directory through the CLI.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found at $BIN"; exit "$SKIP"; }

OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT/xdg-cache" "$OUT/config-home"
if [ -z "${LUSDVIEW_TEST_OUT:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi
CONFIG="$OUT/config.json"
printf '%s\n' '{"window_size":{"width":160,"height":160}}' >"$CONFIG"
SCENE="$REPO_ROOT/examples/lusdview/tests/deform-morph-skin.usda"
CACHE_DIR="$OUT/xdg-cache/lusdview/cuda"

run_viewer() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_CUDA_CACHE_TIMEOUT:-90s}" "$@"
  else
    "$@"
  fi
}

cold_img="$OUT/cold.png"
cold_log="$OUT/cold.log"
run_viewer env XDG_CACHE_HOME="$OUT/xdg-cache" XDG_CONFIG_HOME="$OUT/config-home" \
  "$BIN" --core --headless --cuda --mode depth --camera Cam --frames 1 \
  --time 1 --config "$CONFIG" --no-skeleton --screenshot "$cold_img" "$SCENE" \
  >"$cold_log" 2>&1
cold_rc=$?
if ! grep -q 'CUDA RT wrote' "$cold_log" || [ ! -s "$cold_img" ]; then
  if grep -q 'CUDA kernel cache miss' "$cold_log"; then
    if grep -Eqi 'NVRTC|invalid value for --gpu-architecture|CUDA ray tracing unavailable|no CUDA device' "$cold_log"; then
      echo "SKIP: CUDA/NVRTC cannot compile for the selected device"
      tail -20 "$cold_log"
      exit "$SKIP"
    fi
    echo "FAIL: CUDA/NVRTC was available but the cold cache run failed (exit $cold_rc)"
    cat "$cold_log"
    exit 1
  fi
  echo "SKIP: CUDA/NVRTC unavailable for kernel-cache regression (exit $cold_rc)"
  tail -20 "$cold_log"
  exit "$SKIP"
fi
grep -q 'CUDA kernel cached:' "$cold_log" || {
  echo "FAIL: cold CUDA run did not populate the default lusdview cache"
  cat "$cold_log"
  exit 1
}
ptx_count="$(find "$CACHE_DIR" -maxdepth 1 -type f -name '*.ptx' | wc -l)"
[ "$ptx_count" -eq 1 ] || {
  echo "FAIL: expected one PTX entry under $CACHE_DIR, found $ptx_count"
  exit 1
}
ptx_file="$(find "$CACHE_DIR" -maxdepth 1 -type f -name '*.ptx' -print -quit)"

warm_img="$OUT/warm.png"
warm_log="$OUT/warm.log"
run_viewer env XDG_CONFIG_HOME="$OUT/config-home" \
  "$BIN" --core --headless --cuda --cuda-cache-dir "$CACHE_DIR" --mode depth \
  --camera Cam --frames 1 --time 1 --config "$CONFIG" --no-skeleton \
  --screenshot "$warm_img" "$SCENE" >"$warm_log" 2>&1
warm_rc=$?
if [ "$warm_rc" -ne 0 ] || ! grep -q 'CUDA RT wrote' "$warm_log" ||
   [ ! -s "$warm_img" ]; then
  echo "FAIL: warm CUDA cache run failed (exit $warm_rc)"
  cat "$warm_log"
  exit 1
fi
grep -q "CUDA kernel cache hit: $CACHE_DIR/" "$warm_log" || {
  echo "FAIL: --cuda-cache-dir run did not reuse the default-path PTX"
  cat "$warm_log"
  exit 1
}
cmp -s "$cold_img" "$warm_img" || {
  echo "FAIL: cached PTX changed CUDA pixels"
  exit 1
}

# A cache file can be truncated by storage failure or modified outside lusdview.
# It must never be trusted merely because its keyed filename matches.
printf '%s\n' 'intentionally invalid PTX' >"$ptx_file"
recovered_img="$OUT/recovered.png"
recovered_log="$OUT/recovered.log"
run_viewer env XDG_CONFIG_HOME="$OUT/config-home" \
  "$BIN" --core --headless --cuda --cuda-cache-dir "$CACHE_DIR" --mode depth \
  --camera Cam --frames 1 --time 1 --config "$CONFIG" --no-skeleton \
  --screenshot "$recovered_img" "$SCENE" >"$recovered_log" 2>&1
recovered_rc=$?
if [ "$recovered_rc" -ne 0 ] || ! grep -q 'CUDA RT wrote' "$recovered_log" ||
   [ ! -s "$recovered_img" ]; then
  echo "FAIL: invalid CUDA cache recovery failed (exit $recovered_rc)"
  cat "$recovered_log"
  exit 1
fi
grep -q 'ignoring invalid CUDA kernel cache entry:' "$recovered_log" || {
  echo "FAIL: corrupt PTX was not diagnosed"
  cat "$recovered_log"
  exit 1
}
grep -q 'CUDA kernel cached:' "$recovered_log" || {
  echo "FAIL: rejected PTX was not rebuilt"
  cat "$recovered_log"
  exit 1
}
cmp -s "$cold_img" "$recovered_img" || {
  echo "FAIL: rebuilt PTX changed CUDA pixels"
  exit 1
}

echo "PASS: CUDA kernel cache cold/warm/recovery paths preserve exact pixels"
