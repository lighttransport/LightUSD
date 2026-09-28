#!/usr/bin/env bash
# Check that the viewer's public mesh converter resolves clip-sourced points.
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT"
FIXTURE="$ROOT/examples/lusdview/tests/fixtures/value-clip/main.usda"
for time in 0 1; do
  "$BIN" --headless --backend vk --time "$time" --frames 1 --no-grid \
    --size 192x192 --screenshot "$OUT/frame-$time.ppm" "$FIXTURE" \
    >"$OUT/frame-$time.log" 2>&1 || {
      if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan|no suitable Vulkan' \
          "$OUT/frame-$time.log"; then
        echo "SKIP: Vulkan unavailable"; exit "$SKIP"
      fi
      cat "$OUT/frame-$time.log"; exit 1
    }
  if grep -Eqi 'load failed|no renderable geometry' "$OUT/frame-$time.log"; then
    cat "$OUT/frame-$time.log"; exit 1
  fi
done
python3 - "$OUT" <<'PY'
import pathlib, sys

def bounds(path):
    data = pathlib.Path(path).read_bytes()
    header, dims, maximum, rgb = data.split(b"\n", 3)
    if header != b"P6" or maximum != b"255":
        raise SystemExit(f"FAIL: invalid PPM {path}")
    width, height = map(int, dims.split())
    points = []
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 3
            pixel = rgb[i:i+3]
            if min(pixel) > 80 and max(pixel) - min(pixel) < 20:
                points.append((x, y))
    if len(points) < 50:
        raise SystemExit(f"FAIL: no visible clip-backed mesh in {path}")
    return (max(p[0] for p in points) - min(p[0] for p in points) + 1,
            max(p[1] for p in points) - min(p[1] for p in points) + 1)

root = pathlib.Path(sys.argv[1])
wide = bounds(root / "frame-0.ppm")
tall = bounds(root / "frame-1.ppm")
wide_ratio = wide[0] / wide[1]
tall_ratio = tall[0] / tall[1]
if wide_ratio < 1.7 or tall_ratio > 0.65:
    raise SystemExit(f"FAIL: clip values were not applied: wide={wide}, tall={tall}")
print(f"PASS: clip-backed mesh carrier changed aspect {wide_ratio:.2f} -> {tall_ratio:.2f}")
PY
