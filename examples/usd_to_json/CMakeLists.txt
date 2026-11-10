# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "usd_to_json")

set(LIGHTUSD_USD_TO_JSON_SOURCES
    usdz_to_json-main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_USD_TO_JSON_SOURCES})


target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${EXAMPLE_TARGET} lightusd_core)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_usdz_json_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-usdz-json-example.py
        $<TARGET_FILE:usd_to_json> $<TARGET_FILE:lusdcat>)
    set_tests_properties(core_usdz_json_example PROPERTIES LABELS "core;examples;json")
  endif()
endif()
