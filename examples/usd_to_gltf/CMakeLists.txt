# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "usd_to_gltf")

set(LIGHTUSD_USD_TO_GLTF_CONVERT_SOURCES
    to-gltf-main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_USD_TO_GLTF_CONVERT_SOURCES})
if(COMMAND add_sanitizers)
  add_sanitizers(${EXAMPLE_TARGET})
endif()

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(${EXAMPLE_TARGET} lightusd_render_c)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
