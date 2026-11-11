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


target_link_libraries(${TEST_TARGET_NAME} PRIVATE lightusd_legacy_static ${CMAKE_DL_LIBS})
target_include_directories(${TEST_TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

set_target_properties(${TEST_TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

endif()
