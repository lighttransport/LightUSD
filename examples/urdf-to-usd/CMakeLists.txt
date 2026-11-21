# The root and standalone core projects register this example from src/.
if(NOT TARGET urdf-to-usd)
  add_executable(urdf-to-usd main.cc)
  target_link_libraries(urdf-to-usd PRIVATE tydra lightusd_core)
  set_target_properties(urdf-to-usd PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(NOT MSVC)
    target_compile_options(urdf-to-usd PRIVATE -fexceptions)
  endif()
endif()
