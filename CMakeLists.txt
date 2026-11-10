cmake_minimum_required(VERSION 3.16)

set(LIGHTUSD_TARGET "lightusd") # TODO: drop `z`
set(LIGHTUSD_TARGET_NS "lightusd::lightusd") # dll lib. may not be built.
set(LIGHTUSD_TARGET_STATIC_NS "lightusd::lightusd_static") # static lib is always built.
set(LIGHTUSD_TARGET_STATIC "lightusd_static") # alias to ${LIGHTUSD_TARGET_STATIC_NS}
set(BUILD_TARGET_C "c-lightusd")
#set(BUILD_TARGET_BLENDER_PY "lightusd_blender")
set(LIGHTUSD_TEST_TARGET "test_lightusd")

project(${LIGHTUSD_TARGET} C CXX)
option(LIGHTUSD_NATIVE_IO "Enable built-in native filesystem I/O backends" ON)
if(NOT LIGHTUSD_NATIVE_IO)
  add_definitions(-DLIGHTUSD_NO_NATIVE_IO=1 -DLIGHTUSD_NO_MMAP=1 -DLIGHTUSD_NO_NATIVE_RESOLVER_IO=1 -DLIGHTUSD_MMAP_SUPPORTED=0)
endif()
option(LIGHTUSD_NATIVE_HOST_SERVICES "Build the explicit native host service provider" ON)

include(${PROJECT_SOURCE_DIR}/tools/ousddiff/tests.cmake)

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


# The core implementation (src/) is always built. The legacy implementation
# (src/legacy) is compiled on request only and is not tested.
option(LIGHTUSD_BUILD_LEGACY
       "Also build the legacy implementation (src/legacy; compiled, not tested)" OFF)
if(DEFINED LIGHTUSD_NATIVE_PRODUCT)
  # Pre-1.1 product switch.
  if(LIGHTUSD_NATIVE_PRODUCT STREQUAL "legacy")
    message(DEPRECATION
      "LIGHTUSD_NATIVE_PRODUCT=legacy is deprecated; use -DLIGHTUSD_BUILD_LEGACY=ON.")
    set(LIGHTUSD_BUILD_LEGACY ON CACHE BOOL
        "Also build the legacy implementation (src/legacy; compiled, not tested)" FORCE)
  endif()
  unset(LIGHTUSD_NATIVE_PRODUCT CACHE)
endif()
# [ccache] Configured first so the core and the legacy build use it.
option(LIGHTUSD_USE_CCACHE "Use ccache for faster recompile." ON)
option(LIGHTUSD_LEGACY_USE_PCH "Legacy: use a precompiled header (does not help that codebase; opt-in)." OFF)
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
      if((LIGHTUSD_USE_PCH OR LIGHTUSD_USE_PCH OR LIGHTUSD_LEGACY_USE_PCH OR NOT DEFINED LIGHTUSD_USE_PCH) AND CMAKE_HOST_UNIX)
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

# ---- Core -----------------------------------------------------------------
# Sanitizer support (SANITIZE_ADDRESS / SANITIZE_UNDEFINED) for core targets.
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers)
if(SANITIZE_ADDRESS)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
endif()
if(SANITIZE_UNDEFINED)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
endif()
# Options renamed from LIGHTUSD_NEXT_* / TYDRA_NEXT_* in 1.1.
include(${PROJECT_SOURCE_DIR}/cmake/DeprecatedOption.cmake)
lightusd_apply_deprecated_core_options()
option(LIGHTUSD_BUILD_TESTS "Build tests" OFF)
option(LIGHTUSD_BUILD_EXAMPLES "Build examples" OFF)
if(NOT DEFINED LIGHTUSD_BUILD_LUSDCAT)
  # `lusdcat` is the default CLI. WASM/WASI builds have no command line to
  # drive, so they skip it.
  if(EMSCRIPTEN OR WASI OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(LIGHTUSD_BUILD_LUSDCAT OFF CACHE BOOL "Build the lusdcat CLI")
  else()
    set(LIGHTUSD_BUILD_LUSDCAT ON CACHE BOOL "Build the lusdcat CLI")
  endif()
endif()
if(NOT DEFINED LIGHTUSD_ENABLE_THREAD)
  # Parallel paths (source warm, arc-layer prefetch, opinion fill,
  # crate-writer build) are ON by default for native builds; WASM keeps
  # std::thread off.
  if(EMSCRIPTEN)
    set(LIGHTUSD_ENABLE_THREAD OFF CACHE BOOL "Enable optional parallel paths")
  else()
    set(LIGHTUSD_ENABLE_THREAD ON CACHE BOOL "Enable optional parallel paths")
  endif()
endif()
if(LIGHTUSD_BUILD_EXAMPLES AND NOT DEFINED LIGHTUSD_BUILD_C_API_EXAMPLE)
  set(LIGHTUSD_BUILD_C_API_EXAMPLE ON CACHE BOOL
      "Build the public C API consumer example")
endif()
if(LIGHTUSD_BUILD_TESTS)
  enable_testing()
endif()
add_subdirectory(src)
if(COMMAND add_sanitizers)
  add_sanitizers(lightusd_core)
endif()
add_library(lightusd::core ALIAS lightusd_core)
# Pre-1.1 alias; remove after one release.
add_library(lightusd::next ALIAS lightusd_core)
option(LIGHTUSD_BUILD_TOOLS "Build command-line tools (lusdchecker, ...)" OFF)
if(LIGHTUSD_BUILD_TOOLS)
  add_subdirectory(tools/lusdchecker)
  add_subdirectory(tools/lusddeps)
  add_subdirectory(tools/lusddumpcrate)
  add_subdirectory(tools/lusddiff)
  add_subdirectory(tools/lusdzconvert)
  if(LIGHTUSD_WITH_TYDRA AND NOT TARGET lusdrender)
    add_subdirectory(tools/lusdrender)
  endif()
endif()
if(LIGHTUSD_BUILD_EXAMPLES AND LIGHTUSD_WITH_TYDRA)
  add_subdirectory(examples/workflows)
endif()

# End-to-end workflow over the core CLI tools.
if(LIGHTUSD_BUILD_TESTS AND TARGET lusddeps AND TARGET quickstart AND TARGET lusddiff)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(workflow_capture_args)
    if(TARGET lusdrender)
      list(APPEND workflow_capture_args --capture-renderer $<TARGET_FILE:lusdrender>)
    endif()
    add_test(NAME workflow_tools COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/tests/run-workflow-tools.py --bin-dir ${CMAKE_BINARY_DIR} ${workflow_capture_args})
    set_tests_properties(workflow_tools PROPERTIES TIMEOUT 120 LABELS "tools;core")
  endif()
endif()

option(LIGHTUSD_BUILD_QUICKLOOK
       "Build the minimal portable USD Quick Look viewer 'lusdquicklook' (software UI via vendored lightui, CPU ray tracing; no GPU required)."
       OFF)
if(LIGHTUSD_BUILD_QUICKLOOK)
  add_subdirectory(examples/lusdquicklook)
endif()

# ---- Legacy (optional; compiled, not tested) -------------------------------
if(LIGHTUSD_BUILD_LEGACY)
  include(${PROJECT_SOURCE_DIR}/src/legacy/legacy.cmake)
endif()
