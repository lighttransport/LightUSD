#!/usr/bin/env bash
#
# doubleSided regression's sibling: UsdPreviewSurface specular workflow + IOR on
# the lusdview `--core` loader (audit T12). The loader read only the metallic
# workflow, so `useSpecularWorkflow=1` fell back to a fixed dielectric F0 (0.04)
# and `inputs:specularColor` was ignored -- a colored specular highlight rendered
# plain white. It now computes F0 = specularColor (spec workflow) / dielectric
# from ior (metallic workflow), unified GL<->VK.
#
# The probe: a dark-diffuse sphere with a GREEN specularColor under a distant
# light. The specular highlight must be green-dominant, and GL and VK must agree.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ -n "${LUSDVIEW:-}" ]; then BIN="$LUSDVIEW"
elif [ -x "$REPO_ROOT/build/lusdview" ]; then BIN="$REPO_ROOT/build/lusdview"
else BIN="$REPO_ROOT/build_ninja/lusdview"; fi
if [ ! -x "$BIN" ]; then echo "SKIP: lusdview not found ($BIN)"; exit "$SKIP"; fi
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit $SKIP; }

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CONFIG="$OUT/config.json"
printf '%s\n' '{"window_size":{"width":256,"height":256}}' > "$CONFIG"
XVFB=""
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  XVFB="xvfb-run -a"
fi

cat > "$OUT/spec.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Sphere "S" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    double radius = 1.5
    rel material:binding = </World/M>
  }
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.1, 0.1, 0.1)
      int inputs:useSpecularWorkflow = 1
      color3f inputs:specularColor = (0.05, 1.0, 0.05)
      float inputs:roughness = 0.25
      token outputs:surface
    }
  }
  def DistantLight "L" {
    float inputs:intensity = 3.0
    float3 xformOp:rotateXYZ = (-30, 30, 0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
  }
}
USDA

# Most-green pixel: G and how much it beats R/B. Prints "G dominance".
greenest() {
python3 - "$1" <<'PY'
import sys, re
d = open(sys.argv[1], "rb").read()
m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
if not m: sys.exit(2)
w, h = int(m.group(1)), int(m.group(2)); px = d[m.end():]
if len(px) < w*h*3: sys.exit(2)
bg = bd = 0
for o in range(0, w*h*3, 3):
    r, g, b = px[o], px[o+1], px[o+2]
    dom = g - max(r, b)
    if dom > bd: bd = dom; bg = g
print(bg, bd)
PY
}

fail=0
declare -A dom
for spec in "gl:--backend gl" "vk:--backend vk"; do
  tag="${spec%%:*}"; args="${spec#*:}"
  img="$OUT/spec_${tag}.ppm"
  if [ "$tag" = gl ]; then
    # GL needs a display. `--headless` intentionally selects Vulkan, so do not
    # pass it here or this comparison silently exercises Vulkan twice.
    # shellcheck disable=SC2086
    $XVFB "$BIN" $args --config "$CONFIG" --frames 3 --screenshot "$img" \
        "$OUT/spec.usda" >"$OUT/${tag}.log" 2>&1
  else
    "$BIN" --headless $args --config "$CONFIG" --frames 3 --screenshot "$img" \
        "$OUT/spec.usda" >"$OUT/${tag}.log" 2>&1
  fi
  if ! grep -q 'render stats' "$OUT/${tag}.log"; then
    echo "SKIP: $tag backend unavailable"; continue
  fi
  [ -s "$img" ] || { echo "FAIL: $tag no image"; fail=1; continue; }
  read -r g d < <(greenest "$img") || { echo "FAIL: $tag PPM parse"; fail=1; continue; }
  dom[$tag]=$d
  echo "$tag: greenest highlight G=$g dominance=$d"
  # A green specular highlight: G clearly beats R and B. A plain white/dielectric
  # highlight (the bug) has dominance ~0.
  if [ "$d" -lt 60 ]; then
    echo "FAIL: $tag specular highlight not green (dominance $d) -- specularColor / specular workflow ignored"; fail=1
  fi
done

# GL and VK must agree (unified F0 path).
if [ -n "${dom[gl]:-}" ] && [ -n "${dom[vk]:-}" ]; then
  diff=$(( dom[gl] - dom[vk] )); diff=${diff#-}
  [ "$diff" -le 20 ] \
    || { echo "FAIL: GL/VK specular highlight disagree (gl=${dom[gl]} vk=${dom[vk]})"; fail=1; }
fi

[ "$fail" -eq 0 ] || exit 1
echo "PASS: specular workflow + specularColor honored on --core (GL and VK)"
exit 0
