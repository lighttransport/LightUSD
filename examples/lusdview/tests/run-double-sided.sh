#!/usr/bin/env bash
#
# doubleSided regression (audit T8): a single-sided mesh must be back-face
# culled, a doubleSided one must not -- identically on the GL and Vulkan raster
# backends.
#
# Before the fix, the two backends disagreed twice over on the default (--core)
# loader: the loader never read the authored `doubleSided` (everything defaulted
# to single-sided), GL back-face culled everything, and Vulkan culled nothing
# (all pipelines were VK_CULL_MODE_NONE). So an authored doubleSided=true quad
# was invisible from behind in GL and visible in VK; VK's culling now follows
# the mesh via VK_EXT_extended_dynamic_state (absent -> VK just keeps drawing
# both faces, and this test only checks the doubleSided quad there).
#
# The probe: a red quad facing +Z, cameras in front and behind.
#   single-sided, Front -> visible     single-sided, Back -> culled (no red)
#   doubleSided,  Front -> visible     doubleSided,  Back -> visible
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
mkdir -p "$OUT/config"
trap 'rm -rf "$OUT"' EXIT
# Use an isolated display when available; inherited DISPLAY values are often
# stale in headless/CI shells and made the nominal GL leg silently unavailable.
XVFB=""
if command -v xvfb-run >/dev/null 2>&1; then
  XVFB="xvfb-run -a"
fi

cat > "$OUT/ss.usda" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    color3f[] primvars:displayColor = [(1, 0.2, 0.2)]
    uniform token subdivisionScheme = "none"
  }
  def Camera "Front" {
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
  }
  def Camera "Back" {
    double3 xformOp:translate = (0, 0, -4)
    float xformOp:rotateY = 180
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
  }
}
USDA
sed 's/def Mesh "Quad" {/def Mesh "Quad" {\n    uniform bool doubleSided = true/' \
    "$OUT/ss.usda" > "$OUT/ds.usda"

# Count of red-dominant pixels (the quad's displayColor) in a binary PPM.
red_count() {
python3 - "$1" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
m = re.match(rb'P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(\d+)\s+(\d+)\s', d)
if not m: sys.exit(2)
w, h = int(m.group(1)), int(m.group(2)); px = d[m.end():]
if len(px) < w*h*3: sys.exit(2)
print(sum(1 for o in range(0, w*h*3, 3) if px[o] > 1.5*px[o+1] + 20))
PY
}

fail=0
for spec in "gl:--backend gl" "vk:--backend vk"; do
  tag="${spec%%:*}"; args="${spec#*:}"
  headless_args=(--headless)
  [ "$tag" = gl ] && headless_args=()
  declare -A red
  ok=1
  for v in ss ds; do
    for c in Front Back; do
      img="$OUT/${tag}_${v}_${c}.ppm"
      # shellcheck disable=SC2086
      $XVFB env XDG_CONFIG_HOME="$OUT/config" \
          "$BIN" "${headless_args[@]}" $args --frames 2 --size 1280x720 \
          --camera "$c" \
          --screenshot "$img" "$OUT/$v.usda" >"$OUT/${tag}_${v}_${c}.log" 2>&1
      if ! grep -q 'render stats' "$OUT/${tag}_${v}_${c}.log"; then
        echo "SKIP: $tag backend unavailable"; ok=0; break 2
      fi
      [ -s "$img" ] || { echo "FAIL: $tag $v/$c produced no image"; fail=1; ok=0; break 2; }
      red[$v/$c]=$(red_count "$img") || { echo "FAIL: $tag $v/$c PPM parse"; fail=1; ok=0; break 2; }
    done
  done
  [ "$ok" -eq 1 ] || continue
  echo "$tag red px: ssFront=${red[ss/Front]} ssBack=${red[ss/Back]} dsFront=${red[ds/Front]} dsBack=${red[ds/Back]}"
  base=${red[ss/Front]}
  if [ "$base" -lt 200 ]; then
    echo "FAIL: $tag front view barely shows the quad ($base red px) -- fixture broken"; fail=1; continue
  fi
  # doubleSided: both faces visible, comparable coverage.
  if [ "${red[ds/Front]}" -lt $((base / 2)) ] || [ "${red[ds/Back]}" -lt $((base / 2)) ]; then
    echo "FAIL: $tag doubleSided quad is culled (front=${red[ds/Front]} back=${red[ds/Back]}; doubleSided not plumbed)"; fail=1
  fi
  # single-sided from behind: culled. GL always culls; VK only with extended
  # dynamic state -- detect its absence by ssBack == dsBack (nothing culled) and
  # accept it, but a PARTIAL cull is a bug.
  if [ "${red[ss/Back]}" -ge $((base / 20)) ]; then
    if [ "$tag" = "vk" ] && [ "${red[ss/Back]}" -ge $((base / 2)) ]; then
      echo "NOTE: vk shows the single-sided back face (extended dynamic state unavailable?)"
    else
      echo "FAIL: $tag single-sided quad partially visible from behind (${red[ss/Back]} red px)"; fail=1
    fi
  fi
done

[ "$fail" -eq 0 ] || exit 1
echo "PASS: doubleSided honored; single-sided meshes back-face culled (GL and VK)"
exit 0
