if(LIGHTUSD_WITH_MCP_SERVER)

set(TEST_TARGET_NAME mcp-test)
set(TEST_SOURCES
	mcp-test-main.cc
	mcp-test-stage.cc
	mcp-test-scene.cc
	mcp-test-query.cc
	mcp-test-validate.cc
	)

add_executable(${TEST_TARGET_NAME}
	${TEST_SOURCES}
	)

add_sanitizers(${TEST_TARGET_NAME})

add_test(NAME ${TEST_TARGET_NAME} COMMAND "${TEST_TARGET_NAME}"
         WORKING_DIRECTORY $<TARGET_FILE_DIR:${TEST_TARGET_NAME}>)
set_tests_properties(${TEST_TARGET_NAME} PROPERTIES
  ENVIRONMENT "LIGHTUSD_TEST_FIXTURE_DIR=${LIGHTUSD_TEST_FIXTURE_DIR}")

target_link_libraries(${TEST_TARGET_NAME} PRIVATE lightusd_static ${CMAKE_DL_LIBS})
target_include_directories(${TEST_TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

set_target_properties(${TEST_TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

endif()
