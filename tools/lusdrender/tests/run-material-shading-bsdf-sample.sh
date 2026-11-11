#!/usr/bin/env bash
#
# lusdrender experimental BSDF sampling regression. A reflective quad over a red
# background should shift red in -materialShading lightrt-bsdf because the
# bounded bsdf_sample continuation bounce sees the background; legacy shading
# remains neutral under the same headlight.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/scene.usda" <<'USD'
#usda 1.0
(
  defaultPrim = "World"
  upAxis = "Y"
)

def Xform "World"
{
  def Mesh "Quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  )
  {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
      interpolation = "vertex"
    )
    uniform token subdivisionScheme = "none"
    rel material:binding = </World/Mat>
  }

  def Material "Mat"
  {
    token outputs:surface.connect = </World/Mat/Preview.outputs:surface>

    def Shader "Preview"
    {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.8, 0.8, 0.8)
      float inputs:metallic = 1
      float inputs:roughness = 0.12
      token outputs:surface
    }
  }
}
USD

for mode in legacy lightrt-bsdf; do
  "$LUSDRENDER" "$TMP/scene.usda" "$TMP/$mode.png" -rtPreview \
    -w 64 -height 64 -viewDir 0,0,1 -bg 1,0,0 -ambient 0 -noShadows \
    -samples 4 --materialShading "$mode" --materialResolver tydra \
    >"$TMP/$mode.log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: lusdrender $mode exited with $rc"
    cat "$TMP/$mode.log"
    exit 1
  fi
done

read -r LR LG LB BR BG BB < <(
  python3 - "$REPO_ROOT/examples/lusdview/tests" \
    "$TMP/legacy.png" "$TMP/lightrt-bsdf.png" <<'PY'
import sys

sys.path.insert(0, sys.argv[1])
import asset_fingerprint

def center_rgb(path):
    got = asset_fingerprint._read_image(path)
    if got is None:
        raise SystemExit("cannot decode " + path)
    w, h, rgb = got
    s = [0, 0, 0]
    n = 0
    for y in range(h // 3, 2 * h // 3):
        for x in range(w // 3, 2 * w // 3):
            i = (y * w + x) * 3
            for c in range(3):
                s[c] += rgb[i + c]
            n += 1
    return tuple(v // n for v in s)

l = center_rgb(sys.argv[2])
b = center_rgb(sys.argv[3])
print(l[0], l[1], l[2], b[0], b[1], b[2])
PY
) || {
  echo "FAIL: could not inspect rendered images"
  exit 1
}

echo "legacy RGB=$LR $LG $LB bsdf RGB=$BR $BG $BB"
if [ "$(( BR - LR ))" -lt 8 ] || [ "$(( LG - BG ))" -lt 40 ] || [ "$(( LB - BB ))" -lt 40 ]; then
  echo "FAIL: expected lightrt-bsdf sampled bounce to shift reflective surface red"
  exit 1
fi

echo "PASS: lightrt-bsdf sampled continuation affects reflective shading"
exit 0
