#!/usr/bin/env bash
#
# Regression test: PointInstancer invisibleIds/inactiveIds are authored ids when
# the optional `ids` array exists. Index-only matching incorrectly keeps all four
# instances below visible because invisibleIds=[30] and inactiveIds=[40] do not
# equal any array index. The correct result emits two instances. The second
# scene repeats the same visibility setup in a nested PointInstancer under an
# outer prototype, covering lusdview's recursive instancer expansion.
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

# Pin the window size. Without it the window falls back to whatever the display
# offers, and under xvfb-run that leaves a ~32px-wide 3D viewport once the
# docked panels take their share -- narrow enough that both instances fall
# outside the frustum and are (correctly) culled, so the stat this test asserts
# reflects the window geometry rather than the id filtering under test.
CONFIG="$TMP_DIR/config.json"
printf '%s\n' '{"window_size":{"width":800,"height":600}}' > "$CONFIG"
TOP_ASSET="$TMP_DIR/pointinstancer_invisible_ids.usda"
NESTED_ASSET="$TMP_DIR/nested_pointinstancer_invisible_ids.usda"

cat > "$TOP_ASSET" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Z"
)

def Xform "World"
{
    def PointInstancer "PI"
    {
        int64[] ids = [10, 20, 30, 40]
        int64[] invisibleIds = [30]
        int64[] inactiveIds = [40]
        point3f[] positions = [(-1.5, 0, 0), (-0.5, 0, 0), (0.5, 0, 0), (1.5, 0, 0)]
        int[] protoIndices = [0, 0, 0, 0]
        rel prototypes = [</World/PI/Proto>]

        def Mesh "Proto"
        {
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            int[] faceVertexIndices = [0,1,3,2, 2,3,5,4, 4,5,7,6, 6,7,1,0, 1,7,5,3, 6,0,2,4]
            point3f[] points = [(-0.2,-0.2,-0.2),(-0.2,-0.2,0.2),(-0.2,0.2,-0.2),(-0.2,0.2,0.2),(0.2,0.2,-0.2),(0.2,0.2,0.2),(0.2,-0.2,-0.2),(0.2,-0.2,0.2)]
        }
    }
}
USDA

cat > "$NESTED_ASSET" <<'USDA'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Z"
)

def Xform "World"
{
    def PointInstancer "Outer"
    {
        point3f[] positions = [(0, 0, 0)]
        int[] protoIndices = [0]
        rel prototypes = [</World/Outer/ProtoRoot>]

        def Xform "ProtoRoot"
        {
            def PointInstancer "Inner"
            {
                int64[] ids = [10, 20, 30, 40]
                int64[] invisibleIds = [30]
                int64[] inactiveIds = [40]
                point3f[] positions = [(-1.5, 0, 0), (-0.5, 0, 0), (0.5, 0, 0), (1.5, 0, 0)]
                int[] protoIndices = [0, 0, 0, 0]
                rel prototypes = [</World/Outer/ProtoRoot/Inner/Proto>]

                def Mesh "Proto"
                {
                    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
                    int[] faceVertexIndices = [0,1,3,2, 2,3,5,4, 4,5,7,6, 6,7,1,0, 1,7,5,3, 6,0,2,4]
                    point3f[] points = [(-0.2,-0.2,-0.2),(-0.2,-0.2,0.2),(-0.2,0.2,-0.2),(-0.2,0.2,0.2),(0.2,0.2,-0.2),(0.2,0.2,0.2),(0.2,-0.2,-0.2),(0.2,-0.2,0.2)]
                }
            }
        }
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
  log="$(timeout --kill-after=5s 30s "${RUN[@]}" "$LUSDVIEW" --headless \
         --backend "$BACKEND" --core --config "$CONFIG" --frames 1 \
         --screenshot "$out" "$asset" 2>&1)"
  echo "$log"

  if ! echo "$log" | grep -q "render stats"; then
    echo "SKIP: no renderer (backend '$BACKEND' unavailable in this environment)"
    exit $SKIP
  fi

  if echo "$log" | grep -q "instances 2/2 visible"; then
    echo "PASS: $label hidden ids matched authored ids; emitted 2 visible instances"
    return 0
  fi

  if echo "$log" | grep -q "instances 4/4 visible"; then
    echo "FAIL: $label hidden ids were matched as array indices instead of authored ids."
    exit 1
  fi

  echo "FAIL: $label expected 'instances 2/2 visible'."
  exit 1
}

run_case "top-level" "$TOP_ASSET"
run_case "nested" "$NESTED_ASSET"
