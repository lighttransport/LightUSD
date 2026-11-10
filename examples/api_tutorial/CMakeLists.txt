# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "api_tutorial")

set(LIGHTUSD_API_TUTORIAL_SOURCES
    api-tutorial-main.cc
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
