foreach(pair "next_quickstart|quickstart.cc" "interactive_session|interactive-session.cc"
             "animation_sampling|animation-sampling.cc" "portable_asset_package|portable-package.cc")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 target)
  list(GET parts 1 source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE tydra_next lightusd_next)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_NEXT_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME next_workflow_examples
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/run-workflow-tools.py
              --bin-dir ${CMAKE_BINARY_DIR} --examples-only)
    set_tests_properties(next_workflow_examples PROPERTIES TIMEOUT 120 LABELS "next;examples")
  endif()
endif()
