# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "file_format_example")

set(LIGHTUSD_API_TUTORIAL_SOURCES
    file-format-example.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_API_TUTORIAL_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_core)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS)
  add_test(NAME core_${EXAMPLE_TARGET} COMMAND $<TARGET_FILE:${EXAMPLE_TARGET}>)
  set_tests_properties(core_${EXAMPLE_TARGET} PROPERTIES LABELS "core;examples")
endif()

if(LIGHTUSD_BUILD_TESTS)
  add_test(NAME core_file_format_missing_root
    COMMAND $<TARGET_FILE:file_format_example>
      ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.usda)
  set_tests_properties(core_file_format_missing_root PROPERTIES
    WILL_FAIL TRUE LABELS "core;examples")
endif()
