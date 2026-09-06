# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# Tydra Next - CMake build configuration

cmake_minimum_required(VERSION 3.10)

# Define tydra/next source files
set(TYDRA_NEXT_SOURCES
    render-data.cc
    scene-access.cc
    render-extract.cc
    render-converter.cc
    render-session.cc
    texture-cache.cc
    materialx.cc
    urdf-to-usd.cc
    ../../external/mikktspace/mikktspace.c
)

set(TYDRA_NEXT_HEADERS
    chunked-array.hh
    render-data.hh
    scene-access.hh
    render-extract.hh
    render-converter.hh
    render-session.hh
    resource-budget.hh
    texture-cache.hh
    materialx.hh
    urdf-to-usd.hh
    ../urdf-payload.hh
)

# Prepend source directory to all files
list(TRANSFORM TYDRA_NEXT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM TYDRA_NEXT_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Create static library
add_library(tydra_next STATIC
    ${TYDRA_NEXT_SOURCES}
    ${TYDRA_NEXT_HEADERS}
)

# Link to lightusd_next
target_link_libraries(tydra_next PUBLIC lightusd_next)

# Windows: GetProcessMemoryInfo (process RSS for the memory budget, see
# mem-budget.hh) lives in psapi. PUBLIC so every consumer (test_tydra_next,
# lusdview, lusdrender, tydra_to_renderscene) inherits the link automatically.
if(WIN32)
  target_link_libraries(tydra_next PUBLIC psapi)
endif()

# Keep Tydra-next on the repository C++17 baseline.
target_compile_features(tydra_next PUBLIC cxx_std_17)

if(LIGHTUSD_ENABLE_THREAD)
    target_compile_definitions(tydra_next PUBLIC LIGHTUSD_ENABLE_THREAD)
    target_link_libraries(tydra_next PUBLIC Threads::Threads)
endif()

# Set include directories
target_include_directories(tydra_next PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
    $<INSTALL_INTERFACE:include>
)

# Compiler warnings
if(MSVC)
    target_compile_options(tydra_next PRIVATE /W4)
    # Keep MSVC toolchain + STL compatibility checks from aborting the build on
    # older but still usable compiler versions.
    target_compile_definitions(tydra_next PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
else()
    target_compile_options(tydra_next PRIVATE -Wall -Wextra -Wpedantic)
endif()

# tydra_next uses no C++ RTTI and no C++ exceptions; disable both. The
# LIGHTUSD_CXX_RTTI / _EXCEPTIONS variables are inherited from the parent project
# when built as a subdirectory (undefined -> disabled when built standalone).
if(NOT LIGHTUSD_CXX_RTTI)
    if(MSVC)
        target_compile_options(tydra_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    else()
        target_compile_options(tydra_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS)
    if(MSVC)
        target_compile_options(tydra_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
        target_compile_options(tydra_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
endif()

# The lazy Ptex face decoder is part of the top-level lightusd static library
# when this project is embedded.  Standalone Tydra-next builds need the small
# reader and bundled deflate implementation locally instead.
if(TARGET lightusd_static)
    target_link_libraries(tydra_next PUBLIC lightusd_static)
else()
    target_sources(tydra_next PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../ptx-loader.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-validate.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-topology.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-bilinear.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-catmark.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-loop.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-fvar.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-refine.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-stream.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-limit.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tsd/tsd-lightusd.cc)
endif()

# Optional: Build test
option(TYDRA_NEXT_BUILD_TESTS "Build tests for Tydra Next" OFF)

if(TYDRA_NEXT_BUILD_TESTS)
    add_executable(test_tydra_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tydra/next/test_tydra_next.cc
    )
    target_link_libraries(test_tydra_next PRIVATE tydra_next lightusd_next)
endif()

if(LIGHTUSD_NEXT_BUILD_FUZZERS)
    target_compile_options(tydra_next PRIVATE ${NEXT_FUZZ_LIB_FLAGS})
    add_executable(fuzz_next_render
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fuzzer/next_render_fuzzmain.cc)
    target_link_libraries(fuzz_next_render PRIVATE tydra_next lightusd_next)
    target_compile_options(fuzz_next_render PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
    target_link_options(fuzz_next_render PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
endif()
