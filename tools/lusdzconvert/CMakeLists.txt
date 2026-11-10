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
