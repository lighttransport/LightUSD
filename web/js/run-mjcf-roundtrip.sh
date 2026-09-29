#!/usr/bin/env bash
#
# MJCF -> USD -> MJCF roundtrip runner for the LightUSD WASM build.
#
# For each MuJoCo model it runs the two CLI legs:
#   1. forward:  cli/urdf-to-usd.js  <model.xml> --input-format mjcf  -> USD (usdc)
#   2. return:   cli/usd-to-mjcf.js  <model.usdc>                      -> MJCF
# then compares the body/joint counts that survived the trip through USD.
#
# A roundtrip PASSes when the kinematic structure is preserved:
#   forward "links"  == return "bodies"   AND   forward "joints" == return "joints"
# Mesh visual geometry is summarized and reported but is not part of the
# kinematic pass/fail assertion. This structural sweep writes small placeholder
# OBJ files so the closure remains re-parseable without render extraction.
# Use --real-meshes to exercise the full render-mesh return path.
#
# Usage:
#   web/js/run-mjcf-roundtrip.sh                  # curated representative set
#   web/js/run-mjcf-roundtrip.sh --all            # sweep every menagerie robot
#   web/js/run-mjcf-roundtrip.sh path/to/a.xml b.xml ...
#   web/js/run-mjcf-roundtrip.sh --closure        # also re-parse emitted MJCF
#   web/js/run-mjcf-roundtrip.sh --real-meshes     # export full render meshes
#   web/js/run-mjcf-roundtrip.sh --menagerie <dir> [options]
#
# Env:
#   MUJOCO_MENAGERIE (fallback for MENAGERIE_DIR) or MENAGERIE_DIR (fallback: ../.. /mujoco_menagerie)
#   OUT_DIR        (default: /tmp/mjcf-roundtrip)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_MENAGERIE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)/mujoco_menagerie"
DEFAULT_OUT_DIR="/tmp/mjcf-roundtrip"
MENAGERIE_DIR="${MENAGERIE_DIR:-${MUJOCO_MENAGERIE:-$DEFAULT_MENAGERIE_DIR}}"
OUT_DIR="${OUT_DIR:-$DEFAULT_OUT_DIR}"
FWD="$SCRIPT_DIR/cli/urdf-to-usd.js"
REV="$SCRIPT_DIR/cli/usd-to-mjcf.js"
# Raise the USDC writer's conservative WASM size caps so mesh-dense robots
# (e.g. apptronik_apollo, ~111MB USDC) export instead of hitting the 100MB cap.

CLOSURE=0
SWEEP=0
REAL_MESHES=0
EXPLICIT=()
JSON_OUT=""
MAX_USDC_MB="${MAX_USDC_MB:-2048}"
MAX_MEM_MB="${MAX_MEM_MB:-4096}"

# Curated representative set (relative to MENAGERIE_DIR): arms, quadrupeds,
# humanoids, grippers/hands, and a drone.
DEFAULT_ROBOTS=(
  universal_robots_ur5e/ur5e.xml
  franka_emika_panda/panda.xml
  franka_fr3/fr3.xml
  kuka_iiwa_14/iiwa14.xml
  trossen_vx300s/vx300s.xml
  unitree_go2/go2.xml
  anybotics_anymal_c/anymal_c.xml
  boston_dynamics_spot/spot_arm.xml
  unitree_h1/h1.xml
  unitree_g1/g1_with_hands.xml
  robotiq_2f85/2f85.xml
  shadow_hand/right_hand.xml
  wonik_allegro/right_hand.xml
  skydio_x2/x2.xml
  google_robot/robot.xml
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all) SWEEP=1; shift ;;
    --closure) CLOSURE=1; shift ;;
    --real-meshes) REAL_MESHES=1; shift ;;
    --menagerie-dir|--menagerie)
      MENAGERIE_DIR="$2"; shift 2 ;;
    --max-usdc-mb)
      MAX_USDC_MB="$2"; shift 2 ;;
    --max-mem-mb)
      MAX_MEM_MB="$2"; shift 2 ;;
    --out-dir|--out)
      OUT_DIR="$2"; shift 2 ;;
    --json)
      JSON_OUT="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,40p' "$0"; exit 0 ;;
    -*)
      echo "Unknown option: $1" >&2; exit 2 ;;
    *)
      EXPLICIT+=("$1"); shift ;;
  esac
done

command -v node >/dev/null || { echo "node not found in PATH" >&2; exit 2; }
[ -f "$FWD" ] || { echo "missing $FWD" >&2; exit 2; }
[ -f "$REV" ] || { echo "missing $REV" >&2; exit 2; }

# Pull "<N> <label>" out of a CLI summary line.
get() { echo "$2" | grep -oE "[0-9]+ $1" | head -1 | awk '{print $1}'; }

# Discover the primary MJCF for every robot directory (skip scene/keyframe/mjx
# variants); used by --all.
discover_all() {
  local dir f base cand
  for dir in "$MENAGERIE_DIR"/*/; do
    [ -d "$dir" ] || continue
    cand=""
    while IFS= read -r f; do
      base="$(basename "$f")"
      case "$base" in *scene*|*keyframe*|*mjx*|*nohand*|*_left*|*left_*) continue ;; esac
      grep -q "<worldbody" "$f" 2>/dev/null || continue
      grep -q "<body" "$f" 2>/dev/null || continue
      cand="$f"; break
    # LC_ALL=C: byte order, matching the JS discovery in
    # tests/screenshot-offscreen-batch.mjs (String.sort is code-unit order).
    # A UTF-8 locale collates punctuation differently -- it ranks "g1_mjx.xml"
    # before "g1.xml" -- so the two steps picked DIFFERENT primary models for
    # 8 Menagerie dirs, and the render sweep then failed looking for a .usdc
    # this script had never produced.
    done < <(ls "$dir"*.xml 2>/dev/null | LC_ALL=C sort)
    [ -n "$cand" ] && echo "$cand"
  done
}

# Build the work list.
MODELS=()
if [ "${#EXPLICIT[@]}" -gt 0 ]; then
  MODELS=("${EXPLICIT[@]}")
elif [ "$SWEEP" -eq 1 ]; then
  while IFS= read -r f; do MODELS+=("$f"); done < <(discover_all)
else
  for rel in "${DEFAULT_ROBOTS[@]}"; do
    [ -f "$MENAGERIE_DIR/$rel" ] && MODELS+=("$MENAGERIE_DIR/$rel")
  done
fi

if [ "${#MODELS[@]}" -eq 0 ]; then
  echo "No MJCF models to process (MENAGERIE_DIR=$MENAGERIE_DIR)." >&2
  exit 2
fi

mkdir -p "$OUT_DIR"
printf '%-26s %-22s %8s %8s %8s %8s  %s\n' "robot" "model" "links" "joints" "visuals" "collis" "result"
printf '%s\n' "--------------------------------------------------------------------------------------------"

PASS=0; FAIL=0; ERR=0
FAILED_LIST=()
JSON_ROWS=()

json_escape() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

add_json_row() {
  local robot model result links joints visuals collisions usd mjcf
  robot="$(json_escape "$1")"
  model="$(json_escape "$2")"
  result="$(json_escape "$3")"
  links="${4:-}"; joints="${5:-}"; visuals="${6:-}"; collisions="${7:-}"
  usd="$(json_escape "${8:-}")"; mjcf="$(json_escape "${9:-}")"
  JSON_ROWS+=("{\"robot\":\"$robot\",\"model\":\"$model\",\"result\":\"$result\",\"links\":\"$links\",\"joints\":\"$joints\",\"visuals\":\"$visuals\",\"collisions\":\"$collisions\",\"usd\":\"$usd\",\"mjcf\":\"$mjcf\"}")
}

run_one() {
  local mjcf="$1"
  if [ ! -f "$mjcf" ]; then
    printf '%-26s %-22s %58s\n' "?" "$mjcf" "MISSING-FILE"
    ERR=$((ERR+1)); FAILED_LIST+=("$mjcf (missing)")
    add_json_row "?" "$mjcf" "MISSING-FILE" "" "" "" "" "" ""
    return
  fi
  local name robotdir sub odir usd out_mjcf
  name="$(basename "$mjcf")"; name="${name%.*}"
  robotdir="$(dirname "$mjcf")"
  sub="$(basename "$robotdir")"
  odir="$OUT_DIR/$sub"
  usd="$odir/$name.usdc"
  out_mjcf="$odir/$name.roundtrip.mjcf"
  mkdir -p "$odir"

  # Forward: MJCF -> USD
  local fout frc fline L J V C
  fout="$(node "$FWD" "$mjcf" --input-format mjcf --format usdc -o "$usd" \
            --asset-dir "$robotdir/assets" --asset-dir "$robotdir" \
            --allow-missing --no-verify \
            --max-usdc-mb "$MAX_USDC_MB" --max-mem-mb "$MAX_MEM_MB" 2>&1)"
  frc=$?
  if [ $frc -ne 0 ]; then
    printf '%-26s %-22s %58s\n' "$sub" "$name" "FWD-ERROR"
    [ -n "${VERBOSE:-}" ] && echo "    $(echo "$fout" | tail -1)"
    ERR=$((ERR+1)); FAILED_LIST+=("$sub/$name (forward: $(echo "$fout" | tail -1))")
    add_json_row "$sub" "$name" "FWD-ERROR" "" "" "" "" "$usd" "$out_mjcf"
    return
  fi
  # The forward converter exited 0 -- but verify it actually produced the file.
  # Without this, a forward step that exits 0 without writing shows up as
  # REV-ERROR ("ENOENT ... .usdc"), blaming the return trip for the forward
  # step's failure. That misattribution cost real debugging time on an
  # intermittent all-67-models failure whose root cause is still unknown, so
  # capture the forward output here where it is still available.
  if [ ! -s "$usd" ]; then
    printf '%-26s %-22s %58s\n' "$sub" "$name" "FWD-NO-OUTPUT"
    echo "    forward exited 0 but wrote no usdc: $usd"
    echo "    forward output: $(echo "$fout" | tail -3 | tr '\n' ' ')"
    ERR=$((ERR+1))
    FAILED_LIST+=("$sub/$name (forward exited 0 but produced no output: $(echo "$fout" | tail -1))")
    add_json_row "$sub" "$name" "FWD-NO-OUTPUT" "" "" "" "" "$usd" "$out_mjcf"
    return
  fi

  fline="$(echo "$fout" | grep '^Verified')"
  L="$(get links "$fline")"; J="$(get joints "$fline")"
  V="$(get 'visual meshes' "$fline")"; C="$(get collisions "$fline")"

  # Return: USD -> MJCF
  local rout rrc rline B J2 V2 C2
  local mesh_args=(--placeholder-meshes)
  [ "$REAL_MESHES" -eq 1 ] && mesh_args=()
  rout="$(node "$REV" "$usd" -o "$out_mjcf" "${mesh_args[@]}" 2>&1)"
  rrc=$?
  if [ $rrc -ne 0 ]; then
    printf '%-26s %-22s %58s\n' "$sub" "$name" "REV-ERROR"
    [ -n "${VERBOSE:-}" ] && echo "    $(echo "$rout" | tail -1)"
    ERR=$((ERR+1)); FAILED_LIST+=("$sub/$name (return: $(echo "$rout" | tail -1))")
    add_json_row "$sub" "$name" "REV-ERROR" "$L" "$J" "$V" "$C" "$usd" "$out_mjcf"
    return
  fi
  rline="$(echo "$rout" | grep '^Wrote')"
  B="$(get bodies "$rline")"; J2="$(get joints "$rline")"
  V2="$(get visuals "$rline")"; C2="$(get collisions "$rline")"

  # Compare kinematic structure.
  local result="PASS" ok=1
  [ "${L:-x}" = "${B:-y}" ] || ok=0
  [ "${J:-x}" = "${J2:-y}" ] || ok=0

  # Optional closure: re-parse emitted MJCF through the forward leg.
  if [ "$CLOSURE" -eq 1 ] && [ "$ok" -eq 1 ]; then
    local cout crc cline B2 CJ
    cout="$(node "$FWD" "$out_mjcf" --input-format mjcf --format usdc \
              -o "$odir/$name.reparse.usdc" --allow-missing --no-verify \
              --max-usdc-mb "$MAX_USDC_MB" --max-mem-mb "$MAX_MEM_MB" 2>&1)"
    crc=$?
    if [ $crc -ne 0 ]; then ok=0; result="CLOSURE-ERR"; else
      cline="$(echo "$cout" | grep '^Verified')"
      B2="$(get links "$cline")"; CJ="$(get joints "$cline")"
      { [ "${B2:-x}" = "${B:-y}" ] && [ "${CJ:-x}" = "${J2:-y}" ]; } || { ok=0; result="CLOSURE-DIFF"; }
    fi
  fi

  if [ "$ok" -eq 1 ]; then
    [ "$result" = "PASS" ] && PASS=$((PASS+1))
  else
    [ "$result" = "PASS" ] && result="DIFF($L/$B links, $J/$J2 joints)"
    FAIL=$((FAIL+1)); FAILED_LIST+=("$sub/$name: $result")
  fi

  printf '%-26s %-22s %8s %8s %8s %8s  %s\n' \
    "$sub" "$name" "$L→$B" "$J→$J2" "$V→$V2" "$C→$C2" "$result"
  add_json_row "$sub" "$name" "$result" "$L→$B" "$J→$J2" "$V→$V2" "$C→$C2" "$usd" "$out_mjcf"
}

START=$(date +%s)
for m in "${MODELS[@]}"; do run_one "$m"; done
END=$(date +%s)

echo
echo "================ MJCF -> USD -> MJCF roundtrip summary ================"
echo "models: ${#MODELS[@]}   PASS: $PASS   FAIL: $FAIL   ERROR: $ERR   ($((END-START))s)"
if [ "${#FAILED_LIST[@]}" -gt 0 ]; then
  echo "--- not passing ---"
  for f in "${FAILED_LIST[@]}"; do echo "  - $f"; done
fi
echo "outputs under: $OUT_DIR"

if [ -n "$JSON_OUT" ]; then
  mkdir -p "$(dirname "$JSON_OUT")"
  {
    echo '{'
    echo "  \"models\": ["
    for ((i = 0; i < ${#JSON_ROWS[@]}; i++)); do
      if [ "$i" -lt $((${#JSON_ROWS[@]} - 1)) ]; then
        echo "    ${JSON_ROWS[$i]},"
      else
        echo "    ${JSON_ROWS[$i]}"
      fi
    done
    echo '  ],'
    echo "  \"counts\": {\"models\": ${#MODELS[@]}, \"pass\": $PASS, \"fail\": $FAIL, \"error\": $ERR}"
    echo '}'
  } > "$JSON_OUT"
  echo "json summary: $JSON_OUT"
fi

[ "$FAIL" -eq 0 ] && [ "$ERR" -eq 0 ]
