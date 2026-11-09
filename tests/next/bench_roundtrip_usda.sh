#!/usr/bin/env bash
# Flattened-USDA roundtrip throughput benchmark.
#
# For each scene: flatten -> write a big .usda to $TMPDIR (write MB/s), then
# re-parse it with `lusdcat -l` (read MB/s). Verifies the flatten output
# sha256 (byte-identity oracle) and, with --roundtrip, that re-flattening the
# flat file reproduces it byte-for-byte.
#
# Usage:
#   tests/next/bench_roundtrip_usda.sh [--bin <lusdcat>] [--tmp <dir>]
#                                      [--threads N] [--use-o] [--roundtrip]
#                                      [--keep] [scene ...]
#
# Scenes default to: house island caldera (in that order). Override by listing
# scene names. Paths/expected-shas are in the table below.
set -u

BIN="${BIN:-build_next_thr/lusdcat}"
TMP="${TMP:-${TMPDIR:-/tmp}/lightusd-next-bench}"
THREADS=""          # empty = auto
USE_O=0             # 1 = use `-o <file>` (FdSink) instead of stdout redirect
ROUNDTRIP=0
KEEP=0
SCENES=()

# scene -> "input_path|expected_sha (or - to skip check)".
# Public scenes have fixed paths; a private scene is supplied via the env var
# below (path + optional sha), so no private asset path lives in this file.
declare -A SCENE_INPUT=(
  [house]="${HOUSE_USD:-}|${HOUSE_SHA:--}"
  [island]="${ISLAND_USD:-}|${ISLAND_SHA:--}"
  [caldera]="${CALDERA_USD:-}|${CALDERA_SHA:--}"
)

while [ $# -gt 0 ]; do
  case "$1" in
    --bin) BIN="$2"; shift 2;;
    --tmp) TMP="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --use-o) USE_O=1; shift;;
    --roundtrip) ROUNDTRIP=1; shift;;
    --keep) KEEP=1; shift;;
    *) SCENES+=("$1"); shift;;
  esac
done
[ ${#SCENES[@]} -eq 0 ] && SCENES=(house island caldera)

CAP=(systemd-run --user --scope -q -p MemoryMax=32G -p MemorySwapMax=0)
ENVV=(env LIGHTUSD_NEXT_TIMING=1)
[ -n "$THREADS" ] && ENVV+=(LIGHTUSD_NEXT_NUM_THREADS="$THREADS")

mbps() { awk -v b="$1" -v ms="$2" 'BEGIN{ if(ms>0) printf "%.0f", b/1048576/(ms/1000); else print "?"; }'; }
getms() { grep -o "$1=[0-9.]*" "$2" | head -1 | cut -d= -f2; }

# Disk bandwidth sanity (write is direct; read may be cached).
echo "== disk bandwidth sanity ($TMP) =="
dd if=/dev/zero of="$TMP/.bwtest" bs=1M count=1024 oflag=direct 2>&1 | tail -1
rm -f "$TMP/.bwtest"
echo

printf "%-8s | %13s | %10s | %10s | %10s | %10s | %s\n" \
  scene bytes write_ms write_MBps load_ms read_MBps sha_ok
printf -- '---------+---------------+------------+------------+------------+------------+-------\n'

for s in "${SCENES[@]}"; do
  spec="${SCENE_INPUT[$s]:-}"
  if [ -z "$spec" ]; then echo "unknown scene: $s" >&2; continue; fi
  inp="${spec%%|*}"; want="${spec##*|}"
  # Skip a scene with no configured path (e.g. the private one when HOUSE_USD is unset).
  if [ -z "$inp" ]; then echo "skip $s: no path (set HOUSE_USD)" >&2; continue; fi
  out="$TMP/$s.usda"

  # flatten -> file
  if [ "$USE_O" = 1 ]; then
    "${CAP[@]}" "${ENVV[@]}" "$BIN" -f -o "$out" "$inp" 2>/tmp/_bw.t >/dev/null
  else
    "${CAP[@]}" "${ENVV[@]}" "$BIN" -f "$inp" 2>/tmp/_bw.t > "$out"
  fi
  bytes=$(stat -c%s "$out" 2>/dev/null || echo 0)
  wms=$(getms write /tmp/_bw.t)
  sha=$(sha256sum "$out" | awk '{print $1}')
  shaok="OK"; [ "$want" != "-" ] && [ "$sha" != "$want" ] && shaok="MISMATCH"

  # re-parse
  "${CAP[@]}" "${ENVV[@]}" "$BIN" -l "$out" 2>/tmp/_br.t >/dev/null
  lms=$(getms load /tmp/_br.t)

  printf "%-8s | %13s | %10s | %10s | %10s | %10s | %s\n" \
    "$s" "$bytes" "${wms:-?}" "$(mbps "$bytes" "${wms:-0}")" "${lms:-?}" "$(mbps "$bytes" "${lms:-0}")" "$shaok"

  if [ "$ROUNDTRIP" = 1 ]; then
    "${CAP[@]}" "${ENVV[@]}" "$BIN" -f "$out" 2>/dev/null > "$TMP/${s}_rt.usda"
    if cmp -s "$out" "$TMP/${s}_rt.usda"; then echo "         roundtrip: CLEAN"; else echo "         roundtrip: DIFFERS"; fi
    rm -f "$TMP/${s}_rt.usda"
  fi
  [ "$KEEP" = 0 ] && rm -f "$out"
done
