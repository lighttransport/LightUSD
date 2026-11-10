# Assume this cmake is called from lightusd root(../../)
set(EXAMPLE_TARGET "progressive_composition")

set(LIGHTUSD_EXAMPLE_SOURCES
    main.cc
    )

add_executable(${EXAMPLE_TARGET} ${LIGHTUSD_EXAMPLE_SOURCES})


target_include_directories(${EXAMPLE_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${EXAMPLE_TARGET} lightusd_core)


set_target_properties(${EXAMPLE_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_progressive_composition_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-progressive-example.py
        $<TARGET_FILE:progressive_composition> $<TARGET_FILE:lusdcat>)
    set_tests_properties(core_progressive_composition_example PROPERTIES LABELS "core;examples;composition")
  endif()
endif()
