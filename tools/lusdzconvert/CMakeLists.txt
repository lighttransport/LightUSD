cmake_minimum_required(VERSION 3.16)

# Tool: lusdzconvert
# Convert USD(A/C/Z) to a USDZ package (or flat USDC/USDA) on the core
# library: composition flatten, texture resize / re-encode / budget fit,
# and channel repack. The converter lives in src/usdz/usdz-converter.cc.
if(NOT TARGET lightusd_preview_images)
  message(STATUS "lusdzconvert: lightusd_preview_images unavailable (LIGHTUSD_WITH_TYDRA off?); skipping")
  return()
endif()

add_library(lightusd_usdz_convert STATIC
  ${PROJECT_SOURCE_DIR}/src/usdz/usdz-converter.cc
  ${PROJECT_SOURCE_DIR}/src/usdz/udim-convert.cc
  ${PROJECT_SOURCE_DIR}/src/usdz/material-atlas.cc
  ${PROJECT_SOURCE_DIR}/src/udim-core-layer.cc
  ${PROJECT_SOURCE_DIR}/src/udim-layer.cc
  ${PROJECT_SOURCE_DIR}/src/udim-layer-geometry.cc
  ${PROJECT_SOURCE_DIR}/src/udim-mesh.cc
  ${PROJECT_SOURCE_DIR}/src/udim-bake.cc
  ${PROJECT_SOURCE_DIR}/src/sha256.cc
)
target_include_directories(lightusd_usdz_convert PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(lightusd_usdz_convert PUBLIC cxx_std_17)
target_link_libraries(lightusd_usdz_convert PUBLIC lightusd_preview_images tydra lightusd_core)

add_executable(lusdzconvert lusdzconvert.cc)
target_link_libraries(lusdzconvert PRIVATE lightusd_usdz_convert ${LIGHTUSD_ALLOCATOR_LIB})
set_target_properties(lusdzconvert PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS AND UNIX AND TARGET lusdcat)
  add_test(NAME core_lusdzconvert_regression
           COMMAND bash ${PROJECT_SOURCE_DIR}/tests/run-lusdzconvert-regression.sh
                   $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat>
                   ${PROJECT_SOURCE_DIR})
  set_tests_properties(core_lusdzconvert_regression PROPERTIES LABELS "core")
endif()
if(LIGHTUSD_BUILD_TESTS AND UNIX AND TARGET lusdcat)
  # The variant ref->payload->ref chain through the converter's flatten path.
  add_test(NAME core_lusdzconvert_variant_chain
           COMMAND bash ${PROJECT_SOURCE_DIR}/tests/run-variant-chain.sh
                   $<TARGET_FILE:lusdcat> ${PROJECT_SOURCE_DIR}
                   $<TARGET_FILE:lusdzconvert>)
  set_tests_properties(core_lusdzconvert_variant_chain PROPERTIES LABELS "core")
endif()

if(LIGHTUSD_BUILD_TESTS AND LIGHTUSD_WITH_TEXTOOLS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_lusdzconvert_ktx2
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-converter-ktx2.py
        $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat> ${PROJECT_SOURCE_DIR})
    set_tests_properties(core_lusdzconvert_ktx2 PROPERTIES LABELS "core;converter;textures")
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_lusdzconvert_materials
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-converter-materials.py
        $<TARGET_FILE:lusdzconvert>)
    set_tests_properties(core_lusdzconvert_materials PROPERTIES LABELS "core;converter;materials")
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_lightusd_udim_precision_args)
    if(LIGHTUSD_WITH_EXR)
      set(_lightusd_udim_precision_args --with-exr)
    endif()
    add_test(NAME core_lusdzconvert_udim
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-converter-udim.py
        $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat> ${_lightusd_udim_precision_args})
    set_tests_properties(core_lusdzconvert_udim PROPERTIES LABELS "core;converter;udim")
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat AND Python3_EXECUTABLE)
  add_test(NAME core_lusdzconvert_geometry
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-converter-geometry.py
      $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat> ${PROJECT_SOURCE_DIR})
  set_tests_properties(core_lusdzconvert_geometry PROPERTIES LABELS "core;converter")
endif()

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat AND Python3_EXECUTABLE)
  add_test(NAME core_lusdzconvert_images
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-converter-images.py
      $<TARGET_FILE:lusdzconvert> $<TARGET_FILE:lusdcat>)
  set_tests_properties(core_lusdzconvert_images PROPERTIES LABELS "core;converter;textures")
endif()
