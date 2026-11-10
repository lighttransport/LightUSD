# Optional self-contained QuickJS host over the core Stage API.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/lightusd-quickjs.cmake)
lightusd_add_quickjs(${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/quickjs-ng)
add_executable(js-script main.cc)
target_link_libraries(js-script PRIVATE lightusd_core lightusd_quickjs)
set_target_properties(js-script PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
configure_file(query_layer.js ${CMAKE_BINARY_DIR}/query_layer.js COPYONLY)
if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_js_script_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-js-script-example.py
        $<TARGET_FILE:js-script>)
    set_tests_properties(core_js_script_example PROPERTIES LABELS "core;examples;scripting" TIMEOUT 20)
  endif()
endif()
