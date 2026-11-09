#!/usr/bin/env bash
# RAW USDA parse/write throughput benchmark (compose-free), staged in /dev/shm.
#
# Unlike bench_roundtrip_usda.sh (which flatten=compose+writes then re-parses, so
# the numbers mix compose into the load), this drives `lusdcat --rewrite-layer`
# = LoadLayerFromFile (parse only, NO composition) -> WriteLayer. It reports raw
# parse MB/s and raw write MB/s separately, both with disk I/O excluded (files
# live in tmpfs /dev/shm), and checks the idempotency oracle:
#   rewrite-layer(rewrite-layer(x)) == rewrite-layer(x)   (byte-for-byte)
#
# A big flat .usda is needed as the workload; it is produced once per scene by
# flattening the scene root with `-f` into $TMP (skipped if already present).
#
# Usage:
#   tests/next/bench_rewrite_usda.sh [--bin <lusdcat>] [--tmp <dir>]
#                                    [--threads N] [--keep] [--no-idem] [scene ...]
#
# Scenes default to: house island caldera. External scene paths come from
# HOUSE_USD, ISLAND_USD, and CALDERA_USD; none are hard-coded here.
set -u

BIN="${BIN:-build_next_thr/lusdcat}"
TMP="${TMP:-/dev/shm}"
THREADS=""          # empty = auto
KEEP=0
IDEM=1
SCENES=()

# scene -> "root_input_path" (flattened to $TMP/<scene>.usda as the workload).
declare -A SCENE_INPUT=(
  [house]="${HOUSE_USD:-}"
  [island]="${ISLAND_USD:-}"
  [caldera]="${CALDERA_USD:-}"
)

while [ $# -gt 0 ]; do
  case "$1" in
    --bin) BIN="$2"; shift 2;;
    --tmp) TMP="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --keep) KEEP=1; shift;;
    --no-idem) IDEM=0; shift;;
    *) SCENES+=("$1"); shift;;
  esac
done
[ ${#SCENES[@]} -eq 0 ] && SCENES=(house island caldera)

CAP=(systemd-run --user --scope -q -p MemoryMax=32G -p MemorySwapMax=0)
ENVV=(env LIGHTUSD_NEXT_TIMING=1)
[ -n "$THREADS" ] && ENVV+=(LIGHTUSD_NEXT_NUM_THREADS="$THREADS")

mbps() { awk -v b="$1" -v ms="$2" 'BEGIN{ if(ms>0) printf "%.0f", b/1048576/(ms/1000); else print "?"; }'; }
getms() { grep -o "$1=[0-9.]*" "$2" | head -1 | cut -d= -f2; }

echo "== RAW parse/write bench (compose-free, tmpfs=$TMP, threads=${THREADS:-auto}) =="
df -h "$TMP" | tail -1
echo

printf "%-8s | %13s | %9s | %11s | %9s | %11s | %s\n" \
  scene bytes parse_ms parse_MBps write_ms write_MBps idem
printf -- '---------+---------------+-----------+-------------+-----------+-------------+------\n'

for s in "${SCENES[@]}"; do
  inp="${SCENE_INPUT[$s]:-}"
  if [ -z "$inp" ]; then echo "skip $s: no path (set HOUSE_USD)" >&2; continue; fi
  flat="$TMP/$s.usda"
  rw="$TMP/$s.rw.usda"

  # Stage the flat workload once (compose+write; not measured here).
  if [ ! -s "$flat" ]; then
    echo "  [stage] flattening $s -> $flat ..." >&2
    "${CAP[@]}" env "$BIN" -f -o "$flat" "$inp" 2>/dev/null >/dev/null || {
      echo "skip $s: flatten failed" >&2; continue; }
  fi
  bytes=$(stat -c%s "$flat" 2>/dev/null || echo 0)

  # RAW parse + RAW write (no compose) on the flat layer.
  "${CAP[@]}" "${ENVV[@]}" "$BIN" --rewrite-layer -o "$rw" "$flat" 2>/tmp/_rw.t >/dev/null
  pms=$(getms parse /tmp/_rw.t)
  wms=$(getms write /tmp/_rw.t)

  # Idempotency oracle: re-run rewrite-layer on its own output (to stdout, piped to
  # sha256sum so no third big file is materialized) and compare.
  idem="-"
  if [ "$IDEM" = 1 ]; then
    sha_rw=$(sha256sum "$rw" | awk '{print $1}')
    sha_2=$("${CAP[@]}" env "$BIN" --rewrite-layer "$rw" 2>/dev/null | sha256sum | awk '{print $1}')
    [ "$sha_rw" = "$sha_2" ] && idem="CLEAN" || idem="DIFFERS"
  fi

  printf "%-8s | %13s | %9s | %11s | %9s | %11s | %s\n" \
    "$s" "$bytes" "${pms:-?}" "$(mbps "$bytes" "${pms:-0}")" \
    "${wms:-?}" "$(mbps "$bytes" "${wms:-0}")" "$idem"

  if [ "$KEEP" = 0 ]; then rm -f "$rw" "$flat"; fi
done
