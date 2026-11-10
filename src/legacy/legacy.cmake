# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# Legacy implementation build (src/legacy), included from the root
# CMakeLists.txt when LIGHTUSD_BUILD_LEGACY=ON. It runs in the root directory
# scope, after the core (src/) has been configured, so every relative path
# below is relative to the repository root.
#
# Legacy is compiled but not tested: its libraries, tools, examples and test
# executables still build (CI keeps a build-only job), but no test is
# registered with CTest. add_test()/set_tests_properties() are turned into
# no-ops for everything configured from here on; the core registered its
# tests before this file is included.
message(STATUS "LightUSD: building the legacy implementation (compiled, not tested)")
function(add_test)
endfunction()
function(set_tests_properties)
endfunction()

# ---- Legacy product -------------------------------------------------------
# Legacy-only sources live in src/legacy/ (same relative layout they had under
# src/). Put it ahead of src/ so legacy `#include "stage.hh"` /
# "core/prim.hh" / "tydra/render-data.hh" resolve to the legacy copies while
# shared headers (core/logger.hh, image-loader.hh, ...) still come from src/.
include_directories(BEFORE ${PROJECT_SOURCE_DIR}/src/legacy)
# Legacy code lives in `lightusd::legacy`; this bridge keeps `lightusd::X`
# working for legacy consumers and connects namespaces shared with src/.
if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/FI${PROJECT_SOURCE_DIR}/src/legacy/legacy-namespace.hh>")
else()
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${PROJECT_SOURCE_DIR}/src/legacy/legacy-namespace.hh>")
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
# NOTE: Measured to NOT help (and slightly hurt codegen) on this codebase: the
# compile cost is dominated by template *instantiation* and -O3 codegen, not by
# header parsing, which is all a PCH can cache. Kept as an opt-in knob; default OFF.
option(LIGHTUSD_BUILD_SHARED_LIBS "Build as dll?" ${BUILD_SHARED_LIBS})
option(LIGHTUSD_LEGACY_ENABLE_THREAD "Legacy: enable optional std::thread parallel paths (default OFF; keep OFF for default WASM/browser builds)" OFF)
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
option(LIGHTUSD_BUILD_EXAMPLES
       "Build examples(but not all examples in `examples` folder are being built)"
       ${LIGHTUSD_DEFAULT_BUILD_EXAMPLES})
option(LIGHTUSD_BUILD_TOOLS "Build tools (lusddumpcrate, etc.)" OFF)
# Link a drop-in allocator into the CLI example tools (lusdcat,
# tydra_to_renderscene). The legacy flatten/compose path is allocation-heavy
# (~20% of a large-scene `lusdcat -f` is glibc malloc/free/consolidate);
# jemalloc cut representative large-scene runs by 14-17% at ~+4% RSS with
# byte-identical output. Tools only -- the library
# never forces an allocator (apps choose). Same ranking as src
# (LIGHTUSD_CORE_WITH_TCMALLOC): jemalloc > tcmalloc_minimal > mimalloc.
# NOTE: unlike src tools, no `malloc_conf` no-purge baking here -- the
# decay-purge stall does not show on this path (measured a wash, +5% RSS).
option(LIGHTUSD_WITH_TCMALLOC
       "Link a drop-in allocator (jemalloc/tcmalloc/mimalloc) into CLI example tools (faster flatten/compose on large scenes)" OFF)
# LIGHTUSD_BUILD_GUI_VIEWER is defined by the root CMakeLists.txt.
option(LIGHTUSD_BUILD_TEXTURE_GPU_BENCH
       "Build the standalone headless Vulkan/CUDA/HIP texture processing benchmark"
       OFF)
# LIGHTUSD_USD_ASSETS_ROOT / LIGHTUSD_LUSDVIEW_NVIDIA_OFFLOAD: root CMakeLists.txt.
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
option(LIGHTUSD_LEGACY_USE_CORE_PCP
       "Use stable next::pcp::Cache backend for LargeSceneLoader (links lightusd_core)."
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
# tests/legacy/pprint/, ... — every binary in the build tree gets the runtime
# automatically. Shared libraries need it too: liblightusd_c embeds the
# instrumented asset-import and render-support objects, and macOS rejects
# undefined __asan_* references when linking a dylib.
if(SANITIZE_ADDRESS)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_MODULE_LINKER_FLAGS
      "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
endif()

# [cifuzz]
option(LIGHTUSD_TEST_CIFUZZ "Run cifuzz test" OFF)

if (LIGHTUSD_TEST_CIFUZZ)
  find_package(cifuzz NO_SYSTEM_ENVIRONMENT_PATH)
  enable_fuzz_testing()
endif()

if (LIGHTUSD_LEGACY_ENABLE_THREAD)
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



if (LIGHTUSD_WITH_MCP_SERVER)
  set(LIGHTUSD_WITH_JSON On)
endif()

set(LIGHTUSD_SOURCES
    # Crate path utilities (path sorting/tree encoding for USDC writer)
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-path-utils/path_sort.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-path-utils/tree_encode.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/arg-parser.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/asset-resolution.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/lightusd.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/xform.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/performance.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/spline-binary.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-props.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-entry.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-basetype.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-basetype-inst.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/ascii-parser-timesamples-array.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/audio-loader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/base122.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-reader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-reader-reconstruct-1.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-reader-reconstruct-2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-reader-reconstruct-3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-reader-reconstruct-4.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-property.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim-parse.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim-reconstruct-1.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim-reconstruct-2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim-reconstruct-3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-prim-reconstruct-4.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-reader-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usda-writer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdc-writer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/composition.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/composition-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/composition-graph.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/large-scene-loader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/core/instance-key.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/core/path-expression.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/core/path-expression-eval.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/hash-util.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/chunk-reader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-reader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-reader-arrays.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-reader-values.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-reader-paths.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-reader-timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-format.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-writer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-writer-values.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-writer-inline.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/stage-converter.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-geom.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-physics.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-ar.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-media.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-shader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-light.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-skel.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/sconv-layer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/crate-dump.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/path-util.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-geom.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-geom2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-geom3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-vol.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-lightprim.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-shader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-shader2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-shader3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-shader4.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-physics.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-physics2.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-physics3.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-physics4.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-ar.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-reconstruct-media.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-composition.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-types.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-types-schema.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/core/prim-enums.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/enum-handlers.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/layer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/primvar.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/str-util.cc
    ${PROJECT_SOURCE_DIR}/src/unicode-xid-c.c
    ${PROJECT_SOURCE_DIR}/src/c/spline.c
    ${PROJECT_SOURCE_DIR}/src/core/logger.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usd-dump.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-pprint-dispatch.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-pprint-prim.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-types.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-types-typename.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-types-cast.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-types-matrix.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-types-lerp.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/color-space.cc
    ${PROJECT_SOURCE_DIR}/src/core/color-transform.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/color-space.hh
    ${PROJECT_SOURCE_DIR}/src/legacy/usd-validation.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/tiny-format.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/tiny-string.cc
    ${PROJECT_SOURCE_DIR}/src/io-util.cc
    ${PROJECT_SOURCE_DIR}/src/image-loader.cc
    ${PROJECT_SOURCE_DIR}/src/image-convert.cc
    ${PROJECT_SOURCE_DIR}/src/image-writer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/image-util.cc
    ${PROJECT_SOURCE_DIR}/src/imageio/png-stream.cc
    ${PROJECT_SOURCE_DIR}/src/imageproc/simd.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdz-convert.cc
    ${PROJECT_SOURCE_DIR}/src/udim-bake.cc
    ${PROJECT_SOURCE_DIR}/src/udim-mesh.cc
    ${PROJECT_SOURCE_DIR}/src/udim-layer.cc
    ${PROJECT_SOURCE_DIR}/src/udim-layer-geometry.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdz-udim-layer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdz-udim-bake.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdz-geometry-optimize.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdz-material-optimize.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/linear-algebra.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/value-eval-util.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-accessors.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-scalar-a.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-scalar-b.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-vec-a.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-vec-b.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-role-a.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdGeom-primvar-inst-role-b.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdSkel.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdShade.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdLux.cc
    # usdMtlX has less dependency(pugixml), so add it to core component
    ${PROJECT_SOURCE_DIR}/src/mtlx-xml-tokenizer.cc
    ${PROJECT_SOURCE_DIR}/src/mtlx-xml-parser.cc
    ${PROJECT_SOURCE_DIR}/src/core/mtlx-dom.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/mtlx-simple-parser.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdMtlx.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdMtlx-write.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdPhysics.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/mjcPhysics.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usdObj.cc
    ${PROJECT_SOURCE_DIR}/src/image-loader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-enum.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-meta.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-geom.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-shader.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-light.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-skel.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-physics.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-ar.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprint-media.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/pprinter.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/timesamples-pprint.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/timesamples.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/timesamples-eval.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/stage.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/stage.hh
    ${PROJECT_SOURCE_DIR}/src/legacy/uuid-gen.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/uuid-gen.hh
    ${PROJECT_SOURCE_DIR}/src/legacy/parser-timing.cc
    ${PROJECT_SOURCE_DIR}/src/sha256.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/typed-array.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/task-queue.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/task-queue.hh
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-pprint-parallel.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/prim-pprint-parallel.hh

    )

if (LIGHTUSD_WITH_PCP)
    list(APPEND LIGHTUSD_SOURCES
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/layer-registry.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/layer-registry.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/cache.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/cache.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/cache-impl.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/pcp/cache-parallel.cc
    )
endif()

if (LIGHTUSD_WITH_TYDRA)
    list(APPEND LIGHTUSD_SOURCES
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/facial.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/facial.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/prim-apply.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/prim-apply.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-xform-node.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-skeleton.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-physics.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-collection.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-primvar.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-terminal.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-blendshape.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-property.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-property-names.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-listprims-inst.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access-listshaders-inst.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-access.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-analysis.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/scene-analysis.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/color-management.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/color-management.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/attribute-eval.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/attribute-eval.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/attribute-eval-typed-all.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/attribute-eval-typed-inst-scalar.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/attribute-eval-typed-inst-array.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/command-and-history.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/obj-export.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/usd-export.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/shader-network.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/shader-network.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-state.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-xform-anim.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-stage-instances.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-memory.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-texture.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-optimize.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-dedup.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-deferred.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-hierarchy.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-analytic-basic.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-analytic-solid.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-mesh.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-mesh-tangent.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-material.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-material-assembly.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-material-ktx2.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-mesh-visitor.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-material-signature.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-material-mtlx.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-anim.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-anim-light.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-anim-skeleton.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-anim-property.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-anim-value-clip.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-instancer.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-internal.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-pprint.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-data-pprint.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/raytracing-data.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/raytracing-data.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/raytracing-scene-converter.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/raytracing-scene-converter.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/material-serializer.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/material-serializer.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/materialx-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/materialx-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/physics-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/physics-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/urdf-to-usd.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/urdf-to-usd.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/ar-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/ar-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/ik-solver.h
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/ik-solver.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/ik-solver.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/rb-math.h
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/rb-collision.h
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/rb-dynamics.h
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/rb-dynamics.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/rb-dynamics.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-scene-dump.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/render-scene-dump.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/bone-util.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/bone-util.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/layer-to-renderscene.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/layer-to-renderscene.hh
        ${PROJECT_SOURCE_DIR}/src/tydra/texture-util.cc
        ${PROJECT_SOURCE_DIR}/src/tydra/texture-util.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/common-utils.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/common-utils.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-support.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-support.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-converter.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-converter.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-applier.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/variant-applier.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools.cc
        ${PROJECT_SOURCE_DIR}/src/mcp/mcp-tool-schemas.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-scene.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-scene.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-query.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-query.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-composition.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-composition.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-validate.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-validate.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-usdz.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-usdz.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-diff.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-diff.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-resources.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-resources.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-server.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-server.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-context.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-js-bridge.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-js-bridge.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/value-to-json.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/value-to-json-native.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/value-to-json-native-decode.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/value-to-json.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/diff-and-compare.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/diff-and-compare.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/js-script.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/js-script.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/js-script-persistent.cc
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/threejs-exporter.hh
        ${PROJECT_SOURCE_DIR}/src/legacy/tydra/threejs-exporter.cc
        )

    if(LIGHTUSD_WITH_WAMR)
        list(APPEND LIGHTUSD_SOURCES
            ${PROJECT_SOURCE_DIR}/src/legacy/tydra/wasm-runtime.cc
            ${PROJECT_SOURCE_DIR}/src/legacy/tydra/wasm-runtime.hh
        )
    endif(LIGHTUSD_WITH_WAMR)
endif (LIGHTUSD_WITH_TYDRA)

# Variant extraction utilities handled via variant-support.hh/cc in Tydra section

if(LIGHTUSD_WITH_PXR_COMPAT_API)
  list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/legacy/pxr-compat.cc)
endif()

set(LIGHTUSD_C_API_SOURCES
    ${PROJECT_SOURCE_DIR}/src/legacy/c-lightusd.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/c-lightusd-helpers.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/c-lightusd-tydra.cc)

set(LIGHTUSD_DEP_SOURCES
    ${PROJECT_SOURCE_DIR}/src/legacy/integerCoding.cpp
    ${PROJECT_SOURCE_DIR}/src/legacy/lz4-compression.cc
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
    ${PROJECT_SOURCE_DIR}/src/legacy/image-util.cc         # stb_image_resize
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
    SOURCE ${PROJECT_SOURCE_DIR}/src/legacy/image-util.cc
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
    ${PROJECT_SOURCE_DIR}/src/core/minijson.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/json-to-usd.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/usd-to-json.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/json-writer.cc
    ${PROJECT_SOURCE_DIR}/src/legacy/json-util.cc)

  list(APPEND LIGHTUSD_DEP_SOURCES  ${PROJECT_SOURCE_DIR}/src/external/yyjson.c)
endif()

if(LIGHTUSD_WITH_USDFBX)
  list(APPEND LIGHTUSD_SOURCES
    ${PROJECT_SOURCE_DIR}/src/legacy/usdFbx.cc
  )
  list(APPEND LIGHTUSD_DEP_SOURCES
       ${PROJECT_SOURCE_DIR}/src/external/OpenFBX/src/ofbx.cpp)
endif()

if(LIGHTUSD_WITH_USDOBJ)
  # usdObj uses the shared C11 asset importer.
endif()

if(LIGHTUSD_WITH_USDVOX)
  list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/legacy/usdVox.cc)
endif()

if(LIGHTUSD_WITH_USDVOL)
  # usdVol.cc and tydra/vol build in lightusd_asset_import (shared with the
  # next tydra), with tinyvdb from lightusd_tinyvdb.
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

    file(GLOB TINYEXR_V3_SOURCES CONFIGURE_DEPENDS
         ${PROJECT_SOURCE_DIR}/src/external/tinyexr/src/*.c)

    set_source_files_properties(${TINYEXR_V3_SOURCES} PROPERTIES
      INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/external/tinyexr/include;${PROJECT_SOURCE_DIR}/src/external/tinyexr/src"
      COMPILE_DEFINITIONS "EXR_NO_JPH;EXR_NO_ZSTD")

    list(APPEND LIGHTUSD_DEP_SOURCES ${TINYEXR_V3_SOURCES}
      ${PROJECT_SOURCE_DIR}/src/imageio/exr-codec.c)

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
  list(APPEND LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/legacy/zstd-compression.cc)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-textools.cmake")
set(TEXTOOLS_DIR ${PROJECT_SOURCE_DIR}/src/external/textools)

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

# Share stage-neutral Ptex/subdivision objects with next and render consumers.
include("${PROJECT_SOURCE_DIR}/cmake/lightusd-render-support.cmake")
lightusd_add_render_support()
list(APPEND LIGHTUSD_SOURCES
  $<TARGET_OBJECTS:lightusd_ptex_objects>
  $<TARGET_OBJECTS:lightusd_subdiv_objects>
  ${PROJECT_SOURCE_DIR}/src/legacy/tsd/tsd-lightusd.cc)

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

  # C11 leaves share the strict warning policy. Mixed declarations are valid
  # in C11; disable only the pre-C99 compatibility diagnostic for C sources.
  set_property(SOURCE ${LIGHTUSD_SOURCES} ${LIGHTUSD_C_API_SOURCES}
    APPEND PROPERTY COMPILE_OPTIONS
    "$<$<COMPILE_LANGUAGE:C>:-Wno-declaration-after-statement>")

  # Suppress informational `#pragma message` output. miniz.c no longer needs
  # this, but src/legacy/prim-types.hh and some vendored deps (wamr, wuffs) still
  # emit it, and it is not worth a diagnostic per translation unit.
  #
  # The _LARGEFILE*_SOURCE feature-test macros that miniz.c must define before
  # its first include are handled per-source in cmake/lightusd-render-support.cmake,
  # so the vendored file itself needs no diagnostic pragma.
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

  # clang 23 (e.g. Emscripten 7) adds experimental -Wlifetime-safety-*
  # analyses under -Weverything (tens of thousands of [[clang::lifetimebound]]
  # suggestions on the legacy sources). The subgroups are independent (the
  # -Wlifetime-safety umbrella does not cover them all), so suppress each one
  # the compiler knows about.
  foreach(_lusd_lifetime_warn
      lifetime-safety
      lifetime-safety-intra-tu-suggestions
      lifetime-safety-intra-tu-constructor-suggestions
      lifetime-safety-cross-tu-suggestions
      lifetime-safety-cross-tu-constructor-suggestions
      lifetime-safety-cross-tu-misplaced-lifetimebound
      lifetime-safety-lifetimebound-violation
      lifetime-safety-experimental-field-origin-escape
      lifetime-safety-experimental-invalidation
      lifetime-safety-dangling-field)
    string(MAKE_C_IDENTIFIER "LIGHTUSD_HAS_W${_lusd_lifetime_warn}" _lusd_have_var)
    check_cxx_compiler_flag("-W${_lusd_lifetime_warn}" ${_lusd_have_var})
    if (${_lusd_have_var})
      set(LIGHTUSD_COMPILE_FLAGS "${LIGHTUSD_COMPILE_FLAGS} -Wno-${_lusd_lifetime_warn}")
    endif()
  endforeach()

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
      ${PROJECT_SOURCE_DIR}/src/legacy/usd-to-json.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-js-bridge.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-resources.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-server.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools.cc
      ${PROJECT_SOURCE_DIR}/src/mcp/mcp-tool-schemas.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-composition.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-query.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-scene.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-validate.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-diff.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/mcp-tools-usdz.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/urdf-to-usd.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/value-to-json.cc
      ${PROJECT_SOURCE_DIR}/src/legacy/tydra/js-script-persistent.cc)
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

# miniz (and the bundled zstd) build inside lightusd_tinyvdb, which links
# after the root objects: one copy serves the root library and tinyvdb, and
# single-pass static links resolve both (an archive linked before tinyvdb
# could not provide its codec symbols).
list(REMOVE_ITEM LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/miniz.c)
set(LIGHTUSD_TINYVDB_ZSTD OFF)
# When the core (configured first) already created lightusd_tinyvdb, that
# target was built without the bundled zstd, so legacy keeps zstd.c in its own
# library instead of handing it to tinyvdb.
if(LIGHTUSD_WITH_ZSTD_COMPRESSION AND NOT LIGHTUSD_USE_SYSTEM_ZSTD AND
   NOT TARGET lightusd_tinyvdb)
  list(REMOVE_ITEM LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/zstd.c)
  set(LIGHTUSD_TINYVDB_ZSTD ON)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/lightusd-asset-import.cmake")
if(TARGET lightusd_asset_import)
  # lightusd_asset_import compiles pugixml and is linked below; a second copy
  # in lightusd_static gives macOS ld duplicate pugi:: symbols.
  list(REMOVE_ITEM LIGHTUSD_DEP_SOURCES ${PROJECT_SOURCE_DIR}/src/external/pugixml.cpp)
endif()
list(APPEND LIGHTUSD_SOURCES ${PROJECT_SOURCE_DIR}/src/core/host-services.cc)
add_library(${LIGHTUSD_TARGET_STATIC} STATIC ${LIGHTUSD_SOURCES} ${LIGHTUSD_DEP_SOURCES} ${LIGHTUSD_EXT_SOURCES})
target_link_libraries(${LIGHTUSD_TARGET_STATIC} lightusd_asset_import)
# Shared pixel conversion uses the stage-independent C half-float helpers.
if(TARGET lightusd_core_c)
  target_link_libraries(${LIGHTUSD_TARGET_STATIC} lightusd_core_c)
else()
  target_sources(${LIGHTUSD_TARGET_STATIC} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/c/half.c)
endif()
if(NOT LIGHTUSD_NATIVE_IO)
  target_compile_definitions(${LIGHTUSD_TARGET_STATIC} PUBLIC LIGHTUSD_NO_NATIVE_IO=1 LIGHTUSD_NO_MMAP=1 LIGHTUSD_NO_NATIVE_RESOLVER_IO=1 LIGHTUSD_MMAP_SUPPORTED=0)
endif()
if(LIGHTUSD_NATIVE_HOST_SERVICES AND LIGHTUSD_NATIVE_IO AND (WIN32 OR UNIX) AND NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
  target_compile_definitions(${LIGHTUSD_TARGET_STATIC} PUBLIC LIGHTUSD_NATIVE_HOST_SERVICES=1)
  target_link_libraries(${LIGHTUSD_TARGET_STATIC} ${CMAKE_DL_LIBS})
  if(WIN32)
    target_link_libraries(${LIGHTUSD_TARGET_STATIC} psapi)
  endif()
endif()
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
  target_link_libraries(${LIGHTUSD_TARGET} lightusd_asset_import)
  if(TARGET lightusd_core_c)
    target_link_libraries(${LIGHTUSD_TARGET} lightusd_core_c)
  else()
    target_sources(${LIGHTUSD_TARGET} PRIVATE
      ${PROJECT_SOURCE_DIR}/src/c/half.c)
  endif()
  if(NOT LIGHTUSD_NATIVE_IO)
    target_compile_definitions(${LIGHTUSD_TARGET} PUBLIC LIGHTUSD_NO_NATIVE_IO=1 LIGHTUSD_NO_MMAP=1 LIGHTUSD_NO_NATIVE_RESOLVER_IO=1 LIGHTUSD_MMAP_SUPPORTED=0)
  endif()
  if(LIGHTUSD_NATIVE_HOST_SERVICES AND LIGHTUSD_NATIVE_IO AND (WIN32 OR UNIX) AND NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    target_compile_definitions(${LIGHTUSD_TARGET} PUBLIC LIGHTUSD_NATIVE_HOST_SERVICES=1)
    target_link_libraries(${LIGHTUSD_TARGET} ${CMAKE_DL_LIBS})
    if(WIN32)
      target_link_libraries(${LIGHTUSD_TARGET} psapi)
    endif()
  endif()
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
if(LIGHTUSD_LEGACY_USE_CORE_PCP AND NOT TARGET lightusd_core)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src lightusd_next_build)
endif()

#
# CMake's Object libary have some issue on MSVC build, so build STATIC and SHARED lib indepdendently.
#

#
# -- common target props
#
if(LIGHTUSD_WITH_TYDRA)
  include(${PROJECT_SOURCE_DIR}/cmake/lightusd-rb-solver.cmake)
endif()
foreach(LIGHTUSD_LIB_TARGET ${LIGHTUSD_LIBS})
  if(LIGHTUSD_WITH_TYDRA)
    target_link_libraries(${LIGHTUSD_LIB_TARGET} lightusd_rb_solver)
  endif()

  # Precompiled header to amortize parsing of large, stable headers across the
  # many C++ TUs. Guarded with COMPILE_LANGUAGE:CXX because the library also
  # contains C sources (zstd.c, lz4.c, yyjson.c, ...) which cannot use a C++ PCH.
  if(LIGHTUSD_LEGACY_USE_PCH)
    target_precompile_headers(${LIGHTUSD_LIB_TARGET} PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/src/legacy/lightusd-pch.hh>")
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

  if(LIGHTUSD_LEGACY_USE_CORE_PCP)
    target_compile_definitions(${LIGHTUSD_LIB_TARGET}
                               PUBLIC "LIGHTUSD_LEGACY_USE_CORE_PCP")
    target_link_libraries(${LIGHTUSD_LIB_TARGET} lightusd_core)
  endif()

  target_link_libraries(${LIGHTUSD_LIB_TARGET} ${LIGHTUSD_EXT_LIBRARIES}
                        ${CMAKE_DL_LIBS})

  if (LIGHTUSD_LEGACY_ENABLE_THREAD)
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

# usda_parser/usdc_parser are registered by the core example build.

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
  # The canonical lusdcat target is owned by the core build.
  # usd-texcomp is registered by the core Tydra build.
  if (LIGHTUSD_WITH_TYDRA)
    # tydra_api is registered by the core Tydra build.
    # tydra_to_renderscene is registered by the core example build.
    # variant-lister is a core example, registered by src/CMakeLists.txt.
    # variant-analyzer is registered by the core example build.
    # urdf-to-usd is a core example, registered by src/CMakeLists.txt.

    # examples/lusdview is a core app, added by the root CMakeLists.txt.

    # Independent of LIGHTUSD_BUILD_GUI_VIEWER on purpose: lusdquicklook shares
    # no code with lusdview and needs no window-system or GPU dependency.
    # examples/lusdquicklook is a core app, added by the root CMakeLists.txt.

    # mcp_server is registered against the core MCP library by src/CMakeLists.txt.

    # js-script is registered by the optional core QuickJS example build.
  endif ()

  # api_tutorial is registered by the root core example build.
  # examples/workflows is a core example, added by the root CMakeLists.txt.
  # file_format is registered by the root core example build.
  # asset_resolution is a core example, registered by src/CMakeLists.txt.
  # progressive_composition is a core example.
  # large-scene-load is registered against the core StageSession by src/CMakeLists.txt.
  #if (LIGHTUSD_WITH_JSON)
  # usd_to_json is registered by the core example build.
  #endif()
  # usd_to_gltf is registered by the core C API example build.
  # save_usda is a core example registered by the root project.
  # c_api_example is registered by the root core example build.
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
  # tools/lusddumpcrate is a core tool, added by the root CMakeLists.txt.
  # tools/lusdzconvert is a core tool, added by the root CMakeLists.txt.
  # tools/lusdchecker is a core tool, added by the root CMakeLists.txt.
  # tools/lusddeps is a core tool, added by the root CMakeLists.txt.
  # lusddiff uses the Tydra diff-and-compare API.
  if(LIGHTUSD_WITH_TYDRA)
    # tools/lusddiff is a core tool, added by the root CMakeLists.txt.
    # examples/lusdview (added earlier when LIGHTUSD_BUILD_GUI_VIEWER is also
    # ON) may have already pulled this in for its lightrt_c CPU RT backend.
    # tools/lusdrender is a core tool, added by the root CMakeLists.txt.
  endif()
endif()


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

  # Vendored tptex (Ptex) self-tests: unit tests of the reader, writer, half,
  # utils and filters (the upstream differential suite runs in tinyexr).
  # Self-contained; links its own sources, no textools gate needed.
  file(GLOB LIGHTUSD_TPTEX_ALL_SOURCES CONFIGURE_DEPENDS
       ${PROJECT_SOURCE_DIR}/src/external/textools/ptex/src/*.c)
  file(GLOB LIGHTUSD_TPTEX_TEST_SOURCES CONFIGURE_DEPENDS
       ${PROJECT_SOURCE_DIR}/src/external/textools/ptex/test/*.c)
  add_executable(textools_ptex_test ${LIGHTUSD_TPTEX_TEST_SOURCES}
                 ${LIGHTUSD_TPTEX_ALL_SOURCES} ${PROJECT_SOURCE_DIR}/src/external/miniz.c)
  target_include_directories(textools_ptex_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/external/textools/ptex/include
    ${PROJECT_SOURCE_DIR}/src/external/textools/ptex/src)
  # Set per target: when the core configured cmake/lightusd-render-support.cmake
  # first, the directory-scoped TPTEX_MINIZ_HEADER source property only exists
  # in the core's directory, not here.
  target_compile_definitions(textools_ptex_test PRIVATE _LARGEFILE64_SOURCE=1
                             "TPTEX_MINIZ_HEADER=\"external/miniz.h\"")
  # (cmake/lightusd-ptex.cmake sets TPTEX_MINIZ_HEADER="external/miniz.h" on these
  # sources, resolved from the src/ root.)
  target_include_directories(textools_ptex_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_features(textools_ptex_test PRIVATE c_std_11)
  if(NOT MSVC)
    target_compile_options(textools_ptex_test PRIVATE -ffp-contract=off -w)
    target_link_libraries(textools_ptex_test PRIVATE m)
  endif()
  add_test(NAME textools-ptex COMMAND textools_ptex_test)
  set_tests_properties(textools-ptex PROPERTIES LABELS "textools;ptex"
    ENVIRONMENT "TPTEX_TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")

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
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/legacy/unit)

  #
  # MCP tests (requires MCP server enabled)
  #
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/legacy/mcp)

  #
  # Compression tester
  # (its not involved with lightusd codes, and well tested now. so disabled. will be removed in future release)
  #
  # add_subdirectory(${PROJECT_SOURCE_DIR}/tests/legacy/decompress-int)

  #
  # Pretty-print benchmark
  #
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/legacy/pprint)


  if(TARGET lightusd_tinyvdb)
    add_executable(test-tet-float-range tests/volumetric/tet-float-range.c src/lz4/lz4.c)
    target_link_libraries(test-tet-float-range PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-tet-float-range COMMAND test-tet-float-range)
    set_tests_properties(volumetric-tet-float-range PROPERTIES LABELS "cpu;volume;security")
    if(LIGHTUSD_TINYVDB_GPU AND NOT EMSCRIPTEN)
      add_executable(profile-gpu-level-set tests/volumetric/profile-gpu-level-set.c src/lz4/lz4.c)
      target_link_libraries(profile-gpu-level-set PRIVATE lightusd_tinyvdb ${CMAKE_DL_LIBS})
    endif()
    add_executable(test-level-set-allocator tests/volumetric/test-level-set-allocator.c src/lz4/lz4.c)
    target_link_libraries(test-level-set-allocator PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-level-set-allocator COMMAND test-level-set-allocator)
    set_tests_properties(volumetric-level-set-allocator PROPERTIES LABELS "cpu;volume;security")
    add_executable(test-graded-stencil tests/volumetric/test-graded-stencil.c src/lz4/lz4.c)
    target_link_libraries(test-graded-stencil PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-graded-stencil COMMAND test-graded-stencil)
    set_tests_properties(volumetric-graded-stencil PROPERTIES LABELS "cpu;volume")
    add_executable(test-tet-center-sample tests/volumetric/tet-center-sample.c
      src/lz4/lz4.c)
    target_link_libraries(test-tet-center-sample PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-tet-center-sample COMMAND test-tet-center-sample)
    set_tests_properties(volumetric-tet-center-sample PROPERTIES LABELS "cpu;volume")
    add_executable(test-tet-worker-identity
      tests/volumetric/tet-worker-identity.c src/lz4/lz4.c)
    target_link_libraries(test-tet-worker-identity PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-tet-worker-identity COMMAND test-tet-worker-identity)
    set_tests_properties(volumetric-tet-worker-identity PROPERTIES LABELS "cpu;volume")
    add_executable(profile-tet tests/volumetric/profile-tet.c src/lz4/lz4.c)
    target_link_libraries(profile-tet PRIVATE lightusd_tinyvdb)
    # The graded background is experimental and not wired into stuffing yet.
    add_executable(test-graded-refine tests/volumetric/test-graded-refine.c
      src/external/tinyvdb/tinyvdb_tet_refine.c)
    target_include_directories(test-graded-refine PRIVATE src/external/tinyvdb)
    if(UNIX)
      target_link_libraries(test-graded-refine PRIVATE m)
    endif()
    if(MSVC)
      target_compile_options(test-graded-refine PRIVATE /UNDEBUG)
    else()
      target_compile_options(test-graded-refine PRIVATE -UNDEBUG)
    endif()
    add_test(NAME volumetric-graded-refine COMMAND test-graded-refine)
    set_tests_properties(volumetric-graded-refine PROPERTIES LABELS "cpu;volume")
  endif()

  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND TARGET lightusd_tinyvdb)
    add_executable(test-tet-overflow-32 tests/volumetric/tet-overflow-32.c src/lz4/lz4.c)
    target_include_directories(test-tet-overflow-32 PRIVATE src/external/tinyvdb)
    target_link_libraries(test-tet-overflow-32 PRIVATE lightusd_tinyvdb)
    add_test(NAME volumetric-tet-overflow-32 COMMAND test-tet-overflow-32)
    set_tests_properties(volumetric-tet-overflow-32 PROPERTIES LABELS "cpu;volume;security")
  endif()

  # Prefer an explicitly configured interpreter so CTest can use a virtualenv,
  # cross-build host Python, or another interpreter selected by CI. Keep the
  # automatic lookup as the default for ordinary native builds.
  if(LIGHTUSD_PYTHON_EXECUTABLE)
    set(LUSDR_PYTHON "${LIGHTUSD_PYTHON_EXECUTABLE}")
  else()
    find_program(LUSDR_PYTHON NAMES python3 python)
  endif()

  find_program(LIGHTUSD_HDRGEN_NODE_EXECUTABLE NAMES node nodejs)
  if(LIGHTUSD_HDRGEN_NODE_EXECUTABLE)
    add_test(
      NAME tool-hdrgen-test
      COMMAND ${LIGHTUSD_HDRGEN_NODE_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/tools/hdrgen/examples/test-presets.js)
    set_tests_properties(tool-hdrgen-test PROPERTIES
                         TIMEOUT 30 LABELS "tools;hdrgen;node;cpu")
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
  # lusdrender tests are registered by tools/lusdrender/CMakeLists.txt.


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
  # lusdview tests are registered by examples/lusdview/tests.cmake.


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
            --registry-output ${PROJECT_SOURCE_DIR}/src/schema/generated/openusd-supported-schema-names.inc
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
    #   python3 tests/legacy/usdc-writer/usdc-writer-runner.py --lusdcat ./build/lusdcat --lusddiff ./build/lusddiff --basedir tests/usda --verbose
    # lusddiff now lives under tools/ (LIGHTUSD_BUILD_TOOLS); skip if not built.
    if(TARGET lusddiff)
      add_test(
        NAME usdc-writer-diff-test
        COMMAND ${LUSDR_PYTHON} ${PROJECT_SOURCE_DIR}/tests/legacy/usdc-writer/usdc-writer-runner.py
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
  # LIGHTUSD_USD_ASSETS_ROOT setting, else USD_WG_ASSETS_DIR). The repository
  # verification harness supplies its prepared corpus explicitly. --max-fail
  # 0 gates on the load baseline.
  #
  find_program(LIGHTUSD_NODE_EXECUTABLE NAMES node nodejs)
  if(LIGHTUSD_NODE_EXECUTABLE AND TARGET lusdcat)
    set(_lightusd_corpus_root_args)
    if(LIGHTUSD_USD_ASSETS_ROOT)
      list(APPEND _lightusd_corpus_root_args --assets "${LIGHTUSD_USD_ASSETS_ROOT}")
    endif()
    add_test(
      NAME corpus-parse-native
      COMMAND ${LIGHTUSD_NODE_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/tests/parse-asset-corpus.mjs
              --optional-assets
              ${_lightusd_corpus_root_args}
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
    # lightrt_c_tri.c uses <stdatomic.h>: C11 plus, on MSVC, the C11 atomics flag.
    set_target_properties(feat-lightrt-uv PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
      # A source property, not $<COMPILE_LANGUAGE:C>: Visual Studio generators
      # ignore that genex on targets that mix C and C++.
      set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/src/external/lightrt/lightrt_c_tri.c
        PROPERTIES COMPILE_OPTIONS "/experimental:c11atomics")
    endif()
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
    # lusdzconvert -> src/legacy/usdz-convert.cc) end-to-end on the same fixtures so a
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
    # Both binaries create/remove the same /tmp/perfrc_scene_64.usda fixture.
    set_tests_properties(bench-render-convert PROPERTIES LABELS "benchmark"
                         RESOURCE_LOCK render_convert_fixture)

    if(LIGHTUSD_LEGACY_USE_CORE_PCP AND TARGET lightusd_core AND TARGET tydra)
      add_executable(bench-render-convert-next tests/feat/render-convert/perf-render-convert.cc)
      add_sanitizers(bench-render-convert-next)
      target_link_libraries(bench-render-convert-next PRIVATE
        ${LIGHTUSD_TARGET_STATIC} lightusd_core tydra)
      target_include_directories(bench-render-convert-next PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src)
      target_compile_definitions(bench-render-convert-next PRIVATE PERFRC_ENABLE_CORE=1)
      add_test(NAME bench-render-convert-next COMMAND bench-render-convert-next --iters 1 --prims 64)
      set_tests_properties(bench-render-convert-next PROPERTIES LABELS "benchmark"
                           RESOURCE_LOCK render_convert_fixture)
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

# workflow_tools is registered by the root CMakeLists.txt (core tools).
