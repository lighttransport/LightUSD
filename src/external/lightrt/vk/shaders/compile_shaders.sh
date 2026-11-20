#!/usr/bin/env bash
#
# Regenerate the checked-in SPIR-V C headers from the vk/shaders/*.comp sources.
#
# These headers (vk/shaders/*.spv.h) are committed, so a normal LightRT build
# needs NO shader toolchain — the runtime just hands the embedded uint32_t[]
# array to vkCreateShaderModule. Run this only after editing a .comp; it needs
# glslangValidator (Vulkan SDK) or, as a fallback, glslc (shaderc).
#
# Usage:  src/external/lightrt/vk/shaders/compile_shaders.sh
#         GLSLANG=/path/to/glslangValidator vk/shaders/compile_shaders.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# The .comp sources and committed .spv.h headers live alongside this script.
SH="${SCRIPT_DIR}"

# Prefer the locally built glslang (scripts/setup-and-build-glslang.sh): the
# trace_ray_query shader needs GL_EXT_ray_query, which older system glslang
# (Vulkan SDK < 1.2 / glslang < 11) does not support.
if [[ -z "${GLSLANG:-}" ]]; then
    if [[ -x "${REPO_ROOT}/tools/bin/glslangValidator" ]]; then
        GLSLANG="${REPO_ROOT}/tools/bin/glslangValidator"
    else
        GLSLANG="glslangValidator"
    fi
fi

if ! command -v "${GLSLANG}" >/dev/null 2>&1; then
    echo "error: ${GLSLANG} not found. Run scripts/setup-and-build-glslang.sh," >&2
    echo "       install the Vulkan SDK, or set GLSLANG=..." >&2
    exit 1
fi

# gen <out-basename> <symbol-name> <src-basename> [glslang-args...]
gen() {
    local out="$1" sym="$2" src="$3"
    shift 3
    "${GLSLANG}" -V --target-env vulkan1.2 --vn "${sym}" "$@" \
        -o "${SH}/${out}.spv.h" "${SH}/${src}.comp"
    echo "generated ${SH}/${out}.spv.h  (${sym})"
}

gen_glslc() {
    local out="$1" sym="$2" src="$3"
    shift 3
    local tmp="${SH}/${out}.words.tmp"
    "${GLSLC}" --target-env=vulkan1.2 -mfmt=num "$@" \
        -o "${tmp}" "${SH}/${src}.comp"
    {
        printf 'const uint32_t %s[] = {\n' "${sym}"
        cat "${tmp}"
        printf '\n};\n'
    } > "${SH}/${out}.spv.h"
    rm -f "${tmp}"
    echo "generated ${SH}/${out}.spv.h  (${sym}, glslc)"
}

if [[ "${ONLY:-}" != "trace_materialx_path" ]]; then
    gen trace_bvh        trace_bvh_spv        trace_bvh
    gen build_morton     build_morton_spv     build_morton
    gen trace_ray_query  trace_ray_query_spv  trace_ray_query  # needs VK_KHR_ray_query (SPIR-V 1.4)
    # Wide-id variant of the ray-query trace: 5 words/hit with instanceId + prim index
    # stored separately, so instanced scenes past the 32-bit prim_id product still trace.
    gen trace_ray_query_wide trace_ray_query_wide_spv trace_ray_query -DLRT_WIDE_ID
    gen shade_analytic   shade_analytic_spv   shade_analytic   # analytic sphere/box forward shading
    gen trace_gsplat     trace_gsplat_spv     trace_gsplat     # 3D Gaussian splat chunks (lightrt_c_gsplat)
fi
if [[ -n "${GLSLC:-}" ]]; then
    gen_glslc trace_materialx_path trace_materialx_path_spv trace_materialx_path
    gen_glslc trace_materialx_gsplat_path trace_materialx_gsplat_path_spv trace_materialx_path -DLRT_PATH_GSPLAT
else
    gen trace_materialx_path trace_materialx_path_spv trace_materialx_path
    gen trace_materialx_gsplat_path trace_materialx_gsplat_path_spv trace_materialx_path -DLRT_PATH_GSPLAT
fi

echo "done."
