# Tool: Structured Crate Dump (YAML/JSON)
# Dumps USDC crate file structure for debugging, using the core crate reader.
add_executable(lusddumpcrate lusddumpcrate.cc)
target_link_libraries(lusddumpcrate PRIVATE lightusd_core)
target_include_directories(lusddumpcrate PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(lusddumpcrate PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# lusddumpcrate smoke: YAML and JSON dumps of a known crate.
if(LIGHTUSD_BUILD_TESTS AND TARGET lusddumpcrate)
  add_test(NAME core_lusddumpcrate_yaml
           COMMAND lusddumpcrate --path-filter .points "${PROJECT_SOURCE_DIR}/models/suzanne.usdc")
  set_tests_properties(core_lusddumpcrate_yaml PROPERTIES LABELS "core;tools"
    PASS_REGULAR_EXPRESSION "magic: \"PXR-USDC\".*prim: \"/Suzanne/Suzanne.points\"")
  add_test(NAME core_lusddumpcrate_json
           COMMAND lusddumpcrate -f json "${PROJECT_SOURCE_DIR}/tests/usdc/variantSet-000.usdc")
  set_tests_properties(core_lusddumpcrate_json PROPERTIES LABELS "core;tools"
    PASS_REGULAR_EXPRESSION "\"path\": \"/Implicits\\{shapeVariant=Capsule\\}/Pill\"")
endif()
