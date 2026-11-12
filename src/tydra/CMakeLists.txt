# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# Tydra - CMake build configuration

cmake_minimum_required(VERSION 3.10)

# Define tydra source files
set(TYDRA_SOURCES
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
    mesh-index.cc
    obj-export.cc
    usd-export.cc
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
    materialx-volume.cc
    ../external/lightrt/mtlxrender/color.c
    ../external/lightrt/mtlxrender/mtlx_doc.c
    ../external/lightrt/mtlxrender/mtlx_eval.c
    ../external/lightrt/mtlxrender/mtlx_xml.c
    ../external/lightrt/mtlxrender/texture.c
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

set(TYDRA_HEADERS
    scene-preview.hh
    chunked-array.hh
    render-data.hh
    mesh-index.hh
    obj-export.hh
    usd-export.hh
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
list(TRANSFORM TYDRA_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM TYDRA_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Create static library
add_library(tydra STATIC
    ${TYDRA_SOURCES}
    ${TYDRA_HEADERS}
)
target_compile_features(tydra PRIVATE c_std_11)
target_compile_definitions(tydra PRIVATE LIGHTUSD_MTLX_STB_STATIC=1)
target_include_directories(tydra PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../external"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/lightrt/mtlxrender")
option(LIGHTUSD_TYDRA_USE_PCH "Use a precompiled header for Tydra C++ sources" ON)
if(LIGHTUSD_TYDRA_USE_PCH)
  target_precompile_headers(tydra PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_LIST_DIR}/pch.hh>")
  # Without this, clang embeds the header mtimes in the PCH and ccache cannot
  # cache any TU that uses it (CI sets the matching CCACHE_SLOPPINESS).
  target_compile_options(tydra PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:SHELL:-Xclang -fno-pch-timestamp>")
endif()

# Link to lightusd_core
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-asset-import.cmake")
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-rb-solver.cmake)
target_link_libraries(tydra PUBLIC lightusd_asset_import lightusd_core lightusd_rb_solver)

if(NOT EMSCRIPTEN AND NOT TARGET lusdpreview)
  add_executable(lusdpreview "${CMAKE_CURRENT_LIST_DIR}/../../tools/lusdpreview/main.cc")
  target_link_libraries(lusdpreview PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
  if(TARGET lightusd)
    target_link_libraries(lusdpreview PRIVATE lightusd)
  else()
    # The standalone core product has no image I/O archive. Share the existing
    # decoder/resize implementation without introducing a legacy Stage link.
    add_library(lightusd_preview_images STATIC
      "${CMAKE_CURRENT_LIST_DIR}/../image-loader.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../image-convert.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../image-writer.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../imageio/png-stream.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../io-util.cc"
      "${CMAKE_CURRENT_LIST_DIR}/texture-util.cc"
      "${CMAKE_CURRENT_LIST_DIR}/../../tools/lusdpreview/image-resize-impl.cc")
    target_include_directories(lightusd_preview_images PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    target_compile_features(lightusd_preview_images PRIVATE cxx_std_17)
    option(LIGHTUSD_WITH_TIFF "Build native TIFF/DNG image support" ON)
    if(LIGHTUSD_WITH_TIFF)
      target_sources(lightusd_preview_images PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../external/tiny_dng_loader.cc")
      target_compile_definitions(lightusd_preview_images PRIVATE LIGHTUSD_WITH_TIFF=1)
      # TinyDNG ZIP uses the shared miniz owner, not a second codec copy.
      target_link_libraries(lightusd_preview_images PUBLIC lightusd_tinyvdb)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-native-exr.cmake")
    if(LIGHTUSD_WITH_EXR)
      target_compile_definitions(lightusd_preview_images PUBLIC LIGHTUSD_WITH_EXR=1 LIGHTUSD_EXR_V3=1)
      target_link_libraries(lightusd_preview_images PUBLIC lightusd_exr_codec)
    endif()
    if(LIGHTUSD_WITH_TEXTOOLS)
      target_compile_definitions(lightusd_preview_images PUBLIC LIGHTUSD_WITH_TEXTOOLS=1)
      target_link_libraries(lightusd_preview_images PUBLIC lightusd_textools)
    endif()
    target_link_libraries(lusdpreview PRIVATE lightusd_preview_images)
  endif()
  set_target_properties(lusdpreview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(LIGHTUSD_BUILD_TESTS)
    add_executable(test_scene_preview "${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_scene_preview.cc")
    target_link_libraries(test_scene_preview PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_scene_preview COMMAND test_scene_preview)
    if(LIGHTUSD_WITH_TIFF AND TARGET lightusd_preview_images)
      add_executable(test_native_tiff "${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_native_tiff.cc")
      target_link_libraries(test_native_tiff PRIVATE tydra lightusd_preview_images)
      add_test(NAME core_test_native_tiff COMMAND test_native_tiff)
    endif()
    add_executable(test_scene_preview_compute "${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_scene_preview_compute.cc")
    target_link_libraries(test_scene_preview_compute PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview_compute PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview_compute PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_scene_preview_compute COMMAND test_scene_preview_compute cpu)
    add_executable(test_host_services "${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_host_services.cc")
    target_link_libraries(test_host_services PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_host_services PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_host_services PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_host_services COMMAND test_host_services)
    add_executable(test_scene_preview_photometry "${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_scene_preview_photometry.cc")
    target_link_libraries(test_scene_preview_photometry PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_scene_preview_photometry PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_scene_preview_photometry PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_scene_preview_photometry COMMAND test_scene_preview_photometry)
    add_executable(test_light_filter ${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_light_filter.cc)
    target_link_libraries(test_light_filter PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_light_filter PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_light_filter PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_light_filter COMMAND test_light_filter)
    add_executable(test_preview_light_assets ${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_preview_light_assets.cc)
    target_link_libraries(test_preview_light_assets PRIVATE ${LIGHTUSD_ALLOCATOR_LIB} tydra)
    if(TARGET lightusd_preview_images)
      target_link_libraries(test_preview_light_assets PRIVATE lightusd_preview_images)
    else()
      target_link_libraries(test_preview_light_assets PRIVATE ${LIGHTUSD_TARGET_STATIC})
    endif()
    add_test(NAME core_test_preview_light_assets COMMAND test_preview_light_assets)
  endif()
endif()

# Windows: GetProcessMemoryInfo (process RSS for the memory budget, see
# mem-budget.hh) lives in psapi. PUBLIC so every consumer (test_tydra,
# lusdview, lusdrender, tydra_to_renderscene) inherits the link automatically.
if(WIN32)
  target_link_libraries(tydra PUBLIC psapi)
endif()

# Keep Tydra on the repository C++17 baseline.
target_compile_features(tydra PUBLIC cxx_std_17)

if(LIGHTUSD_ENABLE_THREAD)
    target_compile_definitions(tydra PUBLIC LIGHTUSD_ENABLE_THREAD)
    target_link_libraries(tydra PUBLIC Threads::Threads)
endif()

# Set include directories
target_include_directories(tydra PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

# Compiler warnings
if(MSVC)
    target_compile_options(tydra PRIVATE /W4)
    # Keep MSVC toolchain + STL compatibility checks from aborting the build on
    # older but still usable compiler versions.
    target_compile_definitions(tydra PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
else()
    target_compile_options(tydra PRIVATE -Wall -Wextra -Wpedantic)
endif()

# tydra uses no C++ RTTI and no C++ exceptions; disable both. The
# LIGHTUSD_CXX_RTTI / _EXCEPTIONS variables are inherited from the parent project
# when built as a subdirectory (undefined -> disabled when built standalone).
if(NOT LIGHTUSD_CXX_RTTI)
    if(MSVC)
        target_compile_options(tydra PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    else()
        target_compile_options(tydra PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS)
    if(MSVC)
        target_compile_options(tydra PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
        target_compile_options(tydra PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
endif()

# No legacy Stage/schema dependency: both products embed these shared objects.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/lightusd-render-support.cmake")
lightusd_add_render_support()
target_sources(tydra PRIVATE
    $<TARGET_OBJECTS:lightusd_ptex_objects>
    $<TARGET_OBJECTS:lightusd_subdiv_objects>)

# Optional: Build test
option(LIGHTUSD_TYDRA_BUILD_TESTS "Build tests for Tydra" OFF)

if(LIGHTUSD_TYDRA_BUILD_TESTS)
    # src/ (LIGHTUSD_BUILD_TESTS) also defines test_tydra and
    # registers it with ctest; define it here only when that has not happened,
    # so enabling both options does not fail configure with a duplicate target.
    if(NOT TARGET test_tydra)
        add_executable(test_tydra
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/test_tydra.cc
        )
        target_link_libraries(test_tydra PRIVATE tydra lightusd_core)
    endif()
    add_executable(skinning_points_dump
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/skinning-points-dump.cc)
    target_link_libraries(skinning_points_dump PRIVATE tydra lightusd_core)
endif()

if(LIGHTUSD_BUILD_FUZZERS)
    target_compile_options(tydra PRIVATE ${CORE_FUZZ_LIB_FLAGS})
    add_executable(fuzz_core_render
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzzer/core_render_fuzzmain.cc)
    target_link_libraries(fuzz_core_render PRIVATE tydra lightusd_core)
    target_compile_options(fuzz_core_render PRIVATE ${CORE_FUZZ_BIN_FLAGS})
    target_link_options(fuzz_core_render PRIVATE ${CORE_FUZZ_BIN_FLAGS})
endif()

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_BUILD_TESTS)
  add_executable(test_mesh_index ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/test_mesh_index.cc)
  target_link_libraries(test_mesh_index PRIVATE tydra)
  add_test(NAME core_mesh_index COMMAND test_mesh_index)
  add_executable(test_usd_export ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/test_usd_export.cc)
  target_link_libraries(test_usd_export PRIVATE tydra)
  add_test(NAME core_usd_export COMMAND test_usd_export)
  add_executable(test_obj_export ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/test_obj_export.cc)
  target_link_libraries(test_obj_export PRIVATE tydra)
  add_test(NAME core_obj_export COMMAND test_obj_export)
  add_executable(test_gltf_export ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/test_gltf_export.cc)
  target_link_libraries(test_gltf_export PRIVATE tydra)
  add_test(NAME core_gltf_export COMMAND test_gltf_export)
endif()

# Optional hardware preview compute; CPU builds retain no GPU dependency.
target_sources(tydra PRIVATE scene-preview-compute.cc)
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
    target_sources(tydra PRIVATE scene-preview-compute-vk.cc "${preview_spv}")
    target_compile_definitions(tydra PRIVATE LIGHTUSD_PREVIEW_VULKAN=1)
    target_include_directories(tydra SYSTEM PRIVATE ${Vulkan_INCLUDE_DIRS})
  endif()
  find_path(LIGHTUSD_PREVIEW_CUDA_INCLUDE cuda.h HINTS /usr/local/cuda/include)
  if(LIGHTUSD_PREVIEW_CUDA_INCLUDE AND UNIX)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene-preview-kernel.inc" preview_kernel_source)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/scene-preview-kernel-source.hh"
      "static const char kPreviewKernelSource[] = R\"kernel(${preview_kernel_source})kernel\";\n")
    target_sources(tydra PRIVATE scene-preview-compute-cuda.cc)
    target_include_directories(tydra PRIVATE "${LIGHTUSD_PREVIEW_CUDA_INCLUDE}")
    target_compile_definitions(tydra PRIVATE LIGHTUSD_PREVIEW_CUDA=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS scene-preview-kernel.inc)
  endif()
  target_include_directories(tydra PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(LIGHTUSD_BUILD_EXAMPLES)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../examples/tydra_api
                   ${CMAKE_CURRENT_BINARY_DIR}/examples/tydra_api)
endif()

if(NOT EMSCRIPTEN AND LIGHTUSD_BUILD_EXAMPLES AND TARGET lightusd_textools)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../examples/usd-texcomp
                   ${CMAKE_CURRENT_BINARY_DIR}/examples/usd-texcomp)
endif()

if(NOT EMSCRIPTEN AND LIGHTUSD_BUILD_EXAMPLES)
  add_executable(triangulation_method_example
    ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/triangulation_method_example.cc)
  target_link_libraries(triangulation_method_example PRIVATE tydra)
  set_target_properties(triangulation_method_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(LIGHTUSD_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME core_triangulation_example
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-triangulation-example.py
          $<TARGET_FILE:triangulation_method_example>)
      set_tests_properties(core_triangulation_example PROPERTIES LABELS "core;examples;tydra")
    endif()
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS)
  add_executable(test_rb_solver ${CMAKE_CURRENT_LIST_DIR}/../../tests/core/test_rb_solver.cc)
  target_link_libraries(test_rb_solver PRIVATE lightusd_rb_solver)
  add_test(NAME core_test_rb_solver COMMAND test_rb_solver)
endif()
