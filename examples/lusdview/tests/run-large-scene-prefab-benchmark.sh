#!/usr/bin/env bash
# Benchmark an interactive large-scene prefab through the same path used by
# the windowed viewer. The scene is external and this test is intentionally
# opt-in; it reports render-data setup, first-useful-frame and full-upload
# timings from --timing. Set GPU_ACCEL=nvidia on NVIDIA/Xvfb hosts to use the
# documented hardware-accelerated GL path.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build_ninja/lusdview}"
SCENE="${SCENE:-${CALDERA_PREFAB:-}}"
OUT_DIR="${OUT_DIR:-${TMPDIR:-/tmp}/lusdview-large-scene-prefab}"
PROFILE="${PROFILE:-}"
PAYLOAD_MODE="${PAYLOAD_MODE:-defer}"
PREVIEW_CACHE="${PREVIEW_CACHE:-auto}"
CONVERT_THREADS="${CONVERT_THREADS:-8}"
COMPOSE_THREADS="${COMPOSE_THREADS:-8}"
SIZE="${SIZE:-1280x720}"
FULL_FIDELITY="${FULL_FIDELITY:-0}"
ENDPOINT="${ENDPOINT:-upload}"
EXTRA_ARGS="${EXTRA_ARGS:-}"
GPU_ACCEL="${GPU_ACCEL:-auto}"

if [ -z "$SCENE" ]; then
  echo "SKIP: set SCENE or CALDERA_PREFAB to the prefab USD path"
  exit 77
fi
if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW"
  exit 77
fi
if [ ! -f "$SCENE" ]; then
  echo "FAIL: scene not found: $SCENE"
  exit 1
fi

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/$(basename "$SCENE").log"
REPORT="${REPORT:-$OUT_DIR/$(basename "$SCENE").report.json}"

if command -v xvfb-run >/dev/null 2>&1; then
  if [ "$GPU_ACCEL" = nvidia ]; then
    RUN=(xvfb-run -a env
      __NV_PRIME_RENDER_OFFLOAD=1
      __GLX_VENDOR_LIBRARY_NAME=nvidia
      __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json)
  else
    RUN=(xvfb-run -a)
  fi
else
  RUN=()
fi

echo "scene: $SCENE"
echo "binary: $LUSDVIEW"
echo "log: $LOG"

VIEW_ARGS=(
  --backend gl
  --compose-threads "$COMPOSE_THREADS"
  --convert-threads "$CONVERT_THREADS"
  --timing
  --render-report "$REPORT"
  --size "$SIZE"
  "$SCENE"
)
case "$ENDPOINT" in
  convert) VIEW_ARGS=(--quit-after-convert "${VIEW_ARGS[@]}") ;;
  upload) VIEW_ARGS=(--quit-after-full-upload "${VIEW_ARGS[@]}") ;;
  *) echo "FAIL: ENDPOINT must be convert or upload"; exit 1 ;;
esac
if [ "$FULL_FIDELITY" = 1 ]; then
  VIEW_ARGS=(--full-fidelity "${VIEW_ARGS[@]}")
fi
case "$PAYLOAD_MODE" in
  defer) VIEW_ARGS=(--defer-payloads "${VIEW_ARGS[@]}" ) ;;
  load) VIEW_ARGS=(--load-payloads "${VIEW_ARGS[@]}" ) ;;
  *) echo "FAIL: PAYLOAD_MODE must be defer or load"; exit 1 ;;
esac
case "$PREVIEW_CACHE" in
  off|auto|refresh) VIEW_ARGS=(--preview-cache "$PREVIEW_CACHE" "${VIEW_ARGS[@]}" ) ;;
  *) echo "FAIL: PREVIEW_CACHE must be off, auto, or refresh"; exit 1 ;;
esac
if [ -n "$PROFILE" ]; then
  VIEW_ARGS=(--large-scene-profile "$PROFILE" "${VIEW_ARGS[@]}")
fi
if [ -n "$EXTRA_ARGS" ]; then
  read -r -a EXTRA_ARGV <<< "$EXTRA_ARGS"
  VIEW_ARGS=("${EXTRA_ARGV[@]}" "${VIEW_ARGS[@]}")
fi

set +e
/usr/bin/time -v "${RUN[@]}" "$LUSDVIEW" "${VIEW_ARGS[@]}" >"$LOG" 2>&1
RC=$?
set -e

cat "$LOG"
if [ "$RC" -ne 0 ]; then
  echo "FAIL: lusdview exited with $RC"
  exit "$RC"
fi
if [ "$GPU_ACCEL" = nvidia ] &&
   ! grep -q "renderer: OpenGL, GPU: NVIDIA" "$LOG"; then
  echo "FAIL: GPU_ACCEL=nvidia did not select an NVIDIA OpenGL renderer"
  exit 1
fi

markers=("timing: first useful frame" "timing: full scene converted" \
         "core timing: geometry estimates")
if [ "$ENDPOINT" = upload ]; then
  markers+=("timing: full scene uploaded and presented")
fi
for marker in "${markers[@]}"; do
  if ! grep -q "$marker" "$LOG"; then
    echo "FAIL: timing marker missing: $marker"
    exit 1
  fi
done

echo "PASS: large-scene prefab benchmark completed"
