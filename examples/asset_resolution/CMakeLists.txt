# The root and standalone core projects register this example from src/.
if(NOT TARGET asset_resolution_example)
  add_executable(asset_resolution_example asset-resolution-example.cc)
  target_link_libraries(asset_resolution_example PRIVATE lightusd_core)
  set_target_properties(asset_resolution_example PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
