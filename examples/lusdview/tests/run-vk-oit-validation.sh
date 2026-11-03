#!/usr/bin/env bash
# Optional Vulkan validation smoke for weighted OIT. CI skips when no packaged
# or prebuilt Khronos layer is available; it never builds the layer itself.
set -uo pipefail
export LUSDVIEW_LOG=debug

SKIP=77
viewer="${LUSDVIEW:-build_ninja/lusdview}"
root="${LUSDVIEW_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"

if [[ ! -x "$viewer" ]]; then
  if [[ "${CI:-}" == "true" ]]; then
    echo "SKIP: lusdview executable was not built in this CI profile"
    exit "$SKIP"
  fi
  echo "FAIL: lusdview executable not found: $viewer" >&2
  exit 1
fi

candidate_paths=(
  "${VK_LAYER_PATH:-}"
  "$root/build_vulkan_validation/install/share/vulkan/explicit_layer.d"
  "$root/build_vulkan_validation/build/layers"
  "/usr/share/vulkan/explicit_layer.d"
  "/usr/local/share/vulkan/explicit_layer.d"
)
layer_dir=""
for candidate in "${candidate_paths[@]}"; do
  [[ -n "$candidate" && -d "$candidate" ]] || continue
  if find "$candidate" -maxdepth 2 -name VkLayer_khronos_validation.json \
      -print -quit | grep -q .; then
    layer_dir="$candidate"
    break
  fi
done
if [[ -z "$layer_dir" ]]; then
  echo "SKIP: Khronos validation layer package/prebuilt binary not found"
  exit "$SKIP"
fi

out="$(mktemp -d "${TMPDIR:-/tmp}/lusdview-vk-oit-validation.XXXXXX")"
trap 'rm -rf "$out"' EXIT
log="$out/validation.log"
validation_env=(
  "VK_LAYER_PATH=$layer_dir"
  "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
)
if [[ "${LUSDVIEW_VK_GPU_ASSISTED:-0}" == "1" ]]; then
  cat >"$out/vk_layer_settings.txt" <<'EOF'
khronos_validation.validate_gpu_based = GPU_BASED_GPU_ASSISTED
khronos_validation.gpuav_descriptor_checks = true
khronos_validation.gpuav_buffer_oob = true
EOF
  validation_env+=("VK_LAYER_SETTINGS_PATH=$out/vk_layer_settings.txt")
fi

if ! timeout 120s env "${validation_env[@]}" \
    "$viewer" --headless --backend vk --transparency weighted --frames 2 \
    --size 256x256 --view-dir 0,0,-1 --screenshot "$out/oit.ppm" \
    "$root/models/lusdview-transparency.usda" >"$log" 2>&1; then
  if grep -Eqi 'no compatible Vulkan|no Vulkan device|renderer init failed' "$log"; then
    cat "$log" >&2
    echo "SKIP: Vulkan backend unavailable"
    exit "$SKIP"
  fi
  cat "$log" >&2
  echo "FAIL: weighted OIT validation run failed" >&2
  exit 1
fi
if grep -Eqi 'VUID-|Validation Error|validation error|UNASSIGNED-.*\(ERROR' "$log"; then
  cat "$log" >&2
  echo "FAIL: Vulkan validation reported an OIT-path error" >&2
  exit 1
fi
if ! grep -q 'Vulkan weighted OIT resources ready' "$log"; then
  cat "$log" >&2
  echo "SKIP: weighted OIT unavailable on the selected Vulkan device"
  exit "$SKIP"
fi
[[ -s "$out/oit.ppm" ]] || {
  echo "FAIL: validated weighted OIT render produced no screenshot" >&2
  exit 1
}
echo "PASS: weighted OIT is clean under Khronos Vulkan validation"
