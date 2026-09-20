cmake_minimum_required(VERSION 3.16)

# Tool: lusdrender
# CPU preview raytrace renderer for USD scenes using Tydra + vendored LightRT.

include(CheckCCompilerFlag)

# lusdrender pulls several large static archives into one executable. GNU
# ld.bfd spends most of a RelWithDebInfo relink rescanning symbols and merging
# DWARF; prefer an installed parallel linker for this target without changing
# the linker used by the rest of LightUSD. Set this OFF when diagnosing a
# platform linker issue or when a toolchain already selects its own linker.
option(LUSDRENDER_USE_FAST_LINKER
       "Use mold, LLD, or gold for lusdrender when available" ON)

# Build the shared GPU RT implementation even in tools-only/headless
# configurations. When lusdview was configured first this target already
# exists; otherwise define the identical GUI-free source set here.
if(NOT TARGET lightusd_gpu_rt_common)
  set(LIGHTUSD_GPU_RT_VIEW_DIR ${PROJECT_SOURCE_DIR}/examples/lusdview)
  set(LIGHTUSD_GPU_RT_COMMON_DIR ${PROJECT_SOURCE_DIR}/examples/common)
  set(LIGHTUSD_GPU_RT_MTLX_DIR ${PROJECT_SOURCE_DIR}/src/external/lightrt/mtlxrender)

  if(NOT TARGET lusdview_lightrt_mtlx_const)
    add_library(lusdview_lightrt_mtlx_const OBJECT
      ${LIGHTUSD_GPU_RT_MTLX_DIR}/color.c
      ${LIGHTUSD_GPU_RT_MTLX_DIR}/mtlx_doc.c
      ${LIGHTUSD_GPU_RT_MTLX_DIR}/mtlx_eval.c
      ${LIGHTUSD_GPU_RT_MTLX_DIR}/mtlx_xml.c
      ${LIGHTUSD_GPU_RT_MTLX_DIR}/texture.c
      ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_image_impl.c)
    target_include_directories(lusdview_lightrt_mtlx_const PRIVATE
      ${LIGHTUSD_GPU_RT_MTLX_DIR} ${PROJECT_SOURCE_DIR}/src/external)
    target_compile_features(lusdview_lightrt_mtlx_const PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(lusdview_lightrt_mtlx_const PRIVATE -w)
    endif()
  endif()

  set(LIGHTUSD_GPU_RT_KERNEL_DIR ${CMAKE_CURRENT_BINARY_DIR}/shared_gpu_generated)
  file(MAKE_DIRECTORY ${LIGHTUSD_GPU_RT_KERNEL_DIR})
  if(NOT MSVC)
    set(LIGHTUSD_GPU_RT_KERNEL_TXT ${LIGHTUSD_GPU_RT_VIEW_DIR}/raytracer_kernel_src.txt)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${LIGHTUSD_GPU_RT_KERNEL_TXT})
    file(READ ${LIGHTUSD_GPU_RT_KERNEL_TXT} LIGHTUSD_GPU_RT_KERNEL_CONTENT)
    file(WRITE ${LIGHTUSD_GPU_RT_KERNEL_DIR}/raytracer_kernel.inc
      "const char* kKernelSrc = R\"CUDA(${LIGHTUSD_GPU_RT_KERNEL_CONTENT})CUDA\";\n")
  endif()

  set(LIGHTUSD_GPU_RT_SOURCES
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lighting_ies.cc
    ${PROJECT_SOURCE_DIR}/src/ptx-loader.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/ptex_atlas.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/texture_tools.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_bridge.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_graph_compile.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_graph_phases.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_pack.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/displacement_bake.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/rt_bvh.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/rt_scene_build.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/cuda/cuda_raytracer.cc
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/hip/hip_raytracer.cc
    ${LIGHTUSD_GPU_RT_COMMON_DIR}/light3d/math.cpp
    ${LIGHTUSD_GPU_RT_COMMON_DIR}/cuew/cuew.c
    ${PROJECT_SOURCE_DIR}/src/external/hipew/hipew.c)
  if(MSVC)
    enable_language(RC)
    list(APPEND LIGHTUSD_GPU_RT_SOURCES
      ${LIGHTUSD_GPU_RT_VIEW_DIR}/kernel_resource.cc
      ${LIGHTUSD_GPU_RT_VIEW_DIR}/raytracer_kernel_resource.rc)
  endif()
  add_library(lightusd_gpu_rt_common STATIC
    ${LIGHTUSD_GPU_RT_SOURCES}
    $<TARGET_OBJECTS:lusdview_lightrt_mtlx_const>)
  target_compile_features(lightusd_gpu_rt_common PUBLIC cxx_std_17)
  target_include_directories(lightusd_gpu_rt_common PUBLIC
    ${LIGHTUSD_GPU_RT_VIEW_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/external
    ${LIGHTUSD_GPU_RT_COMMON_DIR}
    ${LIGHTUSD_GPU_RT_COMMON_DIR}/cuew
    ${PROJECT_SOURCE_DIR}/src/external/hipew)
  if(NOT MSVC)
    target_include_directories(lightusd_gpu_rt_common PRIVATE
      ${LIGHTUSD_GPU_RT_KERNEL_DIR})
  endif()
  target_link_libraries(lightusd_gpu_rt_common PUBLIC
    ${LIGHTUSD_TARGET_STATIC} ${CMAKE_DL_LIBS})
  if(LIGHTUSD_WITH_TEXTOOLS)
    target_link_libraries(lightusd_gpu_rt_common PUBLIC lightusd_textools)
    target_compile_definitions(lightusd_gpu_rt_common PRIVATE LUSDVIEW_WITH_TEXTOOLS=1)
    if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
      target_compile_definitions(lightusd_gpu_rt_common PRIVATE
        LIGHTUSD_WITH_ZSTD_COMPRESSION)
    endif()
  endif()
endif()

add_library(lightrt_c STATIC
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c.h
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_tri.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_tri.h
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/mtlxrender/bsdf.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/mtlxrender/bsdf.h
)

set_target_properties(lightrt_c PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

# Vendored external C code: suppress all warnings.
if(NOT MSVC)
  target_compile_options(lightrt_c PRIVATE -w)
endif()

# lightrt's C sources use C11 <stdatomic.h>. MSVC only enables C atomics behind
# /experimental:c11atomics (VS 2022 17.5+); GCC/Clang have them by default.
if(MSVC)
  target_compile_options(lightrt_c PRIVATE /experimental:c11atomics)
endif()

target_include_directories(lightrt_c PUBLIC
  ${PROJECT_SOURCE_DIR}/src/external/lightrt
)

set(LIGHTRT_C_TRI_SIMD_FLAGS "")
if(NOT MSVC)
  check_c_compiler_flag("-msse4.1" LUSDR_LIGHTRT_HAS_SSE41)
  if(LUSDR_LIGHTRT_HAS_SSE41)
    list(APPEND LIGHTRT_C_TRI_SIMD_FLAGS -msse4.1)
  endif()
  check_c_compiler_flag("-mavx2" LUSDR_LIGHTRT_HAS_AVX2)
  if(LUSDR_LIGHTRT_HAS_AVX2)
    list(APPEND LIGHTRT_C_TRI_SIMD_FLAGS -mavx2 -mfma)
  endif()
endif()

if(LIGHTRT_C_TRI_SIMD_FLAGS)
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_tri.c
    PROPERTIES COMPILE_OPTIONS "${LIGHTRT_C_TRI_SIMD_FLAGS}")
endif()

find_package(Threads QUIET)
if(Threads_FOUND)
  target_link_libraries(lightrt_c PUBLIC Threads::Threads)
endif()
if(NOT MSVC)
  target_link_libraries(lightrt_c PUBLIC m)
endif()

add_executable(lusdrender
  lusdrender.cc
  lusdr_args.cc
  lusdr_material.cc
  lusdr_lighting.cc
  lusdr_geom.cc
  lusdr_integrator.cc
  lusdr_scripting.cc
  lusdr_stream.cc
  lusdr_next.cc
  lusdr_lod.cc
  lusdr_rt_lod.cc
  lusdr_legacy.cc
  lusdr_gpu_common.cc
  lusdr_vulkan.cc
  lusdr_d3d.cc
  lusdr_hip.cc
  lusdr_cuda.cc
)

if(LUSDRENDER_USE_FAST_LINKER AND UNIX AND NOT APPLE AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   NOT CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-fuse-ld=")
  set(_lusdrender_fast_linker "")
  find_program(LUSDRENDER_MOLD_EXECUTABLE NAMES mold)
  find_program(LUSDRENDER_LLD_EXECUTABLE
               NAMES ld.lld lld ld.lld-21 lld-21 ld.lld-22 lld-22
                     ld.lld-20 lld-20 ld.lld-19 lld-19 ld.lld-18 lld-18
                     ld.lld-17 lld-17)
  find_program(LUSDRENDER_GOLD_EXECUTABLE NAMES ld.gold gold)
  if(LUSDRENDER_MOLD_EXECUTABLE)
    set(_lusdrender_fast_linker mold)
  elseif(LUSDRENDER_LLD_EXECUTABLE)
    set(_lusdrender_fast_linker lld)
  elseif(LUSDRENDER_GOLD_EXECUTABLE)
    set(_lusdrender_fast_linker gold)
  endif()
  if(_lusdrender_fast_linker)
    if(_lusdrender_fast_linker STREQUAL "lld")
      # Versioned distro installs may intentionally lack a working unversioned
      # ld.lld alternative. GCC's -fuse-ld=lld honors -B, so point it at the
      # real LLVM bin directory selected above.
      get_filename_component(_lusdrender_lld_real
                             "${LUSDRENDER_LLD_EXECUTABLE}" REALPATH)
      get_filename_component(_lusdrender_lld_dir
                             "${_lusdrender_lld_real}" DIRECTORY)
      target_link_options(lusdrender PRIVATE "-B${_lusdrender_lld_dir}")
    endif()
    target_link_options(lusdrender PRIVATE
                        "-fuse-ld=${_lusdrender_fast_linker}")
    message(STATUS
            "lusdrender: using ${_lusdrender_fast_linker} for faster links")
  endif()
  mark_as_advanced(LUSDRENDER_MOLD_EXECUTABLE LUSDRENDER_LLD_EXECUTABLE
                   LUSDRENDER_GOLD_EXECUTABLE)
  unset(_lusdrender_fast_linker)
  unset(_lusdrender_lld_real)
  unset(_lusdrender_lld_dir)
endif()

target_link_libraries(lusdrender
  ${LIGHTUSD_TARGET_STATIC}
  lightrt_c
)

if(COMMAND add_sanitizers)
  add_sanitizers(lusdrender)
endif()

# Vendored texture tools: the opt-in `-ibl envmap` DomeLight IBL backend.
if(LIGHTUSD_WITH_TEXTOOLS)
  target_link_libraries(lusdrender lightusd_textools)
  target_compile_definitions(lusdrender PRIVATE LUSDR_WITH_TEXTOOLS)
endif()

# lusdrender uses the experimental `next` lazy loader (fast, low-memory USDC
# parse) as the default for the RT preview path, plus tydra_next for bit-exact
# world transforms. Build those targets on demand and link them.
# Parallel composition (per-prim opinion fill across cores) needs the next lib's
# std::thread paths; output is byte-identical to the serial fill. Force it on for
# lusdrender's on-demand next build.
set(LIGHTUSD_NEXT_ENABLE_THREAD ON CACHE BOOL "" FORCE)
if(NOT TARGET lightusd_next)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next
                   ${CMAKE_BINARY_DIR}/lusdrender_next_build)
endif()
target_link_libraries(lusdrender lightusd_next tydra_next)
# tydra_next's lazy Ptex decoder uses symbols supplied by the main static
# library.  Keep that archive after tydra_next so single-pass static linkers
# resolve the dependency (the initial common-library link above precedes it).
target_link_libraries(lusdrender ${LIGHTUSD_TARGET_STATIC})

# lusdrender itself spawns std::thread workers for the mesh-stream and render
# passes, so link the threading runtime explicitly (not just transitively via
# lightrt_c).
if(Threads_FOUND)
  target_link_libraries(lusdrender Threads::Threads)
endif()

# Windows: GetProcessMemoryInfo (process RSS for the memory budget) lives in psapi.
if(WIN32)
  target_link_libraries(lusdrender psapi)
endif()

target_include_directories(lusdrender PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src/external/lightrt
)

# Warning flags (same policy as the core library)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(LUSDRENDER_CXX_FLAGS
      "-Weverything -Wno-poison-system-directories -Wno-padded -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-documentation -Wno-documentation-unknown-command -Wno-unused-member-function -Wno-switch-enum -Wno-exit-time-destructors -Wno-sign-conversion -Wno-float-equal -Wno-old-style-cast -Wno-double-promotion -Wno-missing-prototypes -Wno-covered-switch-default -Wno-zero-as-null-pointer-constant -Wno-nrvo -Wno-weak-vtables -Wno-reserved-macro-identifier -Wno-reserved-identifier -Wno-tautological-unsigned-zero-compare -Wno-unknown-warning-option -Wno-undef -Wno-implicit-int-float-conversion -Wno-implicit-float-conversion -Wno-shorten-64-to-32 -Wno-ms-bitfield-padding -Wno-mismatched-tags"
  )
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 16)
    set(LUSDRENDER_CXX_FLAGS "${LUSDRENDER_CXX_FLAGS} -Wno-unsafe-buffer-usage")
  endif()
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
    set(LUSDRENDER_CXX_FLAGS "${LUSDRENDER_CXX_FLAGS} -Wno-switch-default")
  endif()
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDRENDER_CXX_FLAGS "${LUSDRENDER_CXX_FLAGS} -Werror")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(LUSDRENDER_CXX_FLAGS "-Wall")
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDRENDER_CXX_FLAGS "${LUSDRENDER_CXX_FLAGS} -Werror")
  endif()
endif()

# Project-owned C++ sources
set(LUSDRENDER_CXX_SOURCES
  lusdrender.cc
  lusdr_args.cc
  lusdr_material.cc
  lusdr_lighting.cc
  lusdr_geom.cc
  lusdr_integrator.cc
  lusdr_scripting.cc
  lusdr_stream.cc
  lusdr_next.cc
  lusdr_lod.cc
  lusdr_rt_lod.cc
  lusdr_legacy.cc
  lusdr_gpu_common.cc
  lusdr_vulkan.cc
  lusdr_d3d.cc
  lusdr_hip.cc
  lusdr_cuda.cc
)
set_source_files_properties(${LUSDRENDER_CXX_SOURCES}
    PROPERTIES COMPILE_FLAGS "${LUSDRENDER_CXX_FLAGS}")

# Vendored C third-party code: suppress all warnings.
set_source_files_properties(
  ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_vk.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_vkew.c
  ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_hip.c
  ${PROJECT_SOURCE_DIR}/src/external/hipew/hipew.c
  PROPERTIES COMPILE_FLAGS "-w")

# When the project is built with QuickJS, enable lusdrender's -js (scripting)
# and -mcp (MCP stdio control) modes for memory-persistent / interactive
# rendering. The library defines LIGHTUSD_WITH_QJS PRIVATEly, so re-declare it
# for the tool. The JS engine + MCP plumbing come from lightusd_static.
if(LIGHTUSD_WITH_QJS)
  target_compile_definitions(lusdrender PRIVATE LIGHTUSD_WITH_QJS)
endif()

# --- WebSocket browser streaming server (-streamHttp) -----------------------
# Compile the vendored civetweb (websockets enabled) into lusdrender and enable
# lusdr_stream.cc. Frames are encoded via the shared image writer (JPEG/QOI/PNG).
option(LUSDRENDER_WITH_STREAM "Build the WebSocket browser streaming server" ON)
if(LUSDRENDER_WITH_STREAM)
  target_sources(lusdrender PRIVATE
    ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c)
  target_include_directories(lusdrender PRIVATE
    ${PROJECT_SOURCE_DIR}/src/external/civetweb
    ${PROJECT_SOURCE_DIR}/src/external/jsonhpp)
  target_compile_definitions(lusdrender PRIVATE
    LUSDRENDER_WITH_STREAM USE_WEBSOCKET=1 NO_SSL NO_CGI)
  # nlohmann/json (used to parse browser input) needs exceptions enabled.
  set_source_files_properties(lusdr_stream.cc PROPERTIES COMPILE_OPTIONS
    "-fexceptions;$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-thread-safety-negative>")
  # libc++ annotates std::mutex locking, but std::mutex cannot express !mu
  # contracts (it has no operator!). This condition_variable-based server uses
  # standard locks, not the project's annotated mutex wrapper. Disable only
  # the experimental negative-capability warning; keep other TSA diagnostics.
  if(Threads_FOUND)
    target_link_libraries(lusdrender Threads::Threads)
  endif()
  target_link_libraries(lusdrender ${CMAKE_DL_LIBS})
  if(WIN32)
    target_link_libraries(lusdrender ws2_32)
  endif()
endif()

# --- LightRT Vulkan backend (optional, runtime-loaded via vkew) -------------
option(LUSDRENDER_WITH_VULKAN "Build the Vulkan GPU-interop backend" ON)
if(LUSDRENDER_WITH_VULKAN)
  set(LIGHTRT_VK_SRC ${PROJECT_SOURCE_DIR}/src/external/lightrt)
  add_library(lightrt_c_vk STATIC
    ${LIGHTRT_VK_SRC}/lightrt_c_vk.c
    ${LIGHTRT_VK_SRC}/lightrt_c_vk.h
    ${LIGHTRT_VK_SRC}/lightrt_vkew.c
    ${LIGHTRT_VK_SRC}/lightrt_vkew.h)
  target_include_directories(lightrt_c_vk PUBLIC ${LIGHTRT_VK_SRC})
  target_compile_definitions(lightrt_c_vk PRIVATE VKEW_IMPLEMENTATION)
  target_link_libraries(lightrt_c_vk ${CMAKE_DL_LIBS})
  set_target_properties(lightrt_c_vk PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

  target_sources(lusdrender PRIVATE ${LIGHTRT_VK_SRC}/lightrt_c_vk.c
                                    ${LIGHTRT_VK_SRC}/lightrt_vkew.c)
  target_compile_definitions(lusdrender PRIVATE HAVE_VULKAN=1)
  target_include_directories(lusdrender PRIVATE ${LIGHTRT_VK_SRC})
endif()

# --- LightRT HIP/ROCm backend (optional, runtime-loaded via hipew) ----------
# A GPU compute trace path mirroring the Vulkan one. The trace kernel is compiled
# at runtime with hiprtc; libamdhip64 + libhiprtc are opened at runtime via hipew,
# so there is NO link-time ROCm dependency and a build with no ROCm still runs
# (the -hip flag then reports the backend unavailable and falls back).
option(LUSDRENDER_WITH_HIP "Build the HIP/ROCm GPU-interop backend" ON)
if(LUSDRENDER_WITH_HIP AND NOT WIN32)
  set(LIGHTRT_HIP_SRC ${PROJECT_SOURCE_DIR}/src/external/lightrt)
  set(HIPEW_SRC ${PROJECT_SOURCE_DIR}/src/external/hipew)
  target_sources(lusdrender PRIVATE ${LIGHTRT_HIP_SRC}/lightrt_c_hip.c
                                    ${HIPEW_SRC}/hipew.c)
  target_compile_definitions(lusdrender PRIVATE HAVE_HIP=1)
  target_include_directories(lusdrender PRIVATE ${LIGHTRT_HIP_SRC} ${HIPEW_SRC})
  target_link_libraries(lusdrender ${CMAKE_DL_LIBS})
endif()

# The viewer defines this GUI-free target before pulling lusdrender in for its
# CPU LightRT dependency. Reuse its CUDA/NVRTC renderer and material ABI here.
if(TARGET lightusd_gpu_rt_common)
  target_link_libraries(lusdrender lightusd_gpu_rt_common)
  target_compile_definitions(lusdrender PRIVATE HAVE_CUDA_RT=1)
  target_include_directories(lusdrender PRIVATE
      ${PROJECT_SOURCE_DIR}/examples/lusdview
      ${PROJECT_SOURCE_DIR}/examples/common)
endif()

# --- LightRT Direct3D 11 backend (Windows only) ----------------------------
# A D3D11 compute trace path mirroring the Vulkan one (the trace shader is
# decompiled from the Vulkan SPIR-V via SPIRV-Cross). d3d11 + d3dcompiler +
# dxgi ship with Windows, so no SDK is required. Useful where the AMD amdvlk
# Vulkan path mis-renders but the D3D driver is solid.
option(LUSDRENDER_WITH_D3D11 "Build the Direct3D 11 GPU-interop backend" ON)
if(LUSDRENDER_WITH_D3D11 AND WIN32)
  set(LIGHTRT_D3D_SRC ${PROJECT_SOURCE_DIR}/src/external/lightrt)
  target_sources(lusdrender PRIVATE ${LIGHTRT_D3D_SRC}/lightrt_c_d3d11.cpp)
  target_include_directories(lusdrender PRIVATE ${LIGHTRT_D3D_SRC})
  target_compile_definitions(lusdrender PRIVATE HAVE_D3D11=1)
  target_link_libraries(lusdrender d3d11 dxgi d3dcompiler)
endif()

# MinGW (e.g. llvm-mingw): statically link the C++/runtime support libs so the
# .exe is self-contained and needs no libc++.dll / libunwind.dll /
# libwinpthread-1.dll alongside it (mirrors lusdview). Windows system DLLs and
# the UCRT stay dynamic. Opt out with -DLUSDRENDER_STATIC_RUNTIME=OFF.
if(MINGW)
  option(LUSDRENDER_STATIC_RUNTIME
         "Statically link the MinGW C++/unwind/pthread runtime into lusdrender" ON)
  if(LUSDRENDER_STATIC_RUNTIME)
    target_link_options(lusdrender PRIVATE -static)
    message(STATUS "lusdrender: static C++ runtime (-static) for MinGW build")
  endif()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(
    ${LIGHTUSD_GPU_RT_VIEW_DIR}/lightrt_mtlx_graph_compile.cc PROPERTIES
    COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O0>")
endif()
