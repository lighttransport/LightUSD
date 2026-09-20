#!/usr/bin/env bash
# Raster depth-of-field regression. A nearly closed aperture provides the sharp
# reference; the same camera at f/0.5 must produce a discriminating image on GL
# and Vulkan while preserving broad cross-backend response parity.
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
command -v xvfb-run >/dev/null || { echo "SKIP: xvfb-run required"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"
SCENE="$ROOT/examples/lusdview/tests/camera-dof.usda"

render() {
  local backend="$1" fstop="$2" image="$3" log="$4"
  local args=(--next --backend "$backend" --frames 2 --size 320x200
              --camera ThinLens --f-stop "$fstop" --focus-distance 8
              --no-grid --screenshot "$image" "$SCENE")
  if [ "$backend" = vk ]; then
    env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --headless "${args[@]}" \
      >"$log" 2>&1
  else
    env XDG_CONFIG_HOME="$OUT/config-home" xvfb-run -a "$BIN" "${args[@]}" \
      >"$log" 2>&1
  fi
}

BACKENDS="${LUSDVIEW_RASTER_DOF_BACKENDS:-gl vk}"
for backend in $BACKENDS; do
  render "$backend" 1000 "$OUT/$backend-sharp.ppm" "$OUT/$backend-sharp.log" || {
    if [ "$backend" = vk ] && grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/$backend-sharp.log"; then
      echo "SKIP: Vulkan unavailable"; exit "$SKIP"
    fi
    cat "$OUT/$backend-sharp.log"; exit 1
  }
  render "$backend" 0.5 "$OUT/$backend-dof.ppm" "$OUT/$backend-dof.log" || {
    cat "$OUT/$backend-dof.log"; exit 1
  }
done

python3 - "$OUT" "$BACKENDS" <<'PY'
import pathlib, sys

def ppm(path):
    raw = pathlib.Path(path).read_bytes()
    head, dims, maxv, pixels = raw.split(b"\n", 3)
    if head != b"P6" or maxv != b"255":
        raise SystemExit(f"FAIL: invalid PPM {path}")
    w, h = map(int, dims.split())
    return w, h, pixels[:w*h*3]

root = pathlib.Path(sys.argv[1])
backends = sys.argv[2].split()
responses = {}
for backend in backends:
    wa, ha, sharp = ppm(root / f"{backend}-sharp.ppm")
    wb, hb, dof = ppm(root / f"{backend}-dof.ppm")
    if (wa, ha) != (wb, hb) or (wa, ha) != (320, 200):
        raise SystemExit(f"FAIL: {backend} raster DOF extent mismatch")
    mad = sum(abs(a-b) for a, b in zip(sharp, dof)) / len(sharp)
    if mad < 0.5:
        raise SystemExit(
            f"FAIL: {backend} f-stop produced no raster DOF response (MAD {mad:.3f})")
    responses[backend] = mad

if "gl" in responses and "vk" in responses:
    ratio = responses["gl"] / responses["vk"]
    if not 0.25 <= ratio <= 4.0:
        raise SystemExit(f"FAIL: GL/Vulkan raster DOF response diverged: {responses}")
print("PASS: raster DOF " + ", ".join(
    f"{name} MAD={mad:.3f}" for name, mad in responses.items()))
PY
