#!/usr/bin/env bash
#
# lusdrender core-loader degraded-material regression. The shared Tydra material
# resolver must keep the shared converter's per-material degraded surface for an
# unsupported shader, and the degradation must be reported in the load summary.
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
ASSET="$TMP/degraded_material.usda"
OUT="$TMP/degraded_material.png"
LOG="$TMP/degraded_material.log"

cat > "$ASSET" <<'USDA'
#usda 1.0
( upAxis = "Y" )
def Xform "World" {
  def Mesh "M" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/Mats/Broken>
  }
  def Scope "Mats" {
    def Material "Broken" {
      token outputs:surface.connect = </World/Mats/Broken/S.outputs:surface>
      def Shader "S" {
        uniform token info:id = "SomeUnknownShaderType_xyz"
        color3f inputs:baseColor = (0.8, 0.1, 0.05)
        float inputs:roughness = 0.25
        token outputs:surface
      }
    }
  }
}
USDA

"$LUSDRENDER" "$ASSET" "$OUT" -rtPreview --materialResolver tydra \
  -w 64 -height 64 -autoframe -viewDir 0,0,-1 -ambient 1 -noShadows \
  -samples 1 -stats >"$LOG" 2>&1
rc=$?
cat "$LOG"
if [ "$rc" -ne 0 ]; then
  echo "FAIL: lusdrender exited with $rc"
  exit 1
fi
if [ ! -s "$OUT" ]; then
  echo "FAIL: lusdrender produced no image"
  exit 1
fi
if ! grep -Eq '^triangles: 2$' "$LOG"; then
  echo "FAIL: expected quad to render as two triangles"
  exit 1
fi
if ! grep -Eq 'load summary:.*degraded_materials=[1-9]' "$LOG"; then
  echo "FAIL: degraded material was not reported in the load summary"
  exit 1
fi
if ! grep -Fq '/World/Mats/Broken/S' "$LOG" ||
   ! grep -Fq 'SomeUnknownShaderType_xyz' "$LOG"; then
  echo "FAIL: degraded-material diagnostic omitted its shader path or ID"
  exit 1
fi
if grep -q 'using legacy resolver' "$LOG"; then
  echo "FAIL: degraded shared material was discarded for the legacy resolver"
  exit 1
fi

echo "PASS: lusdrender keeps and reports the shared degraded material"
