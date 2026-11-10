# Tool: lusddiff
# Diff two USD(A/C/Z) files at the Layer / PrimSpec / Attribute level
# (text or JSON output). Files are loaded as Layers (pre-composition).
#
# Assume this cmake is called from lightusd root via add_subdirectory().
set(LUSDDIFF_TARGET "lusddiff")

add_executable(${LUSDDIFF_TARGET}
    lusddiff.cc
    collection-diff.cc
    instance-canon.cc
    )
add_sanitizers(${LUSDDIFF_TARGET})

target_include_directories(${LUSDDIFF_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${LUSDDIFF_TARGET} PRIVATE lightusd_core)

# Warning flags (same policy as the core library)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(LUSDDIFF_CXX_FLAGS
      "-Weverything -Wno-poison-system-directories -Wno-padded -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-documentation -Wno-documentation-unknown-command -Wno-unused-member-function -Wno-switch-enum -Wno-exit-time-destructors -Wno-sign-conversion -Wno-float-equal -Wno-old-style-cast -Wno-double-promotion -Wno-missing-prototypes -Wno-covered-switch-default -Wno-zero-as-null-pointer-constant -Wno-nrvo -Wno-weak-vtables -Wno-reserved-macro-identifier -Wno-reserved-identifier -Wno-tautological-unsigned-zero-compare -Wno-unknown-warning-option -Wno-undef -Wno-implicit-int-float-conversion -Wno-implicit-float-conversion -Wno-shorten-64-to-32 -Wno-ms-bitfield-padding -Wno-mismatched-tags -Wno-thread-safety-negative"
  )
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 16)
    set(LUSDDIFF_CXX_FLAGS "${LUSDDIFF_CXX_FLAGS} -Wno-unsafe-buffer-usage")
  endif()
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
    set(LUSDDIFF_CXX_FLAGS "${LUSDDIFF_CXX_FLAGS} -Wno-switch-default")
  endif()
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDDIFF_CXX_FLAGS "${LUSDDIFF_CXX_FLAGS} -Werror")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(LUSDDIFF_CXX_FLAGS "-Wall")
  if(NOT LIGHTUSD_NO_WERROR)
    set(LUSDDIFF_CXX_FLAGS "${LUSDDIFF_CXX_FLAGS} -Werror")
  endif()
endif()
set_source_files_properties(lusddiff.cc PROPERTIES COMPILE_FLAGS "${LUSDDIFF_CXX_FLAGS}")

set_target_properties(${LUSDDIFF_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(LIGHTUSD_BUILD_TESTS)
  # Keep the CLI regression fixtures hermetic. USD files are ignored globally,
  # so source-tree testdata can silently disappear from clean checkouts.
  set(LUSDDIFF_TESTDATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/testdata")
  file(MAKE_DIRECTORY "${LUSDDIFF_TESTDATA_DIR}")
  file(WRITE "${LUSDDIFF_TESTDATA_DIR}/base.usda" [=[#usda 1.0
def Xform "Root" {
    custom int value = 1
}
]=])
  file(WRITE "${LUSDDIFF_TESTDATA_DIR}/composed.usda" [=[#usda 1.0
(
    subLayers = [@base.usda@]
)
]=])
  file(WRITE "${LUSDDIFF_TESTDATA_DIR}/flattened.usda" [=[#usda 1.0
def Xform "Root" {
    custom int value = 1
}
]=])
  add_test(NAME lusddiff_flatten_composed
    COMMAND lusddiff --flatten
            ${LUSDDIFF_TESTDATA_DIR}/composed.usda
            ${LUSDDIFF_TESTDATA_DIR}/flattened.usda)
  add_test(NAME lusddiff_openusd_cli_aliases
    COMMAND lusddiff -n -q -f
            ${LUSDDIFF_TESTDATA_DIR}/composed.usda
            ${LUSDDIFF_TESTDATA_DIR}/flattened.usda)
endif()

if(LIGHTUSD_BUILD_TESTS)
  foreach(fixture uint2-type-001 uint2-array-001 sceneLibrary-001)
    add_test(NAME core_reader_parity_${fixture}
      COMMAND $<TARGET_FILE:lusddiff>
        ${PROJECT_SOURCE_DIR}/tests/usda/${fixture}.usda
        ${PROJECT_SOURCE_DIR}/tests/usdc/${fixture}.usdc)
    set_tests_properties(core_reader_parity_${fixture} PROPERTIES LABELS "core;readers")
  endforeach()
endif()

if(LIGHTUSD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_reader_extension_parity
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-reader-extension-parity.py
        $<TARGET_FILE:lusddiff> ${PROJECT_SOURCE_DIR})
    set_tests_properties(core_reader_extension_parity PROPERTIES LABELS "core;readers;diff")
  endif()
endif()

if(LIGHTUSD_BUILD_TESTS AND TARGET lusdcat)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME core_reader_variant_parity
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-reader-variant-parity.py
        $<TARGET_FILE:lusddiff> $<TARGET_FILE:lusdcat> ${PROJECT_SOURCE_DIR})
    set_tests_properties(core_reader_variant_parity PROPERTIES LABELS "core;readers;variants")
  endif()
endif()
