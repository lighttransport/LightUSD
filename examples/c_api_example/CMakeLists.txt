# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "c_api_example")

set(LIGHTUSD_API_TUTORIAL_SOURCES
    c-api-example-main.c
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_API_TUTORIAL_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${EXAMPLE_TARGET} c-lightusd_static)

# The next C ABI consumer is built when the portable next product is present;
# the legacy example remains available for compatibility builds.
if(TARGET lightusd_c)
  add_executable(next_c_api_example next-c-api-example.c)
  add_sanitizers(next_c_api_example)
  target_include_directories(next_c_api_example PRIVATE ${PROJECT_SOURCE_DIR}/src/c-api)
  target_link_libraries(next_c_api_example lightusd_c)
  set_target_properties(next_c_api_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
