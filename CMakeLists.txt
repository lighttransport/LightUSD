cmake_minimum_required(VERSION 3.16)

set(LIGHTUSD_TARGET "lightusd") # TODO: drop `z`
set(LIGHTUSD_TARGET_NS "lightusd::lightusd") # dll lib. may not be built.
set(LIGHTUSD_TARGET_STATIC_NS "lightusd::lightusd_static") # static lib is always built.
set(LIGHTUSD_TARGET_STATIC "lightusd_static") # alias to ${LIGHTUSD_TARGET_STATIC_NS}
set(BUILD_TARGET_C "c-lightusd")
#set(BUILD_TARGET_BLENDER_PY "lightusd_blender")
set(LIGHTUSD_TEST_TARGET "test_lightusd")
set(LIGHTUSD_BENCHMARK_TARGET "benchmark_lightusd")

project(${LIGHTUSD_TARGET} C CXX)

# PROJECT_IS_TOP_LEVEL is available from cmake 3.21
if (CMAKE_VERSION VERSION_LESS 3.21)
  if ("${CMAKE_PROJECT_NAME}" STREQUAL "${LIGHTUSD_TARGET}")
    set(PROJECT_IS_TOP_LEVEL ON)
  endif()
endif ()

# Default to Release when configured as the top-level project and no build
# type was given. Without this, single-config generators (Make/Ninja) leave
# CMAKE_BUILD_TYPE empty, which on GCC/Clang means -O0 and silently produces
# binaries that are 2-3x slower than expected (observed: lusdcat parsing
# went from 3.8s -> 1.3s after switching from the empty default to Release).
if (PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "LightUSD: CMAKE_BUILD_TYPE not set; defaulting to Release.")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif ()


# Keep the compatibility default until the next product passes the full parity
# gate. Select before configuring legacy dependencies or global compiler flags.
set(LIGHTUSD_NATIVE_PRODUCT "legacy" CACHE STRING "Native implementation: legacy or next")
set_property(CACHE LIGHTUSD_NATIVE_PRODUCT PROPERTY STRINGS legacy next)
if(NOT LIGHTUSD_NATIVE_PRODUCT STREQUAL "legacy" AND
   NOT LIGHTUSD_NATIVE_PRODUCT STREQUAL "next")
  message(FATAL_ERROR "LIGHTUSD_NATIVE_PRODUCT must be legacy or next")
endif()
if(LIGHTUSD_NATIVE_PRODUCT STREQUAL "next")
  option(LIGHTUSD_BUILD_TESTS "Build tests" OFF)
  option(LIGHTUSD_ENABLE_THREAD "Enable optional parallel paths" OFF)
  if(NOT DEFINED LIGHTUSD_NEXT_BUILD_TESTS)
    set(LIGHTUSD_NEXT_BUILD_TESTS ${LIGHTUSD_BUILD_TESTS} CACHE BOOL "Build next tests")
  endif()
  if(NOT DEFINED LIGHTUSD_NEXT_ENABLE_THREAD)
    set(LIGHTUSD_NEXT_ENABLE_THREAD ${LIGHTUSD_ENABLE_THREAD} CACHE BOOL "Enable next parallel paths")
  endif()
  if(LIGHTUSD_NEXT_BUILD_TESTS)
    enable_testing()
  endif()
  add_subdirectory(src/next)
  # Explicit next names prevent consumers from silently binding legacy APIs.
  add_library(lightusd::next ALIAS lightusd_next)
  return()
endif()

if (EMSCRIPTEN)
  set(LIGHTUSD_DEFAULT_NO_WERROR ON)
  set(LIGHTUSD_DEFAULT_PRODUCTION_BUILD On)
  set(LIGHTUSD_DEFAULT_WITH_BUILTIN_IMAGE_LOADER On)
  set(LIGHTUSD_DEFAULT_BUILD_TESTS Off)
  set(LIGHTUSD_DEFAULT_EXAMPLES Off)
  set(LIGHTUSD_DEFAULT_WITH_C_API Off)
  set(LIGHTUSD_DEFAULT_WITH_PXR_COMPAT_API Off)
  # TODO: deprecate in next major version
  set(LIGHTUSD_DEFAULT_WITH_USDA_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_USDC_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_COROUTINE On)
  set(LIGHTUSD_DEFAULT_WITH_MESHOPT On)
elseif(NOT PROJECT_IS_TOP_LEVEL)
  # assume lightusd is added from add_subdirectory()
  # disable tools, tests and examples build by default.
  set(LIGHTUSD_DEFAULT_NO_WERROR OFF)
  set(LIGHTUSD_DEFAULT_PRODUCTION_BUILD Off)
  set(LIGHTUSD_DEFAULT_WITH_BUILTIN_IMAGE_LOADER On)
  set(LIGHTUSD_DEFAULT_BUILD_TESTS Off)
  set(LIGHTUSD_DEFAULT_EXAMPLES Off)
  set(LIGHTUSD_DEFAULT_WITH_C_API Off)
  set(LIGHTUSD_DEFAULT_WITH_PXR_COMPAT_API On)
  # TODO: deprecate in next major version
  set(LIGHTUSD_DEFAULT_WITH_USDA_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_USDC_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_COROUTINE Off)
  set(LIGHTUSD_DEFAULT_WITH_MESHOPT Off)
else()
  set(LIGHTUSD_DEFAULT_NO_WERROR OFF)
  set(LIGHTUSD_DEFAULT_PRODUCTION_BUILD Off)
  set(LIGHTUSD_DEFAULT_WITH_BUILTIN_IMAGE_LOADER On)
  set(LIGHTUSD_DEFAULT_BUILD_TESTS On)
  set(LIGHTUSD_DEFAULT_BUILD_EXAMPLES On)
  set(LIGHTUSD_DEFAULT_WITH_C_API Off)
  set(LIGHTUSD_DEFAULT_WITH_PXR_COMPAT_API On)
  # TODO: deprecate in next major version
  set(LIGHTUSD_DEFAULT_WITH_USDA_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_USDC_PARSER Off)
  set(LIGHTUSD_DEFAULT_WITH_COROUTINE Off)
  set(LIGHTUSD_DEFAULT_WITH_MESHOPT Off)

  # For Visual Studio
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif ()

# options
option(LIGHTUSD_USE_CCACHE "Use ccache for faster recompile." ON)
# NOTE: Measured to NOT help (and slightly hurt codegen) on this codebase: the
# compile cost is dominated by template *instantiation* and -O3 codegen, not by
# header parsing, which is all a PCH can cache. Kept as an opt-in knob; default OFF.
option(LIGHTUSD_USE_PCH "Use a precompiled header (does not help this codebase; opt-in)." OFF)
option(LIGHTUSD_BUILD_SHARED_LIBS "Build as dll?" ${BUILD_SHARED_LIBS})
option(LIGHTUSD_ENABLE_THREAD "Enable optional std::thread parallel paths (default OFF; keep OFF for default WASM/browser builds)" OFF)
option(LIGHTUSD_MSAN "Build with MSAN support(for developer only)" OFF)
option(LIGHTUSD_WITH_C_API "Enable C API." ${LIGHTUSD_DEFAULT_WITH_C_API})
option(LIGHTUSD_BUILD_TESTS "Build tests" ${LIGHTUSD_DEFAULT_BUILD_TESTS})
option(LIGHTUSD_BUILD_PYTHON_TESTS
       "Register the Python binding pytest suite with CTest (requires an installed editable package)"
       OFF)
set(LIGHTUSD_PYTHON_EXECUTABLE "" CACHE FILEPATH
    "Python interpreter used by CTest-integrated tool checks")
set(LIGHTUSD_TEST_FIXTURE_DIR "${PROJECT_SOURCE_DIR}" CACHE PATH
    "Root directory used to resolve relative C++ test fixtures")
# Default external USD asset corpus for regression tests.  The repository-local
# symlink is intentionally the default so a checkout with `usd-assets` present
# exercises the asset gates without machine-specific paths; callers can point
# this at another corpus with -DLIGHTUSD_USD_ASSETS_ROOT=... .
set(_lightusd_default_usd_assets_root "${PROJECT_SOURCE_DIR}/usd-assets")
if(IS_DIRECTORY "${_lightusd_default_usd_assets_root}")
  get_filename_component(_lightusd_default_usd_assets_root
                         "${_lightusd_default_usd_assets_root}" REALPATH)
endif()
set(LIGHTUSD_USD_ASSETS_ROOT "${_lightusd_default_usd_assets_root}" CACHE PATH
    "Root directory for lusdview/lusdrender USD asset regressions")
unset(_lightusd_default_usd_assets_root)
option(LIGHTUSD_BUILD_BENCHMARKS
       "Build some bechmark tests(e.g. internal C++ data structures)" OFF)
option(LIGHTUSD_BUILD_EXAMPLES
       "Build examples(but not all examples in `examples` folder are being built)"
       ${LIGHTUSD_DEFAULT_BUILD_EXAMPLES})
option(LIGHTUSD_BUILD_TOOLS "Build tools (lusddumpcrate, etc.)" OFF)
# Link a drop-in allocator into the CLI example tools (lusdcat,
# tydra_to_renderscene). The legacy flatten/compose path is allocation-heavy
# (~20% of a large-scene `lusdcat -f` is glibc malloc/free/consolidate);
# jemalloc cut representative large-scene runs by 14-17% at ~+4% RSS with
# byte-identical output. Tools only -- the library
# never forces an allocator (apps choose). Same ranking as src/next
# (LIGHTUSD_NEXT_WITH_TCMALLOC): jemalloc > tcmalloc_minimal > mimalloc.
# NOTE: unlike src/next tools, no `malloc_conf` no-purge baking here -- the
# decay-purge stall does not show on this path (measured a wash, +5% RSS).
option(LIGHTUSD_WITH_TCMALLOC
       "Link a drop-in allocator (jemalloc/tcmalloc/mimalloc) into CLI example tools (faster flatten/compose on large scenes)" OFF)
option(LIGHTUSD_BUILD_GUI_VIEWER
       "Build the interactive GUI USD viewer example 'lusdview' (requires OpenGL/GLFW; Vulkan optional). Pulls in window-system deps; keep OFF for headless CI."
       OFF)
option(LIGHTUSD_BUILD_TEXTURE_GPU_BENCH
       "Build the standalone headless Vulkan/CUDA/HIP texture processing benchmark"
       OFF)
option(LIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD
       "Apply NVIDIA PRIME/GLVND offload variables to lusdview CTest cases when the NVIDIA device is visible"
       OFF)
option(LIGHTUSD_BUILD_QUICKLOOK
       "Build the minimal portable USD Quick Look viewer 'lusdquicklook' (software UI via vendored lightui, CPU ray tracing; no GPU required)."
       OFF)
option(LIGHTUSD_WITH_BUILTIN_IMAGE_LOADER
       "Build with built-in image loader(stb_image and fpng). When disabled, app need to provide image loader callback to load images."
       ${LIGHTUSD_DEFAULT_WITH_BUILTIN_IMAGE_LOADER})
option(LIGHTUSD_WITH_TYDRA
       "Build with Tydra module(Handly USD scene converter for the renderer, DCC, etc)."
       ON)
option(LIGHTUSD_WITH_PCP
       "Build the cached/lazy (partial-composition) composition cache (src/pcp)."
       ON)
option(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE
       "Use stable next::pcp::Cache backend for LargeSceneLoader (links lightusd_next)."
       OFF)
option(LIGHTUSD_ENABLE_SIMD
       "Enable SIMD backends (SSE2 on x86, NEON on AArch64) for the next parser; OFF forces the scalar path."
       ON)
option(LIGHTUSD_WITH_MCP_SERVER
       "Build with C++ MCP server(http server) support."
       OFF)
option(LIGHTUSD_WITH_FPNGE
       "Build with fpnge(veluca93) fast PNG encoder. When ON, PNG encoding uses fpnge by default(SIMD path selected by LIGHTUSD_FPNGE_SIMD)."
       OFF)
# fpnge SIMD path, selected at compile time: avx2 | sse41 | sse2 | scalar.
# avx2/sse41 compile fpnge with the matching ISA. sse2/scalar do NOT compile
# fpnge(upstream requires SSE4.1 minimum) and PNG encoding falls back to fpng.
set(LIGHTUSD_FPNGE_SIMD "avx2" CACHE STRING "fpnge SIMD path: avx2, sse41, sse2 or scalar")
set_property(CACHE LIGHTUSD_FPNGE_SIMD PROPERTY STRINGS avx2 sse41 sse2 scalar)
option(LIGHTUSD_WITH_QJS
       "Build with QuickJS(JavaScript) support."
       OFF)

option(LIGHTUSD_WITH_GEOGRAM
       "Build with Geogram library support for advanced geometry processing."
       OFF)

option(LIGHTUSD_WITH_WAMR
       "Build with WAMR (WebAssembly Micro Runtime) support for executing WASM in Tydra."
       OFF)

option(LIGHTUSD_WITH_COROUTINE
       "Build with C++20 coroutine support."
       ${LIGHTUSD_DEFAULT_WITH_COROUTINE})


set(LIGHTUSD_CXX_EXCEPTIONS_DEFAULT Off)

# Enable /MP flags(multi processor build) for Visual Studio
# Based on VTK's Cmake
# No need to set this if you use Ninja Generator
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # default on.
  # disable MP or reduce cores if you have less memory
  set(LIGHTUSD_CXX_MP_FLAG ON CACHE BOOL "Build with /MP flag enabled")
  set(LIGHTUSD_PROCESSOR_COUNT "$ENV{NUMBER_OF_PROCESSORS}")
  set(LIGHTUSD_CXX_MP_NUM_PROCESSORS ${LIGHTUSD_PROCESSOR_COUNT} CACHE STRING "The maximum number of processes for the /MP flag")


  message(STATUS "Use procesors : " ${LIGHTUSD_PROCESSOR_COUNT})

  # /MP flag is set as target_compile_options, so do not add /MP to global CMAKE_CXX_FLAGS variable.
  #if (CMAKE_CXX_MP_FLAG)
  #  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP${CMAKE_CXX_MP_NUM_PROCESSORS}")
  #  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP${CMAKE_CXX_MP_NUM_PROCESSORS}")
  #endif ()
endif()

option(LIGHTUSD_CXX_EXCEPTIONS
       "Enable/Disable C++ exceptions (default: Off; lightusd uses no exceptions)"
       ${LIGHTUSD_CXX_EXCEPTIONS_DEFAULT})

# lightusd (and the vendored tinyvdb) use no RTTI (no dynamic_cast / typeid), so
# disable it by default. This both shrinks the binary and enforces that no RTTI
# creeps in.
option(LIGHTUSD_CXX_RTTI
       "Enable/Disable C++ RTTI (default: Off; lightusd uses no RTTI)"
       Off)

option(LIGHTUSD_WITH_USDMTLX "Build with MaterialX support" ON)
option(LIGHTUSD_WITH_JSON "Build with JSON support" ON)
option(LIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT
       "Build deprecated nlohmann::json object overloads for JSON APIs (minijson is the canonical backend)" OFF)
option(LIGHTUSD_WITH_USD_TO_GLTF "Build with USD to glTF example" ON)
option(LIGHTUSD_WITH_USDOBJ "Build with usdObj support(import wavefront .obj)"
       ON)
option(LIGHTUSD_WITH_USDFBX "Build with usdFbx support(import FBX .fbx)"
        OFF)
option(LIGHTUSD_WITH_USDVOX
       "Build with usdVox support(import MagicaVoxel .vox)" ON)
option(LIGHTUSD_WITH_USDVOL
       "Build with usdVol support(import OpenVDB .vdb via tinyvdb)" ON)

# Subdivision surfaces: tinysubdiv (src/tsd) is always built (dependency-free).
# Optional verification of tsd against an OpenSubdiv source checkout:
option(LIGHTUSD_TSD_VERIFY_WITH_OSD
       "Build tsd vs OpenSubdiv verification test (requires OpenSubdiv_ROOT)" OFF)
set(OpenSubdiv_ROOT "" CACHE PATH "Path to OpenSubdiv source checkout (no pre-build required)")

option(LIGHTUSD_WITH_AUDIO "Build with Audio support(MP3 and WAV)" ON)
option(LIGHTUSD_WITH_ALAC_AUDIO "Build with ALAC(as M4A) Audio support" OFF)

# The Python extension is built by setup.py from src/python using the CPython C API.

option(LIGHTUSD_WITH_PXR_COMPAT_API "Build with pxr compatible API" ${LIGHTUSD_DEFAULT_WITH_PXR_COMPAT_API})

# C API requires Tydra
if (NOT LIGHTUSD_WITH_TYDRA AND LIGHTUSD_WITH_C_API)
  message(WARNING "C API requires Tydra enabled, so disable C API build. Please do not set LIGHTUSD_WITH_TYDRA Off if you need C API library")
  set(LIGHTUSD_WITH_C_API Off CACHE INTERNAL "" FORCE)
endif ()

option(LIGHTUSD_USE_SYSTEM_ZLIB
       "Use system's zlib instead of miniz for TinyEXR/TIFF" OFF)

# -- Zstd compression --
option(LIGHTUSD_WITH_ZSTD_COMPRESSION
       "Enable zstd compression for USD files" ON)
option(LIGHTUSD_USE_SYSTEM_ZSTD
       "Use system's zstd instead of bundled version" OFF)

option(
  LIGHTUSD_PRODUCTION_BUILD
  "Build for production release(e.g. disables debug print, do not include full filepath in error messages)"
  OFF)


option(
  LIGHTUSD_DEBUG_PRINT
  "Enable debug print(For debugging). Disabled when `LIGHTUSD_PRODUCTION_BUILD` is On"
  OFF)

# -- modules --
option(LIGHTUSD_WITH_MODULE_USDA_READER "Build with USDA reader feature" ON)
option(LIGHTUSD_WITH_MODULE_USDA_WRITER "Build with USDA writer feature" ON)

# USDC(Crate binary) reader is mandatory in most situation, so this option is primarily for developer build.
option(LIGHTUSD_WITH_MODULE_USDC_READER "Build with USDC reader feature" ON)
option(LIGHTUSD_WITH_MODULE_USDC_WRITER "Build with USDC writer feature" ON)
# -------------

# -- STB --

#
# In default LightUSD embeds stb_image and stb_image_write to load/save JPG/PNG/BMP images.
# (When LIGHTUSD_WITH_BUILTIN_IMAGE_LOADER is set On)
#

# Set ON if you use stb_image implementation outside of LightUSD library to avoid multiple symbol def error.
option(LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION
       "Do not define STB_IMAGE_IMPLEMENTATION" OFF)

# Set ON if you use stb_image_write implementation outside of LightUSD library in your app to avoid multiple symbol def error.
option(LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION
       "Do not define STB_IMAGE_WRITGE_IMPLEMENTATION" OFF)
# ----------

# -- Wuffs --

#
# Use wuffs to decode jpeg/png/bmp instead of using stb_image.
# (wuffs is well(I guess) fuzz tested image decoder, so has better security than stb_image)
#
option(LIGHTUSD_USE_WUFFS_IMAGE_LOADER
       "Use wuffs to load jpg/png/bmp images" OFF)

# -- nanoimage --
#
# Use nanoimage to decode jpg/png/bmp/tga images.
# nanoimage is a fuzz-tested, memory-safe C image decoder.
# When enabled, replaces the stb_image backend for common image formats.
#
option(LIGHTUSD_WITH_NANOIMAGE
       "Use nanoimage to load jpg/png/bmp/tga images (replaces stb_image for these formats)" ON)
# -----------

# Set ON if you use wuffs implementation outside of LightUSD library in your app to avoid multiple symbol def error.
option(LIGHTUSD_NO_WUFFS_IMPLEMENTATION
       "Do not define WUFFS_IMPLEMENTATION" OFF)
# -----------

#

# -- TIFF --
option(LIGHTUSD_WITH_TIFF
       "Build with TIFF texture(includes 32bit floating point TIFF) support"
       ON)
# ----------

# -- EXR --
option(LIGHTUSD_WITH_EXR "Build with EXR HDR texture support" ON)
option(LIGHTUSD_USE_TINYEXR_V3 "Use the pure-C11 TinyEXR v3 C EXR backend (minimal codecs: no HTJ2K/ZSTD). Set OFF for the deprecated legacy v1 API" ON)
# ---------

option(LIGHTUSD_WITH_TEXTOOLS
       "Build vendored tir/texcomp/texpipe/envmap texture tools" ON)
option(LIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS
       "Build vendored tir/texcomp/texpipe/envmap texture tools in quicklook-only builds" OFF)

if(LIGHTUSD_BUILD_QUICKLOOK AND NOT LIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS)
  # Quicklook is intentionally a minimal, dependency-light path. Disabling
  # texture-tools here avoids building lightusd_textools unless explicitly
  # requested.
  if (LIGHTUSD_WITH_TEXTOOLS)
    message(STATUS "lightusd: disabling LIGHTUSD_WITH_TEXTOOLS for quicklook-only build "
                   "(set -DLIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS=ON to force)")
  endif()
  set(LIGHTUSD_WITH_TEXTOOLS OFF CACHE BOOL "Build vendored tir/texcomp/texpipe/envmap texture tools" FORCE)
endif()

# Vendored textools/tinyexr upstream self-tests are disabled by default: they
# exercise third-party code (not LightUSD), and changes to that vendored copy
# are not part of the LightUSD regression gate. Re-enable with -D
# LIGHTUSD_BUILD_TEXTOOLS_TESTS=ON when validating an upstream sync.
option(LIGHTUSD_BUILD_TEXTOOLS_TESTS
       "Build vendored textools (tir/texcomp/texpipe/envmap) upstream self-tests"
       OFF)

# -- ColorIO --
option(LIGHTUSD_WITH_COLORIO
       "Build with Color IO Baked LUT support(through tinycolorio)" ON)

option(LIGHTUSD_WITH_MESHOPT
       "Build with meshoptimizer support for mesh optimization" ${LIGHTUSD_DEFAULT_WITH_MESHOPT})

option(LIGHTUSD_WITH_REMOTERY
       "Build with Remotery profiling support" OFF)
# ---------

# -- optional tool --
# TODO: deperecate. Use lusdcat
option(LIGHTUSD_WITH_TOOL_USDA_PARSER "Build with USDA parser program" Off)
option(LIGHTUSD_WITH_TOOL_USDC_PARSER "Build with USDC parser program" Off)
# --------------

# -- For developers --
option(LIGHTUSD_COMPILE_TIME_TRACE
       "Add -ftime-trace to profile compilation time(clang only)" OFF)
option(LIGHTUSD_CUSTOM_COMPILE_FLAGS
       "Use hard-coded custom compile flags(described in CMakeLists.txt). For Developer only)" OFF)
# ---

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# When ASan is enabled, propagate the runtime link flag to ALL
# executables. add_sanitizers() (from FindSanitizers.cmake) sets
# LINK_FLAGS as a private target property; any exe target that links
# the asan-instrumented lightusd_static.a but forgets to call
# add_sanitizers() ends up with undefined references to
# __asan_report_load*, __asan_memcpy, etc. Setting CMAKE_EXE_LINKER_FLAGS
# globally avoids that footgun across examples/, tests/feat/,
# tests/pprint/, ... — every binary in the build tree gets the runtime
# automatically.
if(SANITIZE_ADDRESS)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
endif()

# [cifuzz]
option(LIGHTUSD_TEST_CIFUZZ "Run cifuzz test" OFF)

if (LIGHTUSD_TEST_CIFUZZ)
  find_package(cifuzz NO_SYSTEM_ENVIRONMENT_PATH)
  enable_fuzz_testing()
endif()

if (LIGHTUSD_ENABLE_THREAD)
  find_package(Threads REQUIRED)
  # prefer adding "-pthread" compile flag
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()

if(LIGHTUSD_WITH_EXR OR LIGHTUSD_WITH_TIFF)
  if(LIGHTUSD_USE_SYSTEM_ZLIB)
    find_package(ZLIB REQUIRED)
  endif()
endif()


# Currently LightUSD does not claim CXX version when LightUSD is added as add_subdirectory()
# of toplevel(root) project.
# TODO: Property inherit CXX compiler settings from toplevel(root) project.


if (PROJECT_IS_TOP_LEVEL)
  message(STATUS "LightUSD is being built as toplevel project so set CXX standard here.")
  if(LIGHTUSD_WITH_COROUTINE)
    message(STATUS "Use C++20 for c++20 coroutine.")
    # Coroutine support requires C++20
    set(CMAKE_CXX_STANDARD 20)
  else()
    # Require strict C++17 mode(e.g. `-std=c++17`)
    set(CMAKE_CXX_STANDARD 17)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(MSVC)
  # nonstd::expected (src/nonstd/expected.hpp) includes <windows.h> when built
  # without exceptions (our default); without NOMINMAX its min/max macros
  # clobber std::numeric_limits<T>::max()/min() calls project-wide with
  # cryptic C2589/C2059 syntax errors.
  add_compile_definitions(NOMINMAX)

  # MSVC 2022 toolchain + STL header compatibility guard can trip this
  # repository's older C++17 compatibility target set in CI/partner envs.
  # Keep enabled for all targets to avoid STL1001 noise from legacy headers.
  add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

# [ccache]
if(LIGHTUSD_USE_CCACHE)
  if(MSVC)
    # No ccache support
  else()
    find_program(CCACHE_EXE ccache)
    if(CCACHE_EXE)

      message(STATUS "Use ccache : " ${CCACHE_EXE})

      # ccache + precompiled headers: ccache will refuse to cache PCH-using TUs
      # unless told to relax the PCH/time-macro hash checks. Wrap the launcher
      # with `env CCACHE_SLOPPINESS=...` (Unix; MSVC path does not use ccache).
      set(CCACHE_LAUNCHER "${CCACHE_EXE}")
      if(LIGHTUSD_USE_PCH AND CMAKE_HOST_UNIX)
        set(CCACHE_LAUNCHER env CCACHE_SLOPPINESS=pch_defines,time_macros "${CCACHE_EXE}")
      endif()

      # CMAKE_C_COMPILER_LAUNCHER = available from cmake 3.4+)
      if(CMAKE_C_COMPILER_LAUNCHER)
        set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_C_COMPILER_LAUNCHER}"
                                      ${CCACHE_LAUNCHER})
      else()
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_LAUNCHER})
      endif()

      if(CMAKE_CXX_COMPILER_LAUNCHER)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CXX_COMPILER_LAUNCHER}"
                                        ${CCACHE_LAUNCHER})
      else()
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_LAUNCHER})
      endif()

    endif(CCACHE_EXE)
  endif()
endif()


if (LIGHTUSD_WITH_MCP_SERVER)
  set(LIGHTUSD_WITH_JSON On)
endif()

set(LIGHTUSD_SOURCES
    # Crate path utilities (path sorting/tree encoding for USDC writer)
    ${PROJECT_SOURCE_DIR}/src/crate-path-utils/path_sort.cc
    ${PROJECT_SOURCE_DIR}/src/crate-path-utils/tree_encode.cc
    ${PROJECT_SOURCE_DIR}/src/arg-parser.cc
    ${PROJECT_SOURCE_DIR}/src/asset-resolution.cc
    ${PROJECT_SOURCE_DIR}/src/lightusd.cc
    ${PROJECT_SOURCE_DIR}/src/xform.cc
    ${PROJECT_SOURCE_DIR}/src/performance.cc
    ${PROJECT_SOURCE_DIR}/src/spline-binary.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-props.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-entry.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-basetype.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-basetype-inst.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/ascii-parser-timesamples-array.cc
    ${PROJECT_SOURCE_DIR}/src/audio-loader.cc
    ${PROJECT_SOURCE_DIR}/src/base122.cc
    ${PROJECT_SOURCE_DIR}/src/usda-reader.cc
    ${PROJECT_SOURCE_DIR}/src/usda-reader-reconstruct-1.cc
    ${PROJECT_SOURCE_DIR}/src/usda-reader-reconstruct-2.cc
    ${PROJECT_SOURCE_DIR}/src/usda-reader-reconstruct-3.cc
    ${PROJECT_SOURCE_DIR}/src/usda-reader-reconstruct-4.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-property.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim-parse.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim-reconstruct-1.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim-reconstruct-2.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim-reconstruct-3.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-prim-reconstruct-4.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-reader-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/usda-writer.cc
    ${PROJECT_SOURCE_DIR}/src/usdc-writer.cc
    ${PROJECT_SOURCE_DIR}/src/composition.cc
    ${PROJECT_SOURCE_DIR}/src/composition-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/composition-graph.cc
    ${PROJECT_SOURCE_DIR}/src/large-scene-loader.cc
    ${PROJECT_SOURCE_DIR}/src/core/instance-key.cc
    ${PROJECT_SOURCE_DIR}/src/core/path-expression.cc
    ${PROJECT_SOURCE_DIR}/src/core/path-expression-eval.cc
    ${PROJECT_SOURCE_DIR}/src/hash-util.cc
    ${PROJECT_SOURCE_DIR}/src/chunk-reader.cc
    ${PROJECT_SOURCE_DIR}/src/crate-reader.cc
    ${PROJECT_SOURCE_DIR}/src/crate-reader-arrays.cc
    ${PROJECT_SOURCE_DIR}/src/crate-reader-values.cc
    ${PROJECT_SOURCE_DIR}/src/crate-reader-paths.cc
    ${PROJECT_SOURCE_DIR}/src/crate-reader-timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/crate-format.cc
    ${PROJECT_SOURCE_DIR}/src/crate-writer.cc
    ${PROJECT_SOURCE_DIR}/src/crate-writer-values.cc
    ${PROJECT_SOURCE_DIR}/src/crate-writer-inline.cc
    ${PROJECT_SOURCE_DIR}/src/stage-converter.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-geom.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-physics.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-ar.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-media.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-shader.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-light.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-skel.cc
    ${PROJECT_SOURCE_DIR}/src/sconv-layer.cc
    ${PROJECT_SOURCE_DIR}/src/crate-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/crate-dump.cc
    ${PROJECT_SOURCE_DIR}/src/path-util.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct2.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct3.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-geom.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-geom2.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-geom3.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-vol.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-lightprim.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-shader.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-shader2.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-shader3.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-shader4.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-physics.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-physics2.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-physics3.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-physics4.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-ar.cc
    ${PROJECT_SOURCE_DIR}/src/prim-reconstruct-media.cc
    ${PROJECT_SOURCE_DIR}/src/prim-composition.cc
    ${PROJECT_SOURCE_DIR}/src/prim-types.cc
    ${PROJECT_SOURCE_DIR}/src/prim-types-schema.cc
    ${PROJECT_SOURCE_DIR}/src/core/prim-enums.cc
    ${PROJECT_SOURCE_DIR}/src/enum-handlers.cc
    ${PROJECT_SOURCE_DIR}/src/layer.cc
    ${PROJECT_SOURCE_DIR}/src/primvar.cc
    ${PROJECT_SOURCE_DIR}/src/str-util.cc
    ${PROJECT_SOURCE_DIR}/src/usd-dump.cc
    ${PROJECT_SOURCE_DIR}/src/value-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/value-pprint-dispatch.cc
    ${PROJECT_SOURCE_DIR}/src/value-pprint-prim.cc
    ${PROJECT_SOURCE_DIR}/src/value-types.cc
    ${PROJECT_SOURCE_DIR}/src/value-types-typename.cc
    ${PROJECT_SOURCE_DIR}/src/value-types-cast.cc
    ${PROJECT_SOURCE_DIR}/src/value-types-matrix.cc
    ${PROJECT_SOURCE_DIR}/src/value-types-lerp.cc
    ${PROJECT_SOURCE_DIR}/src/color-space.cc
    ${PROJECT_SOURCE_DIR}/src/color-transform.cc
    ${PROJECT_SOURCE_DIR}/src/color-space.hh
    ${PROJECT_SOURCE_DIR}/src/usd-validation.cc
    ${PROJECT_SOURCE_DIR}/src/tiny-format.cc
    ${PROJECT_SOURCE_DIR}/src/tiny-string.cc
    ${PROJECT_SOURCE_DIR}/src/io-util.cc
    ${PROJECT_SOURCE_DIR}/src/image-loader.cc
    ${PROJECT_SOURCE_DIR}/src/ptx-loader.cc
    ${PROJECT_SOURCE_DIR}/src/image-writer.cc
    ${PROJECT_SOURCE_DIR}/src/image-util.cc
    ${PROJECT_SOURCE_DIR}/src/imageio/png-stream.cc
    ${PROJECT_SOURCE_DIR}/src/imageproc/simd.cc
    ${PROJECT_SOURCE_DIR}/src/usdz-convert.cc
    ${PROJECT_SOURCE_DIR}/src/usdz-geometry-optimize.cc
    ${PROJECT_SOURCE_DIR}/src/usdz-material-optimize.cc
    ${PROJECT_SOURCE_DIR}/src/linear-algebra.cc
    ${PROJECT_SOURCE_DIR}/src/value-eval-util.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-accessors.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-scalar-a.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-scalar-b.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-vec-a.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-vec-b.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-role-a.cc
    ${PROJECT_SOURCE_DIR}/src/usdGeom-primvar-inst-role-b.cc
    ${PROJECT_SOURCE_DIR}/src/usdSkel.cc
    ${PROJECT_SOURCE_DIR}/src/usdShade.cc
    ${PROJECT_SOURCE_DIR}/src/usdLux.cc
    # usdMtlX has less dependency(pugixml), so add it to core component
    ${PROJECT_SOURCE_DIR}/src/mtlx-xml-tokenizer.cc
    ${PROJECT_SOURCE_DIR}/src/mtlx-xml-parser.cc
    ${PROJECT_SOURCE_DIR}/src/mtlx-dom.cc
    ${PROJECT_SOURCE_DIR}/src/mtlx-simple-parser.cc
    ${PROJECT_SOURCE_DIR}/src/usdMtlx.cc
    ${PROJECT_SOURCE_DIR}/src/usdMtlx-write.cc
    ${PROJECT_SOURCE_DIR}/src/usdPhysics.cc
    ${PROJECT_SOURCE_DIR}/src/mjcPhysics.cc
    ${PROJECT_SOURCE_DIR}/src/usdObj.cc
    ${PROJECT_SOURCE_DIR}/src/image-loader.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-enum.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-meta.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-geom.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-shader.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-light.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-skel.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-physics.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-ar.cc
    ${PROJECT_SOURCE_DIR}/src/pprint-media.cc
    ${PROJECT_SOURCE_DIR}/src/pprinter.cc
    ${PROJECT_SOURCE_DIR}/src/timesamples-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/timesamples-eval.cc
    ${PROJECT_SOURCE_DIR}/src/stage.cc
    ${PROJECT_SOURCE_DIR}/src/stage.hh
    ${PROJECT_SOURCE_DIR}/src/uuid-gen.cc
    ${PROJECT_SOURCE_DIR}/src/uuid-gen.hh
    ${PROJECT_SOURCE_DIR}/src/parser-timing.cc
    ${PROJECT_SOURCE_DIR}/src/sha256.cc
    ${PROJECT_SOURCE_DIR}/src/typed-array.cc
    ${PROJECT_SOURCE_DIR}/src/task-queue.cc
    ${PROJECT_SOURCE_DIR}/src/task-queue.hh
    ${PROJECT_SOURCE_DIR}/src/prim-pprint-parallel.cc
    ${PROJECT_SOURCE_DIR}/src/prim-pprint-parallel.hh

    )

if (LIGHTUSD_WITH_PCP)
    list(APPEND LIGHTUSD_SOURCES
        ${PROJECT_SOURCE_DIR}/src/pcp/layer-registry.cc
        ${PROJECT_SOURCE_DIR}/src/pcp/layer-registry.hh
        ${PROJECT_SOURCE_DIR}/src/pcp/cache.cc
        ${PROJECT_SOURCE_DIR}/src/pcp/cache.hh
        ${PROJECT_SOURCE_DIR}/src/pcp/cache-impl.hh
        ${PROJECT_SOURCE_DIR}/src/pcp/cache-parallel.cc
    )
endif()

if (LIGHTUSD_WITH_TYDRA)
    list(APPEND LIGHTUSD_SOURCES
        ${PROJECT_SOURCE_DIR}/src/tydra/facial.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/facial.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/prim-apply.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/prim-apply.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-xform-node.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-skeleton.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-physics.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-collection.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-primvar.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-terminal.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-blendshape.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-property.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-property-names.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-listprims-inst.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access-listshaders-inst.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-access.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-analysis.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/scene-analysis.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/color-management.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/color-management.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/attribute-eval.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/attribute-eval.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/attribute-eval-typed-all.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/attribute-eval-typed-inst-scalar.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/attribute-eval-typed-inst-array.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/command-and-history.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/obj-export.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/usd-export.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/shader-network.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/shader-network.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-state.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-xform-anim.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-stage-instances.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-memory.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-texture.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-optimize.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-dedup.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-deferred.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-hierarchy.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-analytic-basic.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-analytic-solid.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-mesh.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-mesh-tangent.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-material.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-material-assembly.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-material-ktx2.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-mesh-visitor.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-material-signature.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-material-mtlx.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-anim.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-anim-light.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-anim-skeleton.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-anim-property.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-anim-value-clip.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-instancer.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-internal.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-pprint.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-data-pprint.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/raytracing-data.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/raytracing-data.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/raytracing-scene-converter.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/raytracing-scene-converter.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/material-serializer.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/material-serializer.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/materialx-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/materialx-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/physics-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/physics-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/urdf-to-usd.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/urdf-to-usd.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/ar-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/ar-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/ik-solver.h
        ${PROJECT_SOURCE_DIR}/src/tydra/ik-solver.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/ik-solver.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-math.h
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-collision.h
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-collision.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-dynamics.h
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-dynamics.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/rb-dynamics.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-scene-dump.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/render-scene-dump.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/bone-util.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/bone-util.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/layer-to-renderscene.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/layer-to-renderscene.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/texture-util.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/texture-util.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/common-utils.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/common-utils.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-support.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-support.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-converter.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-converter.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-applier.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/variant-applier.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-scene.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-scene.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-query.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-query.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-composition.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-composition.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-validate.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-validate.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-usdz.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-usdz.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-diff.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-diff.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-resources.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-resources.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-server.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-server.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-context.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-js-bridge.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/mcp-js-bridge.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/value-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/value-to-json-native.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/value-to-json-native-decode.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/value-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/diff-and-compare.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/diff-and-compare.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/js-script.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/js-script.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/js-script-persistent.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/threejs-exporter.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/threejs-exporter.cc
        )

    if(LIGHTUSD_WITH_WAMR)
        list(APPEND LIGHTUSD_SOURCES
            ${PROJECT_SOURCE_DIR}/src/tydra/wasm-runtime.cc
            ${PROJECT_SOURCE_DIR}/src/tydra/wasm-runtime.hh
        )
    endif(LIGHTUSD_WITH_WAMR)
endif (LIGHTUSD_WITH_TYDRA)

# Variant extraction utilities handled via variant-support.hh/cc in Tydra section

if(LIGHTUSD_WITH_PXR_COMPAT_API)
  list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/pxr-compat.cc)
endif()

set(LIGHTUSD_C_API_SOURCES
    ${PROJECT_SOURCE_DIR}/src/c-lightusd.cc
    ${PROJECT_SOURCE_DIR}/src/c-lightusd-helpers.cc
    ${PROJECT_SOURCE_DIR}/src/c-lightusd-tydra.cc)

set(LIGHTUSD_DEP_SOURCES
    ${PROJECT_SOURCE_DIR}/src/integerCoding.cpp
    ${PROJECT_SOURCE_DIR}/src/lz4-compression.cc
    ${PROJECT_SOURCE_DIR}/src/lz4/lz4.c
    #${PROJECT_SOURCE_DIR}/src/external/pystring.cpp
)

# MikkTSpace tangent computation (zlib license, by Morten S. Mikkelsen)
if(LIGHTUSD_WITH_TYDRA)
  list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/mikktspace/mikktspace.c)
endif()

set(LIGHTUSD_FPNG_ENABLED OFF)
if (LIGHTUSD_WITH_BUILTIN_IMAGE_LOADER)
  set(LIGHTUSD_FPNG_ENABLED ON)
  list(APPEND LIGHTUSD_DEP_SOURCES  ${PROJECT_SOURCE_DIR}/src/external/fpng.cpp)
  # Disable SSE for a while, since -mpclmul required to use SSE feature of fpng.
  set_source_files_properties(${PROJECT_SOURCE_DIR}/src/external/fpng.cpp
                              PROPERTIES COMPILE_DEFINITIONS "FPNG_NO_SSE=1")
endif()

# Image decode/resize/encode TUs are the hot path of texture re-encode (USDZ
# conversion of large scenes). MinSizeRel/-Oz builds (e.g. the wasm targets)
# cripple them (PNG transcode drops to ~2 MP/s), for a few hundred KB of code
# size: compile just these TUs at -O3 regardless of build type.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/image-loader.cc       # stb_image decode
    ${PROJECT_SOURCE_DIR}/src/image-writer.cc       # stb_image_write / fpng entry
    ${PROJECT_SOURCE_DIR}/src/image-util.cc         # stb_image_resize
    ${PROJECT_SOURCE_DIR}/src/imageio/png-stream.cc # streaming PNG transcode/resize
    ${PROJECT_SOURCE_DIR}/src/tydra/texture-util.cc # ResizeImage
    ${PROJECT_SOURCE_DIR}/src/external/fpng.cpp     # fpng PNG encoder
    ${PROJECT_SOURCE_DIR}/src/external/miniz.c      # zlib inflate/deflate (PNG transcode)
    PROPERTIES COMPILE_OPTIONS "-O3")
endif()

# Keep sanitizer builds useful without reporting implementation techniques in
# vendored image codecs as LightUSD failures. fpng has a portable unaligned-I/O
# path, so select it under UBSan. stb_image_resize2 and stb_image_write embed
# their implementations in our wrapper TUs and intentionally use unaligned
# wide copies / signed bit-buffer shifts; disable only those checks for those
# two TUs while retaining ASan and every other UBSan category.
if (SANITIZE_UNDEFINED AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_property(
    SOURCE ${PROJECT_SOURCE_DIR}/src/external/fpng.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS "FPNG_USE_UNALIGNED_LOADS=0")
  set_property(
    SOURCE ${PROJECT_SOURCE_DIR}/src/image-util.cc
    APPEND PROPERTY COMPILE_OPTIONS "-fno-sanitize=alignment")
  set_property(
    SOURCE ${PROJECT_SOURCE_DIR}/src/image-writer.cc
    APPEND PROPERTY COMPILE_OPTIONS "-fno-sanitize=shift")
endif()

# fpnge: fast PNG encoder. Upstream requires SSE4.1 minimum(AVX2 preferred);
# there is no SSE2/scalar code path, so for sse2/scalar we skip compiling fpnge
# and let PNG encoding fall back to the portable fpng encoder.
set(LIGHTUSD_FPNGE_ENABLED OFF)
if (LIGHTUSD_WITH_FPNGE)
  string(TOLOWER "${LIGHTUSD_FPNGE_SIMD}" _fpnge_simd)
  if (_fpnge_simd STREQUAL "avx2" OR _fpnge_simd STREQUAL "sse41")
    set(LIGHTUSD_FPNGE_ENABLED ON)
    list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/fpnge/fpnge.cc)
    if (MSVC)
      if (_fpnge_simd STREQUAL "avx2")
        set_source_files_properties(${PROJECT_SOURCE_DIR}/src/external/fpnge/fpnge.cc
                                    PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
      endif()
      # MSVC implies SSE4.1/PCLMUL inside fpnge.cc; no extra flag needed for sse41.
    else()
      if (_fpnge_simd STREQUAL "avx2")
        set_source_files_properties(${PROJECT_SOURCE_DIR}/src/external/fpnge/fpnge.cc
                                    PROPERTIES COMPILE_OPTIONS "-mavx2;-mpclmul")
      else()
        set_source_files_properties(${PROJECT_SOURCE_DIR}/src/external/fpnge/fpnge.cc
                                    PROPERTIES COMPILE_OPTIONS "-msse4.1;-mpclmul")
      endif()
    endif()
    message(STATUS "fpnge: enabled (SIMD=${_fpnge_simd})")
  else()
    message(STATUS "fpnge: requested SIMD '${_fpnge_simd}' has no fpnge code path; PNG encoding falls back to fpng.")
  endif()
endif()

# Native SIMD for the image-processing row kernels (src/imageproc/simd.cc).
# When enabled on x86 GCC/Clang, the kernels use function multi-versioning
# (target_clones + ifunc) to dispatch SSE2/SSE4.1/AVX/AVX2 at runtime. Off, or
# on non-x86 / non-ELF / MSVC, a single auto-vectorized build is used. (WASM
# auto-vectorizes via -msimd128 regardless; see web/CMakeLists.txt.)
option(LIGHTUSD_ENABLE_NATIVE_SIMD "Runtime-dispatched native SIMD for image kernels" ON)
# Disable FP contraction so vectorized kernels match the scalar reference bit-for-bit.
if (NOT MSVC)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/src/imageproc/simd.cc
                              PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
if (LIGHTUSD_ENABLE_NATIVE_SIMD AND NOT EMSCRIPTEN AND NOT MSVC AND
    (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86" OR
     CMAKE_SYSTEM_PROCESSOR STREQUAL ""))
  set_source_files_properties(${PROJECT_SOURCE_DIR}/src/imageproc/simd.cc
                              PROPERTIES COMPILE_DEFINITIONS LIGHTUSD_IMAGEPROC_MULTIVERSION)
  message(STATUS "imageproc: native SIMD multi-versioning enabled (sse2/sse4.1/avx/avx2 runtime dispatch)")
else()
  message(STATUS "imageproc: native SIMD multi-versioning disabled (single auto-vectorized build)")
endif()

if(LIGHTUSD_WITH_USDMTLX)
  list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/pugixml.cpp)
endif()

if(LIGHTUSD_WITH_JSON)
  list(APPEND LIGHTUSD_SOURCES
    ${PROJECT_SOURCE_DIR}/src/minijson.cc
    ${PROJECT_SOURCE_DIR}/src/json-to-usd.cc
    ${PROJECT_SOURCE_DIR}/src/usd-to-json.cc
    ${PROJECT_SOURCE_DIR}/src/json-writer.cc
    ${PROJECT_SOURCE_DIR}/src/json-util.cc)

  list(APPEND LIGHTUSD_DEP_SOURCES  ${PROJECT_SOURCE_DIR}/src/external/yyjson.c)
endif()

if(LIGHTUSD_WITH_USDFBX)
  list(APPEND LIGHTUSD_SOURCES
    ${PROJECT_SOURCE_DIR}/src/usdFbx.cc
  )
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/OpenFBX/src/ofbx.cpp)
endif()

if(LIGHTUSD_WITH_USDOBJ)
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/tiny_obj_loader.cc)
endif()

if(LIGHTUSD_WITH_USDVOX)
  list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/usdVox.cc)
endif()

if(LIGHTUSD_WITH_USDVOL)
  list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/usdVol.cc)
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/tinyvdb/tinyvdb_io.c)
endif()

if(LIGHTUSD_WITH_TIFF)
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/tiny_dng_loader.cc)
  if(LIGHTUSD_USE_SYSTEM_ZLIB)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/external/tiny_dng_loader.cc
      PROPERTIES COMPILE_DEFINITIONS "TINY_DNG_LOADER_ENABLE_ZIP=1;TINY_DNG_LOADER_USE_SYSTEM_ZLIB=1")
  else()
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/external/tiny_dng_loader.cc
      PROPERTIES INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src/external
                 COMPILE_DEFINITIONS "TINY_DNG_LOADER_ENABLE_ZIP=1")
  endif()

  # NOTE: tiny_dng_loader doesn't depend on tinyexr,
  # tinyexr is now added separately in the LIGHTUSD_WITH_EXR section
endif(LIGHTUSD_WITH_TIFF)

if(LIGHTUSD_WITH_EXR)
  if(LIGHTUSD_USE_TINYEXR_V3)
    # TinyEXR v3 C API (pure-C11 reader/writer; upstream `include/exr.h`).
    # The vendored src/external/tinyexr/src/ holds only the minimal codec set:
    # HTJ2K (EXR_NO_JPH) and ZSTD (EXR_NO_ZSTD) are compiled out for a small
    # footprint. The v3 C core is self-contained (its own DEFLATE via fpnge), so
    # it depends on neither miniz nor zlib.
    message(STATUS "LightUSD: Using TinyEXR v3 C API (pure-C11, minimal codecs)")

    file(GLOB TINYEXR_V3_SOURCES
         ${PROJECT_SOURCE_DIR}/src/external/tinyexr/src/*.c)

    set_source_files_properties(${TINYEXR_V3_SOURCES} PROPERTIES
      INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/external/tinyexr/include;${PROJECT_SOURCE_DIR}/src/external/tinyexr/src"
      COMPILE_DEFINITIONS "EXR_NO_JPH;EXR_NO_ZSTD")

    list(APPEND LIGHTUSD_DEP_SOURCES ${TINYEXR_V3_SOURCES})

  else()
    # TinyEXR v1 API (legacy; deprecated, kept behind LIGHTUSD_USE_TINYEXR_V3=OFF).
    message(STATUS "LightUSD: Using TinyEXR v1 API (legacy)")

    if(LIGHTUSD_USE_SYSTEM_ZLIB)
      set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/src/external/tinyexr.cc
        PROPERTIES COMPILE_DEFINITIONS "TINYEXR_USE_MINIZ=0")
    else()
      set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/src/external/tinyexr.cc
        PROPERTIES INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src/external)
    endif()

    list(APPEND LIGHTUSD_DEP_SOURCES
         ${PROJECT_SOURCE_DIR}/src/external/tinyexr.cc)
  endif()
endif(LIGHTUSD_WITH_EXR)

# LightUSD's PTEX reader and nanoimage backend use miniz directly. Keep this
# bundled dependency even when TinyDNG itself is configured to use system zlib.
list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/miniz.c)
set_source_files_properties(
  ${PROJECT_SOURCE_DIR}/src/external/miniz.c
  PROPERTIES COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE=1")

# -- Zstd compression --
if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  if(NOT LIGHTUSD_USE_SYSTEM_ZSTD)
    list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/zstd.c)
    # Disable ASM to avoid linker issues, suppress warnings for third-party code
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/external/zstd.c
      PROPERTIES COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM=1"
                 COMPILE_FLAGS "-w")
  else()
    find_package(zstd REQUIRED)
  endif()
  list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/zstd-compression.cc)
endif()

# Vendored pure-C11 texture processing stack used by lusdview/lusdrender.
if(LIGHTUSD_WITH_TEXTOOLS)
  set(TEXTOOLS_DIR ${PROJECT_SOURCE_DIR}/src/external/textools)
  file(GLOB TEXTOOLS_SOURCES CONFIGURE_DEPENDS
       ${TEXTOOLS_DIR}/resize/src/*.c
       ${TEXTOOLS_DIR}/texcomp/src/*.c
       ${TEXTOOLS_DIR}/texpipe/src/*.c
       ${TEXTOOLS_DIR}/envmap/src/*.c)
  add_library(lightusd_textools STATIC ${TEXTOOLS_SOURCES})
  target_include_directories(lightusd_textools PUBLIC
      ${TEXTOOLS_DIR}/resize/include
      ${TEXTOOLS_DIR}/texcomp/include
      ${TEXTOOLS_DIR}/texpipe/include
      ${TEXTOOLS_DIR}/envmap/include)
  target_compile_features(lightusd_textools PRIVATE c_std_11)
  if(NOT MSVC)
    target_compile_options(lightusd_textools PRIVATE -w)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64" AND NOT MSVC)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=armv8-a+sve" TEXTOOLS_HAS_SVE)
    if(TEXTOOLS_HAS_SVE)
      set_source_files_properties(
          ${TEXTOOLS_DIR}/resize/src/tir_kernels_sve.c
          PROPERTIES COMPILE_OPTIONS "-march=armv8-a+sve")
    endif()
  endif()
  if(UNIX AND NOT EMSCRIPTEN)
    target_compile_definitions(lightusd_textools PRIVATE
        TIR_ENABLE_THREADS TIR_THREADS_PTHREAD)
    find_package(Threads REQUIRED)
    target_link_libraries(lightusd_textools PUBLIC Threads::Threads)
  endif()
  if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(lightusd_textools PUBLIC m)
  endif()
endif()

# -- nanoimage --
if(LIGHTUSD_WITH_NANOIMAGE)
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_alloc.c
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_bmp.c
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_jpeg.c
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_png.c
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_tga.c
    PROPERTIES COMPILE_FLAGS "-w")
  # nanoimage_zlib uses miniz (bundled) instead of system zlib
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_zlib.c
    PROPERTIES COMPILE_FLAGS "-w" COMPILE_DEFINITIONS "NI_USE_MINIZ"
               INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src/external)
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_alloc.c
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_bmp.c
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_jpeg.c
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_png.c
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_tga.c
       ${PROJECT_SOURCE_DIR}/src/external/nanoimage/nanoimage_zlib.c)
endif()

if(LIGHTUSD_WITH_ALAC_AUDIO)
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/EndianPortable.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/ALACBitUtilities.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/ALACDecoder.cpp
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/ALACEncoder.cpp
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/ag_dec.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/ag_enc.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/dp_dec.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/dp_enc.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/matrix_dec.c
       ${PROJECT_SOURCE_DIR}/src/external/alac/codec/matrix_enc.c
  )
endif(LIGHTUSD_WITH_ALAC_AUDIO)

if(LIGHTUSD_WITH_MCP_SERVER)

  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c
  )
endif(LIGHTUSD_WITH_MCP_SERVER)

if(LIGHTUSD_WITH_QJS)

  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/quickjs-ng/cutils.c
       ${PROJECT_SOURCE_DIR}/src/external/quickjs-ng/libregexp.c
       ${PROJECT_SOURCE_DIR}/src/external/quickjs-ng/libunicode.c
       ${PROJECT_SOURCE_DIR}/src/external/quickjs-ng/quickjs.c
       ${PROJECT_SOURCE_DIR}/src/external/quickjs-ng/xsum.c
  )
endif(LIGHTUSD_WITH_QJS)

# tinysubdiv (tsd): dependency-free subdivision surface library, always built.
list(APPEND LIGHTUSD_SOURCES
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-validate.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-topology.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-bilinear.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-catmark.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-loop.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-fvar.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-refine.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-stream.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-limit.cc
  ${PROJECT_SOURCE_DIR}/src/tsd/tsd-lightusd.cc
)

if(LIGHTUSD_WITH_GEOGRAM)
  # Geogram basic core files (header-only minimal set)
  set(GEOGRAM_BASIC_SOURCES
    # Start with empty list - headers are included via include directories
  )

  # For now, just provide headers for Geogram integration
  # Advanced functionality can be added later as needed
  set(GEOGRAM_POINTS_SOURCES
    # Core geometry processing headers are available
  )

  set(GEOGRAM_DELAUNAY_SOURCES
    # Core triangulation headers are available
  )

  # Minimal triangle library (C code, no exceptions)
  set(GEOGRAM_THIRD_PARTY_SOURCES
    ${PROJECT_SOURCE_DIR}/src/external/geogram/geogram/third_party/triangle/triangle.c
  )

  # Combine all Geogram sources
  set(GEOGRAM_ALL_SOURCES
    ${GEOGRAM_BASIC_SOURCES}
    ${GEOGRAM_POINTS_SOURCES}
    ${GEOGRAM_DELAUNAY_SOURCES}
    ${GEOGRAM_THIRD_PARTY_SOURCES}
  )

  # Add Geogram sources to LightUSD
  list(APPEND LIGHTUSD_DEP_SOURCES ${GEOGRAM_ALL_SOURCES})

  # Add Geogram include directory
  set(GEOGRAM_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/src/external/geogram)

  # Set Geogram-specific compile properties
  set_source_files_properties(${GEOGRAM_ALL_SOURCES}
                              PROPERTIES COMPILE_DEFINITIONS "GEO_STATIC_LIBS;GEOGRAM_WITH_LEGACY_NUMERICS")

endif(LIGHTUSD_WITH_GEOGRAM)

if(LIGHTUSD_WITH_WAMR)
  # WAMR core sources (minimal runtime for WebAssembly execution)
  set(WAMR_CORE_SOURCES
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common/wasm_runtime_common.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common/wasm_native.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common/wasm_exec_env.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common/wasm_memory.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common/wasm_c_api.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/interpreter/wasm_loader.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/interpreter/wasm_runtime.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/interpreter/wasm_interp_classic.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/mem-alloc/mem_alloc.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/mem-alloc/ems/ems_alloc.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/mem-alloc/ems/ems_hmu.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/mem-alloc/ems/ems_kfc.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_assert.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_common.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_hashmap.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_list.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_log.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_queue.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/bh_vector.c
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils/runtime_timer.c
  )

  # Add WAMR platform-specific sources
  if(WIN32)
    list(APPEND WAMR_CORE_SOURCES
      ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/platform/windows/platform_init.c
    )
  else()
    list(APPEND WAMR_CORE_SOURCES
      ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/platform/linux/platform_init.c
    )
  endif()

  # Add WAMR sources to LightUSD
  list(APPEND LIGHTUSD_DEP_SOURCES ${WAMR_CORE_SOURCES})

  # Add WAMR include directories
  set(WAMR_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/include
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/interpreter
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/iwasm/common
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/utils
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/mem-alloc
    ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/platform/include
  )

  # Add platform-specific include directory
  if(WIN32)
    list(APPEND WAMR_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/platform/windows)
  else()
    list(APPEND WAMR_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/external/wamr/core/shared/platform/linux)
  endif()

  # Set WAMR-specific compile definitions
  if(WIN32)
    set_source_files_properties(${WAMR_CORE_SOURCES}
                                PROPERTIES COMPILE_DEFINITIONS "BH_PLATFORM_WINDOWS=1")
  else()
    set_source_files_properties(${WAMR_CORE_SOURCES}
                                PROPERTIES COMPILE_DEFINITIONS "BH_PLATFORM_LINUX=1")
  endif()

endif(LIGHTUSD_WITH_WAMR)

if(LIGHTUSD_WITH_MESHOPT)
  # meshoptimizer source files
  set(MESHOPTIMIZER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/allocator.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/clusterizer.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/indexanalyzer.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/indexcodec.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/indexgenerator.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/overdrawoptimizer.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/partition.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/quantization.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/rasterizer.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/simplifier.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/spatialorder.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/stripifier.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/vcacheoptimizer.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/vertexcodec.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/vertexfilter.cpp
    ${PROJECT_SOURCE_DIR}/src/external/meshoptimizer/vfetchoptimizer.cpp
  )

  list(APPEND LIGHTUSD_DEP_SOURCES ${MESHOPTIMIZER_SOURCES})
endif(LIGHTUSD_WITH_MESHOPT)

if(LIGHTUSD_WITH_REMOTERY)
  # Remotery source files
  set(REMOTERY_SOURCES
    ${PROJECT_SOURCE_DIR}/src/external/Remotery/Remotery.c
  )

  list(APPEND LIGHTUSD_DEP_SOURCES ${REMOTERY_SOURCES})

  # Add Remotery include directory
  list(APPEND LIGHTUSD_DEP_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/external/Remotery)

  # Define RMT_ENABLED to enable Remotery
  add_definitions(-DRMT_ENABLED=1)

  # Platform-specific definitions for Remotery
  if(WIN32)
    add_definitions(-DRMT_USE_D3D11=0)
  elseif(APPLE)
    add_definitions(-DRMT_USE_METAL=0)
  elseif(UNIX)
    add_definitions(-DRMT_USE_OPENGL=0)
  endif()
endif(LIGHTUSD_WITH_REMOTERY)

if(LIGHTUSD_WITH_TIFF OR LIGHTUSD_WITH_EXR)
  if(LIGHTUSD_USE_SYSTEM_ZLIB)
    list(APPEND LIGHTUSD_EXT_LIBRARIES ZLIB::ZLIB)
  endif()
endif()


# Increase warning level for clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")

  set(LIGHTUSD_COMPILE_FLAGS
      "-Weverything -Wno-poison-system-directories -Wno-padded -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-documentation -Wno-unused-member-function -Wno-switch-enum -Wno-exit-time-destructors"
  )

  # Suppress pragma message warnings from external libraries (miniz.c)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-#pragma-messages")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-#pragma-messages")

  option(LIGHTUSD_NO_WERROR "Don't set -Werror when building the lightusd library" ${LIGHTUSD_DEFAULT_NO_WERROR})
  if (NOT LIGHTUSD_NO_WERROR)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Werror")
  endif()

  # clang17 seems enable -Wunsafe-buffer-usage when `-Weverything` is defined,
  # resulting compilation error for some raw array access without bound check.
  # https://reviews.llvm.org/D138940
  # https://github.com/llvm/llvm-project/issues/64646
  # we need to suppress it as a work around.
  if (${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 16)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Wno-unsafe-buffer-usage")
  endif()

  # this is actually a work around of false-positive behavior of clang in Androind NDK 27.
  if (${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Wno-switch-default")
  endif()

  include(CheckCXXCompilerFlag)
  # clang 21+ (e.g. llvm-mingw clang 22) also tightens
  # -Wthread-safety-negative under -Weverything. Suppress it when supported
  # rather than requiring negative-capability annotations throughout the code.
  check_cxx_compiler_flag("-Wthread-safety-negative" LIGHTUSD_HAS_WTHREAD_SAFETY_NEGATIVE)
  if (LIGHTUSD_HAS_WTHREAD_SAFETY_NEGATIVE)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Wno-thread-safety-negative")
  endif()

  if(LIGHTUSD_COMPILE_TIME_TRACE)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -ftime-trace ")
  endif()

  set_source_files_properties(${LIGHTUSD_SOURCES}
                              PROPERTIES COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS}")

  # nlohmann/json is vendored third-party code. Clang 21's -Weverything emits
  # diagnostics from its header during template instantiation, so suppress only
  # those third-party warning classes on translation units that include it.
  set(LIGHTUSD_NLOHMANN_JSON_WARNING_FLAGS
      " -Wno-covered-switch-default -Wno-weak-vtables")
  set(LIGHTUSD_NLOHMANN_JSON_SOURCES
      ${PROJECT_SOURCE_DIR}/src/usd-to-json.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-js-bridge.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-resources.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-server.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-composition.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-query.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-scene.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-validate.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-diff.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/mcp-tools-usdz.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/urdf-to-usd.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/value-to-json.cc
      ${PROJECT_SOURCE_DIR}/src/tydra/js-script-persistent.cc)
  foreach(LIGHTUSD_NLOHMANN_JSON_SOURCE ${LIGHTUSD_NLOHMANN_JSON_SOURCES})
    set_property(SOURCE ${LIGHTUSD_NLOHMANN_JSON_SOURCE}
                 APPEND_STRING PROPERTY COMPILE_FLAGS
                 ${LIGHTUSD_NLOHMANN_JSON_WARNING_FLAGS})
  endforeach()

  if (LIGHTUSD_WITH_C_API)
    set_source_files_properties(${LIGHTUSD_C_API_SOURCES}
                                PROPERTIES COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS}")
  endif()

# Increase warning level for gcc.
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")

  set(LIGHTUSD_COMPILE_FLAGS
      "-Wall "
  )

  option(LIGHTUSD_NO_WERROR "Don't set -Werror when building the lightusd library" ${LIGHTUSD_DEFAULT_NO_WERROR})
  if (NOT LIGHTUSD_NO_WERROR)
    set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Werror")
  endif()

  set_source_files_properties(${LIGHTUSD_SOURCES}
                              PROPERTIES COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS}")

  if (LIGHTUSD_WITH_C_API)
    set_source_files_properties(${LIGHTUSD_C_API_SOURCES}
                                PROPERTIES COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS}")
  endif()
endif()

if(LIGHTUSD_CUSTOM_COMPILE_FLAGS)
  #
  # Overwrite existing LIGHTUSD_COMPILE_FLAGS
  #

  #if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(LIGHTUSD_CUSTOM_COMPILE_FLAGS "-fstack-usage ")
    set_source_files_properties(${LIGHTUSD_SOURCES}
                              PROPERTIES COMPILE_FLAGS "${LIGHTUSD_CUSTOM_COMPILE_FLAGS}")

    if (LIGHTUSD_WITH_C_API)
      set_source_files_properties(${LIGHTUSD_C_API_SOURCES}
                                  PROPERTIES COMPILE_FLAGS "${LIGHTUSD_CUSTOM_COMPILE_FLAGS}")
    endif()
  #endif()
endif()

#
# -- target
#

#
# Disabled Object library for a while, since it has some issue on MSVC target.
## To save compile time, use Object library(non-archival collections) for SHARED and Static lib.
## NOTE: Static lib is always being built with PIC enabled.
##
##
#add_library(lightusd_object OBJECT ${LIGHTUSD_SOURCES} ${LIGHTUSD_DEP_SOURCES} ${LIGHTUSD_EXT_SOURCES})
#set_target_properties(lightusd_object PROPERTIES FOLDER "lightusd")
#set_target_properties(lightusd_object PROPERTIES POSITION_INDEPENDENT_CODE ON)

# LightUSD should compile well without /bigobj flag.
# Uncommend if you encounter the following errror: fatal  error C1128: number of sections exceeded object file format limit: compile with /bigobj
#if(MSVC)
#  target_compile_options(lightusd_object PRIVATE /bigobj)
#elif(WIN32)
#  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
#    # guess mingw-gcc
#    target_compile_options(lightusd_object PRIVATE "-Wa, -mbig-obj")
#  endif()
#endif()

#if (MSVC)
#  if (LIGHTUSD_CXX_MP_FLAG)
#    target_compile_options(lightusd_object PRIVATE /MP${CMAKE_CXX_MP_NUM_PROCESSORS})
#  endif()
#endif()
#
#if(LIGHTUSD_DEBUG_PRINT)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_DEBUG_PRINT")
#endif()
#
#if(LIGHTUSD_PRODUCTION_BUILD)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_PRODUCTION_BUILD")
#endif()
#
## default = enable module, so invert definition
#if(NOT LIGHTUSD_WITH_MODULE_USDA_READER)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_DISABLE_MODULE_USDA_READER")
#endif()
#
#if(NOT LIGHTUSD_WITH_MODULE_USDA_WRITER)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_DISABLE_MODULE_USDA_WRITER")
#endif()
#
#if(NOT LIGHTUSD_WITH_MODULE_USDC_READER)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_DISABLE_MODULE_USDC_READER")
#endif()
#
#if(NOT LIGHTUSD_WITH_MODULE_USDC_WRITER)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_DISABLE_MODULE_USDC_WRITER")
#endif()
#
#if(NOT LIGHTUSD_WITH_BUILTIN_IMAGE_LOADER)
#  target_compile_definitions(lightusd_object PRIVATE "LIGHTUSD_NO_BUILTIN_IMAGE_LOADER")
#endif()
#
#if(LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION)
#  target_compile_definitions(lightusd_object PRIVATE "LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION")
#endif()
#
#if(LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION)
#  target_compile_definitions(lightusd_object PRIVATE "LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION")
#endif()
#
#target_include_directories(lightusd_object
#                           PRIVATE ${PROJECT_SOURCE_DIR}/src)

#if(LIGHTUSD_WITH_JSON)
##target_include_directories(
##  ${LIGHTUSD_LIB_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src/external/jsonhpp/)
#  target_include_directories(
#    ${LIGHTUSD_LIB_TARGET}
#    PRIVATE ${PROJECT_SOURCE_DIR}/src/external/jsonhpp/)
#endif()

#if(LIGHTUSD_WITH_USDMTLX)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_USE_USDMTLX")
#endif()
#
#if(LIGHTUSD_WITH_USDFBX)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_USE_USDFBX")
#endif()
#
#if(LIGHTUSD_WITH_USDOBJ)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_USE_USDOBJ")
#endif()
#
#if(LIGHTUSD_WITH_USDVOX)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_USE_USDVOX")
#endif()

#target_link_libraries(lightusd_object ${LIGHTUSD_EXT_LIBRARIES}
#                      ${CMAKE_DL_LIBS})
#
#if (LIGHTUSD_ENABLE_THREAD)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_ENABLE_THREAD")
#  target_link_libraries(lightusd_object Threads::Threads)
#endif()
#
#
#if(IOS)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_BUILD_IOS")
#endif()
#
#if(LIGHTUSD_WITH_TIFF)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_WITH_TIFF")
#endif(LIGHTUSD_WITH_TIFF)
#
#if(LIGHTUSD_WITH_EXR)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_WITH_EXR")
#endif(LIGHTUSD_WITH_EXR)
#
#if(LIGHTUSD_WITH_COLORIO)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_WITH_COLORIO")
#endif(LIGHTUSD_WITH_COLORIO)
#
#if(LIGHTUSD_WITH_AUDIO)
#  target_compile_definitions(lightusd_object
#                             PRIVATE "LIGHTUSD_WITH_AUDIO")
#endif(LIGHTUSD_WITH_AUDIO)
#
#if(LIGHTUSD_WITH_ALAC_AUDIO)
#  target_compile_definitions(tinuusdz_object
#                             PRIVATE "LIGHTUSD_WITH_ALAC_AUDIO")
#endif(LIGHTUSD_WITH_ALAC_AUDIO)
#
# TODO: USE set_target_properties(... PROPERTIES OUTPUT_NAME outname)?
#add_library(${LIGHTUSD_TARGET_STATIC} STATIC $<TARGET_OBJECTS:lightusd_object>)
#add_sanitizers(${LIGHTUSD_TARGET_STATIC})

add_library(${LIGHTUSD_TARGET_STATIC} STATIC ${LIGHTUSD_SOURCES} ${LIGHTUSD_DEP_SOURCES} ${LIGHTUSD_EXT_SOURCES})
set_target_properties(${LIGHTUSD_TARGET_STATIC} PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_sanitizers(${LIGHTUSD_TARGET_STATIC})
#set_target_properties(lightusd_object PROPERTIES FOLDER "lightusd")

# Target with namespace
add_library(${LIGHTUSD_TARGET_STATIC_NS} ALIAS ${LIGHTUSD_TARGET_STATIC})


if(LIGHTUSD_BUILD_SHARED_LIBS)
  #add_library(
  #  ${LIGHTUSD_TARGET} SHARED $<TARGET_OBJECTS:lightusd_object>)
  add_library(
    ${LIGHTUSD_TARGET} SHARED ${LIGHTUSD_SOURCES} ${LIGHTUSD_DEP_SOURCES} ${LIGHTUSD_EXT_SOURCES})
  add_sanitizers(${LIGHTUSD_TARGET})

  # for dllexport
  target_compile_definitions(${LIGHTUSD_TARGET} PRIVATE "LIGHTUSD_COMPILE_LIBRARY")
  target_compile_definitions(${LIGHTUSD_TARGET} PRIVATE "LIGHTUSD_SHARED_LIBRARY")

  set(LIGHTUSD_LIBS ${LIGHTUSD_TARGET_STATIC} ${LIGHTUSD_TARGET})
else()
  # static only
  set(LIGHTUSD_LIBS ${LIGHTUSD_TARGET_STATIC})
endif()

if(WIN32 AND LIGHTUSD_WITH_PXR_COMPAT_API)
  if(LIGHTUSD_BUILD_SHARED_LIBS)
    target_compile_definitions(${LIGHTUSD_TARGET} PRIVATE "PXR_DYNAMIC")
    target_compile_definitions(${LIGHTUSD_TARGET} PRIVATE "USD_EXPORTS")
  endif()

endif()

#
# Optional: next::pcp::Cache backend for LargeSceneLoader
#
if(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next lightusd_next_build)
endif()

#
# CMake's Object libary have some issue on MSVC build, so build STATIC and SHARED lib indepdendently.
#

#
# -- common target props
#
foreach(LIGHTUSD_LIB_TARGET ${LIGHTUSD_LIBS})

  # Precompiled header to amortize parsing of large, stable headers across the
  # many C++ TUs. Guarded with COMPILE_LANGUAGE:CXX because the library also
  # contains C sources (zstd.c, lz4.c, yyjson.c, ...) which cannot use a C++ PCH.
  if(LIGHTUSD_USE_PCH)
    target_precompile_headers(${LIGHTUSD_LIB_TARGET} PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/src/lightusd-pch.hh>")
  endif()

  # Although LightUSD should compile well without /bigobj flag,
  # corrent LightUSD need to use bigobj format otherwise we'll get: fatal  error C1128: number of sections exceeded object file format limit: compile with /bigobj
  if(MSVC)
    target_compile_options(${LIGHTUSD_LIB_TARGET} PRIVATE /bigobj)
  endif()

  if (MSVC)
    if (LIGHTUSD_CXX_MP_FLAG)
      target_compile_options(${LIGHTUSD_LIB_TARGET} PRIVATE /MP${CMAKE_CXX_MP_NUM_PROCESSORS})
    endif()
  endif()

  # Set a 8MB default stack size on Windows.
  # It defaults to 1MB on MSVC, which is the same as our current JS stack size,
  # so it will overflow and crash otherwise.
  # On MinGW it defaults to 2MB.
  if (LIGHTUSD_WITH_QJS)
    if(WIN32)
       if(MSVC)
           target_compile_options(${LIGHTUSD_LIB_TARGET} PRIVATE /STACK:8388608)
       endif()
    endif()
  endif()

  if(LIGHTUSD_DEBUG_PRINT)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DEBUG_PRINT")
  endif()

  if(LIGHTUSD_PRODUCTION_BUILD)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_PRODUCTION_BUILD")
  endif()

  # default = enable module, so invert definition
  if(NOT LIGHTUSD_WITH_MODULE_USDA_READER)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DISABLE_MODULE_USDA_READER")
  endif()

  if(NOT LIGHTUSD_WITH_MODULE_USDA_WRITER)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DISABLE_MODULE_USDA_WRITER")
  endif()

  if(NOT LIGHTUSD_WITH_MODULE_USDC_READER)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DISABLE_MODULE_USDC_READER")
  endif()

  if(NOT LIGHTUSD_WITH_MODULE_USDC_WRITER)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DISABLE_MODULE_USDC_WRITER")
  endif()

  if(LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET} PRIVATE "LIGHTUSD_NO_STB_IMAGE_IMPLEMENTATION")
  endif()

  if(LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET} PRIVATE "LIGHTUSD_NO_STB_IMAGE_WRITE_IMPLEMENTATION")
  endif()

  if(LIGHTUSD_WITH_NANOIMAGE)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET} PRIVATE "LIGHTUSD_WITH_NANOIMAGE")
  endif()

  if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET} PRIVATE "LIGHTUSD_WITH_ZSTD_COMPRESSION")
    if(LIGHTUSD_USE_SYSTEM_ZSTD)
      target_link_libraries(${LIGHTUSD_LIB_TARGET} PUBLIC zstd::zstd)
    endif()
  endif()

  target_include_directories(${LIGHTUSD_LIB_TARGET}
                             PRIVATE ${PROJECT_SOURCE_DIR}/src)

  if(LIGHTUSD_WITH_JSON)
    #target_include_directories(
    #  ${LIGHTUSD_LIB_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src/external/jsonhpp/)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET} PRIVATE "LIGHTUSD_WITH_JSON")
    if(LIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT)
      target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                                 PUBLIC "LIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT")
    endif()
  endif()

  if(LIGHTUSD_WITH_USDMTLX)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_USE_USDMTLX")
  endif()

  if(LIGHTUSD_WITH_USDFBX)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_USE_USDFBX")
  endif()

  if(LIGHTUSD_WITH_USDOBJ)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_USE_USDOBJ")
  endif()

  if(LIGHTUSD_WITH_USDVOX)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_USE_USDVOX")
  endif()

  if(LIGHTUSD_WITH_USDVOL)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_USE_USDVOL")
  endif()

  if(LIGHTUSD_WITH_PCP)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_PCP")
  endif()

  if(NOT LIGHTUSD_ENABLE_SIMD)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_DISABLE_SIMD")
  endif()

  if(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PUBLIC "LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE")
    target_link_libraries(${LIGHTUSD_LIB_TARGET} lightusd_next)
  endif()

  target_link_libraries(${LIGHTUSD_LIB_TARGET} ${LIGHTUSD_EXT_LIBRARIES}
                        ${CMAKE_DL_LIBS})

  if (LIGHTUSD_ENABLE_THREAD)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_ENABLE_THREAD")
    target_link_libraries(${LIGHTUSD_LIB_TARGET} Threads::Threads)
  endif()

  # On 32-bit systems, 64-bit atomic operations require libatomic
  # (Skip for emscripten as libatomic doesn't exist in wasm environment)
  # (Skip for MSVC as atomics are built into the runtime)
  if (CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT EMSCRIPTEN AND NOT MSVC)
    target_link_libraries(${LIGHTUSD_LIB_TARGET} atomic)
  endif()

  if(IOS)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_BUILD_IOS")
  endif()

  if(LIGHTUSD_WITH_TIFF)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_TIFF")
  endif(LIGHTUSD_WITH_TIFF)

  if(LIGHTUSD_WITH_EXR)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_EXR")
    if(LIGHTUSD_USE_TINYEXR_V3)
      # Select the pure-C11 v3 C EXR backend in image-loader.cc / image-writer.cc.
      target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                                 PRIVATE "LIGHTUSD_EXR_V3=1")
    endif()
  endif(LIGHTUSD_WITH_EXR)

  if(LIGHTUSD_WITH_COLORIO)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_COLORIO")
  endif(LIGHTUSD_WITH_COLORIO)

  if(LIGHTUSD_WITH_TEXTOOLS)
    # KTX2 / GPU-compressed texture decode in image-loader.cc goes through the
    # vendored texpipe/texcomp reader (tp_ktx2_read + tc_*_decompress). Link the
    # C texture libs into core; the PUBLIC include dirs of lightusd_textools
    # propagate texpipe.h/texcomp.h to the core sources.
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_TEXTOOLS")
    # NOTE: core uses the plain target_link_libraries signature everywhere; keep
    # it plain here so CMake doesn't reject a mixed plain/keyword usage.
    target_link_libraries(${LIGHTUSD_LIB_TARGET} lightusd_textools)
  endif(LIGHTUSD_WITH_TEXTOOLS)

  if(LIGHTUSD_WITH_AUDIO)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_AUDIO")
  endif(LIGHTUSD_WITH_AUDIO)

  if(LIGHTUSD_WITH_ALAC_AUDIO)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_ALAC_AUDIO")
  endif(LIGHTUSD_WITH_ALAC_AUDIO)

  if(LIGHTUSD_WITH_MCP_SERVER)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_MCP_SERVER")

    # use dll for SSL + OPENSSL_API_3.0
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "OPENSSL_API_3_0")
  endif(LIGHTUSD_WITH_MCP_SERVER)

  if(LIGHTUSD_FPNGE_ENABLED)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_HAVE_FPNGE")
  endif(LIGHTUSD_FPNGE_ENABLED)

  if(LIGHTUSD_FPNG_ENABLED)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_HAVE_FPNG")
  endif(LIGHTUSD_FPNG_ENABLED)

  if(LIGHTUSD_WITH_QJS)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_QJS")
  endif(LIGHTUSD_WITH_QJS)

  if(LIGHTUSD_WITH_TYDRA)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_TYDRA")
  endif(LIGHTUSD_WITH_TYDRA)

  if(LIGHTUSD_WITH_GEOGRAM)
    target_include_directories(${LIGHTUSD_LIB_TARGET} PRIVATE ${GEOGRAM_INCLUDE_DIR})
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_GEOGRAM")
    # Geogram-specific definitions
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "GEO_STATIC_LIBS")
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "GEOGRAM_WITH_LEGACY_NUMERICS")
  endif(LIGHTUSD_WITH_GEOGRAM)

  if(LIGHTUSD_WITH_WAMR)
    target_include_directories(${LIGHTUSD_LIB_TARGET} PRIVATE ${WAMR_INCLUDE_DIRS})
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_WAMR")
    # WAMR-specific definitions
    if(WIN32)
      target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                                 PRIVATE "BH_PLATFORM_WINDOWS=1")
    else()
      target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                                 PRIVATE "BH_PLATFORM_LINUX=1")
    endif()
  endif(LIGHTUSD_WITH_WAMR)

  if(LIGHTUSD_WITH_COROUTINE)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_COROUTINE")
  endif(LIGHTUSD_WITH_COROUTINE)

  if(LIGHTUSD_WITH_MESHOPT)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE "LIGHTUSD_WITH_MESHOPT")
  endif(LIGHTUSD_WITH_MESHOPT)

  if(LIGHTUSD_CXX_EXCEPTIONS)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE LIGHTUSD_CXX_EXCEPTIONS=1)
  else()
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE LIGHTUSD_CXX_EXCEPTIONS=0 JSON_NOEXCEPTION=1)
  endif()

  if(LIGHTUSD_CXX_RTTI)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE LIGHTUSD_CXX_RTTI=1)
  else()
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PRIVATE LIGHTUSD_CXX_RTTI=0)
  endif()

  if(NOT LIGHTUSD_CXX_EXCEPTIONS)
    if(MSVC)
      target_compile_options(${LIGHTUSD_LIB_TARGET}
                             PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
      target_compile_options(${LIGHTUSD_LIB_TARGET}
                             PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
  endif()

  if(NOT LIGHTUSD_CXX_RTTI)
    if(MSVC)
      target_compile_options(${LIGHTUSD_LIB_TARGET}
                             PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    else()
      # -fno-rtti is a C++-only flag; scope it so the vendored C sources
      # (lz4.c / miniz.c / zstd.c) do not warn.
      target_compile_options(${LIGHTUSD_LIB_TARGET}
                             PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
  endif()

endforeach()




if(LIGHTUSD_WITH_C_API)
  if(LIGHTUSD_BUILD_SHARED_LIBS)
    add_library(${BUILD_TARGET_C} SHARED ${LIGHTUSD_C_API_SOURCES})
    # Link with static(object) version of liblightusd to make dll monolitic
    # TODO: Build LIGHTUSD_TARGET_STATIC as Object?
    target_link_libraries(${BUILD_TARGET_C} PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_compile_definitions(${BUILD_TARGET_C} PRIVATE "LIGHTUSD_COMPILE_LIBRARY")
    target_compile_definitions(${BUILD_TARGET_C} PRIVATE "LIGHTUSD_SHARED_LIBRARY")
    target_include_directories(
      ${BUILD_TARGET_C} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  endif()

  add_library(${BUILD_TARGET_C}_static STATIC ${LIGHTUSD_C_API_SOURCES})
  target_link_libraries(${BUILD_TARGET_C}_static PRIVATE ${LIGHTUSD_TARGET_STATIC})
  target_include_directories(
    ${BUILD_TARGET_C}_static PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()

# TODO: deprecate in next major version
if(LIGHTUSD_WITH_TOOL_USDA_PARSER AND LIGHTUSD_WITH_MODULE_USDA_READER)
  add_executable(
    usda_parser ${CMAKE_CURRENT_SOURCE_DIR}/examples/usda_parser/parser-main.cc)
  add_sanitizers(usda_parser)
  if(LIGHTUSD_PRODUCTION_BUILD)
    target_compile_definitions(usda_parser
                               PRIVATE "LIGHTUSD_PRODUCTION_BUILD")
  endif()
  target_include_directories(usda_parser PRIVATE ${PROJECT_SOURCE_DIR}/src/)
  target_link_libraries(usda_parser PRIVATE ${LIGHTUSD_TARGET_STATIC})
endif()

# TODO: deprecate in next major version
if(LIGHTUSD_WITH_TOOL_USDC_PARSER AND LIGHTUSD_WITH_MODULE_USDC_READER)
  add_executable(
    usdc_parser ${CMAKE_CURRENT_SOURCE_DIR}/examples/usdc_parser/usdc-parser-main.cc)
  add_sanitizers(usdc_parser)
  if(LIGHTUSD_PRODUCTION_BUILD)
    target_compile_definitions(usdc_parser
                               PRIVATE "LIGHTUSD_PRODUCTION_BUILD")
  endif()
  target_include_directories(usdc_parser PRIVATE ${PROJECT_SOURCE_DIR}/src/)
  target_link_libraries(usdc_parser PRIVATE ${LIGHTUSD_TARGET_STATIC})
endif()

# Enable CTest before the example subdirectories are added, so a subdirectory
# (e.g. examples/lusdview) can register its own tests with add_test(). The
# main enable_testing() inside the LIGHTUSD_BUILD_TESTS block below runs after
# these add_subdirectory() calls, which is too late for the subdirectory's own
# add_test() to take effect. Calling it twice is harmless.
if(LIGHTUSD_BUILD_TESTS OR (LIGHTUSD_BUILD_TEXTURE_GPU_BENCH AND BUILD_TESTING))
  enable_testing()
endif()

set(LIGHTUSD_TOOLS_ALLOCATOR_LIB "")
if(LIGHTUSD_WITH_TCMALLOC)
  find_library(LIGHTUSD_ALLOC_JEMALLOC NAMES jemalloc)
  if(LIGHTUSD_ALLOC_JEMALLOC)
    set(LIGHTUSD_TOOLS_ALLOCATOR_LIB "${LIGHTUSD_ALLOC_JEMALLOC}")
  else()
    find_library(LIGHTUSD_ALLOC_TCMALLOC NAMES tcmalloc_minimal tcmalloc)
    if(NOT LIGHTUSD_ALLOC_TCMALLOC)
      # Runtime-only installs often lack the -dev `.so` symlink.
      find_file(LIGHTUSD_ALLOC_TCMALLOC
        NAMES libtcmalloc_minimal.so.4 libtcmalloc.so.4
        PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib)
    endif()
    if(LIGHTUSD_ALLOC_TCMALLOC)
      set(LIGHTUSD_TOOLS_ALLOCATOR_LIB "${LIGHTUSD_ALLOC_TCMALLOC}")
    else()
      find_library(LIGHTUSD_ALLOC_MIMALLOC NAMES mimalloc)
      if(LIGHTUSD_ALLOC_MIMALLOC)
        set(LIGHTUSD_TOOLS_ALLOCATOR_LIB "${LIGHTUSD_ALLOC_MIMALLOC}")
      endif()
    endif()
  endif()
  if(LIGHTUSD_TOOLS_ALLOCATOR_LIB)
    message(STATUS "lightusd: linking allocator ${LIGHTUSD_TOOLS_ALLOCATOR_LIB} into CLI example tools")
  else()
    message(WARNING "LIGHTUSD_WITH_TCMALLOC=ON but no jemalloc/tcmalloc/mimalloc found; using system malloc")
  endif()
endif()

if(LIGHTUSD_BUILD_EXAMPLES)
  add_subdirectory(examples/lusdcat)
  if(LIGHTUSD_WITH_TEXTOOLS)
    # Authors .ktx2 texture companions + the legacy-safe customData hints.
    add_subdirectory(examples/usd-texcomp)
  endif()
  if (LIGHTUSD_WITH_TYDRA)
    add_subdirectory(examples/tydra_api)
    add_subdirectory(examples/tydra_to_renderscene)
    add_subdirectory(examples/variant-lister)
    add_subdirectory(examples/variant-analyzer)
    add_subdirectory(examples/urdf-to-usd)

    if (LIGHTUSD_BUILD_GUI_VIEWER)
      add_subdirectory(examples/lusdview)
    endif()

    # Independent of LIGHTUSD_BUILD_GUI_VIEWER on purpose: lusdquicklook shares
    # no code with lusdview and needs no window-system or GPU dependency.
    if (LIGHTUSD_BUILD_QUICKLOOK)
      add_subdirectory(examples/lusdquicklook)
    endif()

    if (LIGHTUSD_WITH_MCP_SERVER)
      add_subdirectory(examples/mcp_server)
    endif()

    if (LIGHTUSD_WITH_QJS)
      add_subdirectory(examples/js-script)
    endif()
  endif ()

  add_subdirectory(examples/api_tutorial)
  add_subdirectory(examples/file_format)
  add_subdirectory(examples/asset_resolution)
  add_subdirectory(examples/progressive_composition)
  add_subdirectory(examples/large-scene-load)
  #if (LIGHTUSD_WITH_JSON)
  #  add_subdirectory(examples/usd_to_json)
  #endif()
  #if (LIGHTUSD_WITH_USD_TO_GLTF)
  #  add_subdirectory(examples/usd_to_gltf)
  #endif()
  if(LIGHTUSD_WITH_MODULE_USDA_WRITER)
    add_subdirectory(examples/save_usda)
  endif()
  if(LIGHTUSD_WITH_C_API)
    add_subdirectory(examples/c_api_example)
  endif()
endif(LIGHTUSD_BUILD_EXAMPLES)

if(LIGHTUSD_BUILD_TEXTURE_GPU_BENCH)
  if(LIGHTUSD_WITH_TEXTOOLS)
    add_subdirectory(tools/lusdview-texture-bench)
  else()
    message(WARNING "LIGHTUSD_BUILD_TEXTURE_GPU_BENCH requires LIGHTUSD_WITH_TEXTOOLS=ON")
  endif()
endif()

# Register tool-local tests added below as part of the top-level test tree.
if(LIGHTUSD_BUILD_TESTS)
  enable_testing()
endif()

# Tools
if(LIGHTUSD_BUILD_TOOLS)
  add_subdirectory(tools/lusddumpcrate)
  add_subdirectory(tools/lusdzconvert)
  add_subdirectory(tools/lusdchecker)
  # lusddiff uses the Tydra diff-and-compare API.
  if(LIGHTUSD_WITH_TYDRA)
    add_subdirectory(tools/lusddiff)
    # examples/lusdview (added earlier when LIGHTUSD_BUILD_GUI_VIEWER is also
    # ON) may have already pulled this in for its lightrt_c CPU RT backend.
    if(NOT TARGET lusdrender)
      add_subdirectory(tools/lusdrender)
    endif()
  endif()
endif()

if(LIGHTUSD_BUILD_BENCHMARKS)

  #
  # Standalone benchmark exe.
  #
  set(LIGHTUSD_BENCH_SOURCES ${PROJECT_SOURCE_DIR}/benchmarks/benchmark-main.cc)

  add_executable(${LIGHTUSD_BENCHMARK_TARGET} ${LIGHTUSD_BENCH_SOURCES})
  add_sanitizers(${LIGHTUSD_BENCHMARK_TARGET})

  target_include_directories(
    ${LIGHTUSD_BENCHMARK_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                         ${PROJECT_SOURCE_DIR}/bencharks)
  target_link_libraries(${LIGHTUSD_BENCHMARK_TARGET}
                        PRIVATE ${LIGHTUSD_TARGET_STATIC})

endif(LIGHTUSD_BUILD_BENCHMARKS)

# [VisualStudio]
if(WIN32)
  # Set ${LIGHTUSD_TARGET_STATIC} as a startup project for VS IDE
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ${LIGHTUSD_TARGET_STATIC})

  # For easier debugging in VS IDE(cmake 3.8.0 or later required) Set working
  # directory to ${LIGHTUSD_TARGET_STATIC} git repo root.
  if(CMAKE_VERSION VERSION_GREATER 3.8.0)
    set_target_properties(
      ${LIGHTUSD_TARGET_STATIC} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY
                                           "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS)
  #
  # textools (vendored tir/texcomp/texpipe/envmap) upstream self-tests.
  # Plain-C main() programs, no GPU/display needed. Disabled by default
  # (LIGHTUSD_BUILD_TEXTOOLS_TESTS=OFF): these test vendored third-party code,
  # not LightUSD. Enable explicitly when validating a textools sync.
  #
  if(LIGHTUSD_WITH_TEXTOOLS AND LIGHTUSD_BUILD_TEXTOOLS_TESTS)
    set(TEXTOOLS_TEST_PROGRAMS
      "textools_tir_test|${TEXTOOLS_DIR}/resize/tests/tir_test.c|textools-tir"
      "textools_tir_f16_test|${TEXTOOLS_DIR}/resize/tests/tir_f16_test.c|textools-tir-f16"
      "textools_texcomp_test|${TEXTOOLS_DIR}/texcomp/test/test_texcomp.c|textools-texcomp"
      "textools_texpipe_test|${TEXTOOLS_DIR}/texpipe/test/test_texpipe.c|textools-texpipe"
      "textools_envmap_test|${TEXTOOLS_DIR}/envmap/test/test_envmap.c|textools-envmap"
      "textools_pbr_test|${TEXTOOLS_DIR}/envmap/test/test_pbr.c|textools-pbr")
    foreach(entry IN LISTS TEXTOOLS_TEST_PROGRAMS)
      string(REPLACE "|" ";" entry "${entry}")
      list(GET entry 0 tt_target)
      list(GET entry 1 tt_source)
      list(GET entry 2 tt_name)
      add_executable(${tt_target} ${tt_source})
      target_link_libraries(${tt_target} PRIVATE lightusd_textools)
      # Some tests poke library internals (tir_internal.h / texcomp_internal.h).
      target_include_directories(${tt_target} PRIVATE
        ${TEXTOOLS_DIR}/resize/src ${TEXTOOLS_DIR}/texcomp/src)
      target_compile_features(${tt_target} PRIVATE c_std_11)
      if(NOT MSVC)
        target_compile_options(${tt_target} PRIVATE -w)
      endif()
      add_test(NAME ${tt_name} COMMAND ${tt_target})
      set_tests_properties(${tt_name} PROPERTIES LABELS "textools")
    endforeach()
  endif()

  # (The lusdview unit tests are registered inside
  # examples/lusdview/CMakeLists.txt, where their targets are in scope; that
  # works because enable_testing() is now called before that subdirectory is
  # added — see the top-level enable_testing() call.)

  #
  # Standalone tester
  #
  set(LIGHTUSD_TEST_SOURCES ${PROJECT_SOURCE_DIR}/tests/test-main.cc)

  add_executable(${LIGHTUSD_TEST_TARGET} ${LIGHTUSD_TEST_SOURCES})
  add_sanitizers(${LIGHTUSD_TEST_TARGET})

  target_include_directories(
    ${LIGHTUSD_TEST_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                    ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(${LIGHTUSD_TEST_TARGET}
                        PRIVATE ${LIGHTUSD_TARGET_STATIC})

  #
  # unit tester
  #
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/unit)

  #
  # MCP tests (requires MCP server enabled)
  #
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/mcp)

  #
  # Compression tester
  # (its not involved with lightusd codes, and well tested now. so disabled. will be removed in future release)
  #
  # add_subdirectory(${PROJECT_SOURCE_DIR}/tests/decompress-int)

  #
  # Pretty-print benchmark
  #
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/pprint)


  # Prefer an explicitly configured interpreter so CTest can use a virtualenv,
  # cross-build host Python, or another interpreter selected by CI. Keep the
  # automatic lookup as the default for ordinary native builds.
  if(LIGHTUSD_PYTHON_EXECUTABLE)
    set(LUSDR_PYTHON "${LIGHTUSD_PYTHON_EXECUTABLE}")
  else()
    find_program(LUSDR_PYTHON NAMES python3 python)
  endif()

  if(LIGHTUSD_BUILD_PYTHON_TESTS AND LUSDR_PYTHON)
    execute_process(
      COMMAND ${LUSDR_PYTHON} -c "import pytest, lightusd"
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      RESULT_VARIABLE LIGHTUSD_PYTHON_TEST_DEPS_RESULT
      OUTPUT_QUIET
      ERROR_QUIET)
    if(LIGHTUSD_PYTHON_TEST_DEPS_RESULT EQUAL 0)
      add_test(
        NAME python-lightusd-tests
        COMMAND ${LUSDR_PYTHON} -m pytest
                "${PROJECT_SOURCE_DIR}/python/tests"
                -k "not slow and not large and not usd_assets and not aousd" -q)
      set_tests_properties(
        python-lightusd-tests
        PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                   TIMEOUT 600
                   LABELS "python;bindings")
    else()
      message(WARNING
              "LIGHTUSD_BUILD_PYTHON_TESTS=ON but `${LUSDR_PYTHON}` cannot import pytest and lightusd; Python tests will not be registered")
    endif()
  endif()
  if(TARGET lusdrender AND LUSDR_PYTHON)
    add_test(
      NAME tool-lusdrender-smoke
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_lusdrender_smoke.py
              "$<TARGET_FILE:lusdrender>"
              ${PROJECT_SOURCE_DIR}
              ${CMAKE_BINARY_DIR}/tools/lusdrender
    )

    add_test(
      NAME tool-lusdrender-materialx-cpu-graph
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_materialx_cpu_graph.py
              "$<TARGET_FILE:lusdrender>" ${PROJECT_SOURCE_DIR})
    set_tests_properties(tool-lusdrender-materialx-cpu-graph PROPERTIES
                         TIMEOUT 45 LABELS "lusdrender;cpu;materialx;graph")

    add_test(
      NAME tool-lusdrender-materialx-flake-parity
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_materialx_flake_parity.py
              "$<TARGET_FILE:lusdrender>")
    set_tests_properties(tool-lusdrender-materialx-flake-parity PROPERTIES
                         TIMEOUT 100 SKIP_RETURN_CODE 77
                         LABELS "lusdrender;gpu;materialx;graph;parity")

    add_test(
      NAME tool-lusdrender-materialx-projection-parity
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_materialx_projection_parity.py
              "$<TARGET_FILE:lusdrender>")
    set_tests_properties(tool-lusdrender-materialx-projection-parity PROPERTIES
                         TIMEOUT 100 SKIP_RETURN_CODE 77
                         LABELS "lusdrender;gpu;materialx;graph;parity")

    # The legacy (eager tydra) path handles every .usda/.usdz; it used to drop
    # every UsdUVTexture and render a flat constant base color.
    add_test(
      NAME tool-lusdrender-legacy-texture
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_legacy_texture.py
              "$<TARGET_FILE:lusdrender>"
              ${PROJECT_SOURCE_DIR}
              ${CMAKE_BINARY_DIR}/tools/lusdrender
    )
    set_tests_properties(tool-lusdrender-legacy-texture
                         PROPERTIES SKIP_RETURN_CODE 77)

    add_test(
      NAME tool-lusdrender-colorspace-regression
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_colorspace_regression.py
              "$<TARGET_FILE:lusdrender>"
              ${PROJECT_SOURCE_DIR}
              --output ${CMAKE_BINARY_DIR}/tools/lusdrender/colorspace-regression
    )
    set_tests_properties(tool-lusdrender-colorspace-regression
                         PROPERTIES SKIP_RETURN_CODE 77)

    # -autoframe on a FLAT scene (a quad / ground plane) used to frame it exactly
    # edge-on and render black.
    add_test(
      NAME tool-lusdrender-autoframe-flat
      COMMAND ${LUSDR_PYTHON}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_autoframe_flat.py
              "$<TARGET_FILE:lusdrender>"
              ${PROJECT_SOURCE_DIR}
              ${CMAKE_BINARY_DIR}/tools/lusdrender
    )
    set_tests_properties(tool-lusdrender-autoframe-flat
                         PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  # lusdrender's shell regressions in tools/lusdrender/tests/. These existed for
  # a long time with NO add_test() anywhere, so nothing ran them and the docs
  # cited ctest names that did not resolve; run-degraded-material.sh had been
  # segfaulting (infinite MtlxConverter <-> ConvertMaterial recursion) unnoticed.
  # Each takes the binary as $1 and exits 77 to skip.
  #
  # Only the hermetic ones are registered: run-gpu-render.sh needs a GPU, and
  # run-large-scene-profiles.sh / run-material-resolver.sh /
  # run-alpha-blend-sort-external.sh need external multi-GB asset corpora.
  #
  # bash is required (Git Bash/WSL on Windows). Without this find_program guard
  # -- mirroring LUSDR_PYTHON's guard on the smoke tests above -- ctest can't
  # find/launch the COMMAND at all on a native Windows box with neither on
  # PATH, which fails hard rather than the intended SKIP_RETURN_CODE 77 skip
  # (that property only takes effect once the test process actually runs).
  find_program(LUSDR_BASH NAMES bash)
  if(TARGET lusdrender AND LUSDR_BASH)
    add_test(
      NAME tool-lusdrender-gpu-render
      COMMAND ${LUSDR_BASH}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-gpu-render.sh
              "$<TARGET_FILE:lusdrender>")
    set_tests_properties(tool-lusdrender-gpu-render PROPERTIES
                         SKIP_RETURN_CODE 77 LABELS "lusdrender;gpu;rt;materialx;openpbr")
    if(LUSDR_PYTHON)
      add_test(
        NAME tool-lusdrender-materialx-openpbr-parity
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tools/lusdrender/check_materialx_openpbr_parity.py
                "$<TARGET_FILE:lusdrender>" ${PROJECT_SOURCE_DIR})
      set_tests_properties(tool-lusdrender-materialx-openpbr-parity PROPERTIES
                           SKIP_RETURN_CODE 77
                           TIMEOUT 180
                           RESOURCE_LOCK lusdview_semantic_gpu
                           LABELS "lusdrender;gpu;rt;materialx;openpbr;parity")
    endif()
    set(LUSDR_SHELL_TESTS
        backplate
        backface-material
        camera-modes
        cli-robustness
        cmyk-jpeg-texture
        degraded-material
        dome-format
        double-sided-cull
        frames-framespec
        geomsubset-materials
        hermite-curves
        mixed-flat-curve-gpu
        ibl-specular-bound
        instancer-invisible-ids
        instancer-orientation
        light-double-count
        light-normalize
        light-shadow-enable
        material-shading-bsdf-sample
        mesh-light-gaps
        mtlx-usdz-texture-equivalence
        mtlx-usdz-udim-equivalence
        opacity-texture-alpha
        payload-mask
        purpose-filtering
        purpose-visibility
        shaped-area-lights
        sphere-light-nee
        subdivision-schemes
        textureless-dome
        timecode-blendshape-animation
        timecode-normals-animation
        timecode-points-animation
        timecode-transform-animation
        transparency
        usdrender-settings
        usdz-texture-equivalence
        uv-set-routing
        gaussian-carrier-dispatch
        gaussian-schema-aliases
        vk-ray-tiling)
    foreach(t ${LUSDR_SHELL_TESTS})
      add_test(
        NAME tool-lusdrender-${t}
        COMMAND ${LUSDR_BASH}
                ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-${t}.sh
                "$<TARGET_FILE:lusdrender>"
      )
      set_tests_properties(tool-lusdrender-${t} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()

    # run-material-resolver.sh takes the binary via $LUSDRENDER (its positional
    # args are options) and skips itself when USD_ASSETS_ROOT is unset, so it is
    # safe to register: it runs on machines that have the corpus, skips on those
    # that don't. README.md already told people to run it by this ctest name.
    add_test(
      NAME tool-lusdrender-material-resolver
      COMMAND ${LUSDR_BASH} ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-material-resolver.sh
    )
    set_tests_properties(tool-lusdrender-material-resolver
                         PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "LUSDRENDER=$<TARGET_FILE:lusdrender>")

    add_test(
      NAME tool-lusdrender-alpha-blend-sort-external
      COMMAND ${LUSDR_BASH}
              ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-alpha-blend-sort-external.sh
              "$<TARGET_FILE:lusdrender>"
    )
    set_tests_properties(tool-lusdrender-alpha-blend-sort-external
                         PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  #
  # USDA parser tester
  #
  if(LIGHTUSD_WITH_MODULE_USDA_READER)
    add_test(
      NAME usda-parser-unit-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usda/unit-runner.py --app
              "$<TARGET_FILE:test_lightusd>" --basedir "${PROJECT_SOURCE_DIR}/tests/usda"
      )
  endif()

  #
  # AOUSD validation fixture suite (self-checking via `# EXPECT:` markers).
  # Runs `lusdcat --validate-all` over tests/usda/validation/**.
  #
  # These are end-to-end tests that spawn the `lusdcat` binary. On 32-bit
  # (i386) + AddressSanitizer, the i386 ASan runtime/allocator cannot reserve
  # enough virtual address space for the full `lusdcat` process and it aborts
  # before useful work (`ERROR: Failed to mmap`, mmap -> ENOMEM; often while
  # reporting an internal allocator OOM). This reproduces with both gcc's
  # libasan and clang's compiler-rt, so it is a 32-bit ASan address-space
  # limitation for this binary, not a toolchain bug. The validation logic
  # itself is correct on 32-bit (verified with a non-ASan i386 build: full suite
  # passes) and is exercised by every 64-bit job, and ASan coverage of the core
  # library is still provided by unit-test-lightusd (which runs fine under
  # 32-bit ASan). So skip just these lusdcat-driven E2E tests in the 32-bit+ASan
  # configuration.
  set(_lightusd_lusdcat_e2e_ok TRUE)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND SANITIZE_ADDRESS)
    set(_lightusd_lusdcat_e2e_ok FALSE)
    message(STATUS "lightusd: skipping lusdcat E2E validation tests on 32-bit + ASan (i386 ASan runtime cannot reserve enough address space for lusdcat)")
  endif()

  if(TARGET lusdcat AND _lightusd_lusdcat_e2e_ok)
    # Crate-writer round-trip pins. Every check here is a fix for authored USD
    # data that the .usdc writer silently DROPPED or INVENTED: schema fallbacks
    # written for declaration-only attributes, prim/attribute metadata blocks,
    # timeSamples on typed writers, list-edit qualifiers, and imageable fields
    # such as Scope visibility/purpose. Several are latent -- no tests/usda
    # fixture exercises them -- so this suite prevents silent regressions.
    add_test(
      NAME scope-imageable-roundtrip
      COMMAND bash "${PROJECT_SOURCE_DIR}/tests/run-scope-imageable-roundtrip.sh"
              "$<TARGET_FILE:lusdcat>"
      )
    set_tests_properties(scope-imageable-roundtrip PROPERTIES SKIP_RETURN_CODE 77)

    add_test(
      NAME validation-suite-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usda/validation/run-validation-suite.py
              --app "$<TARGET_FILE:lusdcat>"
              --basedir "${PROJECT_SOURCE_DIR}/tests/usda/validation"
      )

    # Corpus-wide robustness regression: validate every parseable fixture in
    # tests/usda and fail only if the validator crashes or hangs.
    add_test(
      NAME validation-regression-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usda/validation/run-validation-regression.py
              --app "$<TARGET_FILE:lusdcat>"
              --basedir "${PROJECT_SOURCE_DIR}/tests/usda"
      )

    add_test(
      NAME validation-usdz-cube-test
      COMMAND "$<TARGET_FILE:lusdcat>" --validate
              "${PROJECT_SOURCE_DIR}/models/cube.usdz"
      )

    add_test(
      NAME validation-usdz-texture-cat-plane-test
      COMMAND "$<TARGET_FILE:lusdcat>" --validate
              "${PROJECT_SOURCE_DIR}/models/texture-cat-plane.usdz"
      )

    find_program(LIGHTUSD_NODE_EXECUTABLE node)
    if(LIGHTUSD_NODE_EXECUTABLE
       AND EXISTS "${PROJECT_SOURCE_DIR}/web/js/src/lightusd/lightusd.js"
       AND EXISTS "${PROJECT_SOURCE_DIR}/web/js/src/lightusd/lightusd.wasm")
      add_test(
        NAME web-validation-parity-test
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDCAT_PATH=$<TARGET_FILE:lusdcat>"
                "LIGHTUSD_WASM_DIR=${PROJECT_SOURCE_DIR}/web/js/src/lightusd"
                "LIGHTUSD_PARITY_SKIP_STALE_WASM=1"
                "${LIGHTUSD_NODE_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/web/js/tests/validation-parity.mjs"
        )
    endif()
  endif()

  # Windowless Vulkan render regression. This works on both hardware and
  # software Vulkan devices and self-skips when no Vulkan device is available.
  if(UNIX AND NOT APPLE AND TARGET lusdview)
    # Opt-in external Island production smoke. With no ISLAND_USD environment
    # variable this returns 77 immediately, proving the corpus remains optional
    # in ordinary CI while making the real-data gate discoverable via CTest.
    add_test(
      NAME lusdview-island-production-smoke
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-island-production-smoke.sh"
      )
    set_tests_properties(lusdview-island-production-smoke PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 5400
                         LABELS "lusdview;external;island")

    add_test(
      NAME lusdview-progressive-checkpoints
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-progressive-checkpoints.sh"
      )
    set_tests_properties(lusdview-progressive-checkpoints PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 150 LABELS "lusdview")

    add_test(
      NAME lusdview-vk-multitlas
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-multitlas.sh"
      )
    set_tests_properties(lusdview-vk-multitlas PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "lusdview")

    add_test(
      NAME lusdview-ptex-camera-demand
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_PTEX_GENERATOR=$<TARGET_FILE:lusdview_ptex_atlas_test>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-ptex-camera-demand.sh"
      )
    set_tests_properties(lusdview-ptex-camera-demand PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 90 LABELS "lusdview")

    add_test(
      NAME lusdview-vk-render
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-render.sh"
      )
    set_tests_properties(lusdview-vk-render PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300)

    add_test(
      NAME lusdview-vk-stacked-glass
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "ASSET=${PROJECT_SOURCE_DIR}/models/lusdview-stacked-glass.usda"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-render.sh"
    )
    set_tests_properties(lusdview-vk-stacked-glass PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300
                         LABELS "lusdview;vulkan;transparency")

    add_test(
      NAME lusdview-vk-swbvh-render
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-swbvh-render.sh"
      )
    set_tests_properties(lusdview-vk-swbvh-render PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;vulkan;rt")

    add_test(
      NAME lusdview-vk-swbvh-stacked-glass
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "ASSET=${PROJECT_SOURCE_DIR}/models/lusdview-stacked-glass.usda"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-swbvh-render.sh"
    )
    set_tests_properties(lusdview-vk-swbvh-stacked-glass PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;vulkan;rt;transparency")

    add_test(
      NAME lusdview-vk-texture-validation
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-texture-validation.sh"
      )
    set_tests_properties(lusdview-vk-texture-validation PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 150
                         LABELS "lusdview;vulkan;validation")

    add_test(
      NAME lusdview-vk-mtlx-cold-fallback
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-mtlx-cold-fallback.sh"
    )
    set_tests_properties(lusdview-vk-mtlx-cold-fallback PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120
                         LABELS "lusdview;vulkan;rt;materialx")

    add_test(
      NAME lusdview-vk-authored-materials
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-authored-materials.sh"
      )
    set_tests_properties(lusdview-vk-authored-materials PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;vulkan;rt;materialx;authored-assets")

    add_test(
      NAME lusdview-raster-multilight
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-multilight.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-raster-multilight PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview"
                         ENVIRONMENT "LUSDVIEW_GL_TIMEOUT=120s"
                         RESOURCE_LOCK lusdview_gl_gpu)

    add_test(
      NAME lusdview-rt-light-links
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-rt-light-links.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-rt-light-links PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 150 LABELS "lusdview")

    add_test(
      NAME lusdview-raster-shadow-map
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-shadow-map.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-raster-shadow-map PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

    add_test(
      NAME lusdview-raster-carrier-shadow
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-raster-carrier-shadow.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-raster-carrier-shadow PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

    add_test(
      NAME lusdview-shadow-alpha-inst
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-shadow-alpha-inst.sh"
      )
    set_tests_properties(lusdview-shadow-alpha-inst PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 60 LABELS "lusdview")

    add_test(
      NAME lusdview-aousd-conformance
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-aousd-conformance.sh"
      )
    set_tests_properties(lusdview-aousd-conformance PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview")

    add_test(
      NAME lusdview-gl-vk-parity
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              # NVIDIA can need one additional cold shader-program compile for
              # a fixture variant not exercised by the global GL warm-up.
              "LUSDVIEW_GL_VK_TIMEOUT=120s"
              bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-vk-parity.sh"
      )
    set_tests_properties(lusdview-gl-vk-parity PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "lusdview")

    add_test(
      NAME lusdview-backend-parity
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_PARITY_FRAMES=1" "LUSDVIEW_PARITY_SIZE=96x96"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-backend-parity.sh"
      )
    set_tests_properties(lusdview-backend-parity PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview")

    add_test(
      NAME lusdview-unsupported-realtime-lobes
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-unsupported-lobes.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-unsupported-realtime-lobes PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 90 LABELS "lusdview")
  endif()

  #
  # lusdview CPU/GPU skinning screenshot parity. This is intentionally
  # registered only for Linux GUI builds with xvfb-run available: OpenGL needs a
  # window/context, and xvfb supplies a headless X server.
  #
  # It needs a HARDWARE GL device to mean anything. Xvfb (and a forwarded
  # DISPLAY) has no DRI, so Mesa falls back to llvmpipe, which fetches no vertex
  # attribute but aPosition -- the skin joints/weights read as zero and the
  # GPU-skinned render is stuck in the rest pose regardless of the code under
  # test. The script detects that device and exits 77 (skip); see
  # tests/lusdview/gpu_backend.py.
  #
  if(UNIX AND NOT APPLE AND TARGET lusdview)
    # HIP is loaded dynamically by lusdview.  CTest often runs with a minimal
    # loader environment, so discover the ROCm runtime directory at configure
    # time and propagate it to lusdview tests.  Preserve any caller-supplied
    # LD_LIBRARY_PATH after the discovered directory; this also supports
    # versioned installations such as /opt/rocm/core-7.14/lib.
    set(_lightusd_hip_library_dirs)
    foreach(_lightusd_rocm_root IN ITEMS
            "/opt/rocm" "$ENV{ROCM_PATH}" "$ENV{ROCM_HOME}")
      if(_lightusd_rocm_root AND IS_DIRECTORY "${_lightusd_rocm_root}")
        list(APPEND _lightusd_hip_library_dirs
             "${_lightusd_rocm_root}/lib")
        file(GLOB _lightusd_rocm_versioned_dirs LIST_DIRECTORIES true
             "${_lightusd_rocm_root}/core-*/lib")
        list(APPEND _lightusd_hip_library_dirs
             ${_lightusd_rocm_versioned_dirs})
      endif()
    endforeach()
    find_library(_lightusd_hip_runtime
                 NAMES amdhip64 libamdhip64.so
                 PATHS ${_lightusd_hip_library_dirs}
                 NO_DEFAULT_PATH)
    if(_lightusd_hip_runtime)
      get_filename_component(_lightusd_hip_runtime_dir
                             "${_lightusd_hip_runtime}" DIRECTORY)
      if(DEFINED ENV{LD_LIBRARY_PATH} AND
         NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
        set(_lightusd_hip_ld_library_path
            "${_lightusd_hip_runtime_dir}:$ENV{LD_LIBRARY_PATH}")
      else()
        set(_lightusd_hip_ld_library_path
            "${_lightusd_hip_runtime_dir}")
      endif()
      message(STATUS "lightusd: HIP CTest runtime path: ${_lightusd_hip_runtime_dir}")
    else()
      set(_lightusd_hip_ld_library_path)
      message(STATUS "lightusd: HIP runtime not found; HIP CTest cases will self-skip")
    endif()

    find_program(LIGHTUSD_XVFB_RUN xvfb-run)
    if(LIGHTUSD_XVFB_RUN)
      add_test(
        NAME lusdview-skinning-screenshot-diff
        COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/lusdview/compare-skinning-screenshots.py
                --app "$<TARGET_FILE:lusdview>"
                --model "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
                --out-dir "${CMAKE_BINARY_DIR}/Testing/lusdview-skinning"
                --backend gl
                --frames 4
                --time 12
                --xvfb-run "${LIGHTUSD_XVFB_RUN}"
        )
      set_tests_properties(lusdview-skinning-screenshot-diff
                           PROPERTIES SKIP_RETURN_CODE 77)

      # The HIP interactive tracer refits its BVH per pose instead of paying a
      # full scene rebuild. Refit must trace byte-identically to the rebuild
      # path (LUSDVIEW_NO_BVH_REFIT=1) over the same --play run. Self-skips
      # without a HIP device.
      add_test(
        NAME lusdview-hip-bvh-refit
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-hip-bvh-refit.py
                "${LIGHTUSD_XVFB_RUN}"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-hip-bvh-refit"
        )
      set_tests_properties(lusdview-hip-bvh-refit
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
      # Skinning must also reach INSTANCED prototypes (a shared prototype behind
      # `instanceable = true`), which the next loader used to convert but never
      # skin -- every instance rendered the rest pose. Asserts the rig actually
      # animates inside the instance; CPU/GPU are not pixel-compared here (a
      # GPU-skinned prototype is de-instanced onto the material shader).
      add_test(
        NAME lusdview-instanced-prototype-skinning
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-instanced-prototype-skinning.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/skintest-instanced.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-instanced-skinning"
        )
      # It exits 77 when no usable GL display is present (it renders WINDOWED, to
      # exercise the real context). Without this the skip is reported as a failure.
      set_tests_properties(lusdview-instanced-prototype-skinning
                           PROPERTIES SKIP_RETURN_CODE 77)

      # The large-scene budget tree must descend from the real device, not the
      # GiB(32)/GiB(16) literals it used to be fed, and --vram-budget must
      # override the probe. Headless + tiny scene; skips without Vulkan.
      add_test(
        NAME lusdview-vram-budget
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-vram-budget.py
                "$<TARGET_FILE:lusdview>"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-vram-budget"
        )
      set_tests_properties(lusdview-vram-budget PROPERTIES SKIP_RETURN_CODE 77)

      # --texture-fit must actually decide whether textures are shrunk and
      # block-compressed. That decision previously had no coverage at all: a
      # regression either way showed up only as a slow or needlessly degraded
      # large-scene load. Headless + tiny scene; skips without Vulkan.
      add_test(
        NAME lusdview-texture-fit
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-texture-fit.py
                "$<TARGET_FILE:lusdview>"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-texture-fit"
        )
      set_tests_properties(lusdview-texture-fit PROPERTIES SKIP_RETURN_CODE 77)

      # A non-instanced blendshape mesh must animate on the default --next loader.
      # Morph used to be built only for instanced prototypes, so a plain
      # SkelRoot -> Mesh blendshape rendered frozen at its rest pose. Headless
      # (Vulkan); skips without a device.
      add_test(
        NAME lusdview-noninstanced-blendshape
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-noninstanced-blendshape.py
                "$<TARGET_FILE:lusdview>"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-noninstanced-blendshape"
        )
      set_tests_properties(lusdview-noninstanced-blendshape
                           PROPERTIES SKIP_RETURN_CODE 77)

      # Per-vertex displayColor must render on the Vulkan raster backend (it used
      # to reach only the RT path; VK raster drew the flat material color).
      add_test(
        NAME lusdview-vertex-color
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-vertex-color.py
                "$<TARGET_FILE:lusdview>"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-vertex-color"
        )
      set_tests_properties(lusdview-vertex-color PROPERTIES SKIP_RETURN_CODE 77)

      # Vulkan RT BLAS compaction: must shrink the acceleration structures, must
      # build ONE BLAS per prototype (not per instance), and must be image-neutral.
      add_test(
        NAME lusdview-blas-compaction
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-blas-compaction.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/blastest-instanced-static.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-blas-compaction"
        )
      set_tests_properties(lusdview-blas-compaction PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 300)

      # A texture must sample the UV set its UsdPrimvarReader names. The secondary
      # set used to be hard-coded as <primary>+"1" (so `uvSet1` was never
      # extracted), and nothing routed it even when it existed.
      add_test(
        NAME lusdview-uv-set-routing
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-uv-set-routing.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/multi-uv-quad.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-uv-set-routing"
        )
      set_tests_properties(lusdview-uv-set-routing PROPERTIES SKIP_RETURN_CODE 77)

      # Cross-tool parity oracle: ONE scene through BOTH binaries. lusdview and
      # lusdrender consume the same tydra-next RenderScene, so they must read
      # GeomSubset material bindings and the secondary UV set the same way.
      # Nothing else in the suite compares the two -- which is precisely how a run
      # of lusdview-only material/UV regressions stayed invisible while every
      # per-tool test kept passing. Needs a real Vulkan device (lusdview
      # --headless); skips without one.
      if(TARGET lusdrender)
        add_test(
          NAME lusdview-lusdrender-parity
          COMMAND bash
                  ${PROJECT_SOURCE_DIR}/tools/lusdrender/tests/run-cross-tool-parity.sh
          )
        set_tests_properties(lusdview-lusdrender-parity PROPERTIES
          SKIP_RETURN_CODE 77
          ENVIRONMENT
            "LUSDVIEW=$<TARGET_FILE:lusdview>;LUSDRENDER=$<TARGET_FILE:lusdrender>")
      endif()

      # Raster LOD must reach NON-instanced meshes, which also requires each one
      # to carry its own AABB (the next loader used to hand every static mesh the
      # scene-spanning box, disabling the per-mesh frustum cull along with it).
      add_test(
        NAME lusdview-noninstanced-lod
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-noninst-lod.py
                "$<TARGET_FILE:lusdview>"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-noninstanced-lod"
        )
      set_tests_properties(lusdview-noninstanced-lod PROPERTIES SKIP_RETURN_CODE 77)

      # The ray tracer traces vertex buffers, so the raster vertex shader's
      # deform never reaches it. --next --rt used to give up and re-run the whole
      # converter per time code; it now re-poses the retained rest vertices.
      add_test(
        NAME lusdview-rt-skinning
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-rt-skinning.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-rt-skinning"
        )
      set_tests_properties(lusdview-rt-skinning PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 600)

      # A skinned mesh's BLAS is refit in place per pose (MODE_UPDATE) instead
      # of destroyed + fully rebuilt. Refit must trace byte-identically to the
      # rebuild path (LUSDVIEW_NO_BLAS_REFIT=1) over the same --play run.
      add_test(
        NAME lusdview-rt-blas-refit
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-rt-blas-refit.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/skintest-animated.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-rt-blas-refit"
        )
      set_tests_properties(lusdview-rt-blas-refit PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 420)

      # A non-instanced blendshaped mesh must morph: the static batch path used to
      # build no morph channels at all, so it rendered its rest shape forever.
      add_test(
        NAME lusdview-blendshape-morph
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-blendshape-morph.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/blendshape-and-animation-test-001.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-blendshape-morph"
        )
      set_tests_properties(lusdview-blendshape-morph PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 600)

      # GPU deform vs CPU bake, in depth through a fixed camera, over the three
      # deform paths that had no fixture at all. Each of these caught a live bug:
      #   skin-xform     -- skinning under a rotated/scaled parent (the bone rows
      #                     must undo the static path's world bake and re-apply it);
      #   morph-subsets  -- a morphed mesh torn into two batches by material-bind
      #                     GeomSubsets, which re-index the sparse delta lists;
      #   morph-skin     -- both deforms live at once, which pins the ORDER (morph
      #                     the bind pose, then skin the morphed points);
      #   instanced-proto -- the same deform reached through NATIVE INSTANCING,
      #                     which is a different emitter (EmitInstancedProto) whose
      #                     vertices stay prototype-LOCAL. It was called exempt from
      #                     the world-bake bugs by construction, and it was -- but
      #                     the scene box then grew from those local vertices without
      #                     the instance placement, so the grid and the depth ramp
      #                     landed in the wrong place.
      #   pointinstancer -- the same prototype placed by a PointInstancer, whose
      #                     placements come from positions/orientations/scales arrays
      #                     rather than from prims referencing it. It caught the
      #                     orientation quaternion being read imaginary-first when the
      #                     next stage stores it real-first: a 30-degree Z rotation
      #                     decoded as a 150-degree X rotation, i.e. upside-down.
      foreach(deform_case skin-xform skin-animation morph-subsets morph-skin instanced-proto
                          pointinstancer)
        add_test(
          NAME lusdview-deform-${deform_case}
          COMMAND ${LUSDR_PYTHON}
                  ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
                  "$<TARGET_FILE:lusdview>"
                  "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-${deform_case}.usda"
                  "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-${deform_case}"
          )
        set_tests_properties(lusdview-deform-${deform_case}
                             PROPERTIES SKIP_RETURN_CODE 77)
      endforeach()
      set_tests_properties(lusdview-deform-instanced-proto
                           PROPERTIES LABELS "lusdview")

      # An instanced prototype under the RAY TRACER: RT cannot use the raster vertex
      # shader's deform (the BLAS is built from vertex buffers, so the geometry itself
      # has to move), and for a prototype those re-posed vertices are prototype-LOCAL
      # -- one BLAS, re-posed once, referenced by every placement's TLAS instance.
      # Raster and RT therefore reach the same pixels by genuinely different routes.
      add_test(
        NAME lusdview-deform-instanced-proto-rt
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-instanced-proto.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-instanced-proto-rt"
                rt
        )
      set_tests_properties(lusdview-deform-instanced-proto-rt
                           PROPERTIES SKIP_RETURN_CODE 77 LABELS "lusdview")

      # The CUDA tracer builds its BVH from draw_ geometry, so it takes the deform
      # through poseNextDrawForTracer instead of the vertex shader. It used to be
      # pinned to the load-time CPU bake -- a second deform implementation, free to
      # disagree with the shader's, and it did. Skips (77) where CUDA is absent.
      add_test(
        NAME lusdview-deform-cuda
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-deform-parity.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-morph-skin.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-deform-cuda"
                cuda
        )
      set_tests_properties(lusdview-deform-cuda PROPERTIES SKIP_RETURN_CODE 77)

      add_test(
        NAME lusdview-cuda-kernel-cache
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cuda-kernel-cache.sh"
        )
      set_tests_properties(lusdview-cuda-kernel-cache
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 210)

      add_test(
        NAME lusdview-cuda-cache-cli
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cuda-cache-cli.sh"
        )

      # LEGACY loader: an animated --time load must pose the mesh ONCE. It bakes the
      # pose into the geometry at load, and the rest-pose load that keeps the shader
      # from posing it a second time was gated on an EXPLICIT --skinning gpu -- so
      # the DEFAULT (auto) deformed twice.
      add_test(
        NAME lusdview-legacy-double-deform
        COMMAND ${LUSDR_PYTHON}
                ${PROJECT_SOURCE_DIR}/tests/lusdview/check-legacy-double-deform.py
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/deform-skin-xform.usda"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-legacy-double-deform"
        )
      set_tests_properties(lusdview-legacy-double-deform
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 420)
    else()
      message(STATUS "lightusd: skipping lusdview screenshot diff test; xvfb-run not found")
    endif()
  endif()

  #
  # UsdShade material-binding resolution: purpose-scoped (material:binding:preview
  # / :full) bindings authored on an ANCESTOR, as production scenes (ALab) bind.
  # Reading only the Mesh's own `material:binding` drops the material and every
  # texture with it. Renders headless (Vulkan offscreen); SKIPs without a GPU.
  #
  if(UNIX AND NOT APPLE AND TARGET lusdview)
    add_test(
      NAME lusdview-material-binding-inheritance
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-material-binding-inheritance.sh"
      )
    set_tests_properties(lusdview-material-binding-inheritance
                         PROPERTIES SKIP_RETURN_CODE 77)

    # Unsupported bound surface shaders retain familiar PBR constants in a
    # per-material degraded PreviewSurface. Geometry must remain renderable and
    # the structured load summary must report the degradation.
    add_test(
      NAME lusdview-degraded-material
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-degraded-material.sh"
      )
    set_tests_properties(lusdview-degraded-material
                         PROPERTIES SKIP_RETURN_CODE 77)

    # Deterministic screenshot controls must reject malformed/ambiguous camera
    # input before renderer initialization, so this remains a headless test.
    add_test(
      NAME lusdview-cli-camera-options
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-camera-options.sh"
              "$<TARGET_FILE:lusdview>"
      )

    add_test(
      NAME lusdview-cli-path-trace-options
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-path-trace-options.sh"
              "$<TARGET_FILE:lusdview>"
      )
    set_tests_properties(lusdview-cli-path-trace-options PROPERTIES
                         LABELS "lusdview;rt;path-trace")

    add_test(
      NAME lusdview-cli-window-size
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cli-window-size.sh"
              "$<TARGET_FILE:lusdview>"
      )
    set_tests_properties(lusdview-cli-window-size
                         PROPERTIES SKIP_RETURN_CODE 77 LABELS "lusdview")

    add_test(
      NAME lusdview-camera-record-equivalence
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-record-equivalence.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
      set_tests_properties(lusdview-camera-record-equivalence
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-camera-dof-cuda
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-dof-cuda.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-camera-dof-cuda
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-camera-dof-vulkan
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-camera-dof-vulkan.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-camera-dof-vulkan
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-area-light-cuda
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-area-light-cuda.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-area-light-cuda
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-area-light-vulkan
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-area-light-vulkan.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(lusdview-area-light-vulkan
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                                    LABELS "lusdview")

    add_test(
      NAME lusdview-next-nonmesh-extraction
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-next-nonmesh-extraction.sh"
              "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
    set_tests_properties(lusdview-next-nonmesh-extraction
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75
                                    LABELS "lusdview")

    if(UNIX AND NOT APPLE)
      add_test(
        NAME lusdview-gl-nonmesh-render
        COMMAND bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-nonmesh-render.sh"
                "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
      set_tests_properties(lusdview-gl-nonmesh-render
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75
                                      LABELS "lusdview")

      add_test(
        NAME lusdview-vk-nonmesh-render
        COMMAND bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-nonmesh-render.sh"
                "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
      set_tests_properties(lusdview-vk-nonmesh-render
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
                                      LABELS "lusdview")

      add_test(
        NAME lusdview-vk-gaussian-chunks
        COMMAND bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-gaussian-chunks.sh"
                "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
      set_tests_properties(lusdview-vk-gaussian-chunks
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 105
                                      LABELS "lusdview")

      add_test(
        NAME lusdview-nonmesh-backend-parity
        COMMAND bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-nonmesh-backend-parity.sh"
                "$<TARGET_FILE:lusdview>" "${PROJECT_SOURCE_DIR}"
      )
      set_tests_properties(lusdview-nonmesh-backend-parity
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150
                                      LABELS "lusdview")
    endif()

    if(LUSDR_PYTHON)
      # Start Vulkan once, then replace the USD scene and capture three cases
      # through MCP. Process-wide backend/loader variants remain separate tests.
      add_test(
        NAME lusdview-mcp-render-batch
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-render-batch.json"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch"
        )
      set_tests_properties(lusdview-mcp-render-batch
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      add_test(
        NAME lusdview-mcp-material-batch
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-material-batch.json"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-material-batch"
        )
      set_tests_properties(lusdview-mcp-material-batch
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      # The grid/no-grid image assertion also pins the RT compute-to-overlay
      # composition path without restarting Vulkan or rebuilding the TLAS.
      add_test(
        NAME lusdview-mcp-render-batch-vk-rt
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-vk-rt-overlay.json"
                "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch-vk-rt"
                --rt
        )
      set_tests_properties(lusdview-mcp-render-batch-vk-rt
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
      add_test(
        NAME lusdview-mcp-live-reload-vulkan
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
                "$<TARGET_FILE:lusdview>" vulkan
                "${PROJECT_SOURCE_DIR}/examples/lusdview/vk/shaders/raytrace.comp"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
                --materialx-vk-shader-max-kib 256
        )
      set_tests_properties(lusdview-mcp-live-reload-vulkan
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360
                                      LABELS "lusdview;rt;mcp;live-reload;vulkan")
      add_test(
        NAME lusdview-mcp-live-reload-vulkan-swbvh
        COMMAND ${CMAKE_COMMAND} -E env LUSDVIEW_RT_FORCE_SW=1
                ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
                "$<TARGET_FILE:lusdview>" vulkan
                "${PROJECT_SOURCE_DIR}/examples/lusdview/vk/shaders/raytrace_swbvh.comp"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
                --async-smoke
        )
      set_tests_properties(lusdview-mcp-live-reload-vulkan-swbvh
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240
                                      LABELS "lusdview;rt;mcp;live-reload;vulkan;swbvh")
      add_test(
        NAME lusdview-mcp-live-reload-cuda
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
                "$<TARGET_FILE:lusdview>" cuda
                "${PROJECT_SOURCE_DIR}/examples/lusdview/raytracer_kernel_src.txt"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
        )
      set_tests_properties(lusdview-mcp-live-reload-cuda
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 480
                                      LABELS "lusdview;rt;mcp;live-reload;cuda")
      add_test(
        NAME lusdview-mcp-live-reload-hip
        COMMAND ${LUSDR_PYTHON}
                "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_shader_reload.py"
                "$<TARGET_FILE:lusdview>" hip
                "${PROJECT_SOURCE_DIR}/examples/lusdview/raytracer_kernel_src.txt"
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/area-light-shadow.usda"
        )
      set_tests_properties(lusdview-mcp-live-reload-hip
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360
                                      LABELS "lusdview;rt;mcp;live-reload;hip")
      find_program(LUSDVIEW_XVFB_RUN NAMES xvfb-run)
      if(LUSDVIEW_XVFB_RUN)
        # Same manifest through one real, persistent GLFW/OpenGL window. The
        # trailing backend overrides the manifest's Vulkan default.
        add_test(
          NAME lusdview-mcp-render-batch-gl-window
          COMMAND "${LUSDVIEW_XVFB_RUN}" -a
                  ${LUSDR_PYTHON}
                  "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                  "$<TARGET_FILE:lusdview>"
                  "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-render-batch.json"
                  "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-render-batch-gl"
                  --windowed --backend gl --size 1024x768
          )
        set_tests_properties(lusdview-mcp-render-batch-gl-window
                             PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_test(
          NAME lusdview-mcp-material-batch-gl-window
          COMMAND "${LUSDVIEW_XVFB_RUN}" -a
                  ${LUSDR_PYTHON}
                  "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_render_batch.py"
                  "$<TARGET_FILE:lusdview>"
                  "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/mcp-material-batch.json"
                  "${CMAKE_BINARY_DIR}/Testing/lusdview-mcp-material-batch-gl"
                  --windowed --backend gl --size 1024x768
          )
        set_tests_properties(lusdview-mcp-material-batch-gl-window
                             PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      endif()
    endif()

    # Exact material:binding:back resolution and draw-side selection, including
    # a GeomSubset-specific degraded back material and whole-mesh fallback.
    add_test(
      NAME lusdview-backface-material
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-backface-material.sh"
      )
    set_tests_properties(lusdview-backface-material
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                                    ENVIRONMENT "LUSDVIEW_XVFB=1")

    # doubleSided (audit T8): a single-sided mesh back-face culls, a doubleSided
    # one does not -- identically on GL and Vulkan. The --next loader used to
    # drop the authored flag entirely, and Vulkan never culled anything.
    add_test(
      NAME lusdview-double-sided
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-double-sided.sh"
      )
    set_tests_properties(lusdview-double-sided PROPERTIES SKIP_RETURN_CODE 77)

    # UsdPreviewSurface specular workflow + IOR on --next (audit T12): a colored
    # specularColor must tint the highlight (not fall back to white dielectric),
    # identically on GL and VK.
    add_test(
      NAME lusdview-specular-workflow
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-specular-workflow.sh"
      )
    set_tests_properties(lusdview-specular-workflow PROPERTIES SKIP_RETURN_CODE 77)

    # T12 opacity completion: separate scalar opacity textures (ordinary/UDIM)
    # on both raster backends, and varying displayOpacity on raster + RT.
    add_test(
      NAME lusdview-opacity-material
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-opacity-material.sh"
      )
    set_tests_properties(lusdview-opacity-material PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 900)

    # Texture-driven material factors must survive the complete raster / RT
    # packing chain. Split the backend matrix so each CTest case reports useful
    # progress and a slow backend cannot hide the others behind one silent
    # aggregate test.
    set(_lightusd_semantic_backends gl vk-raster vk-rt cuda hip)
    foreach(_backend IN LISTS _lightusd_semantic_backends)
      set(_test_name "lusdview-texture-semantic-aov-${_backend}")
      set(_semantic_timeout_env)
      if(_backend STREQUAL "gl")
        # A fixture can select an additional NVIDIA shader variant that was
        # not covered by the global warm-up. Under concurrent suite load its
        # first compile can exceed the normal 60 s launch guard.
        set(_semantic_timeout_env "LUSDVIEW_RENDER_TIMEOUT=180s")
      elseif(_backend STREQUAL "vk-rt")
        # A new compact RT SPIR-V can take roughly 150 s for NVIDIA's first
        # driver JIT.  Let that first process populate lusdview's persistent
        # VkPipelineCache; warm launches are typically below 200 ms.
        set(_semantic_timeout_env "LUSDVIEW_RENDER_TIMEOUT=240s")
      endif()
      add_test(
        NAME "${_test_name}"
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                "LUSDVIEW_SEMANTIC_BACKENDS=${_backend}"
                # The loader-parity shards below own UDIM/USDZ and both-loader
                # coverage. Keep the main shards focused on every OpenPBR AOV;
                # this removes duplicate process/device startup and bounds each
                # hardware backend to roughly 13 viewer launches.
                "LUSDVIEW_SEMANTIC_FAMILIES=openpbr"
                "LUSDVIEW_SEMANTIC_UDIM=0"
                "LUSDVIEW_SEMANTIC_PACKAGE_PARITY=0"
                ${_semantic_timeout_env}
                bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
        )
      # 300 s was enough only while a failed backend probe let most cases be
      # skipped; a full GL run over every mode and family measures ~436 s.
      set_tests_properties("${_test_name}" PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "lusdview"
                           RESOURCE_LOCK lusdview_semantic_gpu)
    endforeach()

    # Explicitly exercise the GL normal-map TBN fallback with constant UVs.
    # Keep this as a small GL-only shard so the full CUDA/HIP semantic matrices
    # do not repeat a regression case that targets the raster shader branch.
    add_test(
      NAME lusdview-texture-semantic-aov-gl-degenerate
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_SEMANTIC_BACKENDS=gl"
              "LUSDVIEW_SEMANTIC_MODES=degenerate-normal degenerate-coat-normal"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
    )
    set_tests_properties(lusdview-texture-semantic-aov-gl-degenerate PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview"
                         RESOURCE_LOCK lusdview_semantic_gpu)

    # Keep the RT texture-table path loader-neutral without repeating the full
    # semantic matrix above. One textured OpenPBR albedo case still exercises
    # external, UDIM, and USDZ resolution through both loaders and compares the
    # resulting images, while reducing this shard from ~162 viewer launches to
    # six.
    set(_lightusd_semantic_rt_backends vk-rt cuda hip)
    foreach(_backend IN LISTS _lightusd_semantic_rt_backends)
      set(_test_name "lusdview-texture-semantic-rt-loader-parity-${_backend}")
      set(_semantic_rt_timeout_env)
      if(_backend STREQUAL "vk-rt")
        set(_semantic_rt_timeout_env "LUSDVIEW_RENDER_TIMEOUT=240s")
      elseif(_backend STREQUAL "cuda")
        # CUDA initialization can stall a child viewer after a preceding CUDA
        # shard has released its context. Keep one stalled render from
        # consuming the entire aggregate parity test timeout; the matrix has
        # passed with this guard and reports a normal backend failure/skip.
        set(_semantic_rt_timeout_env "LUSDVIEW_RENDER_TIMEOUT=20s")
      endif()
      add_test(
        NAME "${_test_name}"
        COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_SEMANTIC_BACKENDS=${_backend}"
              "LUSDVIEW_SEMANTIC_LOADERS=default legacy"
              "LUSDVIEW_SEMANTIC_FAMILIES=openpbr"
              "LUSDVIEW_SEMANTIC_MODES=albedo"
              ${_semantic_rt_timeout_env}
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-texture-semantic-aov.sh"
      )
      set_tests_properties("${_test_name}" PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "lusdview"
                           RESOURCE_LOCK lusdview_semantic_gpu)
    endforeach()

    add_test(
      NAME lusdview-light-record-equivalence
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-light-record-equivalence.sh"
      )
    set_tests_properties(lusdview-light-record-equivalence PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_test(
      NAME lusdview-instancer-invisible-ids
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-instancer-invisible-ids.sh"
      )
    set_tests_properties(lusdview-instancer-invisible-ids PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_test(
      NAME lusdview-tydra-pointinstancer
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-tydra-pointinstancer.sh"
      )
    set_tests_properties(lusdview-tydra-pointinstancer PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_test(
      NAME lusdview-inst-morph-cull
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-inst-morph-cull.sh"
      )
    set_tests_properties(lusdview-inst-morph-cull PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    # DomeLight IBL baking is provided by the optional vendored textools
    # dependency. Do not register a pixel-regression that can never exercise
    # its feature when that dependency is intentionally disabled.
    if(LIGHTUSD_WITH_TEXTOOLS)
      add_test(
        NAME lusdview-dome-orientation
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-dome-orientation.sh"
        )
      set_tests_properties(lusdview-dome-orientation PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 240)
    endif()

    add_test(
      NAME lusdview-displacement-udim
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-displacement-udim.sh"
      )
    set_tests_properties(lusdview-displacement-udim PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 600)

    add_test(
      NAME lusdview-vk-displacement-isolation
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-displacement-isolation.sh"
      )
    set_tests_properties(lusdview-vk-displacement-isolation PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    # A single mesh may bind different materials through materialBind
    # GeomSubsets. All RT backends must resolve the committed triangle's
    # submesh material, not the mesh's first material.
    add_test(
      NAME lusdview-rt-geomsubset-material
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-rt-geomsubset-material.sh"
      )
    set_tests_properties(lusdview-rt-geomsubset-material
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

    # Deferred package payloads must survive the async recomposition handoff in
    # both loaders. Drive the actual MCP action so the test covers the same
    # lifetime boundary as the interactive "Load payload" UI.
    add_test(
      NAME lusdview-usdz-deferred-payload
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usdz-deferred-payload.sh"
      )
    set_tests_properties(lusdview-usdz-deferred-payload
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)

    # Deterministic back-to-front alpha blending on both raster backends.
    # Keep this registered separately from scalar opacity/cutout coverage: it
    # catches draw-order regressions with three overlapping translucent layers.
    add_test(
      NAME lusdview-transparency
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-transparency.sh"
      )
    set_tests_properties(lusdview-transparency PROPERTIES SKIP_RETURN_CODE 77)

    # A fresh cache must not make the default auto transparency mode pay for
    # weighted OIT. The script reports elapsed time but intentionally has no
    # timing threshold, so it is safe across drivers and software Vulkan.
    add_test(
      NAME lusdview-vulkan-cold-start-auto
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-cold-start-auto.sh"
      )
    set_tests_properties(lusdview-vulkan-cold-start-auto
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                                    LABELS "lusdview;vulkan;startup;transparency")

    add_test(NAME lusdview-vulkan-oit-promotion
      COMMAND ${LUSDR_PYTHON} "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_oit_promotion.py"
              "$<TARGET_FILE:lusdview>"
              "${PROJECT_SOURCE_DIR}/models/lusdview-transparency.usda" --scene-changes)
    set_tests_properties(lusdview-vulkan-oit-promotion PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview;vulkan;transparency")
    if(LUSDVIEW_ENABLE_GL_THREAD)
      add_test(NAME lusdview-vulkan-oit-promotion-threaded
        COMMAND ${LUSDR_PYTHON} "${PROJECT_SOURCE_DIR}/tests/lusdview/mcp_oit_promotion.py"
                "$<TARGET_FILE:lusdview>"
                "${PROJECT_SOURCE_DIR}/models/lusdview-transparency.usda" --threaded)
      set_tests_properties(lusdview-vulkan-oit-promotion-threaded PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "lusdview;vulkan;transparency")
    endif()

    # Optional Khronos ValidationLayers smoke for the weighted-OIT command path.
    # Local developers can build the layer with
    # scripts/setup-vulkan-validation-linux.sh. CI may provide a distro package
    # or prebuilt VK_LAYER_PATH; otherwise this test deliberately skips (77)
    # rather than downloading/building ValidationLayers during the test job.
    add_test(
      NAME lusdview-vk-oit-validation
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_ROOT=${PROJECT_SOURCE_DIR}"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-vk-oit-validation.sh"
      )
    set_tests_properties(lusdview-vk-oit-validation PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 150)

    # The CPU tracer must own headless screenshots, including its bounded
    # stacked-alpha compositor, rather than silently capturing Vulkan raster.
    add_test(
      NAME lusdview-cpu-rt-headless
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
      )
    set_tests_properties(lusdview-cpu-rt-headless PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180)

    add_test(
      NAME lusdview-cpu-rt-stacked-glass
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "ASSET=${PROJECT_SOURCE_DIR}/models/lusdview-stacked-glass.usda"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
    )
    set_tests_properties(lusdview-cpu-rt-stacked-glass PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;rt;transparency")

    add_test(
      NAME lusdview-cpu-rt-openpbr-lobes
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "ASSET=${PROJECT_SOURCE_DIR}/tests/usda/lusdview-unsupported-realtime-lobes.usda"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-headless.sh"
    )
    set_tests_properties(lusdview-cpu-rt-openpbr-lobes PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;rt;openpbr")

    add_test(
      NAME lusdview-cpu-rt-materialx-swizzle
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-cpu-rt-materialx-swizzle.sh"
    )
    set_tests_properties(lusdview-cpu-rt-materialx-swizzle PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 180
                         LABELS "lusdview;rt;materialx;openpbr")

    add_test(
      NAME lusdview-openchess-materials
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-openchess-materials.sh"
      )
    set_tests_properties(lusdview-openchess-materials PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 240
                         LABELS "lusdview;rt;materialx;openpbr")

    add_test(
      NAME lusdview-openchess-path-trace
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-openchess-path-trace.sh"
      )
    set_tests_properties(lusdview-openchess-path-trace PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 3600
                         LABELS "lusdview;rt;path-trace;materialx;openpbr;gpu")

    # USD `proxy` and `render` are ALTERNATIVES, not two things to draw. Drawing
    # both put the stand-in on top of the geometry it stands in for (intent-vfx's
    # simpleAsset: a proxy Cube of size 2 exactly encloses the render Sphere of
    # radius 1). Both loaders must supersede the proxy -- and still draw it when
    # there is no render geometry to supersede it.
    add_test(
      NAME lusdview-purpose-proxy-supersede
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-purpose-proxy-supersede.sh"
      )
    set_tests_properties(lusdview-purpose-proxy-supersede
                         PROPERTIES SKIP_RETURN_CODE 77)

    # Layer-relative asset-path anchoring: a look layer nested below the root
    # reaches its texture with `../tex/...`, which only resolves when anchored to
    # the layer that authored it (see src/next/layer/asset-anchor.hh).
    add_test(
      NAME lusdview-asset-path-anchoring
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-material-binding-inheritance.sh"
                   "${PROJECT_SOURCE_DIR}/models/nested-look-texture/root.usda"
      )
    set_tests_properties(lusdview-asset-path-anchoring
                         PROPERTIES SKIP_RETURN_CODE 77)

    # usd-wg/assets regression baseline. The batch harness could always RENDER the
    # corpus; what it had no answer for was "did this change what any of it looks
    # like?" -- so a render that silently turned a scene black still passed. The
    # checked-in baseline (examples/lusdview/tests/usd-assets-goldens.tsv) records a
    # COVERAGE fingerprint per (mode, asset): a silhouette bitfield, chosen over a
    # pixel hash because it survives the GPU/driver lighting differences that would
    # otherwise make the file useless anywhere but the machine that wrote it. A
    # geometry, composition, or material-binding regression moves the silhouette; a
    # shading nudge does not.
    #
    # Still a per-machine artifact in the end, so it is OPT-IN twice over: it needs
    # the corpus (USD_ASSETS_ROOT, else SKIP) and LUSDVIEW_RUN_GOLDEN=1 (else SKIP).
    # Refresh it deliberately, and read the diff before committing it:
    #
    #   LUSDVIEW_RUN_GOLDEN=1 tests/lusdview/run-usd-assets-batch.sh \
    #     --golden-kind coverage \
    #     --update-golden examples/lusdview/tests/usd-assets-goldens.tsv
    add_test(
      NAME lusdview-usd-assets-golden
      COMMAND ${CMAKE_COMMAND} -E env
              "LUSDVIEW=$<TARGET_FILE:lusdview>"
              "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_GOLDEN"
              "LUSDVIEW_USD_ASSETS_FAIL_ON=timeout,backend_error,golden_mismatch,unexpected_degradation,expectation_mismatch"
              bash "${PROJECT_SOURCE_DIR}/tests/lusdview/run-usd-assets-batch.sh"
                   --root "${LIGHTUSD_USD_ASSETS_ROOT}"
                   --golden-kind coverage
                   --golden "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/usd-assets-goldens.tsv"
      )
    set_tests_properties(lusdview-usd-assets-golden
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 5400)

    # Self-contained classification/expectation test for the external-corpus
    # harness. Uses a generated empty layer and lusdrender CPU, so it verifies
    # no_renderable plus hard expectation mismatches without usd-assets data.
    if(TARGET lusdrender)
      add_test(
        NAME lusdview-usd-assets-classification
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                "LUSDRENDER=$<TARGET_FILE:lusdrender>"
                bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-no-renderable-tracking.sh"
        )
      set_tests_properties(lusdview-usd-assets-classification
                           PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

      # Bounded but broad cross-renderer coverage for the external usd-wg/assets
      # checkout. This is separately opt-in because it exercises 40 scenes over
      # four backends and is intentionally much heavier than the self-contained
      # profile/classification regressions below.
      add_test(
        NAME lusdview-usd-assets-broad-smoke
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                "LUSDRENDER=$<TARGET_FILE:lusdrender>"
                "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_USD_ASSETS_BROAD"
                "LUSDVIEW_USD_ASSETS_ALLOW_PARENT=1"
                bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-render-smoke.sh"
                     --root "${LIGHTUSD_USD_ASSETS_ROOT}"
                     --profile usd-assets-broad
                     --require-profile-complete
                     --modes vk-raster,vk-rt,lusdr-cpu,lusdr-vk
                     --time 1
        )
      set_tests_properties(lusdview-usd-assets-broad-smoke PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 7200
                           LABELS "lusdview;external;usd-assets")

      add_test(
        NAME lusdview-usd-assets-animation-smoke
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                "LUSDRENDER=$<TARGET_FILE:lusdrender>"
                "LUSDVIEW_USD_ASSETS_GATE=LUSDVIEW_RUN_USD_ASSETS_ANIMATION"
                "LUSDVIEW_USD_ASSETS_ALLOW_PARENT=1"
                "USD_ASSETS_ROOT=${LIGHTUSD_USD_ASSETS_ROOT}"
                bash "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-animation-smoke.sh"
        )
      set_tests_properties(lusdview-usd-assets-animation-smoke PROPERTIES
                           SKIP_RETURN_CODE 77 TIMEOUT 3600
                           LABELS "lusdview;external;usd-assets;animation")
    endif()

    add_test(
      NAME lusdview-usd-assets-broad-profile
      COMMAND bash
              "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-usd-assets-broad-profile.sh"
      )
    set_tests_properties(lusdview-usd-assets-broad-profile PROPERTIES TIMEOUT 120)
  endif()

  #
  # Guard that src/usd-preview-surface-inputs.inc (the validator's table) stays
  # in sync with src/usd-preview-surface-schema.json. Fails if a schema edit was
  # not followed by `python3 scripts/gen-usd-preview-surface-inputs.py`.
  #
  add_test(
    NAME preview-surface-schema-sync
    COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen-usd-preview-surface-inputs.py --check
    )

  # Offline pin/integrity gate for the generated OpenUSD compatibility
  # baseline. Refreshing the file requires an OpenUSD checkout, but normal CI
  # deliberately has no runtime or network dependency on OpenUSD.
  add_test(
    NAME openusd-schema-manifest
    COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/check-openusd-schema-manifest.py
            ${PROJECT_SOURCE_DIR}/doc/generated/openusd-schema-26.08.json
    )

  add_test(
    NAME openusd-capability-ledger
    COMMAND ${LUSDR_PYTHON}
            ${PROJECT_SOURCE_DIR}/scripts/generate-lightusd-capability-ledger.py
            --check
            --manifest ${PROJECT_SOURCE_DIR}/doc/generated/openusd-schema-26.08.json
            --output ${PROJECT_SOURCE_DIR}/doc/generated/lightusd-capabilities-openusd-26.08.json
    )

  add_test(
    NAME openusd-supported-schema-fixture
    COMMAND ${LUSDR_PYTHON}
            ${PROJECT_SOURCE_DIR}/scripts/generate-openusd-supported-schema-fixture.py
            --check
            --ledger ${PROJECT_SOURCE_DIR}/doc/generated/lightusd-capabilities-openusd-26.08.json
            --output ${PROJECT_SOURCE_DIR}/tests/usda/generated/openusd-supported-schema-26.08.usda
            --registry-output ${PROJECT_SOURCE_DIR}/src/next/schema/generated/openusd-supported-schema-names.inc
    )

  #
  # USDA roundtrip tester
  #
  if(LIGHTUSD_WITH_MODULE_USDA_READER)
    set(LIGHTUSD_ROUNDTRIP_TARGET "usda_roundtrip")
    add_executable(${LIGHTUSD_ROUNDTRIP_TARGET} ${PROJECT_SOURCE_DIR}/tests/usda-roundtrip.cc)
    add_sanitizers(${LIGHTUSD_ROUNDTRIP_TARGET})
    target_include_directories(${LIGHTUSD_ROUNDTRIP_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${LIGHTUSD_ROUNDTRIP_TARGET} PRIVATE ${LIGHTUSD_TARGET_STATIC})
    if(LIGHTUSD_WITH_JSON)
      target_compile_definitions(${LIGHTUSD_ROUNDTRIP_TARGET} PRIVATE "LIGHTUSD_WITH_JSON")
    endif()

    add_test(
      NAME usda-roundtrip-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usda/roundtrip-runner.py --app
              "$<TARGET_FILE:usda_roundtrip>" --basedir "${PROJECT_SOURCE_DIR}/tests/usda"
      )
  endif()

  #
  # USDC roundtrip tester (USDA -> USDC -> reparse -> compare)
  #
  if(LIGHTUSD_WITH_MODULE_USDA_READER AND LIGHTUSD_WITH_MODULE_USDC_WRITER AND LIGHTUSD_WITH_MODULE_USDC_READER)
    set(LIGHTUSD_USDC_ROUNDTRIP_TARGET "usdc_roundtrip")
    add_executable(${LIGHTUSD_USDC_ROUNDTRIP_TARGET} ${PROJECT_SOURCE_DIR}/tests/usdc-roundtrip.cc)
    add_sanitizers(${LIGHTUSD_USDC_ROUNDTRIP_TARGET})
    target_include_directories(${LIGHTUSD_USDC_ROUNDTRIP_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${LIGHTUSD_USDC_ROUNDTRIP_TARGET} PRIVATE ${LIGHTUSD_TARGET_STATIC})
    if(LIGHTUSD_WITH_JSON)
      target_compile_definitions(${LIGHTUSD_USDC_ROUNDTRIP_TARGET} PRIVATE "LIGHTUSD_WITH_JSON")
    endif()

    add_test(
      NAME usdc-roundtrip-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usda/usdc-roundtrip-runner.py --app
              "$<TARGET_FILE:usdc_roundtrip>" --basedir "${PROJECT_SOURCE_DIR}/tests/usda"
      )

    # USDC writer roundtrip: lusdcat writes USDC -> read back as USDA -> lusddiff
    # Uses --report-only so it does not fail the suite (informational baseline).
    # Run without --report-only for strict validation:
    #   python3 tests/usdc-writer/usdc-writer-runner.py --lusdcat ./build/lusdcat --lusddiff ./build/lusddiff --basedir tests/usda --verbose
    # lusddiff now lives under tools/ (LIGHTUSD_BUILD_TOOLS); skip if not built.
    if(TARGET lusddiff)
      add_test(
        NAME usdc-writer-diff-test
        COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usdc-writer/usdc-writer-runner.py
                --lusdcat "$<TARGET_FILE:lusdcat>"
                --lusddiff "$<TARGET_FILE:lusddiff>"
                --basedir "${PROJECT_SOURCE_DIR}/tests/usda"
                --report-only
        )
    endif()
  endif()

  #
  # usd-wg asset corpus parse regression (native `lusdcat -l`).
  #
  # Runs the Node harness over the usd-wg/assets corpus (path from the
  # USD_WG_ASSETS_DIR env var, else its built-in default). The repository
  # verification harness supplies its prepared corpus explicitly. --max-fail
  # 0 gates on the load baseline.
  #
  find_program(LIGHTUSD_NODE_EXECUTABLE NAMES node nodejs)
  if(LIGHTUSD_NODE_EXECUTABLE AND TARGET lusdcat)
    add_test(
      NAME corpus-parse-native
      COMMAND ${LIGHTUSD_NODE_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/tests/parse-asset-corpus.mjs
              --optional-assets
              --mode load
              --lusdcat "$<TARGET_FILE:lusdcat>"
              --out "${CMAKE_BINARY_DIR}/corpus-parse-results"
              --max-fail 0
      )
    set_tests_properties(corpus-parse-native PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  #
  # USDA parser tester
  #
  if(LIGHTUSD_WITH_MODULE_USDC_READER)
    add_test(
      NAME usdc-parser-unit-test
      COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/usdc/unit-runner.py --app
              "$<TARGET_FILE:test_lightusd>" --basedir "${PROJECT_SOURCE_DIR}/tests/usdc"
      )
    # usdc-parser-unit-test globs all *.usdc in tests/usdc/, including
    # *-runtime.usdc files that are generated by unit-test-lightusd.
    # Must run after unit tests to avoid reading partially-written files.
    set_tests_properties(usdc-parser-unit-test PROPERTIES
      DEPENDS unit-test-lightusd)
  endif()

  #
  # Feature tests as CTest targets (Phase 0B)
  #
  if(LIGHTUSD_WITH_MODULE_USDA_READER)
    # Native counterpart to the WASM Lucia UV/LightRT parity fixtures. Keep
    # this independent of the main library so the vendored C BVH ABI and the
    # deterministic center-sample barycentric policy are tested directly.
    add_executable(feat-lightrt-uv
      tests/feat/lightrt-uv/test-lightrt-uv.cc
      ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_tri.c)
    add_sanitizers(feat-lightrt-uv)
    target_include_directories(feat-lightrt-uv PRIVATE
      ${PROJECT_SOURCE_DIR}/src/external/lightrt)
    if(UNIX AND NOT APPLE)
      find_package(Threads REQUIRED)
      target_link_libraries(feat-lightrt-uv PRIVATE Threads::Threads m)
    elseif(UNIX)
      target_link_libraries(feat-lightrt-uv PRIVATE m)
    endif()
    add_test(NAME feat-lightrt-uv COMMAND feat-lightrt-uv
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    add_executable(feat-value-clip tests/feat/value-clip/test-value-clip.cc)
    add_sanitizers(feat-value-clip)
    target_link_libraries(feat-value-clip PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-value-clip PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-value-clip COMMAND feat-value-clip
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # tydra value-level diff API (ULP tolerance, metadata reasons, value display)
    if(LIGHTUSD_WITH_TYDRA)
      add_executable(feat-diff tests/feat/diff/test-diff.cc)
      add_sanitizers(feat-diff)
      target_link_libraries(feat-diff PRIVATE ${LIGHTUSD_TARGET_STATIC})
      target_include_directories(feat-diff PRIVATE ${PROJECT_SOURCE_DIR}/src)
      add_test(NAME feat-diff COMMAND feat-diff
               WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    endif()

    # Composition: a local-only child PrimSpec must survive a reference/inherit/
    # specialize/variant flatten (regression for InheritPrimSpecImpl dropping
    # dst-only children).
    add_executable(feat-localchild tests/feat/composition-localchild/test-localchild.cc)
    add_sanitizers(feat-localchild)
    target_link_libraries(feat-localchild PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-localchild PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-localchild COMMAND feat-localchild
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Composition: a variant selection on the referencing prim must win over the
    # referenced asset's own default, even when the populated variantSet lives
    # several arcs deep (reference -> empty variant + payload -> reference ->
    # populated variantSet). Regression for Pixar Kitchen_set Chair.usd: the
    # referenced default selection clobbered the local one, and eager variant
    # resolution consumed the local selection against empty variant blocks.
    add_executable(feat-variant-payload-chain
                   tests/feat/variant-payload-chain/test-variant-payload-chain.cc)
    add_sanitizers(feat-variant-payload-chain)
    target_link_libraries(feat-variant-payload-chain PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-variant-payload-chain PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-variant-payload-chain COMMAND feat-variant-payload-chain
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # USDC reader must carry the authored specifier (def/over/class) into the
    # Layer/PrimSpec reconstruction; without it an `over` loaded from USDC was
    # promoted to `def` on flatten (Pixar Kitchen_set pCylinder over-in-variant).
    add_executable(feat-usdc-specifier
                   tests/feat/usdc-specifier/test-usdc-specifier.cc)
    add_sanitizers(feat-usdc-specifier)
    target_link_libraries(feat-usdc-specifier PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-usdc-specifier PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-usdc-specifier COMMAND feat-usdc-specifier
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # On --flatten a composed prim's `apiSchemas` list-op must bake to EXPLICIT
    # form (pxrUSD usdcat drops the `prepend`/`append` qualifier). Without it the
    # whole-scene diff vs usdcat on UE-exported assets showed ~2800 spurious
    # `meta:apiSchemas` modifications (every referenced mesh prepends
    # MaterialBindingAPI). Verifies FlattenAppliedSchemas() bakes + recurses, and
    # that a plain load (no flatten) preserves the authored `prepend`.
    add_executable(feat-flatten-apischemas
                   tests/feat/flatten-apischemas/test-flatten-apischemas.cc)
    add_sanitizers(feat-flatten-apischemas)
    target_link_libraries(feat-flatten-apischemas PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-flatten-apischemas PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-flatten-apischemas COMMAND feat-flatten-apischemas
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Variant composition must respect LIVRPS: a host's local `over` for a prim
    # that the selected variant DEFINES must (1) be promoted over->def with the
    # variant's typeName, and (2) keep its directly-authored local attribute
    # opinion winning over the variant's (Local > Variant). Regresses a
    # UE-exported variant-LOD case (variant geometry vanishing as `over`, and a
    # deep sourceAsset override flipping one material variant to another).
    add_executable(feat-variant-local-override
                   tests/feat/variant-local-override/test-variant-local-override.cc)
    add_sanitizers(feat-variant-local-override)
    target_link_libraries(feat-variant-local-override PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-variant-local-override PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-variant-local-override COMMAND feat-variant-local-override
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # OpenUSD inserts variant-selected children BEFORE local children; lightusd
    # used to append them. Regresses CompositeVariant prepend + primChildren sync.
    add_executable(feat-variant-child-order
                   tests/feat/variant-child-order/test-variant-child-order.cc)
    add_sanitizers(feat-variant-child-order)
    target_link_libraries(feat-variant-child-order PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-variant-child-order PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-variant-child-order COMMAND feat-variant-child-order
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Opt-in authored-order emission: a flattened crate must be able to reproduce
    # the authored prim-child order (from the crate `primChildren` field), like
    # usdcat, instead of the default lexicographical order. Verifies default
    # output stays lexicographical and pprint::SetPreserveAuthoredOrder(true)
    # yields authored order through a USDA->USDC->flatten round-trip.
    add_executable(feat-preserve-order
                   tests/feat/preserve-order/test-preserve-order.cc)
    add_sanitizers(feat-preserve-order)
    target_link_libraries(feat-preserve-order PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-preserve-order PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-preserve-order COMMAND feat-preserve-order
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Opt-in OpenUSD-compatible float notation (SetUSDFloatFormat): fixed for
    # decimal exponents in [-6,15), unpadded/sign-less exponent outside -- matches
    # usdcat. Default stays lightusd's shortest-&-robust notation.
    add_executable(feat-usd-float-format
                   tests/feat/usd-float-format/test-usd-float-format.cc)
    add_sanitizers(feat-usd-float-format)
    target_link_libraries(feat-usd-float-format PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-usd-float-format PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-usd-float-format COMMAND feat-usd-float-format
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Opt-in OpenUSD-compatible USDA text layout (SetUSDTextFormat): metadata
    # paren on the `def` line, apiSchemas first, blank line between siblings.
    add_executable(feat-usd-text-format
                   tests/feat/usd-text-format/test-usd-text-format.cc)
    add_sanitizers(feat-usd-text-format)
    target_link_libraries(feat-usd-text-format PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-usd-text-format PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-usd-text-format COMMAND feat-usd-text-format
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Opt-in asset-path normalization on flatten (io::NormalizePath +
    # SetNormalizeAssetPathOnFlatten): collapse `.`/`..` when anchoring a
    # referenced asset's relative path, matching usdcat.
    add_executable(feat-asset-path-normalize
                   tests/feat/asset-path-normalize/test-asset-path-normalize.cc)
    add_sanitizers(feat-asset-path-normalize)
    target_link_libraries(feat-asset-path-normalize PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-asset-path-normalize PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-asset-path-normalize COMMAND feat-asset-path-normalize
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Driver-level counterpart: run the binary flatten drivers (lusdcat, and
    # lusdzconvert -> src/usdz-convert.cc) end-to-end on the same fixtures so a
    # regression of the variant-deferral in an iterative flatten loop (not just
    # the library) is caught. The C++ test above drives the library with its own
    # deferred loop and would not notice a driver revert.
    #
    # Skipped on 32-bit AddressSanitizer builds: the flatten itself is correct
    # and tiny there (~7 MB peak RSS without ASan), but ASan's shadow + per-
    # allocation metadata overhead exhausts the limited 32-bit address space
    # during this run on CI ("AddressSanitizer: out of memory ... failed to mmap
    # InternalMmapVector"). Coverage is retained by the library-level
    # feat-variant-payload-chain test and the 64-bit driver run.
    if(SANITIZE_ADDRESS AND CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_skip_vchain_driver TRUE)
    else()
      set(_skip_vchain_driver FALSE)
    endif()
    if(UNIX AND TARGET lusdcat AND NOT _skip_vchain_driver)
      set(_vchain_lusdzconvert "")
      if(TARGET lusdzconvert)
        set(_vchain_lusdzconvert "$<TARGET_FILE:lusdzconvert>")
      endif()
      add_test(NAME feat-variant-chain-driver
               COMMAND bash ${PROJECT_SOURCE_DIR}/tests/run-variant-chain.sh
                       $<TARGET_FILE:lusdcat> ${PROJECT_SOURCE_DIR}
                       ${_vchain_lusdzconvert})
    endif()

    if(UNIX AND TARGET lusdzconvert AND TARGET lusdcat)
      add_test(NAME lusdzconvert-regression-driver
               COMMAND bash ${PROJECT_SOURCE_DIR}/tests/run-lusdzconvert-regression.sh
                       $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat>
                       ${PROJECT_SOURCE_DIR})
    endif()

    # PointInstancer instancing (compute API + Tydra expansion)
    add_executable(feat-instancing tests/feat/instancing/test-instancing.cc)
    add_sanitizers(feat-instancing)
    target_link_libraries(feat-instancing PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-instancing PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-instancing COMMAND feat-instancing
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    # Cross-directory cwp anchoring (composition) via LargeSceneLoader
    add_executable(feat-large-scene tests/feat/large-scene/test-large-scene.cc)
    add_sanitizers(feat-large-scene)
    target_link_libraries(feat-large-scene PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-large-scene PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-large-scene COMMAND feat-large-scene
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    if(LIGHTUSD_WITH_MODULE_USDC_WRITER AND LIGHTUSD_WITH_MODULE_USDC_READER)
      add_executable(feat-dedp tests/feat/dedp/test-dedp.cc)
      add_sanitizers(feat-dedp)
      target_link_libraries(feat-dedp PRIVATE ${LIGHTUSD_TARGET_STATIC})
      target_include_directories(feat-dedp PRIVATE ${PROJECT_SOURCE_DIR}/src)
      add_test(NAME feat-dedp COMMAND feat-dedp
               WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    endif()

    # MaterialX tests
    add_executable(feat-mtlx-parse tests/feat/mtlx/test_mtlx_parse.cc)
    add_sanitizers(feat-mtlx-parse)
    target_link_libraries(feat-mtlx-parse PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-mtlx-parse PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-mtlx-parse COMMAND feat-mtlx-parse
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    add_executable(feat-mtlx-import tests/feat/mtlx/test_mtlx_import.cc)
    add_sanitizers(feat-mtlx-import)
    target_link_libraries(feat-mtlx-import PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-mtlx-import PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-mtlx-import COMMAND feat-mtlx-import)

    add_executable(feat-mtlx-export tests/feat/mtlx/test_mtlx_export.cc)
    add_sanitizers(feat-mtlx-export)
    target_link_libraries(feat-mtlx-export PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-mtlx-export PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-mtlx-export COMMAND feat-mtlx-export)

    # Variant tests
    add_executable(feat-variant-converter tests/feat/nestedVariantSet/test_variant_converter.cpp)
    add_sanitizers(feat-variant-converter)
    target_link_libraries(feat-variant-converter PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-variant-converter PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-variant-converter COMMAND feat-variant-converter
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

    add_executable(feat-variant-applier tests/feat/nestedVariantSet/test_variant_applier.cpp)
    add_sanitizers(feat-variant-applier)
    target_link_libraries(feat-variant-applier PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-variant-applier PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-variant-applier COMMAND feat-variant-applier
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()

  if(LIGHTUSD_WITH_JSON)
    add_executable(feat-mtlx-grouped-params tests/feat/mtlx/test_grouped_params.cc)
    add_sanitizers(feat-mtlx-grouped-params)
    target_link_libraries(feat-mtlx-grouped-params PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-mtlx-grouped-params PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-mtlx-grouped-params COMMAND feat-mtlx-grouped-params)
  endif()

  #
  # Benchmarks as CTest targets (Phase 0C) - excluded from default run
  #
  if(LIGHTUSD_WITH_MODULE_USDA_READER)
    add_executable(bench-parse-opt tests/feat/parse-opt/test-parse-opt.cc)
    add_sanitizers(bench-parse-opt)
    target_link_libraries(bench-parse-opt PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(bench-parse-opt PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME bench-parse-opt COMMAND bench-parse-opt --quick)
    set_tests_properties(bench-parse-opt PROPERTIES LABELS "benchmark")

    # Render-conversion benchmark: Stage -> raster/RT renderable mesh.
    # Report-only (no timing gate): prints median conversion time plus a
    # deterministic scene checksum for serial-vs-parallel byte-identity A/B
    # checks. When the next-core backend is enabled, a combined binary also
    # measures the tydra::next pipeline on the same input.
    add_executable(bench-render-convert tests/feat/render-convert/perf-render-convert.cc)
    add_sanitizers(bench-render-convert)
    target_link_libraries(bench-render-convert PRIVATE ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(bench-render-convert PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME bench-render-convert COMMAND bench-render-convert --iters 1 --prims 64)
    set_tests_properties(bench-render-convert PROPERTIES LABELS "benchmark")

    if(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE AND TARGET lightusd_next AND TARGET tydra_next)
      add_executable(bench-render-convert-next tests/feat/render-convert/perf-render-convert.cc)
      add_sanitizers(bench-render-convert-next)
      target_link_libraries(bench-render-convert-next PRIVATE
        ${LIGHTUSD_TARGET_STATIC} lightusd_next tydra_next)
      target_include_directories(bench-render-convert-next PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/next)
      target_compile_definitions(bench-render-convert-next PRIVATE PERFRC_ENABLE_NEXT=1)
      add_test(NAME bench-render-convert-next COMMAND bench-render-convert-next --iters 1 --prims 64)
      set_tests_properties(bench-render-convert-next PROPERTIES LABELS "benchmark")
    endif()
  endif()

  #
  # tinysubdiv (tsd) feature tests
  #
  add_executable(feat-subdiv tests/feat/subdiv/test-subdiv.cc)
  add_sanitizers(feat-subdiv)
  target_link_libraries(feat-subdiv PRIVATE ${LIGHTUSD_TARGET_STATIC})
  target_include_directories(feat-subdiv PRIVATE ${PROJECT_SOURCE_DIR}/src)
  add_test(NAME feat-subdiv COMMAND feat-subdiv
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

  # Opt-in: verify tsd against an OpenSubdiv source checkout.
  # The required sdc/vtr/far sources are compiled directly from
  # ${OpenSubdiv_ROOT}; no pre-built OpenSubdiv needed.
  if(LIGHTUSD_TSD_VERIFY_WITH_OSD)
    if(NOT OpenSubdiv_ROOT)
      message(FATAL_ERROR
              "LIGHTUSD_TSD_VERIFY_WITH_OSD requires OpenSubdiv_ROOT to point "
              "to an OpenSubdiv source checkout.")
    endif()
    add_library(tsd_osd_ref STATIC
      ${OpenSubdiv_ROOT}/opensubdiv/sdc/typeTraits.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/sdc/crease.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/fvarLevel.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/fvarRefinement.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/level.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/quadRefinement.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/refinement.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/sparseSelector.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/vtr/triRefinement.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/far/error.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/far/topologyDescriptor.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/far/topologyRefiner.cpp
      ${OpenSubdiv_ROOT}/opensubdiv/far/topologyRefinerthe large reference scene.cpp)
    target_include_directories(tsd_osd_ref SYSTEM PUBLIC ${OpenSubdiv_ROOT})
    target_compile_features(tsd_osd_ref PUBLIC cxx_std_14)
    set_target_properties(tsd_osd_ref PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_sanitizers(tsd_osd_ref)

    add_executable(feat-subdiv-verify tests/feat/subdiv/subdiv-verify-osd.cc)
    add_sanitizers(feat-subdiv-verify)
    target_link_libraries(feat-subdiv-verify PRIVATE
      tsd_osd_ref ${LIGHTUSD_TARGET_STATIC})
    target_include_directories(feat-subdiv-verify PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME feat-subdiv-verify COMMAND feat-subdiv-verify
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(feat-subdiv-verify PROPERTIES LABELS "osd-verify")
  endif()

  # Xvfb normally gives GLFW a Mesa software context. On machines where the
  # NVIDIA device is visible, PRIME/GLVND can route that context to the real
  # GPU; apply the same selection to every lusdview CTest case so nested
  # xvfb-run invocations inherit it. Keep this opt-in because forcing the
  # NVIDIA ICD on a host without /dev/nvidia0 turns a useful capability skip
  # into a hard initialization failure.
  #
  # Kernel/GLVND presence alone is not enough for Vulkan: a container can
  # expose libGLX_nvidia and the NVIDIA layers while the Vulkan ICD is absent,
  # leaving only llvmpipe. Probe the physical-device list before exporting
  # LUSDVIEW_VK_DEVICE=nvidia; otherwise leave Vulkan on its normal automatic
  # hardware/software selection path.
  if(LIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD AND UNIX AND NOT APPLE AND TARGET lusdview)
    if(EXISTS "/proc/driver/nvidia/version" AND
       EXISTS "/dev/nvidia0" AND
       EXISTS "/usr/share/glvnd/egl_vendor.d/10_nvidia.json")
      set(_lightusd_nvidia_vulkan_visible FALSE)
      find_program(_lightusd_vulkaninfo NAMES vulkaninfo)
      if(_lightusd_vulkaninfo)
        execute_process(
          COMMAND "${CMAKE_COMMAND}" -E env
                  "__NV_PRIME_RENDER_OFFLOAD=1"
                  "__GLX_VENDOR_LIBRARY_NAME=nvidia"
                  "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
                  "${_lightusd_vulkaninfo}" --summary
          RESULT_VARIABLE _lightusd_vulkaninfo_rc
          OUTPUT_VARIABLE _lightusd_vulkaninfo_out
          ERROR_VARIABLE _lightusd_vulkaninfo_err
          TIMEOUT 10)
        if(_lightusd_vulkaninfo_rc EQUAL 0)
          string(CONCAT _lightusd_vulkaninfo_text
                 "${_lightusd_vulkaninfo_out}\n${_lightusd_vulkaninfo_err}")
          string(REGEX MATCH
                 "deviceName[ \\t]*=[ \\t]*[^\\n]*(NVIDIA|GeForce|RTX|Quadro|Tesla)"
                 _lightusd_nvidia_device_match
                 "${_lightusd_vulkaninfo_text}")
          if(_lightusd_nvidia_device_match)
            set(_lightusd_nvidia_vulkan_visible TRUE)
          endif()
        endif()
      endif()
      get_property(_lightusd_lusdview_tests DIRECTORY PROPERTY TESTS)
      set(_lightusd_nvidia_gl_cache
          "${CMAKE_BINARY_DIR}/lusdview-nvidia-gl-cache")
      file(MAKE_DIRECTORY "${_lightusd_nvidia_gl_cache}")
      add_test(
        NAME lusdview-nvidia-gl-warmup
        COMMAND ${CMAKE_COMMAND} -E env
                "LUSDVIEW=$<TARGET_FILE:lusdview>"
                bash
                "${PROJECT_SOURCE_DIR}/examples/lusdview/tests/run-gl-warmup.sh")
      set_tests_properties(
        lusdview-nvidia-gl-warmup
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "lusdview;opengl"
                   FIXTURES_SETUP lusdview_nvidia_gl_cache
                   RESOURCE_LOCK lusdview_gl_gpu)
      list(APPEND _lightusd_lusdview_tests lusdview-nvidia-gl-warmup)
      foreach(_lightusd_test IN LISTS _lightusd_lusdview_tests)
        # CPU RT uses Vulkan only to provide a windowless presentation shell;
        # it neither needs GL shader warmup nor an NVIDIA device. Keeping these
        # tests automatic lets them run on RADV/llvmpipe when PRIME state or a
        # configure-time Xvfb display later disappears.
        if(_lightusd_test MATCHES "^lusdview-" AND
           NOT _lightusd_test MATCHES "^lusdview-cpu-rt-")
          get_property(_lightusd_test_env TEST "${_lightusd_test}"
                       PROPERTY ENVIRONMENT)
          if(NOT _lightusd_test_env)
            set(_lightusd_test_env)
          endif()
          list(APPEND _lightusd_test_env
               "__NV_PRIME_RENDER_OFFLOAD=1"
               "__GLX_VENDOR_LIBRARY_NAME=nvidia"
               "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
               "__GL_SHADER_DISK_CACHE=1"
               "__GL_SHADER_DISK_CACHE_PATH=${_lightusd_nvidia_gl_cache}"
               "LUSDVIEW_NVIDIA_OFFLOAD=1"
               "LUSDVIEW_XVFB=1")
          if(_lightusd_nvidia_vulkan_visible)
            list(APPEND _lightusd_test_env "LUSDVIEW_VK_DEVICE=nvidia")
          endif()
          set_tests_properties("${_lightusd_test}"
                               PROPERTIES ENVIRONMENT "${_lightusd_test_env}")
          if(NOT _lightusd_test STREQUAL "lusdview-nvidia-gl-warmup")
            set_property(TEST "${_lightusd_test}" APPEND PROPERTY
                         FIXTURES_REQUIRED lusdview_nvidia_gl_cache)
          endif()
        endif()
      endforeach()
      if(_lightusd_nvidia_vulkan_visible)
        message(STATUS "lightusd: NVIDIA offload environment enabled for lusdview CTest cases (Vulkan NVIDIA device confirmed)")
      else()
        message(STATUS "lightusd: NVIDIA GL offload environment enabled; Vulkan NVIDIA device not visible, leaving Vulkan device selection automatic")
      endif()
    else()
      message(STATUS "lightusd: NVIDIA offload requested but NVIDIA device/GLVND is not visible; lusdview tests keep their normal environment")
    endif()
  endif()

  if(_lightusd_hip_ld_library_path)
    get_property(_lightusd_lusdview_tests DIRECTORY PROPERTY TESTS)
    foreach(_lightusd_test IN LISTS _lightusd_lusdview_tests)
      if(_lightusd_test MATCHES "^lusdview-")
        get_property(_lightusd_test_env TEST "${_lightusd_test}"
                     PROPERTY ENVIRONMENT)
        if(NOT _lightusd_test_env)
          set(_lightusd_test_env)
        endif()
        list(APPEND _lightusd_test_env
             "LD_LIBRARY_PATH=${_lightusd_hip_ld_library_path}")
        set_tests_properties("${_lightusd_test}"
                             PROPERTIES ENVIRONMENT "${_lightusd_test_env}")
      endif()
    endforeach()
  endif()

endif(LIGHTUSD_BUILD_TESTS)

# [VisualStudio]
if(WIN32)
  # Set ${LIGHTUSD_TARGET_STATIC} as a startup project for VS IDE
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ${LIGHTUSD_TARGET_STATIC})

  # For easier debugging in VS IDE(cmake 3.8.0 or later required) Set working
  # directory to ${LIGHTUSD_TARGET_STATIC} git repo root.
  if(CMAKE_VERSION VERSION_GREATER 3.8.0)
    set_target_properties(
      ${LIGHTUSD_TARGET_STATIC} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY
                                           "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()
