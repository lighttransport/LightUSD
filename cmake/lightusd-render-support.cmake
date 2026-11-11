# SPDX-License-Identifier: Apache-2.0
# Shared, stage-independent render support. Embed the same objects in the
# legacy and core archives so manual archive consumers need no extra libraries.
get_filename_component(LIGHTUSD_RENDER_SUPPORT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

function(lightusd_add_render_support)
  if(TARGET lightusd_subdiv_objects)
    return()
  endif()
  set(support "${LIGHTUSD_RENDER_SUPPORT_DIR}")
  # Ptex: vendored portable C11 tptex (src/external/textools/ptex) plus the thin
  # C++ lightusd::ptx wrapper (src/ptx-loader.cc). tptex_file.c is the only
  # file-IO piece; memory-only builds (wasm/WASI) leave it out and define
  # TPTEX_NO_FILEIO.
  if(EMSCRIPTEN OR WASI OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(_tptex_fileio_default OFF)
  else()
    set(_tptex_fileio_default ON)
  endif()
  option(LIGHTUSD_PTEX_ENABLE_FILEIO
         "Build tptex_open_file() (textools/ptex/src/tptex_file.c)" ${_tptex_fileio_default})
  include("${LIGHTUSD_RENDER_SUPPORT_DIR}/../cmake/lightusd-ptex.cmake")
  set(_tptex_sources ${LIGHTUSD_PTEX_C_SOURCES})
  if(LIGHTUSD_PTEX_ENABLE_FILEIO)
    list(APPEND _tptex_sources ${LIGHTUSD_PTEX_C_FILE_SOURCES})
  endif()
  add_library(lightusd_ptex_objects OBJECT
      ${_tptex_sources}
      "${support}/ptx-loader.cc" "${support}/external/miniz.c")
  set_target_properties(lightusd_ptex_objects PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(NOT LIGHTUSD_PTEX_ENABLE_FILEIO)
    target_compile_definitions(lightusd_ptex_objects PUBLIC TPTEX_NO_FILEIO=1)
  endif()
  # Vendored miniz.c has to define the _LARGEFILE*_SOURCE feature-test macros
  # before its first include so the 64-bit stdio entry points get declared;
  # doing so trips -Wreserved-macro-identifier. Suppress it per-source rather
  # than with a diagnostic pragma inside miniz.c, keeping the vendored file
  # free of local edits so it stays easy to re-vendor / merge upstream.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_source_files_properties("${support}/external/miniz.c"
        PROPERTIES COMPILE_FLAGS "-Wno-reserved-macro-identifier")
  endif()
  add_library(lightusd_subdiv_objects OBJECT
      "${support}/tsd/tsd-validate.cc"
      "${support}/tsd/tsd-topology.cc"
      "${support}/tsd/tsd-bilinear.cc"
      "${support}/tsd/tsd-catmark.cc"
      "${support}/tsd/tsd-loop.cc"
      "${support}/tsd/tsd-fvar.cc"
      "${support}/tsd/tsd-refine.cc"
      "${support}/tsd/tsd-stream.cc"
      "${support}/tsd/tsd-limit.cc")
  foreach(support_target lightusd_ptex_objects lightusd_subdiv_objects)
    target_compile_features(${support_target} PRIVATE cxx_std_17)
    target_include_directories(${support_target} PRIVATE "${support}")
    if(COMMAND add_sanitizers)
      add_sanitizers(${support_target})
    endif()
    if(LIGHTUSD_BUILD_SHARED_LIBS OR LIGHTUSD_BUILD_SHARED_C_API)
      set_target_properties(${support_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    if(MSVC)
      target_compile_options(${support_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->
          $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
      target_compile_options(${support_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
          $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
  endforeach()
endfunction()
