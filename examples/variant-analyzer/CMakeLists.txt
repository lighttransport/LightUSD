# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "variant-analyzer")

set(SOURCES main.cc)

add_executable(${EXAMPLE_TARGET} ${SOURCES})
if(COMMAND add_sanitizers)
  add_sanitizers(${EXAMPLE_TARGET})
endif()

target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_core)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_variant_analyzer
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-variant-analyzer.py
        $<TARGET_FILE:variant-analyzer> $<TARGET_FILE:lusdcat>)
    set_tests_properties(core_variant_analyzer PROPERTIES LABELS "core;examples;variants")
  endif()
endif()
