# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "tydra_api")

set(LIGHTUSD_TYDRA_CONVERT_SOURCES
    tydra-api-main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_TYDRA_CONVERT_SOURCES})
if(COMMAND add_sanitizers)
  add_sanitizers(${EXAMPLE_TARGET})
endif()

target_link_libraries(${EXAMPLE_TARGET} PRIVATE tydra)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_tydra_api_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-tydra-api-example.py
        $<TARGET_FILE:tydra_api>)
    set_tests_properties(core_tydra_api_example PROPERTIES LABELS "core;examples;tydra")
  endif()
endif()
