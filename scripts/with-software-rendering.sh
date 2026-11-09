#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Explicit Mesa software rendering for development without hardware GPU access.
set -euo pipefail
if [ "$#" -eq 0 ]; then
  echo "Usage: $0 command [arguments...]" >&2
  exit 2
fi
icd="${LIGHTUSD_SOFTWARE_VULKAN_ICD:-}"
if [ -z "$icd" ]; then
  for candidate in /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.x86_64.json; do
    if [ -f "$candidate" ]; then icd="$candidate"; break; fi
  done
fi
if [ -z "$icd" ] || [ ! -f "$icd" ]; then
  echo "Mesa lavapipe ICD not found; set LIGHTUSD_SOFTWARE_VULKAN_ICD." >&2
  exit 1
fi
export VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd"
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
# Pass --preview-software explicitly to preview applications.
export LP_NUM_THREADS="${LP_NUM_THREADS:-4}"
export __GLX_VENDOR_LIBRARY_NAME=mesa
unset __NV_PRIME_RENDER_OFFLOAD __NV_PRIME_RENDER_OFFLOAD_PROVIDER __VK_LAYER_NV_optimus
unset LUSDVIEW_VK_DEVICE LUSDVIEW_VK_GPU_ASSISTED
export CUDA_VISIBLE_DEVICES=""
if [ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ]; then
  export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
fi
exec "$@"
