# Core MCP embedding example, with stdio and HTTP transports.
find_package(Threads REQUIRED)
add_executable(mcp_server example-mcp-server.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/civetweb/civetweb.c)
target_include_directories(mcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(mcp_server PRIVATE NO_SSL NO_CGI)
target_link_libraries(mcp_server PRIVATE lightusd_mcp Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(mcp_server PRIVATE ws2_32)
endif()
set_target_properties(mcp_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_mcp_example
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test-mcp-example.py
        $<TARGET_FILE:mcp_server>)
    set_tests_properties(core_mcp_example PROPERTIES LABELS "core;mcp;examples" TIMEOUT 45)
  endif()
endif()
