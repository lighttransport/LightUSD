# SPDX-License-Identifier: Apache-2.0
# Shared Emscripten flags for LightUSD WASM modules.

function(lightusd_wasm_apply_common target)
  if (LIGHTUSD_WASM_SIMD)
    target_compile_options(${target} PRIVATE -msimd128)
    target_link_options(${target} PRIVATE -msimd128)
    if (LIGHTUSD_WASM_RELAXED_SIMD)
      target_compile_options(${target} PRIVATE -mrelaxed-simd)
      target_link_options(${target} PRIVATE -mrelaxed-simd)
    endif()
  else()
    target_compile_definitions(${target} PRIVATE LIGHTUSD_DISABLE_SIMD)
  endif()

  if (LIGHTUSD_WASM64)
    target_compile_options(${target} PRIVATE -sMEMORY64=1)
    target_link_options(${target} PRIVATE -sMEMORY64 -sMAXIMUM_MEMORY=16GB)
  endif()

  target_compile_definitions(${target} PRIVATE
    LIGHTUSD_CXX_EXCEPTIONS=0
    LIGHTUSD_CXX_RTTI=0
    LIGHTUSD_TYPED_BINDINGS=1
    JSON_NOEXCEPTION=1
    EMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0)
endfunction()

function(add_lightusd_wasm_module name)
  set(options EXCLUDE_FROM_ALL)
  set(oneValueArgs OUTPUT_NAME POST_JS OUTPUT_WASM_DIR)
  set(multiValueArgs SOURCES LINK_LIBRARIES COMPILE_DEFINITIONS LINK_FLAGS)
  cmake_parse_arguments(WASM "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if (WASM_EXCLUDE_FROM_ALL)
    add_executable(${name} EXCLUDE_FROM_ALL ${WASM_SOURCES})
  else()
    add_executable(${name} ${WASM_SOURCES})
  endif()
  lightusd_wasm_apply_common(${name})

  if (WASM_LINK_LIBRARIES)
    target_link_libraries(${name} PRIVATE ${WASM_LINK_LIBRARIES})
  endif()
  if (WASM_COMPILE_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${WASM_COMPILE_DEFINITIONS})
  endif()

  if (NOT WASM_OUTPUT_NAME)
    set(WASM_OUTPUT_NAME ${name})
  endif()
  if (NOT WASM_OUTPUT_WASM_DIR)
    set(WASM_OUTPUT_WASM_DIR "${PROJECT_SOURCE_DIR}/js/src/lightusd")
  endif()

  set(_flags "")
  if (WASM_POST_JS)
    list(APPEND _flags --post-js "${WASM_POST_JS}")
  endif()
  if (WASM_LINK_FLAGS)
    list(APPEND _flags ${WASM_LINK_FLAGS})
  endif()

  set_target_properties(${name} PROPERTIES
    OUTPUT_NAME ${WASM_OUTPUT_NAME}
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${WASM_OUTPUT_WASM_DIR}
    LINK_FLAGS "${_flags}")
endfunction()
