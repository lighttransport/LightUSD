# Assume this cmake is called from lightusd root(../../)
# usd-texcomp: author GPU-compressed (.ktx2) texture companions + customData hints.
# Requires the vendored textools (tir / texcomp / texpipe).
set(EXAMPLE_TARGET "usd-texcomp")

add_executable(${EXAMPLE_TARGET} usd-texcomp-main.cc)

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${EXAMPLE_TARGET} lightusd_core lightusd_preview_images lightusd_textools)

# Zstd-supercompress KTX2 with the bundled codec when enabled. The example
# owns this optional encoder; the core USD parser does not depend on Zstd.
option(LIGHTUSD_WITH_ZSTD_COMPRESSION "Enable zstd compression" ON)
if(LIGHTUSD_WITH_ZSTD_COMPRESSION)
  target_sources(${EXAMPLE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/zstd.c)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE LIGHTUSD_WITH_ZSTD_COMPRESSION ZSTD_DISABLE_ASM=1)
endif()

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_usd_texcomp_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-texcomp-example.py
        $<TARGET_FILE:usd-texcomp> $<TARGET_FILE:lusdcat> $<BOOL:${LIGHTUSD_WITH_ZSTD_COMPRESSION}>)
    set_tests_properties(core_usd_texcomp_example PROPERTIES LABELS "core;examples;textures")
  endif()
endif()
