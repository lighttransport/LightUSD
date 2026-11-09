#!/usr/bin/env bash
# Verify that a cold MaterialX interpreter shader fails fast and
# leaves the compact Vulkan RT pipeline available for rendering.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
# Direct image/normalmap graphs in OpenChess are now handled by the compact
# shader. Use a procedural graph that actually requires full promotion.
ASSET="${ASSET:-$REPO_ROOT/tests/usda/lusdview-materialx-cold-promotion.usda}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW"
  exit "$SKIP"
fi
if [ ! -f "$ASSET" ]; then
  echo "SKIP: cold MaterialX fallback asset not found at $ASSET"
  exit "$SKIP"
fi
if ! command -v nvidia-smi >/dev/null 2>&1 ||
   ! nvidia-smi -L >/dev/null 2>&1; then
  echo "SKIP: NVIDIA GPU is not available"
  exit "$SKIP"
fi
if command -v vulkaninfo >/dev/null 2>&1; then
  if ! vulkaninfo --summary 2>/dev/null | grep -q 'NVIDIA'; then
    echo "SKIP: Vulkan NVIDIA device is not visible"
    exit "$SKIP"
  fi
else
  echo "SKIP: vulkaninfo is not available to identify the NVIDIA Vulkan device"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
OUT="$TMP/full-fallback.png"
LOGFILE="$TMP/lusdview.log"

# Warm only the compact shader. The full MaterialX variant must remain cold,
# while the hardware fallback needs a ready compact cache to stay on ray query.
env LUSDVIEW_VK_PIPELINE_CACHE_DIR="$TMP/cache" \
  LUSDVIEW_RT_ALLOW_COLD_COMPILE=1 LUSDVIEW_NVIDIA_OFFLOAD=1 \
  timeout --kill-after=5s "${LUSDVIEW_MTLX_VK_WARMUP_TIMEOUT:-300s}" \
  "$LUSDVIEW" --headless --backend vk --vk-device nvidia --rt \
  --frames 1 --size 32x32 --screenshot "$TMP/warmup.ppm" \
  "$REPO_ROOT/tests/usda/cube.usda" >"$TMP/warmup.log" 2>&1
WARMUP_RC=$?
if [ "$WARMUP_RC" -ne 0 ] ||
   ! grep -q 'device=discrete rt=hardware' "$TMP/warmup.log"; then
  cat "$TMP/warmup.log"
  echo "FAIL: compact hardware pipeline warmup failed (exit $WARMUP_RC)"
  exit 1
fi

set +e
if command -v timeout >/dev/null 2>&1; then
  env LUSDVIEW_VK_PIPELINE_CACHE_DIR="$TMP/cache" \
    LUSDVIEW_RT_ALLOW_COLD_COMPILE=0 \
    LUSDVIEW_NVIDIA_OFFLOAD=1 \
    timeout --kill-after=10s "${LUSDVIEW_MTLX_VK_FALLBACK_TIMEOUT:-60s}" \
    "$LUSDVIEW" --headless --backend vk --vk-device nvidia --rt \
    --path-trace --pt-samples 1 \
    --frames 1 --size 32x32 --materialx-vk-shader-max-kib 256 \
    --screenshot "$OUT" "$ASSET" >"$LOGFILE" 2>&1
else
  env LUSDVIEW_VK_PIPELINE_CACHE_DIR="$TMP/cache" \
    LUSDVIEW_RT_ALLOW_COLD_COMPILE=0 \
    LUSDVIEW_NVIDIA_OFFLOAD=1 \
    "$LUSDVIEW" --headless --backend vk --vk-device nvidia --rt \
    --path-trace --pt-samples 1 \
    --frames 1 --size 32x32 --materialx-vk-shader-max-kib 256 \
    --screenshot "$OUT" "$ASSET" >"$LOGFILE" 2>&1
fi
RC=$?
set -e
cat "$LOGFILE"

if [ "$RC" -ne 0 ]; then
  echo "FAIL: cold MaterialX fallback command failed (exit $RC)"
  exit 1
fi
if ! grep -q 'device=discrete rt=hardware' "$LOGFILE"; then
  echo "FAIL: test did not run on a hardware ray-query device"
  exit 1
fi
if ! grep -q 'requires a cold driver compile' "$LOGFILE"; then
  echo "FAIL: full shader did not report compile-required fallback"
  exit 1
fi
if [ ! -s "$OUT" ]; then
  echo "FAIL: compact fallback did not produce a screenshot"
  exit 1
fi

echo "PASS: cold full MaterialX pipeline failed fast and compact RT rendered"
