add_executable(lusddeps main.cc)
target_link_libraries(lusddeps PRIVATE lightusd_core)
set_target_properties(lusddeps PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
