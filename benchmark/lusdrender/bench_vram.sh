#!/usr/bin/env bash
#
# bench_vram.sh — measure one render command's peak VRAM, wall time and
# harness timings, with an optional screenshot sanity check.
#
# Peak VRAM is measured as (max GPU memory.used while the command runs) minus
# an idle baseline sampled just before launch, polled via nvidia-smi. This is
# a whole-GPU number (per-process VRAM is not reliably reported for Vulkan),
# so keep the desktop quiet during a measurement run.
#
# Usage:
#   benchmark/lusdrender/bench_vram.sh [options] -- COMMAND [ARGS...]
#
# Options:
#   -l LABEL    label for the report line (default: first word of COMMAND)
#   -i IMAGE    screenshot the command writes; checked non-blank after the run
#   -g GOLDEN   reference image; report RMSE(IMAGE, GOLDEN) and fail if > -e
#   -e RMSE     max allowed RMSE vs GOLDEN in 8-bit units (default 1.0)
#   -n FRAC     min non-black pixel fraction for IMAGE (default 0.01)
#   -o OUTDIR   where to keep the log/samples (default: mktemp -d)
#   -k          keep OUTDIR (default: removed unless the run fails)
#
# Environment of the target command is passed through, so wrap with e.g.
#   LUSDVIEW_TIME_PRESENT=1 LUSDVIEW_RT_TIMING=1 benchmark/lusdrender/bench_vram.sh ...
# to get the renderer's own stage timings echoed into the report.
#
# Exit code: the command's exit code, or 1 if an image check fails.
#
# Examples:
#   benchmark/lusdrender/bench_vram.sh -l island-rt -i /tmp/out.ppm -- \
#     ./build/lusdview --headless --core --rt --rt-lod --camera shotCam \
#       --frames 8 --screenshot /tmp/out.ppm /mnt/disk1/data/island/usd/island.usda
#
set -uo pipefail

LABEL=""
IMAGE=""
GOLDEN=""
MAX_RMSE="1.0"
MIN_NONBLACK="0.01"
OUTDIR=""
KEEP=0

while getopts "l:i:g:e:n:o:k" opt; do
  case "$opt" in
    l) LABEL="$OPTARG" ;;
    i) IMAGE="$OPTARG" ;;
    g) GOLDEN="$OPTARG" ;;
    e) MAX_RMSE="$OPTARG" ;;
    n) MIN_NONBLACK="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    k) KEEP=1 ;;
    *) echo "usage: bench_vram.sh [-l label] [-i image] [-g golden] [-e rmse] [-n frac] [-o outdir] [-k] -- cmd..." >&2
       exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ "${1:-}" = "--" ] && shift
if [ $# -eq 0 ]; then
  echo "bench_vram.sh: no command given (use -- CMD...)" >&2
  exit 2
fi
[ -z "$LABEL" ] && LABEL="$(basename "$1")"

if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "bench_vram.sh: nvidia-smi not found; VRAM will be reported as n/a" >&2
  HAVE_SMI=0
else
  HAVE_SMI=1
fi

if [ -z "$OUTDIR" ]; then
  OUTDIR="$(mktemp -d -t bench_vram.XXXXXX)"
fi
mkdir -p "$OUTDIR"
LOG="$OUTDIR/run.log"
SAMPLES="$OUTDIR/vram_samples.txt"

# --- idle baseline: min of 3 samples 200 ms apart -------------------------
BASELINE="n/a"
if [ "$HAVE_SMI" -eq 1 ]; then
  BASELINE=999999999
  for _ in 1 2 3; do
    v="$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1 | tr -d ' ')"
    case "$v" in (*[!0-9]*|"") v="" ;; esac
    if [ -n "$v" ] && [ "$v" -lt "$BASELINE" ]; then BASELINE="$v"; fi
    sleep 0.2
  done
  [ "$BASELINE" = 999999999 ] && BASELINE="n/a"
fi

# --- background poller ----------------------------------------------------
POLL_PID=""
if [ "$HAVE_SMI" -eq 1 ]; then
  nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -lms 100 \
    > "$SAMPLES" 2>/dev/null &
  POLL_PID=$!
fi
stop_poller() {
  if [ -n "$POLL_PID" ]; then kill "$POLL_PID" 2>/dev/null; wait "$POLL_PID" 2>/dev/null; POLL_PID=""; fi
}
trap stop_poller EXIT

# --- run ------------------------------------------------------------------
START_NS="$(date +%s%N)"
"$@" > "$LOG" 2>&1
RC=$?
END_NS="$(date +%s%N)"
stop_poller
WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

# --- peak VRAM ------------------------------------------------------------
PEAK="n/a"; DELTA="n/a"
if [ "$HAVE_SMI" -eq 1 ] && [ -s "$SAMPLES" ]; then
  PEAK="$(tr -d ' ' < "$SAMPLES" | grep -E '^[0-9]+$' | sort -n | tail -1)"
  if [ -n "$PEAK" ] && [ "$BASELINE" != "n/a" ]; then
    DELTA=$(( PEAK - BASELINE ))
  fi
fi

# --- image checks ---------------------------------------------------------
IMG_STATUS="skipped"
IMG_DETAIL=""
if [ -n "$IMAGE" ]; then
  IMG_STATUS="fail"
  if [ ! -s "$IMAGE" ]; then
    IMG_DETAIL="missing or empty: $IMAGE"
  else
    IMG_OUT="$(python3 - "$IMAGE" "${GOLDEN:-}" "$MIN_NONBLACK" "$MAX_RMSE" <<'PYEOF'
import subprocess, sys, os, struct, tempfile

def load(path):
    """Return (w, h, bytes RGB8). PPM natively; anything else via ImageMagick."""
    if not path.lower().endswith((".ppm", ".pnm")):
        for conv in (["magick", path, "ppm:-"], ["convert", path, "ppm:-"]):
            try:
                data = subprocess.run(conv, capture_output=True, check=True).stdout
                return parse_ppm(data)
            except (FileNotFoundError, subprocess.CalledProcessError):
                continue
        return None
    with open(path, "rb") as f:
        return parse_ppm(f.read())

def parse_ppm(data):
    # P6 with whitespace/comments between header tokens.
    if not data.startswith(b"P6"):
        return None
    toks, i = [], 2
    while len(toks) < 3:
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i] != 0x0A: i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace(): j += 1
        toks.append(int(data[i:j])); i = j
    i += 1  # single whitespace after maxval
    w, h, maxval = toks
    px = data[i:i + w*h*3]
    if maxval != 255 or len(px) < w*h*3:
        return None
    return (w, h, px)

img_path, golden_path, min_nonblack, max_rmse = (
    sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]))
img = load(img_path)
if img is None:
    print("ERR cannot decode", img_path); sys.exit(3)
w, h, px = img
n = w * h
nonblack = 0
total = 0
# treat a pixel as non-black when any channel > 8/255
for i in range(0, n*3, 3):
    v = max(px[i], px[i+1], px[i+2])
    total += px[i] + px[i+1] + px[i+2]
    if v > 8:
        nonblack += 1
frac = nonblack / max(1, n)
mean = total / max(1, n*3)
line = f"size={w}x{h} nonblack={frac:.4f} mean={mean:.2f}"
ok = frac >= min_nonblack
if golden_path:
    g = load(golden_path)
    if g is None:
        print(line, "ERR cannot decode golden"); sys.exit(3)
    gw, gh, gpx = g
    if (gw, gh) != (w, h):
        print(line, f"RMSE=n/a (size mismatch golden {gw}x{gh})"); sys.exit(4)
    acc = 0
    for a, b in zip(px, gpx):
        d = a - b
        acc += d*d
    rmse = (acc / (n*3)) ** 0.5
    line += f" rmse={rmse:.4f}"
    ok = ok and rmse <= max_rmse
print(line)
sys.exit(0 if ok else 4)
PYEOF
)"
    PYRC=$?
    IMG_DETAIL="$IMG_OUT"
    if [ "$PYRC" -eq 0 ]; then IMG_STATUS="ok"; fi
  fi
fi

# --- report ---------------------------------------------------------------
echo "==== bench_vram: $LABEL ===="
echo "cmd      : $*"
echo "exit     : $RC"
echo "wall     : ${WALL_MS} ms"
if [ "$HAVE_SMI" -eq 1 ]; then
  echo "vram     : peak ${PEAK} MiB, idle ${BASELINE} MiB, delta ${DELTA} MiB"
else
  echo "vram     : n/a (no nvidia-smi)"
fi
if [ -n "$IMAGE" ]; then
  echo "image    : $IMG_STATUS  $IMG_DETAIL"
fi
# Echo the renderer's own stage timings / stats for the record.
grep -E '\[(time|rt|rt-lod|mdi|lod|gpu|upload|present)[] ]|backend: LightRT|render-stats|instances .* visible' \
  "$LOG" | tail -40 | sed 's/^/log      : /'
echo "log file : $LOG"

FINAL=$RC
if [ -n "$IMAGE" ] && [ "$IMG_STATUS" != "ok" ] && [ "$FINAL" -eq 0 ]; then
  FINAL=1
fi
# OUTDIR (log + samples) is always kept: -k is accepted for symmetry but logs
# are cheap and every before/after table wants them.
exit "$FINAL"
