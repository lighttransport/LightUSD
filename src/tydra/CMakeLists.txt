# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# Tydra Next - CMake build configuration

cmake_minimum_required(VERSION 3.10)

# Define tydra/next source files
set(TYDRA_NEXT_SOURCES
    scene-preview.cc
    scene-preview-lod.cc
    scene-preview-cli.cc
    scene-preview-sample.cc
    scene-preview-light.cc
    scene-preview-photometry.cc
    scene-preview-index.cc
    scene-preview-source.cc
    scene-preview-geometry.cc
    scene-preview-write.cc
    scene-preview-native-volume.cc
    scene-preview-volume.cc
    chunk-storage.cc
    render-data.cc
    gltf-export.cc
    scene-access.cc
    render-extract.cc
    render-converter.cc
    render-converter-saturating.cc
    render-converter-state.cc
    render-converter-sink.cc
    render-converter-geometry.cc
    render-converter-convert.cc
    render-converter-points.cc
    render-converter-volume.cc
    render-converter-particles.cc
    render-converter-curves.cc
    render-converter-clip.cc
    render-converter-tangents.cc
    render-converter-topology.cc
    render-converter-mesh-data.cc
    render-converter-primvars.cc
    render-converter-triangulate.cc
    render-converter-mesh.cc
    render-converter-preview.cc
    render-converter-shader-param.cc
    render-converter-texture.cc
    render-converter-animation.cc
  render-converter-lights.cc
  light-filter.cc
  render-converter-camera.cc
  render-converter-material.cc
  render-converter-mtlx-eval.cc
  render-converter-physics.cc
  render-converter-hierarchy.cc
  render-converter-bindings.cc
  render-converter-instancer.cc
  render-converter-assembly.cc
  render-converter-images.cc
  render-converter-instance-meshes.cc
  render-converter-generated.cc
  render-converter-bounds.cc
  render-converter-material-finalize.cc
  render-converter-light-linking.cc
    render-session.cc
    texture-cache.cc
    materialx.cc
    urdf-to-usd.cc
    urdf-to-usd-support.cc
    urdf-to-usd-profile.cc
    urdf-to-usd-geometry.cc
    urdf-to-usd-physics.cc
    urdf-to-usd-extensions.cc
    urdf-to-usd-scene.cc
    urdf-to-usd-deformable.cc
    ../external/mikktspace/mikktspace.c
)

set(TYDRA_NEXT_HEADERS
    scene-preview.hh
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
    urdf-payload.hh
)

# Prepend source directory to all files
list(TRANSFORM TYDRA_NEXT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM TYDRA_NEXT_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Create static library
add_library(tydra_next STATIC
    ${TYDRA_NEXT_SOURCES}
    ${TYDRA_NEXT_HEADERS}
)
option(TYDRA_NEXT_USE_PCH "Use a precompiled header for tydra/next C++ sources" ON)
if(TYDRA_NEXT_USE_PCH)
  target_precompile_headers(tydra_next PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_LIST_DIR}/pch.hh>")
  # Without this, clang embeds the header mtimes in the PCH and ccache cannot
  # cache any TU that uses it (CI sets the matching CCACHE_SLOPPINESS).
  target_compile_options(tydra_next PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:SHELL:-Xclang -fno-pch-timestamp>")
endif()

# Link to lightusd_next
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-asset-import.cmake")
target_link_libraries(tydra_next PUBLIC lightusd_asset_import lightusd_next)

if(NOT EMSCRIPTEN AND NOT TARGET lusdpreview)
  add_executable(lusdpreview "${CMAKE_CURRENT_LIST_DIR}/../../tools/lusdpreview/main.cc")
  target_link_libraries(lusdpreview PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
  if(TARGET lightusd)
    target_link_libraries(lusdpreview PRIVATE lightusd)
  else()
    # The standalone next product has no image I/O archive. Share the existing
    # decoder/resize implementation without introducing a legacy Stage link.
    add_library(lightusd_preview_images STATIC
      "${CMAKE_CURRENT_LIST_DIR}/../image-loader.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../image-writer.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../imageio/png-stream.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../io-util.cc"
      "${CMAKE_CURRENT_LIST_DIR}/texture-util.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../../tools/lusdpreview/image-resize-impl.cc")
    target_include_directories(lightusd_preview_images PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    target_compile_features(lightusd_preview_images PRIVATE cxx_std_17)
    target_link_libraries(lusdpreview PRIVATE lightusd_preview_images)
  endif()
  set_target_properties(lusdpreview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(LIGHTUSD_NEXT_BUILD_TESTS)
    add_executable(test_scene_preview "${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_scene_preview.cc")
    target_link_libraries(test_scene_preview PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_scene_preview COMMAND test_scene_preview)
    add_executable(test_scene_preview_compute "${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_scene_preview_compute.cc")
    target_link_libraries(test_scene_preview_compute PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview_compute PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview_compute PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_scene_preview_compute COMMAND test_scene_preview_compute cpu)
    add_executable(test_host_services "${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_host_services.cc")
    target_link_libraries(test_host_services PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_host_services PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_host_services PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_host_services COMMAND test_host_services)
    add_executable(test_scene_preview_photometry "${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_scene_preview_photometry.cc")
    target_link_libraries(test_scene_preview_photometry PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview_photometry PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview_photometry PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_scene_preview_photometry COMMAND test_scene_preview_photometry)
    add_executable(test_light_filter ${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_light_filter.cc)
    target_link_libraries(test_light_filter PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_light_filter PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_light_filter PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_light_filter COMMAND test_light_filter)
    add_executable(test_preview_light_assets ${CMAKE_CURRENT_LIST_DIR}/../../tests/next/test_preview_light_assets.cc)
    target_link_libraries(test_preview_light_assets PRIVATE ${LIGHTUSD_NEXT_ALLOCATOR_LIB} tydra_next)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_preview_light_assets PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_preview_light_assets PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME next_test_preview_light_assets COMMAND test_preview_light_assets)
  endif()
endif()

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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
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

# No legacy Stage/schema dependency: both products embed these shared objects.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/lightusd-render-support.cmake")
lightusd_add_render_support()
target_sources(tydra_next PRIVATE
    $<TARGET_OBJECTS:lightusd_ptex_objects>
    $<TARGET_OBJECTS:lightusd_subdiv_objects>)

# Optional: Build test
option(TYDRA_NEXT_BUILD_TESTS "Build tests for Tydra Next" OFF)

if(TYDRA_NEXT_BUILD_TESTS)
    # src/next (LIGHTUSD_NEXT_BUILD_TESTS) also defines test_tydra_next and
    # registers it with ctest; define it here only when that has not happened,
    # so enabling both options does not fail configure with a duplicate target.
    if(NOT TARGET test_tydra_next)
        add_executable(test_tydra_next
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/next/test_tydra_next.cc
        )
        target_link_libraries(test_tydra_next PRIVATE tydra_next lightusd_next)
    endif()
    add_executable(skinning_points_dump
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/next/skinning-points-dump.cc)
    target_link_libraries(skinning_points_dump PRIVATE tydra_next lightusd_next)
endif()

if(LIGHTUSD_NEXT_BUILD_FUZZERS)
    target_compile_options(tydra_next PRIVATE ${NEXT_FUZZ_LIB_FLAGS})
    add_executable(fuzz_next_render
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzzer/next_render_fuzzmain.cc)
    target_link_libraries(fuzz_next_render PRIVATE tydra_next lightusd_next)
    target_compile_options(fuzz_next_render PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
    target_link_options(fuzz_next_render PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
endif()

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_NEXT_BUILD_TESTS)
  add_executable(test_gltf_export ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/next/test_gltf_export.cc)
  target_link_libraries(test_gltf_export PRIVATE tydra_next)
  add_test(NAME next_gltf_export COMMAND test_gltf_export)
endif()

# Optional hardware preview compute; CPU builds retain no GPU dependency.
target_sources(tydra_next PRIVATE scene-preview-compute.cc)
option(LIGHTUSD_PREVIEW_GPU_COMPUTE "Enable optional GPU preview compute kernels" ON)
if(NOT EMSCRIPTEN AND LIGHTUSD_PREVIEW_GPU_COMPUTE)
  find_package(Vulkan QUIET)
  find_program(LIGHTUSD_PREVIEW_GLSLANG glslangValidator)
  if(Vulkan_FOUND AND LIGHTUSD_PREVIEW_GLSLANG)
    set(preview_spv "${CMAKE_CURRENT_BINARY_DIR}/scene-preview-compute-spv.hh")
    add_custom_command(OUTPUT "${preview_spv}"
      COMMAND "${LIGHTUSD_PREVIEW_GLSLANG}" -V --target-env vulkan1.1 -S comp
        --vn kPreviewComputeSpv -o "${preview_spv}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scene-preview-compute.comp"
      DEPENDS scene-preview-compute.comp scene-preview-kernel.inc VERBATIM)
    target_sources(tydra_next PRIVATE scene-preview-compute-vk.cc "${preview_spv}")
    target_compile_definitions(tydra_next PRIVATE LIGHTUSD_PREVIEW_VULKAN=1)
    target_include_directories(tydra_next SYSTEM PRIVATE ${Vulkan_INCLUDE_DIRS})
  endif()
  find_path(LIGHTUSD_PREVIEW_CUDA_INCLUDE cuda.h HINTS /usr/local/cuda/include)
  if(LIGHTUSD_PREVIEW_CUDA_INCLUDE AND UNIX)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene-preview-kernel.inc" preview_kernel_source)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/scene-preview-kernel-source.hh"
      "static const char kPreviewKernelSource[] = R\"kernel(${preview_kernel_source})kernel\";\n")
    target_sources(tydra_next PRIVATE scene-preview-compute-cuda.cc)
    target_include_directories(tydra_next PRIVATE "${LIGHTUSD_PREVIEW_CUDA_INCLUDE}")
    target_compile_definitions(tydra_next PRIVATE LIGHTUSD_PREVIEW_CUDA=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS scene-preview-kernel.inc)
  endif()
  target_include_directories(tydra_next PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
