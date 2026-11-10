#!/usr/bin/env bash
#
# Cross-backend renderer parity (§9): render each asset in lusdview's Vulkan
# raster, Vulkan ray-query, and CUDA RT backends and assert they draw the SAME
# geometry. The drawn-triangle count is a shading-independent invariant -- every
# backend rasterizes/traces the same triangulated scene, so the counts must
# agree. A backend that silently drops (or double-counts) geometry shows a
# different count. This is far more reliable than pixel comparison, which is
# confounded by legitimate cross-backend lighting/tone differences.
#
# As an INFORMATIONAL (non-gating) signal, the coarse silhouette "coverage"
# fingerprints of each backend pair are also compared and reported.
#
# Backends that are unavailable in the environment are skipped; an asset needs
# >= 2 available backends to be checked, and the whole test SKIPs (77) if no
# asset could be compared (e.g. GPU-less CI). So this is safe to run always.
#
# Exit codes: 0 = pass, 1 = parity mismatch, 77 = skip.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build/lusdview}"
FINGERPRINT_PY="$SCRIPT_DIR/asset_fingerprint.py"
MODELS_DIR="${LUSDVIEW_PARITY_MODELS:-$REPO_ROOT/models}"
SIZE="${LUSDVIEW_PARITY_SIZE:-160x160}"
FRAMES="${LUSDVIEW_PARITY_FRAMES:-3}"
COVERAGE_TOL="${LUSDVIEW_PARITY_COVERAGE_TOL:-48}"  # informational only
# Space-separated asset names (relative to MODELS_DIR) or absolute paths.
# Default: a lightweight set from the repo's test fixtures.
FILES="${LUSDVIEW_PARITY_FILES:-$REPO_ROOT/tests/usda/lusdview-shadow-alpha-inst.usda $REPO_ROOT/tests/usda/cube-001.usda}"
# Backends to compare (subset of: vk-raster vk-rt cuda-rt).
BACKENDS="${LUSDVIEW_PARITY_BACKENDS:-vk-raster vk-rt}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW (set LUSDVIEW=...)"
  exit $SKIP
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

RUN=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
  RUN=(xvfb-run -a)
fi

backend_args() {
  case "$1" in
    vk-raster) echo "--backend vk" ;;
    vk-rt)     echo "--backend vk --rt" ;;
    cuda-rt)   echo "--cuda" ;;
    *) echo "" ;;
  esac
}

# Render $asset in backend $mode. Echoes the drawn-triangle count on success, or
# one of: UNAVAIL / LOADERR / BLANK / NOSTATS. Screenshot -> $3.
render_one() {
  local mode="$1" asset="$2" shot="$3" log
  local a; a="$(backend_args "$mode")"
  # shellcheck disable=SC2086
  log="$("${RUN[@]}" "$LUSDVIEW" --headless $a --frames "$FRAMES" --size "$SIZE" \
         --screenshot "$shot" "$asset" 2>&1)"
  if echo "$log" | grep -Eiq 'CUDA ray tracing unavailable|no CUDA|NVRTC.*failed|renderer init failed: no Vulkan|no Vulkan physical device|Vulkan (backend )?unavailable|Failed to create Vulkan'; then
    echo "UNAVAIL"; return
  fi
  if echo "$log" | grep -Eiq 'load failed|Failed to load USD'; then
    echo "LOADERR"; return
  fi
  local tris
  tris="$(echo "$log" | grep -oE 'drawn tris [0-9]+' | tail -1 | grep -oE '[0-9]+')"
  if [ -z "$tris" ]; then echo "NOSTATS"; return; fi
  if [ "$tris" = "0" ]; then echo "BLANK"; return; fi
  echo "$tris"
}

fail=0
compared_any=0

for f in $FILES; do
  case "$f" in /*) asset="$f" ;; *) asset="$MODELS_DIR/$f" ;; esac
  if [ ! -f "$asset" ]; then
    echo "WARN: asset not found, skipping: $asset" >&2
    continue
  fi
  name="$(basename "$asset")"

  declare -A tris_of=()
  shots=()
  avail=()
  for mode in $BACKENDS; do
    shot="$TMP_DIR/${name}.${mode}.ppm"
    res="$(render_one "$mode" "$asset" "$shot")"
    case "$res" in
      UNAVAIL) printf '  %-9s %s: backend unavailable\n' "$mode" "$name" ;;
      LOADERR) printf '  %-9s %s: load error\n' "$mode" "$name"; fail=1 ;;
      BLANK)   printf '  %-9s %s: BLANK (0 tris)\n' "$mode" "$name"
               tris_of[$mode]=0; avail+=("$mode") ;;
      NOSTATS) printf '  %-9s %s: no render stats\n' "$mode" "$name" ;;
      *)       printf '  %-9s %s: tris=%s\n' "$mode" "$name" "$res"
               tris_of[$mode]="$res"; shots+=("$mode:$shot"); avail+=("$mode") ;;
    esac
  done

  if [ "${#avail[@]}" -lt 2 ]; then
    echo "  -> $name: only ${#avail[@]} backend(s) available; not compared"
    unset tris_of; continue
  fi
  compared_any=1

  # Gate: every available backend must draw the same triangle count.
  ref_mode="${avail[0]}"; ref="${tris_of[$ref_mode]}"
  mismatch=0
  for mode in "${avail[@]}"; do
    if [ "${tris_of[$mode]}" != "$ref" ]; then mismatch=1; fi
  done
  if [ "$mismatch" -eq 1 ]; then
    echo "  -> FAIL: $name backends disagree on drawn triangles:"
    for mode in "${avail[@]}"; do echo "       $mode = ${tris_of[$mode]}"; done
    fail=1
  else
    echo "  -> PASS: $name all ${#avail[@]} backends draw $ref triangles"
  fi

  # Informational: silhouette coverage distance between the first rendered pair.
  if [ "${#shots[@]}" -ge 2 ] && [ -f "$FINGERPRINT_PY" ] \
     && command -v python3 >/dev/null 2>&1; then
    m1="${shots[0]%%:*}"; s1="${shots[0]#*:}"
    m2="${shots[1]%%:*}"; s2="${shots[1]#*:}"
    c1="$(python3 "$FINGERPRINT_PY" coverage "$s1" 2>/dev/null)"
    c2="$(python3 "$FINGERPRINT_PY" coverage "$s2" 2>/dev/null)"
    if [ -n "$c1" ] && [ -n "$c2" ]; then
      d="$(python3 "$FINGERPRINT_PY" coverage-compare "$c1" "$c2" 0 2>/dev/null | head -1)"
      note="ok"; [ "${d:-0}" -gt "$COVERAGE_TOL" ] 2>/dev/null && note="divergent (shading/framing; informational)"
      echo "     info: $m1 vs $m2 silhouette Hamming=$d ($note)"
    fi
  fi
  unset tris_of
done

if [ "$compared_any" -eq 0 ]; then
  echo "SKIP: no asset had >= 2 available backends to compare"
  exit $SKIP
fi
if [ "$fail" -ne 0 ]; then
  echo "FAIL: cross-backend geometry parity mismatch"
  exit 1
fi
echo "PASS: cross-backend geometry parity holds"
exit 0
