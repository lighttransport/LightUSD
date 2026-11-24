# Core RenderScene diagnostic/export example (also usable as a standalone app).
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(tydra_to_renderscene LANGUAGES C CXX)
  add_subdirectory(../../src ${CMAKE_BINARY_DIR}/lightusd)
endif()
add_executable(tydra_to_renderscene to-renderscene-main.cc)
target_compile_features(tydra_to_renderscene PRIVATE cxx_std_17)
target_link_libraries(tydra_to_renderscene PRIVATE tydra lightusd_preview_images)
set_target_properties(tydra_to_renderscene PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_render_scene_example COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-render-scene-example.py
      $<TARGET_FILE:tydra_to_renderscene> ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    set_tests_properties(core_render_scene_example PROPERTIES LABELS "core;examples")
  endif()
  add_test(NAME core_render_scene_c_api_smoke COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:tydra_to_renderscene> -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/../..
    -P ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra_to_renderscene/c-api-smoke.cmake)
endif()
if(LIGHTUSD_BUILD_TESTS)
  # UsdGeomNurbsPatch tessellates into one RenderMesh (render-converter-generated.cc).
  add_test(NAME core_render_scene_nurbs_patch
    COMMAND $<TARGET_FILE:tydra_to_renderscene>
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models/lusdview-nurbs-patch.usda)
  set_tests_properties(core_render_scene_nurbs_patch PROPERTIES LABELS "core;tydra"
    PASS_REGULAR_EXPRESSION "num_meshes: 1"
    FAIL_REGULAR_EXPRESSION "Failed to convert")
endif()
