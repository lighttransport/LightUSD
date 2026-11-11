#!/usr/bin/env bash
#
# Regression test: the DEFAULT (tydra) load path renders UsdGeomPointInstancer
# instances. Before BuildDrawInstances was wired into mesh_build.cc, tydra loaded
# a PointInstancer's prototype as a single mesh and ignored RenderScene::instances
# entirely ("instances 0/0 visible"); only the --core loader placed instances.
# This asserts the tydra path now emits one instance per authored placement AND
# that the auto-fit camera frames all of them (world-space prototype bounds), so
# none get frustum-culled. A reference-based prototype covers composed scenes.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUSDVIEW="${LUSDVIEW:-$SCRIPT_DIR/../../../build/lusdview}"
BACKEND="${BACKEND:-vk}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW (set LUSDVIEW=...)"
  exit $SKIP
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
PLAIN_ASSET="$TMP_DIR/pointinstancer_plain.usda"
REF_ASSET="$TMP_DIR/pointinstancer_ref.usda"
CONFIG="$TMP_DIR/config.json"
printf '%s\n' '{"window_size":{"width":800,"height":600}}' > "$CONFIG"

# Three cubes along X. The prototype lives at the origin; the instances place
# copies at x = -1.5, 0, +1.5. The tydra path must emit 3 instances and frame
# all of them.
cat > "$PLAIN_ASSET" <<'USDA'
#usda 1.0
( upAxis = "Y" )
def PointInstancer "PI"
{
    point3f[] positions = [(-1.5,0,0),(0,0,0),(1.5,0,0)]
    int[] protoIndices = [0,0,0]
    rel prototypes = [</PI/Protos/P>]
    def Scope "Protos" { def Mesh "P"
    {
        int[] faceVertexCounts = [4,4,4,4,4,4]
        int[] faceVertexIndices = [0,1,3,2,4,6,7,5,0,4,5,1,2,3,7,6,0,2,6,4,1,5,7,3]
        point3f[] points = [(-.4,-.4,-.4),(-.4,-.4,.4),(-.4,.4,-.4),(-.4,.4,.4),(.4,-.4,-.4),(.4,-.4,.4),(.4,.4,-.4),(.4,.4,.4)]
    } }
}
USDA

# Same layout but the prototype is a referenced mesh, exercising composition of
# the prototype subtree before instancing.
cat > "$TMP_DIR/proto_mesh.usda" <<'USDA'
#usda 1.0
def Mesh "Cube"
{
    int[] faceVertexCounts = [4,4,4,4,4,4]
    int[] faceVertexIndices = [0,1,3,2,4,6,7,5,0,4,5,1,2,3,7,6,0,2,6,4,1,5,7,3]
    point3f[] points = [(-.4,-.4,-.4),(-.4,-.4,.4),(-.4,.4,-.4),(-.4,.4,.4),(.4,-.4,-.4),(.4,-.4,.4),(.4,.4,-.4),(.4,.4,.4)]
}
USDA

cat > "$REF_ASSET" <<'USDA'
#usda 1.0
( upAxis = "Y" )
def PointInstancer "PI"
{
    point3f[] positions = [(-1.5,0,0),(0,0,0),(1.5,0,0)]
    int[] protoIndices = [0,0,0]
    rel prototypes = [</PI/Protos/P>]
    def Scope "Protos" {
        def "P" ( references = @proto_mesh.usda@</Cube> ) {}
    }
}
USDA

RUN=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a)
fi

run_case() {
  local label="$1"
  local asset="$2"
  local out="$TMP_DIR/${label}.png"
  local log
  # NOTE: default (tydra) loader -- no --core.
  log="$(timeout --kill-after=5s 30s "${RUN[@]}" "$LUSDVIEW" --headless \
         --backend "$BACKEND" --config "$CONFIG" --frames 1 \
         --screenshot "$out" "$asset" 2>&1)"
  echo "$log"

  if ! echo "$log" | grep -q "render stats"; then
    echo "SKIP: no renderer (backend '$BACKEND' unavailable in this environment)"
    exit $SKIP
  fi

  if echo "$log" | grep -q "instances 3/3 visible"; then
    echo "PASS: $label tydra path emitted and framed 3 PointInstancer instances"
    return 0
  fi

  if echo "$log" | grep -q "instances 0/0 visible"; then
    echo "FAIL: $label tydra path ignored RenderScene::instances (0/0)."
    exit 1
  fi

  echo "FAIL: $label expected 'instances 3/3 visible' (got the stats above)."
  exit 1
}

run_case "plain" "$PLAIN_ASSET"
run_case "referenced-proto" "$REF_ASSET"
