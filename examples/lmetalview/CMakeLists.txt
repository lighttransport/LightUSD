# lmetalview - native macOS (Swift + Metal) minimal USD viewer example.
#
# Added from src/next/CMakeLists.txt, where lightusd_next / lightusd_c /
# lightusd_render_c / tydra_next are already defined. The example links ONLY the
# next product (next + tydra_next); no legacy lightusd/lightusd_static target is
# involved.
#
# Shaders are compiled by the Metal driver at startup (see swift/Shaders.swift),
# so no offline Metal toolchain is required.

get_filename_component(LMV_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The C++ core (lmetalview_core) is portable and also builds on Windows for
# examples/ld3d12view; the Swift + Metal application is macOS only.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT WIN32)
  message(STATUS "lmetalview: skipped (macOS / Windows only)")
  return()
endif()

# ---------------------------------------------------------------------------
# Core: next + tydra_next -> flat preview scene behind a C ABI (core/lmv.h)
# ---------------------------------------------------------------------------

set(LMV_CORE_SOURCES
    core/lmv-scene.cc
    core/lmv-camera.cc
    core/lmv-ptex.cc
    ${LMV_REPO_ROOT}/examples/common/light3d/camera-nav.cc
    ${LMV_REPO_ROOT}/examples/common/light3d/math.cpp)

# Ptex reader support (the same lazy reader lusdview uses) + miniz for its
# compressed blocks. Plain utility translation units, not the legacy library.
list(APPEND LMV_CORE_SOURCES
    ${LMV_REPO_ROOT}/src/ptx-loader.cc
    ${LMV_REPO_ROOT}/src/io-util.cc
    ${LMV_REPO_ROOT}/src/external/miniz.c)
set_source_files_properties(${LMV_REPO_ROOT}/src/external/miniz.c
    PROPERTIES COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE=1"
               COMPILE_FLAGS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

# EXR decode: the vendored pure-C11 tinyexr v3 backend + the LightUSD adapter.
# The `next` product does not build the legacy image-loader stack, so the example
# carries this decoder itself.
set(LMV_EXR_DIR ${LMV_REPO_ROOT}/src/external/tinyexr)
file(GLOB LMV_TINYEXR_SOURCES CONFIGURE_DEPENDS ${LMV_EXR_DIR}/src/*.c)
set(LMV_EXR_SOURCES ${LMV_REPO_ROOT}/src/imageio/exr-codec.c ${LMV_TINYEXR_SOURCES})
set_source_files_properties(${LMV_TINYEXR_SOURCES}
    PROPERTIES INCLUDE_DIRECTORIES "${LMV_EXR_DIR}/include;${LMV_EXR_DIR}/src"
               COMPILE_DEFINITIONS "EXR_NO_JPH;EXR_NO_ZSTD"
               COMPILE_FLAGS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
set_source_files_properties(${LMV_REPO_ROOT}/src/imageio/exr-codec.c
    PROPERTIES INCLUDE_DIRECTORIES "${LMV_REPO_ROOT}/src"
               COMPILE_FLAGS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
list(APPEND LMV_CORE_SOURCES ${LMV_EXR_SOURCES})

# KTX2 preview decode: the vendored texpipe/texcomp C stack + zstd for
# supercompressed containers. Not part of the `next` product, so the example
# builds it here.
set(LMV_TEXTOOLS_DIR ${LMV_REPO_ROOT}/src/external/textools)
file(GLOB LMV_TEXTOOL_SOURCES CONFIGURE_DEPENDS
     ${LMV_TEXTOOLS_DIR}/resize/src/*.c
     ${LMV_TEXTOOLS_DIR}/texcomp/src/*.c
     ${LMV_TEXTOOLS_DIR}/texpipe/src/*.c)
add_library(lmetalview_textools STATIC
    ${LMV_TEXTOOL_SOURCES}
    ${LMV_REPO_ROOT}/src/external/zstd.c)
target_include_directories(lmetalview_textools PUBLIC
    ${LMV_TEXTOOLS_DIR}/resize/include
    ${LMV_TEXTOOLS_DIR}/texcomp/include
    ${LMV_TEXTOOLS_DIR}/texpipe/include)
target_compile_features(lmetalview_textools PRIVATE c_std_11)
target_compile_definitions(lmetalview_textools PRIVATE ZSTD_DISABLE_ASM=1)
if(NOT WIN32)
  # MSVC has no C11 <threads.h> / pthreads; textools runs single-threaded there.
  target_compile_definitions(lmetalview_textools PRIVATE
      TIR_ENABLE_THREADS TIR_THREADS_PTHREAD)
  find_package(Threads REQUIRED)
  target_link_libraries(lmetalview_textools PUBLIC Threads::Threads)
endif()
if(NOT MSVC)
  target_compile_options(lmetalview_textools PRIVATE -w)
endif()

add_library(lmetalview_core STATIC ${LMV_CORE_SOURCES})
target_compile_features(lmetalview_core PUBLIC cxx_std_17)
target_include_directories(lmetalview_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core
    PRIVATE
      ${LMV_REPO_ROOT}/src
      ${LMV_REPO_ROOT}/src/c-api
      ${LMV_REPO_ROOT}/src/external
      ${LMV_REPO_ROOT}/examples/common)
target_link_libraries(lmetalview_core PUBLIC
    lightusd_render_c lightusd_c tydra_next)
target_link_libraries(lmetalview_core PRIVATE lmetalview_textools)

# Same warning policy as the core library / lusdview.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(LMV_CORE_WARNINGS
      -Weverything -Wno-poison-system-directories -Wno-padded
      -Wno-c++98-compat -Wno-c++98-compat-pedantic
      -Wno-documentation -Wno-documentation-unknown-command
      -Wno-unused-member-function -Wno-switch-enum
      -Wno-exit-time-destructors -Wno-sign-conversion -Wno-float-equal
      -Wno-old-style-cast -Wno-double-promotion -Wno-missing-prototypes
      -Wno-covered-switch-default -Wno-zero-as-null-pointer-constant
      -Wno-nrvo -Wno-weak-vtables -Wno-reserved-macro-identifier
      -Wno-reserved-identifier -Wno-tautological-unsigned-zero-compare
      -Wno-unknown-warning-option -Wno-undef
      -Wno-implicit-int-float-conversion -Wno-implicit-float-conversion
      -Wno-shorten-64-to-32 -Wno-ms-bitfield-padding -Wno-mismatched-tags
      -Wno-unsafe-buffer-usage)
  if(${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 17)
    list(APPEND LMV_CORE_WARNINGS -Wno-switch-default)
  endif()
  if(NOT LIGHTUSD_NO_WERROR)
    list(APPEND LMV_CORE_WARNINGS -Werror)
  endif()
  target_compile_options(lmetalview_core PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:${LMV_CORE_WARNINGS}>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(lmetalview_core PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()

if(MSVC)
  target_compile_definitions(lmetalview_core PRIVATE
      _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(lmetalview_core PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
if(NOT LIGHTUSD_CXX_RTTI)
  target_compile_options(lmetalview_core PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>>)
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS AND NOT MSVC)
  target_compile_options(lmetalview_core PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
endif()

# ---------------------------------------------------------------------------
# Swift + Metal application bundle
# ---------------------------------------------------------------------------

include(CheckLanguage)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  check_language(Swift)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_Swift_COMPILER)
  message(STATUS "lmetalview: Swift app skipped (no Swift compiler found)")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_Swift_COMPILER)
enable_language(Swift)

set(LMV_SWIFT_SOURCES
    swift/main.swift
    swift/AppDelegate.swift
    swift/ViewerController.swift
    swift/MetalView.swift
    swift/HUDView.swift
    swift/CameraController.swift
    swift/SceneGPU.swift
    swift/Renderer.swift
    swift/Shaders.swift
    swift/Types.swift
    swift/InputPath.swift)

add_executable(lmetalview MACOSX_BUNDLE ${LMV_SWIFT_SOURCES})
target_include_directories(lmetalview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core)
# Swift imports the core through core/module.modulemap (`import LMV`).
target_link_libraries(lmetalview PRIVATE lmetalview_core)


target_link_libraries(lmetalview PRIVATE
    "-framework Metal"
    "-framework MetalKit"
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework ImageIO"
    "-framework UniformTypeIdentifiers")

set(LMV_VERSION "0.1.0")
set(LMV_BUNDLE_ID "com.lighttransport.lmetalview")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
               ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(lmetalview PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    MACOSX_BUNDLE_BUNDLE_NAME "lmetalview"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${LMV_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${LMV_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${LMV_VERSION}"
    Swift_LANGUAGE_VERSION "5"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(LIGHTUSD_BUILD_TESTS OR LIGHTUSD_NEXT_BUILD_TESTS)
  add_executable(lmetalview_core_test tests/core_test.cc)
  target_compile_features(lmetalview_core_test PRIVATE cxx_std_17)
  target_link_libraries(lmetalview_core_test PRIVATE lmetalview_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT LIGHTUSD_NO_WERROR)
    target_compile_options(lmetalview_core_test PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME lmetalview_core_test
      COMMAND lmetalview_core_test
              ${LMV_REPO_ROOT}/models/cube-previewsurface.usdc
              ${LMV_REPO_ROOT}/models/cube-mtlx-texture.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-xform.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/udim-texture.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/ptex-texture.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/exr-texture.usda
              ${LMV_REPO_ROOT}/models/ktx2-uni-plane.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/ktx2-bc1-texture.usda
              ${LMV_REPO_ROOT}/tests/usda/ue-skinned-blendshape.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-light.usda
              ${LMV_REPO_ROOT}/tests/usda/pointinstancer-full-001.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-camera.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-visibility.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-material.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-instance-visibility.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-proto-index.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-instance-count.usda
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-points-xform.usda)
  set_tests_properties(lmetalview_core_test PROPERTIES
      LABELS "lmetalview" WORKING_DIRECTORY ${LMV_REPO_ROOT})

  if(TARGET lmetalview)
  # Headless renders also compile the MSL at runtime, so these cover the
  # UDIM/Ptex/KTX2/EXR shader paths that the core test cannot reach.
  function(lmv_add_headless_test name fixture)
    add_test(NAME lmetalview-headless-${name}
        COMMAND $<TARGET_FILE:lmetalview> --headless
                --size 320x240
                --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-${name}.png
                ${fixture})
    set_tests_properties(lmetalview-headless-${name} PROPERTIES
        LABELS "lmetalview;gpu;metal"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
        WORKING_DIRECTORY ${LMV_REPO_ROOT})
  endfunction()
  lmv_add_headless_test(smoke ${LMV_REPO_ROOT}/models/cube-mtlx-texture.usda)
  lmv_add_headless_test(udim ${CMAKE_CURRENT_SOURCE_DIR}/tests/udim-texture.usda)
  lmv_add_headless_test(ptex ${CMAKE_CURRENT_SOURCE_DIR}/tests/ptex-texture.usda)
  lmv_add_headless_test(exr ${CMAKE_CURRENT_SOURCE_DIR}/tests/exr-texture.usda)
  lmv_add_headless_test(ktx2 ${LMV_REPO_ROOT}/models/ktx2-uni-plane.usda)
  lmv_add_headless_test(ktx2bc1 ${CMAKE_CURRENT_SOURCE_DIR}/tests/ktx2-bc1-texture.usda)
  lmv_add_headless_test(instanced ${LMV_REPO_ROOT}/models/blastest-instanced-static.usda)
  lmv_add_headless_test(pointinstancer ${LMV_REPO_ROOT}/tests/usda/pointinstancer-full-001.usda)
  lmv_add_headless_test(nonmesh ${LMV_REPO_ROOT}/tests/usda/lusdview-nonmesh-points-curves.usda)

  # In-place playback: load at frame 1, then update skin/morph data to frame 2
  # without re-converting geometry, exercising the GPU skinning/morph path.
  add_test(NAME lmetalview-headless-animated-fast
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --animate-from 1 --time 2
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-fast.png
              ${LMV_REPO_ROOT}/tests/usda/ue-skinned-blendshape.usda)
  set_tests_properties(lmetalview-headless-animated-fast PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated light transforms through the in-place playback path.
  add_test(NAME lmetalview-headless-animated-light
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-light.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-light.usda)
  set_tests_properties(lmetalview-headless-animated-light PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated PointInstancer positions/orientations/scales through the
  # in-place playback path.
  add_test(NAME lmetalview-headless-animated-pointinstancer
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --animate-from 1 --time 12
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-pointinstancer.png
              ${LMV_REPO_ROOT}/tests/usda/pointinstancer-full-001.usda)
  set_tests_properties(lmetalview-headless-animated-pointinstancer PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated authored camera through the in-place playback path.
  add_test(NAME lmetalview-headless-animated-camera
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --animate-from 0 --time 48
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-camera.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-camera.usda)
  set_tests_properties(lmetalview-headless-animated-camera PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated visibility through the in-place playback path.
  add_test(NAME lmetalview-headless-animated-visibility
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-visibility.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-visibility.usda)
  set_tests_properties(lmetalview-headless-animated-visibility PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated material inputs force the re-conversion fallback.
  add_test(NAME lmetalview-headless-animated-material
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-material.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-material.usda)
  set_tests_properties(lmetalview-headless-animated-material PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Animated PointInstancer invisibleIds through the in-place path.
  add_test(NAME lmetalview-headless-animated-instance-visibility
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-instance-visibility.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-instance-visibility.usda)
  set_tests_properties(lmetalview-headless-animated-instance-visibility PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # An instance invisible at load is revealed by animated invisibleIds.
  add_test(NAME lmetalview-headless-animated-instance-reveal
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-instance-reveal.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-instance-reveal.usda)
  set_tests_properties(lmetalview-headless-animated-instance-reveal PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  # Prototype switching and instance-count growth.
  add_test(NAME lmetalview-headless-animated-proto-index
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid --no-shadows
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-proto-index.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-proto-index.usda)
  set_tests_properties(lmetalview-headless-animated-proto-index PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})

  add_test(NAME lmetalview-headless-animated-instance-count
      COMMAND $<TARGET_FILE:lmetalview> --headless
              --size 320x240 --authored-camera --no-grid --no-shadows
              --animate-from 0 --time 24
              --screenshot ${CMAKE_CURRENT_BINARY_DIR}/lmetalview-animated-instance-count.png
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/animated-instance-count.usda)
  set_tests_properties(lmetalview-headless-animated-instance-count PROPERTIES
      LABELS "lmetalview;gpu;metal"
      SKIP_RETURN_CODE 77
      TIMEOUT 180
      WORKING_DIRECTORY ${LMV_REPO_ROOT})
  endif()
endif()
