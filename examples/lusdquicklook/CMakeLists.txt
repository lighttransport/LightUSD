# lusdquicklook minimal portable USD Quick Look viewer.
#
# Deliberately independent of examples/lusdview: no ImGui, no GLFW, no glslang,
# no volk/cuew. The UI is the vendored software-rendered lightui; the default
# renderer is the CPU ray tracer built on the vendored lightrt C API.

set(EXAMPLE_TARGET lusdquicklook)

# Vendored software UI (builds a static `lightui` target; guarded internally
# against double-add).
add_subdirectory(${PROJECT_SOURCE_DIR}/examples/common/lightui
                 ${CMAKE_CURRENT_BINARY_DIR}/lightui)

set(LUSDQUICKLOOK_SOURCES
    main.cc
    app.cc
    ui.cc
    options.cc
    browser.cc
    budget.cc
    ql_scene.cc
    loader.cc
    image_decode.cc
    camera.cc
    render/shade.cc
    render/cpu_rt.cc
    render/pick.cc
    render/gl_raster.cc
    render/gl_context_egl.cc
)

# lightrt's CPU BVH kernel (vendored at src/external/lightrt). The lightusd-local
# indexed build keeps the indexed data instead of a vertex soup, which is what
# lets a preview BLAS stay inside the memory budget.
set(LUSDQUICKLOOK_LIGHTRT_DIR ${PROJECT_SOURCE_DIR}/src/external/lightrt)
list(APPEND LUSDQUICKLOOK_SOURCES
    ${LUSDQUICKLOOK_LIGHTRT_DIR}/lightrt_c.c
    ${LUSDQUICKLOOK_LIGHTRT_DIR}/lightrt_c_tri.c
)

add_executable(${EXAMPLE_TARGET} ${LUSDQUICKLOOK_SOURCES})
add_sanitizers(${EXAMPLE_TARGET})

option(LUSDQUICKLOOK_ENABLE_MCP
       "Build lusdquicklook's embedded MCP stdio + HTTP interface" ON)
if(LUSDQUICKLOOK_ENABLE_MCP)
  target_sources(${EXAMPLE_TARGET} PRIVATE
      mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc
      ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c)
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
      LUSDQUICKLOOK_HAVE_MCP=1 NO_SSL NO_CGI)
  target_link_libraries(${EXAMPLE_TARGET} PRIVATE ${CMAKE_DL_LIBS})
  if(NOT LIGHTUSD_CXX_EXCEPTIONS AND NOT MSVC)
    set_source_files_properties(
        mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc
        PROPERTIES COMPILE_OPTIONS "-fexceptions")
  elseif(NOT LIGHTUSD_CXX_EXCEPTIONS AND MSVC)
    set_source_files_properties(
        mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc mcp/app_mcp.cc
        PROPERTIES COMPILE_OPTIONS "/EHsc")
  endif()
  message(STATUS "lusdquicklook: embedded MCP ENABLED (stdio + HTTP)")
else()
  message(STATUS "lusdquicklook: embedded MCP disabled")
endif()

target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/examples/common
    ${PROJECT_SOURCE_DIR}/src/external/lightrt
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
  # VS2026 toolchain headers guard C11/C++ runtime interop with version checks
  # that are stricter than current compiler support.
  target_compile_options(${EXAMPLE_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
  )
  target_compile_definitions(${EXAMPLE_TARGET} PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

if(LIGHTUSD_BUILD_QUICKLOOK AND NOT LIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS)
  # Keep quicklook lightweight by default: avoid building lightusd_textools
  # unless explicitly requested.
  if (LIGHTUSD_WITH_TEXTOOLS)
    message(STATUS "lusdquicklook: forcing LIGHTUSD_WITH_TEXTOOLS OFF (pass -DLIGHTUSD_BUILD_QUICKLOOK_WITH_TEXTOOLS=ON to enable)")
  endif()
  set(LIGHTUSD_WITH_TEXTOOLS OFF CACHE BOOL "Build vendored tir/texcomp/texpipe/envmap texture tools" FORCE)
endif()

# The `next` lazy loader + tydra_next streaming converter are the whole reason
# this app can hold a small memory budget. Build them on demand, the same way
# tools/lusdrender does.
set(LIGHTUSD_NEXT_ENABLE_THREAD ON CACHE BOOL "" FORCE)
if(NOT TARGET lightusd_next)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/next
                   ${CMAKE_BINARY_DIR}/lusdquicklook_next_build)
endif()

target_link_libraries(${EXAMPLE_TARGET} PRIVATE
    lightui
    ${LIGHTUSD_TOOLS_ALLOCATOR_LIB}
    lightusd_next
    tydra_next
)
# tydra_next's lazy image decoding uses symbols from the main static library;
# keep that archive last so single-pass static linkers resolve it.
target_link_libraries(${EXAMPLE_TARGET} PRIVATE lightusd_static)

find_package(Threads REQUIRED)
target_link_libraries(${EXAMPLE_TARGET} PRIVATE Threads::Threads)

set_target_properties(${EXAMPLE_TARGET} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Headless smoke test. Runs with no display and no GPU (that is the whole point
# of --screenshot), so it is safe in CI.
if(LIGHTUSD_BUILD_TESTS AND UNIX)
  add_executable(lusdquicklook-budget-test tests/byte_budget_test.cc)
  target_include_directories(lusdquicklook-budget-test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME example-lusdquicklook-budget
           COMMAND lusdquicklook-budget-test)

  add_test(NAME example-lusdquicklook-smoke
           COMMAND ${CMAKE_COMMAND} -E env bash
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-quicklook-smoke.sh
                   $<TARGET_FILE:${EXAMPLE_TARGET}>
                   ${PROJECT_SOURCE_DIR})
  set_tests_properties(example-lusdquicklook-smoke PROPERTIES TIMEOUT 300)

  # Interactive path under Xvfb. The headless test never touches
  # lui_window_create, the event loop or lui_window_present, so it cannot catch
  # a window-creation hang or a loop that blocks before its first paint. Skips
  # itself when Xvfb / ImageMagick are unavailable.
  add_test(NAME example-lusdquicklook-gui-smoke
           COMMAND ${CMAKE_COMMAND} -E env bash
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-quicklook-gui-smoke.sh
                   $<TARGET_FILE:${EXAMPLE_TARGET}>
                   ${PROJECT_SOURCE_DIR})
  set_tests_properties(example-lusdquicklook-gui-smoke PROPERTIES TIMEOUT 180)

  if(LUSDQUICKLOOK_ENABLE_MCP)
    add_executable(lusdquicklook-mcp-protocol-test
        tests/mcp_protocol_test.cc
        mcp/mcp_server.cc mcp/mcp_http.cc mcp/mcp_stdio.cc
        ${PROJECT_SOURCE_DIR}/src/external/civetweb/civetweb.c)
    add_sanitizers(lusdquicklook-mcp-protocol-test)
    target_include_directories(lusdquicklook-mcp-protocol-test PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(lusdquicklook-mcp-protocol-test PRIVATE
        NO_SSL NO_CGI)
    target_link_libraries(lusdquicklook-mcp-protocol-test PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads)
    if(NOT LIGHTUSD_CXX_EXCEPTIONS AND NOT MSVC)
      set_source_files_properties(
          tests/mcp_protocol_test.cc mcp/mcp_server.cc mcp/mcp_http.cc
          mcp/mcp_stdio.cc PROPERTIES COMPILE_OPTIONS "-fexceptions")
    elseif(NOT LIGHTUSD_CXX_EXCEPTIONS AND MSVC)
      set_source_files_properties(
          tests/mcp_protocol_test.cc mcp/mcp_server.cc mcp/mcp_http.cc
          mcp/mcp_stdio.cc PROPERTIES COMPILE_OPTIONS "/EHsc")
    endif()
    add_test(NAME example-lusdquicklook-mcp-protocol
             COMMAND lusdquicklook-mcp-protocol-test)

    # The MCP server drives the same windowed event loop, so exercise the stdio
    # handshake and main-thread tool queue under Xvfb as well.
    add_test(NAME example-lusdquicklook-mcp-smoke
             COMMAND ${CMAKE_COMMAND} -E env bash
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-quicklook-mcp-smoke.sh
                     $<TARGET_FILE:${EXAMPLE_TARGET}>
                     ${PROJECT_SOURCE_DIR})
    set_tests_properties(example-lusdquicklook-mcp-smoke PROPERTIES
                         TIMEOUT 180 SKIP_RETURN_CODE 77)
  endif()

  # Real NVIDIA offload smoke. The script is capability-gated and returns 77
  # when this machine has no live driver, so ordinary CPU/Mesa CI stays green.
  add_test(NAME example-lusdquicklook-nvidia-smoke
           COMMAND ${CMAKE_COMMAND} -E env bash
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-quicklook-nvidia-smoke.sh
                   $<TARGET_FILE:${EXAMPLE_TARGET}>
                   ${PROJECT_SOURCE_DIR})
  set_tests_properties(example-lusdquicklook-nvidia-smoke PROPERTIES
                       TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()
