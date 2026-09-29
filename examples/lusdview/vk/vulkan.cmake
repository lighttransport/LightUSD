# lusdview - Vulkan backend wiring. Included from examples/lusdview/CMakeLists.txt
# when LUSDVIEW_WITH_VULKAN is ON. EXAMPLE_TARGET / COMMON_DIR are in scope.
#
# The Vulkan API is resolved at runtime via the vendored volk meta-loader
# (examples/common/volk) — cuew-style, no link against the Vulkan loader and no
# Vulkan SDK required to build.
#
# Shaders are pre-compiled to SPIR-V and committed as C headers under
# vk/shaders/embedded/, so the build needs NO glslang and produces a single,
# self-contained binary. Regenerate those headers with vk/shaders/build-shaders.sh
# whenever a shader changes (that script is the only thing that needs glslang).

# --- Pre-compiled (embedded) SPIR-V ----------------------------------------
set(LUSDVIEW_SHADER_EMBED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vk/shaders/embedded)

# Core raster and environment shader headers are mandatory for the Vulkan backend.
set(_required_spv
    ${LUSDVIEW_SHADER_EMBED_DIR}/mesh_vert.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/mesh_frag.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/mesh_oit_simple_frag.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/wire_frag.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/wire_vert.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/environment_vert.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/environment_frag.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/line_vert.spv.h
    ${LUSDVIEW_SHADER_EMBED_DIR}/line_frag.spv.h)
set(_missing_spv "")
foreach(h ${_required_spv})
  if(NOT EXISTS ${h})
    list(APPEND _missing_spv ${h})
  endif()
endforeach()
if(_missing_spv)
  message(STATUS
    "lusdview: Vulkan backend SKIPPED — embedded SPIR-V headers missing "
    "(${_missing_spv}). Generate them with vk/shaders/build-shaders.sh, then "
    "reconfigure. Building GL-only. Disable with -DLUSDVIEW_WITH_VULKAN=OFF.")
  return()  # leave the target as a GL-only build
endif()

# The ray-query compute shader is optional: present only when it was generated
# by a glslang new enough for GL_EXT_ray_query. Absent -> RT path compiled out
# (LUSDVIEW_HAVE_RT_SHADER=0) and Vulkan falls back to rasterization.
if(EXISTS ${LUSDVIEW_SHADER_EMBED_DIR}/raytrace_comp.spv.h)
  set(_have_rt_shader 1)
  message(STATUS "lusdview: Vulkan backend ENABLED (runtime-loaded via volk; embedded SPIR-V, ray query ON)")
else()
  set(_have_rt_shader 0)
  message(STATUS "lusdview: Vulkan backend ENABLED (runtime-loaded via volk; embedded SPIR-V, rasterization only — no raytrace_comp.spv.h)")
endif()

# Compute-BVH ray-tracing fallback (raytrace_swbvh.comp): used on GPUs without
# hardware ray-query/acceleration-structure support so --rt still ray traces
# instead of dropping straight to rasterization. Independent of _have_rt_shader
# above -- needs no GL_EXT_ray_query, so it can be present even when the
# hardware shader isn't (e.g. an older glslang), or vice versa.
if(EXISTS ${LUSDVIEW_SHADER_EMBED_DIR}/raytrace_swbvh_comp.spv.h)
  set(_have_swrt_shader 1)
  message(STATUS "lusdview: Vulkan compute-BVH ray-tracing fallback ENABLED")
else()
  set(_have_swrt_shader 0)
  message(STATUS "lusdview: Vulkan compute-BVH ray-tracing fallback DISABLED — no raytrace_swbvh_comp.spv.h (regenerate with build-shaders.sh)")
endif()

# GPU compute skinning for the RT vertex stream (skin.comp): optional like the
# ray-query shader. Absent -> per-frame RT skinning falls back to the CPU path.
if(EXISTS ${LUSDVIEW_SHADER_EMBED_DIR}/skin_comp.spv.h)
  set(_have_skin_shader 1)
else()
  set(_have_skin_shader 0)
endif()

# --- Backend sources + link ------------------------------------------------
# volk (meta-loader) + vendored Vulkan headers. VK_NO_PROTOTYPES makes both the
# Vulkan headers and volk declare the vk* entry points as function pointers
# (no link-time symbols); IMGUI_IMPL_VULKAN_USE_VOLK makes imgui_impl_vulkan
# pull in volk.h and dispatch through the same pointers.
set(LUSDVIEW_VULKAN_INCLUDE ${COMMON_DIR}/vulkan-headers/include)
set(LUSDVIEW_VOLK_DIR ${COMMON_DIR}/volk)

target_sources(${EXAMPLE_TARGET} PRIVATE
    vk/vk_renderer.cc
    ${LUSDVIEW_VOLK_DIR}/volk.c
    ${COMMON_DIR}/imgui/imgui_impl_vulkan.cpp)
target_compile_definitions(${EXAMPLE_TARGET} PRIVATE HAVE_VULKAN=1
    VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK
    LUSDVIEW_HAVE_RT_SHADER=${_have_rt_shader}
    LUSDVIEW_HAVE_SWRT_SHADER=${_have_swrt_shader}
    LUSDVIEW_HAVE_SKIN_SHADER=${_have_skin_shader})
target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${LUSDVIEW_SHADER_EMBED_DIR}
    ${LUSDVIEW_VULKAN_INCLUDE}
    ${LUSDVIEW_VOLK_DIR})

# Optional GPU texture preprocessing.  This reuses the headless processor used
# by tools/lusdview-texture-bench, but its Vulkan commands use a local volk
# device table so it cannot disturb the renderer's active dispatch table.
find_program(LUSDVIEW_TEXTURE_GLSLANG glslangValidator)
if(LUSDVIEW_TEXTURE_GLSLANG)
  set(LUSDVIEW_TEXTURE_GPU_GEN ${CMAKE_CURRENT_BINARY_DIR}/texture_gpu_generated)
  file(MAKE_DIRECTORY ${LUSDVIEW_TEXTURE_GPU_GEN})
  add_custom_command(
    OUTPUT ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_comp.spv.h
           ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_hdr_comp.spv.h
           ${LUSDVIEW_TEXTURE_GPU_GEN}/compress_comp.spv.h
           ${LUSDVIEW_TEXTURE_GPU_GEN}/bc6h_comp.spv.h
    COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2 --vn resize_comp_spv
            -o ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_comp.spv.h
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/resize.comp
    COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2 --vn resize_hdr_comp_spv
            -o ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_hdr_comp.spv.h
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/resize_hdr.comp
    COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2 --vn compress_comp_spv
            -o ${LUSDVIEW_TEXTURE_GPU_GEN}/compress_comp.spv.h
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/compress.comp
    COMMAND ${LUSDVIEW_TEXTURE_GLSLANG} -V --target-env vulkan1.2 --vn bc6h_comp_spv
            -o ${LUSDVIEW_TEXTURE_GPU_GEN}/bc6h_comp.spv.h
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/bc6h.comp
    DEPENDS ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/resize.comp
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/resize_hdr.comp
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/compress.comp
            ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vk/bc6h.comp
    VERBATIM)
  add_custom_target(lusdview_texture_gpu_shaders_viewer
    DEPENDS ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_comp.spv.h
            ${LUSDVIEW_TEXTURE_GPU_GEN}/resize_hdr_comp.spv.h
            ${LUSDVIEW_TEXTURE_GPU_GEN}/compress_comp.spv.h
            ${LUSDVIEW_TEXTURE_GPU_GEN}/bc6h_comp.spv.h)
  target_sources(${EXAMPLE_TARGET} PRIVATE
      ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/texture_gpu.cc
      ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/vulkan_processor.cc)
  add_dependencies(${EXAMPLE_TARGET} lusdview_texture_gpu_shaders_viewer)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE LUSDVIEW_TEXTURE_GPU=1
      LUSDVIEW_TEXTURE_GPU_VULKAN=1)
  target_include_directories(${EXAMPLE_TARGET} PRIVATE
      ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench
      ${LUSDVIEW_TEXTURE_GPU_GEN})
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    target_sources(${EXAMPLE_TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/cuda_processor.cu)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/cuda_processor.cu
        PROPERTIES LANGUAGE CUDA)
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        CUDA_ARCHITECTURES "native")
    target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
        LUSDVIEW_TEXTURE_HAVE_CUDA=1)
    message(STATUS "lusdview: CUDA texture preprocessing ENABLED")
  endif()
  check_language(HIP)
  if(CMAKE_HIP_COMPILER)
    enable_language(HIP)
    if(NOT TARGET lusdview_texture_hip_plugin)
      add_library(lusdview_texture_hip_plugin MODULE
          ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/hip_processor.hip)
      target_include_directories(lusdview_texture_hip_plugin PRIVATE
          ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench)
      set(_lusdview_hip_architectures "${CMAKE_HIP_ARCHITECTURES}")
      if(NOT _lusdview_hip_architectures)
        set(_lusdview_hip_architectures native)
      endif()
      set_target_properties(lusdview_texture_hip_plugin PROPERTIES
          HIP_ARCHITECTURES "${_lusdview_hip_architectures}"
          PREFIX "")
    endif()
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/tools/lusdview-texture-bench/hip_processor.hip
        PROPERTIES LANGUAGE HIP)
    target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
        LUSDVIEW_TEXTURE_HAVE_HIP=1 LUSDVIEW_TEXTURE_HIP_DYNAMIC=1
        LUSDVIEW_TEXTURE_HIP_PLUGIN_PATH="$<TARGET_FILE:lusdview_texture_hip_plugin>")
    add_dependencies(${EXAMPLE_TARGET} lusdview_texture_hip_plugin)
    message(STATUS "lusdview: HIP texture preprocessing ENABLED via hipew-isolated runtime")
  endif()
  message(STATUS "lusdview: Vulkan GPU texture preprocessing ENABLED")
else()
  message(STATUS "lusdview: Vulkan GPU texture preprocessing disabled (glslangValidator missing)")
endif()
# volk uses dlopen() to find the loader at runtime on non-Windows platforms.
if(UNIX AND NOT APPLE)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE ${CMAKE_DL_LIBS})
endif()
