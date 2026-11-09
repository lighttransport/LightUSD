set(EXAMPLE_TARGET legacy-urdf-to-usd)

set(SOURCES
  main.cc
)

add_executable(${EXAMPLE_TARGET} ${SOURCES})

add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/external
)

target_link_libraries(${EXAMPLE_TARGET} lightusd_static)
