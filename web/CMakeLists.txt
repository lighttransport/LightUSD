# Assume this project is invoked by emcmake.
cmake_minimum_required(VERSION 3.16)

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
#list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../../cmake/sanitizers)
#find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)


option(LIGHTUSD_WASM64 "Use wasm64(memory64). Available only for Chrome and Firefox as of 2025 Jul" OFF)
option(LIGHTUSD_WASM_DEBUG "Enable WASM debug mode with source maps and assertions" OFF)
option(LIGHTUSD_WASM_SIMD "Enable WebAssembly SIMD (128-bit, widely supported since 2021)" ON)
option(LIGHTUSD_WASM_RELAXED_SIMD "Enable WebAssembly Relaxed SIMD (requires recent browsers)" OFF)
option(LIGHTUSD_WASM_COROUTINE "Enable C++20 coroutine-based async loading (allows browser repaint during loading)" ON)
set(LIGHTUSD_WASM_PRODUCT "legacy" CACHE STRING
    "WASM product to build: legacy, next, or combined (combined is compatibility-only)")
set_property(CACHE LIGHTUSD_WASM_PRODUCT PROPERTY STRINGS legacy next combined)

# Keep the historical switches as compatibility aliases for existing build
# scripts. New callers should select the product explicitly.
option(LIGHTUSD_WASM_NEXT_ONLY "Deprecated alias for LIGHTUSD_WASM_PRODUCT=next" OFF)
option(LIGHTUSD_WASM_LEGACY_ONLY "Deprecated alias for LIGHTUSD_WASM_PRODUCT=legacy" OFF)
if (LIGHTUSD_WASM_NEXT_ONLY)
  set(LIGHTUSD_WASM_PRODUCT "next")
  message(DEPRECATION "LIGHTUSD_WASM_NEXT_ONLY is deprecated; use -DLIGHTUSD_WASM_PRODUCT=next")
elseif (LIGHTUSD_WASM_LEGACY_ONLY)
  set(LIGHTUSD_WASM_PRODUCT "legacy")
  message(DEPRECATION "LIGHTUSD_WASM_LEGACY_ONLY is deprecated; use -DLIGHTUSD_WASM_PRODUCT=legacy")
endif()

if (NOT LIGHTUSD_WASM_PRODUCT STREQUAL "legacy" AND
    NOT LIGHTUSD_WASM_PRODUCT STREQUAL "next" AND
    NOT LIGHTUSD_WASM_PRODUCT STREQUAL "combined")
  message(FATAL_ERROR
    "LIGHTUSD_WASM_PRODUCT must be legacy, next, or combined (got '${LIGHTUSD_WASM_PRODUCT}')")
endif()

if (LIGHTUSD_WASM_PRODUCT STREQUAL "next")
  if (LIGHTUSD_WASM64)
    set(BUILD_TARGET "lightusd_next_64")
  else()
    set(BUILD_TARGET "lightusd_next")
  endif()
elseif (LIGHTUSD_WASM_PRODUCT STREQUAL "combined")
  if (LIGHTUSD_WASM64)
    set(BUILD_TARGET "lightusd_combined_64")
  else()
    set(BUILD_TARGET "lightusd_combined")
  endif()
elseif (LIGHTUSD_WASM64)
  set(BUILD_TARGET "lightusd_64")
else()
  set(BUILD_TARGET "lightusd")
endif()

project(${BUILD_TARGET} CXX C)

include(CTest)
option(LIGHTUSD_WASM_BROWSER_REGRESSION_TESTS
       "Register host-browser WASM regression tests with CTest" ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "LightUSD WASM: CMAKE_BUILD_TYPE not set; defaulting to MinSizeRel.")
  set(CMAKE_BUILD_TYPE "MinSizeRel" CACHE STRING
      "Build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Make C++20 default to use C++20 coroutine.
# Emscripten 4.0 or above supports(C++20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SIMD configuration
# WebAssembly SIMD is widely supported (Chrome 91+, Firefox 89+, Safari 16.4+, Edge 91+)
# See: https://caniuse.com/wasm-simd
set(LIGHTUSD_SIMD_FLAGS "")
if (LIGHTUSD_WASM_SIMD)
  message(STATUS "LightUSD WASM: SIMD enabled (-msimd128)")
  set(LIGHTUSD_SIMD_FLAGS "-msimd128")
  if (LIGHTUSD_WASM_RELAXED_SIMD)
    message(STATUS "LightUSD WASM: Relaxed SIMD enabled (-mrelaxed-simd)")
    set(LIGHTUSD_SIMD_FLAGS "${LIGHTUSD_SIMD_FLAGS} -mrelaxed-simd")
  endif()
else()
  # No wasm SIMD: also force the next parser's scanner onto its scalar path so
  # Backend() honestly reports "scalar" (the SSE2/NEON gates are inert on wasm
  # anyway, but keep one SIMD knob coherent across the build).
  add_compile_definitions(LIGHTUSD_DISABLE_SIMD)
endif()

set(LIGHTUSD_WASM_COMMON_COMPILE_OPTIONS "")
if (LIGHTUSD_SIMD_FLAGS)
  separate_arguments(LIGHTUSD_SIMD_OPTIONS NATIVE_COMMAND "${LIGHTUSD_SIMD_FLAGS}")
  list(APPEND LIGHTUSD_WASM_COMMON_COMPILE_OPTIONS ${LIGHTUSD_SIMD_OPTIONS})
endif()

if (LIGHTUSD_WASM64)
  list(APPEND LIGHTUSD_WASM_COMMON_COMPILE_OPTIONS "-sMEMORY64=1")
endif()

# The OpenPBR visual parity test requires the combined module because it renders
# every fixture through both the legacy and next backends. It is a host-side
# Node/Puppeteer test even though this project is configured through emcmake.
if (BUILD_TESTING AND LIGHTUSD_WASM_BROWSER_REGRESSION_TESTS AND
    LIGHTUSD_WASM_PRODUCT STREQUAL "combined")
  find_program(LIGHTUSD_NODE_EXECUTABLE NAMES node nodejs)
  find_program(LIGHTUSD_CHROME_EXECUTABLE
               NAMES google-chrome-stable google-chrome chromium chromium-browser)
  set(LIGHTUSD_OPENPBR_VISUAL_DIFF
      "${PROJECT_SOURCE_DIR}/js/cli/openpbr-visual-diff.mjs")
  set(LIGHTUSD_NEXT_WASM_JS
      "${PROJECT_SOURCE_DIR}/js/src/lightusd/lightusd_next.js")
  set(LIGHTUSD_NEXT_WASM_BINARY
      "${PROJECT_SOURCE_DIR}/js/src/lightusd/lightusd_next.wasm")
  if (LIGHTUSD_NODE_EXECUTABLE AND EXISTS "${LIGHTUSD_OPENPBR_VISUAL_DIFF}" AND
      EXISTS "${LIGHTUSD_NEXT_WASM_JS}" AND EXISTS "${LIGHTUSD_NEXT_WASM_BINARY}")
    add_test(
      NAME wasm-regression-openpbr-visual-diff
      COMMAND "${LIGHTUSD_NODE_EXECUTABLE}" "${LIGHTUSD_OPENPBR_VISUAL_DIFF}"
              --sw --dump-images --dump-composite
              --out "${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/openpbr-visual-diff")
    set_tests_properties(
      wasm-regression-openpbr-visual-diff
      PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
        TIMEOUT 300
        RUN_SERIAL TRUE
        LABELS "wasm;browser;regression")
    if (LIGHTUSD_CHROME_EXECUTABLE)
      # npm installs used by CI may intentionally skip Puppeteer's browser
      # download. Prefer an installed browser when one is available so a plain
      # `ctest` invocation remains self-contained.
      set_tests_properties(
        wasm-regression-openpbr-visual-diff
        PROPERTIES
          ENVIRONMENT
            "PUPPETEER_EXECUTABLE_PATH=${LIGHTUSD_CHROME_EXECUTABLE}")
    endif()
  else()
    message(STATUS
      "LightUSD WASM: skipping OpenPBR browser regression registration "
      "(Node, visual-diff CLI, or prebuilt next-only WASM module not found)")
  endif()

  # Color management has two complementary gates: direct legacy/next WASM
  # value parity, and float render-target pixel readback in a headless browser.
  # The browser runner starts its own loopback Vite server when no URL is given.
  set(LIGHTUSD_COLORSPACE_BACKEND_TEST
      "${PROJECT_SOURCE_DIR}/js/tests/colorspace-backend-parity.mjs")
  set(LIGHTUSD_COLORSPACE_PIXEL_TEST
      "${PROJECT_SOURCE_DIR}/js/tests/colorspace-render-batch.mjs")
  set(LIGHTUSD_LEGACY_WASM_JS
      "${PROJECT_SOURCE_DIR}/js/src/lightusd/lightusd.js")
  set(LIGHTUSD_LEGACY_WASM_BINARY
      "${PROJECT_SOURCE_DIR}/js/src/lightusd/lightusd.wasm")
  if (LIGHTUSD_NODE_EXECUTABLE AND
      EXISTS "${LIGHTUSD_COLORSPACE_BACKEND_TEST}" AND
      EXISTS "${LIGHTUSD_LEGACY_WASM_JS}" AND
      EXISTS "${LIGHTUSD_LEGACY_WASM_BINARY}" AND
      EXISTS "${LIGHTUSD_NEXT_WASM_JS}" AND
      EXISTS "${LIGHTUSD_NEXT_WASM_BINARY}")
    add_test(
      NAME wasm-regression-colorspace-backend-parity
      COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
              "${LIGHTUSD_COLORSPACE_BACKEND_TEST}")
    set_tests_properties(
      wasm-regression-colorspace-backend-parity
      PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
        TIMEOUT 90
        LABELS "wasm;colorspace;regression")

    if (LIGHTUSD_CHROME_EXECUTABLE AND
        EXISTS "${LIGHTUSD_COLORSPACE_PIXEL_TEST}" AND
        EXISTS "${PROJECT_SOURCE_DIR}/js/node_modules/puppeteer/package.json" AND
        EXISTS "${PROJECT_SOURCE_DIR}/js/node_modules/vite/bin/vite.js")
      add_test(
        NAME wasm-regression-colorspace-pixel-readback
        COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
                "${LIGHTUSD_COLORSPACE_PIXEL_TEST}")
      set_tests_properties(
        wasm-regression-colorspace-pixel-readback
        PROPERTIES
          ENVIRONMENT
            "PUPPETEER_EXECUTABLE_PATH=${LIGHTUSD_CHROME_EXECUTABLE};LIGHTUSD_COLORSPACE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/colorspace"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
          TIMEOUT 120
          RUN_SERIAL TRUE
          LABELS "wasm;colorspace;browser;pixel;regression")
    endif()
  endif()

  if (LIGHTUSD_NODE_EXECUTABLE)
    # Keep the complete web gate in web/js so npm and CTest exercise the same
    # ordered procedure.  The Menagerie checkout is intentionally external;
    # run setup-mujoco-menagerie.sh before invoking this test.
    set(LIGHTUSD_WEB_REGRESSION_RUNNER
        "${PROJECT_SOURCE_DIR}/js/tests/run-regression.mjs")
    if (EXISTS "${LIGHTUSD_WEB_REGRESSION_RUNNER}")
      add_test(
        NAME wasm-regression-full
        COMMAND "${LIGHTUSD_NODE_EXECUTABLE}" "${LIGHTUSD_WEB_REGRESSION_RUNNER}"
                --profile full)
      set_tests_properties(
        wasm-regression-full
        PROPERTIES
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
          TIMEOUT 3600
          RUN_SERIAL TRUE
          LABELS "wasm;browser;physics;regression")
    endif()

    add_test(
      NAME wasm-js-loader-texture-compression
      COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/js/tests/loader-utils-texture-compression.mjs")
    add_test(
      NAME wasm-texture-compression-abi
      COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/js/tests/texture-compression-wasm.mjs")
    add_test(
      NAME wasm-js-loader-basis-ktx2
      COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/js/tests/loader-utils-basis-ktx2.mjs")
    if (LIGHTUSD_CHROME_EXECUTABLE)
      add_test(
        NAME wasm-texture-compression-three-ktx2
        COMMAND "${LIGHTUSD_NODE_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/js/tests/texture-compression-three-ktx2.mjs")
      set_tests_properties(
        wasm-texture-compression-three-ktx2
        PROPERTIES
          ENVIRONMENT
            "PUPPETEER_EXECUTABLE_PATH=${LIGHTUSD_CHROME_EXECUTABLE}"
          WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
          TIMEOUT 90
          LABELS "wasm;texture;browser;regression")
    endif()
    set_tests_properties(
      wasm-js-loader-texture-compression wasm-texture-compression-abi
      wasm-js-loader-basis-ktx2
      PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/js"
        TIMEOUT 60
        LABELS "wasm;texture;regression")
  endif()
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")

  if (LIGHTUSD_WASM_DEBUG)
    # Enhanced debug mode with source maps
    if (LIGHTUSD_WASM64)
      set(CMAKE_CXX_FLAGS_DEBUG "-g -gsource-map -sMEMORY64 ${LIGHTUSD_SIMD_FLAGS}")
      set(CMAKE_C_FLAGS_DEBUG "-g -gsource-map -sMEMORY64 ${LIGHTUSD_SIMD_FLAGS}")
    else()
      set(CMAKE_CXX_FLAGS_DEBUG "-g -gsource-map ${LIGHTUSD_SIMD_FLAGS}")
      set(CMAKE_C_FLAGS_DEBUG "-g -gsource-map ${LIGHTUSD_SIMD_FLAGS}")
    endif()
  else()
    # Standard debug mode
    if (LIGHTUSD_WASM64)
      set(CMAKE_CXX_FLAGS_DEBUG "-g4 -sMEMORY64 ${LIGHTUSD_SIMD_FLAGS}")
      set(CMAKE_C_FLAGS_DEBUG "-g4 -sMEMORY64 ${LIGHTUSD_SIMD_FLAGS}")
    else()
      set(CMAKE_CXX_FLAGS_DEBUG "-g4 ${LIGHTUSD_SIMD_FLAGS}")
      set(CMAKE_C_FLAGS_DEBUG "-g4 ${LIGHTUSD_SIMD_FLAGS}")
    endif()
  endif()

else()

  add_compile_options(${LIGHTUSD_WASM_COMMON_COMPILE_OPTIONS})

  if (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # Keep CMake's default MinSizeRel flags off the command line. CMake uses
    # -Os for MinSizeRel, which would otherwise mix with the explicit -Oz.
    set(CMAKE_CXX_FLAGS_MINSIZEREL "")
    set(CMAKE_C_FLAGS_MINSIZEREL "")

    add_compile_options("-Oz")
    add_compile_definitions(NDEBUG)
  endif()
endif()

if (NOT EMSCRIPTEN)
  message(FATAL "Must be compiled with emscripten")
endif()


option(LIGHTUSD_WASM_DEMODEV "Demo developer mode(install WASM modules to demo/node_modules/lightusd." OFF)
set(LIGHTUSD_WASM_OUTPUT_DIRECTORY "" CACHE PATH
    "Override generated JS/WASM output directory (empty uses the product default)")
if (LIGHTUSD_WASM_DEMODEV)
  message(STATUS "demodev build")
endif()

# WASM/browser builds intentionally keep C++ threading disabled by default. Do
# not pull in Emscripten pthreads/SharedArrayBuffer unless a future explicit
# browser-threading build option opts into it.
set(LIGHTUSD_ENABLE_THREAD OFF CACHE BOOL "Disable std::thread paths for default WASM builds" FORCE)

if (LIGHTUSD_WASM_PRODUCT STREQUAL "next")
  message(STATUS "LightUSD WASM: building next-core + tydra-next only module")
  set(LIGHTUSD_CXX_RTTI OFF CACHE BOOL "Disable RTTI in next/tydra-next libs" FORCE)
  set(LIGHTUSD_CXX_EXCEPTIONS OFF CACHE BOOL "Disable C++ exceptions in next/tydra-next libs" FORCE)
  set(LIGHTUSD_NEXT_ENABLE_THREAD OFF CACHE BOOL "Disable next std::thread paths for WASM" FORCE)

  add_subdirectory(${PROJECT_SOURCE_DIR}/../src/next lightusd_next_build EXCLUDE_FROM_ALL)

  add_executable(lightusd_next_wasm binding-next.cc binding-next-scene.cc
    binding-next-api.cc
    binding-next-util.cc
    binding-next-render-lifecycle.cc
    binding-next-render-scene.cc
    binding-next-render-animation.cc
    binding-next-render-outputs.cc
    binding-next-render-mesh-helpers.cc
    binding-next-render-mesh.cc
    binding-next-render-material-key.cc
    binding-next-render-material.cc
    binding-next-render-merge.cc
    binding-next-render-material-output.cc)
  # tinysubdiv (tsd): dependency-free subdivision streaming for SubdivStreamer.
  target_sources(lightusd_next_wasm PRIVATE
    ${PROJECT_SOURCE_DIR}/lightrt-wasm.cc
    ${PROJECT_SOURCE_DIR}/../src/external/lightrt/lightrt_c.c
    ${PROJECT_SOURCE_DIR}/../src/external/lightrt/lightrt_c_tri.c
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-validate.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-topology.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-bilinear.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-catmark.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-loop.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-fvar.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-refine.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-stream.cc
    ${PROJECT_SOURCE_DIR}/../src/tsd/tsd-limit.cc)
  target_link_libraries(lightusd_next_wasm PRIVATE tydra_next lightusd_next)
  target_include_directories(lightusd_next_wasm PRIVATE "${PROJECT_SOURCE_DIR}/../src/")
  target_compile_features(lightusd_next_wasm PRIVATE cxx_std_17)
  set_property(TARGET lightusd_next_wasm APPEND PROPERTY LINK_DEPENDS
    "${PROJECT_SOURCE_DIR}/next-api.js")
  target_compile_options(lightusd_next_wasm
                         PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
  target_compile_definitions(lightusd_next_wasm
                             PRIVATE JSON_NOEXCEPTION=1
                                     LIGHTUSD_CXX_EXCEPTIONS=0)
  if (LIGHTUSD_WASM64)
    target_compile_definitions(lightusd_next_wasm PRIVATE LIGHTUSD_WASM_MEMORY64=1)
  endif()

  if (LIGHTUSD_WASM_DEMODEV)
    set(OUTPUT_WASM_DIR ${PROJECT_SOURCE_DIR}/demo/node_modules/lightusd/)
  else()
    set(OUTPUT_WASM_DIR ${PROJECT_SOURCE_DIR}/js/src/lightusd/)
  endif()
  if(LIGHTUSD_WASM_OUTPUT_DIRECTORY)
    set(OUTPUT_WASM_DIR "${LIGHTUSD_WASM_OUTPUT_DIRECTORY}")
  endif()

  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LIGHTUSD_NEXT_EMCC_LINK_FLAGS "-g4 -sENVIRONMENT='web,worker,node' \
     -sMALLOC=emmalloc \
     -sSTACK_SIZE=512000 \
     -sALLOW_MEMORY_GROWTH=1 \
     -sMEMORY_GROWTH_GEOMETRIC_STEP=0.1 \
     -sFILESYSTEM=0 \
     -sWASM_BIGINT=1 \
     -sMODULARIZE=1 -sEXPORT_ES6 \
     -sINVOKE_RUN=0 --bind ")
  else()
    if (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
      set(LIGHTUSD_NEXT_EMCC_LINK_OPT_FLAGS "-Oz ")
    else()
      set(LIGHTUSD_NEXT_EMCC_LINK_OPT_FLAGS "")
    endif()
    set(LIGHTUSD_NEXT_EMCC_LINK_FLAGS "${LIGHTUSD_NEXT_EMCC_LINK_OPT_FLAGS}-sENVIRONMENT='web,worker,node' \
     -sMALLOC=emmalloc \
     -sSTACK_SIZE=512000 \
     -sALLOW_MEMORY_GROWTH=1 \
     -sMEMORY_GROWTH_GEOMETRIC_STEP=0.1 \
     -sFILESYSTEM=0 \
     -sSAFE_HEAP=0 \
     -sWASM_BIGINT=1 \
     -sMODULARIZE=1 -sEXPORT_ES6 \
     -sINVOKE_RUN=0 --bind ")
  endif()
  if (LIGHTUSD_SIMD_FLAGS)
    string(APPEND LIGHTUSD_NEXT_EMCC_LINK_FLAGS " ${LIGHTUSD_SIMD_FLAGS}")
  endif()
  if (LIGHTUSD_WASM64)
    string(APPEND LIGHTUSD_NEXT_EMCC_LINK_FLAGS " -sASSERTIONS=2 -sMEMORY64 -sMAXIMUM_MEMORY=16GB -sMALLOC=dlmalloc")
  else()
    string(APPEND LIGHTUSD_NEXT_EMCC_LINK_FLAGS " -sASSERTIONS=1")
  endif()
  string(APPEND LIGHTUSD_NEXT_EMCC_LINK_FLAGS " -sEXPORTED_RUNTIME_METHODS=['HEAPU8']")
  string(APPEND LIGHTUSD_NEXT_EMCC_LINK_FLAGS
    " --post-js \"${PROJECT_SOURCE_DIR}/next-api.js\"")

  set_target_properties(
      lightusd_next_wasm
      PROPERTIES OUTPUT_NAME ${BUILD_TARGET}
                 SUFFIX ".js"
                 RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_WASM_DIR}
                 LINK_FLAGS "${LIGHTUSD_NEXT_EMCC_LINK_FLAGS}")
  return()
endif()

# [lightusd]
add_subdirectory(../  lightusd)

# The combined compatibility module shares the pipeline source definition.
# Its legacy library supplies LZ4 and the other shared support implementations.
set(LIGHTUSD_NEXT_DIR "${PROJECT_SOURCE_DIR}/../src/next")
include("${LIGHTUSD_NEXT_DIR}/pipeline-sources.cmake")
set(NEXT_SOURCES ${LIGHTUSD_NEXT_PIPELINE_SOURCES})
list(TRANSFORM NEXT_SOURCES PREPEND "${LIGHTUSD_NEXT_DIR}/")

list(
  APPEND
  SOURCES
  binding.cc
  lightrt-wasm.cc
  ${PROJECT_SOURCE_DIR}/../src/external/lightrt/lightrt_c.c
  ${PROJECT_SOURCE_DIR}/../src/external/lightrt/lightrt_c_tri.c
)
# The legacy product deliberately excludes the next core. The combined product
# remains available for compatibility testing, but is not the shipped default.
if (LIGHTUSD_WASM_PRODUCT STREQUAL "combined")
  list(APPEND SOURCES ${NEXT_SOURCES})
endif()


add_executable(${BUILD_TARGET} ${SOURCES})
add_sanitizers(${BUILD_TARGET})

target_compile_options(${BUILD_TARGET} PRIVATE ${EXT_COMPILE_OPTIONS})
target_compile_options(${BUILD_TARGET}
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
                               $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
target_compile_definitions(${BUILD_TARGET}
                           PRIVATE JSON_NOEXCEPTION=1
                                   LIGHTUSD_CXX_EXCEPTIONS=0
                                   LIGHTUSD_CXX_RTTI=0
                                   EMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0)
if (LIGHTUSD_WASM_PRODUCT STREQUAL "combined")
  target_compile_definitions(${BUILD_TARGET} PRIVATE LIGHTUSD_WASM_WITH_NEXT=1)
endif()
if (LIGHTUSD_WASM64)
  target_compile_definitions(${BUILD_TARGET} PRIVATE LIGHTUSD_WASM_MEMORY64=1)
endif()

# Pass through compile definitions for feature flags
if (LIGHTUSD_WITH_EXR)
  target_compile_definitions(${BUILD_TARGET} PRIVATE LIGHTUSD_WITH_EXR=1)
endif()

# C++20 coroutine support for async loading
if (LIGHTUSD_WASM_COROUTINE)
  message(STATUS "LightUSD WASM: C++20 coroutine async loading enabled")
  target_compile_definitions(${BUILD_TARGET} PRIVATE LIGHTUSD_USE_COROUTINE=1)
endif()

# lightusd dir
target_include_directories(${BUILD_TARGET}
                           PRIVATE "${PROJECT_SOURCE_DIR}/../src/")


target_link_libraries(${BUILD_TARGET} PRIVATE lightusd::lightusd_static
                                              ${EXT_LIBRARIES} PUBLIC ${CMAKE_DL_LIBS})

# The main WASM module exposes the same Basis-free `uni` scene-texture path as
# the standalone texcomp demo. Referencing textools directly keeps its public
# headers visible to binding.cc and prevents the transcoders from being stripped.
if (LIGHTUSD_WITH_TEXTOOLS)
  target_compile_definitions(${BUILD_TARGET} PRIVATE LIGHTUSD_WITH_TEXTOOLS=1)
  target_link_libraries(${BUILD_TARGET} PRIVATE lightusd_textools)
endif()

source_group("Source Files" FILES ${SOURCES})

if (EMSCRIPTEN)

    if (LIGHTUSD_WASM_DEMODEV)
      set(OUTPUT_WASM_DIR ${PROJECT_SOURCE_DIR}/demo/node_modules/lightusd/)
    else()
      set(OUTPUT_WASM_DIR ${PROJECT_SOURCE_DIR}/js/src/lightusd/)
    endif()
    if(LIGHTUSD_WASM_OUTPUT_DIRECTORY)
      set(OUTPUT_WASM_DIR "${LIGHTUSD_WASM_OUTPUT_DIRECTORY}")
    endif()

    set_target_properties(
        ${BUILD_TARGET}
        PROPERTIES OUTPUT_NAME ${BUILD_TARGET}
                   SUFFIX ".js"
                   RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_WASM_DIR})
    #set(CMAKE_EXECUTABLE_SUFFIX ".html")
endif()


# for debugging
# --memoryprofiler
# --source-map-base http://localhost:8021/

# flags = try to lower heap memory use

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (LIGHTUSD_WASM_DEBUG)
    # Enhanced debug mode with source maps, stack traces, and runtime checks
    set(LIGHTUSD_EMCC_LINK_FLAGS "-g -gsource-map -sENVIRONMENT='web,worker,node' \
     -sMALLOC=emmalloc \
     -sSTACK_SIZE=1024000 \
     -sALLOW_MEMORY_GROWTH=1 \
     -sFILESYSTEM=0 \
     -sSAFE_HEAP=1 \
     -sSTACK_OVERFLOW_CHECK=2 \
     -sASSERTIONS=2 \
     -sWASM_BIGINT=1 \
     -sMODULARIZE=1 -sEXPORT_ES6 \
     -sINVOKE_RUN=0 --bind \
     --source-map-base http://localhost:5173/ ")
  else()
    # Standard debug mode
    set(LIGHTUSD_EMCC_LINK_FLAGS "-g4 -sENVIRONMENT='web,worker,node' \
     -sMALLOC=emmalloc \
     -sSTACK_SIZE=1024000 \
     -sALLOW_MEMORY_GROWTH=1 \
     -sFILESYSTEM=0 \
     -sSAFE_HEAP=1 \
     -sWASM_BIGINT=1 \
     -sMODULARIZE=1 -sEXPORT_ES6 \
     -sINVOKE_RUN=0 --bind \
     --source-map-base http://localhost:5173/ ")
  endif()
else()
  if (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(LIGHTUSD_EMCC_LINK_OPT_FLAGS "-Oz ")
  else()
    set(LIGHTUSD_EMCC_LINK_OPT_FLAGS "")
  endif()

  set(LIGHTUSD_EMCC_LINK_FLAGS "${LIGHTUSD_EMCC_LINK_OPT_FLAGS}-sENVIRONMENT='web,worker,node' \
   -sMALLOC=emmalloc \
   -sSTACK_SIZE=512000 \
   -sALLOW_MEMORY_GROWTH=1 \
   -sFILESYSTEM=0 \
   -sSAFE_HEAP=0 \
   -sWASM_BIGINT=1 \
   -sMODULARIZE=1 -sEXPORT_ES6 \
   -sINVOKE_RUN=0 --bind ")
endif()

if (LIGHTUSD_SIMD_FLAGS)
  string(APPEND LIGHTUSD_EMCC_LINK_FLAGS " ${LIGHTUSD_SIMD_FLAGS}")
endif()

if (LIGHTUSD_WASM64)
  # assertion=1 cause runtime error(Cannot mix BigInt and ... in assert()), so use 2
  # MAXIMUM_MEMORY=16GB: only reserves address space (ALLOW_MEMORY_GROWTH grows
  # on demand); the in-heap folder flatten of a multi-GB UE-export scene (e.g.
  # a 2.5 GB source folder) needs ~10 GB.
  # MALLOC=dlmalloc: emmalloc asserts (debug_region_is_consistent in
  # emmalloc_free) once the wasm64 heap grows past ~8 GB; dlmalloc handles the
  # large-heap case. (The later -sMALLOC overrides the emmalloc set in the
  # common flags above.)
  string(APPEND LIGHTUSD_EMCC_LINK_FLAGS " -sASSERTIONS=2 -sMEMORY64 -sMAXIMUM_MEMORY=16GB -sMALLOC=dlmalloc")
else()
  string(APPEND LIGHTUSD_EMCC_LINK_FLAGS " -sASSERTIONS=1 ")
endif()

# Export heap views for zero-copy streaming and typed pointer descriptors used
# by the web loader fast path.
string(APPEND LIGHTUSD_EMCC_LINK_FLAGS " -sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAP8','HEAP16','HEAPU32','HEAPF32']")

message(STATUS ${LIGHTUSD_EMCC_LINK_FLAGS})

set_target_properties(${BUILD_TARGET} PROPERTIES LINK_FLAGS "${LIGHTUSD_EMCC_LINK_FLAGS}")

# ENVIRONMENT=web
# SINGLE_FILE=1
