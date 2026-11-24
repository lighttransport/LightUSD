# The root and standalone core projects register this example from src/.
if(NOT TARGET variant-lister)
  add_executable(variant-lister main.cc)
  target_link_libraries(variant-lister PRIVATE lightusd_c)
  set_target_properties(variant-lister PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
