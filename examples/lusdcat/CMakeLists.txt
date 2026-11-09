# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "lusdcat")

set(LIGHTUSD_SIMPLE_DUMP_SOURCES
    main.cc
    variant-format.cc
    comp-graph-dump.cc
    explain.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_SIMPLE_DUMP_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/c-api)
# Allocator (if enabled) must precede the rest so it wins symbol resolution.
target_link_libraries(${EXAMPLE_TARGET} ${LIGHTUSD_TOOLS_ALLOCATOR_LIB} lightusd_static)
if(NOT TARGET lightusd_next)
  # Legacy product fallback: pull in the next library for the shared reader
  # paths, but do NOT let it define its own `lusdcat` CLI (that would clash
  # with the legacy target defined above). The unified next CLI is only built
  # by the `LIGHTUSD_NATIVE_PRODUCT=next` root path.
  set(LIGHTUSD_NEXT_BUILD_TOOLS OFF CACHE BOOL "Build next CLI tools (lusdcat)" FORCE)
  set(LIGHTUSD_NEXT_BUILD_TESTS OFF CACHE BOOL "Build tests for stable LightUSD Next" FORCE)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next ${CMAKE_BINARY_DIR}/lusdcat_next_build)
endif()
target_link_libraries(${EXAMPLE_TARGET} lightusd_next)
if(TARGET lightusd_c)
  target_link_libraries(${EXAMPLE_TARGET} lightusd_c)
endif()


set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
