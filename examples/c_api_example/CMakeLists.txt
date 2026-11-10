# Consumer example for the portable core C ABI.
add_executable(c_api_example c-api-example-main.c)
add_sanitizers(c_api_example)
target_link_libraries(c_api_example PRIVATE lightusd_c)
set_target_properties(c_api_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(LIGHTUSD_BUILD_TESTS)
  add_test(NAME core_c_api_example
    COMMAND c_api_example ${PROJECT_SOURCE_DIR}/tests/usda/anytype-001.usda)
  set_tests_properties(core_c_api_example PROPERTIES LABELS "core;examples;c-api")
endif()
