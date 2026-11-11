#!/usr/bin/env bash
#
# Compare lusdrender's core-loader material resolvers over curated USD assets.
#
# This is an opt-in coverage harness for the shared material-eval migration:
# legacy remains the default renderer, tydra exercises
# RenderSceneConverter -> RenderMaterial -> shared OpenPBR params, and compare
# renders legacy while reporting fields that differ. Resolver differences are
# measured, not treated as failures by default; load/render failures still fail.
#
# Exit codes: 0 = pass, 1 = hard failure, 77 = skip.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

LUSDRENDER="${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}"
USD_ASSETS_ROOT="${USD_ASSETS_ROOT:-$REPO_ROOT/usd-assets}"
if [ -d "$USD_ASSETS_ROOT" ]; then
  USD_ASSETS_ROOT="$(cd "$USD_ASSETS_ROOT" && pwd -P)"
fi
OUT_DIR="${LUSDR_MATERIAL_RESOLVER_OUT:-}"
SIZE="${LUSDR_MATERIAL_RESOLVER_SIZE:-160x120}"
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
TIMEOUT_DUR="${LUSDR_MATERIAL_RESOLVER_TIMEOUT:-45s}"
LIMIT="${LUSDR_MATERIAL_RESOLVER_LIMIT:-0}"
FILES="${LUSDR_MATERIAL_RESOLVER_FILES:-}"
FAIL_ON_DIFF="${LUSDR_MATERIAL_RESOLVER_FAIL_ON_DIFF:-0}"
GATE_VAR="${LUSDR_MATERIAL_RESOLVER_GATE:-}"
MATERIAL_SHADING="${LUSDR_MATERIAL_SHADING:-}"

usage() {
  cat <<EOF
Usage: USD_ASSETS_ROOT=/path/to/usd-assets $0 [options]

Options:
  --root DIR       Asset root (same as USD_ASSETS_ROOT)
  --out DIR        Output dir (default: temporary dir)
  --files LIST     Space-separated assets, relative to root or absolute
  --limit N        Limit auto-discovered files
  --size WxH       Render size (default: 160x120)
  --timeout DUR    Per-render timeout(1) duration (default: 45s)
  --fail-on-diff   Treat materialResolver compare differences as failure

Environment:
  LUSDRENDER=/path/to/lusdrender
  LUSDR_MATERIAL_RESOLVER_FILES="file.usda ..."
  LUSDR_MATERIAL_RESOLVER_GATE=ENVVAR
  LUSDR_MATERIAL_SHADING=legacy|lightrt-bsdf
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --root) USD_ASSETS_ROOT="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --files) FILES="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --size) SIZE="$2"; WIDTH="${SIZE%x*}"; HEIGHT="${SIZE#*x}"; shift 2 ;;
    --timeout) TIMEOUT_DUR="$2"; shift 2 ;;
    --fail-on-diff) FAIL_ON_DIFF=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
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
if [ -n "$GATE_VAR" ] && [ -z "${!GATE_VAR:-}" ]; then
  echo "SKIP: gate variable '$GATE_VAR' is not set"
  exit "$SKIP"
fi
SHADING_ARGS=()
if [ -n "$MATERIAL_SHADING" ]; then
  case "$MATERIAL_SHADING" in
    legacy|lightrt-bsdf) SHADING_ARGS=(--materialShading "$MATERIAL_SHADING") ;;
    *)
      echo "ERROR: invalid LUSDR_MATERIAL_SHADING: $MATERIAL_SHADING" >&2
      exit 1
      ;;
  esac
fi

if [ -z "$OUT_DIR" ]; then
  OUT_DIR="$(mktemp -d)"
  CLEAN_OUT=1
else
  mkdir -p "$OUT_DIR"
  CLEAN_OUT=0
fi
trap '[ "$CLEAN_OUT" -eq 1 ] && rm -rf "$OUT_DIR"' EXIT

asset_rel() {
  python3 - "$USD_ASSETS_ROOT" "$1" <<'PY'
import os, sys
print(os.path.relpath(sys.argv[2], sys.argv[1]).replace(os.sep, "/"))
PY
}

safe_name() {
  printf '%s' "$1" | sed 's#[/\\ ]#_#g; s#[^A-Za-z0-9._-]#_#g'
}

add_if_exists() {
  local f="$1" p
  case "$f" in
    /*) p="$f" ;;
    *) p="$USD_ASSETS_ROOT/$f" ;;
  esac
  [ -f "$p" ] && ASSETS+=("$p")
}

ASSETS=()
if [ -n "$FILES" ]; then
  for f in $FILES; do
    add_if_exists "$f" || true
  done
else
  # Fast, material-heavy AOUSD/usd-assets subset. Missing files are ignored so
  # the same list works across partial asset checkouts.
  for f in \
    test_assets/MaterialXTest/basicTextured.usda \
    test_assets/MaterialXTest/basicTextured_flatten.usda \
    test_assets/TextureTransformTest/TextureTransformTest.usd \
    test_assets/TextureCoordinateTest/TextureCoordinateTest.usda \
    test_assets/TextureCoordinateTest/TextureCoordinateTestMaterialX.usda \
    test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda \
    test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usdz \
    test_assets/RoughnessTest/RoughnessTest.usdz \
    test_assets/AlphaBlendModeTest/AlphaBlendModeTest.usd \
    test_assets/AlphaBlendSortTest/AlphaBlendSortTest.usda \
    test_assets/TextureFileFormatTests/all_files.usda \
    test_assets/TextureFileFormatTests/png_rgb_16-bit.usda \
    test_assets/TextureFileFormatTests/png_rgb_32-bit.usda \
    test_assets/USDZ/AnimatedTriangle/AnimatedTriangle.usdz \
    test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz \
    test_assets/USDZ/BrainStem/BrainStem.usdz \
    test_assets/USDZ/CesiumMan/CesiumMan.usdz \
    full_assets/StandardShaderBall/standard_shader_ball_scene.usda \
    full_assets/Teapot/Teapot.usd \
    full_assets/OpenChessSet/chess_set.usda \
    full_assets/UsdCookie/UsdCookie.usdz \
    full_assets/CarbonFrameBike/CarbonFrameBike.usdz \
    full_assets/McUsd/McUsd.usdz \
    intent-vfx/scenes/teapotScene.usd \
    full_assets/Vehicles/USD_Mini_Car_Kit/assets/vehicles/formula/asset/formulaFullAsset.usda \
    full_assets/Vehicles/USD_Mini_Car_Kit/assets/vehicles/tractor/asset/tractorBodyAsset.usda \
    full_assets/Vehicles/USD_Mini_Car_Kit/assets/vehicles/tractor/asset/tractorFullAsset.usda; do
    add_if_exists "$f" || true
  done
fi

if [ "${#ASSETS[@]}" -eq 0 ]; then
  mapfile -t ASSETS < <(
    find "$USD_ASSETS_ROOT" -type f \( -name '*.usd' -o -name '*.usda' -o -name '*.usdc' -o -name '*.usdz' \) \
      | sort
  )
fi
if [ "$LIMIT" -gt 0 ] 2>/dev/null && [ "${#ASSETS[@]}" -gt "$LIMIT" ]; then
  ASSETS=("${ASSETS[@]:0:$LIMIT}")
fi
if [ "${#ASSETS[@]}" -eq 0 ]; then
  echo "SKIP: no USD-family assets found under $USD_ASSETS_ROOT"
  exit "$SKIP"
fi

RESULTS="$OUT_DIR/material-resolver.tsv"
: > "$RESULTS"
printf 'resolver\tstatus\tdiffs\tasset\timage\tlog\n' >> "$RESULTS"

run_lusdrender() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "$TIMEOUT_DUR" "$LUSDRENDER" "$@"
  else
    "$LUSDRENDER" "$@"
  fi
}

classify() {
  local log="$1" out="$2" rc="$3"
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "timeout"
  elif grep -Eiq 'load failed|Failed to load USD|LoadUSDFromFile.*failed|parse error|No such file|cannot open|^ERR .*load|^ERROR .*load' "$log"; then
    echo "load_error"
  elif [ "$rc" -ne 0 ] && grep -Eiq 'failed|ERROR|backend .*unavailable' "$log"; then
    echo "render_error"
  elif [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 512 ]; then
    echo "blank"
  else
    echo "rendered"
  fi
}

count_compare_diffs() {
  local log="$1"
  grep -c '^materialResolver compare: .* differs:' "$log" 2>/dev/null || true
}

fail=0
diff_assets=0

echo "lusdrender: $LUSDRENDER"
echo "assets    : $USD_ASSETS_ROOT"
echo "out       : $OUT_DIR"
echo "size      : $SIZE, timeout: $TIMEOUT_DUR"
[ "${#SHADING_ARGS[@]}" -gt 0 ] && echo "shading   : $MATERIAL_SHADING"
echo

for asset in "${ASSETS[@]}"; do
  rel="$(asset_rel "$asset")"
  printf 'asset: %s\n' "$rel"
  for resolver in legacy tydra compare; do
    safe="$(safe_name "${resolver}_${rel}")"
    out="$OUT_DIR/${safe}.png"
    log="$OUT_DIR/${safe}.log"
    run_lusdrender "$asset" "$out" -rtPreview -w "$WIDTH" -height "$HEIGHT" \
      -autoframe -samples 1 --materialResolver "$resolver" \
      "${SHADING_ARGS[@]}" >"$log" 2>&1
    rc=$?
    status="$(classify "$log" "$out" "$rc")"
    diffs=0
    if [ "$resolver" = "compare" ]; then
      diffs="$(count_compare_diffs "$log")"
      if [ "$diffs" -gt 0 ]; then
        diff_assets=$((diff_assets + 1))
      fi
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$resolver" "$status" "$diffs" "$rel" "$out" "$log" >> "$RESULTS"
    printf '  %-10s %-12s diffs=%s\n' "$resolver" "$status" "$diffs"
    case "$status" in
      rendered) ;;
      *) fail=1 ;;
    esac
    if [ "$resolver" = "compare" ] && [ "$FAIL_ON_DIFF" = "1" ] && [ "$diffs" -gt 0 ]; then
      fail=1
    fi
  done
done

echo
echo "Summary:"
awk 'NR > 1 { counts[$1 "\t" $2]++ } END { for (k in counts) print counts[k] "\t" k }' "$RESULTS" | sort -k2,2 -k3,3
echo "compare assets with reported resolver diffs: $diff_assets"
echo "results: $RESULTS"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: material resolver harness saw a hard render failure"
  exit 1
fi

echo "PASS: material resolver harness completed"
exit 0
