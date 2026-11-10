# Core StageSession example: deferred payloads and immutable snapshots.
if(NOT TARGET large-scene-load)
  add_executable(large-scene-load main.cc)
  target_link_libraries(large-scene-load PRIVATE lightusd_core)
  set_target_properties(large-scene-load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_large_scene_cat)
    if(TARGET lusdcat)
      set(_large_scene_cat $<TARGET_FILE:lusdcat>)
    endif()
    add_test(NAME core_large_scene_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-large-scene-example.py
        $<TARGET_FILE:large-scene-load> ${_large_scene_cat})
    set_tests_properties(core_large_scene_example PROPERTIES LABELS "core;examples;payload")
  endif()
endif()
