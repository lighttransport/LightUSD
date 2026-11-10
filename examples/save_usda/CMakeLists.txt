# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "save_usda")

set(LIGHTUSD_SAVE_USDA_SOURCES
    main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_SAVE_USDA_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_core)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke")
  add_test(NAME core_save_usda COMMAND $<TARGET_FILE:save_usda>)
  set_tests_properties(core_save_usda PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke" LABELS "core;examples")
  if(TARGET lusdcat)
    add_test(NAME core_save_usda_reparse
      COMMAND $<TARGET_FILE:lusdcat> "${CMAKE_CURRENT_BINARY_DIR}/smoke/output.usda")
    set_tests_properties(core_save_usda_reparse PROPERTIES
      DEPENDS core_save_usda LABELS "core;examples")
  endif()
endif()
