# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "legacy-variant-lister")

set(SOURCES main.cc)

add_executable(${EXAMPLE_TARGET} ${SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${EXAMPLE_TARGET} lightusd_static)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
