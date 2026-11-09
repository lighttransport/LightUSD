# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "tydra_to_renderscene")

set(LIGHTUSD_TYDRA_CONVERT_SOURCES
    to-renderscene-main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_TYDRA_CONVERT_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
# Allocator (if enabled) must precede the rest so it wins symbol resolution.
target_link_libraries(${EXAMPLE_TARGET} PRIVATE ${LIGHTUSD_TOOLS_ALLOCATOR_LIB} lightusd_static)

# `tydra/next` converter symbols are only required/available when the next large-
# scene path is built.
if(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  if(NOT TARGET lightusd_next)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/next ${CMAKE_BINARY_DIR}/tydra_to_renderscene_next_build)
  endif()
  if(NOT TARGET tydra_next)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/tydra/next ${CMAKE_BINARY_DIR}/tydra_to_renderscene_tydra_next_build)
  endif()
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_render_c)
  if(LIGHTUSD_BUILD_TESTS)
    add_test(NAME tydra-next-c-api-smoke
      COMMAND ${CMAKE_COMMAND}
        -DAPP=$<TARGET_FILE:${EXAMPLE_TARGET}>
        -DROOT=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/tests/tydra_to_renderscene/c-api-smoke.cmake)
    set_tests_properties(tydra-next-c-api-smoke PROPERTIES LABELS "next;c-api")
  endif()
endif()

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
