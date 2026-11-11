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
SCENE="$ROOT/examples/lusdview/tests/camera-dof.usda"

render() {
  local camera="$1" image="$2" log="$3"
  env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --core --headless --cuda \
    --frames 1 --rt-samples 8 --camera "$camera" --config "$OUT/config.json" \
    --screenshot "$image" "$SCENE" >"$log" 2>&1
}

render Pinhole "$OUT/pinhole.ppm" "$OUT/pinhole.log"
if ! grep -q 'CUDA RT wrote' "$OUT/pinhole.log" || [ ! -s "$OUT/pinhole.ppm" ]; then
  echo "SKIP: CUDA/NVRTC unavailable for camera DOF regression"
  tail -20 "$OUT/pinhole.log"
  exit "$SKIP"
fi
render ThinLens "$OUT/dof.ppm" "$OUT/dof.log"
if ! grep -q 'CUDA RT wrote' "$OUT/dof.log" || [ ! -s "$OUT/dof.ppm" ]; then
  echo "FAIL: authored thin-lens CUDA render failed"
  cat "$OUT/dof.log"
  exit 1
fi

python3 - "$OUT/pinhole.ppm" "$OUT/dof.ppm" <<'PY'
import sys

def ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        size = f.readline().split()
        assert f.readline().strip() == b'255'
        return int(size[0]), int(size[1]), f.read()

wa, ha, a = ppm(sys.argv[1]); wb, hb, b = ppm(sys.argv[2])
assert (wa, ha) == (wb, hb)
mad = sum(abs(x-y) for x, y in zip(a, b)) / len(a)
if mad < 0.5:
    raise SystemExit(f'FAIL: authored fStop produced no discriminating DOF response (MAD {mad:.3f})')
print(f'PASS: CUDA thin-lens DOF differs from pinhole rendering (MAD {mad:.3f})')
PY
