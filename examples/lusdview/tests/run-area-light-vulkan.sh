#!/usr/bin/env bash
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"
printf '%s\n' '{"window_size":{"width":160,"height":120}}' > "$OUT/config.json"

render() {
  local scene="$1" frames="$2" image="$3" log="$4"
  timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-120s}" \
    env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next --headless \
    --backend vk --rt --frames "$frames" --camera Camera --config "$OUT/config.json" \
    --screenshot "$image" "$scene" >"$log" 2>&1
}

# Software Vulkan adapters can expose the raster path while spending minutes
# in the compute-BVH ray-query fallback. Probe the adapter before entering the
# RT path so the focused regression remains a hardware-RT test instead of
# consuming the CTest timeout.
probe_log="$OUT/probe.log"
if ! timeout --kill-after=5s 30s env XDG_CONFIG_HOME="$OUT/config-home-probe" \
    "$BIN" --next --headless --backend vk --frames 1 --size 16x16 \
    --camera Camera --config "$OUT/config.json" \
    "$ROOT/examples/lusdview/tests/point-light-shadow.usda" \
    >"$probe_log" 2>&1; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$probe_log"; then
    echo "SKIP: Vulkan unavailable"
    exit "$SKIP"
  fi
  cat "$probe_log"
  exit 1
fi
if grep -Eqi 'caps:.*device=cpu|renderer: Vulkan .*GPU:.*(llvmpipe|SwiftShader)' \
    "$probe_log"; then
  echo "SKIP: Vulkan RT area-light test needs a non-CPU Vulkan adapter"
  exit "$SKIP"
fi

render "$ROOT/examples/lusdview/tests/point-light-shadow.usda" 1 \
       "$OUT/point.ppm" "$OUT/point.log"
if ! grep -q 'rt yes' "$OUT/point.log" || [ ! -s "$OUT/point.ppm" ]; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/point.log"; then
    echo "SKIP: Vulkan ray query unavailable"
    exit "$SKIP"
  fi
  echo "SKIP: Vulkan ray query unavailable for area-light regression"
  tail -20 "$OUT/point.log"
  exit "$SKIP"
fi
render "$ROOT/examples/lusdview/tests/area-light-shadow.usda" 64 \
       "$OUT/area.ppm" "$OUT/area.log"
[ -s "$OUT/area.ppm" ] || { echo "FAIL: Vulkan area-light render failed"; cat "$OUT/area.log"; exit 1; }

python3 - "$OUT/point.ppm" "$OUT/area.ppm" <<'PY'
import sys
def ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = map(int, f.readline().split())
        assert f.readline().strip() == b'255'
        return w, h, f.read()
w, h, point = ppm(sys.argv[1]); wa, ha, area = ppm(sys.argv[2])
assert (w, h) == (wa, ha)
mad = sum(abs(a-b) for a, b in zip(point, area)) / len(point)
# A finite emitter must materially change the hard-shadow reference. Requiring
# many changed pixels prevents a single stochastic outlier from passing.
changed = sum(1 for i in range(0, len(point), 3)
              if sum(abs(point[i+c]-area[i+c]) for c in range(3)) >= 12)
if mad < 0.5 or changed < w*h//100:
    raise SystemExit(f'FAIL: finite RectLight produced no soft-shadow response '
                     f'(MAD {mad:.3f}, changed {changed})')
print(f'PASS: finite RectLight differs from zero-radius hard shadow '
      f'(MAD {mad:.3f}, changed {changed})')
PY
