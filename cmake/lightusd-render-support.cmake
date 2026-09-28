# SPDX-License-Identifier: Apache-2.0
# Shared, stage-independent render support. Embed the same objects in the
# legacy and next archives so manual archive consumers need no extra libraries.
get_filename_component(LIGHTUSD_RENDER_SUPPORT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

function(lightusd_add_render_support)
  if(TARGET lightusd_subdiv_objects)
    return()
  endif()
  set(support "${LIGHTUSD_RENDER_SUPPORT_DIR}")
  add_library(lightusd_ptex_objects OBJECT
      "${support}/ptx-loader.cc" "${support}/external/miniz.c")
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
    if(LIGHTUSD_BUILD_SHARED_LIBS OR LIGHTUSD_NEXT_BUILD_SHARED_C_API)
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
