#!/usr/bin/env bash
# Local-only NVIDIA Ptex coverage. No CI target depends on this script because
# it requires a Vulkan RT-capable NVIDIA GPU and an external Ptex corpus.
set -euo pipefail

viewer="${LUSDVIEW:-build_ninja/lusdview}"
device="${LUSDVIEW_NVIDIA_DEVICE:-NVIDIA GeForce RTX 5060 Ti}"
files="${LUSDVIEW_LOCAL_PTEX_FILES:-}"
if [[ -z "$files" ]]; then
  echo "SKIP: set LUSDVIEW_LOCAL_PTEX_FILES to a colon-separated Ptex file list"
  exit 77
fi
if [[ ! -x "$viewer" ]]; then
  echo "FAIL: lusdview executable is missing: $viewer" >&2
  exit 1
fi
if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "SKIP: xvfb-run unavailable"
  exit 77
fi

out="$(mktemp -d "${TMPDIR:-/tmp}/lusdview-ptex-nvidia.XXXXXX")"
if [[ "${LUSDVIEW_LOCAL_KEEP:-0}" == "1" ]]; then
  trap 'echo "logs kept in $out"' EXIT
else
  trap 'rm -rf "$out"' EXIT
fi
index=0
IFS=: read -r -a ptex_files <<< "$files"
for ptex in "${ptex_files[@]}"; do
  if [[ ! -f "$ptex" ]]; then
    echo "FAIL: Ptex file does not exist: $ptex" >&2
    exit 1
  fi
  scene="$out/scene-$index.usda"
  cat > "$scene" <<EOF
#usda 1.0
def Camera "Camera" {
    float2 clippingRange = (0.1, 1000)
    double focalLength = 50
    double horizontalAperture = 20.955
    matrix4d xformOp:transform = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,5,1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
def Material "Mat" {
    token outputs:surface.connect = </Mat/Surface.outputs:surface>
    def Shader "Ptex" {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @$ptex@
        float3 outputs:rgb
    }
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </Mat/Ptex.outputs:rgb>
        token outputs:surface
    }
}
def Mesh "Mesh" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    rel material:binding = </Mat>
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
    uniform token subdivisionScheme = "none"
}
EOF
  base="$out/$index"
  common=(--core --texture-compress bc7 --texture-gpu vulkan
          "--texture-gpu-device=$device" --ptex-initial-faces 1
          --ptex-cache-mb 8 --texture-mips on --frames 2 --size 256x160
          --camera /Camera)

  env -u DISPLAY LUSDVIEW_VK_DEVICE=nvidia LUSDVIEW_RT_DEBUG=1 \
    timeout 180s "$viewer" \
    --headless --backend vk "${common[@]}" \
    --render-report "$base-vk.json" --screenshot "$base-vk.png" "$scene" \
    >"$base-vk.log" 2>&1
  grep -F "Vulkan device" "$base-vk.log" | grep -F "$device" >/dev/null
  grep -F "GPU texture processing: vulkan $device" "$base-vk.log" >/dev/null
  grep -F "texture-compress:" "$base-vk.log" >/dev/null

  env -u DISPLAY LUSDVIEW_VK_DEVICE=nvidia timeout 180s "$viewer" \
    --headless --backend vk --rt "${common[@]}" \
    --render-report "$base-rt.json" --screenshot "$base-rt.png" "$scene" \
    >"$base-rt.log" 2>&1
  grep -F "renderer: Vulkan (ray query), GPU: $device" "$base-rt.log" >/dev/null
  grep -F "RT texture table:" "$base-rt.log" >/dev/null
  grep -F "[vk_rt] direct compressed Ptex images:" "$base-rt.log" >/dev/null

  if [[ "${LUSDVIEW_LOCAL_SW_RT:-1}" == "1" ]]; then
    env -u DISPLAY LUSDVIEW_VK_DEVICE=nvidia LUSDVIEW_RT_FORCE_SW=1 \
      LUSDVIEW_RT_DEBUG=1 \
      timeout 180s "$viewer" --headless --backend vk --rt "${common[@]}" \
      --render-report "$base-swrt.json" --screenshot "$base-swrt.png" "$scene" \
      >"$base-swrt.log" 2>&1
    grep -F "renderer: Vulkan (compute BVH), GPU: $device" "$base-swrt.log" >/dev/null
    grep -F "[vk_swrt] direct compressed Ptex images:" "$base-swrt.log" >/dev/null
    grep -F "RT texture table:" "$base-swrt.log" >/dev/null
  fi

  env __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia \
    LUSDVIEW_VK_DEVICE=nvidia timeout 180s xvfb-run -a "$viewer" \
    --backend gl "${common[@]}" \
    --render-report "$base-gl.json" --screenshot "$base-gl.png" "$scene" \
    >"$base-gl.log" 2>&1
  grep -F "renderer: OpenGL, GPU: NVIDIA Corporation" "$base-gl.log" >/dev/null
  grep -F "texture-compress:" "$base-gl.log" >/dev/null
  echo "PASS: $ptex (Vulkan raster, Vulkan RT, OpenGL; device=$device)"
  index=$((index + 1))
done
