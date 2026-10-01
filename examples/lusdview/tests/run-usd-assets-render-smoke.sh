#!/usr/bin/env bash
#
# Batch smoke render USD-family assets with lusdview.
#
# This is intentionally external-data gated: set USD_ASSETS_ROOT to a local
# usd-wg/assets checkout or another USD asset suite. Without it, the script
# SKIPs so normal ctest runs do not depend on large assets.
#
# Each (mode, asset) is classified into a bucket: load_error, timeout,
# backend_unavailable, backend_error, no_renderable, rendered_with_warnings,
# rendered, or golden_mismatch. Results are written as TSV (results.tsv) and
# JSON (results.json). With a golden baseline (--golden / LUSDVIEW_USD_ASSETS_
# GOLDEN), each successful render is fingerprinted and compared, catching gross
# visual regressions the non-blank check cannot. The default hash is a coarse
# color grid; --golden-kind coverage records a silhouette bitfield that is more
# portable across GPU/CPU lighting differences. Record/refresh a baseline with
# --update-golden.
#
# Backends:
#   gl-raster : lusdview --backend gl (X11/Xvfb)
#   vk-raster : lusdview --headless --backend vk
#   vk-rt     : lusdview --headless --backend vk --rt
#   cuda-rt   : lusdview --headless --cuda
#   hip-rt    : lusdview --headless --hip
#   lusdr-cpu : lusdrender -rtPreview
#   lusdr-vk  : lusdrender -vk
#   lusdr-vkr : lusdrender -vkr
#
# Useful NVIDIA hybrid/offload env:
#   LUSDVIEW_NVIDIA_OFFLOAD=1 LUSDVIEW_XVFB=1 LUSDVIEW_VK_DEVICE=nvidia \
#     USD_ASSETS_ROOT=/path/to/assets examples/lusdview/tests/run-usd-assets-render-smoke.sh
#
# Exit codes: 0 = pass, 1 = one or more hard failures, 77 = skip.
set -uo pipefail

SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ -n "${LUSDVIEW:-}" ]; then
  LUSDVIEW_BIN="$LUSDVIEW"
elif [ -x "$REPO_ROOT/build_ninja/lusdview" ]; then
  LUSDVIEW_BIN="$REPO_ROOT/build_ninja/lusdview"
else
  LUSDVIEW_BIN="$REPO_ROOT/build/lusdview"
fi
if [ -n "${LUSDRENDER:-}" ]; then
  LUSDRENDER_BIN="$LUSDRENDER"
elif [ -x "$REPO_ROOT/build_ninja/tools/lusdrender/lusdrender" ]; then
  LUSDRENDER_BIN="$REPO_ROOT/build_ninja/tools/lusdrender/lusdrender"
else
  LUSDRENDER_BIN="$REPO_ROOT/build/tools/lusdrender/lusdrender"
fi

USD_ASSETS_ROOT="${USD_ASSETS_ROOT:-$REPO_ROOT/usd-assets}"
if [ -d "$USD_ASSETS_ROOT" ]; then
  USD_ASSETS_ROOT="$(cd "$USD_ASSETS_ROOT" && pwd -P)"
fi
OUT_DIR="${LUSDVIEW_USD_ASSETS_OUT:-}"
LIMIT="${LUSDVIEW_USD_ASSETS_LIMIT:-0}"
PROFILE="${LUSDVIEW_USD_ASSETS_PROFILE:-all}"
REQUIRE_PROFILE_COMPLETE="${LUSDVIEW_USD_ASSETS_REQUIRE_PROFILE_COMPLETE:-0}"
FRAMES="${LUSDVIEW_USD_ASSETS_FRAMES:-4}"
SIZE="${LUSDVIEW_USD_ASSETS_SIZE:-256x256}"
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
TIMEOUT_DUR="${LUSDVIEW_USD_ASSETS_TIMEOUT:-45s}"
TIME_CODE="${LUSDVIEW_USD_ASSETS_TIME:-}"
MODES="${LUSDVIEW_USD_ASSETS_MODES:-vk-raster,vk-rt,cuda-rt,lusdr-cpu,lusdr-vk,lusdr-vkr}"
VK_DEVICE="${LUSDVIEW_VK_DEVICE:-}"
# Permit '..' (parent-directory) segments in composition asset paths for the
# lusdview modes. lusdview defaults this OFF (security policy); lusdrender's
# next path allows parent refs by default. External asset suites (e.g.
# usd-wg/assets) routinely reference `../_common/*.usda`, so enable this to
# exercise them fairly. Off by default here to preserve the strict default.
ALLOW_PARENT="${LUSDVIEW_USD_ASSETS_ALLOW_PARENT:-0}"
NVIDIA_OFFLOAD="${LUSDVIEW_NVIDIA_OFFLOAD:-auto}"
USE_XVFB="${LUSDVIEW_XVFB:-auto}"
FAIL_ON="${LUSDVIEW_USD_ASSETS_FAIL_ON:-load_error,timeout,backend_error}"
# CPU texture quality for the lusdview modes. lusdview now skips the CPU
# mip-generation + BCn-compression pipeline by default whenever the decoded
# textures fit within half the device VRAM (OpenChessSet's 640 MB is ~1/16 of a
# 16 GB card), so ordinary smoke scenes skip conversion automatically. This knob
# forces the choice regardless of that default:
#   full         lusdview default (skip conversion when textures fit in VRAM/2)
#   uncompressed --texture-compress off
#   no-mips      --texture-mips off
#   none         --texture-compress off --texture-mips off
TEXTURE_OPTS="${LUSDVIEW_USD_ASSETS_TEXTURES:-full}"
# Golden-fingerprint regression layer (opt-in). Point GOLDEN at a TSV baseline
# (mode<TAB>asset<TAB>fingerprint). With GOLDEN_UPDATE=1 the baseline is
# (re)written from this run instead of compared. GOLDEN_TOL is the max L1
# distance (sum of per-nibble deltas over a 12x12x3 quantized grid, range
# 0..6480 for hash, 0..144 for coverage) tolerated before a render is
# downgraded to golden_mismatch.
GOLDEN_FILE="${LUSDVIEW_USD_ASSETS_GOLDEN:-}"
GOLDEN_UPDATE="${LUSDVIEW_USD_ASSETS_GOLDEN_UPDATE:-0}"
GOLDEN_KIND="${LUSDVIEW_USD_ASSETS_GOLDEN_KIND:-hash}"
GOLDEN_TOL="${LUSDVIEW_USD_ASSETS_GOLDEN_TOL:-}"
FINGERPRINT_PY="$SCRIPT_DIR/asset_fingerprint.py"
EXPECTATIONS_FILE="${LUSDVIEW_USD_ASSETS_EXPECTATIONS:-}"
# JSON mirror of results.tsv (default on; set to a path or empty to disable).
JSON_OUT="${LUSDVIEW_USD_ASSETS_JSON:-auto}"

usage() {
  cat <<EOF
Usage: USD_ASSETS_ROOT=/path/to/assets $0 [options]

Options:
  --root DIR       Asset root (same as USD_ASSETS_ROOT)
  --out DIR        Output dir (default: temporary dir)
  --limit N        Limit discovered USD files (default: all)
  --profile NAME   Asset profile when --files is omitted: all,
                   usd-assets-curated, or usd-assets-broad
  --require-profile-complete  Fail when a selected profile asset is missing
  --modes LIST     Comma list: gl-raster,vk-raster,vk-rt,cuda-rt,hip-rt,lusdr-cpu,lusdr-vk,lusdr-vkr
  --size WxH       lusdview --size (default: 256x256)
  --frames N       lusdview --frames (default: 4)
  --timeout DUR    Per-render timeout(1) duration (default: 45s)
  --time CODE      Evaluate animated assets at this USD time code
  --vk-device DEV  Forward --vk-device DEV to Vulkan modes
  --fail-on LIST   Comma statuses that make the script fail
  --textures KIND  CPU texture quality for lusdview modes: full,
                   uncompressed, no-mips, or none (default: full)
  --golden FILE    Compare rendered fingerprints against a golden TSV baseline
  --update-golden FILE  (Re)write the golden baseline from this run
  --golden-kind K  Fingerprint kind: hash or coverage (default: hash)
  --golden-tol N   Max fingerprint distance before golden_mismatch
                  (default: 160 for hash, 8 for coverage)
  --expectations FILE  Per-asset expected status/warning/tolerance TSV
  --json FILE      Also write results as JSON (default: <out>/results.json)

Environment:
  LUSDVIEW=/path/to/lusdview
  LUSDRENDER=/path/to/lusdrender
  LUSDVIEW_NVIDIA_OFFLOAD=auto|1|0
  LUSDVIEW_XVFB=auto|1|0|external
  LUSDVIEW_USD_ASSETS_GOLDEN=FILE  LUSDVIEW_USD_ASSETS_GOLDEN_UPDATE=1
  LUSDVIEW_USD_ASSETS_EXPECTATIONS=FILE
  LUSDVIEW_USD_ASSETS_TEXTURES=full|uncompressed|no-mips|none
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --root) USD_ASSETS_ROOT="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    --require-profile-complete) REQUIRE_PROFILE_COMPLETE=1; shift ;;
    --modes) MODES="$2"; shift 2 ;;
    --size) SIZE="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --timeout) TIMEOUT_DUR="$2"; shift 2 ;;
    --time) TIME_CODE="$2"; shift 2 ;;
    --vk-device) VK_DEVICE="$2"; shift 2 ;;
    --fail-on) FAIL_ON="$2"; shift 2 ;;
    --textures) TEXTURE_OPTS="$2"; shift 2 ;;
    --golden) GOLDEN_FILE="$2"; shift 2 ;;
    --update-golden) GOLDEN_FILE="$2"; GOLDEN_UPDATE=1; shift 2 ;;
    --golden-kind) GOLDEN_KIND="$2"; shift 2 ;;
    --golden-tol) GOLDEN_TOL="$2"; shift 2 ;;
    --expectations) EXPECTATIONS_FILE="$2"; shift 2 ;;
    --json) JSON_OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

case "$GOLDEN_KIND" in
  hash)
    GOLDEN_TOL="${GOLDEN_TOL:-160}"
    GOLDEN_HASH_CMD=hash
    GOLDEN_COMPARE_CMD=compare
    ;;
  coverage)
    GOLDEN_TOL="${GOLDEN_TOL:-8}"
    GOLDEN_HASH_CMD=coverage
    GOLDEN_COMPARE_CMD=coverage-compare
    ;;
  *)
    echo "ERROR: invalid golden kind '$GOLDEN_KIND' (expected hash or coverage)" >&2
    exit 1
    ;;
esac

case "$TEXTURE_OPTS" in
  full|uncompressed|no-mips|none) ;;
  *)
    echo "ERROR: invalid texture options '$TEXTURE_OPTS' (expected full, uncompressed, no-mips, or none)" >&2
    exit 1
    ;;
esac

if [ ! -x "$LUSDVIEW_BIN" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW_BIN (set LUSDVIEW=...)"
  exit "$SKIP"
fi

if [ -z "$USD_ASSETS_ROOT" ]; then
  echo "SKIP: USD_ASSETS_ROOT is not set"
  exit "$SKIP"
fi
if [ ! -d "$USD_ASSETS_ROOT" ]; then
  echo "SKIP: USD_ASSETS_ROOT does not exist: $USD_ASSETS_ROOT"
  exit "$SKIP"
fi
# --root overrides the environment-derived path after the initial setup.
# Resolve it here as well: find does not descend through a symlink root.
USD_ASSETS_ROOT="$(cd "$USD_ASSETS_ROOT" && pwd -P)"

# Opt-in gate: if LUSDVIEW_USD_ASSETS_GATE names an env var and that var is
# empty/unset, SKIP. Golden fingerprints are a per-machine baseline, so the
# checked-in golden ctest gates on LUSDVIEW_RUN_GOLDEN to avoid false failures
# on CI GPUs that differ from the machine that recorded the baseline.
GATE_VAR="${LUSDVIEW_USD_ASSETS_GATE:-}"
if [ -n "$GATE_VAR" ] && [ -z "${!GATE_VAR:-}" ]; then
  echo "SKIP: gate variable '$GATE_VAR' is not set (opt-in test)"
  exit "$SKIP"
fi

if [ -z "$OUT_DIR" ]; then
  OUT_DIR="$(mktemp -d)"
  CLEAN_OUT=1
else
  mkdir -p "$OUT_DIR"
  CLEAN_OUT=0
fi
trap '[ "$CLEAN_OUT" -eq 1 ] && rm -rf "$OUT_DIR"' EXIT

RESULTS="$OUT_DIR/results.tsv"
: > "$RESULTS"
printf 'mode\tstatus\tgolden\tasset\timage\tlog\tdiagnostics\n' >> "$RESULTS"

# Golden baseline handling. In compare mode, load mode+asset -> fingerprint into
# an associative array. In update mode, freshly computed fingerprints are
# collected and written out at the end.
GOLDEN_ENABLED=0
declare -A GOLDEN_EXPECT
NEW_GOLDEN="$OUT_DIR/new-golden.tsv"
: > "$NEW_GOLDEN"
if [ -n "$GOLDEN_FILE" ]; then
  if [ ! -x "$FINGERPRINT_PY" ] && ! command -v python3 >/dev/null 2>&1; then
    echo "WARN: golden requested but python3/asset_fingerprint.py unavailable; disabling golden" >&2
  else
    GOLDEN_ENABLED=1
    if [ "$GOLDEN_UPDATE" != "1" ] && [ -f "$GOLDEN_FILE" ]; then
      while IFS=$'\t' read -r g_mode g_asset g_fp; do
        [ -z "$g_mode" ] && continue
        case "$g_mode" in \#*) continue ;; esac
        GOLDEN_EXPECT["$g_mode|$g_asset"]="$g_fp"
      done < "$GOLDEN_FILE"
    fi
  fi
fi

# Optional expectations TSV:
#   mode<TAB>asset<TAB>status<TAB>warning-regex<TAB>golden-tolerance
# `*` mode is a fallback. Empty or `*` status accepts any rendered status.
# A warning regex must match the render log. A per-entry tolerance overrides
# --golden-tol. This makes known degradation explicit instead of silently
# accepting every warning emitted by an external corpus.
declare -A EXPECT_STATUS EXPECT_WARNING EXPECT_TOL
if [ -n "$EXPECTATIONS_FILE" ]; then
  if [ ! -f "$EXPECTATIONS_FILE" ]; then
    echo "ERROR: expectations file does not exist: $EXPECTATIONS_FILE" >&2
    exit 1
  fi
  while IFS=$'\t' read -r e_mode e_asset e_status e_warning e_tol; do
    [ -z "$e_mode" ] && continue
    case "$e_mode" in \#*) continue ;; esac
    key="$e_mode|$e_asset"
    EXPECT_STATUS["$key"]="$e_status"
    EXPECT_WARNING["$key"]="$e_warning"
    EXPECT_TOL["$key"]="$e_tol"
  done < "$EXPECTATIONS_FILE"
fi

has_status() {
  case ",$1," in
    *",$2,"*) return 0 ;;
    *) return 1 ;;
  esac
}

# Classify a non-blank render's log into rendered / rendered_with_warnings /
# degraded_material. Prefers lusdview's structured "load summary:" line (emitted
# only when there are actionable diagnostics) and the "using default material"
# signal (both tools) for the most severe bucket, then falls back to the broad
# warning grep for anything else.
classify_rendered() {
  local log="$1"
  if grep -Eq 'load summary:.*degraded_materials=[1-9]' "$log" \
     || grep -Eiq 'using default material' "$log"; then
    echo "degraded_material"
  elif grep -Eq 'load summary:' "$log" \
       || grep -Eiq 'warn|warning|unsupported|fallback|TODO|failed to load texture|failed to load environment' "$log"; then
    echo "rendered_with_warnings"
  else
    echo "rendered"
  fi
}

should_use_nvidia_offload() {
  case "$NVIDIA_OFFLOAD" in
    1|ON|on|true|TRUE|yes|YES) return 0 ;;
    0|OFF|off|false|FALSE|no|NO) return 1 ;;
  esac
  [ -f /usr/share/glvnd/egl_vendor.d/10_nvidia.json ] && command -v nvidia-smi >/dev/null 2>&1
}

should_use_xvfb() {
  case "$USE_XVFB" in
    1|ON|on|true|TRUE|yes|YES) return 0 ;;
    0|OFF|off|false|FALSE|no|NO) return 1 ;;
    external|EXTERNAL) return 0 ;;
  esac
  should_use_nvidia_offload && command -v xvfb-run >/dev/null 2>&1
}

use_external_xvfb() {
  case "$USE_XVFB" in
    external|EXTERNAL) return 0 ;;
    *) return 1 ;;
  esac
}

nonblank_ppm() {
  local img="$1"
  python3 - "$img" <<'PY'
import sys

try:
    data = open(sys.argv[1], "rb").read()
except OSError:
    sys.exit(2)

if not data.startswith(b"P6"):
    sys.exit(2)

i = 2
tokens = []
while len(tokens) < 3 and i < len(data):
    c = data[i]
    if c == 35:  # '#'
        while i < len(data) and data[i] not in (10, 13):
            i += 1
    elif chr(c).isspace():
        i += 1
    else:
        start = i
        while i < len(data) and not chr(data[i]).isspace():
            i += 1
        tokens.append(data[start:i])
if len(tokens) != 3:
    sys.exit(2)
# PPM has exactly one whitespace byte after maxval. Do not skip an arbitrary
# whitespace run here: binary pixel data may legitimately start with bytes such
# as 0x09, 0x0a, 0x0d, 0x1c..0x1f that Python classifies as whitespace.
if i < len(data) and chr(data[i]).isspace():
    i += 1

w, h, maxv = [int(x) for x in tokens]
if maxv != 255:
    sys.exit(2)
px = data[i:i + w * h * 3]
if len(px) < w * h * 3:
    sys.exit(2)
first = px[:3]
for p in range(3, len(px), 3):
    if px[p:p+3] != first:
        sys.exit(0)
sys.exit(1)
PY
}

nonblank_file() {
  local img="$1"
  [ -s "$img" ] && [ "$(wc -c < "$img")" -gt 256 ]
}

asset_rel() {
  local asset="$1"
  python3 - "$USD_ASSETS_ROOT" "$asset" <<'PY'
import os, sys
print(os.path.relpath(sys.argv[2], sys.argv[1]).replace(os.sep, "/"))
PY
}

safe_name() {
  printf '%s' "$1" | sed 's#[/\\ ]#_#g; s#[^A-Za-z0-9._-]#_#g'
}

run_one() {
  local mode="$1"
  local asset="$2"
  local rel safe out log status rc
  rel="$(asset_rel "$asset")"
  safe="$(safe_name "${mode}_${rel}")"
  out="$OUT_DIR/${safe}.ppm"
  log="$OUT_DIR/${safe}.log"

  local args=()
  local use_vk_env=0
  local use_xvfb_for_viewer=0
  if [ "$mode" = "gl-raster" ] || [ "$mode" = "vk-raster" ] ||
     [ "$mode" = "vk-rt" ]; then
    if should_use_xvfb; then
      use_xvfb_for_viewer=1
    fi
  fi
  case "$mode" in
    gl-raster)
      args=(--backend gl)
      ;;
    vk-raster)
      args=(--backend vk)
      [ "$use_xvfb_for_viewer" -eq 0 ] && args=(--headless "${args[@]}")
      use_vk_env=1
      ;;
    vk-rt)
      args=(--backend vk --rt)
      [ "$use_xvfb_for_viewer" -eq 0 ] && args=(--headless "${args[@]}")
      use_vk_env=1
      ;;
    cuda-rt)
      args=(--headless --cuda)
      ;;
    hip-rt)
      args=(--headless --hip)
      ;;
    lusdr-cpu|lusdr-vk|lusdr-vkr)
      if [ ! -x "$LUSDRENDER_BIN" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "backend_unavailable" "-" "$rel" "" "" "-" >> "$RESULTS"
        printf '%-9s %-22s %-9s %s\n' "$mode" "backend_unavailable" "-" "$rel"
        return 0
      fi
      ;;
    *)
      echo "ERROR: unknown mode '$mode'" >&2
      return 2
      ;;
  esac

  local cmd=()
  if [[ "$mode" == lusdr-* ]]; then
    out="$OUT_DIR/${safe}.png"
    case "$mode" in
      lusdr-cpu) args=("$asset" "$out" -rtPreview) ;;
      lusdr-vk) args=("$asset" "$out" -vk) ;;
      lusdr-vkr) args=("$asset" "$out" -vkr) ;;
    esac
    args+=(-w "$WIDTH" -height "$HEIGHT" -autoframe -samples 1)
    [ -n "$TIME_CODE" ] && args+=(-timecode "$TIME_CODE")
    cmd+=(env "$LUSDRENDER_BIN" "${args[@]}")
  else
    if [ "$use_vk_env" -eq 1 ] && [ -n "$VK_DEVICE" ]; then
      args+=(--vk-device "$VK_DEVICE")
    fi
    case "$ALLOW_PARENT" in
      1|ON|on|true|TRUE|yes|YES) args+=(--allow-parent-paths) ;;
    esac
    case "$TEXTURE_OPTS" in
      uncompressed) args+=(--texture-compress off) ;;
      no-mips) args+=(--texture-mips off) ;;
      none) args+=(--texture-compress off --texture-mips off) ;;
    esac
    args+=(--frames "$FRAMES" --size "$SIZE" --screenshot "$out" "$asset")
    [ -n "$TIME_CODE" ] && args+=(--time "$TIME_CODE")

    if [ "$use_xvfb_for_viewer" -eq 1 ] && ! use_external_xvfb; then
      cmd+=(xvfb-run -a env)
    else
      cmd+=(env)
    fi
    if [ "$use_vk_env" -eq 1 ] && should_use_nvidia_offload; then
      cmd+=(__NV_PRIME_RENDER_OFFLOAD=1)
      cmd+=(__GLX_VENDOR_LIBRARY_NAME=nvidia)
      cmd+=(__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json)
    fi
    cmd+=("$LUSDVIEW_BIN" "${args[@]}")
  fi

  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "$TIMEOUT_DUR" "${cmd[@]}" >"$log" 2>&1
    rc=$?
  else
    "${cmd[@]}" >"$log" 2>&1
    rc=$?
  fi

  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    status="timeout"
  elif grep -Eiq 'CUDA ray tracing unavailable|CUDA RT failed|no CUDA|NVRTC.*failed|HIP ray tracing unavailable|HIP RT failed|no HIP|hiprtc.*failed|renderer init failed: no Vulkan|no Vulkan physical device|Vulkan backend unavailable|Vulkan unavailable|backend .*unavailable|Failed to create Vulkan|lightrt_vk_engine_create failed' "$log"; then
    status="backend_unavailable"
  # The viewer reports empty but valid layers as "load failed: ... no
  # renderable geometry produced".  Check this before the broad load-failure
  # matcher so material/camera/look layers do not inflate hard load errors.
  elif grep -Eqi 'no renderable geometry produced|render stats: meshes 0/0 visible, instances 0/0 visible, drawn tris 0|loaded .*: 0 mesh\(es\), 0 tri\(s\)|rt meshes: 0|triangles: 0|found no renderable Mesh triangles' "$log"; then
    status="no_renderable"
  elif grep -Eiq 'load failed|Failed to load USD|LoadUSDFromFile.*failed|parse error|No such file|cannot open|^ERR .*load|^ERROR .*load' "$log"; then
    status="load_error"
  elif [[ "$mode" == lusdr-* ]] && [ ! -s "$out" ]; then
    status="backend_error"
  elif [[ "$mode" == lusdr-* ]] && nonblank_file "$out"; then
    status="$(classify_rendered "$log")"
  elif [[ "$mode" == lusdr-* ]]; then
    status="no_renderable"
  elif [ "$mode" = "cuda-rt" ] && ! grep -q 'CUDA RT wrote' "$log"; then
    status="backend_error"
  elif [ "$mode" = "hip-rt" ] && ! grep -q 'HIP RT wrote' "$log"; then
    status="backend_error"
  elif [ "$mode" != "cuda-rt" ] && [ "$mode" != "hip-rt" ] &&
       ! grep -q 'render stats' "$log"; then
    status="backend_error"
  elif [ ! -s "$out" ]; then
    status="backend_error"
  elif nonblank_ppm "$out"; then
    status="$(classify_rendered "$log")"
  elif ! nonblank_ppm "$out"; then
    status="no_renderable"
  else
    status="$(classify_rendered "$log")"
  fi

  # Compact the structured renderer diagnostics into the machine-readable row.
  local diagnostics="-"
  local summary_line
  summary_line="$(grep -E 'load summary:' "$log" | tail -1 || true)"
  if [ -n "$summary_line" ]; then
    diagnostics="${summary_line#*load summary: }"
    diagnostics="${diagnostics//$'\t'/ }"
  fi

  local expect_key="$mode|$rel"
  local fallback_key="*|$rel"
  if [ -z "${EXPECT_STATUS[$expect_key]+x}" ] &&
     [ -n "${EXPECT_STATUS[$fallback_key]+x}" ]; then
    expect_key="$fallback_key"
  fi
  local expected_status="${EXPECT_STATUS[$expect_key]:-}"
  local expected_warning="${EXPECT_WARNING[$expect_key]:-}"
  local entry_tol="${EXPECT_TOL[$expect_key]:-$GOLDEN_TOL}"
  local observed_status="$status"

  # Golden-fingerprint check: only meaningful for a real (non-blank) render.
  # A mismatch overrides the status so the existing FAIL_ON path handles it.
  local golden="-"
  if [ "$GOLDEN_ENABLED" -eq 1 ] \
     && { [ "$status" = "rendered" ] || [ "$status" = "rendered_with_warnings" ]; } \
     && [ -s "$out" ]; then
    local fp
    fp="$(python3 "$FINGERPRINT_PY" "$GOLDEN_HASH_CMD" "$out" 2>/dev/null)"
    if [ -z "$fp" ]; then
      golden="nohash"
    elif [ "$GOLDEN_UPDATE" = "1" ]; then
      printf '%s\t%s\t%s\n' "$mode" "$rel" "$fp" >> "$NEW_GOLDEN"
      golden="updated"
    else
      local expect="${GOLDEN_EXPECT["$mode|$rel"]:-}"
      if [ -z "$expect" ]; then
        golden="missing"
      elif python3 "$FINGERPRINT_PY" "$GOLDEN_COMPARE_CMD" "$fp" "$expect" "$entry_tol" >/dev/null 2>&1; then
        golden="ok"
      else
        golden="mismatch"
        status="golden_mismatch"
      fi
    fi
  fi

  # If a baseline says this asset previously rendered normally, degradation is
  # a regression unless the expectations manifest explicitly allows it.
  if [ "$status" = "degraded_material" ] &&
     [ -n "${GOLDEN_EXPECT["$mode|$rel"]:-}" ] &&
     [ -z "$expected_status" ]; then
    status="unexpected_degradation"
  fi

  if [ -n "$expected_status" ] && [ "$expected_status" != "*" ]; then
    case ",$expected_status," in
      *",$observed_status,"*) ;;
      *) status="expectation_mismatch" ;;
    esac
  fi
  if [ "$status" != "rendered" ] && [ -n "$expected_warning" ] &&
     [ "$expected_warning" != "-" ] &&
     ! grep -Eq "$expected_warning" "$log"; then
    status="expectation_mismatch"
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "$status" "$golden" "$rel" "$out" "$log" "$diagnostics" >> "$RESULTS"
  printf '%-9s %-22s %-9s %s\n' "$mode" "$status" "$golden" "$rel"

  if has_status "$FAIL_ON" "$status"; then
    return 1
  fi
  return 0
}

# An explicit curated file list (LUSDVIEW_USD_ASSETS_FILES: space/newline
# separated, paths relative to USD_ASSETS_ROOT or absolute) renders exactly
# those assets, in order. Empty = use the selected profile. The checked-in ctests
# pass explicit repo-model lists; usd-assets-curated is for public external
# usd-wg/assets checkouts and skips missing files.
ASSET_FILES="${LUSDVIEW_USD_ASSETS_FILES:-}"
if [ -n "$ASSET_FILES" ]; then
  ASSETS=()
  for f in $ASSET_FILES; do
    case "$f" in
      /*) p="$f" ;;
      *)  p="$USD_ASSETS_ROOT/$f" ;;
    esac
    if [ -f "$p" ]; then
      ASSETS+=("$p")
    else
      echo "WARN: curated asset not found, skipping: $p" >&2
    fi
  done
else
  ASSETS=()
  PROFILE_MISSING=0
  add_profile_asset() {
    local p="$USD_ASSETS_ROOT/$1"
    if [ -f "$p" ]; then
      ASSETS+=("$p")
    else
      PROFILE_MISSING=$((PROFILE_MISSING + 1))
      echo "WARN: profile asset not found, skipping: $p" >&2
    fi
  }
  case "$PROFILE" in
    all)
      mapfile -t ASSETS < <(
        find "$USD_ASSETS_ROOT" -type f \( -name '*.usd' -o -name '*.usda' -o -name '*.usdc' -o -name '*.usdz' \) \
          | sort
      )
      ;;
    usd-assets-curated)
      for f in \
        test_assets/AlphaBlendModeTest/AlphaBlendModeTest.usd \
        test_assets/AlphaBlendSortTest/AlphaBlendSortTest.usda \
        test_assets/MaterialXTest/basicTextured.usda \
        test_assets/TextureTransformTest/TextureTransformTest.usd \
        test_assets/TextureCoordinateTest/TextureCoordinateTest.usda \
        test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda \
        test_assets/RoughnessTest/RoughnessTest.usdz \
        test_assets/TextureFileFormatTests/all_files.usda \
        full_assets/StandardShaderBall/standard_shader_ball_scene.usda; do
        add_profile_asset "$f"
      done
      ;;
    usd-assets-broad)
      # A bounded cross-section of usd-wg/assets. Keep this list focused on
      # top-level scenes or deliberately useful schema leaves: rendering every
      # layer in the corpus mostly measures expected empty material/camera
      # layers and obscures scene-fidelity regressions.
      for f in \
        docs/CompositionPuzzles/PayloadAndReference/solution/shot.usda \
        docs/CompositionPuzzles/VariantSetAndLocal1/puzzle_1.usda \
        docs/PrimvarInterpolation/primvar_interpolation.usda \
        test_assets/foundation/stage_composition/payload/payload_same_folder.usda \
        test_assets/foundation/stage_composition/references/reference_same_folder.usda \
        test_assets/foundation/stage_composition/subLayer/sublayer_same_folder.usda \
        test_assets/foundation/stage_composition/purpose.usda \
        test_assets/schemaTests/usdGeom/extent/no_extent.usda \
        test_assets/schemaTests/usdGeom/meshes/normals_types/normalsTypes.usda \
        test_assets/schemaTests/usdGeom/meshes/points_types/pointsTypes.usda \
        test_assets/schemaTests/usdGeom/meshes/doubleSided/doubleSided_quad.usda \
        test_assets/schemaTests/usdGeom/meshes/subdiv_bilinear/subdiv_bilinear.usda \
        test_assets/schemaTests/usdGeom/meshes/subdiv_catmullClark/subdiv_catmullClark.usda \
        test_assets/schemaTests/usdGeom/meshes/subdiv_loop_triangles/subdiv_loop_triangles.usda \
        test_assets/schemaTests/usdGeom/primitives/all_primitives.usda \
        test_assets/schemaTests/usdGeom/transforms/complex_transform.usda \
        test_assets/References/OverridingReferencedInternalReferencesTest.usda \
        intent-vfx/scenes/simpleAssetScene.usd \
        intent-vfx/scenes/teapotScene.usd \
        test_assets/AlphaBlendModeTest/AlphaBlendModeTest.usd \
        test_assets/AlphaBlendSortTest/AlphaBlendSortTest.usda \
        test_assets/MaterialXTest/basic.usda \
        test_assets/MaterialXTest/basicTextured.usda \
        test_assets/TextureCoordinateTest/TextureCoordinateTestMaterialX.usda \
        test_assets/TextureTransformTest/TextureTransformTest.usd \
        test_assets/TextureFileFormatTests/all_files.usda \
        test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usdz \
        test_assets/RoughnessTest/RoughnessTest.usdz \
        full_assets/SubdivisionSurfaces/Creases_SpinningPyramids.usda \
        full_assets/StandardShaderBall/standard_shader_ball_scene.usda \
        full_assets/Teapot/Teapot.usd \
        full_assets/OpenChessSet/chess_set.usda \
        full_assets/UsdCookie/UsdCookie.usdz \
        test_assets/USDZ/AnimatedCube/AnimatedCube.usdz \
        test_assets/USDZ/AnimatedTriangle/AnimatedTriangle.usdz \
        test_assets/USDZ/BoxAnimated/BoxAnimated.usdz \
        test_assets/USDZ/InterpolationTest/InterpolationTest.usdz \
        test_assets/USDZ/CesiumMan/CesiumMan.usdz \
        test_assets/USDZ/RiggedFigure/RiggedFigure.usdz \
        test_assets/USDZ/RiggedSimple/RiggedSimple.usdz; do
        add_profile_asset "$f"
      done
      ;;
    *)
      echo "ERROR: unknown asset profile '$PROFILE' (expected all, usd-assets-curated, or usd-assets-broad)" >&2
      exit 1
      ;;
  esac
  if [ "$REQUIRE_PROFILE_COMPLETE" = "1" ] && [ "$PROFILE_MISSING" -ne 0 ]; then
    echo "ERROR: profile '$PROFILE' is incomplete: $PROFILE_MISSING asset(s) missing" >&2
    exit 1
  fi
fi

if [ "${#ASSETS[@]}" -eq 0 ]; then
  echo "SKIP: no USD-family assets found under $USD_ASSETS_ROOT"
  exit "$SKIP"
fi

if [ "$LIMIT" -gt 0 ] 2>/dev/null && [ "${#ASSETS[@]}" -gt "$LIMIT" ]; then
  ASSETS=("${ASSETS[@]:0:$LIMIT}")
fi

IFS=',' read -r -a MODE_LIST <<< "$MODES"

echo "lusdview: $LUSDVIEW_BIN"
echo "assets : $USD_ASSETS_ROOT"
echo "out    : $OUT_DIR"
echo "modes  : $MODES"
echo "profile: $PROFILE"
echo "size   : $SIZE, frames: $FRAMES, timeout: $TIMEOUT_DUR"
[ -n "$TIME_CODE" ] && echo "time   : $TIME_CODE"
[ -n "$VK_DEVICE" ] && echo "vk dev : $VK_DEVICE"
echo "tex    : $TEXTURE_OPTS"
echo

fail=0
for asset in "${ASSETS[@]}"; do
  for mode in "${MODE_LIST[@]}"; do
    run_one "$mode" "$asset" || fail=1
  done
done

# Persist an updated golden baseline (sorted + de-duplicated, last wins).
if [ "$GOLDEN_ENABLED" -eq 1 ] && [ "$GOLDEN_UPDATE" = "1" ]; then
  {
    printf '# lusdview usd-assets render fingerprints (mode<TAB>asset<TAB>fp).\n'
    printf '# kind=%s tol=%s; regenerate with --update-golden.\n' "$GOLDEN_KIND" "$GOLDEN_TOL"
    printf '# size=%s; fixed-frame headless viewport.\n' "$SIZE"
    sort -t$'\t' -k1,1 -k2,2 "$NEW_GOLDEN"
  } > "$GOLDEN_FILE"
  echo "golden : wrote $(grep -cv '^#' "$GOLDEN_FILE") fingerprints -> $GOLDEN_FILE"
fi

# Machine-readable JSON mirror of the TSV.
if [ "$JSON_OUT" = "auto" ]; then
  JSON_OUT="$OUT_DIR/results.json"
fi
if [ -n "$JSON_OUT" ] && command -v python3 >/dev/null 2>&1; then
  python3 - "$RESULTS" "$JSON_OUT" <<'PY'
import csv, json, sys
rows = []
with open(sys.argv[1], newline="") as f:
    for r in csv.DictReader(f, delimiter="\t"):
        rows.append(r)
summary = {}
for r in rows:
    key = "%s/%s" % (r.get("mode", ""), r.get("status", ""))
    summary[key] = summary.get(key, 0) + 1
with open(sys.argv[2], "w") as f:
    json.dump({"results": rows, "summary": summary}, f, indent=2)
PY
  echo "json   : $JSON_OUT"
fi

echo
echo "Summary:"
awk 'NR > 1 { counts[$1 "\t" $2]++ } END { for (k in counts) print counts[k] "\t" k }' "$RESULTS" | sort -k2,2 -k3,3
if [ "$GOLDEN_ENABLED" -eq 1 ] && [ "$GOLDEN_UPDATE" != "1" ]; then
  echo "Golden:"
  awk -F'\t' 'NR > 1 && $3 != "-" { g[$3]++ } END { for (k in g) print g[k] "\t" k }' "$RESULTS" | sort -k2,2
fi
echo "results: $RESULTS"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: one or more statuses matched LUSDVIEW_USD_ASSETS_FAIL_ON=$FAIL_ON"
  exit 1
fi

echo "PASS: usd-assets lusdview smoke completed"
exit 0
