foreach(pair "next_quickstart|quickstart.cc" "interactive_session|interactive-session.cc"
             "animation_sampling|animation-sampling.cc" "portable_asset_package|portable-package.cc")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 target)
  list(GET parts 1 source)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src/c-api)
  target_link_libraries(${target} PRIVATE lightusd_c)
  if(target STREQUAL "interactive_session")
    target_link_libraries(${target} PRIVATE lightusd_render_c)
  endif()
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

# The examples above go through the C API. This one is the C++ `next` API
# walkthrough (load -> compose -> traverse -> schema -> evaluate -> write).
if(TARGET lightusd_next)
  add_executable(comprehensive_example comprehensive_example.cc)
  # lightusd_next exports the repo `src/` directory as a PUBLIC include dir,
  # which is what makes the "next/..." includes resolve. Do not add
  # ${PROJECT_SOURCE_DIR}/src here: in the standalone src/next build
  # PROJECT_SOURCE_DIR is src/next, not the repo root.
  target_link_libraries(comprehensive_example PRIVATE lightusd_next)
  set_target_properties(comprehensive_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_NEXT_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME next_workflow_examples
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/run-workflow-tools.py
              --bin-dir ${CMAKE_BINARY_DIR} --examples-only)
    set_tests_properties(next_workflow_examples PROPERTIES TIMEOUT 120 LABELS "next;examples")

    # Smoke-test the C++ example. It writes output.usda/output.usdc next to
    # its CWD, so run it in the build tree rather than the source tree.
    # CMAKE_CURRENT_SOURCE_DIR (not PROJECT_SOURCE_DIR) resolves the repo
    # root correctly in both the standalone src/next and top-level builds.
    if(TARGET comprehensive_example)
      add_test(NAME next_comprehensive_example
        COMMAND comprehensive_example
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/apishcema-none-000.usda)
      set_tests_properties(next_comprehensive_example PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        TIMEOUT 120 LABELS "next;examples")
    endif()
  endif()
endif()
