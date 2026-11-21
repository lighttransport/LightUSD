# SPDX-License-Identifier: Apache-2.0
option(LIGHTUSD_WITH_TEXTOOLS
       "Build vendored tir/texcomp/texpipe/envmap texture tools" ON)
# Vendored pure-C11 texture processing stack used by lusdview/lusdrender.
if(LIGHTUSD_WITH_TEXTOOLS AND NOT TARGET lightusd_textools)
  get_filename_component(TEXTOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/external/textools" ABSOLUTE)
  file(GLOB TEXTOOLS_SOURCES CONFIGURE_DEPENDS
       ${TEXTOOLS_DIR}/resize/src/*.c
       ${TEXTOOLS_DIR}/texcomp/src/*.c
       ${TEXTOOLS_DIR}/texpipe/src/*.c
       ${TEXTOOLS_DIR}/envmap/src/*.c)
  add_library(lightusd_textools STATIC ${TEXTOOLS_SOURCES})
  set_target_properties(lightusd_textools PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lightusd_textools PUBLIC
      ${TEXTOOLS_DIR}/resize/include
      ${TEXTOOLS_DIR}/texcomp/include
      ${TEXTOOLS_DIR}/texpipe/include
      ${TEXTOOLS_DIR}/envmap/include)
  target_compile_features(lightusd_textools PRIVATE c_std_11)
  if(NOT MSVC)
    target_compile_options(lightusd_textools PRIVATE -w)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64" AND NOT MSVC)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=armv8-a+sve" TEXTOOLS_HAS_SVE)
    if(TEXTOOLS_HAS_SVE)
      set_source_files_properties(
          ${TEXTOOLS_DIR}/resize/src/tir_kernels_sve.c
          PROPERTIES COMPILE_OPTIONS "-march=armv8-a+sve")
    endif()
  endif()
  if(UNIX AND NOT EMSCRIPTEN)
    target_compile_definitions(lightusd_textools PRIVATE
        TIR_ENABLE_THREADS TIR_THREADS_PTHREAD)
    find_package(Threads REQUIRED)
    target_link_libraries(lightusd_textools PUBLIC Threads::Threads)
  endif()
  if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(lightusd_textools PUBLIC m)
  endif()
endif()
