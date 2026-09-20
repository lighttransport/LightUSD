# lusdview - OpenGL/Vulkan USD viewer example.
# Configured from the top-level lightusd CMake, so PROJECT_SOURCE_DIR is the
# lightusd repo root.

set(EXAMPLE_TARGET "lusdview")
set(COMMON_DIR ${PROJECT_SOURCE_DIR}/examples/common)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# Bundled GLFW (guard against another example also adding it).
if(NOT TARGET glfw)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${COMMON_DIR}/glfw-3.3.6 ${CMAKE_BINARY_DIR}/_deps/lusdview-glfw EXCLUDE_FROM_ALL)
endif()

# Vulkan backend. Entry points are resolved at runtime via the vendored volk
# meta-loader (examples/common/volk) — we do NOT link the Vulkan SDK loader, so
# no find_package(Vulkan) / Vulkan::Vulkan dependency. Only a build-time glslang
# is needed to compile the GLSL shaders to SPIR-V; if it is missing the Vulkan
# backend is skipped and the app builds GL-only.
#
# find_package(Vulkan) is still probed (QUIET) purely so vulkan.cmake can fall
# back to the SDK's glslangValidator when no local/system one is found.
option(LUSDVIEW_WITH_VULKAN "Build the Vulkan backend (runtime-loaded via volk)" ON)
set(LUSDVIEW_RT_PTEXTURE_IMAGE_CAP "256" CACHE STRING
    "Maximum source texture slots available to Vulkan RT compressed Ptex sampling")
if(NOT LUSDVIEW_RT_PTEXTURE_IMAGE_CAP MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "LUSDVIEW_RT_PTEXTURE_IMAGE_CAP must be a positive integer")
endif()
find_package(Vulkan QUIET)

# Optional NVIDIA OptiX backend. OptiX is a driver API and is loaded at runtime;
# the build only consumes headers from a separately installed/pinned optix-dev
# checkout. We intentionally do not copy the SDK headers into this repository
# because current releases contain a mixture of BSD and proprietary headers.
set(LIGHTUSD_LUSDVIEW_OPTIX "AUTO" CACHE STRING
    "Build OptiX support for CUDA RT: AUTO, ON, or OFF")
set_property(CACHE LIGHTUSD_LUSDVIEW_OPTIX PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${LIGHTUSD_LUSDVIEW_OPTIX}" _lusdview_optix_mode)
if(NOT _lusdview_optix_mode MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR
      "LIGHTUSD_LUSDVIEW_OPTIX must be AUTO, ON, or OFF")
endif()

set(LUSDVIEW_HAVE_OPTIX OFF)
if(NOT _lusdview_optix_mode STREQUAL "OFF")
  set(OptiX_ROOT "" CACHE PATH
      "Path to a local NVIDIA optix-dev/OptiX SDK checkout")
  find_path(LUSDVIEW_OPTIX_INCLUDE_DIR optix_function_table.h
      HINTS ${OptiX_ROOT} ENV OptiX_ROOT ENV OPTIX_ROOT
      PATH_SUFFIXES include)
  find_path(LUSDVIEW_CUDA_INCLUDE_DIR cuda.h
      HINTS ENV CUDA_PATH ENV CUDA_HOME
      PATHS /usr/local/cuda /opt/cuda
      PATH_SUFFIXES include targets/x86_64-linux/include)
  if(LUSDVIEW_OPTIX_INCLUDE_DIR AND LUSDVIEW_CUDA_INCLUDE_DIR)
    set(LUSDVIEW_HAVE_OPTIX ON)
    message(STATUS
        "lusdview OptiX: enabled (${LUSDVIEW_OPTIX_INCLUDE_DIR})")
  elseif(_lusdview_optix_mode STREQUAL "ON")
    message(FATAL_ERROR
        "OptiX was requested but optix_function_table.h or cuda.h was not found. "
        "Set OptiX_ROOT to a local optix-dev checkout and CUDA_PATH to a CUDA toolkit.")
  else()
    message(STATUS
        "lusdview OptiX: disabled (set OptiX_ROOT to optix-dev and CUDA_PATH if needed)")
  endif()
endif()

# Optional native file dialog (needs GTK3 on Linux). Skipped if unavailable.
set(LUSDVIEW_HAVE_NFD OFF)
if(WIN32 OR APPLE)
  set(LUSDVIEW_HAVE_NFD ON)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
    if(GTK3_FOUND)
      set(LUSDVIEW_HAVE_NFD ON)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(LUSDVIEW_SOURCES
    main.cc
    app.cc
    config.cc
    scene_loader.cc
    scene_optimize.cc
    incremental_scene_update.cc
    next_scene_loader.cc
    preview_cache.cc
    texture_residency_policy.cc
    dome_light.cc
    mesh_build.cc
    gpu_budget_lod.cc
    rt_lod.cc
    gizmo_build.cc
    parametric_tess.cc
    skinning.cc
    ${PROJECT_SOURCE_DIR}/examples/vchar/autorigger_client.cc
    vchar_control_map.cc
    camera_nav.cc
    camera_stereo.cc
    frame_packet.cc
    gui.cc
    gui_stringify.cc
    gui_style.cc
    gl/gl_util.cc
    gl/gl_renderer.cc
    cpu/cpu_raytracer.cc
    lod_stream.cc)

set(LUSDVIEW_COMMON_SOURCES
    ${COMMON_DIR}/glad/src/glad.c
    ${COMMON_DIR}/imgui/imgui.cpp
    ${COMMON_DIR}/imgui/imgui_draw.cpp
    ${COMMON_DIR}/imgui/imgui_tables.cpp
    ${COMMON_DIR}/imgui/imgui_widgets.cpp
    ${COMMON_DIR}/imgui/imgui_impl_glfw.cpp
    ${COMMON_DIR}/imgui/imgui_impl_opengl3.cpp
    ${COMMON_DIR}/light3d/camera.cpp
    ${COMMON_DIR}/light3d/material.cpp
    ${COMMON_DIR}/light3d/texture.cpp)

if(MSVC)
  list(APPEND LUSDVIEW_COMMON_SOURCES
       ${COMMON_DIR}/light3d/material.rc)
endif()

# hipew intentionally uses variables named like HIP entry points. Keep those
# loader variables out of the executable's dynamic symbol table so a directly
# compiled HIP translation unit resolves hipGetDeviceCount/etc. to libamdhip64
# rather than interposing on hipew's function-pointer variables.
if(UNIX AND NOT APPLE)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/src/external/hipew/hipew.c
                              PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
endif()

# Compile the vendored LightRT MaterialX/OpenPBR evaluator core. The viewer uses
# the same texture-cache implementation as LightRT now; the cache is populated
# from the material's asset directory before evaluation and owns its decoded
# image lifetime. `material_bind.c` stays vendored but is excluded because it is
# glTF-specific rather than USD-facing.
set(LUSDVIEW_LIGHTRT_MTLX_DIR ${PROJECT_SOURCE_DIR}/src/external/lightrt/mtlxrender)
add_library(lusdview_lightrt_mtlx OBJECT
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/bsdf.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/color.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/env.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_doc.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_eval.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_xml.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/texture.c
    lightrt_mtlx_image_impl.c)
target_include_directories(lusdview_lightrt_mtlx PRIVATE
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}
    ${PROJECT_SOURCE_DIR}/src/external
    ${PROJECT_SOURCE_DIR}/src/external/tinyexr/include)
target_compile_features(lusdview_lightrt_mtlx PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lusdview_lightrt_mtlx PRIVATE -w)
endif()

add_library(lusdview_lightrt_mtlx_const OBJECT
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/color.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_doc.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_eval.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_xml.c
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}/texture.c
    lightrt_mtlx_image_impl.c)
target_include_directories(lusdview_lightrt_mtlx_const PRIVATE
    ${LUSDVIEW_LIGHTRT_MTLX_DIR}
    ${PROJECT_SOURCE_DIR}/src/external)
target_compile_features(lusdview_lightrt_mtlx_const PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lusdview_lightrt_mtlx_const PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# CUDA/HIP trace kernel source (shared by cuda/cuda_raytracer.cc and
# hip/hip_raytracer.cc, compiled at runtime by NVRTC/hiprtc). Its single
# on-disk copy is raytracer_kernel_src.txt (plain CUDA C, no C++ wrapper).
#
# - MSVC: embedded as a Win32 RCDATA resource (raytracer_kernel_resource.rc)
#   and loaded at runtime (kernel_resource.cc), because a single MSVC string
#   literal is capped at ~16,380 bytes (C2026), well under this kernel's size.
# - Everyone else: GCC/Clang have no such limit, so it's wrapped as a plain
#   C++ raw-string literal and `#include`d directly. That wrapped file is
#   generated here (re-run cmake after editing raytracer_kernel_src.txt).
# ---------------------------------------------------------------------------
if(NOT MSVC)
  set(LUSDVIEW_KERNEL_SRC_TXT ${CMAKE_CURRENT_SOURCE_DIR}/raytracer_kernel_src.txt)
  set(LUSDVIEW_KERNEL_INC_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/raytracer_kernel.inc)
  # The generated literal participates in the CUDA/HIP persistent-cache key.
  # Make a kernel edit trigger CMake regeneration automatically; otherwise an
  # ordinary `cmake --build` can silently keep compiling/loading stale PTX.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${LUSDVIEW_KERNEL_SRC_TXT})
  file(READ ${LUSDVIEW_KERNEL_SRC_TXT} LUSDVIEW_KERNEL_SRC_CONTENT)
  file(WRITE ${LUSDVIEW_KERNEL_INC_GEN}
       "const char* kKernelSrc = R\"CUDA(${LUSDVIEW_KERNEL_SRC_CONTENT})CUDA\";\n")
endif()

if(MSVC)
  enable_language(RC)
endif()

# GUI-independent GPU ray-tracing core shared by lusdview and lusdrender.
# Keep all scene/material packing and runtime shader implementations behind one
# target so the CLI renderer cannot drift from the viewer's OpenPBR/MaterialX
# ABI.  In particular this target has no GLFW, ImGui, OpenGL, or X11 dependency.
set(LIGHTUSD_GPU_RT_COMMON_SOURCES
    lighting_ies.cc
    ${PROJECT_SOURCE_DIR}/src/ptx-loader.cc
    ptex_atlas.cc
    texture_tools.cc
    lightrt_mtlx_bridge.cc
    lightrt_mtlx_graph_compile.cc
    lightrt_mtlx_graph_phases.cc
    lightrt_mtlx_pack.cc
    displacement_bake.cc
    rt_bvh.cc
    rt_scene_build.cc
    cuda/cuda_raytracer.cc
    hip/hip_raytracer.cc
    ${COMMON_DIR}/light3d/math.cpp
    ${COMMON_DIR}/cuew/cuew.c
    ${PROJECT_SOURCE_DIR}/src/external/hipew/hipew.c)
if(LUSDVIEW_HAVE_OPTIX)
  find_program(LUSDVIEW_NVCC_EXECUTABLE NAMES nvcc
      HINTS ENV CUDA_PATH ENV CUDA_HOME
      PATH_SUFFIXES bin)
  if(NOT LUSDVIEW_NVCC_EXECUTABLE)
    message(FATAL_ERROR "OptiX support requires nvcc to generate embedded OptiX IR")
  endif()
  set(LUSDVIEW_OPTIX_IR
      ${CMAKE_CURRENT_BINARY_DIR}/generated/lusdview_optix_device.optixir)
  set(LUSDVIEW_OPTIX_IR_HEADER
      ${CMAKE_CURRENT_BINARY_DIR}/generated/lusdview_optix_device_ir.hh)
  add_custom_command(
      OUTPUT ${LUSDVIEW_OPTIX_IR} ${LUSDVIEW_OPTIX_IR_HEADER}
      COMMAND ${LUSDVIEW_NVCC_EXECUTABLE} --std=c++17 --optix-ir
              -I${LUSDVIEW_OPTIX_INCLUDE_DIR}
              -I${LUSDVIEW_CUDA_INCLUDE_DIR}
              ${CMAKE_CURRENT_SOURCE_DIR}/cuda/optix_device.cu
              -o ${LUSDVIEW_OPTIX_IR}
      COMMAND ${CMAKE_COMMAND}
              -DINPUT=${LUSDVIEW_OPTIX_IR}
              -DOUTPUT=${LUSDVIEW_OPTIX_IR_HEADER}
              -DSYMBOL=kLusdviewOptixDeviceIr
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cuda/embed_binary.cmake
      DEPENDS cuda/optix_device.cu cuda/embed_binary.cmake
      VERBATIM
      COMMENT "Generating embedded lusdview OptiX IR")
  add_custom_target(lusdview_optix_ir DEPENDS ${LUSDVIEW_OPTIX_IR_HEADER})
  list(APPEND LIGHTUSD_GPU_RT_COMMON_SOURCES cuda/optix_runtime.cc)
endif()
if(MSVC)
  list(APPEND LIGHTUSD_GPU_RT_COMMON_SOURCES
       kernel_resource.cc raytracer_kernel_resource.rc)
endif()

add_library(lightusd_gpu_rt_common STATIC
    ${LIGHTUSD_GPU_RT_COMMON_SOURCES}
    $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
add_dependencies(lightusd_gpu_rt_common lusdview_lightrt_mtlx)
target_compile_features(lightusd_gpu_rt_common PUBLIC cxx_std_17)
target_include_directories(lightusd_gpu_rt_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/external
    ${COMMON_DIR}
    ${COMMON_DIR}/cuew
    ${PROJECT_SOURCE_DIR}/src/external/hipew)
if(LUSDVIEW_HAVE_OPTIX)
  add_dependencies(lightusd_gpu_rt_common lusdview_optix_ir)
  target_include_directories(lightusd_gpu_rt_common PRIVATE
      ${LUSDVIEW_OPTIX_INCLUDE_DIR}
      ${LUSDVIEW_CUDA_INCLUDE_DIR})
  target_compile_definitions(lightusd_gpu_rt_common PUBLIC LUSDVIEW_HAVE_OPTIX=1)
endif()
if(NOT MSVC)
  target_include_directories(lightusd_gpu_rt_common PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/generated)
endif()
target_link_libraries(lightusd_gpu_rt_common PUBLIC
    ${LIGHTUSD_TARGET_STATIC}
    ${CMAKE_DL_LIBS})
if(Threads_FOUND)
  target_link_libraries(lightusd_gpu_rt_common PUBLIC Threads::Threads)
endif()
if(LIGHTUSD_WITH_TEXTOOLS)
  target_link_libraries(lightusd_gpu_rt_common PUBLIC lightusd_textools)
  target_compile_definitions(lightusd_gpu_rt_common PRIVATE
      LUSDVIEW_WITH_TEXTOOLS=1)
  if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
    target_compile_definitions(lightusd_gpu_rt_common PRIVATE
        LIGHTUSD_WITH_ZSTD_COMPRESSION)
  endif()
endif()

add_executable(${EXAMPLE_TARGET}
    ${LUSDVIEW_SOURCES}
    ${LUSDVIEW_COMMON_SOURCES})
# vchar is a profile of the same native runtime. Keeping one executable image is
# deliberate: GL/Vulkan raster, Vulkan ray query, skinning, morphing and hair
# must not acquire a second implementation. argv[0] selects the focused UI and
# capability policy. The copy is emitted next to lusdview for scripts/packages.
add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${EXAMPLE_TARGET}>
            $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>/vchar$<TARGET_FILE_SUFFIX:${EXAMPLE_TARGET}>
    COMMENT "Creating vchar virtual-human renderer profile")
add_dependencies(${EXAMPLE_TARGET} lusdview_lightrt_mtlx)
if(NOT MSVC)
  target_include_directories(${EXAMPLE_TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
endif()

# Helper: apply -Weverything (Clang) / -Wall (GCC) to a C++ target's CXX sources.
function(lusdview_set_warning_flags target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_flags_list
        -Weverything -Wno-poison-system-directories -Wno-padded
        -Wno-c++98-compat -Wno-c++98-compat-pedantic
        -Wno-documentation -Wno-documentation-unknown-command
        -Wno-unused-member-function -Wno-switch-enum -Wno-exit-time-destructors
        -Wno-sign-conversion -Wno-float-equal -Wno-old-style-cast
        -Wno-double-promotion -Wno-missing-prototypes
        -Wno-covered-switch-default -Wno-zero-as-null-pointer-constant
        -Wno-nrvo -Wno-weak-vtables
        -Wno-reserved-macro-identifier -Wno-reserved-identifier
        -Wno-tautological-unsigned-zero-compare -Wno-unknown-warning-option
        -Wno-undef -Wno-implicit-int-float-conversion
        -Wno-implicit-float-conversion -Wno-shorten-64-to-32
        -Wno-ms-bitfield-padding -Wno-mismatched-tags)
    if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 16)
      list(APPEND _flags_list -Wno-unsafe-buffer-usage)
    endif()
    if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
      list(APPEND _flags_list -Wno-switch-default)
    endif()
    if(NOT LIGHTUSD_NO_WERROR)
      list(APPEND _flags_list -Werror)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(_flags_list -Wall)
    if(NOT LIGHTUSD_NO_WERROR)
      list(APPEND _flags_list -Werror)
    endif()
  endif()
  target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:${_flags_list}>)
endfunction()

if(LIGHTUSD_BUILD_TESTS)
  if(LUSDVIEW_HAVE_OPTIX)
    add_executable(lusdview_optix_runtime_test
        tests/optix_runtime_test.cc
        cuda/optix_runtime.cc)
    target_compile_features(lusdview_optix_runtime_test PRIVATE cxx_std_17)
    lusdview_set_warning_flags(lusdview_optix_runtime_test)
    target_include_directories(lusdview_optix_runtime_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LUSDVIEW_OPTIX_INCLUDE_DIR}
        ${LUSDVIEW_CUDA_INCLUDE_DIR})
    target_link_libraries(lusdview_optix_runtime_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME lusdview_optix_runtime_test
        COMMAND lusdview_optix_runtime_test)
    set_tests_properties(lusdview_optix_runtime_test PROPERTIES
        LABELS "lusdview;gpu;optix"
        SKIP_RETURN_CODE 77)
    add_test(NAME lusdview-optix-integration
        COMMAND ${CMAKE_COMMAND} -E env
          LUSDVIEW=$<TARGET_FILE:lusdview>
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-optix-integration.sh)
    set_tests_properties(lusdview-optix-integration PROPERTIES
        LABELS "lusdview;gpu;optix"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()

  add_executable(lusdview_ptex_atlas_test
      tests/ptex_atlas_test.cc
      ptex_atlas.cc
      displacement_bake.cc)
  target_compile_features(lusdview_ptex_atlas_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_ptex_atlas_test)
  target_include_directories(lusdview_ptex_atlas_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_ptex_atlas_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})

  add_executable(lusdview_lightrt_bridge_test
      tests/lightrt_bridge_test.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      lighting_ies.cc
      rt_scene_build.cc
      rt_bvh.cc
      texture_tools.cc
      ptex_atlas.cc
      ${PROJECT_SOURCE_DIR}/src/ptx-loader.cc
      ${PROJECT_SOURCE_DIR}/src/image-loader.cc
      ${PROJECT_SOURCE_DIR}/src/io-util.cc
      ${PROJECT_SOURCE_DIR}/src/external/miniz.c
      displacement_bake.cc
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_lightrt_bridge_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_lightrt_bridge_test)
  target_include_directories(lusdview_lightrt_bridge_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(lusdview_lightrt_bridge_test PRIVATE Threads::Threads)
  endif()

  add_executable(lusdview_openpbr_material_test
      tests/openpbr_material_test.cc
      mesh_build.cc
      texture_tools.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      lighting_ies.cc
      displacement_bake.cc
      skinning.cc
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/bsdf.c
      ${COMMON_DIR}/light3d/math.cpp
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_openpbr_material_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_openpbr_material_test)
  target_include_directories(lusdview_openpbr_material_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_openpbr_material_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_openpbr_material_test PRIVATE Threads::Threads)
  endif()

  add_executable(lusdview_lighting_test
      tests/lighting_test.cc
      dome_light.cc
      lighting_ies.cc
      mesh_build.cc
      texture_tools.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      rt_scene_build.cc
      rt_bvh.cc
      ptex_atlas.cc
      displacement_bake.cc
      skinning.cc
      ${COMMON_DIR}/light3d/math.cpp
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_lighting_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_lighting_test)
  target_include_directories(lusdview_lighting_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_lighting_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_lighting_test PRIVATE Threads::Threads)
  endif()

  add_executable(lusdview_geometry_primvar_test
      tests/geometry_primvar_test.cc
      mesh_build.cc
      texture_tools.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      lighting_ies.cc
      displacement_bake.cc
      skinning.cc
      ${COMMON_DIR}/light3d/math.cpp
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_geometry_primvar_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_geometry_primvar_test)
  target_include_directories(lusdview_geometry_primvar_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_geometry_primvar_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_geometry_primvar_test PRIVATE Threads::Threads)
  endif()

  add_executable(lusdview_texture_pipeline_test
      tests/texture_pipeline_test.cc
      mesh_build.cc
      texture_tools.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      lighting_ies.cc
      displacement_bake.cc
      skinning.cc
      ${COMMON_DIR}/light3d/math.cpp
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_texture_pipeline_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_texture_pipeline_test)
  target_include_directories(lusdview_texture_pipeline_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_texture_pipeline_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_texture_pipeline_test PRIVATE Threads::Threads)
  endif()

  add_executable(lusdview_lightrt_mtlx_eval_test
      tests/lightrt_mtlx_eval_test.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/color.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_doc.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_eval.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_xml.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/texture.c
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/third_party_impl.c)
  target_compile_features(lusdview_lightrt_mtlx_eval_test PRIVATE c_std_11)
  target_include_directories(lusdview_lightrt_mtlx_eval_test PRIVATE
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}
      ${PROJECT_SOURCE_DIR}/src/external)
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_lightrt_mtlx_eval_test PRIVATE m)
  endif()

  foreach(mtlx_suite graph_connection graph_evaluation)
    add_executable(lusdview_lightrt_mtlx_${mtlx_suite}_test
        tests/lightrt_mtlx_${mtlx_suite}_test.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/color.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_doc.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_eval.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/mtlx_xml.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/texture.c
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}/third_party_impl.c)
    target_compile_features(lusdview_lightrt_mtlx_${mtlx_suite}_test PRIVATE c_std_11)
    target_include_directories(lusdview_lightrt_mtlx_${mtlx_suite}_test PRIVATE
        ${LUSDVIEW_LIGHTRT_MTLX_DIR}
        ${PROJECT_SOURCE_DIR}/src/external)
    if(UNIX AND NOT APPLE)
      target_link_libraries(lusdview_lightrt_mtlx_${mtlx_suite}_test PRIVATE m)
    endif()
  endforeach()

  add_executable(lusdview_camera_nav_test
      tests/camera_nav_test.cc
      camera_nav.cc
      ${COMMON_DIR}/light3d/math.cpp)
  target_compile_features(lusdview_camera_nav_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_camera_nav_test)
  target_include_directories(lusdview_camera_nav_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${COMMON_DIR})

  add_executable(lusdview_camera_stereo_test
      tests/camera_stereo_test.cc
      camera_stereo.cc)
  target_compile_features(lusdview_camera_stereo_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_camera_stereo_test)
  target_include_directories(lusdview_camera_stereo_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})

  add_executable(lusdview_preview_cache_test
      tests/preview_cache_test.cc
      preview_cache.cc)
  target_compile_features(lusdview_preview_cache_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_preview_cache_test)
  target_include_directories(lusdview_preview_cache_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external)
  target_link_libraries(lusdview_preview_cache_test PRIVATE lightusd_next)

  add_executable(lusdview_texture_residency_policy_test
      tests/texture_residency_policy_test.cc
      texture_residency_policy.cc)
  target_compile_features(lusdview_texture_residency_policy_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_texture_residency_policy_test)
  target_include_directories(lusdview_texture_residency_policy_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(lusdview_incremental_scene_update_test
      tests/incremental_scene_update_test.cc
      incremental_scene_update.cc)
  target_compile_features(lusdview_incremental_scene_update_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_incremental_scene_update_test)
  target_include_directories(lusdview_incremental_scene_update_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_incremental_scene_update_test PRIVATE
      lightusd_next)

  add_executable(lusdview_rt_lod_test
      tests/rt_lod_test.cc
      rt_lod.cc
      ${COMMON_DIR}/light3d/camera.cpp
      ${COMMON_DIR}/light3d/math.cpp)
  target_compile_features(lusdview_rt_lod_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_rt_lod_test)
  target_include_directories(lusdview_rt_lod_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${COMMON_DIR})

  add_executable(lusdview_scene_safety_test
      tests/scene_safety_test.cc
      scene_optimize.cc
      lightrt_mtlx_bridge.cc
      lightrt_mtlx_graph_compile.cc
      lightrt_mtlx_graph_phases.cc
      lightrt_mtlx_pack.cc
      ${LUSDVIEW_LIGHTRT_MTLX_DIR}/bsdf.c
      $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lusdview_scene_safety_test PRIVATE cxx_std_17)
  lusdview_set_warning_flags(lusdview_scene_safety_test)
  target_include_directories(lusdview_scene_safety_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/external
      ${COMMON_DIR})
  target_link_libraries(lusdview_scene_safety_test PRIVATE
      ${LIGHTUSD_TARGET_STATIC})
  if(UNIX AND NOT APPLE)
    target_link_libraries(lusdview_scene_safety_test PRIVATE Threads::Threads)
  endif()

  # Register the lusdview unit tests with ctest. They were built here but never
  # registered (this file had no add_test at all), so a regression in any of
  # them went unnoticed. Works because the top-level CMakeLists.txt calls
  # enable_testing() before this subdirectory is added. All are self-contained
  # (no GPU/display) and take no arguments; non-zero exit == failure.
  foreach(tv_test
      lusdview_ptex_atlas_test
      lusdview_lightrt_bridge_test
      lusdview_openpbr_material_test
      lusdview_lighting_test
      lusdview_geometry_primvar_test
      lusdview_texture_pipeline_test
      lusdview_lightrt_mtlx_eval_test
      lusdview_lightrt_mtlx_graph_connection_test
      lusdview_lightrt_mtlx_graph_evaluation_test
      lusdview_camera_nav_test
      lusdview_camera_stereo_test
      lusdview_preview_cache_test
      lusdview_texture_residency_policy_test
      lusdview_incremental_scene_update_test
      lusdview_rt_lod_test
      lusdview_scene_safety_test)
    add_test(NAME ${tv_test} COMMAND ${tv_test})
    set_tests_properties(${tv_test} PROPERTIES
      LABELS "lusdview"
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    if(COMMAND add_sanitizers)
      add_sanitizers(${tv_test})
    endif()
  endforeach()

  find_program(LUSDVIEW_PYTHON NAMES python3 python)
  if(LUSDVIEW_PYTHON)
    add_test(NAME lusdview-gpu-material-abi
      COMMAND ${LUSDVIEW_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gpu_material_abi.py
        ${PROJECT_SOURCE_DIR})
    set_tests_properties(lusdview-gpu-material-abi PROPERTIES LABELS "lusdview")
  endif()
endif()

# Vendored texture tools (tir/texcomp/texpipe/envmap). All calls are routed
# through texture_tools.cc and guarded by LUSDVIEW_WITH_TEXTOOLS, so the app
# builds unchanged when LIGHTUSD_WITH_TEXTOOLS=OFF. Applied to every target
# that compiles mesh_build.cc / texture_tools.cc.
if(LIGHTUSD_WITH_TEXTOOLS)
  set(LUSDVIEW_TEXTOOLS_CONSUMERS ${EXAMPLE_TARGET})
  if(LIGHTUSD_BUILD_TESTS)
    list(APPEND LUSDVIEW_TEXTOOLS_CONSUMERS
        lusdview_lightrt_bridge_test
        lusdview_openpbr_material_test
        lusdview_lighting_test
        lusdview_geometry_primvar_test
        lusdview_texture_pipeline_test)
  endif()
  foreach(tt_consumer IN LISTS LUSDVIEW_TEXTOOLS_CONSUMERS)
    target_link_libraries(${tt_consumer} PRIVATE lightusd_textools)
    target_compile_definitions(${tt_consumer} PRIVATE LUSDVIEW_WITH_TEXTOOLS=1)
    # Zstd-supercompressed KTX2 (scheme 2) is what usd-texcomp writes by default;
    # the core lib defines this PRIVATEly, so mesh_build.cc needs its own copy.
    # ZSTD_decompress itself comes from lightusd_static (src/external/zstd.c).
    if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
      target_compile_definitions(${tt_consumer} PRIVATE LIGHTUSD_WITH_ZSTD_COMPRESSION)
    endif()
  endforeach()
endif()

if(COMMAND add_sanitizers)
  add_sanitizers(${EXAMPLE_TARGET})
endif()

target_compile_features(${EXAMPLE_TARGET} PRIVATE cxx_std_17)

# lusdview uses no C++ RTTI and (outside the optional MCP server) no C++
# exceptions; disable both to match the lightusd library build (LIGHTUSD_CXX_RTTI
# / _EXCEPTIONS default Off). Scoped to C++ so vendored C sources (glad.c, cuew,
# civetweb.c, ...) don't warn. The MCP server TUs re-enable exceptions below.
if(NOT LIGHTUSD_CXX_RTTI)
  if(MSVC)
    target_compile_options(${EXAMPLE_TARGET}
                           PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
  else()
    target_compile_options(${EXAMPLE_TARGET}
                           PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
  endif()
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS)
  if(MSVC)
    target_compile_options(${EXAMPLE_TARGET}
                           PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
  else()
    target_compile_options(${EXAMPLE_TARGET}
                           PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
  endif()
endif()

target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/external
    ${COMMON_DIR}
    ${COMMON_DIR}/cuew
    ${PROJECT_SOURCE_DIR}/src/external/hipew
    ${COMMON_DIR}/glad/include
    ${COMMON_DIR}/imgui)

# Source defaults used only by the opt-in persistent-process live-reload loop.
# They are compile-time development hints; packaged builds may override every
# path through the MCP shader_reload tool.
target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
    LUSDVIEW_DEFAULT_VK_SHADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/vk/shaders/raytrace.comp"
    LUSDVIEW_DEFAULT_VK_SW_SHADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/vk/shaders/raytrace_swbvh.comp"
    LUSDVIEW_DEFAULT_GPU_KERNEL_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/raytracer_kernel_src.txt")

target_link_libraries(${EXAMPLE_TARGET} PRIVATE
    lightusd_gpu_rt_common
    ${LIGHTUSD_TARGET_STATIC}
    glfw
    OpenGL::GL
    ${CMAKE_DL_LIBS})

# --- `next` lazy loader + tydra-next converter (large-scene preview path) -----
# Mirrors tools/lusdrender/CMakeLists.txt: build the next libs on demand (with
# the std::thread parallel-compose path forced on) and link them. Used by
# next_scene_loader.cc (the --next OpenGL mesh preview).
set(LIGHTUSD_NEXT_ENABLE_THREAD ON CACHE BOOL "" FORCE)
if(NOT TARGET lightusd_next)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next
                   ${CMAKE_BINARY_DIR}/lusdview_next_build)
endif()
if(NOT TARGET tydra_next)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/tydra/next
                   ${CMAKE_BINARY_DIR}/lusdview_tydra_next_build)
endif()
target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_next tydra_next)

# CPU RT (cpu/cpu_raytracer.cc): reuse the vendored lightrt_c BVH library
# (src/external/lightrt/lightrt_c_tri.*) via the same CMake target
# tools/lusdrender already defines, instead of duplicating that build. The
# add_subdirectory also configures the lusdrender executable as a harmless
# side effect (the guard makes this idempotent when both are built together).
if(NOT TARGET lightrt_c)
  add_subdirectory(${PROJECT_SOURCE_DIR}/tools/lusdrender
                   ${CMAKE_BINARY_DIR}/lusdview_lightrt_c_build)
endif()
target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightrt_c)

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE ${X11_LIBRARIES})
  find_package(Threads REQUIRED)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE Threads::Threads)
elseif(APPLE)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE
      "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

# MinGW (e.g. llvm-mingw): statically link the C++/runtime support libs so the
# .exe is self-contained and needs no libc++.dll / libunwind.dll /
# libwinpthread-1.dll alongside it. Only the toolchain runtime is pulled in
# statically; Windows system DLLs (kernel32, user32, ws2_32, …) and the UCRT
# stay dynamic. Opt out with -DLUSDVIEW_STATIC_RUNTIME=OFF.
if(MINGW)
  option(LUSDVIEW_STATIC_RUNTIME
         "Statically link the MinGW C++/unwind/pthread runtime into lusdview" ON)
  if(LUSDVIEW_STATIC_RUNTIME)
    target_link_options(${EXAMPLE_TARGET} PRIVATE -static)
    message(STATUS "lusdview: static C++ runtime (-static) for MinGW build")
  endif()
endif()

# ---------------------------------------------------------------------------
# Vulkan backend (optional) - SPIR-V generation + vk_renderer + imgui_impl_vulkan
# ---------------------------------------------------------------------------
if(LUSDVIEW_WITH_VULKAN)
  include(${CMAKE_CURRENT_SOURCE_DIR}/vk/vulkan.cmake)
endif()

# ---------------------------------------------------------------------------
# Native file dialog (optional)
# ---------------------------------------------------------------------------
if(LUSDVIEW_HAVE_NFD)
  if(NOT TARGET nfd)
    add_subdirectory(${COMMON_DIR}/nativefiledialog-extended
                     ${CMAKE_BINARY_DIR}/_deps/lusdview-nfd EXCLUDE_FROM_ALL)
  endif()
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE nfd)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE HAVE_NFD=1)
  message(STATUS "lusdview: native file dialog ENABLED")
else()
  message(STATUS "lusdview: native file dialog disabled (pass a file on CLI)")
endif()

# ---------------------------------------------------------------------------
# Embedded MCP (Model Context Protocol) server: stdio + HTTP transports.
# Dependencies (nlohmann/json + civetweb) are vendored in lightusd/src/external,
# so this defaults ON (no system packages needed).
# ---------------------------------------------------------------------------
# civetweb HTTP/WebSocket server. Shared by the WebSocket image-stream server
# (always built) and the optional MCP server. Built with websockets ENABLED.
target_sources(${EXAMPLE_TARGET} PRIVATE
    stream/stream_server.cc
    ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c)

# Runtime-generated index-only prototype LODs. Keep this viewer-private: the
# core library's optional LIGHTUSD_WITH_MESHOPT setting remains unchanged.
target_sources(${EXAMPLE_TARGET} PRIVATE prototype_lod.cc)
if(NOT LIGHTUSD_WITH_MESHOPT)
  target_sources(${EXAMPLE_TARGET} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/allocator.cpp
      ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/simplifier.cpp
      ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/vcacheoptimizer.cpp)
endif()
target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
    NO_SSL NO_CGI USE_WEBSOCKET=1
    LUSDVIEW_RT_PTEXTURE_IMAGE_CAP=${LUSDVIEW_RT_PTEXTURE_IMAGE_CAP})
target_link_libraries(${EXAMPLE_TARGET} PRIVATE ${CMAKE_DL_LIBS})
if(UNIX)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE Threads::Threads)
elseif(WIN32)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE ws2_32)  # civetweb Winsock
  if(MINGW)
    target_link_libraries(${EXAMPLE_TARGET} PRIVATE winpthread)
  endif()
endif()
# stream_server.cc uses nlohmann/json (needs exceptions when built -fno-exceptions).
if(NOT LIGHTUSD_CXX_EXCEPTIONS AND NOT MSVC)
  set_source_files_properties(stream/stream_server.cc
                              PROPERTIES COMPILE_OPTIONS "-fexceptions")
elseif(NOT LIGHTUSD_CXX_EXCEPTIONS AND MSVC)
  set_source_files_properties(stream/stream_server.cc
                              PROPERTIES COMPILE_OPTIONS "/EHsc")
endif()

option(LUSDVIEW_ENABLE_MCP "Build the embedded MCP server (stdio + HTTP)" ON)
if(LUSDVIEW_ENABLE_MCP)
  target_sources(${EXAMPLE_TARGET} PRIVATE
      mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE LUSDVIEW_HAVE_MCP=1)
  # The MCP server uses try/catch around JSON-RPC; it needs C++ exceptions even
  # when the rest of lusdview is built -fno-exceptions.
  if(NOT LIGHTUSD_CXX_EXCEPTIONS AND NOT MSVC)
    set_source_files_properties(
        mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc
        PROPERTIES COMPILE_OPTIONS "-fexceptions")
  elseif(NOT LIGHTUSD_CXX_EXCEPTIONS AND MSVC)
    set_source_files_properties(
        mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc
        PROPERTIES COMPILE_OPTIONS "/EHsc")
  endif()
  message(STATUS "lusdview: embedded MCP server ENABLED (stdio + HTTP)")
else()
  message(STATUS "lusdview: embedded MCP server disabled")
endif()

# ---------------------------------------------------------------------------
# Experimental multi-threaded rendering (--threaded): a dedicated render thread owns
# the GL context / Vulkan queue so the UI loop never blocks on the GPU. Covers both
# the GL and Vulkan (raster + ray-query) backends; verified byte-identical to the
# single-threaded path. OFF by default — the single-threaded path is unchanged when
# off. See doc/threading-stage2.md. (Macro name kept GL_THREAD for compat.)
# ---------------------------------------------------------------------------
option(LUSDVIEW_ENABLE_GL_THREAD
       "Build the dedicated GL/Vulkan render thread (--threaded)" ON)
if(LUSDVIEW_ENABLE_GL_THREAD)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE LUSDVIEW_ENABLE_GL_THREAD=1)
  if(UNIX)
    target_link_libraries(${EXAMPLE_TARGET} PRIVATE Threads::Threads)
  endif()
  message(STATUS "lusdview: experimental threaded GL/Vulkan render path ENABLED (--threaded)")
endif()

# ---------------------------------------------------------------------------
# Warning flags (same policy as the core library)
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(LUSDVIEW_CXX_FLAGS
      "-Weverything -Wno-poison-system-directories -Wno-padded -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-documentation -Wno-documentation-unknown-command -Wno-unused-member-function -Wno-switch-enum -Wno-exit-time-destructors -Wno-sign-conversion -Wno-float-equal -Wno-old-style-cast -Wno-double-promotion -Wno-missing-prototypes -Wno-covered-switch-default -Wno-zero-as-null-pointer-constant -Wno-nrvo -Wno-weak-vtables -Wno-reserved-macro-identifier -Wno-reserved-identifier -Wno-tautological-unsigned-zero-compare -Wno-unknown-warning-option -Wno-undef -Wno-implicit-int-float-conversion -Wno-implicit-float-conversion -Wno-shorten-64-to-32 -Wno-ms-bitfield-padding -Wno-mismatched-tags"
  )
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 16)
    set(LUSDVIEW_CXX_FLAGS "${LUSDVIEW_CXX_FLAGS} -Wno-unsafe-buffer-usage")
  endif()
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
    set(LUSDVIEW_CXX_FLAGS "${LUSDVIEW_CXX_FLAGS} -Wno-switch-default")
  endif()
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDVIEW_CXX_FLAGS "${LUSDVIEW_CXX_FLAGS} -Werror")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(LUSDVIEW_CXX_FLAGS "-Wall")
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDVIEW_CXX_FLAGS "${LUSDVIEW_CXX_FLAGS} -Werror")
  endif()
endif()

# Apply -Weverything only to project-owned C++ sources in lusdview.
set_source_files_properties(${LUSDVIEW_SOURCES}
    PROPERTIES COMPILE_FLAGS "${LUSDVIEW_CXX_FLAGS}")

# Vendored C/C++ third-party code: suppress all warnings.
set_source_files_properties(
    ${LUSDVIEW_COMMON_SOURCES}
    ${COMMON_DIR}/imgui/imgui_impl_vulkan.cpp
    ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c
    PROPERTIES COMPILE_FLAGS "-w")

set_target_properties(${EXAMPLE_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # The closure-lowering function is dominated by compiler-generated control
  # flow and GCC still spends over a minute optimizing it at -O1. It runs only
  # while importing MaterialX graphs, so keep that TU at -O0. The independently
  # compiled packing/order phase is small enough to use the target's normal
  # Release optimization level.
  set_source_files_properties(lightrt_mtlx_graph_compile.cc PROPERTIES
    COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O0>")
endif()
