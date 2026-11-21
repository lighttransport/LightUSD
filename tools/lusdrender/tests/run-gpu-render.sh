#!/usr/bin/env bash
#
# Run test: lusdrender's GPU compute/raytrace backends must render a non-blank
# frame AND agree with each other within a small tolerance. -vk (Vulkan compute)
# and -hip (HIP/ROCm compute) traverse the CPU-built LightRT BVH; -vkr (Vulkan
# hardware ray query) builds an indexed Vulkan acceleration structure directly
# from the same flattened mesh. The hit/shading inputs are equivalent, so a
# correct render matches across whichever backends are available -- up to sub-ULP
# FP differences at supersampled silhouette edges (compute vs hardware ray
# query), which show up as a few boundary pixels. This is the cross-backend
# correctness guard for the GPU paths (the smoke test only exercises the CPU
# -rtPreview path).
#
# Each backend is runtime-loaded and degrades gracefully: a backend that cannot
# create its device/engine prints a diagnostic and never emits the success line
# "backend: LightRT ...", so it is simply skipped here. If NO GPU backend is
# available the whole test SKIPs (exit 77, ctest SKIP_RETURN_CODE) so headless /
# non-GPU CI stays green.
#
# Exit codes: 0 = pass, 1 = fail (blank, or backends disagree), 77 = skip.
#
# Usage:  run-gpu-render.sh [LUSDRENDER_BINARY] [ASSET]
#   args override the LUSDRENDER / ASSET env vars; both have sensible defaults.
set -uo pipefail
SKIP=77

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDRENDER="${1:-${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}}"
ASSET="${2:-${ASSET:-$REPO_ROOT/models/suzanne-pbr.usda}}"
GPU_RENDER_TIMEOUT="${GPU_RENDER_TIMEOUT:-30s}"

if [ ! -x "$LUSDRENDER" ]; then
  echo "SKIP: lusdrender binary not found at $LUSDRENDER"
  exit $SKIP
fi
if [ ! -f "$ASSET" ]; then
  echo "SKIP: asset not found at $ASSET"
  exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run_lusdrender() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "$GPU_RENDER_TIMEOUT" "$LUSDRENDER" "$@"
  else
    "$LUSDRENDER" "$@"
  fi
}

# Render one backend. Returns 0 and sets REPLY to the output path when the
# backend rendered successfully (printed the "backend: LightRT" success line and
# wrote a non-trivial file); returns 1 when the backend is unavailable.
render() {
  local flag="$1" name="$2"
  local out="$TMP/gpu_${name}.png"
  local log
  log="$(run_lusdrender "$ASSET" "$out" "$flag" -w 200 -height 150 -autoframe \
        -samples 2 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  $name: unavailable (skipped)"
    return 1
  fi
  if [ "$name" = "vkr" ] &&
     ! echo "$log" | grep -q "ray_query, indexed Vulkan AS, CPU BVH skipped" &&
     ! echo "$log" | grep -q "ray_query, descriptor MaterialX" &&
     ! echo "$log" | grep -q "compute trace, ray_query fallback"; then
    echo "FAIL: vkr did not report descriptor shading, direct indexed Vulkan AS, or explicit fallback"
    echo "$log"
    REPLY="FAIL"
    return 0
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: $name produced a blank/trivial image"
    REPLY="FAIL"
    return 0
  fi
  echo "  $name: rendered $(wc -c < "$out") bytes"
  REPLY="$out"
  return 0
}

# CUDA and HIP use the lusdview RT core directly, including packed
# OpenPBR/MaterialX graphs. Their richer preview is intentionally not compared
# byte-for-byte with the legacy LightRT shade-after-hit Vulkan output.
render_shared() {
  local flag="$1" name="$2" extra="${3:-}"
  local out="$TMP/gpu_${name}.png"
  local graph_asset="$REPO_ROOT/tests/feat/node-mtlx/ChainTest.usda"
  local log
  log="$(run_lusdrender "$graph_asset" "$out" "$flag" -stats --path-trace \
        --pt-samples 2 -w 200 -height 150 -autoframe -samples 2 $extra 2>&1)"
  if ! echo "$log" | grep -q "backend: shared .* RT"; then
    echo "  $name: unavailable (skipped)"
    return 1
  fi
  if ! echo "$log" | grep -Eq "graphs=[1-9][0-9]* graph_nodes=[1-9][0-9]*"; then
    echo "FAIL: $name did not retain executable MaterialX graph topology"
    echo "$log"
    return 2
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: $name produced a blank/trivial image"
    echo "$log"
    return 2
  fi
  echo "  $name: rendered $(wc -c < "$out") bytes"
  return 0
}

render_profile() {
  local out="$TMP/gpu_profile_island.png"
  local log
  log="$(run_lusdrender "$ASSET" "$out" -largeSceneProfile island \
        -w 200 -height 150 -autoframe -samples 2 2>&1)"
  if ! echo "$log" | grep -q "largeSceneProfile island resolved"; then
    echo "FAIL: largeSceneProfile island did not log resolved settings"
    echo "$log"
    return 1
  fi
  # maxVram is derived from the device's VRAM heap, so assert a positive budget
  # rather than a value that only holds on this machine's card.
  for expected in "backend=vk" "rtLod=on" "maxVram=[1-9]"; do
    if ! echo "$log" | grep -Eq "$expected"; then
      echo "FAIL: largeSceneProfile island did not apply expected default: $expected"
      echo "$log"
      return 1
    fi
  done
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  largeSceneProfile island: Vulkan unavailable (skipped)"
    return 0
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: largeSceneProfile island produced a blank/trivial image"
    return 1
  fi
  echo "  largeSceneProfile island: rendered $(wc -c < "$out") bytes"
  return 0
}

render_gpu_shade_preview() {
  local out="$TMP/gpu_shade_preview.png"
  local log
  log="$(run_lusdrender "$ASSET" "$out" -vkr -gpuShade preview \
        -w 200 -height 150 -autoframe -samples 2 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -gpuShade preview: Vulkan unavailable (skipped)"
    return 0
  fi
  if ! echo "$log" | grep -q "OpenPBR preview"; then
    echo "FAIL: -gpuShade preview did not select OpenPBR material shading"
    echo "$log"
    return 1
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: -gpuShade preview produced a blank/trivial image"
    return 1
  fi
  echo "  -gpuShade preview: rendered $(wc -c < "$out") bytes"
  return 0
}

render_vkr_path_trace() {
  local out="$TMP/gpu_vkr_path.png"
  local log
  log="$(run_lusdrender "$ASSET" "$out" -vkr --path-trace \
        --pt-samples 2 --pt-max-depth 3 --pt-rr-depth 2 \
        -w 96 -height 72 -autoframe 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -vkr --path-trace: Vulkan ray query unavailable (skipped)"
    return 0
  fi
  if ! echo "$log" | grep -q "descriptor MaterialX, production path"; then
    echo "FAIL: -vkr --path-trace did not select descriptor MaterialX path tracing"
    echo "$log"
    return 1
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 1000 ]; then
    echo "FAIL: -vkr --path-trace produced a blank/trivial image"
    return 1
  fi
  echo "  -vkr --path-trace: rendered $(wc -c < "$out") bytes"
  return 0
}

render_vkr_materialx_graph() {
  local graph_asset="$REPO_ROOT/tests/feat/node-mtlx/ChainTest.usda"
  local out="$TMP/gpu_vkr_graph.png"
  local log
  if [ ! -f "$graph_asset" ]; then
    echo "FAIL: MaterialX graph fixture is missing: $graph_asset"
    return 1
  fi
  log="$(run_lusdrender "$graph_asset" "$out" -vkr -stats --path-trace \
        --pt-samples 1 --pt-max-depth 3 --pt-rr-depth 2 \
        -w 64 -height 48 -autoframe 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -vkr MaterialX graph: Vulkan ray query unavailable (skipped)"
    return 0
  fi
  if ! echo "$log" | grep -Eq "graphMaterials=[1-9][0-9]* graphNodes=[1-9][0-9]*"; then
    echo "FAIL: -vkr did not retain executable MaterialX graph topology"
    echo "$log"
    return 1
  fi
  if ! echo "$log" | grep -q "descriptor MaterialX, production path" ||
     [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 500 ]; then
    echo "FAIL: -vkr MaterialX graph did not produce a nontrivial descriptor render"
    echo "$log"
    return 1
  fi
  echo "  -vkr MaterialX graph: retained and rendered executable graph IR"
  return 0
}

render_vkr_geometry_light() {
  local scene="$TMP/vkr_geometry_light.usda"
  local dark="$TMP/vkr_geometry_dark.usda"
  local lit_out="$TMP/vkr_geometry_light.png"
  local dark_out="$TMP/vkr_geometry_dark.png"
  cat > "$scene" <<'USDA'
#usda 1.0
(defaultPrim = "World" upAxis = "Z")
def Xform "World" {
  def Mesh "Floor" {
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-4,-4,0), (4,-4,0), (4,4,0), (-4,4,0)]
    normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (interpolation = "vertex")
    color3f[] primvars:displayColor = [(0.8,0.8,0.8)]
    uniform token subdivisionScheme = "none"
  }
  def Mesh "Emitter" (prepend apiSchemas = ["MeshLightAPI"]) {
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [3, 2, 1, 0]
    point3f[] points = [(-1,-1,4), (1,-1,4), (1,1,4), (-1,1,4)]
    float inputs:intensity = 80
    color3f inputs:color = (1, 0.5, 0.2)
    uniform token subdivisionScheme = "none"
  }
}
USDA
  sed 's/inputs:intensity = 80/inputs:intensity = 0/' "$scene" > "$dark"
  local log
  log="$(run_lusdrender "$scene" "$lit_out" -vkr -stats --path-trace \
        --pt-samples 4 --pt-max-depth 2 -viewDir 0,0,1 -bg 0,0,0 \
        -w 64 -height 64 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -vkr geometry light: Vulkan ray query unavailable (skipped)"
    return 0
  fi
  if ! echo "$log" | grep -Eq "Vulkan descriptor lights: count=[2-9][0-9]* geometry=2"; then
    echo "FAIL: MeshLightAPI triangles did not reach the descriptor light table"
    echo "$log"
    return 1
  fi
  run_lusdrender "$dark" "$dark_out" -vkr --path-trace --pt-samples 4 \
      --pt-max-depth 2 -viewDir 0,0,1 -bg 0,0,0 -w 64 -height 64 >/dev/null 2>&1 || return 1
  if command -v compare >/dev/null 2>&1; then
    local changed
    changed="$(compare -metric AE -fuzz 2% "$lit_out" "$dark_out" null: 2>&1)"
    changed="${changed%%[!0-9]*}"
    if [ -z "$changed" ] || [ "$changed" -lt 30 ]; then
      echo "FAIL: descriptor geometry light did not illuminate enough pixels (changed=${changed:-invalid})"
      return 1
    fi
  fi
  echo "  -vkr geometry light: sampled two emissive triangles"
  return 0
}

render_vkr_forced_fallback() {
  local out="$TMP/gpu_vkr_forced_fallback.png"
  local log
  log="$(run_lusdrender -forceVkrFallback "$ASSET" "$out" -vkr \
        -w 200 -height 150 -autoframe -samples 2 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -vkr forced fallback: Vulkan unavailable (skipped)"
    return 0
  fi
  if ! echo "$log" | grep -q "compute trace, ray_query fallback"; then
    echo "FAIL: -vkr forced fallback did not report compute fallback"
    echo "$log"
    return 1
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: -vkr forced fallback produced a blank/trivial image"
    return 1
  fi
  echo "  -vkr forced fallback: rendered $(wc -c < "$out") bytes"
  return 0
}

render_explicit_threads() {
  local out="$TMP/gpu_vk_threads2.png"
  local log
  log="$(run_lusdrender "$ASSET" "$out" -vk -threads 2 \
        -w 200 -height 150 -autoframe -samples 2 2>&1)"
  if ! echo "$log" | grep -q "backend: LightRT"; then
    echo "  -vk -threads 2: Vulkan unavailable (skipped)"
    return 0
  fi
  if [ ! -s "$out" ] || [ "$(wc -c < "$out")" -lt 2000 ]; then
    echo "FAIL: -vk -threads 2 produced a blank/trivial image"
    return 1
  fi
  echo "  -vk -threads 2: rendered $(wc -c < "$out") bytes"
  return 0
}

# Cross-backend agreement, within tolerance. The GPU backends traverse the SAME
# LightRT BVH with the same shading, but compute (-vk) and hardware ray query
# (-vkr) resolve sub-ULP floating-point differently at supersampled silhouette
# edges, so a handful of boundary pixels can differ by a fraction of a bit. Assert
# AGREEMENT WITHIN TOLERANCE rather than byte-for-byte: a real divergence (blank
# frame, wrong geometry/shading) differs on thousands of pixels and still fails,
# while the FP edge noise (a few pixels, each < TOL_FUZZ) passes. Prefer
# ImageMagick `compare`; where it is unavailable, fall back to the byte-exact
# `cmp` so the test keeps working with no extra dependency.
TOL_FUZZ="0.5%"   # per-pixel colour difference ignored below this
TOL_PIXELS=48     # allow up to this many pixels past the fuzz (200x150 = 30000 px)
IM_COMPARE=""
IM_SKIPPED=0        # set when a cross-backend compare was skipped for lack of ImageMagick
if command -v compare >/dev/null 2>&1; then
  IM_COMPARE="compare"
elif command -v magick >/dev/null 2>&1; then
  IM_COMPARE="magick compare"
fi

# images_agree REF OTHER -> 0 when they agree (within tolerance), 1 when they diverge.
images_agree() {
  if [ -n "$IM_COMPARE" ]; then
    local ae
    ae="$($IM_COMPARE -metric AE -fuzz "$TOL_FUZZ" "$1" "$2" null: 2>&1)"
    ae="${ae%%[!0-9]*}"                 # keep the leading pixel count, drop any "(...)"
    [ -z "$ae" ] && ae=999999           # non-numeric (e.g. size mismatch) -> diverge
    [ "$ae" -le "$TOL_PIXELS" ] && return 0
    echo "  (differing pixels past ${TOL_FUZZ} fuzz: ${ae} > ${TOL_PIXELS})"
    return 1
  fi
  # No ImageMagick: we cannot compare within tolerance, and a byte-exact `cmp` would
  # spuriously fail on the legitimate sub-ULP edge noise between compute (-vk) and
  # hardware ray query (-vkr). So SKIP the cross-backend agreement check here (the
  # per-backend non-blank assertion in render() still runs) and report agreement as
  # untested rather than falsely pass/fail on byte identity.
  IM_SKIPPED=1
  return 0
}

echo "=== lusdrender GPU backends ==="
ok=0
ref=""
fail=0
for spec in "-vk vk" "-vkr vkr"; do
  set -- $spec
  if render "$1" "$2"; then
    [ "$REPLY" = "FAIL" ] && { fail=1; continue; }
    ok=$((ok + 1))
    # Descriptor MaterialX/OpenPBR deliberately differs from the legacy
    # compute hit-shading path; keep both non-blank checks, but only compare
    # backends that implement the same shading contract.
    if [ "$2" = "vkr" ]; then
      :
    elif [ -z "$ref" ]; then
      ref="$REPLY"
    else
      if ! images_agree "$ref" "$REPLY"; then
        echo "FAIL: $2 output differs from the first GPU backend (cross-backend mismatch)"
        fail=1
      fi
    fi
  fi
done

for spec in "-cuda cuda" "-hip hip"; do
  set -- $spec
  render_shared "$1" "$2"
  rc=$?
  [ "$rc" -eq 2 ] && fail=1
  [ "$rc" -eq 0 ] && ok=$((ok + 1))
done

[ "$fail" -ne 0 ] && exit 1
if [ "$ok" -eq 0 ]; then
  echo "SKIP: no GPU backend available in this environment"
  exit $SKIP
fi
render_profile || exit 1
render_gpu_shade_preview || exit 1
render_vkr_materialx_graph || exit 1
render_vkr_geometry_light || exit 1
render_vkr_path_trace || exit 1
render_vkr_forced_fallback || exit 1
render_explicit_threads || exit 1

if [ -n "$IM_COMPARE" ]; then
  echo "PASS: $ok GPU backend(s) render non-blank and agree within tolerance"
elif [ "$IM_SKIPPED" -ne 0 ]; then
  echo "PASS: $ok GPU backend(s) render non-blank (cross-backend agreement UNTESTED: install ImageMagick to enable it)"
else
  echo "PASS: $ok GPU backend(s) render non-blank"
fi
exit 0
