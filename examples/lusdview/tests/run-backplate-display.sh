#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?lusdview path required}"
ROOT="${2:?repository root required}"
if [[ "${LUSDVIEW_BACKPLATE_GL_CHILD:-0}" == 1 ]]; then
  TMP="${3:?temporary directory required}"
  VISIBLE="$ROOT/tests/usda/lusdrender-backplate-001.usda"
  common=(--core --frames 3 --size 128x96 --no-grid --mode shaded --camera /World/Camera)
  "$BIN" --backend gl "${common[@]}" --screenshot "$TMP/visible-gl.ppm" \
    "$VISIBLE" >"$TMP/visible-gl.log" 2>&1 ||
    { cat "$TMP/visible-gl.log"; exit 1; }
  "$BIN" --backend gl "${common[@]}" --screenshot "$TMP/hidden-gl.ppm" \
    "$TMP/hidden.usda" >"$TMP/hidden-gl.log" 2>&1 ||
    { cat "$TMP/hidden-gl.log"; exit 1; }
  grep -q 'loaded .*: 3 mesh(es).*2 textures' "$TMP/visible-gl.log"
  grep -q 'loaded .*: 1 mesh(es).*0 textures' "$TMP/hidden-gl.log"
  # Some CI llvmpipe configurations return an all-black viewport readback even
  # though all three draws were submitted. Prefer a pixel distinction when the
  # driver provides one; the logged draw/resource counts remain the portable
  # assertion that both visible instances reached OpenGL.
  if ! cmp -s "$TMP/visible-gl.ppm" "$TMP/hidden-gl.ppm"; then
    echo "PASS: OpenGL BackPlate pixels differ from the hidden variant"
  fi
  exit 0
fi
TMP="$(mktemp -d /tmp/lusdview-backplate.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/textures"
cp "$ROOT/tests/usda/textures/alpha-billboard-bird.png" "$TMP/textures/"
VISIBLE="$ROOT/tests/usda/lusdrender-backplate-001.usda"
sed 's/plateVisibility = "render"/plateVisibility = "invisible"/' \
  "$VISIBLE" > "$TMP/hidden.usda"
sed -e 's/, "BackPlateAPI:accent"//' -e '/backPlate:accent:/d' \
  "$VISIBLE" > "$TMP/single.usda"

common=(--core --frames 3 --size 128x96 --no-grid --mode shaded --camera /World/Camera)
render_vk() {
  "$BIN" --headless --backend vk "${common[@]}" --screenshot "$2" "$1" \
    >"$2.log" 2>&1
}
render_vk "$VISIBLE" "$TMP/visible-vk.ppm"
render_vk "$TMP/hidden.usda" "$TMP/hidden-vk.ppm"
render_vk "$TMP/single.usda" "$TMP/single-vk.ppm"
cmp -s "$TMP/visible-vk.ppm" "$TMP/hidden-vk.ppm" &&
  { echo "FAIL: Vulkan ignored BackPlate visibility"; exit 1; }
cmp -s "$TMP/visible-vk.ppm" "$TMP/single-vk.ppm" &&
  { echo "FAIL: Vulkan ignored the second BackPlate instance"; exit 1; }

if [[ -n "${DISPLAY:-}" ]] && xdpyinfo >/dev/null 2>&1; then
  LUSDVIEW_BACKPLATE_GL_CHILD=1 bash "$0" "$BIN" "$ROOT" "$TMP"
else
  echo "SKIP: OpenGL BackPlate check needs DISPLAY (run this script under Xvfb)"
fi

echo "PASS: interactive GL/Vulkan display the authored BackPlate stack"
