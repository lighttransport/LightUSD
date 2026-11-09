# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "api_tutorial")

set(LIGHTUSD_API_TUTORIAL_SOURCES
    api-tutorial-main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_API_TUTORIAL_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)

if(LIGHTUSD_NATIVE_PRODUCT STREQUAL "next")
  target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src/next)
  target_link_libraries(${EXAMPLE_TARGET} lightusd_next)
else()
  target_link_libraries(${EXAMPLE_TARGET} lightusd_static)
endif()

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
