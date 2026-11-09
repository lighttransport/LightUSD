#!/usr/bin/env bash
set -uo pipefail
SKIP=77
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BIN="${1:-${LUSDVIEW:-$ROOT/build_ninja/lusdview}}"
[ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/config-home"
cat >"$OUT/shutter.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" startTimeCode = 0 endTimeCode = 20)
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
  }
  def Camera "Shot" {
    float focalLength = 50
    float horizontalAperture = 25
    float verticalAperture = 15
    double shutter:open = -0.25
    double shutter:close = 0.5
    double3 xformOp:translate = (0, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
}
USDA
if ! env XDG_CONFIG_HOME="$OUT/config-home" "$BIN" --next --headless \
    --backend vk --frames 1 --size 64x48 --camera Shot --time 10 \
    --render-report "$OUT/report.json" \
    "$OUT/shutter.usda" >"$OUT/run.log" 2>&1; then
  if grep -Eqi 'Vulkan.*(unavailable|failed)|no Vulkan' "$OUT/run.log"; then
    echo "SKIP: Vulkan unavailable"; exit "$SKIP"
  fi
  cat "$OUT/run.log"; exit 1
fi
python3 - "$OUT/report.json" <<'PY'
import json, math, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
s = r.get("camera_shutter", {})
if not s.get("enabled") or not math.isclose(s.get("width", 0), .75):
    raise SystemExit(f"FAIL: authored shutter missing from report: {s}")
want = [9.9375, 10.3125]
got = s.get("motion_sample_times")
if got is None or len(got) != len(want) or any(
        not math.isclose(a, b, abs_tol=1e-9) for a, b in zip(got, want)):
    raise SystemExit(f"FAIL: shutter stratification mismatch: {got} != {want}")
print("PASS: authored shutter contract and midpoint motion samples")
PY
